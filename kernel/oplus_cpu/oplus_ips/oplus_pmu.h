/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Oplus. All rights reserved.
 */

#ifndef _OPLUS_PMU_H
#define _OPLUS_PMU_H

#include <linux/kernel.h>

/*
 * max online pmu + amu default set to 32 is enough.
 * platform limit is set through dtsi. for example:
 * sm8550: 7-(1) ccntr + (6) evcntr + (1) llcc
 * sm8650: 19
 * sm8750/sm8850: 13-(12) evcntr + (1) llcc
 * mt6993 DX-5: 32
 */
#define ARCH_PMU_MAX_EVS	32
#define INVALID_PMU_HW_IDX	0xFF

enum cpucp_ev_idx {
	CPU_CYC_EVT = 0,
	CNT_CYC_EVT,
	INST_RETIRED_EVT,
	STALL_BACKEND_EVT,
	L2D_CACHE_REFILL_EVT,
	L2D_WB_EVT,
	L3_CACHE_REFILL_EVT,
	L3_ACCESS_EVT,
	LLCC_CACHE_REFILL_EVT,
	MAX_CPUCP_EVT,
};

struct qcom_pmu_data {
	u32			event_ids[ARCH_PMU_MAX_EVS];
	u64			ev_data[ARCH_PMU_MAX_EVS];
	u32			num_evs;
};

typedef void (*idle_fn_t)(struct qcom_pmu_data *data, int cpu, int state);
struct qcom_pmu_notif_node {
	idle_fn_t		idle_cb;
	struct list_head	node;
};

enum amu_counters {
	SYS_AMU_CONST_CYC,
	SYS_AMU_CORE_CYC,
	SYS_AMU_INST_RET,
	SYS_AMU_STALL_MEM,
	SYS_AMU_MAX,
};

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_IPS)
int qcom_pmu_event_supported(u32 event_id, int cpu);
int qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data);
int qcom_pmu_read_local(u32 event_id, u64 *pmu_data);
int qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data);
int qcom_pmu_read_all_local(struct qcom_pmu_data *data);
int qcom_pmu_idle_register(struct qcom_pmu_notif_node *idle_node);
int qcom_pmu_idle_unregister(struct qcom_pmu_notif_node *idle_node);
bool qcom_pmu_cluster_valid(int cluster_id);
bool qcom_pmu_is_healthy(void);
void qcom_pmu_get_hotplug_errors(u64 *count, u64 *last_jiffies,
				 int *last_cpu, u32 *last_event, int *last_ret);
#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
void qcom_pmu_set_fault_inject(int cpu);
int qcom_pmu_get_fault_inject(void);
#endif
#else
static inline int qcom_pmu_event_supported(u32 event_id, int cpu)
{
	return -ENODEV;
}
static inline int qcom_pmu_read(int cpu, u32 event_id, u64 *pmu_data)
{
	return -ENODEV;
}
static inline int qcom_pmu_read_local(u32 event_id, u64 *pmu_data)
{
	return -ENODEV;
}
static inline int qcom_pmu_read_all(int cpu, struct qcom_pmu_data *data)
{
	return -ENODEV;
}
static inline int qcom_pmu_read_all_local(struct qcom_pmu_data *data)
{
	return -ENODEV;
}
static inline int qcom_pmu_idle_register(struct qcom_pmu_notif_node *idle_node)
{
	return -ENODEV;
}
static inline int qcom_pmu_idle_unregister(
					struct qcom_pmu_notif_node *idle_node)
{
	return -ENODEV;
}
static inline bool qcom_pmu_cluster_valid(int cluster_id)
{
	return false;
}
static inline bool qcom_pmu_is_healthy(void)
{
	return false;
}
static inline void qcom_pmu_get_hotplug_errors(u64 *count, u64 *last_jiffies,
					       int *last_cpu, u32 *last_event,
					       int *last_ret)
{
	if (count)
		*count = 0;
	if (last_jiffies)
		*last_jiffies = 0;
	if (last_cpu)
		*last_cpu = -1;
	if (last_event)
		*last_event = 0;
	if (last_ret)
		*last_ret = 0;
}
#if IS_ENABLED(CONFIG_OPLUS_IPS_INJECT_TEST)
static inline void qcom_pmu_set_fault_inject(int cpu) { }
static inline int qcom_pmu_get_fault_inject(void) { return -1; }
#endif
#endif

#endif /* _OPLUS_PMU_H */
