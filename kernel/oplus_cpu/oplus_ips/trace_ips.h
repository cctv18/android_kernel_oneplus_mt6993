/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Oplus. All rights reserved.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ips

#if !defined(_TRACE_IPS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IPS_H

#include <linux/tracepoint.h>


TRACE_EVENT(ips_memlat_cpu_time,

	TP_PROTO(int cpu, long cycle_cnt_cputime, long rq_cpu_time, long cpu_run_time, long delta_us),

	TP_ARGS(cpu, cycle_cnt_cputime, rq_cpu_time, cpu_run_time, delta_us),

	TP_STRUCT__entry(
		__field(int, cpu)
		__field(long, cycle_cnt_cputime)
		__field(long, rq_cpu_time)
		__field(long, cpu_run_time)
		__field(long, delta_us)),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->cycle_cnt_cputime = cycle_cnt_cputime;
		__entry->rq_cpu_time = rq_cpu_time;
		__entry->cpu_run_time = cpu_run_time;
		__entry->delta_us = delta_us;),

	TP_printk("cpu=%d cycle_cnt_cputime=%lu sched_irq_cputime=%lu cpu_run_time=%lu delta_us=%lu",
		__entry->cpu,
		__entry->cycle_cnt_cputime,
		__entry->rq_cpu_time,
		__entry->cpu_run_time,
		__entry->delta_us)
);

TRACE_EVENT(ips_memlat_mem_pmus,

    TP_PROTO(int cpu, u64 p0, u64 p1, u64 p2, u64 p3, u64 p4, u64 p5, u64 p6, u64 p7, u64 p8, long p9, long p10),

    TP_ARGS(cpu, p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10),

    TP_STRUCT__entry(
        __field(int, cpu)
        __field(u64, p0)
        __field(u64, p1)
        __field(u64, p2)
        __field(u64, p3)
        __field(u64, p4)
        __field(u64, p5)
        __field(u64, p6)
        __field(u64, p7)
        __field(u64, p8)
        __field(long, p9)
        __field(long, p10)),

    TP_fast_assign(
        __entry->cpu = cpu;
        __entry->p0 = p0;
        __entry->p1 = p1;
        __entry->p2 = p2;
        __entry->p3 = p3;
        __entry->p4 = p4;
        __entry->p5 = p5;
        __entry->p6 = p6;
        __entry->p7 = p7;
        __entry->p8 = p8;
        __entry->p9 = p9;
        __entry->p10 = p10;),

    TP_printk("cpu=%d p0=%llu p1=%llu p2=%llu p3=%llu p4=%llu p5=%llu p6=%llu demand_ips=%llu my_demand_ips=%llu raw_inst_CPI=%ld raw_mem_stall=%ld",
        __entry->cpu,
        __entry->p0,
        __entry->p1,
        __entry->p2,
        __entry->p3,
        __entry->p4,
        __entry->p5,
        __entry->p6,
        __entry->p7,
        __entry->p8,
        __entry->p9,
        __entry->p10)
);

TRACE_EVENT(ips_memlat_mem_stall,

	TP_PROTO(int cpu, long mem_stall, long inst_stall_CPI, long pSPI, long pred_pSPI, u64 cycles, unsigned long prev_freq, unsigned long raw_freq, u64 inst_rd, unsigned long util_rate, unsigned long vote_freq),

	TP_ARGS(cpu, mem_stall, inst_stall_CPI, pSPI, pred_pSPI, cycles, prev_freq, raw_freq, inst_rd, util_rate, vote_freq),

	TP_STRUCT__entry(
		__field(int, cpu)
		__field(long, mem_stall)
		__field(long, inst_stall_CPI)
		__field(long, pSPI)
		__field(long, pred_pSPI)
		__field(u64, cycles)
		__field(unsigned long, prev_freq)
		__field(unsigned long, raw_freq)
		__field(u64, inst_rd)
		__field(unsigned long, util_rate)
		__field(unsigned long, vote_freq)),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->mem_stall = mem_stall;
		__entry->inst_stall_CPI = inst_stall_CPI;
		__entry->pSPI = pSPI;
		__entry->pred_pSPI = pred_pSPI;
		__entry->cycles = cycles;
		__entry->prev_freq = prev_freq;
		__entry->raw_freq = raw_freq;
		__entry->inst_rd = inst_rd;
		__entry->util_rate = util_rate;
		__entry->vote_freq = vote_freq;),

	TP_printk("cpu=%d mem_stall=%ld inst_stall_CPI=%ld pSPI=%ld pred_pSPI=%ld cycles=%llu prev_freq=%lu raw_freq=%lu inst_rd=%llu util_rate=%lu vote_freq=%lu",
		__entry->cpu,
		__entry->mem_stall,
		__entry->inst_stall_CPI,
		__entry->pSPI,
		__entry->pred_pSPI,
		__entry->cycles,
		__entry->prev_freq,
		__entry->raw_freq,
		__entry->inst_rd,
		__entry->util_rate,
		__entry->vote_freq)
);

