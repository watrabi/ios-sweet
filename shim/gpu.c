/*
 * bonito GPU probe, stage 1 (shim, -DGPUPROBE): Adreno 615 on SDM670.
 *
 * Port of ios-flame's shim/gpuprobe.c stage 1 (NWylynko/ios-flame, MIT;
 * written there for the Adreno 640 / SM8150). Powers only the GPU's CX
 * side - no GX rail, no GMU firmware, nothing executes on the GPU:
 *
 *   GCC   gpu_cfg_ahb 0x71004, ddrss_gpu_axi 0x44038 (sm8150: 0x71154),
 *         gpu_memnoc_gfx 0x7100c, gpu_snoc_dvm_gfx 0x71018 (gcc-sdm845.c)
 *   GPUCC (0x5090000) CX GDSC 0x106c on, gmu_clk_src 0x1120 = XO /1,
 *         cxo 0x109c and cx_gmu 0x1098 branches (gpucc-sdm845.c; the
 *         sm8150 "ahb" branch 0x1078 has no sdm845 counterpart: skipped)
 *
 * then reads GMU registers, checks that the GMU's ITCM/DTCM take writes and
 * reads the Adreno SMMU's ID registers. Addresses from mainline sdm670.dtsi:
 * GPU 0x5000000, GMU 0x506a000 (= GPU + 0x6a000, the same relative layout
 * as sm8150, so ios-flame's dword offsets apply), SMMU 0x5040000.
 *
 * An access to an unclocked block can stall the bus for good: every risky
 * step prints its line first, so the last line on screen says where.
 */
#include <stdint.h>
#include "console.h"
#include "gpu.h"

#define GCC		0x00100000UL
#define GPUCC		0x05090000UL
#define GPU		0x05000000UL
#define GMU_REG(o)	(GPU + 4UL * (o))
#define SMMU		0x05040000UL

#define GPUCC_CX_GDSCR	0x106c
#define GPUCC_GX_GDSCR	0x100c
#define GPUCC_GMU_RCG	0x1120
#define GPUCC_CXO	0x109c
#define GPUCC_CX_GMU	0x1098

#define GMU_ITCM	0x1b400
#define GMU_DTCM	0x1c400
#define GMU_FW_VERSION	0x1d3f8
#define GMU_INIT_RESULT	0x1f81c
#define GMU_RPMH_STATE	0x1f8ec
#define GMU_GENERAL_7	0x1f9cc
#define GMU_CX_BUSY	0x23b0c

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v)
{
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void mdelay(unsigned ms)
{
	uint64_t end = ticks() + (uint64_t)ms * 19200;
	while (ticks() < end)
		;
}

static int wait_bit(uintptr_t reg, uint32_t mask, int set, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if (!!(rd32(reg) & mask) == set)
			return 0;
		mdelay(1);
	}
	return -1;
}

static void branch_on(const char *name, uintptr_t reg)
{
	con_str("  on", name);
	wr32(reg, rd32(reg) | 1);
	int rc = wait_bit(reg, 1u << 31, 0, 10);
	con_hex2(rc ? "  HALTED" : "  ok", "reg=", (uint32_t)reg, "val=", rd32(reg));
}

void gpu_probe(void)
{
	con_line("GPU PROBE stage 1  Adreno 615: CX power, GMU regs, SMMU", GREEN);
	con_hex2("GPUCC", "cx_gdsc=", rd32(GPUCC + GPUCC_CX_GDSCR), "gx_gdsc=", rd32(GPUCC + GPUCC_GX_GDSCR));
	con_hex2(" gmu", "rcg=", rd32(GPUCC + GPUCC_GMU_RCG), "cfg=", rd32(GPUCC + GPUCC_GMU_RCG + 4));
	con_hex2(" br", "cxo=", rd32(GPUCC + GPUCC_CXO), "cx_gmu=", rd32(GPUCC + GPUCC_CX_GMU));
	con_hex2("GCC", "cfg_ahb=", rd32(GCC + 0x71004), "axi=", rd32(GCC + 0x44038));
	con_hex2(" gcc", "memnoc=", rd32(GCC + 0x7100c), "snoc=", rd32(GCC + 0x71018));

	branch_on("gcc gpu_cfg_ahb", GCC + 0x71004);
	branch_on("gcc ddrss_gpu_axi", GCC + 0x44038);
	branch_on("gcc gpu_memnoc_gfx", GCC + 0x7100c);
	branch_on("gcc gpu_snoc_dvm_gfx", GCC + 0x71018);

	/* CX GDSC: clear SW_COLLAPSE (bit 0), wait for PWR_ON (bit 31). */
	con_str("STEP", "CX GDSC on");
	uintptr_t gdsc = GPUCC + GPUCC_CX_GDSCR;
	wr32(gdsc, rd32(gdsc) & ~1u);
	int rc = wait_bit(gdsc, 1u << 31, 1, 100);
	con_hex2(rc ? "  TIMEOUT" : "  on", "gdsc=", rd32(gdsc), 0, 0);

	/* gmu_clk_src: source 0 (XO), HID divider 1, then update. */
	con_str("STEP", "gmu_clk_src = XO");
	uintptr_t rcg = GPUCC + GPUCC_GMU_RCG;
	wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | 1u);
	wr32(rcg, rd32(rcg) | 1u);
	rc = wait_bit(rcg, 1, 0, 10);
	con_hex2(rc ? "  UPD TIMEOUT" : "  ok", "rcg=", rd32(rcg), "cfg=", rd32(rcg + 4));

	branch_on("gpucc cxo", GPUCC + GPUCC_CXO);
	branch_on("gpucc cx_gmu", GPUCC + GPUCC_CX_GMU);

	con_str("STEP", "GMU register reads");
	con_hex2("GMU", "fw_ver=", rd32(GMU_REG(GMU_FW_VERSION)), "init=", rd32(GMU_REG(GMU_INIT_RESULT)));
	con_hex2(" gmu", "rpmh=", rd32(GMU_REG(GMU_RPMH_STATE)), "gen7=", rd32(GMU_REG(GMU_GENERAL_7)));
	con_hex2(" gmu", "cx_busy=", rd32(GMU_REG(GMU_CX_BUSY)), 0, 0);

	con_str("STEP", "ITCM/DTCM write test");
	uintptr_t itcm = GMU_REG(GMU_ITCM), dtcm = GMU_REG(GMU_DTCM);
	uint32_t i0 = rd32(itcm), d0 = rd32(dtcm);
	wr32(itcm, 0x464c414d);
	wr32(dtcm, 0x45475055);
	con_hex2("ITCM", "was=", i0, "now=", rd32(itcm));
	con_hex2("DTCM", "was=", d0, "now=", rd32(dtcm));
	con_str(" want", "ITCM 464C414D, DTCM 45475055");

	con_str("STEP", "Adreno SMMU ID registers");
	con_hex2("SMMU", "cr0=", rd32(SMMU + 0x0), "idr0=", rd32(SMMU + 0x20));
	con_hex2(" smmu", "idr1=", rd32(SMMU + 0x24), "idr2=", rd32(SMMU + 0x28));
	con_hex2(" smmu", "idr7=", rd32(SMMU + 0x3c), 0, 0);

	con_str("GPU", "stage 1 done - nothing ran on the GPU");
}
