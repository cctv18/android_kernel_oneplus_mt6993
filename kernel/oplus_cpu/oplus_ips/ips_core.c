// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/arch_topology.h>
#include <linux/cpuidle.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <uapi/linux/sched/types.h>
#include <trace/hooks/cpuidle.h>
#include <trace/hooks/power.h>
#include "trace_ips.h"
#include "ips_table_chip.h"
#include "ips_private.h"

/* enum ips_ev_idx defined in ips_private.h */



/* vote ddr freq api defined in ips_memlat.c */

/* online table gen */
#define IPS_CPU_FREQ_MAX_NUM 32
#define IPS_CPU_MEM_STALL_MAX_NUM 25
#define IPS_CPU_BASE_CPI_MAX_NUM 23
/* base cpi and mem stall range get from sampling */
static int base_CPI_coef[3] = {2, 1, 25};
static int mem_stall_coef[3] = {0, 10, 250};


/* IPS_TIME_NS_PER_CYC_CNT defined in ips_private.h */
/*-------------basic var for ips algo-------------------*/
/* get cpu topology & cpufreq info start@ips_algo_init */
u32 IPS_MAX_CLUSTER_NUM = 0;
u32 IPS_MAX_CPU_NUM = 0;
struct cpumask ips_cluster_cpumask[ARCH_CLUSTER_NUM];
u32 ips_cluster_max_freq[ARCH_CLUSTER_NUM] = {0};
static u32 ips_physical_cpus[NR_CPUS];
u32 ips_cpu_capacity[NR_CPUS] = {0};
u32 ips_c0_cpufreq_table[IPS_CPU_FREQ_MAX_NUM] = {0};
u32 ips_c1_cpufreq_table[IPS_CPU_FREQ_MAX_NUM] = {0};
u32 ips_c2_cpufreq_table[IPS_CPU_FREQ_MAX_NUM] = {0};
static u32 ips_cpufreq_num[ARCH_CLUSTER_NUM];
u32 IPS_C0_CPUFREQ_NUM = 0;
u32 IPS_C1_CPUFREQ_NUM = 0;
u32 IPS_C2_CPUFREQ_NUM = 0;
u32 IPS_CLUSTER0_FREQ_MAX_IDX = 0;
u32 IPS_CLUSTER1_FREQ_MAX_IDX = 0;
u32 IPS_CLUSTER2_FREQ_MAX_IDX = 0;
/* TODO: use this instead of above */
static u32 ips_cluster_freq_max_idx[ARCH_CLUSTER_NUM] = {0};
/* get cpu topology & cpufreq info end */


static bool ready_for_qos_update = false;
/* cluster cpufreq policy default constraint 0-min,1-max */
/* defined in ips_private.h as extern */
struct freq_qos_stats ips_qos_stats_cluster[ARCH_CLUSTER_NUM][2];


u64 prev_freq[NR_CPUS] = {0};
/* stall_SPI aka mem_stall
 * 0-mem_stall: raw
 * 1-my_mem_stall: smmoth */
long prev_mem_stall[NR_CPUS][2] = {0};
/* base_CPI aka inst_stall_CPI
 * 0-inst_CPI
 * 1-my_inst_CPI
 */
long prev_inst_CPI[NR_CPUS][2] = {0};
/* previous demand ips */
u64 prev_cpu_demand_ips[NR_CPUS] = {0};
/* record prev vote min:0/max:1 freq idx */
u32 ips_prev_vote_freq[ARCH_CLUSTER_NUM][2] = {0};
/* freq instead of index */
static u32 ips_cur_vote_freq[ARCH_CLUSTER_NUM][2] = {0};
static u32 ips_lst_vote_freq[ARCH_CLUSTER_NUM][2] = {0};

/*-------------stats for ips algo-------------------*/
/* Comprehensive error statistics */
struct ips_error_stats ips_err_stats = {0};

/*
 * Record a non-critical runtime error (statistics only).
 * Critical errors (PMU hotplug) are tracked by oplus_pmu.c.
 * CORE/QOS errors here are for debugging purposes only.
 */
void ips_record_error(u32 error_code)
{
	u8 mod_id = (error_code >> 24) & 0xFF;

	/* Update last error info */
	ips_err_stats.last_error = error_code;
	ips_err_stats.last_error_jiffies = jiffies;

	/* Update per-module counters (statistics only) */
	switch (mod_id) {
	case IPS_ERR_MOD_CORE:
		ips_err_stats.core_errors++;
		break;
	case IPS_ERR_MOD_QOS:
		ips_err_stats.qos_errors++;
		break;
	}

	/* Log with rate limiting */
	pr_warn_ratelimited("ips: error 0x%08x (%s)\n",
			    error_code, ips_err_mod_name(error_code));
}

/* online stats, total indice */
u64 ips_reset_stats_ts = 0;

u32 total_ipc;
u64 total_ips;
u64 total_average_freq;
u64 total_instrs_acc;
u64 total_cycles_acc;
u64 total_ips_algo_count;

/* per cluster stats */
u64 cluster_instrs_acc[ARCH_CLUSTER_NUM] = {0};
u64 cluster_cycles_acc[ARCH_CLUSTER_NUM] = {0};
u64 cluster_average_freq[ARCH_CLUSTER_NUM] = {0};
u64 cluster_ips[ARCH_CLUSTER_NUM] = {0};
u32 cluster_ipc[ARCH_CLUSTER_NUM] = {0};
/* 10us: 0-9us, 10-19us, 20us-29us etc... */
/* IPS_ALGO_TIME_BASE, IPS_ALGO_TIME_STATS_SIZE, IPS_CPI_HISTOGRAM_SIZE defined in ips_private.h */
u32 c0_cpi_hist[IPS_CPI_HISTOGRAM_SIZE] = {0};
u32 c1_cpi_hist[IPS_CPI_HISTOGRAM_SIZE] = {0};
u32 c2_cpi_hist[IPS_CPI_HISTOGRAM_SIZE] = {0};
u64 ips_algo_t_hist[IPS_ALGO_TIME_STATS_SIZE] = {0};

/* ips range: [0, 1000, 2000, ... 19000,20000], interval 1000 */
/* IPS_DEMAND_IPS_HIST_SIZE defined in ips_private.h */
u64 ips_demand_hist[ARCH_CLUSTER_NUM][IPS_DEMAND_IPS_HIST_SIZE] = {{0}};

/* stats all */
u32 c0_bcpi_mstall_table[IPS_CLUSTER0_BASE_CPI_NUM][IPS_CLUSTER0_MEM_STALL_NUM] = {{0}};
u32 c1_bcpi_mstall_table[IPS_CLUSTER1_BASE_CPI_NUM][IPS_CLUSTER1_MEM_STALL_NUM] = {{0}};
u32 c2_bcpi_mstall_table[IPS_CLUSTER2_BASE_CPI_NUM][IPS_CLUSTER2_MEM_STALL_NUM] = {{0}};
/* stats vote only */
u32 c0_bcpi_mstall_table_vote[IPS_CLUSTER0_BASE_CPI_NUM][IPS_CLUSTER0_MEM_STALL_NUM] = {{0}};
u32 c1_bcpi_mstall_table_vote[IPS_CLUSTER1_BASE_CPI_NUM][IPS_CLUSTER1_MEM_STALL_NUM] = {{0}};
u32 c2_bcpi_mstall_table_vote[IPS_CLUSTER2_BASE_CPI_NUM][IPS_CLUSTER2_MEM_STALL_NUM] = {{0}};
/* default show cluster0 base_CPI & stall_SPI */
u32 ips_cluster_cm = 0;
/* per cpu stats */
u32 per_cpu_ipc[NR_CPUS] = {0};
u64 per_cpu_instrs[NR_CPUS] = {0};
u64 per_cpu_cycles[NR_CPUS] = {0};
u64 per_cpu_cycle_cnt[NR_CPUS] = {0};
/* vsync or sched tick count */
u64 vsync_count = 0;
u64 delta_vsync_us;

