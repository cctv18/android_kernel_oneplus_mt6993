// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */
#define pr_fmt(fmt) "ips: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/of_fdt.h>
#include <linux/of_device.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <trace/hooks/sched.h>
#include <linux/sched/clock.h>
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_QCOM)
#include <linux/cpu_phys_log_map.h>
#include <soc/qcom/pmu_lib.h>
#else
#include "oplus_pmu.h"
#include "mtk_drm_arr.h"
#include "dvfsrc-exp.h"
#endif
#include "trace_ips.h"
#include "ips_table_chip.h"
#include "ips_private.h"

struct cpu_ctrs {
	u64				ips_ctrs[NUM_IPS_EVS];
};

struct cpu_stats {
	struct cpu_ctrs			prev;
	struct cpu_ctrs			curr;
	struct cpu_ctrs			delta;
	struct qcom_pmu_data		raw_ctrs;
	bool				idle_sample;
	ktime_t				sample_ts;
	ktime_t				last_sample_ts;
	spinlock_t			ctrs_lock;
};

struct ips_dev_data {
	struct kobject			kobj;
	u32				ips_ev_ids[NUM_IPS_EVS];
	struct work_struct		work;
	struct workqueue_struct		*ips_wq;
	u32				sample_ms;
	struct hrtimer			timer;
	ktime_t				last_update_ts;
	ktime_t				last_jiffy_ts;
	bool				sampling_inited;
	bool				sampling_enabled;
	bool				inited;
};

static struct ips_dev_data		*ips_data;
static DEFINE_PER_CPU(struct cpu_stats *, sampling_stats);
static DEFINE_MUTEX(ips_lock);

struct oplus_ips_attr {
	struct attribute		attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
			const char *buf, size_t count);
};

#define to_ips_attr(_attr) \
	container_of(_attr, struct oplus_ips_attr, attr)

#define IPS_ATTR_RW(_name)						\
static struct oplus_ips_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)			\

#define IPS_ATTR_RO(_name)						\
static struct oplus_ips_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)					\


#define MIN_SAMPLE_MS	4U
#define MAX_SAMPLE_MS	1000U


static void ips_set_sample_ms(u32 sample_ms)
{
	mutex_lock(&ips_lock);
	ips_data->sample_ms = sample_ms;
	mutex_unlock(&ips_lock);
}

/* DDR stats collection control - disabled by default */
static bool ddr_stats_enabled;

#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_QCOM)
extern void memlat_set_sample_ms_ops(void (*sample_ms_ops)(u32 sample_ms));
#else
static unsigned int drm_fps;
static bool fps_cb_registered;

static void ips_fps_limit_cb(unsigned int fps_limit)
{
	u32 sample_ms;

	/* check if module is still active */
	if (unlikely(!ips_data || !ips_data->inited))
		return;

	drm_fps = fps_limit;
	switch (drm_fps) {
	case 60:
		sample_ms = 16;
		break;
	case 90:
		sample_ms = 12;
		break;
	case 120:
		sample_ms = 8;
		break;
	default:
		sample_ms = 8;
		break;
	}
	ips_set_sample_ms(sample_ms);
}

static void set_sample_ms_dummy(u32 sample_ms) {}
static void (*set_sample_ms_ops)(u32 sample_ms) = set_sample_ms_dummy;
static void memlat_set_sample_ms_ops(void (*sample_ms_ops)(u32 sample_ms))
{
	set_sample_ms_ops = sample_ms_ops;
	drm_register_fps_chg_callback(ips_fps_limit_cb);
	fps_cb_registered = true;
	pr_info("%s: fps callback registered\n", __func__);
}

static void memlat_clear_sample_ms_ops(void)
{
	if (fps_cb_registered) {
		drm_unregister_fps_chg_callback(ips_fps_limit_cb);
		fps_cb_registered = false;
		set_sample_ms_ops = set_sample_ms_dummy;
		pr_info("%s: fps callback unregistered\n", __func__);
	}
}
#endif

static ssize_t store_sample_ms(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;
	val = max(val, MIN_SAMPLE_MS);
	val = min(val, MAX_SAMPLE_MS);
	mutex_lock(&ips_lock);
	ips_data->sample_ms = val;
	mutex_unlock(&ips_lock);

	return count;
}

static ssize_t show_sample_ms(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int cnt = 0;
	mutex_lock(&ips_lock);
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "sample_ms=%u\n", ips_data->sample_ms);
	mutex_unlock(&ips_lock);

	return cnt;
}

static ssize_t store_ips_cm_vote(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val == 100) {
		memset(c0_bcpi_mstall_table_vote, 0,
		       IPS_CLUSTER0_BASE_CPI_NUM*IPS_CLUSTER0_MEM_STALL_NUM*sizeof(c0_bcpi_mstall_table[0][0]));
		memset(c1_bcpi_mstall_table_vote, 0,
		       IPS_CLUSTER1_BASE_CPI_NUM*IPS_CLUSTER1_MEM_STALL_NUM*sizeof(c1_bcpi_mstall_table[0][0]));
		memset(c2_bcpi_mstall_table_vote, 0,
		       IPS_CLUSTER2_BASE_CPI_NUM*IPS_CLUSTER2_MEM_STALL_NUM*sizeof(c2_bcpi_mstall_table[0][0]));
	}

	if (val >= 0 && val <= IPS_MAX_CLUSTER_NUM)
		ips_cluster_cm = val;

	return count;
}

static ssize_t show_ips_cm_vote(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int cpi_idx, mem_idx, cnt = 0, cpi_size, mem_size;
	u32 (*bcpi_mstall_table)[IPS_CLUSTER0_MEM_STALL_NUM];

	if (ips_cluster_cm == 0) {
		cpi_size = IPS_CLUSTER0_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER0_MEM_STALL_NUM;
		bcpi_mstall_table = c0_bcpi_mstall_table_vote;
	} else if (ips_cluster_cm == 1) {
		cpi_size = IPS_CLUSTER1_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER1_MEM_STALL_NUM;
		bcpi_mstall_table = c1_bcpi_mstall_table_vote;
	} else if (ips_cluster_cm == 2) {
		cpi_size = IPS_CLUSTER2_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER2_MEM_STALL_NUM;
		bcpi_mstall_table = c2_bcpi_mstall_table_vote;
	} else {
		//default use cluster0
		cpi_size = IPS_CLUSTER0_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER0_MEM_STALL_NUM;
		bcpi_mstall_table = c0_bcpi_mstall_table_vote;
	}

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "c%u: column-%u,row-%u\n",
			 ips_cluster_cm, cpi_size, mem_size);

	for (mem_idx = mem_size - 1; mem_idx >= 0; mem_idx--) {
		for (cpi_idx = 0; cpi_idx < cpi_size; cpi_idx++) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%u,",
					 bcpi_mstall_table[cpi_idx][mem_idx]);
		}
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}
	if (cnt >= PAGE_SIZE) {
		pr_warn_once("table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}

	return cnt;
}

static ssize_t store_ips_cm(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val == 100) {
		memset(c0_bcpi_mstall_table, 0,
		       IPS_CLUSTER0_BASE_CPI_NUM*IPS_CLUSTER0_MEM_STALL_NUM*sizeof(c0_bcpi_mstall_table[0][0]));
		memset(c1_bcpi_mstall_table, 0,
		       IPS_CLUSTER1_BASE_CPI_NUM*IPS_CLUSTER1_MEM_STALL_NUM*sizeof(c1_bcpi_mstall_table[0][0]));
		memset(c2_bcpi_mstall_table, 0,
		       IPS_CLUSTER2_BASE_CPI_NUM*IPS_CLUSTER2_MEM_STALL_NUM*sizeof(c2_bcpi_mstall_table[0][0]));
	}
	if (val >= 0 && val <= IPS_MAX_CLUSTER_NUM)
		ips_cluster_cm = val;

	return count;
}

static ssize_t show_ips_cm(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int cpi_idx, mem_idx, cnt = 0, cpi_size, mem_size;
	u32 (*bcpi_mstall_table)[IPS_CLUSTER0_MEM_STALL_NUM];

	if (ips_cluster_cm == 0) {
		cpi_size = IPS_CLUSTER0_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER0_MEM_STALL_NUM;
		bcpi_mstall_table = c0_bcpi_mstall_table;
	} else if (ips_cluster_cm == 1){
		cpi_size = IPS_CLUSTER1_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER1_MEM_STALL_NUM;
		bcpi_mstall_table = c1_bcpi_mstall_table;
	} else if (ips_cluster_cm == 2) {
		cpi_size = IPS_CLUSTER2_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER2_MEM_STALL_NUM;
		bcpi_mstall_table = c2_bcpi_mstall_table;
	} else {
		cpi_size = IPS_CLUSTER0_BASE_CPI_NUM;
		mem_size = IPS_CLUSTER0_MEM_STALL_NUM;
		bcpi_mstall_table = c0_bcpi_mstall_table;
	}

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "c%u: column-%u,row-%u\n",
			 ips_cluster_cm, cpi_size, mem_size);

	for (mem_idx = mem_size - 1; mem_idx >= 0; mem_idx--) {
		for (cpi_idx = 0; cpi_idx < cpi_size; cpi_idx++) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%u,",
					 bcpi_mstall_table[cpi_idx][mem_idx]);
		}
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}
	if (cnt >= PAGE_SIZE) {
		pr_warn_once("table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}

	return cnt;
}

static ssize_t show_ips_enable(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_on ? "true" : "false");
}

static ssize_t store_ips_ddr_stats(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ddr_stats_enabled = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_ddr_stats(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ddr_stats_enabled ? "true" : "false");
}

/* loading similar with benchmark */
static ssize_t store_ips_bcm_limit(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_bcm_limit_enable = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_bcm_limit(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_bcm_limit_enable ? "true" : "false");
}

static ssize_t show_ips_vote(struct kobject *kobj,
                        struct attribute *attr, char *buf)
{
	bool vote[ARCH_CLUSTER_NUM];
	unsigned long flags;
	ssize_t cnt = 0;
	u32 cid;

	spin_lock_irqsave(&ips_vote_cpufreq_lock, flags);
	vote[0] = ips_vote_cpufreq[0];
	vote[1] = ips_vote_cpufreq[1];
	vote[2] = ips_vote_cpufreq[2];
	spin_unlock_irqrestore(&ips_vote_cpufreq_lock, flags);

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%u %u %u\n",
			 vote[0], vote[1], vote[2]);	/* Governor runtime statistics (per-cluster) */

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			"\n[Governor Runtime]\n");
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			"cluster  start  stop  time(sec)  active\n");

	for (cid = 0; cid < IPS_MAX_CLUSTER_NUM; cid++) {
		u64 gov_start_cnt = 0, gov_stop_cnt = 0, gov_total_ms = 0;
		bool gov_active = false;

		ips_gov_get_cluster_stats(cid, &gov_start_cnt, &gov_stop_cnt,
					&gov_total_ms, &gov_active);

		/* Always show all clusters for consistent user-space parsing */
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				"C%d       %5llu  %4llu  %5llu.%03llu  %s\n",
				cid, gov_start_cnt, gov_stop_cnt,
				gov_total_ms / 1000, gov_total_ms % 1000,
				gov_active ? "YES" : "NO");
	}
	return cnt;
}

static ssize_t store_ips_vote_type(struct kobject *kobj,
                        struct attribute *attr, const char *buf,
                        size_t count)
{
        int ret;
        unsigned int new_val;

        ret = kstrtouint(buf, 10, &new_val);
        if (ret < 0)
                return ret;

	/* range: [0, 2] */
	if (new_val > 2)
		new_val = 2;

        ips_vote_cpufreq_type = new_val;

        DCVS_ATRACE_C("ips_vote_cpufreq_type", ips_vote_cpufreq_type);

        return count;
}

