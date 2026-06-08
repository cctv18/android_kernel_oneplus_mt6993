// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */
#define pr_fmt(fmt) "oplus-pmu: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/perf_event.h>
#include <linux/cpuidle.h>
#include <trace/events/power.h>
#include <trace/hooks/cpuidle.h>
#include <linux/tracepoint.h>
#include <linux/smp.h>
#include <linux/perf/arm_pmu.h>
#include "oplus_pmu.h"

#define MAX_PMU_EVS	ARCH_PMU_MAX_EVS
#define INVALID_ID	0xFF
#define PMU_MAX_CLUSTER	4  /* Maximum number of CPU clusters */

struct event_data {
	u32			event_id;
	struct perf_event	*pevent;
	int			cpu;
	u64			cached_count;
	enum amu_counters	amu_id;
	enum cpucp_ev_idx	cid;
};

struct amu_data {
	enum amu_counters	amu_id;
	u64			count;
};

struct cpu_data {
	bool			is_idle;
	bool			is_hp;
	bool			is_pc;
	struct event_data	events[MAX_PMU_EVS];
	u32			num_evs;
	u32			num_amu;
	u32			num_pmu;
	atomic_t		read_cnt;
	spinlock_t		read_lock;
};

static DEFINE_PER_CPU(struct cpu_data *, cpu_ev_data);
static bool qcom_pmu_inited;
static bool pmu_long_counter;
static int cpuhp_state;
static LIST_HEAD(idle_notif_list);
static DEFINE_SPINLOCK(idle_list_lock);

/*-------------PMU error statistics-------------------*/
/*
 * Only track runtime errors (hotplug).
 * Init errors are logged via pr_err and will cause module load failure.
 */
static u64 pmu_err_hotplug;		/* Hotplug event setup failures (runtime) */
static u64 pmu_err_hotplug_jiffies;	/* Timestamp of last hotplug error */
static int pmu_err_last_cpu = -1;	/* CPU that last failed */
static u32 pmu_err_last_event;		/* Event ID that last failed */
static int pmu_err_last_ret;		/* Error code of last failure */

/*-------------Fault injection for testing-------------------*/
#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
/*
 * pmu_fault_inject_cpu: CPU ID to inject hotplug failure
 *   -1: disabled (default)
 *   >=0: inject failure on this CPU during hotplug
 * After injection, the flag is automatically cleared.
 */
static int pmu_fault_inject_cpu = -1;
#endif

/*
 * Per-cluster PMU tracking:
 * - pmu_cluster_inited: true if cluster was successfully initialized at probe
 * - pmu_cluster_failed_cpus: cpumask of CPUs with PMU setup failure
 * Cluster is valid if: inited && failed_cpus is empty
 */
static bool pmu_cluster_inited[PMU_MAX_CLUSTER] = {false};
static cpumask_t pmu_cluster_failed_cpus[PMU_MAX_CLUSTER];
/* platform dependent; sm8750/sm8850: 13,4 */
static u32 max_online_pmu = 13;
static u32 max_online_amu = 4;

/*
 * is_amu_valid: Check if AMUs are supported and if the id corresponds to the
 * four supported AMU counters i.e. SYS_AMEVCNTR0_CONST_EL0,
 * SYS_AMEVCNTR0_CORE_EL0, SYS_AMEVCNTR0_INST_RET_EL0, SYS_AMEVCNTR0_MEM_STALL
 */
static inline bool is_amu_valid(enum amu_counters amu_id)
{
	return (amu_id >= SYS_AMU_CONST_CYC && amu_id < SYS_AMU_MAX &&
		IS_ENABLED(CONFIG_ARM64_AMU_EXTN));
}

/*
 * is_cid_valid: Check if events are supported and if the id corresponds to the
 * supported CPUCP events i.e. enum cpucp_ev_idx,
 */
static inline bool is_cid_valid(enum cpucp_ev_idx cid)
{
	return (cid >= CPU_CYC_EVT && cid < MAX_CPUCP_EVT);
}

/*
 * is_event_valid: Check if event has an id and a corresponding pevent or
 * valid amu id.
 */
static inline bool is_event_valid(struct event_data *ev)
{
	return (ev->event_id && (ev->pevent || is_amu_valid(ev->amu_id)));
}

/*
 * is_event_shared: Check if event is supposed to be shared with cpucp.
 */
static inline bool is_event_shared(struct event_data *ev)
{
	return is_cid_valid(ev->cid);
}