/* ips freq qos client stats */
static const char *ips_qos_freq_restore_str[ARCH_CLUSTER_NUM] = {
	"ips_qos_restore_c0",
	"ips_qos_restore_c1",
	"ips_qos_restore_c2",
	"ips_qos_restore_c3"
};
static const char *cluster_atrace_tag[ARCH_CLUSTER_NUM] = {
	"c0_ips",
	"c1_ips",
	"c2_ips",
	"c3_ips"
};


/*-------------interface for ips algo-------------------*/
/* target load range: [50, 150]
 * cpi modle 0-4,2 default: 83
 * cpi model 1-5,3 default: 87
 */
#define IPS_TARGET_LOAD_MIN 50
#define IPS_TARGET_LOAD_MAX 150
u32 ips_target_load[ARCH_CLUSTER_NUM] = {80, 70, 70};
#define IPS_DYN_TARGET_LOAD_LEVEL 5
/* fine tune
 * 1000-901: 0-95 5%
 * 900-801:  1-92 8%
 * 800-701:  2-90 10%
 * 700-601:  3-87 13%
 * 600-501:  4-85 15%
 * 500-000:  4-85 15%
 */
u32 ips_dyn_target_load[IPS_DYN_TARGET_LOAD_LEVEL] = {95, 92, 90, 87, 85};
bool ips_use_dyn_tl = false;

/* task similar with benchmark cosume more ips with higher freq.
 * discount ips demand to prevent hysteria boost freq.
 */
#define IPS_BCM_MEM_STALL_MAX_IDX 1
#define IPS_BCM_BASE_CPI_MAX_IDX  4
#define IPS_BCM_UTIL_RATE_MIN 600
#define IPS_BCM_UTIL_RATE_MAX 1000
/* todo: soc dependend */
bool ips_bcm_limit_enable = false;
u32 ips_bcm_demand_discount1[ARCH_CLUSTER_NUM] = {130, 130, 130, 130};
u32 ips_bcm_demand_discount2[ARCH_CLUSTER_NUM] = {110, 110, 110, 110};
static u32 ips_bcm_demand_ips_threshold[ARCH_CLUSTER_NUM] = {10000, 18000, 18000};
u32 ips_bcm_util_rate_threshold1[ARCH_CLUSTER_NUM] = {920, 920, 920};
u32 ips_bcm_util_rate_threshold2[ARCH_CLUSTER_NUM] = {850, 900, 920};

/* high mem stall limit */
int ips_mstall_idx = 12;
bool ips_use_mstall_limit = false;
u32 ips_mstall_discount = 90;
bool ips_he_limit = false;
#define LIMIT_TRACE_NAME "limit_track"

/* low demand ips with high freq vote limit */
bool ips_le_limit = false;
int ips_le_bcpi_idx = 4;
int ips_le_mstall_idx = 4;
int ips_le_util = 930;
int ips_le_demand_threshold = 3500;
/* todo: use freq instead of idx: 1670400, 1632000 */
int ips_le_freq_idx[ARCH_CLUSTER_NUM] = {11, 7, 7};

/* true: ips works; false: ips off with stats still works */
bool ips_on = true;

/* base_CPI model select; 1: fixed; 0: first version */
u32 ips_bcpi_model_num[ARCH_CLUSTER_NUM] = {0, 0, 0, 0};
/* slope algo perf first. 1: rel; others: abs*/
u32 ips_freq_slope = 1;

/* ips vote cpufreq config start */
struct ips_vinfo {
	int cpu;
	int min;
	int max;
};
static struct ips_vinfo ips_vote_msg[ARCH_CLUSTER_NUM];
static spinlock_t ips_vote_lock[ARCH_CLUSTER_NUM];
/* true: ips vote cpufreq; false: sample & stats but no vote only */
bool ips_vote_cpufreq[ARCH_CLUSTER_NUM] = {false, false, false};
/*
 * Spinlock to protect ips_vote_cpufreq array state changes.
 * Used to synchronize:
 * - Governor start/stop (write)
 * - Sampling enable/disable checks (read with decision)
 * - sysfs show (read)
 */
DEFINE_SPINLOCK(ips_vote_cpufreq_lock);
/* 0: qos self request(default);
 * 1: cpufreq_walt;
 * 2: ips cpufreq governor
 */
u32 ips_vote_cpufreq_type = 1;
char *ips_vote_type_str[] = {
	"raw", /* calc raw freq from formula */
	"old_slope", /* default: 1, use min_freq slope when > raw index */
	"new_slope" /* working */
};
/* min max freq range: [3, 6], default: 3 */
#define IPS_CPUFREQ_INDEX_RANGE_MIN 3
#define IPS_CPUFREQ_INDEX_RANGE_MAX 6
int ips_vote_cpufreq_range[ARCH_CLUSTER_NUM] = {4, 4, 4};
/* ips vote cpufreq config end */

/* true: filter; false: use previous */
bool is_smooth = false;
/* true: my_demand_ips; false: current demand_ips */
bool is_chain = false;

/*-------------debug for ips algo-------------------*/
bool ips_atrace = false;

/*
 * Unified tracing function for Android systrace/perfetto.
 * @force: if true, always print regardless of ips_atrace setting.
 *         Used for critical events like governor start/stop.
 */
void tracing_mark_write(char type, u32 pid, const char *track, u64 val,
			const char *name, bool force)
{
	char trace_buf[DCVS_TRACE_LEN];

	if (!force && likely(!ips_atrace))
		return;
	if ('N' == type)
		scnprintf(trace_buf, DCVS_TRACE_LEN, "%c|%u|%s|%s\n",
			  type, pid, track, name);
	else if ('G' == type)
		scnprintf(trace_buf, DCVS_TRACE_LEN, "%c|%u|%s|%s|%llu\n",
			  type, pid, track, name, val);
	else if ('B' == type)
		scnprintf(trace_buf, DCVS_TRACE_LEN, "%c|%u|%s\n", type, pid, name);
	else if ('E' == type)
		scnprintf(trace_buf, DCVS_TRACE_LEN, "%c|%u\n", type, pid);
	else
		scnprintf(trace_buf, DCVS_TRACE_LEN, "%c|%u|%s|%llu\n", type, pid, track, val);
	trace_printk(trace_buf);
}



static char ips_vote_tag[ARCH_CLUSTER_NUM][DCVS_TRACE_LEN];
char ips_cpu_vote_tag[NR_CPUS][DCVS_TRACE_LEN];
static unsigned long ips_cookie[ARCH_CLUSTER_NUM];

/*-------------table for ips algo-------------------*/
/* IPS table */
static int c0_ips_stall_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM][IPS_CPU_FREQ_MAX_NUM];
static int c1_ips_stall_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM][IPS_CPU_FREQ_MAX_NUM];
static int c2_ips_stall_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM][IPS_CPU_FREQ_MAX_NUM];

/* IPS slope table: upper & lower table */
static int c0_ips_rel_upper_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];
static int c0_ips_rel_lower_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];
static int c1_ips_rel_upper_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];
static int c1_ips_rel_lower_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];
static int c2_ips_rel_upper_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];
static int c2_ips_rel_lower_table[IPS_CPU_BASE_CPI_MAX_NUM][IPS_CPU_MEM_STALL_MAX_NUM];

/* my_long_max and my_long_min moved to ips_private.h */