static ssize_t show_ips_vote_type(struct kobject *kobj,
                        struct attribute *attr, char *buf)
{
        return scnprintf(buf, PAGE_SIZE, "%u:%s\n", ips_vote_cpufreq_type,
			 ips_vote_type_str[ips_vote_cpufreq_type]);
}

static ssize_t store_ips_vote_range(struct kobject *kobj,
                        struct attribute *attr, const char *buf,
                        size_t count)
{
	int i;
	int cluster[ARCH_CLUSTER_NUM] = {0};

	if (sscanf(buf, "%u %u %u", &cluster[0], &cluster[1], &cluster[2]) !=
	    IPS_MAX_CLUSTER_NUM)
	       return -EINVAL;

	for (i = 0 ; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (cluster[i] >= IPS_CPUFREQ_INDEX_RANGE_MIN && cluster[i] <=
		    IPS_CPUFREQ_INDEX_RANGE_MAX)
			ips_vote_cpufreq_range[i] = cluster[i];
	}

        return count;
}

static ssize_t show_ips_vote_range(struct kobject *kobj,
                        struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "c0=%d c1=%d c2=%d\n",
			 ips_vote_cpufreq_range[0],
			 ips_vote_cpufreq_range[1],
			 ips_vote_cpufreq_range[2]);
}

static ssize_t store_ips_dyn_tl(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_use_dyn_tl = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_dyn_tl(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_use_dyn_tl ? "dynamic" : "static");
}

static ssize_t store_ips_targetload(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
        int i;
	int cluster[ARCH_CLUSTER_NUM] = {0};

	if (sscanf(buf, "%u %u %u", &cluster[0], &cluster[1], &cluster[2]) !=
	    IPS_MAX_CLUSTER_NUM)
	       return -EINVAL;

	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (cluster[i] >= IPS_TARGET_LOAD_MIN && cluster[i] <=
		    IPS_TARGET_LOAD_MAX)
			ips_target_load[i] = cluster[i];
	}
	return count;
}

static ssize_t show_ips_targetload(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	// todo: adaptive show
	return scnprintf(buf, PAGE_SIZE, "c0=%u c1=%u c2=%u\n",
			 ips_target_load[0],
			 ips_target_load[1],
			 ips_target_load[2]);
}

static ssize_t store_ips_dyn_targetload(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
        int i;
	int level[IPS_DYN_TARGET_LOAD_LEVEL] = {0};

	if (sscanf(buf, "%u %u %u %u %u", &level[0], &level[1], &level[2],
		   &level[3], &level[4]) != IPS_DYN_TARGET_LOAD_LEVEL)
	       return -EINVAL;

	for (i = 0 ; i < IPS_DYN_TARGET_LOAD_LEVEL; i++) {
		if (level[i] >= IPS_TARGET_LOAD_MIN && level[i] <=
		    IPS_TARGET_LOAD_MAX)
			ips_dyn_target_load[i] = level[i];
	}
	return count;
}

static ssize_t show_ips_dyn_targetload(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u %u %u %u %u\n",
			 ips_dyn_target_load[0], ips_dyn_target_load[1],
			 ips_dyn_target_load[2], ips_dyn_target_load[3],
			 ips_dyn_target_load[4]);
}

/*  */
static ssize_t store_ips_use_mstall(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_use_mstall_limit = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_use_mstall(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_use_mstall_limit ? "true" : "false");
}

static ssize_t store_ips_mstall_conf(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int idx;
	u32 discount;

	if (sscanf(buf, "%d %u", &idx, &discount) != 2)
	       return -EINVAL;

	/* Bounds check: idx should be within mem_stall index range */
	if (idx < 0 || idx > IPS_CLUSTER0_MEM_STALL_MAX_IDX)
		return -EINVAL;

	/* Bounds check: discount should be reasonable (50-200) */
	if (discount < 50 || discount > 200)
		return -EINVAL;

	ips_mstall_idx = idx;
	ips_mstall_discount = discount;

	return count;
}

static ssize_t show_ips_mstall_conf(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "mstall_idx=%d, discount=%u\n",
			 ips_mstall_idx, ips_mstall_discount);
}

static ssize_t store_ips_use_le(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_le_limit = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_use_le(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_le_limit ? "true" : "false");
}

static ssize_t store_ips_le_conf(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int cpi_idx, mstall_idx, util, ips;
	int freq_idx[ARCH_CLUSTER_NUM];

	if (sscanf(buf, "%d %d %d %d %d %d %d",
		   &cpi_idx, &mstall_idx, &util, &ips,
		   &freq_idx[0], &freq_idx[1], &freq_idx[2]) != 7)
	       return -EINVAL;

	/* Bounds check for indices */
	if (cpi_idx < 0 || cpi_idx > IPS_CLUSTER0_BASE_CPI_MAX_IDX)
		return -EINVAL;
	if (mstall_idx < 0 || mstall_idx > IPS_CLUSTER0_MEM_STALL_MAX_IDX)
		return -EINVAL;
	if (util < 0 || util > 1000)
		return -EINVAL;
	if (ips < 0 || ips > 100000)
		return -EINVAL;
	if (freq_idx[0] < 0 || freq_idx[0] >= IPS_CPU_FREQ_MAX_NUM)
		return -EINVAL;
	if (freq_idx[1] < 0 || freq_idx[1] >= IPS_CPU_FREQ_MAX_NUM)
		return -EINVAL;
	if (freq_idx[2] < 0 || freq_idx[2] >= IPS_CPU_FREQ_MAX_NUM)
		return -EINVAL;

	ips_le_bcpi_idx = cpi_idx;
	ips_le_mstall_idx = mstall_idx;
	ips_le_util = util;
	ips_le_demand_threshold = ips;
	ips_le_freq_idx[0] = freq_idx[0];
	ips_le_freq_idx[1] = freq_idx[1];
	ips_le_freq_idx[2] = freq_idx[2];

	return count;
}

static ssize_t show_ips_le_conf(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d %d %d %d %d %d %d\n",
			 ips_le_bcpi_idx, ips_le_mstall_idx,
			 ips_le_util, ips_le_demand_threshold,
			 ips_le_freq_idx[0], ips_le_freq_idx[1],
			 ips_le_freq_idx[2]);
}

static ssize_t store_ips_use_he(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_he_limit = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_use_he(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_he_limit ? "true" : "false");
}

static ssize_t store_ips_bcm_tl(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
        int i;
	int cluster[ARCH_CLUSTER_NUM] = {0};
	int cl[ARCH_CLUSTER_NUM] = {0};

	if (sscanf(buf, "%u %u %u %u %u %u",
		   &cluster[0], &cluster[1], &cluster[2],
		   &cl[0], &cl[1], &cl[2]) != 6)
	       return -EINVAL;

	for (i = 0 ; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (cluster[i] >= IPS_TARGET_LOAD_MIN && cluster[i] <=
		    IPS_TARGET_LOAD_MAX)
			ips_bcm_demand_discount1[i] = cluster[i];
		if (cl[i] >= IPS_TARGET_LOAD_MIN && cl[i] <=
		    IPS_TARGET_LOAD_MAX)
			ips_bcm_demand_discount2[i] = cl[i];
	}
	return count;
}

static ssize_t show_ips_bcm_tl(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "tl1:c0=%u c1=%u c2=%u\n"
			 "tl2:c0=%u c1=%u c2=%u\n",
			 ips_bcm_demand_discount1[0],
			 ips_bcm_demand_discount1[1],
			 ips_bcm_demand_discount1[2],
			 ips_bcm_demand_discount2[0],
			 ips_bcm_demand_discount2[1],
			 ips_bcm_demand_discount2[2]);
}

static ssize_t store_ips_bcm_util(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
        int i;
	int cluster[ARCH_CLUSTER_NUM] = {0};
	int cl[ARCH_CLUSTER_NUM] = {0};

	if (sscanf(buf, "%u %u %u %u %u %u",
		   &cluster[0], &cluster[1], &cluster[2],
		   &cl[0], &cl[1], &cl[2]) != 6)
	       return -EINVAL;

	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (cluster[i] >= IPS_BCM_UTIL_RATE_MIN && cluster[i] <=
		    IPS_BCM_UTIL_RATE_MAX)
			ips_bcm_util_rate_threshold1[i] = cluster[i];
		if (cl[i] >= IPS_BCM_UTIL_RATE_MIN && cl[i] <=
		    IPS_BCM_UTIL_RATE_MAX)
			ips_bcm_util_rate_threshold2[i] = cl[i];
	}
	return count;
}

static ssize_t show_ips_bcm_util(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "util_rate1:c0=%u c1=%u c2=%u\n"
			 "util_rate2:c0=%u c1=%u c2=%u\n",
			 ips_bcm_util_rate_threshold1[0],
			 ips_bcm_util_rate_threshold1[1],
			 ips_bcm_util_rate_threshold1[2],
			 ips_bcm_util_rate_threshold2[0],
			 ips_bcm_util_rate_threshold2[1],
			 ips_bcm_util_rate_threshold2[2]);
}

/*
 * show_ips_abnormal_stats - Display IPS system status for Android statsd
 *
 * Output format (key=value pairs for statsd parsing):
 *   status=OK|ERR         - Current PMU health status (real-time)
 *   pmu_err_count=N       - Historical PMU hotplug error count
 *   pmu_err_ts_ms=N       - Timestamp of last error in milliseconds
 *   pmu_err_reason=...    - Details of last error (cpuN_evt0xHEX_retN or none)
 *   active_count=N        - Number of times IPS governor activated (cluster0)
 *   active_time_ms=N      - Total IPS governor active time in ms (cluster0)
 *   active=0|1            - Current governor active state (cluster0)
 */
static ssize_t show_ips_abnormal_stats(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	ssize_t cnt = 0;
	u64 pmu_hotplug_err = 0;
	u64 pmu_hotplug_jiffies = 0;
	int pmu_err_cpu = -1;
	u32 pmu_err_event = 0;
	int pmu_err_ret = 0;
	bool pmu_healthy;
	u64 gov_start_cnt = 0, gov_stop_cnt = 0, gov_total_ms = 0;
	bool gov_active = false;

	/* Get PMU runtime error stats */
	qcom_pmu_get_hotplug_errors(&pmu_hotplug_err, &pmu_hotplug_jiffies,
				    &pmu_err_cpu, &pmu_err_event, &pmu_err_ret);

	/*
	 * Status indicator: based on current real-time health, not historical errors.
	 * A cluster can recover after CPU hotplug succeeds, clearing the failed mask.
	 */
	pmu_healthy = qcom_pmu_is_healthy();
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "status=%s\n", pmu_healthy ? "OK" : "ERR");

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "pmu_err_count=%llu\n", pmu_hotplug_err);

	/* Display timestamp in milliseconds for statsd parsing */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "pmu_err_ts_ms=%llu\n",
			 (u64)pmu_hotplug_jiffies * MSEC_PER_SEC / HZ);

	/* Display last error reason for debugging */
	if (pmu_err_cpu >= 0) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "pmu_err_reason=cpu%d_evt0x%x_ret%d\n",
				 pmu_err_cpu, pmu_err_event, pmu_err_ret);
	} else if (pmu_hotplug_jiffies > 0) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "pmu_err_reason=fault_injected\n");
	} else {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "pmu_err_reason=none\n");
	}

	/*
	 * Governor runtime statistics from cluster0 as overall IPS status.
	 * (Per-cluster details available via ips_vote node)
	 */
	ips_gov_get_cluster_stats(0, &gov_start_cnt, &gov_stop_cnt,
				  &gov_total_ms, &gov_active);

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "active_count=%llu\n", gov_start_cnt);

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "active_time_ms=%llu\n", gov_total_ms);

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "active=%d\n", gov_active ? 1 : 0);

	return cnt;
}