#define CYCLE_COUNTER_ID 0x11
static inline u64 cached_count_value(struct event_data *ev, u64 event_cached_count, bool amu)
{
	struct arm_pmu *cpu_pmu = container_of(ev->pevent->pmu, struct arm_pmu, pmu);

	if (amu)
		return event_cached_count;

	if (cpu_pmu->pmuver >= ID_AA64DFR0_EL1_PMUVer_V3P5)
		event_cached_count |= (pmu_long_counter ? BIT(63) : GENMASK(63, 31));
	else {
		if (ev->event_id == CYCLE_COUNTER_ID)
			event_cached_count |= GENMASK(63, 31);
		else
			event_cached_count = ((event_cached_count & GENMASK(31, 0)) |
						BIT(31));
	}

	return event_cached_count;
}

static struct perf_event_attr *alloc_attr(void)
{
	struct perf_event_attr *attr;

	attr = kzalloc(sizeof(struct perf_event_attr), GFP_KERNEL);
	if (!attr)
		return attr;

	attr->size = sizeof(struct perf_event_attr);
	attr->pinned = 1;

	return attr;
}

static int set_event(struct event_data *ev, int cpu,
		     struct perf_event_attr *attr)
{
	struct perf_event *pevent;
	u32 type = PERF_TYPE_RAW;

	/* Set the cpu and exit if amu is supported */
	if (is_amu_valid(ev->amu_id))
		goto set_cpu;
	else
		ev->amu_id = SYS_AMU_MAX;

	if (!is_cid_valid(ev->cid))
		ev->cid = MAX_CPUCP_EVT;

	if (!ev->event_id)
		return 0;

	attr->config = ev->event_id;
	/* enable 64-bit counter */
	if (pmu_long_counter)
		attr->config1 = 1;

	attr->type = type;
	pevent = perf_event_create_kernel_counter(attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(pevent)) {
		pr_err("pmu set_event failed! event_id = 0x%x\n", ev->event_id);
		return PTR_ERR(pevent);
	}

	perf_event_enable(pevent);
	ev->pevent = pevent;
set_cpu:
	ev->cpu = cpu;

	return 0;
}

static inline void delete_event(struct event_data *event)
{
	if (event->pevent) {
		perf_event_release_kernel(event->pevent);
		event->pevent = NULL;
	}
}

static void read_amu_reg(void *amu_data)
{
	struct amu_data *data = amu_data;

	switch (data->amu_id) {
	case SYS_AMU_CONST_CYC:
		data->count = read_sysreg_s(SYS_AMEVCNTR0_CONST_EL0);
		break;
	case SYS_AMU_CORE_CYC:
		data->count = read_sysreg_s(SYS_AMEVCNTR0_CORE_EL0);
		break;
	case SYS_AMU_INST_RET:
		data->count = read_sysreg_s(SYS_AMEVCNTR0_INST_RET_EL0);
		break;
	case SYS_AMU_STALL_MEM:
		data->count = read_sysreg_s(SYS_AMEVCNTR0_MEM_STALL);
		break;
	default:
		pr_err("AMU counter %d not supported!\n", data->amu_id);
	}
}

static inline u64 read_event(struct event_data *event, bool local)
{
	u64 enabled, running, total = 0;
	struct amu_data data;
	int ret = 0;

	if (is_amu_valid(event->amu_id)) {
		data.amu_id = event->amu_id;
		if (local)
			read_amu_reg(&data);
		else {
			ret = smp_call_function_single(event->cpu, read_amu_reg,
							&data, true);
			if (ret < 0)
				return event->cached_count;
		}
		total = data.count;
	} else {
		if (!event->pevent)
			return event->cached_count;
		if (local)
			perf_event_read_local(event->pevent, &total, NULL, NULL);
		else
			total = perf_event_read_value(event->pevent, &enabled,
								&running);
	}
	event->cached_count = total;

	return total;
}

static int __qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data, bool local)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i;
	unsigned long flags;

	if (!qcom_pmu_inited)
		return -ENODEV;

	if (!event_id || !pmu_data || !cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);
	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (event->event_id == event_id)
			break;
	}
	if (i == cpu_data->num_evs)
		return -ENOENT;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_hp || cpu_data->is_idle || cpu_data->is_pc) {
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		*pmu_data = event->cached_count;
		return 0;
	}
	atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	*pmu_data = read_event(event, local);
	atomic_dec(&cpu_data->read_cnt);

	return 0;
}

