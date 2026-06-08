// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */
#include <linux/cpufreq.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/tracepoint.h>
#include <trace/hooks/power.h>
#include "ips_private.h"

#define IPS_OWNER_MOD_NAME  "[oplus_ips]"
#define IPS_OWNER_MOD_NAME_SIZE 8

/* per cpu freq qos start */
struct cpu_status {
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, qos_req_cpufreq);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_min);
static DEFINE_PER_CPU(struct freq_qos_request, qos_req_max);
/* flag to prevent callback execution during exit */
static bool freq_qos_stats_enabled = false;


/* list for client and ringbuffer for LRU */
DEFINE_SPINLOCK(freq_qos_client_lock);
u32 qos_client_list_num = 0;
u32 qos_stats_lru_num[2];
LIST_HEAD(qos_client_list);
/* Maximum number of QoS clients to prevent excessive memory usage */
#define IPS_FREQ_QOS_CLIENT_MAX 128
/*0: memlat; 1: other client */
struct freq_qos_stats ips_qos_stats_lru[2][IPS_FREQ_QOS_LRU_SIZE];


/*
 * qos stats related
 * only freq qos constraints can get cluster id.
 * filter other qos eg. dev_pm_qos
 */
static inline int ips_stats_qos_freq_verify(struct freq_constraints *qos)
{
	int i, cid = -1;

	if (!qos)
		return cid;

	/* verify freq qos constraint */
	for (i = 0; i < IPS_MAX_CLUSTER_NUM; i++) {
		if (qos == ips_qos_stats_cluster[i][0].qos) {
			cid = i;
			break;
		}
	}

	/*
	 * cid == -1 is normal for non-cpufreq qos (e.g. dev_pm_qos),
	 * only log at debug level to avoid log pollution.
	 */
	if (cid == -1)
		pr_debug("%s: non-cpufreq qos constraint(0x%8p), caller-(%ps).\n",
			 __func__, qos, (void *)__builtin_return_address(2));

	return cid;
}

static inline void ips_stats_qos_client_init(struct freq_qos_request *req,
					enum freq_qos_req_type type,
					int value,
					int cid,
					char src,
					u64 now,
					u64 stacktrace2,
					u64 stacktrace3,
					struct freq_qos_stats *qos_client)
{
	char mod_name[IPS_MODULE_NAME];
	char buf[IPS_MODULE_NAME];
	qos_client->cid = cid;
	qos_client->qos = req->qos;
	qos_client->req = req;
	qos_client->type = type;
	qos_client->value = value;
	qos_client->update_ts = now;
	qos_client->src = src;
	qos_client->count = 1;
	qos_client->pid = current->pid;
	strscpy_pad(qos_client->comm, current->comm, TASK_COMM_LEN);
	qos_client->stacktrace[2] = stacktrace2;
	qos_client->stacktrace[3] = stacktrace3;
	scnprintf(buf, sizeof(buf), "%ps", (void *)stacktrace2);
	if (sscanf(buf, "%*s%s", mod_name) != 1) {
		strscpy_pad(mod_name, buf, IPS_MODULE_NAME);
	}
	scnprintf(qos_client->tag, IPS_MODULE_NAME, "%s-c%d_%s", mod_name, cid, type == FREQ_QOS_MIN ? "min" : "max");
}