#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
/*
 * PMU fault injection for testing hotplug error handling.
 * Write CPU ID to arm fault injection, -1 to disable.
 * After injection occurs, it is automatically cleared.
 */
static ssize_t store_ips_fault_inject(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	int val;

	ret = kstrtoint(buf, 10, &val);
	if (ret < 0)
		return ret;

	if (val >= -1 && val < IPS_MAX_CPU_NUM)
		qcom_pmu_set_fault_inject(val);
	else
		return -EINVAL;

	return count;
}

static ssize_t show_ips_fault_inject(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"%d\nusage: echo <cpu_id> to inject fault, -1 to disable\n",
		qcom_pmu_get_fault_inject());
}
#endif /* CONFIG_OPLUS_IPS_INJECT_TEST */

static ssize_t store_ips_atrace_enable(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_atrace = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_atrace_enable(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", ips_atrace);
}

static ssize_t store_ips_slope_type(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	ips_freq_slope = val;

	return count;
}

static ssize_t show_ips_slope_type(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", ips_freq_slope);
}

static ssize_t show_ips_model_num(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "c0=%u c1=%u c2=%u\n",
			 ips_bcpi_model_num[0], ips_bcpi_model_num[1],
			 ips_bcpi_model_num[2]);
}

static ssize_t show_ips_vote_info(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	u32 c0_min = ips_prev_vote_freq[0][0];
	u32 c0_max = ips_prev_vote_freq[0][1];
	u32 c1_min = ips_prev_vote_freq[1][0];
	u32 c1_max = ips_prev_vote_freq[1][1];
	u32 c2_min = ips_prev_vote_freq[2][0];
	u32 c2_max = ips_prev_vote_freq[2][1];

	/* Bounds check to prevent array out-of-bounds access */
	if (c0_min >= IPS_CPU_FREQ_MAX_NUM)
		c0_min = 0;
	if (c0_max >= IPS_CPU_FREQ_MAX_NUM)
		c0_max = IPS_CLUSTER0_FREQ_MAX_IDX;
	if (c1_min >= IPS_CPU_FREQ_MAX_NUM)
		c1_min = 0;
	if (c1_max >= IPS_CPU_FREQ_MAX_NUM)
		c1_max = IPS_CLUSTER1_FREQ_MAX_IDX;
	if (c2_min >= IPS_CPU_FREQ_MAX_NUM)
		c2_min = 0;
	if (c2_max >= IPS_CPU_FREQ_MAX_NUM)
		c2_max = IPS_CLUSTER2_FREQ_MAX_IDX;

	return scnprintf(buf, PAGE_SIZE, "c0:min %7u, max %7u, gap=%2u\n"
			 "c1:min %7u, max %7u, gap=%2u\n"
			 "c2:min %7u, max %7u, gap=%2u\n",
			 ips_c0_cpufreq_table[c0_min],
			 ips_c0_cpufreq_table[c0_max],
			 c0_max - c0_min,
			 ips_c1_cpufreq_table[c1_min],
			 ips_c1_cpufreq_table[c1_max],
			 c1_max - c1_min,
			 ips_c2_cpufreq_table[c2_min],
			 ips_c2_cpufreq_table[c2_max],
			 c2_max - c2_min);
}

static ssize_t show_ips_algo_time(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int i, cnt = 0;
	u64 algo_count = total_ips_algo_count;

	for (i = 0; i < IPS_ALGO_TIME_STATS_SIZE && cnt < PAGE_SIZE; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%llu ",
				 ips_algo_t_hist[i]);
	}

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	/* Prevent divide by zero */
	if (algo_count == 0)
		algo_count = 1;

	for (i = 0; i < IPS_ALGO_TIME_STATS_SIZE && cnt < PAGE_SIZE; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%llu ",
				100 * ips_algo_t_hist[i] / algo_count);
		ips_algo_t_hist[i] = 0;
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	total_ips_algo_count = 0;

	return cnt;
}

/* vote ips demand stats */
static ssize_t show_ips_demand_stat(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int i, cid, cnt = 0;

	for (cid = 0; cid < IPS_MAX_CLUSTER_NUM; cid++) {
		for (i = 0; i < IPS_DEMAND_IPS_HIST_SIZE && cnt < PAGE_SIZE; i++) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%llu ",
				 ips_demand_hist[cid][i]);
			ips_demand_hist[cid][i] = 0;
		}
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}

	return cnt;
}

/*
 * Reserve space for footer line showing pagination info.
 * Format: "[more: XXX/XXX, echo 0 to reset]\n" = ~35 bytes max
 */
#define QOS_CSTATS_FOOTER_RESERVE 48

/*
 * Minimum space needed for one entry. Breakdown:
 * - "%2d ts=%llu c%d-%s: "          ~40 bytes
 * - "value=%d pid=%d[%s] "          ~50 bytes (comm can be 16 chars)
 * - "count=%llu %c:%ps->%ps\n"      ~80 bytes (symbol names ~25 each)
 * Total: ~170 bytes typical, allow 200 for safety margin
 */
#define QOS_CSTATS_ENTRY_MIN_SIZE 200

/* Pagination state for ips_qos_cstats (shared between show and store) */
static int qos_cstats_pageflip;
/* Pagination state for ips_qos_lstats: [0]=policy min/max, [1]=other clients */
static int qos_lstats_pageflip[2];
/* default to show other freq qos clients */
static u32 ips_lru_type = 1;

static ssize_t store_ips_qos_cstats(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* echo 0 to reset pagination to beginning */
	if (val == 0)
		qos_cstats_pageflip = 0;

	return count;
}

static ssize_t show_ips_qos_cstats(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int cluster_id, ignore = 0, cnt = 0, idx = ips_lru_type;
	int start, displayed = 0;
	unsigned long flags;
	struct freq_qos_stats *client;
	int remaining;

	/* Header: cluster qos status */
	for (cluster_id = 0; cluster_id < IPS_MAX_CLUSTER_NUM; cluster_id++) {
		struct freq_constraints *qos = ips_qos_stats_cluster[cluster_id][0].qos;

		if (cnt >= PAGE_SIZE - QOS_CSTATS_FOOTER_RESERVE)
			break;

		if (qos) {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
					 "c%d: min=%d max=%d\n",
					 cluster_id,
					 qos->min_freq.target_value,
					 qos->max_freq.target_value);
		} else {
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
					 "c%d: min=N/A max=N/A\n", cluster_id);
		}
	}

	spin_lock_irqsave(&freq_qos_client_lock, flags);

	/* Reset pagination if past end */
	if (qos_cstats_pageflip >= qos_client_list_num)
		qos_cstats_pageflip = 0;

	start = qos_cstats_pageflip;

	/* Summary line */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "total=%u start=%d lru_num=%u\n",
			 qos_client_list_num, start, qos_stats_lru_num[idx]);

	/* Dynamic display: fill as many entries as fit in remaining space */
	list_for_each_entry(client, &qos_client_list, client_list) {
		if (ignore++ < start)
			continue;

		/* Check if enough space for this entry + footer */
		remaining = PAGE_SIZE - cnt - QOS_CSTATS_FOOTER_RESERVE;
		if (remaining < QOS_CSTATS_ENTRY_MIN_SIZE)
			break;

		cnt += scnprintf(buf + cnt, remaining,
				 "%2d ts=%llu c%d-%s: "
				 "value=%d pid=%d[%s] "
				 "count=%llu %c:%ps->%ps\n",
				 start + displayed,
				 client->update_ts,
				 client->cid,
				 client->type == FREQ_QOS_MIN ? "min" : "max",
				 client->value,
				 client->pid,
				 client->comm,
				 client->count,
				 client->src,
				 (void *)client->stacktrace[3],
				 (void *)client->stacktrace[2]);
		displayed++;
	}

	/* Update pagination for next read */
	qos_cstats_pageflip = start + displayed;

	spin_unlock_irqrestore(&freq_qos_client_lock, flags);

	/* Footer: show pagination status */
	if (qos_cstats_pageflip < qos_client_list_num) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "[more: %d/%u, echo 0 to reset]\n",
				 qos_cstats_pageflip, qos_client_list_num);
	} else {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "[end, echo 0 to reset]\n");
	}

	return cnt;
}

static ssize_t store_ips_qos_lstats(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	/* Set lru_type: 0=policy min/max, 1=other clients */
	ips_lru_type = val > 0 ? 1 : 0;

	return count;
}

static ssize_t show_ips_qos_lstats(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int start, pos, idx = ips_lru_type, cnt = 0;
	int remaining;
	unsigned long flags;
	struct freq_qos_stats *lru;

	spin_lock_irqsave(&freq_qos_client_lock, flags);

	/* Reset pagination if past end */
	if (qos_lstats_pageflip[idx] >= IPS_FREQ_QOS_LRU_SIZE)
		qos_lstats_pageflip[idx] = 0;

	start = qos_lstats_pageflip[idx];
	pos = qos_stats_lru_num[idx] % IPS_FREQ_QOS_LRU_SIZE;

	/* Header */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "lru_num=%u start=%d pos=%d type=%u\n",
			 qos_stats_lru_num[idx], start, pos, idx);

	/* Dynamic display: fill as many entries as fit */
	for (; start < IPS_FREQ_QOS_LRU_SIZE; start++) {
		lru = &ips_qos_stats_lru[idx][start];

		if (lru->req == NULL)
			continue;

		/* Check if enough space for this entry + footer */
		remaining = PAGE_SIZE - cnt - QOS_CSTATS_FOOTER_RESERVE;
		if (remaining < QOS_CSTATS_ENTRY_MIN_SIZE)
			break;

		cnt += scnprintf(buf + cnt, remaining,
				 "%2d ts=%llu c%d-%s: "
				 "value=%d pid=%d[%s] "
				 "count=%llu %c:%ps->%ps\n",
				 start,
				 lru->update_ts,
				 lru->cid,
				 lru->type == FREQ_QOS_MIN ? "min" : "max",
				 lru->value,
				 lru->pid,
				 lru->comm,
				 lru->count,
				 lru->src,
				 (void *)lru->stacktrace[3],
				 (void *)lru->stacktrace[2]);
	}

	/* Update pagination for next read */
	qos_lstats_pageflip[idx] = start;

	spin_unlock_irqrestore(&freq_qos_client_lock, flags);

	/* Footer: show pagination status */
	if (start < IPS_FREQ_QOS_LRU_SIZE) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "[more: %d/%d]\n", start, IPS_FREQ_QOS_LRU_SIZE);
	} else {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "[end]\n");
	}

	return cnt;
}