int __qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data, bool local)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i, cnt = 0;
	bool use_cache = false;
	unsigned long flags;

	if (!qcom_pmu_inited)
		return -ENODEV;

	if (!data || !cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);
	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_hp || cpu_data->is_idle || cpu_data->is_pc)
		use_cache = true;
	else
		atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);

	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (!event->event_id)
			continue;
		data->event_ids[cnt] = event->event_id;
		if (use_cache)
			data->ev_data[cnt] = event->cached_count;
		else
			data->ev_data[cnt] = read_event(event, local);
		cnt++;
	}
	data->num_evs = cnt;

	if (!use_cache)
		atomic_dec(&cpu_data->read_cnt);

	return 0;
}

static struct event_data *get_event(u32 event_id, int cpu)
{
	struct cpu_data *cpu_data;
	struct event_data *event;
	int i;

	if (!qcom_pmu_inited)
		return ERR_PTR(-EPROBE_DEFER);

	if (!event_id || !cpumask_test_cpu(cpu, cpu_possible_mask))
		return ERR_PTR(-EINVAL);

	cpu_data = per_cpu(cpu_ev_data, cpu);
	for (i = 0; i < cpu_data->num_evs; i++) {
		event = &cpu_data->events[i];
		if (event->event_id == event_id)
			return event;
	}
	return ERR_PTR(-ENOENT);
}

int qcom_pmu_event_supported(u32 event_id, int cpu)
{
	struct event_data *event;

	event = get_event(event_id, cpu);

	return PTR_ERR_OR_ZERO(event);
}
EXPORT_SYMBOL_GPL(qcom_pmu_event_supported);

int qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data)
{
	return __qcom_pmu_read(cpu, event_id, pmu_data, false);
}
EXPORT_SYMBOL_GPL(qcom_pmu_read);

int qcom_pmu_read_local(u32 event_id, u64 *pmu_data)
{
	int this_cpu = smp_processor_id();

	return __qcom_pmu_read(this_cpu, event_id, pmu_data, true);
}
EXPORT_SYMBOL_GPL(qcom_pmu_read_local);

int qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data)
{
	return __qcom_pmu_read_all(cpu, data, false);
}
EXPORT_SYMBOL_GPL(qcom_pmu_read_all);

int qcom_pmu_read_all_local(struct qcom_pmu_data *data)
{
	int this_cpu = smp_processor_id();

	return __qcom_pmu_read_all(this_cpu, data, true);
}
EXPORT_SYMBOL_GPL(qcom_pmu_read_all_local);