#undef MAX
#undef MIN
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* IPS table gen */
static void ips_gen_perf_stall_table(int ips_table[][IPS_CPU_MEM_STALL_MAX_NUM][IPS_CPU_FREQ_MAX_NUM],
				     int freq_list[IPS_CPU_FREQ_MAX_NUM],
				     int base_cpi_num,
				     int mem_stall_num,
				     int freq_num,
				     int base_cpi_coef[3],
				     int mem_stall_coef[3],
				     int max_cap,
				     int max_freq)
{
	int base_cpi_idx, mem_stall_idx, freq_idx;
	long frequency, cap, base_cpi, mem_stall_spi, ips;
	/* frequency unit: kHz(not Hz), it won't overflow */
	for (base_cpi_idx = 0; base_cpi_idx < base_cpi_num; base_cpi_idx++) {
		for (mem_stall_idx = 0; mem_stall_idx < mem_stall_num; mem_stall_idx++) {
			base_cpi = base_cpi_coef[0] + base_cpi_coef[1] * base_cpi_idx;
			mem_stall_spi = mem_stall_coef[0] + mem_stall_coef[1] * mem_stall_idx;
			for (freq_idx = 0; freq_idx < freq_num; freq_idx++) {
				frequency = freq_list[freq_idx];
				ips = frequency * 1000 * 10 * 10000 * 100 / (mem_stall_spi * frequency / 100 + base_cpi * 100000);
				cap = ips*max_cap / max_freq / 1000;
				ips_table[base_cpi_idx][mem_stall_idx][freq_idx] = cap;
			}
		}
	}
}

/* IPS slope table gen */
static void ips_gen_perf_slope_table(int slope_table[][IPS_CPU_MEM_STALL_MAX_NUM],
			     int freq_list[IPS_CPU_FREQ_MAX_NUM],
			     int base_cpi_num,
			     int mem_stall_num,
			     int freq_num,
			     int base_cpi_coef[3],
			     int mem_stall_coef[3],
			     int slope_type, int ef_rate)
{
	int base_cpi_idx, mem_stall_idx, freq_idx;
	long prev_ips, ips, frequency, prev_freq, second_freq, second_ips, first_perf_diff, base_cpi, mem_stall_spi;
	/* todo: check para */
	for (base_cpi_idx = 0; base_cpi_idx < base_cpi_num; base_cpi_idx++) {
		base_cpi = base_cpi_coef[0] + base_cpi_coef[1] * base_cpi_idx;
		for (mem_stall_idx = 0; mem_stall_idx < mem_stall_num; mem_stall_idx++) {
			mem_stall_spi = mem_stall_coef[0] + mem_stall_coef[1] * mem_stall_idx;
			prev_freq = freq_list[0];
			prev_ips = prev_freq * 1000 * 10 * 10000 / (mem_stall_spi * prev_freq  / 1000 + base_cpi * 10000);

			for (freq_idx = 0; freq_idx < freq_num; freq_idx++) {
				frequency = freq_list[freq_idx];
				ips = frequency * 1000 * 10 * 10000 / (mem_stall_spi * frequency  / 1000 + base_cpi * 10000);

				/* rel ref */
				if (slope_type == 1) {
					if (freq_idx == freq_num - 1)
						slope_table[base_cpi_idx][mem_stall_idx] = freq_idx;
					else if (((ips - prev_ips) * 1000000 / prev_ips) < (frequency - prev_freq) * 10000 * ef_rate / prev_freq) {
						slope_table[base_cpi_idx][mem_stall_idx] = freq_idx;
						break;
					}
				} else {
					/* abs ref */
					if (freq_idx == 0) {
						second_freq = freq_list[1];
						second_ips = second_freq * 1000 * 10 * 10000 / (mem_stall_spi * second_freq / 1000 + base_cpi * 10000);
						first_perf_diff = second_ips - ips;
						continue;
					}
					if (freq_idx == freq_num - 1)
						slope_table[base_cpi_idx][mem_stall_idx] = freq_idx;
					else if ((ips - prev_ips) < (first_perf_diff * ef_rate / 100)) {
						slope_table[base_cpi_idx][mem_stall_idx] = freq_idx;
						break;
					}
				}

				prev_ips = ips;
				prev_freq = frequency;
			}
		}
	}
}

static void ips_table_init(void)
{
	ips_gen_perf_stall_table(c0_ips_stall_table, ips_c0_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C0_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 ips_cpu_capacity[cpumask_first(&ips_cluster_cpumask[0])],
				 ips_cluster_max_freq[0]);
	ips_gen_perf_stall_table(c1_ips_stall_table, ips_c1_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C1_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 ips_cpu_capacity[cpumask_first(&ips_cluster_cpumask[1])],
				 ips_cluster_max_freq[1]);
	if (IPS_C2_CPUFREQ_NUM != 0)
	ips_gen_perf_stall_table(c2_ips_stall_table, ips_c2_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C2_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 ips_cpu_capacity[cpumask_first(&ips_cluster_cpumask[2])],
				 ips_cluster_max_freq[2]);
	/* cluster0,1 rel upper & lower table */
	ips_gen_perf_slope_table(c0_ips_rel_upper_table, ips_c0_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C0_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 60);
	ips_gen_perf_slope_table(c0_ips_rel_lower_table, ips_c0_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C0_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 40);
	ips_gen_perf_slope_table(c1_ips_rel_upper_table, ips_c1_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C1_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 60);
	ips_gen_perf_slope_table(c1_ips_rel_lower_table, ips_c1_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C1_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 40);
	if (IPS_C2_CPUFREQ_NUM != 0) {
	ips_gen_perf_slope_table(c2_ips_rel_upper_table, ips_c2_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C2_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 60);
	ips_gen_perf_slope_table(c2_ips_rel_lower_table, ips_c2_cpufreq_table,
				 IPS_CPU_BASE_CPI_MAX_NUM,
				 IPS_CPU_MEM_STALL_MAX_NUM,
				 IPS_C2_CPUFREQ_NUM,
				 base_CPI_coef,
				 mem_stall_coef,
				 1, 40);
	}
}

/* ips process time histogram stats */
void ips_algo_time_histogram_stats(u64 cost_ns)
{
	u32 idx = cost_ns / IPS_ALGO_TIME_BASE;
	total_ips_algo_count += 1;

	if (idx < IPS_ALGO_TIME_STATS_SIZE - 1)
		ips_algo_t_hist[idx] += 1;
	else
		ips_algo_t_hist[IPS_ALGO_TIME_STATS_SIZE - 1] += 1;
}

/* demand ips vote stats */
static inline void ips_demand_ips_histogram_stats(u32 cluster_id, u64 demand_ips)
{
	u32 idx = demand_ips / 1000;
	if (idx < IPS_DEMAND_IPS_HIST_SIZE - 1)
		ips_demand_hist[cluster_id][idx] += 1;
	else
		ips_demand_hist[cluster_id][IPS_DEMAND_IPS_HIST_SIZE -1] += 1;
	return;
}

/* cpi histogram stats */
void cluster_cpi_histogram_stats(u32 cluster, u32 cpi)
{
	u32 *cluster_cpi_his = NULL;
	u32 idx = cpi / 100;

	if (cpi == 0)
		return;

	if (cluster == 0)
		cluster_cpi_his = c0_cpi_hist;
	else if (cluster == 1)
		cluster_cpi_his = c1_cpi_hist;
	else if (cluster == 2)
		cluster_cpi_his = c2_cpi_hist;
	else
		cluster_cpi_his = c0_cpi_hist;

	if (idx < IPS_CPI_HISTOGRAM_SIZE - 1)
		cluster_cpi_his[idx] += 1;
	else
		cluster_cpi_his[IPS_CPI_HISTOGRAM_SIZE - 1] += 1;
}

