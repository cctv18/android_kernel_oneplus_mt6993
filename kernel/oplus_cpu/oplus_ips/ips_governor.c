// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 *
 * IPS Governor - Fast Switch Only Implementation
 * Note: This governor only supports fast_switch mode.
 *       kthread-based slow path is not supported.
 */

#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/rcupdate.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/arch_topology.h>
#include <linux/cpuidle.h>
#include "ips_private.h"
#include "oplus_pmu.h"


struct ips_gov_tunables {
	struct gov_attr_set		attr_set;
	unsigned int			target_loads;
	int				soft_freq_max;
	int				soft_freq_min;
	int				ignore_freq_min;
	int				ignore_freq_max;
	int				soft_freq_boost;
};

struct ips_gov_policy {
	struct cpufreq_policy	*policy;

	struct ips_gov_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	unsigned int		next_freq;
	unsigned int		freq_cached;

	unsigned int	target_load;
};

struct ips_gov_cpu {
	struct ips_gov_policy	*ig_policy;
	unsigned int		cpu;

	unsigned long		util;
	unsigned int		reasons;
	unsigned int		flags;
};

static DEFINE_PER_CPU(struct ips_gov_cpu, ips_gov_cpu);
static DEFINE_PER_CPU(struct ips_gov_tunables *, cached_tunables);
static DEFINE_MUTEX(global_tunables_lock);
static struct ips_gov_tunables *global_tunables;

/*-------------IPS Governor runtime statistics (per-cluster)-------------------*/
struct ips_gov_cluster_stats {
	u64 start_count;		/* Number of times governor started */
	u64 stop_count;			/* Number of times governor stopped */
	u64 total_time_ms;		/* Total time governor was active (ms) */
	u64 last_start_jiffies;		/* Timestamp of last start */
	bool is_active;			/* Is this cluster governor active */
};
static struct ips_gov_cluster_stats ips_gov_cstats[ARCH_CLUSTER_NUM];

/**
 * ips_gov_get_cluster_stats - Get IPS governor runtime statistics for a cluster
 * @cid: Cluster ID (0-3)
 * @start_count: Output for number of governor starts
 * @stop_count: Output for number of governor stops
 * @total_time_ms: Output for total active time in milliseconds
 * @is_active: Output for current active state
 */
void ips_gov_get_cluster_stats(int cid, u64 *start_count, u64 *stop_count,
			       u64 *total_time_ms, bool *is_active)
{
	if (cid < 0 || cid >= ARCH_CLUSTER_NUM) {
		if (start_count)
			*start_count = 0;
		if (stop_count)
			*stop_count = 0;
		if (total_time_ms)
			*total_time_ms = 0;
		if (is_active)
			*is_active = false;
		return;
	}

	if (start_count)
		*start_count = READ_ONCE(ips_gov_cstats[cid].start_count);
	if (stop_count)
		*stop_count = READ_ONCE(ips_gov_cstats[cid].stop_count);
	if (total_time_ms) {
		u64 total = READ_ONCE(ips_gov_cstats[cid].total_time_ms);
		/* Add current session time if still active */
		if (READ_ONCE(ips_gov_cstats[cid].is_active)) {
			u64 current_session = jiffies_to_msecs(
				jiffies - READ_ONCE(ips_gov_cstats[cid].last_start_jiffies));
			total += current_session;
		}
		*total_time_ms = total;
	}
	if (is_active)
		*is_active = READ_ONCE(ips_gov_cstats[cid].is_active);
}



/************************** sysfs interface ************************/
static void ips_gov_update_soft_limit_cpufreq(struct cpufreq_policy *policy, int boost_freq);

static inline struct ips_gov_tunables *to_ips_gov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct ips_gov_tunables, attr_set);
}

static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	return sprintf(buf, "%d\n", tunables->target_loads);
}

static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf,
					size_t count)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	unsigned int new_target_loads = 80;

	if (kstrtouint(buf, 10, &new_target_loads))
		return -EINVAL;

	tunables->target_loads = new_target_loads;
	return count;
}

static ssize_t soft_freq_max_show(struct gov_attr_set *attr_set, char *buf)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	int soft_freq_max = tunables->soft_freq_max;

	if (soft_freq_max < 0) {
		return sprintf(buf, "max\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_max);
	}
}