static ssize_t show_ips_trace_stats(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	ssize_t count;
	u64 vsyncs = vsync_count;
	u64 duration = ktime_get_ns() - ips_reset_stats_ts;

	/* Prevent divide by zero */
	if (vsyncs == 0)
		vsyncs = 1;

	count = scnprintf(buf, PAGE_SIZE, "ips=%llu ipc=%u ipc0=%u ipc1=%u "
			  "ipc2=%u ipc3=%u freq=%llu freq0=%llu freq1=%llu freq2=%llu freq3=%llu "
			  "instrs=%llu cycles=%llu vsync=%llu\n"
			  "cpu ipc: ipc0=%u ipc1=%u ipc2=%u ipc3=%u ipc4=%u ipc5=%u ipc6=%u ipc7=%u\n"
			  "cluster ips: ips0=%llu ips1=%llu ips2=%llu ips3=%llu\n"
			  "cluster0 cpi hist: %u %u %u %u %u %u %u %u %u %u\n"
			  "cluster1 cpi hist: %u %u %u %u %u %u %u %u %u %u\n"
			  "cluster2 cpi hist: %u %u %u %u %u %u %u %u %u %u\n"
			  "cpu_time: t=%llu cpu0=%llu cpu1=%llu cpu2=%llu cpu3=%llu cpu4=%llu cpu5=%llu cpu6=%llu cpu7=%llu\n",
			  total_ips / vsyncs,
			  total_ipc,
			  cluster_ipc[0],
			  cluster_ipc[1],
			  cluster_ipc[2],
			  cluster_ipc[3],
			  total_average_freq / vsyncs,
			  cluster_average_freq[0] / vsyncs,
			  cluster_average_freq[1] / vsyncs,
			  cluster_average_freq[2] / vsyncs,
			  cluster_average_freq[3] / vsyncs,
			  total_instrs_acc / vsyncs,
			  total_cycles_acc / vsyncs,
			  vsyncs,
			  per_cpu_ipc[0],
			  per_cpu_ipc[1],
			  per_cpu_ipc[2],
			  per_cpu_ipc[3],
			  per_cpu_ipc[4],
			  per_cpu_ipc[5],
			  per_cpu_ipc[6],
			  per_cpu_ipc[7],
			  cluster_ips[0] / vsyncs,
			  cluster_ips[1] / vsyncs,
			  cluster_ips[2] / vsyncs,
			  cluster_ips[3] / vsyncs,
			  c0_cpi_hist[0], c0_cpi_hist[1],
			  c0_cpi_hist[2], c0_cpi_hist[3],
			  c0_cpi_hist[4], c0_cpi_hist[5],
			  c0_cpi_hist[6], c0_cpi_hist[7],
			  c0_cpi_hist[8], c0_cpi_hist[9],
			  c1_cpi_hist[0], c1_cpi_hist[1],
			  c1_cpi_hist[2], c1_cpi_hist[3],
			  c1_cpi_hist[4], c1_cpi_hist[5],
			  c1_cpi_hist[6], c1_cpi_hist[7],
			  c1_cpi_hist[8], c1_cpi_hist[9],
			  c2_cpi_hist[0], c2_cpi_hist[1],
			  c2_cpi_hist[2], c2_cpi_hist[3],
			  c2_cpi_hist[4], c2_cpi_hist[5],
			  c2_cpi_hist[6], c2_cpi_hist[7],
			  c2_cpi_hist[8], c2_cpi_hist[9],
			  duration,
			  per_cpu_cycle_cnt[0] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[1] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[2] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[3] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[4] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[5] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[6] * IPS_TIME_NS_PER_CYC_CNT,
			  per_cpu_cycle_cnt[7] * IPS_TIME_NS_PER_CYC_CNT);
	vsync_count = 0;
	total_ips = 0;
	total_ipc = 0;
	total_average_freq = 0;
	total_instrs_acc = 0;
	total_cycles_acc = 0;
	memset(cluster_ipc, 0, ARCH_CLUSTER_NUM * sizeof(cluster_ipc[0]));
	memset(cluster_average_freq, 0, ARCH_CLUSTER_NUM * sizeof(cluster_average_freq[0]));
	memset(cluster_instrs_acc, 0,
	       ARCH_CLUSTER_NUM * sizeof(cluster_instrs_acc[0]));
	memset(cluster_cycles_acc, 0,
	       ARCH_CLUSTER_NUM * sizeof(cluster_cycles_acc[0]));
	memset(cluster_ips, 0,
	       ARCH_CLUSTER_NUM * sizeof(cluster_ips[0]));
	memset(per_cpu_ipc, 0, num_possible_cpus() * sizeof(per_cpu_ipc[0]));
	memset(per_cpu_instrs, 0, num_possible_cpus() * sizeof(per_cpu_instrs[0]));
	memset(per_cpu_cycles, 0, num_possible_cpus() * sizeof(per_cpu_cycles[0]));
	memset(per_cpu_cycle_cnt, 0, num_possible_cpus() * sizeof(per_cpu_cycle_cnt[0]));
	memset(c0_cpi_hist, 0, IPS_CPI_HISTOGRAM_SIZE * sizeof(c0_cpi_hist[0]));
	memset(c1_cpi_hist, 0, IPS_CPI_HISTOGRAM_SIZE * sizeof(c1_cpi_hist[0]));
	memset(c2_cpi_hist, 0, IPS_CPI_HISTOGRAM_SIZE * sizeof(c2_cpi_hist[0]));

	/* reset ts */
	ips_reset_stats_ts = ktime_get_ns();

	return count;
}

static ssize_t show_ips_cpu_info(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	int i;
	ssize_t cnt = 0;

	/* cpu topology info */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
			 "IPS_MAX_CLUSTER_NUM(%u) IPS_MAX_CPU_NUM(%u) "
			 "IPS_CLUSTER0_FREQ_MAX_IDX(%u) IPS_CLUSTER1_FREQ_MAX_IDX(%u) "
			 "IPS_CLUSTER2_FREQ_MAX_IDX(%u)\n",
			  IPS_MAX_CLUSTER_NUM, IPS_MAX_CPU_NUM,
			  IPS_CLUSTER0_FREQ_MAX_IDX, IPS_CLUSTER1_FREQ_MAX_IDX,
			  IPS_CLUSTER2_FREQ_MAX_IDX);

	/* cluster bitmap(topology) */
	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "cluster%u=%*pbl, ",
				 i, cpumask_pr_args(&ips_cluster_cpumask[i]));
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	/* cpu capacity */
	for (i = 0; i < IPS_MAX_CPU_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "cpu%u_capacity(%u), ",
				 i, ips_cpu_capacity[i]);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	/* cluster max freq */
	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "cluster%u max freq(%u), ",
				 i, ips_cluster_max_freq[i]);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\ncluster0 cpufreq(%u): ",
			 IPS_C0_CPUFREQ_NUM);

	/* cpufreq avaliable list */
	for (i = 0; i < IPS_C0_CPUFREQ_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "%u ",
				 ips_c0_cpufreq_table[i]);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\ncluster1 cpufreq(%u): ",
			 IPS_C1_CPUFREQ_NUM);
	for (i = 0; i < IPS_C1_CPUFREQ_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "%u ",
				 ips_c1_cpufreq_table[i]);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\ncluster2 cpufreq(%u): ",
			 IPS_C2_CPUFREQ_NUM);
	for (i = 0; i < IPS_C2_CPUFREQ_NUM; i++) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt,
				 "%u ",
				 ips_c2_cpufreq_table[i]);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	return cnt;
}

static ssize_t store_ips_use_smooth(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	is_smooth = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_use_smooth(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", is_smooth);
}

static ssize_t store_ips_use_chain(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0)
		return ret;

	is_chain = val > 0 ? true : false;

	return count;
}

static ssize_t show_ips_use_chain(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", is_chain);
}

static ssize_t show_ips_sample_on(struct kobject *kobj,
			struct attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 ips_data->sampling_enabled ? "true" : "false");
}

static ssize_t store_ips_sample_on(struct kobject *kobj,
			struct attribute *attr, const char *buf,
			size_t count)
{
	bool enable;

	if (!ips_data)
		return -ENODEV;

	/* Parse input: accept "1", "0", "true", "false" */
	if (sysfs_streq(buf, "1") || sysfs_streq(buf, "true"))
		enable = true;
	else if (sysfs_streq(buf, "0") || sysfs_streq(buf, "false"))
		enable = false;
	else
		return -EINVAL;

	mutex_lock(&ips_lock);

	if (enable == ips_data->sampling_enabled) {
		/* No change needed */
		mutex_unlock(&ips_lock);
		return count;
	}

	if (!enable) {
		int cid;
		bool gov_active = false;

		unsigned long flags;

		/*
		 * Check if any IPS Governor is running.
		 * If ips_vote_cpufreq[cid] is true, Governor is active and
		 * depends on sampling data. Disabling sampling would cause
		 * Governor to have no data for frequency voting.
		 *
		 * Use ips_vote_cpufreq_lock to atomically check all clusters
		 * and prevent race with governor start.
		 */
		spin_lock_irqsave(&ips_vote_cpufreq_lock, flags);
		for (cid = 0; cid < IPS_MAX_CLUSTER_NUM; cid++) {
			if (ips_vote_cpufreq[cid]) {
				gov_active = true;
				break;
			}
		}
		spin_unlock_irqrestore(&ips_vote_cpufreq_lock, flags);

		if (gov_active) {
			mutex_unlock(&ips_lock);
			pr_warn("ips_sample_on: cannot disable, governor active on cluster %d\n", cid);
			return -EBUSY;
		}
	}

	if (enable) {
		int cpu;
		struct cpu_stats *stats;
		unsigned long flags;
		ktime_t now = ktime_get();

		/*
		 * Reset prev = curr and timestamps to avoid stale data issues.
		 * When sampling was disabled, curr continued to accumulate while
		 * prev stayed frozen. Without reset, first delta would be huge
		 * (accumulated over the disabled period), causing:
		 * - Average load instead of instantaneous load
		 * - Potential integer overflow
		 * - Stale prev_cpu_demand_ips affecting smoothing
		 */
		local_irq_save(flags);
		for_each_possible_cpu(cpu) {
			stats = per_cpu(sampling_stats, cpu);
			if (!stats)
				continue;
			spin_lock(&stats->ctrs_lock);
			memcpy(&stats->prev, &stats->curr, sizeof(stats->curr));
			stats->last_sample_ts = now;
			stats->sample_ts = now;
			/*
			 * Set idle_sample = true to indicate sample_ts is not valid
			 * for delta calculation. This makes calculate_sampling_stats
			 * use global update_us instead of (sample_ts - last_sample_ts).
			 *
			 * After reset, sample_ts == last_sample_ts, so delta would be 0
			 * causing division by zero. By setting idle_sample = true:
			 * - If sched_tick samples before next calculate: idle_sample
			 *   becomes false and sample_ts is updated (correct path)
			 * - If no sampling before calculate: uses update_us (safe path)
			 *
			 * Note: idle_sample semantically means "sample_ts is stale",
			 * not necessarily "CPU is idle". Same flag is used when CPU
			 * enters idle (sample_ts not updated in idle callback).
			 */
			stats->idle_sample = true;
			spin_unlock(&stats->ctrs_lock);
			/* Reset smoothing state */
			prev_cpu_demand_ips[cpu] = 0;
		}
		local_irq_restore(flags);
		ips_data->last_update_ts = now;

		ips_data->sampling_enabled = true;
		pr_info("ips_sample_on: sampling enabled, stats reset\n");
	} else {
		/*
		 * The hrtimer will skip calculate_sampling_stats when disabled
		 */
		ips_data->sampling_enabled = false;
		pr_info("ips_sample_on: sampling disabled\n");
	}

	mutex_unlock(&ips_lock);
	return count;
}

/**
 * ips_is_sampling_enabled - Check if IPS sampling is enabled
 *
 * This is used by ips_governor to check if sampling is active
 * before starting the governor.
 *
 * Return: true if sampling is enabled, false otherwise
 */
bool ips_is_sampling_enabled(void)
{
	if (!ips_data)
		return false;
	return READ_ONCE(ips_data->sampling_enabled);
}