/* intital ips variable */
static void ips_cpu_info_init(void)
{
	int max_cluster_id = 0;
	unsigned int cpu, idx, cluster_id, freq_count;
	struct cpufreq_frequency_table *table;
	struct cpufreq_policy *policy;

	/* show cpu topology and capacity */
	for (idx = 0; idx < ARCH_CLUSTER_NUM; idx++) {
		cpumask_clear(&ips_cluster_cpumask[idx]);
	}

	/* cluster and cpu mapping */
	for_each_possible_cpu(cpu) {
		idx = topology_cluster_id(cpu);
		if (idx > max_cluster_id)
			max_cluster_id = idx;
		cpumask_set_cpu(cpu, &ips_cluster_cpumask[idx]);
		ips_cpu_capacity[cpu] = arch_scale_cpu_capacity(cpu);
		ips_physical_cpus[cpu] = 1;
		IPS_MAX_CPU_NUM++;
		pr_info("cpu=%u, cpu_capacity %u(arch_scale), cluster_id=%u\n",
		       cpu, ips_cpu_capacity[cpu], idx);
	}
	IPS_MAX_CLUSTER_NUM = max_cluster_id + 1;

	/* cpufreq info */
	for (cluster_id = 0; cluster_id < IPS_MAX_CLUSTER_NUM; cluster_id++) {
		cpu = cpumask_first(&ips_cluster_cpumask[cluster_id]);
		pr_info("cluster%d first cpu is %d", cluster_id, cpu);
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("cpufreq_cpu_get failed for cpu%d, cluster%d\n", cpu, cluster_id);
			ips_on = false;
			continue;
		}
		table = policy->freq_table;
		if (table) {
			freq_count = cpufreq_table_count_valid_entries(policy);
			for (idx = 0; idx < freq_count; idx++) {
				if (policy->freq_table_sorted ==
				    CPUFREQ_TABLE_SORTED_DESCENDING) {
					/* mtk cpufreq index descend */
					if (cluster_id == 0)
						ips_c0_cpufreq_table[idx] = table[freq_count - idx - 1].frequency;
					else if (cluster_id == 1)
						ips_c1_cpufreq_table[idx] = table[freq_count - idx - 1].frequency;
					else if (cluster_id == 2)
						ips_c2_cpufreq_table[idx] = table[freq_count - idx - 1].frequency;
					else {
						pr_err("fatal error: soc cluster max than 3!");
						ips_on = false;
						break;
					}
					pr_debug("cluster%u idx=%u sorted=%u inefficiency=%d freq=%u",
					       cluster_id, idx,
					       policy->freq_table_sorted,
					       table[freq_count - idx - 1].flags & CPUFREQ_INEFFICIENT_FREQ ? 1 : 0,
					       table[freq_count - idx - 1].frequency);
				} else {
					if (cluster_id == 0)
						ips_c0_cpufreq_table[idx] = table[idx].frequency;
					else if (cluster_id == 1)
						ips_c1_cpufreq_table[idx] = table[idx].frequency;
					else if (cluster_id == 2)
						ips_c2_cpufreq_table[idx] = table[idx].frequency;
					else {
						pr_err("fatal error: soc cluster max than 3!");
						ips_on = false;
						break;
					}
					pr_debug("cluster%u idx=%u sorted=%u inefficiency=%d freq=%u",
					       cluster_id, idx,
					       policy->freq_table_sorted,
					       table[idx].flags & CPUFREQ_INEFFICIENT_FREQ ? 1 : 0,
					       table[idx].frequency);
				}
			}
			if (idx + 1 > IPS_CPU_FREQ_MAX_NUM) {
				pr_err("fatal error: cpu freq num max than %d",
				       IPS_CPU_FREQ_MAX_NUM);
				ips_on = false;
			}
			if (cluster_id == 0) {
				IPS_CLUSTER0_FREQ_MAX_IDX = idx - 1;
				IPS_C0_CPUFREQ_NUM = idx;
				ips_cpufreq_num[cluster_id] = IPS_C0_CPUFREQ_NUM;
				ips_cluster_max_freq[cluster_id] = ips_c0_cpufreq_table[IPS_CLUSTER0_FREQ_MAX_IDX];
				ips_cluster_freq_max_idx[cluster_id] = IPS_CLUSTER0_FREQ_MAX_IDX;
				pr_info("IPS_C0_CPUFREQ_NUM=%u, IPS_CLUSTER0_FREQ_MAX_IDX=%u",
				       IPS_C0_CPUFREQ_NUM,
				       IPS_CLUSTER0_FREQ_MAX_IDX);
			} else if (cluster_id == 1) {
				IPS_CLUSTER1_FREQ_MAX_IDX = idx - 1;
				IPS_C1_CPUFREQ_NUM = idx;
				ips_cpufreq_num[cluster_id] = IPS_C1_CPUFREQ_NUM;
				ips_cluster_max_freq[cluster_id] = ips_c1_cpufreq_table[IPS_CLUSTER1_FREQ_MAX_IDX];
				ips_cluster_freq_max_idx[cluster_id] = IPS_CLUSTER1_FREQ_MAX_IDX;
				pr_info("IPS_C1_CPUFREQ_NUM=%u, IPS_CLUSTER1_FREQ_MAX_IDX=%u",
				       IPS_C1_CPUFREQ_NUM,
				       IPS_CLUSTER1_FREQ_MAX_IDX);
			} else if (cluster_id == 2) {
				IPS_CLUSTER2_FREQ_MAX_IDX = idx - 1;
				IPS_C2_CPUFREQ_NUM = idx;
				ips_cpufreq_num[cluster_id] = IPS_C2_CPUFREQ_NUM;
				ips_cluster_max_freq[cluster_id] = ips_c2_cpufreq_table[IPS_CLUSTER2_FREQ_MAX_IDX];
				ips_cluster_freq_max_idx[cluster_id] = IPS_CLUSTER2_FREQ_MAX_IDX;
				pr_info("IPS_C2_CPUFREQ_NUM=%u, IPS_CLUSTER2_FREQ_MAX_IDX=%u",
				       IPS_C2_CPUFREQ_NUM,
				       IPS_CLUSTER2_FREQ_MAX_IDX);
			}
			/* store per cluster min & max qos */
			ips_qos_stats_cluster[cluster_id][0].qos = (void *)&policy->constraints;
			ips_qos_stats_cluster[cluster_id][0].req = policy->min_freq_req;
			ips_qos_stats_cluster[cluster_id][0].cid = cluster_id;
			ips_qos_stats_cluster[cluster_id][1].qos = (void *)&policy->constraints;
			ips_qos_stats_cluster[cluster_id][1].req = policy->max_freq_req;
			ips_qos_stats_cluster[cluster_id][1].cid = cluster_id;
		}
		cpufreq_cpu_put(policy);
	}
}

static int cpufreq_table_get_freq(unsigned int cid, int idx)
{
	if (cid == 0) {
		return ips_c0_cpufreq_table[idx];
	} else if (cid == 1) {
		return ips_c1_cpufreq_table[idx];
	} else if (cid == 2) {
		return ips_c2_cpufreq_table[idx];
	}

	return 0;
}

/**
 * @brief Set the target freq
 *
 * target_freq_index: TODO: RANGE MIN MAX
 *
 * @param pl_cpu 0,3,7 silver/gold/prime
 * @param target_freq_index
 */
