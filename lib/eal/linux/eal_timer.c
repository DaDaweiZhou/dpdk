/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation.
 * Copyright(c) 2012-2013 6WIND S.A.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#ifdef RTE_LIBEAL_USE_HPET
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_thread.h>

#include <eal_export.h>
#include "eal_private.h"

RTE_EXPORT_SYMBOL(eal_timer_source)
enum timer_source eal_timer_source = EAL_TIMER_HPET;

#ifdef RTE_LIBEAL_USE_HPET

#define DEV_HPET "/dev/hpet"

/* Maximum number of counters. */
#define HPET_TIMER_NUM 3

/* General capabilities register */
#define CLK_PERIOD_SHIFT     32 /* Clock period shift. */
#define CLK_PERIOD_MASK      0xffffffff00000000ULL /* Clock period mask. */

/**
 * HPET timer registers. From the Intel IA-PC HPET (High Precision Event
 * Timers) Specification.
 */
struct eal_hpet_regs {
	/* Memory-mapped, software visible registers */
	uint64_t capabilities;      /**< RO General Capabilities Register. */
	uint64_t reserved0;         /**< Reserved for future use. */
	uint64_t config;            /**< RW General Configuration Register. */
	uint64_t reserved1;         /**< Reserved for future use. */
	uint64_t isr;               /**< RW Clear General Interrupt Status. */
	uint64_t reserved2[25];     /**< Reserved for future use. */
	union {
		uint64_t counter;   /**< RW Main Counter Value Register. */
		struct {
			uint32_t counter_l; /**< RW Main Counter Low. */
			uint32_t counter_h; /**< RW Main Counter High. */
		};
	};
	uint64_t reserved3;         /**< Reserved for future use. */
	struct {
		uint64_t config;    /**< RW Timer Config and Capability Reg. */
		uint64_t comp;      /**< RW Timer Comparator Value Register. */
		uint64_t fsb;       /**< RW FSB Interrupt Route Register. */
		uint64_t reserved4; /**< Reserved for future use. */
	} timers[HPET_TIMER_NUM]; /**< Set of HPET timers. */
};

/* Mmap'd hpet registers */
static volatile struct eal_hpet_regs *eal_hpet = NULL;

/* Period at which the HPET counter increments in
 * femtoseconds (10^-15 seconds). */
static uint32_t eal_hpet_resolution_fs = 0;

/* Frequency of the HPET counter in Hz */
static uint64_t eal_hpet_resolution_hz = 0;

/* Incremented 4 times during one 32bits hpet full count */
static uint32_t eal_hpet_msb;

static rte_thread_t msb_inc_thread_id;

/*
 * This function runs on a specific thread to update a global variable
 * containing used to process MSB of the HPET (unfortunately, we need
 * this because hpet is 32 bits by default under linux).
 */
static uint32_t
hpet_msb_inc(__rte_unused void *arg)
{
	uint32_t t;

	while (1) {
		t = (eal_hpet->counter_l >> 30);
		if (t != (eal_hpet_msb & 3))
			eal_hpet_msb ++;
		sleep(10);
	}
	return 0;
}

RTE_EXPORT_SYMBOL(rte_get_hpet_hz)
uint64_t
rte_get_hpet_hz(void)
{
	const struct internal_config *internal_conf =
		eal_get_internal_configuration();

	if (internal_conf->no_hpet)
		rte_panic("Error, HPET called, but no HPET present\n");

	return eal_hpet_resolution_hz;
}

RTE_EXPORT_SYMBOL(rte_get_hpet_cycles)
uint64_t
rte_get_hpet_cycles(void)
{
	uint32_t t, msb;
	uint64_t ret;
	const struct internal_config *internal_conf =
		eal_get_internal_configuration();

	if (internal_conf->no_hpet)
		rte_panic("Error, HPET called, but no HPET present\n");

	t = eal_hpet->counter_l;
	msb = eal_hpet_msb;
	ret = (msb + 2 - (t >> 30)) / 4;
	ret <<= 32;
	ret += t;
	return ret;
}

#endif

#ifdef RTE_LIBEAL_USE_HPET
/*
 * Open and mmap /dev/hpet (high precision event timer) that will
 * provide our time reference.
 */
