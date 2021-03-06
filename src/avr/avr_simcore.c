/*
 * Copyright (c) 2017, 2018,
 * Dmitry Salychev <darkness.bsd@gmail.com>,
 * Alexander Salychev <ppsalex@rambler.ru> et al.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "mcusim/mcusim.h"
#include "mcusim/hex/ihex.h"
#include "mcusim/log.h"

#define REG_ZH			0x1F
#define REG_ZL			0x1E
#define CLK_RISE		0
#define CLK_FALL		1
#define TICKS_MAX		UINT64_MAX

/* Macros to read and update AVR status register (SREG) */
#define GLOB_INT	7
#define UPDATE_SREG(mcu, flag, set_f) do {				\
	if ((set_f) == 0) {						\
		*mcu->sreg &= (uint8_t)~(1<<(flag));			\
	} else {							\
		*mcu->sreg |= (uint8_t)(1<<(flag));			\
	}								\
} while (0)
#define READ_SREG(mcu, flag) ((uint8_t)((*mcu->sreg>>(flag))&1))

typedef int (*init_func)(struct MSIM_AVR *mcu, struct MSIM_InitArgs *args);

/* Function to process interrupt request according to the order */
static int handle_irq(struct MSIM_AVR *mcu);

/* Cell contains AVR MCU part and its init function. */
struct init_func_info {
	char partno[20];
	char name[20];
	init_func f;
};

/* Init functions for the supported AVR MCUs. */
static struct init_func_info init_funcs[] = {
	{ "m8",		"ATmega8",	MSIM_M8AInit },
	{ "m8a",	"ATmega8A",	MSIM_M8AInit },
	{ "m328",	"ATmega328",	MSIM_M328Init },
	{ "m328p",	"ATmega328P",	MSIM_M328PInit },
//	{ "m2560",	"ATmega2560",	MSIM_M2560Init }
};

static int load_progmem(struct MSIM_AVR *mcu, FILE *fp);

