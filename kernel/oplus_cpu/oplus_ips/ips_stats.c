// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "ips-dcvs: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/sched/clock.h>
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK_DDR)
#include <linux/arm-smccc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>
#endif
#include "ips_private.h"


#define DVFSRC_DDR_DVFS_GET_FREQ_COUNT 0x7
#define DVFSRC_DDR_DVFS_GET_FREQ_INFO 0x5

#define MAX_FREQ_COUNT 32

struct dcvs_hw {
	enum	dcvs_hw_type type;
	u32	freq_table[MAX_FREQ_COUNT];
	u32	table_len;
	u32	hw_min_freq;
	u32	hw_max_freq;
	/* time in stat */
	u64	*time_in_freq;
	u64	*time_in_freq_backup;
	u64	last_vote_time;
	u64	vote_count;
	u32	last_vote_index;
	u32	*freq_percent;
};

static struct dcvs_hw ips_dcvs_hw[NUM_DCVS_HW_TYPES];
static DEFINE_SPINLOCK(dcvs_stats_lock);

static const char* const dcvs_hw_names[NUM_DCVS_HW_TYPES] = {
	[DCVS_DDR]		= "DDR",
	[DCVS_SLC]		= "SLC",
	[DCVS_L3]		= "L3",
};


static void dcvs_stats_update(struct dcvs_hw *hw,
				 unsigned long long time)
{
	unsigned long long cur_time = local_clock();
	if (unlikely(!hw->time_in_freq))
		return;
	hw->time_in_freq[hw->last_vote_index] += cur_time - time;
	hw->last_vote_time = cur_time;
}

static void dcvs_stats_reset_table(struct dcvs_hw *hw)
{
	unsigned long long cur_time = local_clock();

	if (unlikely(!hw->time_in_freq))
		return;
	/*
	 * Only reset time_in_freq, NOT time_in_freq_backup.
	 * time_in_freq_backup holds the snapshot for percentage calculation
	 * in show_ddr_time_in_state after the lock is released.
	 */
	memset(hw->time_in_freq, 0, hw->table_len * sizeof(u64));
	hw->last_vote_time = cur_time;
	hw->vote_count = 0;
	/* current time as reset time, this freq stats is little. */
	dcvs_stats_update(hw, cur_time);
}

static int get_target_freq_index(struct dcvs_hw *hw, u32 freq)
{
	u32 *freq_table = hw->freq_table;
	u32 len = hw->table_len;
	int i;

	for (i = 0; i < len; i++) {
		if (freq <= freq_table[i])
			break;
	}

	if (i == len)
		i = len - 1;

	return i;
}

/* extern */
ssize_t show_ddr_time_in_state(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct dcvs_hw *hw = &ips_dcvs_hw[DCVS_DDR];
	int i, cnt = 0;
	unsigned long long time, total_time;
	unsigned long long  percent_in_freq;
	u32 integer_per, decimal_per;
	unsigned long flags;

	/* ddr,llcc,l3 freq */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%s,", dcvs_hw_names[hw->type]);
	for (i = 0; i < hw->table_len; i++) {
		if (cnt >= PAGE_SIZE)
			break;
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%u,",
				hw->freq_table[i]);
	}
	if (cnt >= PAGE_SIZE)
		return PAGE_SIZE;

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	if (unlikely(!hw->time_in_freq))
		return cnt;

	/* protect concurrent access to time_in_freq and related fields */
	spin_lock_irqsave(&dcvs_stats_lock, flags);

	/* freq time_in_state */
	for (i = 0; i < hw->table_len; i++) {
		time = hw->time_in_freq[i];
		if (i == hw->last_vote_index)
			time += local_clock() - hw->last_vote_time;
		hw->time_in_freq_backup[i] = time;
	}

	/* reset freq stats time and percent */
	dcvs_stats_reset_table(hw);

	spin_unlock_irqrestore(&dcvs_stats_lock, flags);

	/* freq percent - uses backup data, no lock needed */
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%s_pct,",
			 dcvs_hw_names[hw->type]);
	total_time = 0;
	for (i = 0; i < hw->table_len; i++) {
		total_time += hw->time_in_freq_backup[i];
	}
	for (i = 0; i < hw->table_len; i++) {
		if (cnt >= PAGE_SIZE)
			break;
		percent_in_freq = 0;
		if (total_time != 0) {
			percent_in_freq = hw->time_in_freq_backup[i] * 10000 / total_time;
		}
		integer_per = percent_in_freq / 100;
		decimal_per = percent_in_freq % 100;
		hw->freq_percent[i] = (decimal_per & 0xffff) | ((integer_per & 0xffff) << 16);
		if (decimal_per >= 10)
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d.%2d%%,",
						integer_per, decimal_per);
		else
			cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d.%d%%,",
						integer_per, decimal_per);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	if (cnt >= PAGE_SIZE) {
		pr_warn_once("vote time table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}

	return cnt;
}

ssize_t show_ddr_available_frequencies(struct kobject *kobj,
				struct attribute *attr, char *buf)
{
	struct dcvs_hw *hw = &ips_dcvs_hw[DCVS_DDR];
	int i, cnt = 0;

	for (i = 0; i < hw->table_len; i++)
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d ",
				hw->freq_table[i]);

	if (cnt)
		cnt--;

	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");

	return cnt;
}