int qcom_pmu_idle_register(struct qcom_pmu_notif_node *idle_node)
{
	struct qcom_pmu_notif_node *tmp_node;

	if (!idle_node || !idle_node->idle_cb)
		return -EINVAL;

	spin_lock(&idle_list_lock);
	list_for_each_entry(tmp_node, &idle_notif_list, node)
		if (tmp_node->idle_cb == idle_node->idle_cb)
			goto out;
	list_add_tail(&idle_node->node, &idle_notif_list);
out:
	spin_unlock(&idle_list_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(qcom_pmu_idle_register);

int qcom_pmu_idle_unregister(struct qcom_pmu_notif_node *idle_node)
{
	struct qcom_pmu_notif_node *tmp_node;
	int ret = -EINVAL;

	if (!idle_node || !idle_node->idle_cb)
		return ret;

	spin_lock(&idle_list_lock);
	list_for_each_entry(tmp_node, &idle_notif_list, node) {
		if (tmp_node->idle_cb == idle_node->idle_cb) {
			/*
			 * Use list_del_init instead of list_del to avoid LIST_POISON.
			 * This prevents crash if the node is accessed after removal
			 * due to race conditions during module unload.
			 */
			list_del_init(&tmp_node->node);
			ret = 0;
			break;
		}
	}
	spin_unlock(&idle_list_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_pmu_idle_unregister);

/**
 * qcom_pmu_cluster_valid - Check if a cluster's PMU is valid for IPS governor
 * @cluster_id: The cluster ID to check
 *
 * Returns true if:
 *   1. PMU is initialized
 *   2. Cluster was successfully initialized at probe
 *   3. No CPUs in this cluster have failed PMU setup (failed_cpus is empty)
 *
 * This allows recovery: if a CPU fails hotplug PMU setup, but later
 * succeeds on another hotplug, the cluster becomes valid again.
 */
bool qcom_pmu_cluster_valid(int cluster_id)
{
	if (!READ_ONCE(qcom_pmu_inited))
		return false;

	if (cluster_id < 0 || cluster_id >= PMU_MAX_CLUSTER)
		return false;

	/* Cluster must have been initialized at probe */
	if (!READ_ONCE(pmu_cluster_inited[cluster_id]))
		return false;

	/* Check if any CPU in this cluster has failed PMU setup */
	return cpumask_empty(&pmu_cluster_failed_cpus[cluster_id]);
}
EXPORT_SYMBOL_GPL(qcom_pmu_cluster_valid);

/**
 * qcom_pmu_is_healthy - Check if all PMU clusters are currently healthy
 *
 * Returns true if all initialized clusters have no failed CPUs.
 * This reflects the current real-time status, not historical errors.
 * Use this to determine if the IPS system is functioning normally.
 */
bool qcom_pmu_is_healthy(void)
{
	int i;

	if (!READ_ONCE(qcom_pmu_inited))
		return false;

	for (i = 0; i < PMU_MAX_CLUSTER; i++) {
		/* Only check clusters that were initialized */
		if (READ_ONCE(pmu_cluster_inited[i])) {
			if (!cpumask_empty(&pmu_cluster_failed_cpus[i]))
				return false;
		}
	}
	return true;
}
EXPORT_SYMBOL_GPL(qcom_pmu_is_healthy);

/**
 * qcom_pmu_get_hotplug_errors - Get PMU hotplug error statistics
 * @count: Output for error count (may be NULL)
 * @last_jiffies: Output for timestamp of last error (may be NULL)
 * @last_cpu: Output for CPU that last failed (may be NULL)
 * @last_event: Output for event ID that last failed (may be NULL)
 * @last_ret: Output for error code of last failure (may be NULL)
 *
 * Returns the number of PMU event setup failures during CPU hotplug.
 * This is the only runtime error tracked by the PMU module.
 */
void qcom_pmu_get_hotplug_errors(u64 *count, u64 *last_jiffies,
				 int *last_cpu, u32 *last_event, int *last_ret)
{
	if (count)
		*count = READ_ONCE(pmu_err_hotplug);
	if (last_jiffies)
		*last_jiffies = READ_ONCE(pmu_err_hotplug_jiffies);
	if (last_cpu)
		*last_cpu = READ_ONCE(pmu_err_last_cpu);
	if (last_event)
		*last_event = READ_ONCE(pmu_err_last_event);
	if (last_ret)
		*last_ret = READ_ONCE(pmu_err_last_ret);
}
EXPORT_SYMBOL_GPL(qcom_pmu_get_hotplug_errors);

#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
/**
 * qcom_pmu_set_fault_inject - Set CPU for hotplug fault injection
 * @cpu: CPU ID to inject fault (-1 to disable)
 *
 * When set, the next hotplug coming_up for this CPU will fail.
 * The flag is automatically cleared after injection.
 */
void qcom_pmu_set_fault_inject(int cpu)
{
	WRITE_ONCE(pmu_fault_inject_cpu, cpu);
	if (cpu >= 0)
		pr_info("pmu: fault injection armed for cpu %d\n", cpu);
	else
		pr_info("pmu: fault injection disabled\n");
}
EXPORT_SYMBOL_GPL(qcom_pmu_set_fault_inject);

/**
 * qcom_pmu_get_fault_inject - Get current fault injection CPU
 *
 * Returns the CPU ID set for fault injection, or -1 if disabled.
 */
int qcom_pmu_get_fault_inject(void)
{
	return READ_ONCE(pmu_fault_inject_cpu);
}
EXPORT_SYMBOL_GPL(qcom_pmu_get_fault_inject);
#endif /* CONFIG_OPLUS_IPS_INJECT_TEST */

static void qcom_pmu_idle_enter_notif(void *unused, int *state,
				      struct cpuidle_device *dev)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, dev->cpu);
	struct qcom_pmu_data pmu_data;
	struct event_data *ev;
	struct qcom_pmu_notif_node *idle_node;
	int i, cnt = 0;
	unsigned long flags;

	/* Safety check: cpu_data may be NULL during module unload */
	if (unlikely(!cpu_data))
		return;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_idle || cpu_data->is_hp || cpu_data->is_pc) {
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		return;
	}
	cpu_data->is_idle = true;
	atomic_inc(&cpu_data->read_cnt);
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	for (i = 0; i < cpu_data->num_evs; i++) {
		ev = &cpu_data->events[i];
		if (!is_event_valid(ev))
			continue;
		ev->cached_count = read_event(ev, true);
		pmu_data.event_ids[cnt] = ev->event_id;
		pmu_data.ev_data[cnt] = ev->cached_count;
		cnt++;
	}
	atomic_dec(&cpu_data->read_cnt);
	pmu_data.num_evs = cnt;

	/*
	 * Send snapshot of pmu data to all registered idle clients.
	 * Must hold idle_list_lock to prevent race with unregister.
	 * Use spin_lock (not irqsave) since we're already in interrupt context.
	 *
	 * Use list_for_each_entry_safe to handle the case where a node might
	 * be removed during iteration (although we hold the lock, this adds
	 * extra safety during module unload scenarios).
	 */
	spin_lock(&idle_list_lock);
	if (!list_empty(&idle_notif_list)) {
		struct qcom_pmu_notif_node *tmp;

		list_for_each_entry_safe(idle_node, tmp, &idle_notif_list, node) {
			if (idle_node->idle_cb)
				idle_node->idle_cb(&pmu_data, dev->cpu, *state);
		}
	}
	spin_unlock(&idle_list_lock);
}