static ssize_t soft_freq_max_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	struct ips_gov_policy *ig_policy;
	int cid;
	int new_soft_freq_max = -1;
	char tmpbuf[DCVS_TRACE_LEN];

	if (list_empty(&attr_set->policy_list))
		return -ENODEV;

	ig_policy = list_first_entry(&attr_set->policy_list, struct ips_gov_policy, tunables_hook);
	cid = get_cid_from_cpu(ig_policy->policy->cpu);

	if (kstrtoint(buf, 10, &new_soft_freq_max))
		return -EINVAL;

	if (tunables->soft_freq_max == new_soft_freq_max) {
		return count;
	}

	if (new_soft_freq_max == 3) {
		/* enable soft_freq_max */
		tunables->ignore_freq_max = false;
		tunables->soft_freq_max = -1;
		return count;
	} else if (new_soft_freq_max == 4) {
		/* ignore soft freq max set */
		tunables->ignore_freq_max = true;
		tunables->soft_freq_max = -1;
	}

	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_ips_max_freq", cid);
	DCVS_ATRACE_C(tmpbuf, new_soft_freq_max);

	if (tunables->ignore_freq_max)
		return count;

	tunables->soft_freq_max = new_soft_freq_max;
	ips_gov_update_soft_limit_cpufreq(ig_policy->policy, 0);
	return count;
}

static ssize_t soft_freq_min_show(struct gov_attr_set *attr_set, char *buf)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	int soft_freq_min = tunables->soft_freq_min;

	if (soft_freq_min < 0) {
		return sprintf(buf, "0\n");
	} else {
		return sprintf(buf, "%d\n", soft_freq_min);
	}
}

static ssize_t soft_freq_min_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	struct ips_gov_policy *ig_policy;
	int cid;
	int new_soft_freq_min = -1;
	char tmpbuf[DCVS_TRACE_LEN];

	if (list_empty(&attr_set->policy_list))
		return -ENODEV;

	ig_policy = list_first_entry(&attr_set->policy_list, struct ips_gov_policy, tunables_hook);
	cid = get_cid_from_cpu(ig_policy->policy->cpu);

	if (kstrtoint(buf, 10, &new_soft_freq_min))
		return -EINVAL;

	if (tunables->soft_freq_min == new_soft_freq_min) {
		return count;
	}

	if (new_soft_freq_min == 3) {
		/* enable soft_freq_min */
		tunables->ignore_freq_min = false;
		tunables->soft_freq_min = 0;
		return count;
	} else if (new_soft_freq_min == 4) {
		/* ignore soft freq min set */
		tunables->ignore_freq_min = true;
		tunables->soft_freq_min = 0;
	}

	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_ips_min_freq", cid);
	DCVS_ATRACE_C(tmpbuf, new_soft_freq_min);

	if (tunables->ignore_freq_min)
		return count;

	tunables->soft_freq_min = new_soft_freq_min;
	ips_gov_update_soft_limit_cpufreq(ig_policy->policy, 0);
	return count;
}

static ssize_t soft_freq_boost_show(struct gov_attr_set *attr_set, char *buf)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	int soft_freq_boost = tunables->soft_freq_boost;

	return sprintf(buf, "%d\n", soft_freq_boost);
}

static ssize_t soft_freq_boost_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct ips_gov_tunables *tunables = to_ips_gov_tunables(attr_set);
	int new_soft_freq_boost = -1;

	if (kstrtoint(buf, 10, &new_soft_freq_boost))
		return -EINVAL;

	if (tunables->soft_freq_boost == new_soft_freq_boost) {
		return count;
	}

	tunables->soft_freq_boost = new_soft_freq_boost;

	return count;
}

static struct governor_attr target_loads =
	__ATTR(target_loads, 0664, target_loads_show, target_loads_store);

static struct governor_attr soft_freq_max =
	__ATTR(soft_freq_max, 0664, soft_freq_max_show, soft_freq_max_store);

static struct governor_attr soft_freq_min =
	__ATTR(soft_freq_min, 0664, soft_freq_min_show, soft_freq_min_store);

static struct governor_attr soft_freq_boost =
	__ATTR(soft_freq_boost, 0664, soft_freq_boost_show, soft_freq_boost_store);