static inline void ips_stats_qos_lru_add(struct freq_qos_request *req,
					enum freq_qos_req_type type,
					int value,
					int cid,
					char src,
					u64 now,
					u64 count,
					u64 stacktrace2,
					u64 stacktrace3)
{
	unsigned int cpu, idx = 1, i;
	struct freq_qos_request *min_req, *max_req;

	/* TODO: optimize to 2 round */
	for_each_present_cpu(cpu) {
		min_req = &per_cpu(qos_req_min, cpu);
		if (min_req == req) {
			idx = 0;
			break;
		}

		max_req = &per_cpu(qos_req_max, cpu);
		if (max_req == req) {
			idx = 0;
			break;
		}
	}

	/* add to LRU list */
	i = qos_stats_lru_num[idx] % IPS_FREQ_QOS_LRU_SIZE;
	ips_qos_stats_lru[idx][i].cid = cid;
	ips_qos_stats_lru[idx][i].qos = req->qos;
	ips_qos_stats_lru[idx][i].req = req;
	ips_qos_stats_lru[idx][i].type = type;
	ips_qos_stats_lru[idx][i].value = value;
	ips_qos_stats_lru[idx][i].update_ts = now;
	ips_qos_stats_lru[idx][i].src = src;
	ips_qos_stats_lru[idx][i].count = count;
	ips_qos_stats_lru[idx][i].pid = current->pid;
	strscpy_pad(ips_qos_stats_lru[idx][i].comm, current->comm, TASK_COMM_LEN);
	ips_qos_stats_lru[idx][i].stacktrace[2] = stacktrace2;
	ips_qos_stats_lru[idx][i].stacktrace[3] = stacktrace3;
	qos_stats_lru_num[idx]++;
}

/* freq qos add cb */
static void ips_stats_freq_qos_add(void *unused, struct freq_constraints *qos,
				      struct freq_qos_request *req,
				      enum freq_qos_req_type type,
				      int value, int ret)
{
	int cid;
	unsigned long flags;
	struct freq_qos_stats *qos_client;
	u64 now = ktime_to_ms(ktime_get());
	u64 stacktrace2 = (unsigned long)__builtin_return_address(2);
	u64 stacktrace3 = (unsigned long)__builtin_return_address(3);

	if (!freq_qos_stats_enabled)
		return;

	cid = ips_stats_qos_freq_verify(qos);
	if (cid == -1)
		return;

	qos_client = kzalloc(sizeof(struct freq_qos_stats), GFP_ATOMIC);
	if (qos_client != NULL) {
		ips_stats_qos_client_init(req, type, value, cid, 'a', now,
					  stacktrace2, stacktrace3, qos_client);
	}

	spin_lock_irqsave(&freq_qos_client_lock, flags);
	if (qos_client != NULL) {
		/* Double-check: if android_oem_data1 was already set by another
		 * thread (e.g., concurrent update), free our allocation to avoid
		 * memory leak and orphaned list nodes.
		 */
		if (req->android_oem_data1 != 0) {
			spin_unlock_irqrestore(&freq_qos_client_lock, flags);
			kfree(qos_client);
			return;
		}
		/* Check if we've reached the maximum client limit */
		if (qos_client_list_num >= IPS_FREQ_QOS_CLIENT_MAX) {
			/* Ensure oem_data is NULL so update/remove know no tracking */
			req->android_oem_data1 = 0;
			spin_unlock_irqrestore(&freq_qos_client_lock, flags);
			kfree(qos_client);
			ips_record_error(IPS_ERR_QOS_LIST_FULL);
			return;
		}
		/* add to client list */
		list_add_tail(&qos_client->client_list, &qos_client_list);
		qos_client_list_num++;
		req->android_oem_data1 = (u64)qos_client;
	}
	ips_stats_qos_lru_add(req, type, value, cid, 'a', now, 1, stacktrace2, stacktrace3);
	spin_unlock_irqrestore(&freq_qos_client_lock, flags);
}

/* freq qos remove cb */
static void ips_stats_freq_qos_remove(void *unused, struct freq_qos_request *req)
{
	int cid;
	unsigned long flags;
	struct freq_qos_stats *qos_client;
	struct freq_constraints *qos = req->qos;
	u64 now = ktime_to_ms(ktime_get());
	u64 stacktrace2 = (unsigned long)__builtin_return_address(2);
	u64 stacktrace3 = (unsigned long)__builtin_return_address(3);

	if (!freq_qos_stats_enabled)
		return;

	cid = ips_stats_qos_freq_verify(qos);
	if (cid == -1)
		return;

	spin_lock_irqsave(&freq_qos_client_lock, flags);

	/* read android_oem_data1 under lock to avoid race with exit */
	qos_client = (struct freq_qos_stats *)req->android_oem_data1;

	ips_stats_qos_lru_add(req, req->type, req->pnode.prio, cid, 'r', now,
			       qos_client != NULL ? qos_client->count++ : 1,
			       stacktrace2, stacktrace3);
	if (!qos_client) {
		spin_unlock_irqrestore(&freq_qos_client_lock, flags);
		return;
	}

	/* remove from client list */
	list_del_init(&qos_client->client_list);
	qos_client_list_num--;
	req->android_oem_data1 = 0;
	spin_unlock_irqrestore(&freq_qos_client_lock, flags);

	/* free outside lock since kfree doesn't need protection */
	kfree(qos_client);
}