void ips_set_target_freq(int cid, int pl_cpu, int cpu, int target_freq_index,
				long mem_stall, long inst_stall_CPI,
				u64 my_demand_ips, unsigned long util_rate, int bcm_value)
{
	int max_target_freq_index = 0, min_target_freq_index = 0, vote_freq_index = 0;
	int mem_idx = 0, inst_idx = 0, upper = 0, lower = 0, min_freq = 0,
	    max_freq = 0, vote_freq = 0;
	unsigned long flags;
	char tf_index[DCVS_TRACE_LEN];
	char demand_ips_tag[DCVS_TRACE_LEN];
	char tmpbuf[DCVS_TRACE_LEN];

	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_cap", cid);

	if (target_freq_index >= 0) {
		if (cid == 0) {
			mem_idx = mem_stall / IPS_CLUSTER0_MEM_STALL_STEP;
			mem_idx = MAX(IPS_CLUSTER0_MEM_STALL_MIN_IDX, mem_idx);
			mem_idx = MIN(IPS_CLUSTER0_MEM_STALL_MAX_IDX, mem_idx);
			inst_idx = (inst_stall_CPI - IPS_CLUSTER0_MIN_BASE_CPI) / IPS_CLUSTER0_BASE_CPI_STEP;
			inst_idx = MAX(IPS_CLUSTER0_BASE_CPI_MIN_IDX, inst_idx);
			inst_idx = MIN(IPS_CLUSTER0_BASE_CPI_MAX_IDX, inst_idx);

			/* perf first, rel */
			upper = c0_ips_rel_upper_table[inst_idx][mem_idx];
			lower = c0_ips_rel_lower_table[inst_idx][mem_idx];

			if (lower < upper)
				ips_record_error(IPS_ERR_CORE_BOUNDARY_C0);

			if (target_freq_index < upper) {
				/* low freq: high efficiency, freq interval is large@8750 */
				max_target_freq_index = MIN(target_freq_index + ips_vote_cpufreq_range[0], IPS_CLUSTER0_FREQ_MAX_IDX);
				min_target_freq_index = target_freq_index;
			} else if (target_freq_index < lower) {
				/* mid freq: low efficiency */
				max_target_freq_index = MIN(target_freq_index + ips_vote_cpufreq_range[0] - 1, IPS_CLUSTER0_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - 1, 0);
			} else {
				/* high freq: lowest efficiency */
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER0_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[0], 0);
			}

			if (bcm_value == 1) {
				/* bcm process */
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER0_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[0], 0);
			}

			/* low effecient limit, low demand ips with high freq vote */
			if (ips_le_limit && inst_idx >= ips_le_bcpi_idx
			    && mem_idx >= ips_le_mstall_idx
			    && util_rate >= ips_le_util
			    && my_demand_ips <= ips_le_demand_threshold
			    && target_freq_index > ips_le_freq_idx[cid]) {
				target_freq_index = ips_le_freq_idx[cid];
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER0_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[0], 0);
				DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c0_le_limit", target_freq_index);
				DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, target_freq_index);
			}

			if (inst_stall_CPI == 0) {
				min_target_freq_index = 0;
				max_target_freq_index = IPS_CLUSTER0_FREQ_MAX_IDX;
			}

			if (min_target_freq_index < 0) {
				min_target_freq_index = 0;
				max_target_freq_index = MIN(min_target_freq_index + ips_vote_cpufreq_range[0], IPS_CLUSTER0_FREQ_MAX_IDX);
			}
			/* cluster0 boudary check, make sure gap is ips_vote_cpufreq_range */
			if (max_target_freq_index - min_target_freq_index < ips_vote_cpufreq_range[0]) {
				max_target_freq_index = min_target_freq_index  + ips_vote_cpufreq_range[0];
				if (max_target_freq_index > IPS_CLUSTER0_FREQ_MAX_IDX) {
					max_target_freq_index = IPS_CLUSTER0_FREQ_MAX_IDX;
					min_target_freq_index = IPS_CLUSTER0_FREQ_MAX_IDX - ips_vote_cpufreq_range[0];
				}
			}
			/* stats */
			c0_bcpi_mstall_table_vote[inst_idx][mem_idx] += 1;
			trace_ips_memlat_ips_slope(cpu,
					       &c0_ips_stall_table[inst_idx][mem_idx][0]);
		} else if (cid == 1) {
			mem_idx = mem_stall / IPS_CLUSTER1_MEM_STALL_STEP;
			mem_idx = MAX(IPS_CLUSTER1_MEM_STALL_MIN_IDX, mem_idx);
			mem_idx = MIN(IPS_CLUSTER1_MEM_STALL_MAX_IDX, mem_idx);
			inst_idx = (inst_stall_CPI - IPS_CLUSTER1_MIN_BASE_CPI) / IPS_CLUSTER1_BASE_CPI_STEP;
			inst_idx = MAX(IPS_CLUSTER1_BASE_CPI_MIN_IDX, inst_idx);
			inst_idx = MIN(IPS_CLUSTER1_BASE_CPI_MAX_IDX, inst_idx);

			/* perf first, rel */
			upper = c1_ips_rel_upper_table[inst_idx][mem_idx];
			lower = c1_ips_rel_lower_table[inst_idx][mem_idx];

			if (lower < upper)
				ips_record_error(IPS_ERR_CORE_BOUNDARY_C1);

			if (target_freq_index < upper) {
				/* low freq: high efficiency, freq interval is large@8750 */
				max_target_freq_index = MIN(target_freq_index + ips_vote_cpufreq_range[1], IPS_CLUSTER1_FREQ_MAX_IDX);
				min_target_freq_index = target_freq_index;
			} else if (target_freq_index < lower) {
				/* mid freq: low efficiency */
				max_target_freq_index = MIN(target_freq_index + ips_vote_cpufreq_range[1] - 1, IPS_CLUSTER1_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - 1, 0);
			} else {
				/* high freq: lowest efficiency */
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER1_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[1], 0);
			}

			if (bcm_value == 1) {
				/* bcm process */
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER1_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[1], 0);
			}

			if (inst_stall_CPI == 0) {
				/* tf_idx -1 */
				min_target_freq_index = 0;
				max_target_freq_index = IPS_CLUSTER1_FREQ_MAX_IDX;
			}

			/* low effecient limit, low demand ips with high freq vote */
			if (ips_le_limit && inst_idx >= ips_le_bcpi_idx
			    && mem_idx >= ips_le_mstall_idx
			    && util_rate >= ips_le_util
			    && my_demand_ips <= ips_le_demand_threshold
			    && target_freq_index > ips_le_freq_idx[cid]) {
				target_freq_index = ips_le_freq_idx[cid];
				max_target_freq_index = MIN(target_freq_index, IPS_CLUSTER1_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - ips_vote_cpufreq_range[1], 0);
				DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c1_le_limit", target_freq_index);
				DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, target_freq_index);
			}

			if (min_target_freq_index < 0) {
				min_target_freq_index = 0;
				max_target_freq_index = MIN(min_target_freq_index + ips_vote_cpufreq_range[1], IPS_CLUSTER1_FREQ_MAX_IDX);
			}

			/* cluster1 boudary check, make sure gap is ips_vote_cpufreq_range */
			if (max_target_freq_index - min_target_freq_index < ips_vote_cpufreq_range[1]) {
				max_target_freq_index = min_target_freq_index  + ips_vote_cpufreq_range[1];
				if (max_target_freq_index > IPS_CLUSTER1_FREQ_MAX_IDX) {
					max_target_freq_index = IPS_CLUSTER1_FREQ_MAX_IDX;
					min_target_freq_index = IPS_CLUSTER1_FREQ_MAX_IDX - ips_vote_cpufreq_range[1];
				}
			}

			/* stats */
			c1_bcpi_mstall_table_vote[inst_idx][mem_idx] += 1;
			trace_ips_memlat_ips_slope(cpu,
					       &c1_ips_stall_table[inst_idx][mem_idx][0]);
		} else if (cid == 2) {
			mem_idx = mem_stall / IPS_CLUSTER2_MEM_STALL_STEP;
			mem_idx = MAX(IPS_CLUSTER2_MEM_STALL_MIN_IDX, mem_idx);
			mem_idx = MIN(IPS_CLUSTER2_MEM_STALL_MAX_IDX, mem_idx);
			inst_idx = (inst_stall_CPI - IPS_CLUSTER2_MIN_BASE_CPI)	/ IPS_CLUSTER2_BASE_CPI_STEP;
			inst_idx = MAX(IPS_CLUSTER2_BASE_CPI_MIN_IDX, inst_idx);
			inst_idx = MIN(IPS_CLUSTER2_BASE_CPI_MAX_IDX, inst_idx);

			/* perf first, rel */
			upper = c2_ips_rel_upper_table[inst_idx][mem_idx];
			lower = c2_ips_rel_lower_table[inst_idx][mem_idx];

			if (lower < upper)
				ips_record_error(IPS_ERR_CORE_BOUNDARY_C2);

			if (target_freq_index < upper) {
				/* low freq: high efficiency, freq interval is large@8750 */
				max_target_freq_index = MIN(target_freq_index +
							    ips_vote_cpufreq_range[cid],
							    IPS_CLUSTER2_FREQ_MAX_IDX);
				min_target_freq_index = target_freq_index;
			} else if (target_freq_index < lower) {
				/* mid freq: low efficiency */
				max_target_freq_index = MIN(target_freq_index +
							    ips_vote_cpufreq_range[cid] - 1,
							    IPS_CLUSTER2_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index - 1, 0);
			} else {
				/* high freq: lowest efficiency */
				max_target_freq_index = MIN(target_freq_index,
							    IPS_CLUSTER2_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index -
							    ips_vote_cpufreq_range[cid], 0);
			}

			if (bcm_value == 1) {
				/* bcm process */
				max_target_freq_index = MIN(target_freq_index,
							    IPS_CLUSTER2_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index -
							    ips_vote_cpufreq_range[cid], 0);
			}

			if (inst_stall_CPI == 0) {
				/* tf_idx -1 */
				min_target_freq_index = 0;
				max_target_freq_index = IPS_CLUSTER2_FREQ_MAX_IDX;
			}

			/* low effecient limit, low demand ips with high freq vote */
			if (ips_le_limit && inst_idx >= ips_le_bcpi_idx
			    && mem_idx >= ips_le_mstall_idx
			    && util_rate >= ips_le_util
			    && my_demand_ips <= ips_le_demand_threshold
			    && target_freq_index > ips_le_freq_idx[cid]) {
				target_freq_index = ips_le_freq_idx[cid];
				max_target_freq_index = MIN(target_freq_index,
							    IPS_CLUSTER2_FREQ_MAX_IDX);
				min_target_freq_index = MAX(target_freq_index -
							    ips_vote_cpufreq_range[cid], 0);
				DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c2_le_limit", target_freq_index);
				DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, target_freq_index);
			}

			if (min_target_freq_index < 0) {
				min_target_freq_index = 0;
				max_target_freq_index =
					MIN(min_target_freq_index + ips_vote_cpufreq_range[cid],
					    IPS_CLUSTER2_FREQ_MAX_IDX);
			}

			/* cluster1 boudary check, make sure gap is ips_vote_cpufreq_range */
			if (max_target_freq_index - min_target_freq_index < ips_vote_cpufreq_range[cid]) {
				max_target_freq_index = min_target_freq_index  +
					ips_vote_cpufreq_range[cid];
				if (max_target_freq_index > IPS_CLUSTER2_FREQ_MAX_IDX) {
					max_target_freq_index = IPS_CLUSTER2_FREQ_MAX_IDX;
					min_target_freq_index =
						IPS_CLUSTER2_FREQ_MAX_IDX - ips_vote_cpufreq_range[2];
				}
			}

			/* stats */
			c2_bcpi_mstall_table_vote[inst_idx][mem_idx] += 1;
			trace_ips_memlat_ips_slope(cpu,
					       &c2_ips_stall_table[inst_idx][mem_idx][0]);
		}
		DCVS_ATRACE_C(ips_qos_freq_restore_str[cid], 0);
	} else {
		/* -1: release vote limit */
		target_freq_index = 0;
		min_target_freq_index = 0;
		max_target_freq_index = ips_cluster_freq_max_idx[cid];
		/* better show in perfetto */
		DCVS_ATRACE_C(ips_qos_freq_restore_str[cid], 1);
	}

	/* atrace H end */
	if (ips_vote_tag[cid][0] == 'c')
		DCVS_ATRACE_H(0, cluster_atrace_tag[cid], ips_cookie[cid]);
	ips_cookie[cid]++;

	if (ips_on) {
		/* READ_ONCE pairs with WRITE_ONCE in ipsgov_stop */
		if (READ_ONCE(ips_vote_cpufreq[cid])) {
			/* pick an online cpu in this cluster for governor update */
			int first_cpu = cpumask_first_and(&ips_cluster_cpumask[cid],
							cpu_online_mask);
			if (first_cpu >= nr_cpu_ids)
				first_cpu = -1;

			switch(ips_vote_cpufreq_type) {
			case 0:
				/* raw, calc from ips */
				vote_freq_index = target_freq_index;
				break;
			case 1:
				/* use old slope algo */
				vote_freq_index = min_target_freq_index >= target_freq_index ? min_target_freq_index : target_freq_index;
				break;
			case 2:
				/* TODO: new slope */
				vote_freq_index = target_freq_index;
				break;
			default:
				vote_freq_index = target_freq_index;
				break;
			}

			vote_freq = cpufreq_table_get_freq(cid, vote_freq_index);
			/* refill cpufreq vote info */
			spin_lock_irqsave(&ips_vote_lock[cid], flags);
			ips_vote_msg[cid].cpu = pl_cpu;

			ips_prev_vote_freq[cid][0] = vote_freq_index;
			ips_lst_vote_freq[cid][0] = ips_cur_vote_freq[cid][0];
			ips_cur_vote_freq[cid][0] = vote_freq;
			ips_vote_msg[cid].min = vote_freq;

			spin_unlock_irqrestore(&ips_vote_lock[cid], flags);

			/* ips governor vote freq (only if an online cpu exists) */
			if (first_cpu >= 0)
				ips_gov_update_cpufreq(first_cpu, vote_freq);
		}
	}
	min_freq = cpufreq_table_get_freq(cid, min_target_freq_index);
	max_freq = cpufreq_table_get_freq(cid, max_target_freq_index);
	scnprintf(ips_vote_tag[cid], DCVS_TRACE_LEN, "c-%d(%d,%d)_[%lu,%lu]_%d[%d,%d]",
		  cpu, inst_idx, mem_idx, inst_stall_CPI, mem_stall,
		  vote_freq, min_freq, max_freq);
	/* atrace G begin */
	DCVS_ATRACE_G(0, cluster_atrace_tag[cid], ips_vote_tag[cid], ips_cookie[cid]);
	/* anaylze slope algo */
	trace_ips_memlat_ips_tf(cpu, min_freq, max_freq,
			    target_freq_index,
			    min_target_freq_index,
			    max_target_freq_index,
			    upper, lower, inst_idx, mem_idx, mem_stall,
			    inst_stall_CPI);

	scnprintf(tf_index, sizeof(tf_index), "c%d_tf_index", cid);
	DCVS_ATRACE_C(tf_index, vote_freq);
	scnprintf(demand_ips_tag, sizeof(demand_ips_tag), "c%d_demand_ips", cid);
	DCVS_ATRACE_C(demand_ips_tag, my_demand_ips);
	ips_demand_ips_histogram_stats(cid, my_demand_ips);
	return;
}

