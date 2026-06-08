/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#ifndef _IPS_PRIVATE_H
#define _IPS_PRIVATE_H

#include <linux/kernel.h>
#include <linux/cpufreq.h>
#include <linux/pm_qos.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/*-------------common macros-------------------*/
#define ARCH_CLUSTER_NUM 4
#define DCVS_TRACE_LEN 128

/*-------------error statistics-------------------*/
/*
 * Runtime Error Statistics
 *
 * Error code format (32-bit):
 *   [31:24] Module ID
 *   [15:0]  Specific error code
 *
 * Note: Only runtime errors are tracked here.
 * Initialization errors are printed via pr_err and will cause
 * module load failure (sysfs unavailable anyway).
 *
 * PMU module (oplus_pmu.c) manages its own error statistics.
 * Use qcom_pmu_get_error_stats() to query PMU errors.
 */

/* Module IDs (for statistics only, not counted in total_errors) */
#define IPS_ERR_MOD_CORE	0x03
#define IPS_ERR_MOD_QOS		0x05

/* Build error code macro */
#define IPS_ERR_CODE(mod, code)		(((mod) << 24) | (code))

/* CORE runtime errors (statistics only) */
#define IPS_ERR_CORE_BOUNDARY_C0	IPS_ERR_CODE(IPS_ERR_MOD_CORE, 0x01)
#define IPS_ERR_CORE_BOUNDARY_C1	IPS_ERR_CODE(IPS_ERR_MOD_CORE, 0x02)
#define IPS_ERR_CORE_BOUNDARY_C2	IPS_ERR_CODE(IPS_ERR_MOD_CORE, 0x03)

/* QOS runtime errors (statistics only) */
#define IPS_ERR_QOS_LIST_FULL		IPS_ERR_CODE(IPS_ERR_MOD_QOS, 0x01)

/* Runtime error statistics structure (for non-critical errors) */
struct ips_error_stats {
	u32 last_error;			/* Last error code */
	u64 last_error_jiffies;		/* Last error timestamp (jiffies) */
	/* Per-module counters (statistics only, not critical) */
	u64 core_errors;
	u64 qos_errors;
};

extern struct ips_error_stats ips_err_stats;

/* Error reporting function (runtime errors only) */
void ips_record_error(u32 error_code);

/* Get module name from error code */
static inline const char *ips_err_mod_name(u32 error_code)
{
	switch ((error_code >> 24) & 0xFF) {
	case IPS_ERR_MOD_CORE:	 return "CORE";
	case IPS_ERR_MOD_QOS:	 return "QOS";
	default:		 return "UNKNOWN";
	}
}

/*-------------common inline functions-------------------*/
static inline long my_long_max(long a, long b)
{
	return (a > b) ? a : b;
}

static inline long my_long_min(long a, long b)
{
	return (a < b) ? a : b;
}

enum ips_ev_idx {
	/* AMU */
	IPS_INST_IDX, /* 0x11 instructions */
	IPS_CYC_IDX,  /* 0x08 cycles */
	IPS_CYC_CNT_IDX, /* 0X4004 cycle cnt */
	IPS_STALL_BKD_MEM_IDX, /* 0X4005 stall backend mem, aka mem stall */
	/* PMU */
	IPS_L1I_TLB_CR_IDX, /* 0x02 L1I_TLB_REFILL */
	IPS_MEM_ACCESS_IDX, /* 0x13 MEM_ACCESS */
	IPS_L1I_CA_IDX,/* 0x14 L1I_CACHE_ACCESS */
	IPS_STALL_BKD_IDX, /* 0x24 stall-backend */
	IPS_L1D_WB_V_IDX, /* 0x46, l1d cache write-back victim */
	IPS_L2D_CW_IDX, /* 0x51, l2d cache write */
	IPS_STALL_FTD_MEMBOUND_IDX,/* 0x8158 stall frontend membound */
	IPS_STALL_FTD_CPUBOUND_IDX,/* 0x8160 stall frontend cpubound */
	IPS_STALL_BKD_CPUBOUND_IDX,/* 0x816A stall backend cpubound */
	NUM_IPS_EVS
};