TRACE_EVENT(ips_memlat_compute_cost,

	TP_PROTO(int cpu, u64 cost, unsigned long freq),

	TP_ARGS(cpu, cost, freq),

	TP_STRUCT__entry(
		__field(int, cpu)
		__field(u64, cost)
		__field(unsigned long, freq)),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->cost = cost;
		__entry->freq = freq;),

	TP_printk("cpu=%d ips_algo_time=%llu cpufreq=%lu",
		__entry->cpu,
		__entry->cost,
		__entry->freq)
);

TRACE_EVENT(tracing_mark_write,
	TP_PROTO(char trace_type, int pid,
		const char *name, int value),
	TP_ARGS(trace_type, pid, name, value),
	TP_STRUCT__entry(
			__field(char, trace_type)
			__field(int, pid)
			__string(name, name)
			__field(int, value)),
	TP_fast_assign(
			__entry->trace_type = trace_type;
			__entry->pid = pid;
			__assign_str(name);
			__entry->value = value;),
	TP_printk("%c|%d|%s|%d", __entry->trace_type,
			__entry->pid, __get_str(name), __entry->value)
);

TRACE_EVENT(ips_memlat_ips_xmus,

    TP_PROTO(int cpu, unsigned long cpufreq, u64 cputime, u64 *pmus, u64 *amus),

    TP_ARGS(cpu, cpufreq, cputime, pmus, amus),

    TP_STRUCT__entry(
        __field(int, cpu)
	__field(unsigned long, cpufreq)
	__field(u64, cputime)
	__array(u64, pmus, 32)
	__array(u64, amus, 4)
	__field(u64, a0)
	__field(u64, a1)
	__field(u64, a2)
	__field(u64, a3)
	__field(u64, p0)
	__field(u64, p1)
	__field(u64, p2)
	__field(u64, p3)
	__field(u64, p4)
	__field(u64, p5)
	__field(u64, p6)
	__field(u64, p7)
	__field(u64, p8)
	__field(u64, p9)
	__field(u64, p10)
	__field(u64, p11)
	__field(u64, p12)
	__field(u64, p13)
	__field(u64, p14)
	__field(u64, p15)
	__field(u64, p16)
	__field(u64, p17)
	__field(u64, p18)
	__field(u64, p19)
	__field(u64, p20)
	__field(u64, p21)
	__field(u64, p22)
	__field(u64, p23)
	__field(u64, p24)
	__field(u64, p25)
	__field(u64, p26)
	__field(u64, p27)
	__field(u64, p28)),

    TP_fast_assign(
	__entry->cpu = cpu;
	__entry->cpufreq = cpufreq;
	__entry->cputime = cputime;
	__entry->a0 = amus[0];
	__entry->a1 = amus[1];
	__entry->a2 = amus[2];
	__entry->a3 = amus[3];
	__entry->p0 = pmus[0];
	__entry->p1 = pmus[1];
	__entry->p2 = pmus[2];
	__entry->p3 = pmus[3];
	__entry->p4 = pmus[4];
	__entry->p5 = pmus[5];
	__entry->p6 = pmus[6];
	__entry->p7 = pmus[7];
	__entry->p8 = pmus[8];
	__entry->p9 = pmus[9];
	__entry->p10 = pmus[10];
	__entry->p11 = pmus[11];
	__entry->p12 = pmus[12];
	__entry->p13 = pmus[13];
	__entry->p14 = pmus[14];
	__entry->p15 = pmus[15];
	__entry->p16 = pmus[16];
	__entry->p17 = pmus[17];
	__entry->p18 = pmus[18];
	__entry->p19 = pmus[19];
	__entry->p20 = pmus[20];
	__entry->p21 = pmus[21];
	__entry->p22 = pmus[22];
	__entry->p23 = pmus[23];
	__entry->p24 = pmus[24];
	__entry->p25 = pmus[25];
	__entry->p26 = pmus[26];
	__entry->p27 = pmus[27];
	__entry->p28 = pmus[28];),

    TP_printk("cpu=%d cpufreq=%lu cputime=%llu cycles=%llu instrs=%llu cycles_cnt=%llu stall_bkd_mem=%llu p0=%llu p1=%llu p2=%llu p3=%llu p4=%llu p5=%llu p6=%llu p7=%llu p8=%llu p9=%llu p10=%llu p11=%llu p12=%llu p13=%llu p14=%llu p15=%llu p16=%llu p17=%llu p18=%llu p19=%llu p20=%llu p21=%llu p22=%llu p23=%llu p24=%llu p25=%llu p26=%llu p27=%llu p28=%llu",
	__entry->cpu,
	__entry->cpufreq,
	__entry->cputime,
	__entry->a0,
	__entry->a1,
	__entry->a2,
	__entry->a3,
	__entry->p0,
	__entry->p1,
	__entry->p2,
	__entry->p3,
	__entry->p4,
	__entry->p5,
	__entry->p6,
	__entry->p7,
	__entry->p8,
	__entry->p9,
	__entry->p10,
	__entry->p11,
	__entry->p12,
	__entry->p13,
	__entry->p14,
	__entry->p15,
	__entry->p16,
	__entry->p17,
	__entry->p18,
	__entry->p19,
	__entry->p20,
	__entry->p21,
	__entry->p22,
	__entry->p23,
	__entry->p24,
	__entry->p25,
	__entry->p26,
	__entry->p27,
	__entry->p28)
);

