

#ifndef _ASM_S390_TIMEX_H
#define _ASM_S390_TIMEX_H

/* The value of the TOD clock for 1.1.1970. */
#define TOD_UNIX_EPOCH 0x7d91048bca000000ULL

/* Inline functions for clock register access. */
static inline int set_clock(__u64 time)
{
	int cc;

	asm volatile(
		"   sck   %1\n"
		"   ipm   %0\n"
		"   srl   %0,28\n"
		: "=d" (cc) : "Q" (time) : "cc");
	return cc;
}

static inline int store_clock(__u64 *time)
{
	int cc;

	asm volatile(
		"   stck  %1\n"
		"   ipm   %0\n"
		"   srl   %0,28\n"
		: "=d" (cc), "=Q" (*time) : : "cc");
	return cc;
}

static inline void set_clock_comparator(__u64 time)
{
	asm volatile("sckc %0" : : "Q" (time));
}

static inline void store_clock_comparator(__u64 *time)
{
	asm volatile("stckc %0" : "=Q" (*time));
}

#define CLOCK_TICK_RATE	1193180 /* Underlying HZ */

typedef unsigned long long cycles_t;

static inline unsigned long long get_clock (void)
{
	unsigned long long clk;

	asm volatile("stck %0" : "=Q" (clk) : : "cc");
	return clk;
}

static inline void get_clock_ext(char *clk)
{
	asm volatile("stcke %0" : "=Q" (*clk) : : "cc");
}

static inline unsigned long long get_clock_xt(void)
{
	unsigned char clk[16];
	get_clock_ext(clk);
	return *((unsigned long long *)&clk[1]);
}

static inline cycles_t get_cycles(void)
{
	return (cycles_t) get_clock() >> 2;
}

int get_sync_clock(unsigned long long *clock);
void init_cpu_timer(void);
unsigned long long monotonic_clock(void);

void tod_to_timeval(__u64, struct timespec *);

static inline
void stck_to_timespec(unsigned long long stck, struct timespec *ts)
{
	tod_to_timeval(stck - TOD_UNIX_EPOCH, ts);
}

extern u64 sched_clock_base_cc;

static inline unsigned long long get_clock_monotonic(void)
{
	return get_clock_xt() - sched_clock_base_cc;
}

#endif