/* pmu events for IPS */
#define CYCLE_CNT_EV 0x4004
#define STALL_BK_MEM_EV	0x4005
#define L1D_CACHE_EV 0x04
#define L1D_TLB_REFILL_EV 0x05
#define BR_MIS_PRED_EV 0x10
#define MEM_ACCESS_EV 0x13
#define L1I_CACHE_EV 0x14
#define INST_BR_MISS_PRED_EV 0x22
#define STALL_FTD_EV 0x23
#define STALL_BKD_EV 0x24
#define L1I_TLB_EV 0x26
#define L3D_CACHE_REFILL_EV 0x2A
#define L3D_CACHE_EV 0x2B
#define L2D_TLB_REFILL_EV 0x2D
#define L2D_TLB_EV 0x2F
#define L1I_TLB_REFILL_EV 0x02
#define ITLB_WALK_EV 0x35 /* QCOM not used */
#define LL_CA_MIS_RD_EV 0x37
#define STREX_FAIL_SPEC_EV 0x6E
#define LDST_SPEC_EV 0x72
#define STALL_FTD_MEMBOUND_EV 0x8158
#define STALL_FTD_CPUBOUND_EV 0x8160
#define STALL_BKD_MEMBOUND_EV 0x8164
#define STALL_BKD_CPUBOUND_EV 0x816a
#define L1D_WB_V_EV 0x46 /* refer to hw */
#define L1D_WB_C_EV 0x47
#define L2D_W_EV 0x51
#define L2D_WB_V_EV 0x56 /* refer to hw */
#define LDST_SPEC 0x72 /* unsupport */
#define L2D_CA_EV 0x16 /* 0x16 L2D_CACHE_REFILL */
#define L2D_CR_EV 0x17 /* 0x17 L2D_CACHE_REFILL */
#define IPS_QCOM_LLC_MISS_RD_EV 0x1000 /* 0x1000 qcom llc miss read */


/*-------------ips_freqqos.c-------------------*/
#define IPS_STACK_TRACE_LEN 4
#define IPS_MODULE_NAME 64
#define IPS_FREQ_QOS_LRU_SIZE 32

struct freq_qos_stats {
	struct list_head client_list;
	int cid;
	struct freq_constraints *qos;
	struct freq_qos_request *req;
	int type;
	int value;
	int pid;
	char comm[TASK_COMM_LEN];
	char src;
	u64 update_ts;
	u64 count;
	unsigned long stacktrace[IPS_STACK_TRACE_LEN];
	char tag[IPS_MODULE_NAME]; /* perfetto show */
};

int freq_qos_request_init(void);
void freq_qos_request_exit(void);

/* ips_freqqos.c extern variables */
extern spinlock_t freq_qos_client_lock;
extern u32 qos_client_list_num;
extern u32 qos_stats_lru_num[2];
extern struct list_head qos_client_list;
extern struct freq_qos_stats ips_qos_stats_lru[2][IPS_FREQ_QOS_LRU_SIZE];

/*-------------ips_core.c-------------------*/
#include "ips_table_chip.h"

#define IPS_CPU_FREQ_MAX_NUM 32
#define IPS_DYN_TARGET_LOAD_LEVEL 5
#define IPS_TARGET_LOAD_MIN 50
#define IPS_TARGET_LOAD_MAX 150
#define IPS_BCM_UTIL_RATE_MIN 600
#define IPS_BCM_UTIL_RATE_MAX 1000
#define IPS_CPUFREQ_INDEX_RANGE_MIN 3
#define IPS_CPUFREQ_INDEX_RANGE_MAX 6

int get_cid_from_cpu(int cpu);
void ips_algo_init(void);
void ips_algo_exit(void);
void ips_set_target_freq(int cid, int pl_cpu, int cpu, int target_freq_index,
			 long mem_stall, long inst_stall_CPI,
			 u64 my_demand_ips, unsigned long util_rate, int bcm_value);
int ips_find_target_freq_idx(u64 demand_ips, long mem_stall,
			     long inst_stall_CPI, int cpu, u64 real_ips, u64 raw_freq,
			     unsigned long util_rate, int *bcm_value, u32 ips_line[3]);
void ips_algo_time_histogram_stats(u64 cost_ns);
void cluster_cpi_histogram_stats(u32 cluster, u32 cpi);