static void ips_stats_freq_qos_update(void *unused, struct freq_qos_request
				      *req, int new_value)
{
	int cid, tmp_value = new_value;
	unsigned long flags;
	bool new_alloc = false;
	u64 now = ktime_to_ms(ktime_get());
	struct freq_constraints *qos = req->qos;
	struct freq_qos_stats *qos_client;
	struct freq_qos_stats *new_client = NULL;
	u64 stacktrace2 = (unsigned long)__builtin_return_address(2);
	u64 stacktrace3 = (unsigned long)__builtin_return_address(3);

	if (!freq_qos_stats_enabled)
		return;

	cid = ips_stats_qos_freq_verify(qos);
	if (cid == -1)
		return;

	/* read android_oem_data1 outside lock first */
	qos_client = (struct freq_qos_stats *)req->android_oem_data1;

	if (!qos_client) {
		/* oem data is null, try to create a node */
		new_client = kzalloc(sizeof(struct freq_qos_stats), GFP_ATOMIC);
		if (new_client != NULL) {
			ips_stats_qos_client_init(req, req->type, new_value,
						  cid, 'u', now,
						  stacktrace2, stacktrace3,
						  new_client);
		}
	}

	spin_lock_irqsave(&freq_qos_client_lock, flags);

	/* re-check android_oem_data1 under lock to avoid race */
	qos_client = (struct freq_qos_stats *)req->android_oem_data1;

	if (!qos_client && new_client) {
		/* Check if we've reached the maximum client limit */
		if (qos_client_list_num >= IPS_FREQ_QOS_CLIENT_MAX) {
			spin_unlock_irqrestore(&freq_qos_client_lock, flags);
			kfree(new_client);
			ips_record_error(IPS_ERR_QOS_LIST_FULL);
			return;
		}
		/* still null, use our new allocation */
		qos_client = new_client;
		new_client = NULL;  /* mark as used, don't free later */
		new_alloc = true;
		list_add_tail(&qos_client->client_list, &qos_client_list);
		qos_client_list_num++;
		req->android_oem_data1 = (u64)qos_client;
	}
	/* Note: if new_client is still non-NULL here, someone else already
	 * allocated qos_client. We'll free our duplicate after releasing lock. */

	if (qos_client != NULL && !new_alloc) {
		/* already exist node, update it */
		qos_client->src = 'u';
		qos_client->count++;
		qos_client->update_ts = now;
		qos_client->value = new_value;
		qos_client->pid = current->pid;
		strscpy_pad(qos_client->comm, current->comm, TASK_COMM_LEN);
		qos_client->stacktrace[2] = stacktrace2;
		qos_client->stacktrace[3] = stacktrace3;
		/* better show in perfetto */
		if (tmp_value == FREQ_QOS_MAX_DEFAULT_VALUE)
			tmp_value = ips_cluster_max_freq[cid] + 1000000;
		DCVS_ATRACE_C(qos_client->tag, tmp_value);
		if (0 != strncmp(qos_client->tag, IPS_OWNER_MOD_NAME, IPS_OWNER_MOD_NAME_SIZE)
		    && NULL == strstr(qos_client->comm, "kworker")) {
			DCVS_ATRACE_C("freq_qos_client", qos_client->pid);
			DCVS_ATRACE_F(qos_client->comm, qos_client->pid);
			DCVS_ATRACE_S(qos_client->comm, qos_client->pid);
		}
	}

	/* add to LRU list */
	ips_stats_qos_lru_add(req, req->type, new_value, cid, 'u', now,
			       qos_client != NULL ? qos_client->count : 1,
			       stacktrace2, stacktrace3);
	spin_unlock_irqrestore(&freq_qos_client_lock, flags);

	/* Free duplicate allocation outside lock (if someone else won the race) */
	if (new_client)
		kfree(new_client);
}


