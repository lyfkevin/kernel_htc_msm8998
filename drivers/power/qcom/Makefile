obj-$(CONFIG_HTC_POWER_DEBUG)   += htc_util.o htc_cpu_usage_stats.o
obj-$(CONFIG_MSM_IDLE_STATS)	+= lpm-stats.o
obj-$(CONFIG_APSS_CORE_EA)	+= msm-core.o
obj-$(CONFIG_MSM_APM)		+= apm.o

ifeq ($(CONFIG_APSS_CORE_EA),y)
obj-$(CONFIG_DEBUG_FS)	+= debug_core.o
endif