static struct attribute *ips_gov_attrs[] = {
	&target_loads.attr,
	&soft_freq_max.attr,
	&soft_freq_min.attr,
	&soft_freq_boost.attr,
	NULL
};
ATTRIBUTE_GROUPS(ips_gov);

static struct kobj_type ips_gov_tunables_ktype = {
	.default_groups = ips_gov_groups,
	.sysfs_ops = &governor_sysfs_ops,
};

/*
 * Soft frequency clamping with priority: boost > min > max
 *
 * Priority rules:
 * 1. min: Sets the frequency floor (applied first)
 * 2. boost: Overrides min for temporary performance needs (applied second)
 * 3. max: Absolute ceiling, cannot be exceeded (applied last)
 *
 * Example: target=500, min=800, boost=1200, max=2000
 *          → After min: 800 → After boost: 1200 → After max: 1200
 *
 * Example: target=500, min=1500, boost=1000, max=2000
 *          → After min: 1500 → After boost: 1500 (boost < current, no change)
 *
 * Note: boost only raises frequency, it doesn't lower it below min.
 *       To force a specific boost frequency even below min, set min=-1.
 */
static unsigned int soft_freq_clamp(struct ips_gov_policy *ig_policy, unsigned int target_freq)
{
	int soft_freq_max = ig_policy->tunables->soft_freq_max;
	int soft_freq_min = ig_policy->tunables->soft_freq_min;
	int soft_freq_boost = ig_policy->tunables->soft_freq_boost;

	/* Step 1: Apply min floor first */
	if (soft_freq_min >= 0 && soft_freq_min > target_freq) {
		target_freq = soft_freq_min;
	}

	/* Step 2: Apply boost (can override min) */
	if (soft_freq_boost >= 0 && soft_freq_boost > target_freq) {
		target_freq = soft_freq_boost;
	}

	/* Step 3: Apply max ceiling (absolute limit) */
	if (soft_freq_max >= 0 && soft_freq_max < target_freq) {
		target_freq = soft_freq_max;
	}

	return target_freq;
}

static unsigned int get_next_freq(struct cpufreq_policy *policy, unsigned int target_freq)
{
	unsigned int next_f;
	struct ips_gov_policy *ig_policy = policy->governor_data;
	char tmpbuf[DCVS_TRACE_LEN];
	int cid = get_cid_from_cpu(policy->cpu);

	/* soft limit */
	next_f = soft_freq_clamp(ig_policy, target_freq);
	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_share1_soft", cid);
	DCVS_ATRACE_C(tmpbuf, next_f);

	/* hard limit */
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_QCOM)
	/* qcom not set cpufreq inefficiency flag */
	target_freq = cpufreq_driver_resolve_freq(policy, next_f);
#else
	/* for mtk, mtk set cpufreq inefficiency flag */
	target_freq = clamp_val(next_f, policy->min, policy->max);
#endif
	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_share1_hard", cid);
	DCVS_ATRACE_C(tmpbuf, target_freq);
	return target_freq;
}

/* update cpufreq api */
void ips_gov_update_cpufreq(unsigned int cpu, unsigned int target_freq)
{
	unsigned int next_f;
	struct ips_gov_cpu *ig_cpu = &per_cpu(ips_gov_cpu, cpu);
	struct ips_gov_policy *ig_policy;
	struct cpufreq_policy *policy;
	unsigned long flags;
	char tmpbuf[DCVS_TRACE_LEN];
	int cid;

	/*
	 * Use RCU read lock to safely access ig_policy.
	 * This prevents ig_policy from being freed while we're using it.
	 * The corresponding synchronize_rcu() is in ipsgov_exit().
	 */
	rcu_read_lock();

	ig_policy = rcu_dereference(ig_cpu->ig_policy);
	if (!ig_policy) {
		rcu_read_unlock();
		return;
	}

	policy = ig_policy->policy;
	cid = get_cid_from_cpu(cpu);

	raw_spin_lock_irqsave(&ig_policy->update_lock, flags);

	/* soft & hard limit */
	next_f = get_next_freq(policy, target_freq);
	ig_policy->freq_cached = next_f;
	if (ig_policy->next_freq == next_f)
		goto unlock;

	ig_policy->next_freq = next_f;
	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%d_ips_set", cid);
	DCVS_ATRACE_C(tmpbuf, next_f);

	/* fast switch only - kthread not supported */
	ig_policy->next_freq = cpufreq_driver_fast_switch(policy, next_f);

unlock:
	raw_spin_unlock_irqrestore(&ig_policy->update_lock, flags);
	rcu_read_unlock();
}