/* file node */
IPS_ATTR_RW(sample_ms);
IPS_ATTR_RO(ips_enable);
IPS_ATTR_RW(ips_ddr_stats);
IPS_ATTR_RW(ips_sample_on);
IPS_ATTR_RO(ips_vote);
IPS_ATTR_RW(ips_vote_type);
IPS_ATTR_RW(ips_vote_range);
IPS_ATTR_RW(ips_cm);
IPS_ATTR_RW(ips_cm_vote);
IPS_ATTR_RW(ips_atrace_enable);
IPS_ATTR_RW(ips_slope_type);
IPS_ATTR_RO(ips_model_num);
IPS_ATTR_RO(ips_trace_stats);
IPS_ATTR_RW(ips_qos_cstats);
IPS_ATTR_RW(ips_qos_lstats);
IPS_ATTR_RO(ips_vote_info);
IPS_ATTR_RO(ips_algo_time);
IPS_ATTR_RO(ips_demand_stat);
IPS_ATTR_RO(ips_cpu_info);
IPS_ATTR_RO(ips_abnormal_stats);
#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
IPS_ATTR_RW(ips_fault_inject);
#endif
IPS_ATTR_RW(ips_dyn_tl);
IPS_ATTR_RW(ips_targetload);
IPS_ATTR_RW(ips_dyn_targetload);
IPS_ATTR_RW(ips_use_he);
IPS_ATTR_RW(ips_use_le);
IPS_ATTR_RW(ips_le_conf);
IPS_ATTR_RW(ips_use_mstall);
IPS_ATTR_RW(ips_mstall_conf);
IPS_ATTR_RW(ips_bcm_tl);
IPS_ATTR_RW(ips_bcm_util);
IPS_ATTR_RW(ips_bcm_limit);
IPS_ATTR_RW(ips_use_smooth);
IPS_ATTR_RW(ips_use_chain);
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK_DDR)
IPS_ATTR_RO(ddr_available_frequencies);
IPS_ATTR_RO(ddr_time_in_state);
#endif

static struct attribute *ips_settings_attrs[] = {
	&sample_ms.attr,
	&ips_enable.attr,
	&ips_ddr_stats.attr,
	&ips_sample_on.attr,
	&ips_vote.attr,
	&ips_vote_type.attr,
	&ips_vote_range.attr,
	&ips_cm.attr,
	&ips_cm_vote.attr,
	&ips_atrace_enable.attr,
	&ips_slope_type.attr,
	&ips_model_num.attr,
	&ips_trace_stats.attr,
	&ips_qos_cstats.attr,
	&ips_qos_lstats.attr,
	&ips_vote_info.attr,
	&ips_algo_time.attr,
	&ips_demand_stat.attr,
	&ips_cpu_info.attr,
	&ips_use_smooth.attr,
	&ips_use_chain.attr,
	&ips_abnormal_stats.attr,
#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
	&ips_fault_inject.attr,
#endif
	&ips_dyn_tl.attr,
	&ips_targetload.attr,
	&ips_dyn_targetload.attr,
	&ips_use_he.attr,
	&ips_use_le.attr,
	&ips_le_conf.attr,
	&ips_use_mstall.attr,
	&ips_mstall_conf.attr,
	&ips_bcm_tl.attr,
	&ips_bcm_util.attr,
	&ips_bcm_limit.attr,
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK_DDR)
	&ddr_available_frequencies.attr,
	&ddr_time_in_state.attr,
#endif
	NULL,
};
ATTRIBUTE_GROUPS(ips_settings);

static ssize_t attr_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct oplus_ips_attr *ips_attr = to_ips_attr(attr);
	ssize_t ret = -EIO;

	if (ips_attr->show)
		ret = ips_attr->show(kobj, attr, buf);

	return ret;
}

static ssize_t attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
	struct oplus_ips_attr *ips_attr = to_ips_attr(attr);
	ssize_t ret = -EIO;

	if (ips_attr->store)
		ret = ips_attr->store(kobj, attr, buf, count);

	return ret;
}

static const struct sysfs_ops ips_sysfs_ops = {
	.show	= attr_show,
	.store	= attr_store,
};

static const struct kobj_type ips_settings_ktype = {
	.sysfs_ops	= &ips_sysfs_ops,
	.default_groups	= ips_settings_groups,
};