/* ips_core.c extern variables */
extern struct freq_qos_stats ips_qos_stats_cluster[ARCH_CLUSTER_NUM][2];
extern u32 IPS_MAX_CLUSTER_NUM;
extern u32 IPS_MAX_CPU_NUM;
extern struct cpumask ips_cluster_cpumask[ARCH_CLUSTER_NUM];
extern u32 ips_cluster_max_freq[ARCH_CLUSTER_NUM];
extern u32 ips_cpu_capacity[NR_CPUS];
extern u32 ips_c0_cpufreq_table[IPS_CPU_FREQ_MAX_NUM];
extern u32 ips_c1_cpufreq_table[IPS_CPU_FREQ_MAX_NUM];
extern u32 ips_c2_cpufreq_table[IPS_CPU_FREQ_MAX_NUM];
extern u32 IPS_C0_CPUFREQ_NUM;
extern u32 IPS_C1_CPUFREQ_NUM;
extern u32 IPS_C2_CPUFREQ_NUM;
extern u32 IPS_CLUSTER0_FREQ_MAX_IDX;
extern u32 IPS_CLUSTER1_FREQ_MAX_IDX;
extern u32 IPS_CLUSTER2_FREQ_MAX_IDX;
extern u32 ips_prev_vote_freq[ARCH_CLUSTER_NUM][2];
extern u32 c0_bcpi_mstall_table[IPS_CLUSTER0_BASE_CPI_NUM][IPS_CLUSTER0_MEM_STALL_NUM];
extern u32 c1_bcpi_mstall_table[IPS_CLUSTER1_BASE_CPI_NUM][IPS_CLUSTER1_MEM_STALL_NUM];
extern u32 c2_bcpi_mstall_table[IPS_CLUSTER2_BASE_CPI_NUM][IPS_CLUSTER2_MEM_STALL_NUM];
extern u32 c0_bcpi_mstall_table_vote[IPS_CLUSTER0_BASE_CPI_NUM][IPS_CLUSTER0_MEM_STALL_NUM];
extern u32 c1_bcpi_mstall_table_vote[IPS_CLUSTER1_BASE_CPI_NUM][IPS_CLUSTER1_MEM_STALL_NUM];
extern u32 c2_bcpi_mstall_table_vote[IPS_CLUSTER2_BASE_CPI_NUM][IPS_CLUSTER2_MEM_STALL_NUM];
extern u32 ips_cluster_cm;
extern u32 ips_target_load[ARCH_CLUSTER_NUM];
extern u32 ips_dyn_target_load[IPS_DYN_TARGET_LOAD_LEVEL];
extern bool ips_use_dyn_tl;
extern bool ips_bcm_limit_enable;
extern u32 ips_bcm_demand_discount1[ARCH_CLUSTER_NUM];
extern u32 ips_bcm_demand_discount2[ARCH_CLUSTER_NUM];
extern u32 ips_bcm_util_rate_threshold1[ARCH_CLUSTER_NUM];
extern u32 ips_bcm_util_rate_threshold2[ARCH_CLUSTER_NUM];
extern int ips_mstall_idx;
extern bool ips_use_mstall_limit;
extern u32 ips_mstall_discount;
extern bool ips_he_limit;
extern bool ips_le_limit;
extern int ips_le_bcpi_idx;
extern int ips_le_mstall_idx;
extern int ips_le_util;
extern int ips_le_demand_threshold;
extern int ips_le_freq_idx[ARCH_CLUSTER_NUM];
extern bool ips_on;
extern u32 ips_bcpi_model_num[ARCH_CLUSTER_NUM];
extern u32 ips_freq_slope;
extern bool ips_vote_cpufreq[ARCH_CLUSTER_NUM];
extern spinlock_t ips_vote_cpufreq_lock;
extern u32 ips_vote_cpufreq_type;
extern char *ips_vote_type_str[];
extern int ips_vote_cpufreq_range[ARCH_CLUSTER_NUM];
extern bool is_smooth;
extern bool is_chain;
extern bool ips_atrace;
extern char ips_cpu_vote_tag[NR_CPUS][DCVS_TRACE_LEN];
/* IPS sampling state interface for governor */
bool ips_is_sampling_enabled(void);
extern u64 ips_reset_stats_ts;
extern u32 total_ipc;
extern u64 total_ips;
extern u64 total_average_freq;
extern u64 total_instrs_acc;
extern u64 total_cycles_acc;
extern u64 total_ips_algo_count;
extern u64 cluster_instrs_acc[ARCH_CLUSTER_NUM];
extern u64 cluster_cycles_acc[ARCH_CLUSTER_NUM];
extern u64 cluster_average_freq[ARCH_CLUSTER_NUM];
extern u64 cluster_ips[ARCH_CLUSTER_NUM];
extern u32 cluster_ipc[ARCH_CLUSTER_NUM];
#define IPS_ALGO_TIME_BASE 10000
#define IPS_ALGO_TIME_STATS_SIZE 20
#define IPS_CPI_HISTOGRAM_SIZE 10
extern u32 c0_cpi_hist[IPS_CPI_HISTOGRAM_SIZE];
extern u32 c1_cpi_hist[IPS_CPI_HISTOGRAM_SIZE];
extern u32 c2_cpi_hist[IPS_CPI_HISTOGRAM_SIZE];
extern u64 ips_algo_t_hist[IPS_ALGO_TIME_STATS_SIZE];
#define IPS_DEMAND_IPS_HIST_SIZE 20
extern u64 ips_demand_hist[ARCH_CLUSTER_NUM][IPS_DEMAND_IPS_HIST_SIZE];
extern u32 per_cpu_ipc[NR_CPUS];
extern u64 per_cpu_instrs[NR_CPUS];
extern u64 per_cpu_cycles[NR_CPUS];
extern u64 per_cpu_cycle_cnt[NR_CPUS];
extern u64 vsync_count;
extern u64 delta_vsync_us;
extern u64 prev_freq[NR_CPUS];
extern long prev_mem_stall[NR_CPUS][2];
extern long prev_inst_CPI[NR_CPUS][2];
extern u64 prev_cpu_demand_ips[NR_CPUS];