/*
 * update cpufreq api: soft limit for frameboost
 *
 * This function is called from sysfs store handlers (soft_freq_max_store,
 * soft_freq_min_store). Use RCU protection to safely access ig_policy,
 * preventing use-after-free if ipsgov_exit races with sysfs access.
 */
static void ips_gov_update_soft_limit_cpufreq(struct cpufreq_policy *policy, int boost_freq)
{
	unsigned int next_f;
	struct ips_gov_policy *ig_policy;
	unsigned long flags;

	if (!policy)
		return;

	/*
	 * Use RCU read lock to safely access governor_data.
	 * This pairs with synchronize_rcu() in ipsgov_exit().
	 */
	rcu_read_lock();

	ig_policy = rcu_dereference(policy->governor_data);
	if (!ig_policy) {
		rcu_read_unlock();
		return;
	}

	raw_spin_lock_irqsave(&ig_policy->update_lock, flags);

	if (boost_freq != 0) /* tweak */
		ig_policy->tunables->soft_freq_boost = boost_freq;

	/* soft & hard limit */
	next_f = get_next_freq(policy, ig_policy->next_freq);
	ig_policy->freq_cached = next_f;
	if (ig_policy->next_freq == next_f)
		goto unlock;

	ig_policy->next_freq = next_f;

	/* fast switch only - kthread not supported */
	ig_policy->next_freq = cpufreq_driver_fast_switch(policy, next_f);

unlock:
	raw_spin_unlock_irqrestore(&ig_policy->update_lock, flags);
	rcu_read_unlock();
}

static struct ips_gov_tunables *ips_gov_tunables_alloc(struct ips_gov_policy *ig_policy)
{
	struct ips_gov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &ig_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void ips_gov_tunables_free(struct ips_gov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static void ips_gov_tunables_save(struct cpufreq_policy *policy,
		struct ips_gov_tunables *tunables)
{
	int cpu;
	struct ips_gov_tunables *cached = per_cpu(cached_tunables, policy->cpu);

	if (!cached) {
		cached = kzalloc(sizeof(*tunables), GFP_KERNEL);
		if (!cached)
			return;

		for_each_cpu(cpu, policy->related_cpus)
			per_cpu(cached_tunables, cpu) = cached;
	}

	/* actually save the tunables data */
	cached->target_loads = tunables->target_loads;
	cached->soft_freq_max = tunables->soft_freq_max;
	cached->soft_freq_min = tunables->soft_freq_min;
	cached->ignore_freq_min = tunables->ignore_freq_min;
	cached->ignore_freq_max = tunables->ignore_freq_max;
	cached->soft_freq_boost = tunables->soft_freq_boost;
}

static void ips_gov_clear_cached_tunables(void)
{
	int cpu;
	struct ips_gov_tunables *cached, *prev = NULL;

	for_each_possible_cpu(cpu) {
		cached = per_cpu(cached_tunables, cpu);
		if (cached && cached != prev) {
			kfree(cached);
			prev = cached;
		}
		per_cpu(cached_tunables, cpu) = NULL;
	}
}

static struct ips_gov_policy *ips_gov_policy_alloc(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy;

	ig_policy = kzalloc(sizeof(*ig_policy), GFP_KERNEL);
	if (!ig_policy)
		return NULL;

	ig_policy->policy = policy;
	raw_spin_lock_init(&ig_policy->update_lock);
	return ig_policy;
}

static void ips_gov_policy_free(struct ips_gov_policy *ig_policy)
{
	kfree(ig_policy);
}

/* cpufreq ips governor interface */
#define IPS_CPUFREQ_GOV_NAME "ips"
static int ipsgov_init(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy;
	struct ips_gov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	/* This governor only supports fast_switch mode */
	if (!policy->fast_switch_enabled) {
		pr_err("ips_gov: fast_switch not enabled, governor not supported\n");
		ret = -EINVAL;
		goto disable_fast_switch;
	}

	ig_policy = ips_gov_policy_alloc(policy);
	if (!ig_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_ig_policy;
		}
		policy->governor_data = ig_policy;
		ig_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set,
				 &ig_policy->tunables_hook);
		goto out;
	}