static void calculate_sampling_stats(void)
{
	int i, cpu, level = 0;
	unsigned long flags;
	struct cpu_stats *stats;
	struct cpu_ctrs *delta;
	ktime_t now = ktime_get();
	s64 delta_us, update_us;
	int bcm_value, cluster_bcm[ARCH_CLUSTER_NUM];
	unsigned long util_rate, cluster_target_util[ARCH_CLUSTER_NUM] = {1000,
		1000, 1000, 1000};
	int cluster_id, cluster_cpu[ARCH_CLUSTER_NUM];
	int my_target_freq_idx = -1, cluster_target_freq_idx[ARCH_CLUSTER_NUM] = {-1, -1, -1};
	u64 raw_freq, demand_ips, raw_demand_ips, my_demand_ips, real_ips,
	    cycle_cnt, cpu_capacity, cpu_max_freq, cluster_demand_ips[ARCH_CLUSTER_NUM];
	long mem_stall, inst_stall_CPI, pSPI, pred_pSPI, my_mem_stall,
	     my_inst_CPI, raw_mem_stall, raw_inst_CPI, cluster_target_mem_stall[ARCH_CLUSTER_NUM],
	     cluster_target_inst_CPI[ARCH_CLUSTER_NUM];
	long instrs, cycles;
	/* base_CPI fitting pmu */
	long stall_bkd_mem, l1i_tlb_cr, l1i_ca, l1d_wb_v, l2d_cw, mem_access,
	     stall_bkd, stall_ftd_membound, stall_ftd_cpubound, stall_bkd_cpubound;
	long stall_bkd_mem_pi, l1i_tlb_cr_pi, l1i_ca_pi, l1d_wb_v_pi, l2d_cw_pi,
	     mem_access_pi, stall_bkd_pi, stall_ftd_membound_pi, stall_ftd_cpubound_pi,
	     stall_bkd_cpubound_pi, stall_b_diff_stall_m_pi;
	u64 start_ns, end_ns, cost_ns = 0, cpu_run_time,  cpu_cycle_time;
	u64 instrs_acc = 0, cycles_acc = 0, delta_us_acc = 0;
	s64 delta_ns, update_ns;
	u64 cluster_instrs[ARCH_CLUSTER_NUM] = {0};
	u64 cluster_cycles[ARCH_CLUSTER_NUM] = {0};
	u64 cluster_delta_vsync_us[ARCH_CLUSTER_NUM] = {0};
	u64 pmus[32] = {0};
	u64 amus[4] = {0};
	u32 tf = 0, ips_line[3] = {0};
	char cpu_tag[DCVS_TRACE_LEN];

	/* safety check: ensure sampling_stats is allocated */
	stats = per_cpu(sampling_stats, 0);
	if (unlikely(!stats))
		return;

	update_us = ktime_us_delta(now, ips_data->last_update_ts);
	update_ns = now - ips_data->last_update_ts;
	ips_data->last_update_ts = now;

	local_irq_save(flags);
	for_each_possible_cpu(cpu) {
		stats = per_cpu(sampling_stats, cpu);
		if (level == 0)
			spin_lock(&stats->ctrs_lock);
		else
			spin_lock_nested(&stats->ctrs_lock, level);
		level++;
	}

	for_each_possible_cpu(cpu) {
		stats = per_cpu(sampling_stats, cpu);
		delta = &stats->delta;
		/* use update_us and now to synchronize idle cpus */
		if (stats->idle_sample) {
			delta_us = update_us;
			delta_ns = update_ns;
			stats->last_sample_ts = now;
		} else {
			delta_us = ktime_us_delta(stats->sample_ts,
							stats->last_sample_ts);
			delta_ns = stats->sample_ts - stats->last_sample_ts;
			stats->last_sample_ts = stats->sample_ts;
		}

		/* IPS algo start */
		start_ns = ktime_get_ns();
		for (i = 0; i < NUM_IPS_EVS; i++) {
			if (!ips_data->ips_ev_ids[i])
				continue;
			delta->ips_ctrs[i] = stats->curr.ips_ctrs[i] -
						stats->prev.ips_ctrs[i];
		}
		instrs = delta->ips_ctrs[IPS_INST_IDX];
		cycles = delta->ips_ctrs[IPS_CYC_IDX];
		cycle_cnt = delta->ips_ctrs[IPS_CYC_CNT_IDX];

		/* per clustr stats */

		cluster_id = get_cid_from_cpu(cpu);
		cluster_instrs[cluster_id] += instrs;
		cluster_cycles[cluster_id] += cycles;
		cluster_delta_vsync_us[cluster_id] += delta_us;
		/* Prevent divide-by-zero: only compute CPI if instrs > 0 */
		if (instrs > 0)
			cluster_cpi_histogram_stats(cluster_id, 100 * cycles / instrs);

		/* per cpu stats */
		per_cpu_instrs[cpu] += instrs;
		per_cpu_cycles[cpu] += cycles;
		per_cpu_cycle_cnt[cpu] += cycle_cnt;
		/* Prevent divide-by-zero */
		if (per_cpu_cycles[cpu] > 0)
			per_cpu_ipc[cpu] = per_cpu_instrs[cpu] * 1000 / per_cpu_cycles[cpu];
		/* aggregated all cores */
		instrs_acc += instrs;
		cycles_acc += cycles;
		delta_us_acc += delta_us;

		/* directly ignore while not running at all,
		 * ignore idle process instrs about 90000 less than 100000;
		 */
		if (instrs < 100000) {
			prev_cpu_demand_ips[cpu] = 0;
			goto loop_ignore;
		}
		cpu_cycle_time = IPS_TIME_NS_PER_CYC_CNT * cycle_cnt;
		cpu_run_time = cpu_cycle_time;
		if (cpu_run_time > delta_ns)
			cpu_run_time = delta_ns;

		/* amplify 1000 */
		util_rate = cpu_run_time / delta_us;
		/* raw_freq = cycles/time, Hz */
		raw_freq =  mult_frac(100000, cycles, cpu_run_time / 10000);

		/* print all pmus */
		amus[0] = cycles;
		amus[1] = instrs;
		amus[2] = cycle_cnt;
		amus[3] = delta->ips_ctrs[IPS_STALL_BKD_MEM_IDX];
		pmus[0] = delta->ips_ctrs[IPS_STALL_BKD_CPUBOUND_IDX];
		pmus[1] = delta->ips_ctrs[IPS_STALL_FTD_MEMBOUND_IDX];
		pmus[2] = delta->ips_ctrs[IPS_L2D_CW_IDX];
		pmus[3] = delta->ips_ctrs[IPS_STALL_BKD_IDX];
		pmus[6] = delta->ips_ctrs[IPS_L1I_CA_IDX];
		pmus[10] = delta->ips_ctrs[IPS_L1D_WB_V_IDX];
		pmus[14] = delta->ips_ctrs[IPS_L1I_TLB_CR_IDX];
		pmus[16] = delta->ips_ctrs[IPS_MEM_ACCESS_IDX];
		pmus[26] = delta->ips_ctrs[IPS_STALL_FTD_CPUBOUND_IDX];
		pmus[28] = raw_freq / 1000;

		trace_ips_memlat_ips_xmus(cpu, cpufreq_quick_get(cpu), cpu_run_time, pmus, amus);

		if (!ips_on)
			goto loop_ignore;

		/* SPI = running_time/instructions, picosecond */
		pSPI = cpu_run_time * 1000 / instrs;
		cpu_capacity = ips_cpu_capacity[cpu];

		if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[0])) {
			/* cluster0, cpu0-3 */
			cpu_max_freq = ips_cluster_max_freq[0];
			demand_ips = instrs * 10000 / delta_us * cpu_capacity / cpu_max_freq;
			real_ips = instrs * 10000 / (cpu_run_time / 1000) * cpu_capacity / cpu_max_freq;
			raw_demand_ips = demand_ips;

			l1i_tlb_cr = delta->ips_ctrs[IPS_L1I_TLB_CR_IDX];
			mem_access = delta->ips_ctrs[IPS_MEM_ACCESS_IDX];
			stall_bkd_cpubound = delta->ips_ctrs[IPS_STALL_BKD_CPUBOUND_IDX];
			/* pi, multply 100000 */
			l1i_tlb_cr_pi = l1i_tlb_cr * 100000 / instrs;
			mem_access_pi  = mem_access * 100000 / instrs;
			stall_bkd_cpubound_pi =	stall_bkd_cpubound * 100000 / instrs;

			inst_stall_CPI = (447120 * l1i_tlb_cr_pi
					 + 14485 * mem_access_pi
					 - 1155 * stall_bkd_cpubound_pi) / 1000;

			raw_inst_CPI = inst_stall_CPI;
			inst_stall_CPI = my_long_max(IPS_CLUSTER0_MIN_TEST_BASE_CPI, inst_stall_CPI);
			inst_stall_CPI = my_long_min(inst_stall_CPI, cycles * 1000000 / instrs);
		} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[1])) {
			/* cluster1, cpu4-6 */
			cpu_max_freq = ips_cluster_max_freq[1];
			demand_ips = instrs * 10000 / delta_us * cpu_capacity / cpu_max_freq;
			real_ips = instrs * 10000 / (cpu_run_time / 1000) * cpu_capacity / cpu_max_freq;
			raw_demand_ips = demand_ips;

			l1i_ca = delta->ips_ctrs[IPS_L1I_CA_IDX];
			l1d_wb_v = delta->ips_ctrs[IPS_L1D_WB_V_IDX];
			stall_ftd_cpubound = delta->ips_ctrs[IPS_STALL_FTD_CPUBOUND_IDX];
			/* pi, multiply 100000 */
			l1i_ca_pi = l1i_ca * 100000 / instrs;
			l1d_wb_v_pi = l1d_wb_v * 100000 / instrs;
			stall_ftd_cpubound_pi =	stall_ftd_cpubound * 100000 / instrs;

			inst_stall_CPI = (840208 * l1d_wb_v_pi
					 + 12232 * l1i_ca_pi
					 - 922 * stall_ftd_cpubound_pi) / 1000;
			raw_inst_CPI = inst_stall_CPI;
			inst_stall_CPI = my_long_max(IPS_CLUSTER1_MIN_TEST_BASE_CPI, inst_stall_CPI);
			inst_stall_CPI = my_long_min(inst_stall_CPI, cycles * 1000000 / instrs);
		} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[2])) {
			/* cluster2, cpu7 */
			cpu_max_freq = ips_cluster_max_freq[2];
			demand_ips = instrs * 10000 / delta_us * cpu_capacity / cpu_max_freq;
			real_ips = instrs * 10000 / (cpu_run_time / 1000) * cpu_capacity / cpu_max_freq;
			raw_demand_ips = demand_ips;

			l1i_ca = delta->ips_ctrs[IPS_L1I_CA_IDX];
			l2d_cw = delta->ips_ctrs[IPS_L2D_CW_IDX];
			stall_ftd_membound = delta->ips_ctrs[IPS_STALL_FTD_MEMBOUND_IDX];
			stall_bkd = delta->ips_ctrs[IPS_STALL_BKD_IDX];
			stall_bkd_mem = delta->ips_ctrs[IPS_STALL_BKD_MEM_IDX];
			/* pi, multiply 100000 */
			l1i_ca_pi = l1i_ca * 100000 / instrs;
			l2d_cw_pi = l2d_cw * 100000 / instrs;
			stall_ftd_membound_pi =	stall_ftd_membound * 100000 / instrs;
			stall_bkd_pi = stall_bkd * 100000 / instrs;
			stall_bkd_mem_pi = stall_bkd_mem * 100000 / instrs;
			stall_b_diff_stall_m_pi = stall_bkd_pi - stall_bkd_mem_pi;

			inst_stall_CPI = (518586 * l2d_cw_pi
					 + 11330 * l1i_ca_pi
					 + 2858 * stall_ftd_membound_pi
					 - 6521 * stall_b_diff_stall_m_pi) / 1000;
			raw_inst_CPI = inst_stall_CPI;
			inst_stall_CPI = my_long_max(IPS_CLUSTER2_MIN_TEST_BASE_CPI, inst_stall_CPI);
			inst_stall_CPI = my_long_min(inst_stall_CPI, cycles * 1000000 / instrs);
		}

		if (prev_cpu_demand_ips[cpu] > 0 && is_smooth) {
			/* smooth for performance demand, best para is 60% */
			my_demand_ips = (demand_ips * 60 + prev_cpu_demand_ips[cpu] * 40) / 100;
			if (is_chain) /* consider long long history frame smooth */
				prev_cpu_demand_ips[cpu] = my_demand_ips;
			else /* 2 frame smooth */
				prev_cpu_demand_ips[cpu] = demand_ips;
		} else {
			/* no smooth: just use previous frame's ips demand */
			my_demand_ips = demand_ips;
			prev_cpu_demand_ips[cpu] = demand_ips;
		}

		/* base_CPI fitting: cal stall_SPI aka mem_stall */
		my_inst_CPI = inst_stall_CPI;
		mem_stall = pSPI - my_inst_CPI / (raw_freq / 1000000);
		raw_mem_stall = mem_stall;

		if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[0])) {
			/* cluster 0: cpu0-3 */
			mem_stall = my_long_min(IPS_CLUSTER0_MAX_MEM_STALL, mem_stall);
			mem_stall = my_long_min(pSPI, mem_stall);
			my_mem_stall = my_long_max(IPS_CLUSTER0_MIN_MEM_STALL, mem_stall);
			my_target_freq_idx = ips_find_target_freq_idx(my_demand_ips,
								  my_mem_stall,
								  inst_stall_CPI,
								  cpu, real_ips,
								  raw_freq / 1000,
								  util_rate,
								  &bcm_value,
								  ips_line);
			if (my_target_freq_idx > cluster_target_freq_idx[0]) {
				cluster_target_freq_idx[0] = my_target_freq_idx;
				cluster_target_inst_CPI[0] = inst_stall_CPI;
				cluster_target_mem_stall[0] = my_mem_stall;
				cluster_cpu[0] = cpu;
				cluster_bcm[0] = bcm_value;
				cluster_target_util[0] = util_rate;
				cluster_demand_ips[0] = my_demand_ips;
			} else if (my_target_freq_idx == cluster_target_freq_idx[0]
				   && util_rate > cluster_target_util[0]) {
				/* prevent high mem stall and light load vote
				 * the final freq */
				cluster_target_freq_idx[0] = my_target_freq_idx;
				cluster_target_inst_CPI[0] = inst_stall_CPI;
				cluster_target_mem_stall[0] = my_mem_stall;
				cluster_cpu[0] = cpu;
				cluster_bcm[0] = bcm_value;
				cluster_target_util[0] = util_rate;
				cluster_demand_ips[0] = my_demand_ips;
			}
			tf = ips_c0_cpufreq_table[my_target_freq_idx];
		} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[1])) {
			/* cluster1: cpu4-6 */
			mem_stall = my_long_min(IPS_CLUSTER1_MAX_MEM_STALL, mem_stall);
			mem_stall = my_long_min(pSPI, mem_stall);
			my_mem_stall = my_long_max(IPS_CLUSTER1_MIN_MEM_STALL, mem_stall);
			my_target_freq_idx = ips_find_target_freq_idx(my_demand_ips,
								  my_mem_stall,
								  inst_stall_CPI,
								  cpu, real_ips,
								  raw_freq / 1000,
								  util_rate,
								  &bcm_value,
								  ips_line);
			if (my_target_freq_idx > cluster_target_freq_idx[1]) {
				cluster_target_freq_idx[1] = my_target_freq_idx;
				cluster_target_inst_CPI[1] = inst_stall_CPI;
				cluster_target_mem_stall[1] = my_mem_stall;
				cluster_cpu[1] = cpu;
				cluster_bcm[1] = bcm_value;
				cluster_target_util[1] = util_rate;
				cluster_demand_ips[1] = my_demand_ips;
			} else if (my_target_freq_idx == cluster_target_freq_idx[1]
				   && util_rate > cluster_target_util[1]) {
				/* prevent high mem stall and light load vote
				 * the final freq */
				cluster_target_freq_idx[1] = my_target_freq_idx;
				cluster_target_inst_CPI[1] = inst_stall_CPI;
				cluster_target_mem_stall[1] = my_mem_stall;
				cluster_cpu[1] = cpu;
				cluster_bcm[1] = bcm_value;
				cluster_target_util[1] = util_rate;
				cluster_demand_ips[1] = my_demand_ips;
			}
			tf = ips_c1_cpufreq_table[my_target_freq_idx];
		} else if (cpumask_test_cpu(cpu, &ips_cluster_cpumask[2])) {
			/* cluster2: cpu7 */
			mem_stall = my_long_min(IPS_CLUSTER2_MAX_MEM_STALL, mem_stall);
			mem_stall = my_long_min(pSPI, mem_stall);
			my_mem_stall = my_long_max(IPS_CLUSTER2_MIN_MEM_STALL, mem_stall);
			my_target_freq_idx = ips_find_target_freq_idx(my_demand_ips,
								  my_mem_stall,
								  inst_stall_CPI,
								  cpu, real_ips,
								  raw_freq / 1000,
								  util_rate,
								  &bcm_value,
								  ips_line);
			if (my_target_freq_idx > cluster_target_freq_idx[2]) {
				cluster_target_freq_idx[2] = my_target_freq_idx;
				cluster_target_inst_CPI[2] = inst_stall_CPI;
				cluster_target_mem_stall[2] = my_mem_stall;
				cluster_cpu[2] = cpu;
				cluster_bcm[2] = bcm_value;
				cluster_target_util[2] = util_rate;
				cluster_demand_ips[2] = my_demand_ips;
			} else if (my_target_freq_idx == cluster_target_freq_idx[2]
				   && util_rate > cluster_target_util[2]) {
				/* prevent high mem stall and light load vote
				 * the final freq */
				cluster_target_freq_idx[2] = my_target_freq_idx;
				cluster_target_inst_CPI[2] = inst_stall_CPI;
				cluster_target_mem_stall[2] = my_mem_stall;
				cluster_cpu[2] = cpu;
				cluster_bcm[2] = bcm_value;
				cluster_target_util[2] = util_rate;
				cluster_demand_ips[2] = my_demand_ips;
			}
			tf = ips_c2_cpufreq_table[my_target_freq_idx];
		}

		/* predict SPI = (freq * stall_SPI +  base_CPI)/freq */
		pred_pSPI = (raw_freq * prev_mem_stall[cpu][1] + prev_inst_CPI[cpu][1] * 1000000) / raw_freq;

		trace_ips_memlat_mem_pmus(cpu, bcm_value, ips_line[0], ips_line[1],
				      my_target_freq_idx, ips_line[2], 0, raw_demand_ips,
				      demand_ips, my_demand_ips, raw_inst_CPI, raw_mem_stall);
		/* trace event stats */
		trace_ips_memlat_mem_stall(cpu, mem_stall, inst_stall_CPI, pSPI,
				pred_pSPI, cycles, prev_freq[cpu], raw_freq,
				instrs, util_rate, tf);
		trace_ips_memlat_cpu_time(cpu, cpu_cycle_time, 0,
				      cpu_run_time, delta_us);
		scnprintf(ips_cpu_vote_tag[cpu], DCVS_TRACE_LEN,
			  "%d_%llu(%u)[%u,%u]_%lu_%u(%llu)_%ld",
			  bcm_value, demand_ips, ips_line[2]/*boost IPS*/,
			  ips_line[0], ips_line[1], util_rate,
			  tf / 1000, raw_freq / 1000000, instrs * 100 / cycles);
		scnprintf(cpu_tag, DCVS_TRACE_LEN, "CPU%d_ips", cpu);
		DCVS_ATRACE_N(cpu_tag, ips_cpu_vote_tag[cpu]);

		prev_freq[cpu] = raw_freq;
		prev_mem_stall[cpu][0] = mem_stall;
		prev_mem_stall[cpu][1] = my_mem_stall;
		prev_inst_CPI[cpu][0] = inst_stall_CPI;
		prev_inst_CPI[cpu][1] = my_inst_CPI;
		ips_line[0] = 0;
		ips_line[1] = 0;
		ips_line[2] = 0;