#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_QCOM)
#define IPS_TIME_NS_PER_CYC_CNT 52
#else
#define IPS_TIME_NS_PER_CYC_CNT 1
#endif

/* ips_core.c tracing functions */
void tracing_mark_write(char type, u32 pid, const char *track, u64 val,
			const char *name, bool force);

/* Controlled by ips_atrace - for debug tracing (force=false) */
/* async for track begin: G; end: H */
#define DCVS_ATRACE_H(pid, track, value) tracing_mark_write('H', pid, track, value, NULL, false)
#define DCVS_ATRACE_G(pid, track, name, value) tracing_mark_write('G', pid, track, value, name, false)
/* pid as track name */
#define DCVS_ATRACE_B(pid, name) tracing_mark_write('B', pid, NULL, 0, name, false)
#define DCVS_ATRACE_E(pid)       tracing_mark_write('E', pid, NULL, 0, NULL, false)
#define DCVS_ATRACE_I(pid, track) tracing_mark_write('I', pid, track, 0, NULL, false)
#define DCVS_ATRACE_N(track, name) tracing_mark_write('N', 0, track, 0, name, false)
#define DCVS_ATRACE_C(track, value) tracing_mark_write('C', 0, track, value, NULL, false)
#define DCVS_ATRACE_S(track, value) tracing_mark_write('S', 0, track, value, NULL, false)
#define DCVS_ATRACE_F(track, value) tracing_mark_write('F', 0, track, value, NULL, false)

/* Always enabled - for critical events like governor start/stop (force=true) */
#define DCVS_ATRACE_N2(track, name) tracing_mark_write('N', 0, track, 0, name, true)

/*-------------ips_governor.c-------------------*/
int ipsgov_register(void);
void ipsgov_unregister(void);
void ips_gov_update_cpufreq(unsigned int cpu, unsigned int target_freq);
void ips_gov_get_cluster_stats(int cid, u64 *start_count, u64 *stop_count,
			       u64 *total_time_ms, bool *is_active);

/*-------------ips_stats.c-------------------*/
enum dcvs_hw_type {
	DCVS_DDR,
	DCVS_SLC,
	DCVS_L3,
	NUM_DCVS_HW_TYPES
};

int ips_dcvs_hw_init(void);
void ips_dcvs_hw_exit(void);
void dcvs_stats_record_time(enum dcvs_hw_type hw_type, unsigned int new_freq);
ssize_t show_ddr_time_in_state(struct kobject *kobj, struct attribute *attr, char *buf);
ssize_t show_ddr_available_frequencies(struct kobject *kobj, struct attribute *attr, char *buf);

#endif /* _IPS_PRIVATE_H */