int MSIM_AVR_Simulate(struct MSIM_AVR *mcu, unsigned long steps,
                      unsigned long addr, unsigned char firmware_test)
{
	FILE *vcd_f = NULL;	/* File to write VCD dump to */
	uint64_t tick = 0;	/* # of ticks (rises+falls) sinse start */
	uint8_t tick_ovf = 0;	/* Ticks overflow flag */
	int ret_code = 0;	/* Return code */

	/* Do we have registers to dump? */
	if (mcu->vcd->i >= 0) {
		vcd_f = MSIM_AVR_VCDOpenDump(mcu, mcu->vcd_file);
		if (vcd_f == NULL) {
			snprintf(mcu->log, sizeof mcu->log, "failed to open a "
			         "VCD file %s to write to", mcu->vcd_file);
			MSIM_LOG_FATAL(mcu->log);
			return -1;
		}
	}

	/* Force MCU to run in a firmware-test mode. */
	if (firmware_test) {
		MSIM_LOG_DEBUG("running in a firmware test mode");
		mcu->state = AVR_RUNNING;
	}

	/* Main simulation loop. Each iteration represents both rise (R) and
	 * fall (F) of the microcontroller's clock. It's necessary to dump
	 * CLK_IO to the timing diagram in a pulse-accurate way.
	 *
	 *                          MAIN LOOP ITERATIONS
	 *            R     F     R     F     R     F     R     F     R
	 *           /     /     /     /     /     /     /     /     /
	 *          |           |           |           |           |
	 *          |_____      |_____      |_____      |_____      |_____
	 *          |     |     |     |     |     |     |     |     |     |
	 * CLK_IO   |     |     |     |     |     |     |     |     |     |
	 *          |     |_____|     |_____|     |_____|     |_____|     |__
	 *          |           |           |           |           |
	 *          |           |___________|           |___________|
	 *          |           |           |           |           |
	 * CLK_IO/2 |           |           |           |           |
	 *          |___________|           |___________|           |________
	 *          |           |           |           |           |
	 *          |           |           |           |           |
	 */
	while (1) {
		/* The main simulation loop can be terminated by setting
		 * MCU state to AVR_MSIM_STOP. The primary (and maybe only)
		 * source of this state setting is a command from debugger. */
		if ((mcu->ic_left == 0) && (mcu->state == AVR_MSIM_STOP)) {
			if (MSIM_LOG_ISDEBUG) {
				snprintf(mcu->log, sizeof mcu->log, "simulation "
				         "terminated because of stopped mcu, "
				         "pc=0x%06" PRIX32, mcu->pc);
				MSIM_LOG_DEBUG(mcu->log);
			}
			break;
		}
		if ((mcu->ic_left == 0) && (mcu->state == AVR_MSIM_TESTFAIL)) {
			MSIM_LOG_DEBUG("simulation terminated because of a "
			               "failed test");
			ret_code = 1;
			break;
		}

		/* Wait for request from GDB in MCU stopped mode */
		if (!firmware_test && !mcu->ic_left &&
		                (mcu->state == AVR_STOPPED) &&
		                MSIM_AVR_RSPHandle()) {
			snprintf(mcu->log, sizeof mcu->log, "handling message "
			         "from GDB RSP client failed: pc=0x%06" PRIX32,
			         mcu->pc);
			MSIM_LOG_FATAL(mcu->log);
			ret_code = 1;
			break;
		}

		/* Tick MCU periferals.
		 *
		 * NOTE: It is important to tick MCU peripherals before
		 * updating Lua models. One of the reasons is an accessing
		 * mechanism of the registers which share the same I/O
		 * location (UBRRH/UCSRC of ATmega8A for example). */
		if (mcu->tick_perf != NULL) {
			mcu->tick_perf(mcu);
		}

		/* Tick peripherals written in Lua */
		MSIM_AVR_LUATickModels(mcu);

		/* Dump registers to VCD */
		if (vcd_f && !tick_ovf) {
			MSIM_AVR_VCDDumpFrame(vcd_f, mcu, tick, CLK_RISE);
		}

		/* Test scope of a program counter within flash size */
		if ((mcu->pc>>1) > (mcu->flashend>>1)) {
			snprintf(mcu->log, sizeof mcu->log, "program counter is "
			         "out of flash memory: pc=0x%06" PRIX32
			         ", flashend=0x%06" PRIX32,
			         mcu->pc>>1, mcu->flashend>>1);
			MSIM_LOG_FATAL(mcu->log);
			ret_code = 1;
			break;
		}
		/* Test scope of a program counter */
		if ((mcu->pc+2) >= mcu->pm_size) {
			snprintf(mcu->log, sizeof mcu->log, "program counter "
			         "is out of scope: pc=0x%06" PRIX32 ", pm_size"
			         "=0x%06" PRIX32, mcu->pc, mcu->pm_size);
			MSIM_LOG_FATAL(mcu->log);
			ret_code = 1;
			break;
		}

		/* Decode next instruction. It's usually hard to say in
		 * which state the MCU registers will be between neighbor
		 * cycles of a multi-cycle instruction. This talk may be
		 * taken into account:
		 * https://electronics.stackexchange.com/questions/132171/
		 * 	what-happens-to-avr-registers-during-multi-
		 * 	cycle-instructions,
		 * but this change of LSB and MSB can be MCU-specific and
		 * not a general way of how it really works. Detailed
		 * information can be obtained directly from Atmel, but
		 * there is no intention to do this in order not to
		 * unveil their secrets. However, any details they're ready
		 * to share are highly welcome.
		 *
		 * Simulator doesn't guarantee anything special here either.
		 * The only thing you may rely on is the instruction which
		 * will be completed _after all_ of these cycles required to
		 * finish instruction itself. */
		if ((mcu->ic_left || mcu->state == AVR_RUNNING ||
		                mcu->state == AVR_MSIM_STEP) &&
		                MSIM_AVR_Step(mcu)) {
			snprintf(mcu->log, sizeof mcu->log, "decoding "
			         "instruction failed: pc=0x%06" PRIX32,
			         mcu->pc);
			MSIM_LOG_FATAL(mcu->log);
			ret_code = 1;
			break;
		}

		/* Provide IRQs based on the MCU flags and handle them if this
		 * is possible.
		 *
		 * It's important to understand an interrupt may occur during
		 * execution of a multi-cycle instruction. This instruction
		 * is completed before the interrupt is served (according to
		 * the multiple AVR datasheets). It means that we may provide
		 * IRQs, but will have to wait required number of cycles
		 * to serve them. */
		if (mcu->pass_irqs != NULL) {
			mcu->pass_irqs(mcu);
		}
		if (READ_SREG(mcu, GLOB_INT) &&
		                (!mcu->ic_left) && (!mcu->intr.exec_main) &&
		                (mcu->state == AVR_RUNNING ||
		                 mcu->state == AVR_MSIM_STEP)) {
			handle_irq(mcu);
		}

		/* Halt MCU after a single step performed */
		if (!mcu->ic_left && mcu->state == AVR_MSIM_STEP) {
			mcu->state = AVR_STOPPED;
		}

		/* All cycles of a single instruction from a main program
		 * have to be performed. */
		if (mcu->ic_left == 0) {
			mcu->intr.exec_main = 0;
		}

		/* Increment ticks or print a warning message in case of
		 * maximum amount of ticks reached (extremely unlikely
		 * if compiler supports "unsigned long long" type). */
		if (tick == TICKS_MAX) {
			tick_ovf = 1;
			MSIM_LOG_WARN("maximum amount of simulation ticks "
			              "reached");
			if (vcd_f != NULL) {
				MSIM_LOG_WARN("VCD dump will not be recorded "
				              "further");
			}
		} else {
			tick++;
		}
		/* Dump a fall to VCD */
		if (vcd_f && !tick_ovf) {
			MSIM_AVR_VCDDumpFrame(vcd_f, mcu, tick, CLK_FALL);
		}
		tick++;
	}

	if (vcd_f) {
		fclose(vcd_f);
	}
	return ret_code;
}