loop_ignore:
		end_ns = ktime_get_ns();
		cost_ns += (end_ns - start_ns);
		/* ---- IPS algo end ---- */

		memcpy(&stats->prev, &stats->curr, sizeof(stats->curr));
	}

	for_each_possible_cpu(cpu) {
		stats = per_cpu(sampling_stats, cpu);
		spin_unlock(&stats->ctrs_lock);
	}
	local_irq_restore(flags);

	trace_ips_memlat_compute_cost(smp_processor_id(), cost_ns, cpufreq_quick_get(smp_processor_id()));
	ips_algo_time_histogram_stats(cost_ns);
	/* ips stats only start */
	vsync_count++;
	delta_vsync_us = delta_us_acc / 8;
	/* all cpu instrutions, ips@per period, unit: ms */
	total_ips += instrs_acc * 1000 / delta_vsync_us;
	/* total instrutions & cycles */
	total_instrs_acc += instrs_acc;
	total_cycles_acc += cycles_acc;
	/* total ipc until reset, stats NOT per frame */
	if (total_cycles_acc > 0)
		total_ipc = total_instrs_acc * 1000 / total_cycles_acc;
	/* all cpu cycles, unit: kHz, freq@per frame, TODO: normalization */
	total_average_freq += cycles_acc * 1000 / delta_vsync_us;

	for (cluster_id = 0; cluster_id < IPS_MAX_CLUSTER_NUM; cluster_id++) {
		cluster_instrs_acc[cluster_id] += cluster_instrs[cluster_id];
		cluster_cycles_acc[cluster_id] += cluster_cycles[cluster_id];
		/* Prevent divide-by-zero */
		if (cluster_cycles_acc[cluster_id] > 0)
			cluster_ipc[cluster_id] = cluster_instrs_acc[cluster_id] * 1000 / cluster_cycles_acc[cluster_id];
		if (cluster_id == 0) {
			delta_vsync_us = cluster_delta_vsync_us[cluster_id] / 4;
			cluster_ips[cluster_id] +=
				cluster_instrs[cluster_id] * 1000 / delta_vsync_us;
			/* per cluster cycles unit: kHz */
			cluster_average_freq[cluster_id] += cluster_cycles[cluster_id] * 1000 / delta_vsync_us;
		} else if (cluster_id == 1) {
			delta_vsync_us = cluster_delta_vsync_us[cluster_id] / 3;
			cluster_ips[cluster_id] +=
				cluster_instrs[cluster_id] * 1000 / delta_vsync_us;
			/* per cluster cycles unit: kHz */
			cluster_average_freq[cluster_id] += cluster_cycles[cluster_id] * 1000 / delta_vsync_us;
		} else if (cluster_id == 2) {
			delta_vsync_us = cluster_delta_vsync_us[cluster_id] / 1;
			cluster_ips[cluster_id] +=
				cluster_instrs[cluster_id] * 1000 / delta_vsync_us;
			/* per cluster cycles unit: kHz */
			cluster_average_freq[cluster_id] += cluster_cycles[cluster_id] * 1000 / delta_vsync_us;
		}
	}
	/* ips stats only end */

	/* final step3: set cpu min/max freq, use MAX core freq */
	for (cluster_id = 0; cluster_id < IPS_MAX_CLUSTER_NUM; cluster_id++) {
		ips_set_target_freq(cluster_id,
				    cpumask_first(&ips_cluster_cpumask[cluster_id]),
				    cluster_cpu[cluster_id],
				    cluster_target_freq_idx[cluster_id],
				    cluster_target_mem_stall[cluster_id],
				    cluster_target_inst_CPI[cluster_id],
				    cluster_demand_ips[cluster_id],
				    cluster_target_util[cluster_id],
				    cluster_bcm[cluster_id]);
	}
}

/* sampling path update work */
static void ips_update_work(struct work_struct *work)
{
	/* safety check: ensure ips_data is valid and module is initialized */
	if (unlikely(!ips_data || !ips_data->inited))
		return;

#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK_DDR)
	s64 duration;
	ktime_t now = ktime_get();
	int dram_khz = mtk_dvfsrc_query_opp_info(MTK_DVFSRC_CURR_DRAM_KHZ);
	int vcore_uv = mtk_dvfsrc_query_opp_info(MTK_DVFSRC_CURR_VCORE_UV);
	duration = ktime_us_delta(ktime_get(), now);
	DCVS_ATRACE_C("ddr_time", duration);
	DCVS_ATRACE_C("ddr_freq", dram_khz);
	DCVS_ATRACE_C("vcore_uv", vcore_uv);
	dcvs_stats_record_time(DCVS_DDR, dram_khz);
#endif
}

static enum hrtimer_restart ips_hrtimer_handler(struct hrtimer *timer)
{
	s64 duration;
	ktime_t now = ktime_get();

	/* safety check: ensure ips_data is valid and module is initialized */
	if (unlikely(!ips_data || !ips_data->inited))
		return HRTIMER_NORESTART;

	/*
	 * Double-check both sampling_enabled and ddr_stats_enabled here to
	 * handle race condition: hrtimer may have been started just before
	 * both features were disabled.
	 */
	if (unlikely(!ips_data->sampling_enabled && !ddr_stats_enabled))
		return HRTIMER_NORESTART;

	/* Only run IPS calculation if sampling is enabled */
	if (ips_data->sampling_enabled)
		calculate_sampling_stats();

	/* cost time < 100us, general ~20us */
	duration = ktime_us_delta(ktime_get(), now);
	DCVS_ATRACE_C("hrtimer_time", duration);

	/* DDR stats workqueue - independent of sampling_enabled */
	if (ddr_stats_enabled && ips_data->ips_wq)
		queue_work(ips_data->ips_wq, &ips_data->work);

	return HRTIMER_NORESTART;
}

static const u64 HALF_TICK_NS = (NSEC_PER_SEC / HZ) >> 1;
#define IPS_UPDATE_DELAY (100 * NSEC_PER_USEC)
static void ips_jiffies_update_cb(void *unused, void *extra)
{
	ktime_t now;
	s64 delta_ns;

	/* check ips_data first to avoid null pointer dereference */
	if (unlikely(!ips_data || !ips_data->inited))
		return;

	now = ktime_get();
	delta_ns = now - ips_data->last_jiffy_ts + HALF_TICK_NS;

	if (delta_ns > ms_to_ktime(ips_data->sample_ms)) {
		DCVS_ATRACE_C("trigger_hrtimer", 1);
		/* delay time > 100us, measure about 125us+ */
		/*
		 * Start hrtimer if either:
		 * - sampling_enabled: for IPS frequency calculation
		 * - ddr_stats_enabled: for DDR frequency distribution stats
		 */
		if (ips_data->sampling_enabled || ddr_stats_enabled)
			hrtimer_start(&ips_data->timer, IPS_UPDATE_DELAY,
							HRTIMER_MODE_REL_PINNED);
		ips_data->last_jiffy_ts = now;
	} else {
		DCVS_ATRACE_C("trigger_hrtimer", 0);
	}
}

/*
 * Note: must hold stats->ctrs_lock and populate stats->raw_ctrs
 * before calling this API.
 */
static void process_raw_ctrs(struct cpu_stats *stats)
{
	int i, idx;
	struct cpu_ctrs *curr_ctrs = &stats->curr;
	struct qcom_pmu_data *raw_ctrs = &stats->raw_ctrs;
	u32 event_id;
	u64 ev_data;

	for (i = 0; i < raw_ctrs->num_evs; i++) {
		event_id = raw_ctrs->event_ids[i];
		ev_data = raw_ctrs->ev_data[i];
		if (!event_id)
			break;

		for (idx = 0; idx < NUM_IPS_EVS; idx++) {
			if (event_id != ips_data->ips_ev_ids[idx])
				continue;
			curr_ctrs->ips_ctrs[idx] = ev_data;
			break;
		}
	}
}

static void ips_pmu_idle_cb(struct qcom_pmu_data *data, int cpu, int state)
{
	struct cpu_stats *stats;
	unsigned long flags;

	/* check ips_data first to avoid null pointer dereference */
	if (unlikely(!ips_data || !ips_data->inited))
		return;

	stats = per_cpu(sampling_stats, cpu);
	if (unlikely(!stats))
		return;

	spin_lock_irqsave(&stats->ctrs_lock, flags);
	memcpy(&stats->raw_ctrs, data, sizeof(*data));
	process_raw_ctrs(stats);
	stats->idle_sample = true;
	spin_unlock_irqrestore(&stats->ctrs_lock, flags);
}

static struct qcom_pmu_notif_node ips_idle_notif = {
	.idle_cb = ips_pmu_idle_cb,
};

static void ips_sched_tick_cb(void *unused, struct rq *rq)
{
	int ret, cpu = smp_processor_id();
	struct cpu_stats *stats;
	ktime_t now = ktime_get();
	s64 delta_ns;
	unsigned long flags;

	/* check ips_data first to avoid null pointer dereference */
	if (unlikely(!ips_data || !ips_data->inited))
		return;

	stats = per_cpu(sampling_stats, cpu);
	if (unlikely(!stats))
		return;

	spin_lock_irqsave(&stats->ctrs_lock, flags);
	delta_ns = now - stats->last_sample_ts + HALF_TICK_NS;
	if (delta_ns < ms_to_ktime(ips_data->sample_ms))
		goto out;
	stats->sample_ts = now;
	stats->idle_sample = false;
	stats->raw_ctrs.num_evs = 0;
	ret = qcom_pmu_read_all_local(&stats->raw_ctrs);
	if (ret < 0 || stats->raw_ctrs.num_evs == 0) {
		/* PMU read failure is caused by init event setup failure,
		 * if init succeed, no error anymore */
		pr_err_ratelimited("error reading pmu counters on cpu%d: %d\n", cpu, ret);
		goto out;
	}
	process_raw_ctrs(stats);

out:
	spin_unlock_irqrestore(&stats->ctrs_lock, flags);
}