static int cpufreq_table_find_idx(unsigned int target_freq, int cluster_id)
{
	int num, freq_idx;
	unsigned int *cpufreq_table;

	if (cluster_id == 0) {
		num = IPS_C0_CPUFREQ_NUM;
		cpufreq_table = ips_c0_cpufreq_table;
	} else if (cluster_id == 1) {
		num = IPS_C1_CPUFREQ_NUM;
		cpufreq_table = ips_c1_cpufreq_table;
	} else if (cluster_id == 2) {
		num = IPS_C2_CPUFREQ_NUM;
		cpufreq_table = ips_c2_cpufreq_table;
	} else {
		num = IPS_C0_CPUFREQ_NUM;
		cpufreq_table = ips_c0_cpufreq_table;
	}

	for (freq_idx = 0; freq_idx < num; freq_idx++) {
		if (target_freq <= cpufreq_table[freq_idx])
			return freq_idx;
	}
	return num - 1;
}

int get_cid_from_cpu(int cpu)
{
	int i = 0;
	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[i]))
			return i;
	}
	return 0;
}

/**
 * @brief get freq idx for giving mem_stall_SPI & inst_stall_CPI
 * todo: use delta_ips instead of rel or abs
 * @param raw_dm_ips demand_ips = instructions/vsync_interval
 * @param demand_ips my_demand_ips = smooth demand_ips
 * @param mem_stall  mem_stall_SPI (time)
 * @param inst_stall_CPI  aka base_CPI
 * @param cpu
 * @param real_ips real_ips = instructions/cpu_running_time
 * @return int
 */