	tunables = ips_gov_tunables_alloc(ig_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_ig_policy;
	}

	tunables->target_loads = 80;
	tunables->soft_freq_max = -1;
	tunables->soft_freq_min = -1;
	tunables->ignore_freq_min = false;
	tunables->ignore_freq_max = false;
	tunables->soft_freq_boost = -1;

	policy->governor_data = ig_policy;
	ig_policy->tunables = tunables;

	ret =  kobject_init_and_add(&tunables->attr_set.kobj,
				    &ips_gov_tunables_ktype,
				    get_governor_parent_kobj(policy),
				    "%s",
				    IPS_CPUFREQ_GOV_NAME);
	if (ret)
		goto fail;

	policy->dvfs_possible_from_any_cpu = 1;

out:
	mutex_unlock(&global_tunables_lock);
	pr_info("ips_gov: init succeed\n");
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	ips_gov_tunables_free(tunables);

free_ig_policy:
	mutex_unlock(&global_tunables_lock);
	ips_gov_policy_free(ig_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("ips_gov: init failed (err %d)\n", ret);
	return ret;
}

static void ipsgov_exit(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy = policy->governor_data;
	struct ips_gov_tunables *tunables = ig_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &ig_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		ips_gov_tunables_save(policy, tunables);
		ips_gov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	/*
	 * Ensure all RCU readers in ips_gov_update_cpufreq have completed
	 * before freeing ig_policy. This pairs with rcu_read_lock/unlock
	 * in ips_gov_update_cpufreq.
	 *
	 * Note: ig_cpu->ig_policy was already cleared in ipsgov_stop().
	 */
	synchronize_rcu();

	ips_gov_policy_free(ig_policy);
	cpufreq_disable_fast_switch(policy);
	pr_info("ips_gov: exit\n");
}

static int ipsgov_start(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy = policy->governor_data;
	char tmpbuf[DCVS_TRACE_LEN];
	unsigned int cpu, cid;
	unsigned long flags;

	cid = get_cid_from_cpu(policy->cpu);

	/*
	 * Check if IPS sampling is enabled.
	 * If sampling is disabled, reject starting the governor since
	 * there won't be any PMU data for frequency calculation.
	 */
	if (!ips_is_sampling_enabled()) {
		pr_err("ips_gov: start failed, IPS sampling not enabled\n");
		return -ENODEV;
	}

	/*
	 * Check if PMU is valid for this cluster.
	 * If any CPU in this cluster failed PMU initialization,
	 * reject starting the governor to prevent invalid frequency voting.
	 */
	if (!qcom_pmu_cluster_valid(cid)) {
		pr_err("ips_gov: start failed, cluster %u PMU not valid\n", cid);
		return -ENODEV;
	}

	ig_policy->next_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct ips_gov_cpu *ig_cpu = &per_cpu(ips_gov_cpu, cpu);

		memset(ig_cpu, 0, sizeof(*ig_cpu));
		ig_cpu->cpu = cpu;
		/*
		 * Publish ig_policy pointer with proper memory barrier.
		 * rcu_assign_pointer ensures all prior writes to ig_policy
		 * are visible before the pointer is published to RCU readers.
		 */
		rcu_assign_pointer(ig_cpu->ig_policy, ig_policy);
	}

	/* Enable voting - protected by ips_vote_cpufreq_lock */
	spin_lock_irqsave(&ips_vote_cpufreq_lock, flags);
	ips_vote_cpufreq[cid] = true;
	spin_unlock_irqrestore(&ips_vote_cpufreq_lock, flags);

	/* Trace governor start for each cluster - always enabled for debug */
	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%u_ips_gov_start", cid);
	DCVS_ATRACE_N2("ips_gov", tmpbuf);

	/* Per-cluster runtime statistics */
	if (cid < ARCH_CLUSTER_NUM) {
		ips_gov_cstats[cid].start_count++;
		ips_gov_cstats[cid].last_start_jiffies = jiffies;
		ips_gov_cstats[cid].is_active = true;
	}