void dcvs_stats_record_time(enum dcvs_hw_type hw_type, unsigned int new_freq)
{
	int old_index, new_index;
	struct dcvs_hw *hw = &ips_dcvs_hw[hw_type];
	unsigned long flags;

	/* hw is static array address, check time_in_freq instead */
	if (unlikely(!hw->time_in_freq))
		return;

	old_index = hw->last_vote_index;
	new_index = get_target_freq_index(hw, new_freq);

	/* We can't do hw->time_in_freq[-1]= .. */
	if (unlikely(old_index == -1 || new_index == -1))
		return;

	spin_lock_irqsave(&dcvs_stats_lock, flags);

	/* always update time for current frequency, even if freq unchanged */
	dcvs_stats_update(hw, hw->last_vote_time);

	/* only count when frequency actually changes */
	if (old_index != new_index) {
		hw->vote_count++;
		hw->last_vote_index = new_index;
	}

	spin_unlock_irqrestore(&dcvs_stats_lock, flags);
}

/* refer @mtk-dvfsrc-devfrq.c */
static int init_ddr_freq_info(struct dcvs_hw *hw)
{
#if IS_ENABLED(CONFIG_OPLUS_IPS_FOR_MTK_DDR)
	/* refer @mtk-dvfsrc-devfrq.c */
	struct arm_smccc_res res;
	int index;
	unsigned long rate;

	arm_smccc_smc(MTK_SIP_VCOREFS_CONTROL, DVFSRC_DDR_DVFS_GET_FREQ_COUNT,
			0, 0, 0, 0, 0, 0, &res);
	if (res.a0)
		return -ENODEV;

	hw->table_len = res.a1;
	if (hw->table_len <= 0 || hw->table_len > MAX_FREQ_COUNT)
		return -EINVAL;

	for (index = 0; index < hw->table_len; ++index) {
		arm_smccc_smc(MTK_SIP_VCOREFS_CONTROL,
				DVFSRC_DDR_DVFS_GET_FREQ_INFO,
				index, 0, 0, 0, 0, 0, &res);
		if ((res.a0) || (long)res.a1 <= 0)
			return -EINVAL;
		/* return freq as khz*/
		rate = res.a1;

		hw->freq_table[index] = rate;
	}
#endif
	return 0;
}

int ips_dcvs_hw_init(void)
{
	enum dcvs_hw_type hw_type = DCVS_DDR;
	struct dcvs_hw *hw = &ips_dcvs_hw[hw_type];
	int ret = 0;
	u32 alloc_size;


	ret = init_ddr_freq_info(hw);
	if (ret < 0) {
		pr_err("get mtk ddr info failed, ret=%d\n", ret);
		return ret;
	}

	hw->type = DCVS_DDR;
	hw->hw_max_freq = hw->freq_table[hw->table_len-1];
	hw->hw_min_freq = hw->freq_table[0];

	/* initial vote freq time in state */
	alloc_size = hw->table_len * sizeof(u32);
	hw->freq_percent = kzalloc(alloc_size, GFP_KERNEL);
	if (!hw->freq_percent)
		goto freq_pct_fail;

	alloc_size = hw->table_len * sizeof(u64);
	hw->time_in_freq = kzalloc(alloc_size, GFP_KERNEL);
	if (!hw->time_in_freq)
		goto time_freq_fail;

	hw->time_in_freq_backup = kzalloc(alloc_size, GFP_KERNEL);
	if (!hw->time_in_freq_backup)
		goto time_freq_bk_fail;

	hw->vote_count = 0;
	hw->last_vote_index = 0;
	hw->last_vote_time = local_clock();

	pr_info("ips_dcvs_hw_init success, ddr freq table_len=%u\n", hw->table_len);
	return 0;

time_freq_bk_fail:
	kfree(hw->time_in_freq);
	hw->time_in_freq = NULL;
time_freq_fail:
	kfree(hw->freq_percent);
	hw->freq_percent = NULL;
freq_pct_fail:
	pr_err("ips_dcvs_hw_init memory alloc failed\n");
	return -ENOMEM;
}

void ips_dcvs_hw_exit(void)
{
	enum dcvs_hw_type hw_type = DCVS_DDR;
	struct dcvs_hw *hw = &ips_dcvs_hw[hw_type];

	if (hw->time_in_freq_backup) {
		kfree(hw->time_in_freq_backup);
		hw->time_in_freq_backup = NULL;
	}

	if (hw->time_in_freq) {
		kfree(hw->time_in_freq);
		hw->time_in_freq = NULL;
	}

	if (hw->freq_percent) {
		kfree(hw->freq_percent);
		hw->freq_percent = NULL;
	}

	pr_info("ips_dcvs_hw_exit: resources freed\n");
}