int ips_find_target_freq_idx(u64 demand_ips, long mem_stall,
		long inst_stall_CPI, int cpu, u64 real_ips, u64 raw_freq,
		unsigned long util_rate, int *bcm_value, u32 ips_line[3])
{
	int mem_idx, inst_idx, freq_idx, tg_f_idx = 0;
	u64 demand_ips_discount, idx;

	if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[0])) {
		/* gold cluster: 0-5 */
		mem_idx = mem_stall / IPS_CLUSTER0_MEM_STALL_STEP;
		mem_idx = MAX(IPS_CLUSTER0_MEM_STALL_MIN_IDX, mem_idx);
		mem_idx = MIN(IPS_CLUSTER0_MEM_STALL_MAX_IDX, mem_idx);
		inst_idx = (inst_stall_CPI - IPS_CLUSTER0_MIN_BASE_CPI) / IPS_CLUSTER0_BASE_CPI_STEP;
		inst_idx = MAX(IPS_CLUSTER0_BASE_CPI_MIN_IDX, inst_idx);
		inst_idx = MIN(IPS_CLUSTER0_BASE_CPI_MAX_IDX, inst_idx);
		/* stats */
		c0_bcpi_mstall_table[inst_idx][mem_idx] += 1;

		if (ips_use_dyn_tl) {
			idx = (1000 - util_rate) / 100;
			if (idx > IPS_DYN_TARGET_LOAD_LEVEL - 1)
				idx = IPS_DYN_TARGET_LOAD_LEVEL - 1;
			demand_ips_discount = demand_ips * 100 / ips_dyn_target_load[idx];
		} else
			demand_ips_discount = demand_ips * 100 / ips_target_load[0];

		/* detect mem stall high, use min target load to prevent vote
		 * high cpufreq. mem stall idx 12.
		 * TODO: use mem stall to vote slc/ddr
		 */
		if (ips_use_mstall_limit && mem_idx >= ips_mstall_idx) {
			demand_ips_discount = demand_ips * 100 / ips_mstall_discount;
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c0_mstall_clamp", demand_ips_discount);
			DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
		}

		/* high efficiency task with high util, not boost too large */
		if (ips_he_limit && inst_idx <= 4 && mem_idx <= 4) {
			if (util_rate >= 950) {
				if (ips_target_load[0] < 80) {
					demand_ips_discount = demand_ips * 100 / 80;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c0_he_950_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			} else if (util_rate >= 900) {
				if (ips_target_load[0] < 75) {
					demand_ips_discount = demand_ips * 100 / 75;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c0_he_900_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			}
		}

		if (ips_bcm_limit_enable && inst_idx <= IPS_BCM_BASE_CPI_MAX_IDX
		    && mem_idx < IPS_BCM_MEM_STALL_MAX_IDX
		    && demand_ips >= ips_bcm_demand_ips_threshold[0]) {
			/* case 1: ips is high, mem stall & base cpi is low and
			 * cpu util is high.
			 */
			if (util_rate >= ips_bcm_util_rate_threshold1[0]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount1[0];
				*bcm_value = 1;
			} else if (util_rate >= ips_bcm_util_rate_threshold2[0]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount2[0];
				*bcm_value = 1;
			}
		} else
			*bcm_value = 0;

		for (freq_idx = 0; freq_idx < IPS_C0_CPUFREQ_NUM; freq_idx++) {
			if (demand_ips_discount <=
			    c0_ips_stall_table[inst_idx][mem_idx][freq_idx]) {
				ips_line[0] = inst_idx;
				ips_line[1] = mem_idx;
				ips_line[2] = c0_ips_stall_table[inst_idx][mem_idx][freq_idx];
				return freq_idx;
			}
		}

		if (inst_idx == 0 && mem_idx >= 18
		    && util_rate >= 950 && demand_ips <= 1000) {
			/* case 2: ips is low, mem stall is high, base cpi is zero. */
			return cpufreq_table_find_idx(raw_freq, 0);
		}
		/* TODO: not BOOST to the larget freq.
		 * ignore too much stall
		 */
		ips_line[0] = inst_idx;
		ips_line[1] = mem_idx;
		ips_line[2] = demand_ips_discount;
		if (mem_idx >= ips_mstall_idx) {
			tg_f_idx = cpufreq_table_find_idx(raw_freq, 0);
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c0_mstall_freq", tg_f_idx);
			DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, tg_f_idx);
		} else
			tg_f_idx = freq_idx - 1;
	} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[1])) {
		/* gold cluster: 0-5 */
		/* prime cluster: 6-7 */
		mem_idx = mem_stall / IPS_CLUSTER1_MEM_STALL_STEP;
		mem_idx = MAX(IPS_CLUSTER1_MEM_STALL_MIN_IDX, mem_idx);
		mem_idx = MIN(IPS_CLUSTER1_MEM_STALL_MAX_IDX, mem_idx);
		inst_idx = (inst_stall_CPI - IPS_CLUSTER1_MIN_BASE_CPI) / IPS_CLUSTER1_BASE_CPI_STEP;
		inst_idx = MAX(IPS_CLUSTER1_BASE_CPI_MIN_IDX, inst_idx);
		inst_idx = MIN(IPS_CLUSTER1_BASE_CPI_MAX_IDX, inst_idx);
		/* stats */
		c1_bcpi_mstall_table[inst_idx][mem_idx] += 1;

		if (ips_use_dyn_tl) {
			idx = (1000 - util_rate) / 100;
			if (idx > IPS_DYN_TARGET_LOAD_LEVEL - 1)
				idx = IPS_DYN_TARGET_LOAD_LEVEL - 1;
			demand_ips_discount = demand_ips * 100 / ips_dyn_target_load[idx];
		} else
			demand_ips_discount = demand_ips * 100 / ips_target_load[1];

		/* detect mem stall high, use min target load to prevent vote
		 * high cpufreq. mem stall idx 12.
		 * TODO: use mem stall to vote slc/ddr
		 */
		if (ips_use_mstall_limit && mem_idx >= ips_mstall_idx) {
			demand_ips_discount = demand_ips * 100 / ips_mstall_discount;
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c1_mstall_clamp", demand_ips_discount);
			DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
		}

		/* high efficiency task with high util, not boost too large */
		if (ips_he_limit && inst_idx <= 4 && mem_idx <= 4) {
			if (util_rate >= 950) {
				if (ips_target_load[1] < 80) {
					demand_ips_discount = demand_ips * 100 / 80;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c1_he_950_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			} else if (util_rate >= 900) {
				if (ips_target_load[1] < 75) {
					demand_ips_discount = demand_ips * 100 / 75;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c1_he_900_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			}
		}

		if (ips_bcm_limit_enable && inst_idx <= IPS_BCM_BASE_CPI_MAX_IDX
		    && mem_idx < IPS_BCM_MEM_STALL_MAX_IDX
		    && demand_ips >= ips_bcm_demand_ips_threshold[1]) {
			/* case 1: ips is high, mem stall & base cpi is low
			 * and cpu util is high.
			 */
			if (util_rate >= ips_bcm_util_rate_threshold1[1]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount1[1];
				*bcm_value = 1;
			} else if (util_rate >= ips_bcm_util_rate_threshold2[1]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount2[1];
				*bcm_value = 1;
			}
		} else
			*bcm_value = 0;

		for (freq_idx = 0; freq_idx < IPS_C1_CPUFREQ_NUM; freq_idx++) {
			if (demand_ips_discount <=
			    c1_ips_stall_table[inst_idx][mem_idx][freq_idx]) {
				ips_line[0] = inst_idx;
				ips_line[1] = mem_idx;
				ips_line[2] = c1_ips_stall_table[inst_idx][mem_idx][freq_idx];
				return freq_idx;
			}
		}

		if (inst_idx == 0 && mem_idx >= 18
		    && util_rate >= 950 && demand_ips <= 1000) {
			/* case 1: ips is low, mem stall is high, base cpi is zero. */
			return cpufreq_table_find_idx(raw_freq, 1);
		}
		/* TODO: not BOOST to the larget freq.
		 * ignore too much stall
		 */
		ips_line[0] = inst_idx;
		ips_line[1] = mem_idx;
		ips_line[2] = demand_ips_discount;
		if (mem_idx >= ips_mstall_idx) {
			tg_f_idx = cpufreq_table_find_idx(raw_freq, 1);
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c1_mstall_freq", tg_f_idx);
			DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, tg_f_idx);
		} else
			tg_f_idx = freq_idx - 1;
	} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[2])) {
		/* cluster3 */
		mem_idx = mem_stall / IPS_CLUSTER2_MEM_STALL_STEP;
		mem_idx = MAX(IPS_CLUSTER2_MEM_STALL_MIN_IDX, mem_idx);
		mem_idx = MIN(IPS_CLUSTER2_MEM_STALL_MAX_IDX, mem_idx);
		inst_idx = (inst_stall_CPI - IPS_CLUSTER2_MIN_BASE_CPI) / IPS_CLUSTER2_BASE_CPI_STEP;
		inst_idx = MAX(IPS_CLUSTER2_BASE_CPI_MIN_IDX, inst_idx);
		inst_idx = MIN(IPS_CLUSTER2_BASE_CPI_MAX_IDX, inst_idx);
		/* stats */
		c2_bcpi_mstall_table[inst_idx][mem_idx] += 1;

		if (ips_use_dyn_tl) {
			idx = (1000 - util_rate) / 100;
			if (idx > IPS_DYN_TARGET_LOAD_LEVEL - 1)
				idx = IPS_DYN_TARGET_LOAD_LEVEL - 1;
			demand_ips_discount = demand_ips * 100 / ips_dyn_target_load[idx];
		} else
			demand_ips_discount = demand_ips * 100 / ips_target_load[2];

		/* detect mem stall high, use min target load to prevent vote
		 * high cpufreq. mem stall idx 12.
		 * TODO: use mem stall to vote slc/ddr
		 */
		if (ips_use_mstall_limit && mem_idx >= ips_mstall_idx) {
			demand_ips_discount = demand_ips * 100 / ips_mstall_discount;
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c2_mstall_clamp", demand_ips_discount);
			DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
		}

		/* high efficiency task with high util, not boost too large */
		if (ips_he_limit && inst_idx <= 4 && mem_idx <= 4) {
			if (util_rate >= 950) {
				if (ips_target_load[2] < 80) {
					demand_ips_discount = demand_ips * 100 / 80;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c2_he_950_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			} else if (util_rate >= 900) {
				if (ips_target_load[2] < 75) {
					demand_ips_discount = demand_ips * 100 / 75;
					DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c2_he_900_clamp", demand_ips_discount);
					DCVS_ATRACE_H(0, LIMIT_TRACE_NAME, demand_ips_discount);
				}
			}
		}

		if (ips_bcm_limit_enable && inst_idx <= IPS_BCM_BASE_CPI_MAX_IDX
		    && mem_idx < IPS_BCM_MEM_STALL_MAX_IDX
		    && demand_ips >= ips_bcm_demand_ips_threshold[2]) {
			/* case 1: ips is high, mem stall & base cpi is low
			 * and cpu util is high.
			 */
			if (util_rate >= ips_bcm_util_rate_threshold1[2]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount1[2];
				*bcm_value = 1;
			} else if (util_rate >= ips_bcm_util_rate_threshold2[2]) {
				demand_ips_discount =
					demand_ips * 100 / ips_bcm_demand_discount2[2];
				*bcm_value = 1;
			}
		} else
			*bcm_value = 0;

		for (freq_idx = 0; freq_idx < IPS_C2_CPUFREQ_NUM; freq_idx++) {
			if (demand_ips_discount <=
			    c2_ips_stall_table[inst_idx][mem_idx][freq_idx]) {
				ips_line[0] = inst_idx;
				ips_line[1] = mem_idx;
				ips_line[2] = c2_ips_stall_table[inst_idx][mem_idx][freq_idx];
				return freq_idx;
			}
		}

		if (inst_idx == 0 && mem_idx >= 18
		    && util_rate >= 950 && demand_ips <= 1000) {
			/* case 1: ips is low, mem stall is high, base cpi is zero. */
			return cpufreq_table_find_idx(raw_freq, 2);
		}
		/* TODO: not BOOST to the larget freq.
		 * ignore too much stall
		 */
		ips_line[0] = inst_idx;
		ips_line[1] = mem_idx;
		ips_line[2] = demand_ips_discount;
		if (mem_idx >= ips_mstall_idx) {
			tg_f_idx = cpufreq_table_find_idx(raw_freq, 2);
			DCVS_ATRACE_G(0, LIMIT_TRACE_NAME, "c2_mstall_freq", tg_f_idx);
		} else
			tg_f_idx = freq_idx - 1;
	}
		/* gold cluster: 0-5 */
	return tg_f_idx;
}