static void qcom_pmu_idle_exit_notif(void *unused, int state,
				     struct cpuidle_device *dev)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, dev->cpu);
	unsigned long flags;

	/* Safety check: cpu_data may be NULL during module unload */
	if (unlikely(!cpu_data))
		return;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	cpu_data->is_idle = false;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
}

static int memlat_pm_notif(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	int cpu = smp_processor_id();
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, cpu);
	struct event_data *ev;
	int i, cid, aid;
	u32 count;
	bool read_ev  = true;
	unsigned long flags;

	/* Safety check: cpu_data may be NULL during module unload */
	if (unlikely(!cpu_data))
		return NOTIFY_OK;

	/* Exit if cpu is in hotplug */
	spin_lock_irqsave(&cpu_data->read_lock, flags);
	if (cpu_data->is_hp) {
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		return NOTIFY_OK;
	}

	if (action == CPU_PM_EXIT) {
		cpu_data->is_pc = false;
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
		return NOTIFY_OK;
	}

	if (cpu_data->is_idle || cpu_data->is_pc)
		read_ev = false;
	else
		atomic_inc(&cpu_data->read_cnt);
	cpu_data->is_pc = true;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);

	for (i = 0; i < cpu_data->num_evs; i++) {
		ev = &cpu_data->events[i];
		cid = ev->cid;
		aid = ev->amu_id;
		if (!is_event_valid(ev) || !is_event_shared(ev))
			continue;
		if (read_ev)
			ev->cached_count = read_event(ev, true);
		count = cached_count_value(ev, ev->cached_count, is_amu_valid(aid));
	}

	if (read_ev)
		atomic_dec(&cpu_data->read_cnt);

	return NOTIFY_OK;
}

static struct notifier_block memlat_event_pm_nb = {
	.notifier_call = memlat_pm_notif,
};

#if IS_ENABLED(CONFIG_HOTPLUG_CPU)
static int qcom_pmu_hotplug_coming_up(unsigned int cpu)
{
	struct perf_event_attr *attr = alloc_attr();
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, cpu);
	int i, ret = 0;
	unsigned long flags;
	struct event_data *ev;
	cpumask_t mask;
	int attempt;
	int cluster_id;
	bool has_failure = false;

	if (!attr)
		return -ENOMEM;

	if (!qcom_pmu_inited || !cpu_data)
		goto out;

	cluster_id = topology_cluster_id(cpu);
	if (cluster_id < 0 || cluster_id >= PMU_MAX_CLUSTER)
		cluster_id = 0;

#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
	/* Fault injection for testing */
	if (READ_ONCE(pmu_fault_inject_cpu) == cpu) {
		pr_warn("pmu: FAULT INJECTED for cpu %d hotplug\n", cpu);
		WRITE_ONCE(pmu_fault_inject_cpu, -1);  /* Auto-clear after injection */
		has_failure = true;
		goto fault_injected;
	}
#endif

	for (i = 0; i < cpu_data->num_evs; i++) {
		ev = &cpu_data->events[i];
		/* hotplug callback do a quick retry if failed */
		for (attempt = 0; attempt < 3; attempt++) {
			ret = set_event(ev, cpu, attr);
			if (ret == 0)
				break;
			udelay(20);
			pr_warn("hotplug cpu_up: event %d set for cpu %d failed, attempt %d, ret %d\n",
				ev->event_id, cpu, attempt, ret);
		}
		if (ret < 0) {
			pr_err("hotplug cpu_up: event 0x%x set for cpu %d failed, ret %d\n",
			       ev->event_id, cpu, ret);
			/* Record failure details for debug */
			WRITE_ONCE(pmu_err_last_cpu, cpu);
			WRITE_ONCE(pmu_err_last_event, ev->event_id);
			WRITE_ONCE(pmu_err_last_ret, ret);
			has_failure = true;
			break;
		}
	}