static int ips_sampling_init(void)
{
	int ret;

	pr_err("%s: enter.\n", __func__);
	ips_data->ips_wq = create_freezable_workqueue("ips_wq");
	if (!ips_data->ips_wq) {
		pr_err("Couldn't create ips workqueue.\n");
		return -ENOMEM;
	}
	INIT_WORK(&ips_data->work, &ips_update_work);

	hrtimer_init(&ips_data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ips_data->timer.function = ips_hrtimer_handler;

	ret = register_trace_android_vh_jiffies_update(ips_jiffies_update_cb, NULL);
	if (ret) {
		pr_err("Failed to register jiffies_update hook: %d\n", ret);
		destroy_workqueue(ips_data->ips_wq);
		ips_data->ips_wq = NULL;
		return ret;
	}

	return 0;
}

static void ips_sampling_stats_free(void)
{
	int cpu;
	struct cpu_stats *stats;

	for_each_possible_cpu(cpu) {
		stats = per_cpu(sampling_stats, cpu);
		if (stats) {
			kfree(stats);
			per_cpu(sampling_stats, cpu) = NULL;
		}
	}
}

static int ips_sampling_enable(void)
{
	int cpu, ret;
	struct cpu_stats *stats;

	pr_err("%s: enter.\n", __func__);
	for_each_possible_cpu(cpu) {
		stats = kzalloc(sizeof(*stats), GFP_KERNEL);
		if (!stats) {
			pr_err("Couldn't alloc stats memory.\n");
			ret = -ENOMEM;
			goto err_free_stats;
		}
		per_cpu(sampling_stats, cpu) = stats;
		spin_lock_init(&stats->ctrs_lock);
	}

	ret = register_trace_android_vh_scheduler_tick(ips_sched_tick_cb, NULL);
	if (ret) {
		pr_err("Failed to register scheduler_tick hook: %d\n", ret);
		goto err_free_stats;
	}

	ret = qcom_pmu_idle_register(&ips_idle_notif);
	if (ret) {
		pr_err("Failed to register PMU idle notif: %d\n", ret);
		goto err_unregister_tick;
	}

	return 0;

err_unregister_tick:
	unregister_trace_android_vh_scheduler_tick(ips_sched_tick_cb, NULL);
err_free_stats:
	ips_sampling_stats_free();
	return ret;
}

#define INST_EV		0x08
#define CYC_EV		0x11
#define IPS_DTSI_CONFIG_PATH "/soc/qcom,memlat/ddr/gold"
static int __init oplus_ips_init(void)
{
	struct device *dev_root;
	struct ips_dev_data *dev_data;
	struct device_node *leaf_np = NULL;
	int i, cpu, ret;
	u32 event_id;

	dev_data = kzalloc(sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	dev_data->sample_ms = 8;

	/* 4 amu: AMU INST */
	dev_data->ips_ev_ids[IPS_INST_IDX] = INST_EV;
	/* AMU CYCLE */
	dev_data->ips_ev_ids[IPS_CYC_IDX] = CYC_EV;
	/* AMU STALL-BACKEND-MEM */
	dev_data->ips_ev_ids[IPS_STALL_BKD_MEM_IDX] = STALL_BK_MEM_EV;
	/* AMU CYCLE_CNT */
	dev_data->ips_ev_ids[IPS_CYC_CNT_IDX] = CYCLE_CNT_EV;
	/* add pmu event for IPS */
	// 0x02
	dev_data->ips_ev_ids[IPS_L1I_TLB_CR_IDX] = L1I_TLB_REFILL_EV;
	// 0x13
	dev_data->ips_ev_ids[IPS_MEM_ACCESS_IDX] = MEM_ACCESS_EV;
	// 0x14
	dev_data->ips_ev_ids[IPS_L1I_CA_IDX] = L1I_CACHE_EV;
	// stall-backend(0x24)
	dev_data->ips_ev_ids[IPS_STALL_BKD_IDX] = STALL_BKD_EV;
	// 0x46
	dev_data->ips_ev_ids[IPS_L1D_WB_V_IDX] = L1D_WB_V_EV;
	// 0x51
	dev_data->ips_ev_ids[IPS_L2D_CW_IDX] = L2D_W_EV;
	// stall frontend membound(0x8158)
	dev_data->ips_ev_ids[IPS_STALL_FTD_MEMBOUND_IDX] = STALL_FTD_MEMBOUND_EV;
	// 0x8160
	dev_data->ips_ev_ids[IPS_STALL_FTD_CPUBOUND_IDX] = STALL_FTD_CPUBOUND_EV;
	// stall backend cpubound(0x816A)
	dev_data->ips_ev_ids[IPS_STALL_BKD_CPUBOUND_IDX] = STALL_BKD_CPUBOUND_EV;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < NUM_IPS_EVS; i++) {
			event_id = dev_data->ips_ev_ids[i];
			if (!event_id)
				continue;
			ret = qcom_pmu_event_supported(event_id, cpu);
			if (!ret)
				continue;
			if (ret != -EPROBE_DEFER) {
				pr_err("ev=0x%x not found on cpu%d: %d\n",
						event_id, cpu, ret);
				if (event_id == INST_EV || event_id == CYC_EV)
					goto err_free_devdata;
			} else {
				pr_err("pmu configure error=%d, event=0x%x init failed!\n",
				       event_id, ret);
				goto err_free_devdata;
			}
		}
	}

	dev_root = bus_get_dev_root(&cpu_subsys);
	if (dev_root) {
		ret = kobject_init_and_add(&dev_data->kobj,
					   &ips_settings_ktype, &dev_root->kobj,
					   "ips_settings");
		put_device(dev_root);
	} else {
		pr_err("failed to get cpu_subsys dev_root\n");
		ret = -ENODEV;
		goto err_free_devdata;
	}
	if (ret < 0) {
		pr_err("failed to init ips_settings kobj: %d\n", ret);
		goto err_put_kobj;
	}
	ips_data = dev_data;

	mutex_lock(&ips_lock);
	leaf_np = of_find_node_by_path(IPS_DTSI_CONFIG_PATH);
	if (leaf_np != NULL) {
		pr_info("find group ddr and gold.\n");
		/* read mon attr  */
		if (of_property_read_bool(leaf_np, "qcom,sampling-enabled")) {
			/* disable ips function. */
			ips_data->sampling_enabled   = false;
			pr_err("find qcom,sampling-enabled, so DISABLE ips pmu sampling & voting.\n");
		} else {
			/* enable ips function
			* qcom: false and not found 'qcom,sampling-enabled', this means qcom,cpucp-enabled.
			* todo: mtk:  false, enable sampling.
			*/
			pr_err("Not found qcom,sampling-enabled, so ENABLE ips pmu sampling and voting.\n");
			if (of_property_read_bool(leaf_np, "qcom,cpucp-enabled")) {
				pr_err("find qcom,cpucp-enabled, tips: ips sample pmu and voting.\n");
			} else {
				pr_err("Not found qcom,cpucp-enabled, MTK platfrom ???.\n");
			}
			if (!ips_data->sampling_enabled) {
				ret = ips_sampling_enable();
				if (ret < 0) {
					of_node_put(leaf_np);
					mutex_unlock(&ips_lock);
					pr_err("ips_sampling_enable failed: %d\n", ret);
					goto err_sampling_enable;
				}
				ips_data->sampling_enabled = true;
			}
		}
		of_node_put(leaf_np);
	} else {
		pr_err("Not found ips sampling config dtsi, mtk platform.\n");
		if (!ips_data->sampling_enabled) {
			ret = ips_sampling_enable();
			if (ret < 0) {
				mutex_unlock(&ips_lock);
				pr_err("ips_sampling_enable failed: %d\n", ret);
				goto err_sampling_enable;
			}
			ips_data->sampling_enabled = true;
		}
	}

	/* Now it's safe to init sampling - sampling_stats is already allocated */
	if (!ips_data->sampling_inited) {
		ret = ips_sampling_init();
		if (ret < 0) {
			mutex_unlock(&ips_lock);
			pr_err("ips_sampling_init failed: %d\n", ret);
			goto err_sampling_init;
		}
		ips_data->sampling_inited = true;
	}
	mutex_unlock(&ips_lock);
	memlat_set_sample_ms_ops(ips_set_sample_ms);
	ips_algo_init();
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK)
	/* ddr freq time in stat */
	ret = ips_dcvs_hw_init();
	if (ret < 0) {
		pr_err("ips_dcvs_hw_init failed: %d\n", ret);
		goto err_dcvs_init;
	}
#endif
	ips_data->inited = true;
	pr_info("OPLUS IPS module initialized successfully\n");
	return 0;

/* error process - cleanup in reverse order of initialization */
err_dcvs_init:
	/* DCVS has already cleared in ips_dcvs_hw_init */
	ips_algo_exit();  /* cleanup governor and freq_qos */
err_sampling_init:
	/* clear sampling_init resource */
	if (ips_data->sampling_inited) {
		unregister_trace_android_vh_jiffies_update(ips_jiffies_update_cb, NULL);
		if (ips_data->ips_wq) {
			destroy_workqueue(ips_data->ips_wq);
			ips_data->ips_wq = NULL;
		}
	}
err_sampling_enable:
	/* unregister sampling_enable hooks and free stats */
	if (ips_data->sampling_enabled) {
		unregister_trace_android_vh_scheduler_tick(ips_sched_tick_cb, NULL);
		qcom_pmu_idle_unregister(&ips_idle_notif);
		ips_sampling_stats_free();
	}
	/* reset data */
	ips_data = NULL;
err_put_kobj:
	/* clear kobject */
	kobject_del(&dev_data->kobj);
	kobject_put(&dev_data->kobj);
err_free_devdata:
	/* free dev_data */
	kfree(dev_data);
	pr_err("OPLUS IPS module initialization failed: %d\n", ret);
	return ret;
}

static void __exit oplus_ips_exit(void)
{
	pr_info("%s: cleaning up IPS module\n", __func__);

	if (!ips_data || !ips_data->inited)
		return;

	/* 1. set flag to prevent process */
	mutex_lock(&ips_lock);
	ips_data->inited = false;
	ips_data->sampling_enabled = false;
	mutex_unlock(&ips_lock);

	/* 2. unregister fps change callback (MTK) */
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK)
	memlat_clear_sample_ms_ops();
#endif

	/* 3. cleanup ips dcvs hw (MTK ddr stats) */
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK)
	ips_dcvs_hw_exit();
	pr_info("ips_dcvs_hw resources cleaned up\n");
#endif

	/* 4. cleanup ips algo (governor, freq qos) */
	ips_algo_exit();
	pr_info("IPS algo resources cleaned up\n");

	/*
	 * 5. Unregister trace hooks BEFORE cancelling hrtimer/workqueue.
	 * Critical: jiffies_update hook can restart hrtimer, so we must
	 * unregister it first to prevent race condition.
	 */
	unregister_trace_android_vh_jiffies_update(ips_jiffies_update_cb, NULL);
	unregister_trace_android_vh_scheduler_tick(ips_sched_tick_cb, NULL);
	qcom_pmu_idle_unregister(&ips_idle_notif);
	/*
	 * Ensure all callbacks have completed on all CPUs.
	 * tracepoint_synchronize_unregister uses synchronize_rcu_tasks
	 * to wait for any in-flight callbacks.
	 */
	tracepoint_synchronize_unregister();
	pr_info("hooks unregistered and synchronized\n");

	/* 6. cancel hrtimer - now safe since jiffies_update is unregistered */
	if (hrtimer_active(&ips_data->timer)) {
		hrtimer_cancel(&ips_data->timer);
		pr_info("hrtimer cancelled\n");
	}

	/* 7. cancel workqueue */
	if (ips_data->ips_wq) {
		cancel_work_sync(&ips_data->work);
		flush_workqueue(ips_data->ips_wq);
		destroy_workqueue(ips_data->ips_wq);
		ips_data->ips_wq = NULL;
		pr_info("workqueue destroyed\n");
	}

	/* 8. free percpu memory */
	ips_sampling_stats_free();
	pr_info("per-cpu stats freed\n");

	/* 9. cleanup kobject */
	if (ips_data->kobj.state_initialized) {
		kobject_del(&ips_data->kobj);
		kobject_put(&ips_data->kobj);
		pr_info("kobject cleaned up\n");
	}

	/* 10. free ips_data and set to NULL */
	kfree(ips_data);
	ips_data = NULL;

	pr_info("%s: IPS module cleanup complete\n", __func__);
}

module_init(oplus_ips_init);
module_exit(oplus_ips_exit);
MODULE_DESCRIPTION("OPLUS IPS Driver");
MODULE_LICENSE("GPL");