void ips_algo_init(void)
{
	int err;
	int cluster_id;
	u64 start_ns, end_ns;

	ips_cpu_info_init();
	start_ns = ktime_get_ns();
	ips_table_init();
	end_ns = ktime_get_ns();
	pr_info("ips_table_init cost time: %llu ns, %llu us\n",
		end_ns - start_ns, (end_ns - start_ns) / 1000);

	err = freq_qos_request_init();
	if (err) {
		pr_err("%s: Failed to init qos requests policy for err=%d\n",
		       __func__, err);
		ready_for_qos_update = false;
	} else {
		for (cluster_id = 0; cluster_id < IPS_MAX_CLUSTER_NUM; cluster_id++) {
			spin_lock_init(&ips_vote_lock[cluster_id]);
		}

		ready_for_qos_update = true;
		pr_info("ips ready for freq qos update.\n");
	}

	err = ipsgov_register();
	if (err)
		pr_warn("%s: ipsgov_register failed (%d), governor not available\n",
			__func__, err);
}

void ips_algo_exit(void)
{
	int i;

	pr_info("%s: cleaning up IPS algo resources\n", __func__);

	/* 1. unregister cpufreq governor */
	ipsgov_unregister();
	pr_info("ipsgov unregistered\n");

	/* 2. remove freq qos requests and unregister trace hooks */
	if (ready_for_qos_update) {
		freq_qos_request_exit();
		ready_for_qos_update = false;
	}

	/*
	 * 3. Clear ips_qos_stats_cluster to prevent dangling pointer access.
	 * These pointers reference cpufreq_policy internal structures which
	 * may be accessed by other modules' freq_qos callbacks.
	 */
	for (i = 0; i < ARCH_CLUSTER_NUM; i++) {
		ips_qos_stats_cluster[i][0].qos = NULL;
		ips_qos_stats_cluster[i][0].req = NULL;
		ips_qos_stats_cluster[i][1].qos = NULL;
		ips_qos_stats_cluster[i][1].req = NULL;
	}

	/* 4. Reset cluster count to 0 for next insmod */
	IPS_MAX_CLUSTER_NUM = 0;
	IPS_MAX_CPU_NUM = 0;

	pr_info("%s: IPS algo cleanup complete\n", __func__);
}