	pr_info("ips_gov: start: cluster_id=%u, %*pbl, online:%*pbl\n",
	       cid,
	       cpumask_pr_args(policy->related_cpus),
	       cpumask_pr_args(policy->cpus));
	return 0;
}

static void ipsgov_stop(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy = policy->governor_data;
	char tmpbuf[DCVS_TRACE_LEN];
	unsigned int cpu, cid;
	unsigned long flags;

	cid = get_cid_from_cpu(policy->cpu);

	/*
	 * Step 1: Clear ips_vote_cpufreq to prevent new calls to
	 * ips_gov_update_cpufreq from ips_core.c
	 * Protected by ips_vote_cpufreq_lock
	 */
	spin_lock_irqsave(&ips_vote_cpufreq_lock, flags);
	ips_vote_cpufreq[cid] = false;
	spin_unlock_irqrestore(&ips_vote_cpufreq_lock, flags);

	/*
	 * Step 2: Clear per-CPU ig_policy references.
	 * This prevents ips_gov_update_cpufreq from accessing ig_policy
	 * after stop. Combined with RCU protection:
	 * - RCU_INIT_POINTER clears the pointer (no barrier needed for NULL)
	 * - rcu_dereference in readers pairs with rcu_assign_pointer in start
	 * - synchronize_rcu() in ipsgov_exit ensures all RCU readers complete
	 *   before ig_policy is freed
	 *
	 * ig_policy is guaranteed valid here (allocated in init, freed in exit).
	 */
	for_each_cpu(cpu, ig_policy->policy->cpus) {
		struct ips_gov_cpu *ig_cpu = &per_cpu(ips_gov_cpu, cpu);

		RCU_INIT_POINTER(ig_cpu->ig_policy, NULL);
	}

	/* Trace governor stop for each cluster - always enabled for debug */
	scnprintf(tmpbuf, DCVS_TRACE_LEN, "c%u_ips_gov_stop", cid);
	DCVS_ATRACE_N2("ips_gov", tmpbuf);

	/* Per-cluster runtime statistics */
	if (cid < ARCH_CLUSTER_NUM && ips_gov_cstats[cid].is_active) {
		u64 duration_ms;

		ips_gov_cstats[cid].stop_count++;
		duration_ms = jiffies_to_msecs(jiffies - ips_gov_cstats[cid].last_start_jiffies);
		ips_gov_cstats[cid].total_time_ms += duration_ms;
		ips_gov_cstats[cid].is_active = false;
	}

	pr_info("ips_gov: stop: cluster_id=%u, %*pbl, online:%*pbl\n",
	       cid,
	       cpumask_pr_args(policy->related_cpus),
	       cpumask_pr_args(policy->cpus));
}

static void ipsgov_limits(struct cpufreq_policy *policy)
{
	struct ips_gov_policy *ig_policy;
	unsigned long flags;
	unsigned int freq, final_freq, cid;

	if (!policy || !policy->governor_data)
		return;

	ig_policy = policy->governor_data;
	raw_spin_lock_irqsave(&ig_policy->update_lock, flags);

	freq = ig_policy->next_freq;
	final_freq = get_next_freq(policy, freq);
	if (final_freq != freq)
		ig_policy->next_freq = cpufreq_driver_fast_switch(policy, final_freq);

	raw_spin_unlock_irqrestore(&ig_policy->update_lock, flags);

	cid = get_cid_from_cpu(policy->cpu);
	pr_debug("ips_gov: limits: cluster_id=%u, %*pbl, online:%*pbl\n",
		 cid,
		 cpumask_pr_args(policy->related_cpus),
		 cpumask_pr_args(policy->cpus));
}

static struct cpufreq_governor cpufreq_ips_gov = {
	.name		= IPS_CPUFREQ_GOV_NAME,
	.owner		= THIS_MODULE,
	.flags		= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init		= ipsgov_init,
	.exit		= ipsgov_exit,
	.start		= ipsgov_start,
	.stop		= ipsgov_stop,
	.limits		= ipsgov_limits,
};

int ipsgov_register(void)
{
	return cpufreq_register_governor(&cpufreq_ips_gov);
}

void ipsgov_unregister(void)
{
	cpufreq_unregister_governor(&cpufreq_ips_gov);
	ips_gov_clear_cached_tunables();
}