#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
fault_injected:
#endif
	/*
	 * Update cluster failed CPU mask based on hotplug result.
	 * - On failure: add CPU to failed mask
	 * - On success: remove CPU from failed mask (allows recovery)
	 * Cluster is valid when failed mask is empty.
	 */
	if (has_failure) {
		cpumask_set_cpu(cpu, &pmu_cluster_failed_cpus[cluster_id]);
		pr_warn("hotplug: cluster %d cpu %d PMU setup failed, failed_cpus=0x%*pbl\n",
			cluster_id, cpu,
			cpumask_pr_args(&pmu_cluster_failed_cpus[cluster_id]));
		/* Record runtime error count and timestamp */
		WRITE_ONCE(pmu_err_hotplug, READ_ONCE(pmu_err_hotplug) + 1);
		WRITE_ONCE(pmu_err_hotplug_jiffies, jiffies);
	} else {
		/* Success: clear this CPU from failed mask if it was there */
		if (cpumask_test_cpu(cpu, &pmu_cluster_failed_cpus[cluster_id])) {
			cpumask_clear_cpu(cpu, &pmu_cluster_failed_cpus[cluster_id]);
			pr_info("hotplug: cluster %d cpu %d PMU recovered, failed_cpus=0x%*pbl\n",
				cluster_id, cpu,
				cpumask_pr_args(&pmu_cluster_failed_cpus[cluster_id]));
		}
	}

	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	cpu_data->is_hp = false;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
out:
	kfree(attr);
	return 0;
}

static int qcom_pmu_hotplug_going_down(unsigned int cpu)
{
	struct cpu_data *cpu_data = per_cpu(cpu_ev_data, cpu);
	struct event_data *ev;
	int i, cid, aid;
	unsigned long flags;

	if (!qcom_pmu_inited || !cpu_data)
		return 0;

	spin_lock_irqsave(&cpu_data->read_lock, flags);
	cpu_data->is_hp = true;
	spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	while (atomic_read(&cpu_data->read_cnt) > 0)
		udelay(10);
	for (i = 0; i < cpu_data->num_evs; i++) {
		ev = &cpu_data->events[i];
		cid = ev->cid;
		aid = ev->amu_id;
		if (!is_event_valid(ev))
			continue;
		ev->cached_count = read_event(ev, false);
		delete_event(ev);
	}

	return 0;
}

static int qcom_pmu_cpu_hp_init(void)
{
	int ret;

	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN,
						"OPLUS_PMU",
						qcom_pmu_hotplug_coming_up,
						qcom_pmu_hotplug_going_down);
	if (ret < 0)
		pr_err("oplus_pmu: CPU hotplug notifier error: %d\n",
		       ret);

	return ret;
}
#else
static int qcom_pmu_cpu_hp_init(void) { return 0; }
#endif


static void delete_events(void)
{
	int i;
	unsigned int cpu;
	struct cpu_data *cpu_data;
	struct event_data *event;
	unsigned long flags;

	/* 1. remove CPU hotplug callback */
	if (cpuhp_state > 0) {
		cpuhp_remove_state_nocalls(cpuhp_state);
		cpuhp_state = 0;
	}

	/* 2. cancel trace hooks and notifier */
	unregister_trace_android_vh_cpu_idle_enter(qcom_pmu_idle_enter_notif, NULL);
	unregister_trace_android_vh_cpu_idle_exit(qcom_pmu_idle_exit_notif, NULL);
	cpu_pm_unregister_notifier(&memlat_event_pm_nb);

	/* 3. wait for all callbacks to complete */
	tracepoint_synchronize_unregister();

	/* 4. set flags to prevent new event reading */
	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		if (!cpu_data)
			continue;
		spin_lock_irqsave(&cpu_data->read_lock, flags);
		cpu_data->is_hp = true;
		cpu_data->is_idle = true;
		cpu_data->is_pc = true;
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	}

	/* 5. wait for ongoing reads to complete, then delete events */
	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		if (!cpu_data)
			continue;
		while (atomic_read(&cpu_data->read_cnt) > 0)
			udelay(10);
		for (i = 0; i < cpu_data->num_evs; i++) {
			event = &cpu_data->events[i];
			if (!is_event_valid(event))
				continue;
			delete_event(event);
		}
		/* 6. clear per-CPU pointer (memory is automatically released by devm) */
		per_cpu(cpu_ev_data, cpu) = NULL;
	}
}

