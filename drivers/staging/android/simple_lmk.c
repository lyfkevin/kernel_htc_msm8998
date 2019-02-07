/*
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "simple_lmk: " fmt

#include <linux/cpu_input_boost.h>
#include <linux/devfreq_boost.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/simple_lmk.h>

#define MIN_FREE_PAGES (CONFIG_ANDROID_SIMPLE_LMK_MINFREE * SZ_1M / PAGE_SIZE)

/* Duration to boost CPU and DDR bus to the max per memory reclaim event */
#define BOOST_DURATION_MS (250)

/* Pulled from the Android framework */
static const short int adj_prio[] = {
	906, /* CACHED_APP_MAX_ADJ */
	900, /* CACHED_APP_MIN_ADJ */
	800, /* SERVICE_B_ADJ */
	700, /* PREVIOUS_APP_ADJ */
	600, /* HOME_APP_ADJ */
	500, /* SERVICE_ADJ */
	400, /* HEAVY_WEIGHT_APP_ADJ */
	300, /* BACKUP_APP_ADJ */
	200, /* PERCEPTIBLE_APP_ADJ */
	100, /* VISIBLE_APP_ADJ */
	0    /* FOREGROUND_APP_ADJ */
};

static void simple_lmk_reclaim_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(reclaim_work, simple_lmk_reclaim_work);
static DEFINE_MUTEX(reclaim_lock);
static struct workqueue_struct *simple_lmk_wq;
static unsigned long last_reclaim_jiffies;
static atomic_t simple_lmk_ready = ATOMIC_INIT(0);

static unsigned long scan_and_kill(int min_adj, int max_adj,
	unsigned long pages_needed)
{
	/* Boost priority of victim tasks so they can die quickly */
	static const struct sched_param param = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct task_struct *tsk;
	unsigned long pages_freed = 0;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *victim;
		unsigned long tasksize;
		short oom_score_adj;

		/* Don't commit suicide or kill kthreads */
		if (same_thread_group(tsk, current) || tsk->flags & PF_KTHREAD)
			continue;

		victim = find_lock_task_mm(tsk);
		if (!victim)
			continue;

		/* Don't kill tasks that have been killed or lack memory */
		if (victim->lmk_sigkill_sent ||
			test_tsk_thread_flag(victim, TIF_MEMDIE)) {
			task_unlock(victim);
			continue;
		}

		oom_score_adj = victim->signal->oom_score_adj;
		if (oom_score_adj < min_adj || oom_score_adj > max_adj) {
			task_unlock(victim);
			continue;
		}

		tasksize = get_mm_rss(victim->mm);
		task_unlock(victim);
		if (!tasksize)
			continue;

		get_task_struct(victim);
		if (do_send_sig_info(SIGKILL, SEND_SIG_FORCED, victim, true)) {
			put_task_struct(victim);
			continue;
		}

		victim->lmk_sigkill_sent = true;
		sched_setscheduler_nocheck(victim, SCHED_FIFO, &param);
		put_task_struct(victim);

		pages_freed += tasksize;
		if (pages_freed >= pages_needed)
			break;
	}
	rcu_read_unlock();

	return pages_freed;
}

static unsigned long do_lmk_reclaim(unsigned long pages_needed)
{
	unsigned long pages_freed = 0;
	int i;

	cpu_input_boost_kick_max(BOOST_DURATION_MS);
	devfreq_boost_kick_max(DEVFREQ_MSM_CPUBW, BOOST_DURATION_MS);

	for (i = 1; i < ARRAY_SIZE(adj_prio); i++) {
		pages_freed += scan_and_kill(adj_prio[i], adj_prio[i - 1],
					pages_needed - pages_freed);
		if (pages_freed >= pages_needed)
			break;
	}

	last_reclaim_jiffies = jiffies;
	return pages_freed * PAGE_SIZE / SZ_1M;
}

static void simple_lmk_reclaim_work(struct work_struct *work)
{
	unsigned long mib_freed = 0;

	mutex_lock(&reclaim_lock);
	if (time_after_eq(jiffies, last_reclaim_jiffies + LMK_KSWAPD_TIMEOUT))
		mib_freed = do_lmk_reclaim(MIN_FREE_PAGES);
	mutex_unlock(&reclaim_lock);

	if (mib_freed)
		pr_info("kswapd: freed %lu MiB\n", mib_freed);

	queue_delayed_work(simple_lmk_wq, &reclaim_work, LMK_KSWAPD_TIMEOUT);
}

void simple_lmk_force_reclaim(void)
{
	unsigned long mib_freed = 0;

	if (!atomic_read(&simple_lmk_ready))
		return;

	/* Only one memory reclaim event can occur at a time */
	if (!mutex_trylock(&reclaim_lock))
		return;

	if (time_after_eq(jiffies, last_reclaim_jiffies + LMK_OOM_TIMEOUT))
		mib_freed = do_lmk_reclaim(MIN_FREE_PAGES);
	mutex_unlock(&reclaim_lock);

	if (mib_freed)
		pr_info("oom: freed %lu MiB\n", mib_freed);
}

void simple_lmk_start_reclaim(void)
{
	if (!atomic_read(&simple_lmk_ready))
		return;

	queue_delayed_work(simple_lmk_wq, &reclaim_work, LMK_KSWAPD_TIMEOUT);
}

void simple_lmk_stop_reclaim(void)
{
	if (!atomic_read(&simple_lmk_ready))
		return;

	cancel_delayed_work_sync(&reclaim_work);
}

/* Initialize Simple LMK when LMKD in Android writes to the minfree parameter */
static int simple_lmk_init_set(const char *val, const struct kernel_param *kp)
{
	if (atomic_read(&simple_lmk_ready))
		return 0;

	simple_lmk_wq = alloc_workqueue("simple_lmk",
				WQ_HIGHPRI | WQ_UNBOUND | WQ_FREEZABLE, 0);
	BUG_ON(!simple_lmk_wq);

	atomic_set(&simple_lmk_ready, 1);
	return 0;
}

static const struct kernel_param_ops simple_lmk_init_ops = {
	.set = simple_lmk_init_set
};

/* Needed to prevent Android from thinking there's no LMK and thus rebooting */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "lowmemorykiller."
module_param_cb(minfree, &simple_lmk_init_ops, NULL, 0200);