int MSIM_AVR_Init(struct MSIM_AVR *mcu, const char *mcu_name,
                  unsigned char *pm, unsigned long pm_size,
                  unsigned char *dm, unsigned long dm_size,
                  unsigned char *mpm, FILE *fp)
{
	unsigned int i;
	char mcu_found = 0;
	char log[1024];
	struct MSIM_InitArgs args;

	args.pm = pm;
	args.dm = dm;
	args.pmsz = (uint32_t)pm_size;
	args.dmsz = (uint32_t)dm_size;

	for (i = 0; i < sizeof(init_funcs)/sizeof(init_funcs[0]); i++) {
		if (!strcmp(init_funcs[i].partno, mcu_name)) {
			if (init_funcs[i].f(mcu, &args)) {
				return -1;
			} else {
				mcu_found = 1;
				break;
			}
		}
	}

	if (!mcu_found) {
		snprintf(log, sizeof log, "MCU model is not supported: %s",
		         mcu_name);
		MSIM_LOG_FATAL(log);
		return -1;
	}

	if (load_progmem(mcu, fp)) {
		MSIM_LOG_FATAL("program memory cannot be loaded from a file");
		return -1;
	}
	mcu->state = AVR_STOPPED;
	mcu->read_from_mpm = 0;
	return 0;
}