RTE_EXPORT_SYMBOL(rte_eal_hpet_init)
int
rte_eal_hpet_init(int make_default)
{
	int fd, ret;
	struct internal_config *internal_conf =
		eal_get_internal_configuration();

	if (internal_conf->no_hpet) {
		EAL_LOG(NOTICE, "HPET is disabled");
		return -1;
	}

	fd = open(DEV_HPET, O_RDONLY);
	if (fd < 0) {
		EAL_LOG(ERR, "ERROR: Cannot open "DEV_HPET": %s!",
			strerror(errno));
		internal_conf->no_hpet = 1;
		return -1;
	}
	eal_hpet = mmap(NULL, 1024, PROT_READ, MAP_SHARED, fd, 0);
	if (eal_hpet == MAP_FAILED) {
		EAL_LOG(ERR, "ERROR: Cannot mmap "DEV_HPET"!");
		close(fd);
		internal_conf->no_hpet = 1;
		return -1;
	}
	close(fd);

	eal_hpet_resolution_fs = (uint32_t)((eal_hpet->capabilities &
					CLK_PERIOD_MASK) >>
					CLK_PERIOD_SHIFT);

	eal_hpet_resolution_hz = (1000ULL*1000ULL*1000ULL*1000ULL*1000ULL) /
		(uint64_t)eal_hpet_resolution_fs;

	EAL_LOG(INFO, "HPET frequency is ~%"PRIu64" kHz",
			eal_hpet_resolution_hz/1000);

	eal_hpet_msb = (eal_hpet->counter_l >> 30);

	/* create a thread that will increment a global variable for
	 * msb (hpet is 32 bits by default under linux) */
	ret = rte_thread_create_internal_control(&msb_inc_thread_id, "hpet-msb",
			hpet_msb_inc, NULL);
	if (ret != 0) {
		EAL_LOG(ERR, "ERROR: Cannot create HPET timer thread!");
		internal_conf->no_hpet = 1;
		return -1;
	}

	if (make_default)
		eal_timer_source = EAL_TIMER_HPET;
	return 0;
}
#endif

/* Check if the kernel deems the arch provided TSC frequency trustworthy. */

static bool
is_tsc_known_freq(void)
{
	bool ret = true; /* Assume tsc_known_freq */

#if defined(RTE_ARCH_X86)
	char line[2048];
	FILE *stream;

	stream = fopen("/proc/cpuinfo", "r");
	if (!stream) {
		EAL_LOG(WARNING, "Unable to open /proc/cpuinfo");
		return ret;
	}

	while (fgets(line, sizeof(line), stream)) {
		if (strncmp(line, "flags", 5) != 0)
			continue;

		if (!strstr(line, "tsc_known_freq"))
			ret = false;

		break;
	}

	fclose(stream);
#endif

	return ret;
}

uint64_t
get_tsc_freq(uint64_t arch_hz)
{
#ifdef CLOCK_MONOTONIC_RAW
#define NS_PER_SEC 1E9
#define CYC_PER_100KHZ 1E5

	struct timespec sleeptime = {.tv_nsec = NS_PER_SEC / 10 }; /* 1/10 second */

	struct timespec t_start, t_end;
	uint64_t tsc_hz;

	if (arch_hz && is_tsc_known_freq())
		return arch_hz;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &t_start) == 0) {
		uint64_t ns, end, start = rte_rdtsc();
		nanosleep(&sleeptime,NULL);
		clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
		end = rte_rdtsc();
		ns = ((t_end.tv_sec - t_start.tv_sec) * NS_PER_SEC);
		ns += (t_end.tv_nsec - t_start.tv_nsec);

		double secs = (double)ns/NS_PER_SEC;
		tsc_hz = (uint64_t)((end - start)/secs);

		if (arch_hz) {
			/* Make sure we're within 1% for sanity check */
			if (RTE_MAX(arch_hz, tsc_hz) - RTE_MIN(arch_hz, tsc_hz) > arch_hz / 100)
				return arch_hz;

			EAL_LOG(DEBUG,
				"Refined arch frequency %"PRIu64" to measured frequency %"PRIu64,
				arch_hz, tsc_hz);
		}

		/* Round up to 100Khz. 1E5 ~ 100Khz */
		return RTE_ALIGN_MUL_NEAR(tsc_hz, CYC_PER_100KHZ);
	}
#endif
	return arch_hz;
}

int
rte_eal_timer_init(void)
{

	eal_timer_source = EAL_TIMER_TSC;

	set_tsc_freq();
	return 0;
}