int freq_qos_request_init(void)
{
	unsigned int cpu;
	int ret;
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n",
				__func__, cpu);
			ret = -EAGAIN;
			goto cleanup;
		}
		per_cpu(qos_req_cpufreq, cpu).min = 0;
		req = &per_cpu(qos_req_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(qos_req_cpufreq, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		req = &per_cpu(qos_req_max, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
			FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add max freq constraint (%d)\n",
				__func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		cpufreq_cpu_put(policy);
	}

	/* qos client stats - register trace hooks */
	ret = register_trace_android_vh_freq_qos_add_request(ips_stats_freq_qos_add, NULL);
	if (ret) {
		pr_warn("%s: Failed to register freq_qos_add trace hook (%d)\n",
			__func__, ret);
	}
	ret = register_trace_android_vh_freq_qos_remove_request(ips_stats_freq_qos_remove, NULL);
	if (ret) {
		pr_warn("%s: Failed to register freq_qos_remove trace hook (%d)\n",
			__func__, ret);
	}
	ret = register_trace_android_vh_freq_qos_update_request(ips_stats_freq_qos_update, NULL);
	if (ret) {
		pr_warn("%s: Failed to register freq_qos_update trace hook (%d)\n",
			__func__, ret);
	}

	/* enable callbacks after everything is ready */
	freq_qos_stats_enabled = true;
	return 0;

cleanup:
	for_each_present_cpu(cpu) {
		req = &per_cpu(qos_req_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		req = &per_cpu(qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		per_cpu(qos_req_cpufreq, cpu).min = 0;
		per_cpu(qos_req_cpufreq, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	return ret;
}

void freq_qos_request_exit(void)
{
	unsigned int cpu;
	unsigned long flags;
	struct freq_qos_request *req;
	struct freq_qos_stats *qos_client, *tmp;

	/* 1. disable callbacks first to prevent any new operations */
	freq_qos_stats_enabled = false;

	/* 2. unregister trace hooks */
	unregister_trace_android_vh_freq_qos_add_request(ips_stats_freq_qos_add, NULL);
	unregister_trace_android_vh_freq_qos_remove_request(ips_stats_freq_qos_remove, NULL);
	unregister_trace_android_vh_freq_qos_update_request(ips_stats_freq_qos_update, NULL);

	/*
	 * 3. Wait for any in-flight trace callbacks to complete.
	 * This is critical to prevent use-after-free when callbacks
	 * access qos_client data that we're about to free.
	 */
	tracepoint_synchronize_unregister();
	pr_info("freq qos trace hooks unregistered and synchronized\n");

	/* 4. free all qos_client nodes in the list */
	spin_lock_irqsave(&freq_qos_client_lock, flags);
	list_for_each_entry_safe(qos_client, tmp, &qos_client_list, client_list) {
		/*
		 * CRITICAL: Clear android_oem_data1 BEFORE freeing qos_client.
		 * Otherwise, other modules' freq_qos callbacks (outside our module)
		 * may access the dangling pointer stored in android_oem_data1.
		 */
		if (qos_client->req)
			qos_client->req->android_oem_data1 = 0;
		list_del_init(&qos_client->client_list);
		kfree(qos_client);
		qos_client_list_num--;
	}
	spin_unlock_irqrestore(&freq_qos_client_lock, flags);
	pr_info("freq qos client list freed, remaining=%u\n", qos_client_list_num);

	/* 4. remove freq qos requests */
	for_each_present_cpu(cpu) {
		req = &per_cpu(qos_req_min, cpu);
		if (freq_qos_request_active(req))
			freq_qos_remove_request(req);

		req = &per_cpu(qos_req_max, cpu);
		if (freq_qos_request_active(req))
			freq_qos_remove_request(req);

		per_cpu(qos_req_cpufreq, cpu).min = 0;
		per_cpu(qos_req_cpufreq, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	pr_info("freq qos requests removed\n");
}