static int setup_events(void)
{
	struct perf_event_attr *attr = alloc_attr();
	struct cpu_data *cpu_data;
	int i, ret = 0, cluster_id;
	unsigned int cpu;
	struct event_data *event;
	unsigned long flags;
	bool cluster_has_failure[PMU_MAX_CLUSTER] = {false};
	int cpu_event_count[PMU_MAX_CLUSTER] = {0};

	if (!attr)
		return -ENOMEM;

	/* Initialize all clusters as not initialized and clear failed masks */
	for (i = 0; i < PMU_MAX_CLUSTER; i++) {
		pmu_cluster_inited[i] = false;
		cpumask_clear(&pmu_cluster_failed_cpus[i]);
	}

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		cluster_id = topology_cluster_id(cpu);
		if (cluster_id < 0 || cluster_id >= PMU_MAX_CLUSTER)
			cluster_id = 0;

		for (i = 0; i < cpu_data->num_evs; i++) {
			event = &cpu_data->events[i];
			ret = set_event(event, cpu, attr);
			if (ret < 0) {
				pr_err("event %d not set for cpu %d ret %d\n",
					event->event_id, cpu, ret);
				event->event_id = 0;
				/* Mark this cluster as having a failure */
				cluster_has_failure[cluster_id] = true;
				/*
				 * Only return error for -EPROBE_DEFER. Clear
				 * ret for all other cases as it is okay for
				 * some events to fail.
				 */
				if (ret == -EPROBE_DEFER)
					goto cleanup_events;
				else
					ret = 0;
			} else {
				cpu_event_count[cluster_id]++;
			}
		}
		spin_lock_irqsave(&cpu_data->read_lock, flags);
		cpu_data->is_hp = !cpumask_test_cpu(cpu, cpu_online_mask);
		cpu_data->is_idle = false;
		cpu_data->is_pc = false;
		spin_unlock_irqrestore(&cpu_data->read_lock, flags);
	}

	/* Mark clusters as initialized if they have CPUs with events */
	for (i = 0; i < PMU_MAX_CLUSTER; i++) {
		if (cpu_event_count[i] > 0) {
			pmu_cluster_inited[i] = true;
			if (!cluster_has_failure[i]) {
				pr_info("cluster %d PMU initialized successfully\n", i);
			} else {
				pr_warn("cluster %d PMU has failures, IPS governor will reject\n", i);
			}
		}
	}

	cpuhp_state = qcom_pmu_cpu_hp_init();
	if (cpuhp_state < 0) {
		ret = cpuhp_state;
		pr_err("qcom pmu driver failed to initialize hotplug: %d\n", ret);
		goto out;
	}

	goto out;

cleanup_events:
	for_each_possible_cpu(cpu) {
		cpu_data = per_cpu(cpu_ev_data, cpu);
		for (i = 0; i < cpu_data->num_evs; i++) {
			event = &cpu_data->events[i];
			delete_event(event);
		}
	}
	/* Mark all clusters as not initialized on cleanup */
	for (i = 0; i < PMU_MAX_CLUSTER; i++)
		pmu_cluster_inited[i] = false;
out:
	cpus_read_unlock();
	if (ret != -EPROBE_DEFER && ret != cpuhp_state) {
		register_trace_android_vh_cpu_idle_enter(qcom_pmu_idle_enter_notif, NULL);
		register_trace_android_vh_cpu_idle_exit(qcom_pmu_idle_exit_notif, NULL);
		cpu_pm_register_notifier(&memlat_event_pm_nb);
	}
	kfree(attr);
	return ret;
}

static int configure_pmu_event(u32 event_id, int amu_id, int cid, int cpu)
{
	struct cpu_data *cpu_data;
	struct event_data *event;

	if (!event_id || !cpumask_test_cpu(cpu, cpu_possible_mask))
		return -EINVAL;

	cpu_data = per_cpu(cpu_ev_data, cpu);

	if (cpu_data->num_pmu >= max_online_pmu && !is_amu_valid(amu_id)) {
		pr_info("pmu overflow: cpu=%d, num_evs=%u, num_amu=%u, num_pmu=%u, event_id=0x%x, cid=0x%x\n",
			cpu,
			cpu_data->num_evs,
			cpu_data->num_amu,
			cpu_data->num_pmu,
			event_id, cid);
		return -ENOSPC;
	} else if (cpu_data->num_amu >= max_online_amu && is_amu_valid(amu_id)) {
		pr_info("amu overflow: cpu=%d, num_evs=%u, num_amu=%u, num_pmu=%u, event_id=0x%x, cid=0x%x\n",
			cpu,
			cpu_data->num_evs,
			cpu_data->num_amu,
			cpu_data->num_pmu,
			event_id, cid);
		return -ENOSPC;
	}

	event = &cpu_data->events[cpu_data->num_evs];
	event->event_id = event_id;
	event->amu_id = amu_id;
	event->cid = cid;
	cpu_data->num_evs++;
	if (is_amu_valid(amu_id))
		cpu_data->num_amu++;
	else
		cpu_data->num_pmu++;

	return 0;
}