TRACE_EVENT(ips_memlat_ips_tf,

	TP_PROTO(int cpu, int min_freq, int max_freq, int tf_idx, int min_idx, int max_idx, int upper, int lower, int inst_idx, int mem_idx, long mem_stall, long base_cpi),

	TP_ARGS(cpu, min_freq, max_freq, tf_idx, min_idx, max_idx, upper, lower, inst_idx, mem_idx, mem_stall, base_cpi),

	TP_STRUCT__entry(
		__field(int, cpu)
		__field(int, min_freq)
		__field(int, max_freq)
		__field(int, tf_idx)
		__field(int, min_idx)
		__field(int, max_idx)
		__field(int, upper)
		__field(int, lower)
		__field(int, inst_idx)
		__field(int, mem_idx)
		__field(long, mem_stall)
		__field(long, base_cpi)),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->min_freq = min_freq;
		__entry->max_freq = max_freq;
		__entry->tf_idx = tf_idx;
		__entry->min_idx = min_idx;
		__entry->max_idx = max_idx;
		__entry->upper = upper;
		__entry->lower = lower;
		__entry->inst_idx = inst_idx;
		__entry->mem_idx = mem_idx;
		__entry->mem_stall = mem_stall;
		__entry->base_cpi = base_cpi;),

	TP_printk("cpu=%d min_freq=%d max_freq=%d tf_idx=%d min_idx=%d max_idx=%d upper=%d lower=%d inst_idx=%d mem_idx=%d mem_stall=%ld inst_stall_CPI=%ld",
		__entry->cpu,
		__entry->min_freq,
		__entry->max_freq,
		__entry->tf_idx,
		__entry->min_idx,
		__entry->max_idx,
		__entry->upper,
		__entry->lower,
		__entry->inst_idx,
		__entry->mem_idx,
		__entry->mem_stall,
		__entry->base_cpi)
);

TRACE_EVENT(ips_memlat_ips_slope,

    TP_PROTO(int cpu, int *slope),

    TP_ARGS(cpu, slope),

    TP_STRUCT__entry(
        __field(int, cpu)
	__array(int, slope, 32)
	__field(int, p0)
	__field(int, p1)
	__field(int, p2)
	__field(int, p3)
	__field(int, p4)
	__field(int, p5)
	__field(int, p6)
	__field(int, p7)
	__field(int, p8)
	__field(int, p9)
	__field(int, p10)
	__field(int, p11)
	__field(int, p12)
	__field(int, p13)
	__field(int, p14)
	__field(int, p15)),

    TP_fast_assign(
	__entry->cpu = cpu;
	__entry->p0 = slope[0];
	__entry->p1 = slope[1];
	__entry->p2 = slope[2];
	__entry->p3 = slope[3];
	__entry->p4 = slope[4];
	__entry->p5 = slope[5];
	__entry->p6 = slope[6];
	__entry->p7 = slope[7];
	__entry->p8 = slope[8];
	__entry->p9 = slope[9];
	__entry->p10 = slope[10];
	__entry->p11 = slope[11];
	__entry->p12 = slope[12];
	__entry->p13 = slope[13];
	__entry->p14 = slope[14];
	__entry->p15 = slope[15];),

    TP_printk("cpu=%d s0=%d s1=%d s2=%d s3=%d s4=%d s5=%d s6=%d s7=%d s8=%d s9=%d s10=%d s11=%d s12=%d s13=%d s14=%d s15=%d",
	__entry->cpu,
	__entry->p0,
	__entry->p1,
	__entry->p2,
	__entry->p3,
	__entry->p4,
	__entry->p5,
	__entry->p6,
	__entry->p7,
	__entry->p8,
	__entry->p9,
	__entry->p10,
	__entry->p11,
	__entry->p12,
	__entry->p13,
	__entry->p14,
	__entry->p15)
);
#endif /* !defined(_TRACE_IPS_H) || defined(TRACE_HEADER_MULTI_READ) */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH oplus_ips

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_ips

#include <trace/define_trace.h>