static int load_progmem(struct MSIM_AVR *mcu, FILE *fp)
{
	IHexRecord rec, mem_rec;
	char log[1024];

	if (fp == NULL) {
		MSIM_LOG_FATAL("cannot read program memory from a file");
		return -1;
	}

	/* Copy HEX data to program memory of the MCU */
	while (Read_IHexRecord(&rec, fp) == IHEX_OK) {
		switch (rec.type) {
		case IHEX_TYPE_00:	/* Data */
			memcpy(mcu->pm + rec.address,
			       rec.data, (uint16_t) rec.dataLen);
			break;
		case IHEX_TYPE_01:	/* End of File */
		default:		/* Other types, unlikely occured */
			continue;
		}
	}

	/* Verify checksum of the loaded data */
	rewind(fp);
	while (Read_IHexRecord(&rec, fp) == IHEX_OK) {
		if (rec.type != IHEX_TYPE_00) {
			continue;
		}

		memcpy(mem_rec.data, mcu->pm + rec.address,
		       (uint16_t) rec.dataLen);
		mem_rec.address = rec.address;
		mem_rec.dataLen = rec.dataLen;
		mem_rec.type = rec.type;
		mem_rec.checksum = 0;

		mem_rec.checksum = Checksum_IHexRecord(&mem_rec);
		if (mem_rec.checksum != rec.checksum) {
			snprintf(log, sizeof log, "IHEX record checksum is "
			         "not correct: 0x%X (memory) != 0x%X (file)",
			         mem_rec.checksum, rec.checksum);
			MSIM_LOG_FATAL(log);
			MSIM_LOG_FATAL("file record:");
			MSIM_IHEX_PrintRecord(&rec);
			MSIM_LOG_FATAL("memory record:");
			MSIM_IHEX_PrintRecord(&mem_rec);
			return -1;
		}
	}
	return 0;
}

static int handle_irq(struct MSIM_AVR *mcu)
{
	unsigned int i;
	int ret;

	/* Let's try to find IRQ with the highest priority:
	 * i == 0		highest priority
	 * i == AVR_IRQ_NUM-1	lowest priority */
	for (i = 0; i < MSIM_AVR_IRQNUM; i++) {
		if (mcu->intr.irq[i] > 0) {
			break;
		}
	}

	ret = 0;
	if (i != MSIM_AVR_IRQNUM) {
		/* Clear selected IRQ */
		mcu->intr.irq[i] = 0;

		/* Disable interrupts globally.
		 * It is not applicable for the AVR XMEGA cores. */
		if (!mcu->xmega) {
			UPDATE_SREG(mcu, GLOB_INT, 0);
		}

		/* Push PC onto the stack */
		MSIM_AVR_StackPush(mcu, (uint8_t)(mcu->pc & 0xFF));
		MSIM_AVR_StackPush(mcu, (uint8_t)((mcu->pc >> 8)&0xFF));
		if (mcu->pc_bits > 16) {
			MSIM_AVR_StackPush(mcu, (uint8_t)((mcu->pc>>16)&0xFF));
		}

		/* Load interrupt vector to PC */
		mcu->pc = mcu->intr.ivt+(i*2);

		/* Switch MCU to step mode if it's necessary */
		if (mcu->intr.trap_at_isr && mcu->state == AVR_RUNNING) {
			mcu->state = AVR_MSIM_STEP;
		}
	} else {
		/* No IRQ, do nothing */
		ret = 2;
	}
	return ret;
}

void MSIM_AVR_StackPush(struct MSIM_AVR *mcu, unsigned char val)
{
	unsigned int sp;

	sp = (unsigned int)((*mcu->spl) | (*mcu->sph<<8));
	mcu->dm[sp--] = val;
	*mcu->spl = (unsigned char) (sp & 0xFF);
	*mcu->sph = (unsigned char) (sp >> 8);
}

uint8_t MSIM_AVR_StackPop(struct MSIM_AVR *mcu)
{
	unsigned int sp;
	unsigned char v;

	sp = (unsigned int)((*mcu->spl) | (*mcu->sph<<8));
	v = mcu->dm[++sp];
	*mcu->spl = (unsigned char)(sp & 0xFF);
	*mcu->sph = (unsigned char)(sp >> 8);
	return v;
}

void MSIM_AVR_PrintParts(void)
{
	uint32_t i;

	for (i = 0; i < sizeof(init_funcs)/sizeof(init_funcs[0]); i++) {
		printf("%s %s\n", init_funcs[i].partno, init_funcs[i].name);
	}
}