/* keep consitent with dts */
#define PMU_LONG_COUNTER_PROP "oplus,arch-xmu-long-counter"
#define MAX_ONLINE_PMU_PROP "oplus,arch-max-online-pmu"
#define MAX_ONLINE_AMU_PROP "oplus,arch-max-online-amu"
#define PMU_TBL_PROP	"oplus,pmu-events-tbl"
#define NUM_COL		4
static int init_pmu_events(struct device *dev)
{
	struct device_node *of_node = dev->of_node;
	int ret, len, i, j, cpu;
	u32 data = 0, event_id, cid;
	unsigned long cpus;
	int amu_id;

	if (of_find_property(of_node, PMU_LONG_COUNTER_PROP, &len))
		pmu_long_counter = true;

	ret = of_property_read_u32(of_node, MAX_ONLINE_PMU_PROP, &data);
	if (ret < 0)
		pr_info("%s not set, use default(%u)\n", MAX_ONLINE_PMU_PROP,
			max_online_pmu);
	else
		max_online_pmu = data;

	ret = of_property_read_u32(of_node, MAX_ONLINE_AMU_PROP, &data);
	if (ret < 0)
		pr_info("%s not set, use default(%u)\n", MAX_ONLINE_AMU_PROP,
			max_online_amu);
	else
		max_online_amu = data;

	pr_info("max-online-pmu=%u, max-online-amu=%u\n",
		max_online_pmu,
		max_online_amu);

	if (!of_find_property(of_node, PMU_TBL_PROP, &len))
		return -ENODEV;
	len /= sizeof(data);
	if (len % NUM_COL || len == 0)
		return -EINVAL;
	len /= NUM_COL;

	for (i = 0, j = 0; i < len; i++, j += NUM_COL) {
		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j,
							&event_id);
		if (ret < 0 || !event_id)
			return -EINVAL;

		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j + 1,
							&data);
		if (ret < 0 || !data)
			return -EINVAL;
		cpus = (unsigned long)data;

		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j + 2,
							&amu_id);
		if (ret < 0)
			return -EINVAL;

		ret = of_property_read_u32_index(of_node, PMU_TBL_PROP, j + 3,
						 &cid);
		if (ret < 0)
			return -EINVAL;

		for_each_cpu(cpu, to_cpumask(&cpus)) {
			if (cpumask_test_cpu(cpu, cpu_possible_mask)) {
				ret = configure_pmu_event(event_id, amu_id, cid, cpu);
				if (ret < 0)
					return ret;
			}
		}

		dev_dbg(dev, "entry=%d: ev=%d, cpus=%lu cpucp id=%d amu_id=%d\n",
			i, event_id, cpus, cid, amu_id);
	}

	return 0;
}

static int oplus_pmu_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	unsigned int cpu;
	struct cpu_data *cpu_data;

	for_each_possible_cpu(cpu) {
		cpu_data = devm_kzalloc(dev, sizeof(*cpu_data), GFP_KERNEL);
		if (!cpu_data)
			return -ENOMEM;

		spin_lock_init(&cpu_data->read_lock);
		atomic_set(&cpu_data->read_cnt, 0);
		per_cpu(cpu_ev_data, cpu) = cpu_data;
	}

	ret = init_pmu_events(dev);
	if (ret < 0) {
		dev_err(dev, "failed to initialize pmu events: %d\n", ret);
		return ret;
	}

	ret = setup_events();
	if (ret < 0) {
		dev_err(dev, "failed to setup all pmu/amu events: %d\n", ret);
		return ret;
	}

	qcom_pmu_inited = true;

	return ret;
}

static void oplus_pmu_driver_remove(struct platform_device *pdev)
{
	int i;

	/* 1. set flags to prevent new operations */
	qcom_pmu_inited = false;

	/* 2. mark all clusters as not initialized */
	for (i = 0; i < PMU_MAX_CLUSTER; i++) {
		WRITE_ONCE(pmu_cluster_inited[i], false);
		cpumask_clear(&pmu_cluster_failed_cpus[i]);
	}

	/* 3. delete all events */
	delete_events();

	pr_info("oplus_pmu driver removed\n");
}

static const struct of_device_id pmu_match_table[] = {
	{.compatible = "oplus,pmu"},
	{}
};

static struct platform_driver oplus_pmu_driver = {
	.probe = oplus_pmu_driver_probe,
	.remove = oplus_pmu_driver_remove,
	.driver = {
		.name = "oplus-pmu",
		.of_match_table = pmu_match_table,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(oplus_pmu_driver);


MODULE_DESCRIPTION("OPLUS PMU Driver");
MODULE_LICENSE("GPL");
