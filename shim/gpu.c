/*
 * bonito GPU probe, stages 1-2 (shim, -DGPUPROBE): Adreno 615 on SDM670.
 * Stage 2 boots the GMU (legacy firmware a630_gmu.bin, see gmu_boot()).
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


/* ---- stage 2: GMU boot (legacy GMU: A615/A630 family) ---- */

#define SMMU_GR1	(SMMU + 0x1000)
#define SMMU_CB(n)	(SMMU + 0x8000 + 0x1000UL * (n))
#define SMR(n)		(SMMU + 0x800 + 4 * (n))
#define S2CR(n)		(SMMU + 0xc00 + 4 * (n))
#define CBAR(n)		(SMMU_GR1 + 4 * (n))
#define CBA2R(n)	(SMMU_GR1 + 0x800 + 4 * (n))
#define CB_SCTLR	0x00
#define CB_TCR2		0x10
#define CB_TTBR0	0x20
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_FSR		0x58
#define CB_FAR		0x60
#define CB_FSYNR0	0x68

#define GMU_SID		5		/* sdm670.dtsi: gmu iommus = <&adreno_smmu 5> */
#define GMU_SID_MASK	0	/* bonito SMMU: IDR0.NUMSIDB = 3 (3-bit stream ids); a mask bit
				   beyond that makes the SMR write be dropped (sm8150 used 0x400) */

/* GMU page tables + buffers: verified-free RAM (memwalk), 1 MiB. */
#define GMEM		0x170000000UL
#define GMEM_L1		(GMEM + 0x0000)
#define GMEM_L2_0	(GMEM + 0x1000)		/* VA 0x00000000-0x3fffffff */
#define GMEM_L2_1	(GMEM + 0x2000)		/* VA 0x40000000-0x7fffffff */
#define GMEM_L3_LOW	(GMEM + 0x3000)		/* VA 0x00000000-0x001fffff */
#define GMEM_ICACHE	(GMEM + 0x10000)
#define GMEM_DCACHE	(GMEM + 0x50000)
#define VA_ICACHE	0x4000u
#define VA_DCACHE	0x44000u
#define CACHE_SIZE	0x3c000u
#define GMEM_L3_6	(GMEM + 0x4000)		/* VA 0x60000000-0x601fffff */
#define GMEM_DUMMY	(GMEM + 0x90000)
#define GMEM_LOG	(GMEM + 0x91000)
#define GMEM_HFI	(GMEM + 0x95000)
#define GMEM_DEBUG	(GMEM + 0x99000)
#define GMEM_SIZE	0x100000UL
#define VA_DUMMY	0x60000000u
#define VA_LOG		0x60001000u
#define VA_HFI		0x60005000u
#define VA_DEBUG	0x60009000u	/* legacy GMU debug buffer (mainline gmu->debug) */

/* a6xx_gmu.c: rscc = gmu->mmio + 0x23000 before A650; sdm670.dtsi gmu_pdc / gmu_pdc_seq */
#define RSCC		(0x0506a000UL + 0x23000)
#define RSCC_REG(o)	(RSCC + 4UL * (o))
#define PDC		0x0b280000UL
#define PDC_REG(o)	(PDC + 4UL * (o))
#define PDC_SEQ		0x0b480000UL
#define PDC_ADDR_OFF	0x30080		/* A615: not A618/A640 (0x30090) */

#define GMU_SYSRESET	0x1f800
#define GMU_BOOT_CFG	0x1f801
#define GMU_CM3_CFG	0x1f82d
#define GMU_PWRCOL_CTRL	0x1f8c0
#define GMU_RPMH_CTRL	0x1f8e8
#define GMU_CP_MSG	0x1f900
#define GMU_CP_RESP	0x1f901
#define GMU_HFI_CTRL	0x1f980
#define GMU_SFR_ADDR	0x1f982
#define GMU_QTBL_INFO	0x1f984
#define GMU_QTBL_ADDR	0x1f985
#define GMU_SYS_BUS	0x1f40f
#define GMU_ICACHE_CFG	0x1f400
#define GMU_DCACHE_CFG	0x1f401
#define GMU_RSCC_REQ	0x23b07
#define GMU_RSCC_ACK	0x23b08
#define GMU_FENCE0	0x23b11
#define GMU_CHIPID	0x06015000u	/* chip 0x06010500 in the GMU's packing */

extern const uint8_t gmu_fw[], gmu_fw_end[];
static void gmu_power_up(void);
static void crumb(const char *what);

/* PC-relative address of an embedded symbol: keeps the compiler from folding
 * symbol arithmetic into an absolute literal (the shim is not relocated). */
#define PCREL(sym) ({ const uint8_t *_p; \
	__asm__("adrp %0, " #sym "\n\tadd %0, %0, :lo12:" #sym : "=r"(_p)); _p; })

static void zero32(uintptr_t pa, uint64_t size)
{
	for (uint64_t o = 0; o < size; o += 4)
		*(volatile uint32_t *)(pa + o) = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void wr64(uintptr_t pa, uint64_t v)
{
	wr32(pa, (uint32_t)v);
	wr32(pa + 4, (uint32_t)(v >> 32));
}

/* Stage-1 AArch64 4K page: AF, inner shareable, AttrIndx 0, AP=01 (data). */
static void map_pages_ap(uintptr_t l3, uint32_t va, uintptr_t pa, uint32_t size, int priv)
{
	for (uint32_t o = 0; o < size; o += 0x1000) {
		uint32_t idx = ((va + o) >> 12) & 0x1ff;
		/* priv: AP=00 (privileged RW, executable); else AP=01 */
		wr64(l3 + 8 * idx, (pa + o) | (1u << 10) | (3u << 8) | (priv ? 0 : (1u << 6)) | 3u);
	}
}

static void map_pages(uintptr_t l3, uint32_t va, uintptr_t pa, uint32_t size)
{
	for (uint32_t o = 0; o < size; o += 0x1000) {
		uint32_t idx = ((va + o) >> 12) & 0x1ff;
		wr64(l3 + 8 * idx, (pa + o) | (1u << 10) | (3u << 8) | (1u << 6) | 3u);
	}
}

static int gmu_smr_ok;
static int gmu_cb = -1;

static int smmu_setup(void)
{
	uint32_t nsmr = rd32(SMMU + 0x20) & 0xff, ncb = rd32(SMMU + 0x24) & 0xff;
	int smr = -1, cb = -1;
	uint32_t used_cb = 0;
	con_hex2("SMMU", "nsmr=", nsmr, "ncb=", ncb);
	for (uint32_t i = 0; i < nsmr && i < 16; i++) {
		uint32_t m = rd32(SMR(i)), c = rd32(S2CR(i));
		con_hex2(" smr", "smr=", m, "s2cr=", c);
		if ((m >> 31) && ((c >> 16) & 3) == 0)
			used_cb |= 1u << (c & 0xff);
		if (smr < 0 && !(m >> 31))
			smr = (int)i;
	}
	for (uint32_t i = 0; i < ncb && i < 16; i++)
		if (cb < 0 && !(used_cb & (1u << i)) && !(rd32(SMMU_CB(i) + CB_SCTLR) & 1))
			cb = (int)i;
	con_hex2(" use", "smr=", (uint32_t)smr, "cb=", (uint32_t)cb);
	if (smr < 0 || cb < 0)
		return -1;

	zero32(GMEM, GMEM_SIZE);
	wr64(GMEM_L1 + 0, GMEM_L2_0 | 3);
	wr64(GMEM_L1 + 8, GMEM_L2_1 | 3);
	wr64(GMEM_L2_0 + 0, GMEM_L3_LOW | 3);
	/* icache/dcache windows: mainline skips them for legacy GMUs ("if you don't get
	 * random pagefaults"); downstream maps them. Zero-filled, privileged + exec. */
	map_pages_ap(GMEM_L3_LOW, VA_ICACHE, GMEM_ICACHE, CACHE_SIZE, 1);
	map_pages_ap(GMEM_L3_LOW, VA_DCACHE, GMEM_DCACHE, CACHE_SIZE, 1);
	wr64(GMEM_L2_1 + 8 * ((VA_DUMMY >> 21) & 0x1ff), GMEM_L3_6 | 3);
	map_pages(GMEM_L3_6, VA_DUMMY, GMEM_DUMMY, 0x1000);
	map_pages(GMEM_L3_6, VA_LOG, GMEM_LOG, 0x4000);
	map_pages(GMEM_L3_6, VA_HFI, GMEM_HFI, 0x4000);
	map_pages(GMEM_L3_6, VA_DEBUG, GMEM_DEBUG, 0x4000);

	uintptr_t c = SMMU_CB(cb);
	wr32(c + CB_SCTLR, 0);
	wr32(CBA2R(cb), 1);						/* AArch64 */
	wr32(CBAR(cb), (1u << 16) | (0xfu << 12) | (3u << 8) | ((uint32_t)cb + 1));
	wr32(c + CB_TCR2, 2);						/* 40-bit PA */
	wr64(c + CB_TTBR0, GMEM_L1);
	wr32(c + CB_TCR, 32 | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23));
	wr32(c + CB_MAIR0, 0x0000ff44);
	wr32(c + CB_FSR, 0xffffffff);
	wr32(c + CB_SCTLR, 1 | 2 | 4 | 0x20 | 0x40);
	wr32(SMMU + 0x68, 0);						/* TLBIALLNSNH */
	wr32(SMMU + 0x70, 0);						/* TLBGSYNC */
	wait_bit(SMMU + 0x74, 1, 0, 10);
	/* Stage-2 run showed the SMR write did not stick (read back 0): probe it. */
	con_hex2(" scr0", "cr0=", rd32(SMMU + 0x0), "acr=", rd32(SMMU + 0x10));
	wr32(S2CR(smr), (uint32_t)cb);
	con_hex2(" s2cr", "wrote=", (uint32_t)cb, "read=", rd32(S2CR(smr)));
	uint32_t want = (1u << 31) | (GMU_SID_MASK << 16) | GMU_SID;
	wr32(SMR(smr), want);
	con_hex2(" smr", "wrote=", want, "read=", rd32(SMR(smr)));
	/* other encodings: no mask, then the id alone */
	if (rd32(SMR(smr)) != want) {
		wr32(SMR(smr), (1u << 31) | GMU_SID);
		con_hex2(" smr", "nomask=", (1u << 31) | GMU_SID, "read=", rd32(SMR(smr)));
		wr32(SMR(smr), GMU_SID);
		con_hex2(" smr", "id only=", GMU_SID, "read=", rd32(SMR(smr)));
		/* another SMR index */
		int o = smr + 1 < (int)nsmr ? smr + 1 : 0;
		wr32(SMR(o), want);
		con_hex2(" smr+1", "idx=", (uint32_t)o, "read=", rd32(SMR(o)));
		wr32(SMR(o), 0);
		wr32(SMR(smr), want);
	}
	con_hex2(" now", "smr=", rd32(SMR(smr)), "sctlr=", rd32(c + CB_SCTLR));
	gmu_smr_ok = rd32(SMR(smr)) == want;
	gmu_cb = cb;
	return cb;
}

/* a6xx_gmu_rpmh_init for a non-A618 A6xx gen1 (A615): PDC offset 0x30080. */
static void rpmh_init(void)
{
	con_hex2("RSCC", "status0=", rd32(RSCC_REG(0x0004)), "seqbusy=", rd32(RSCC_REG(0x0101)));
	wr32(RSCC_REG(0x0004), 1u << 24);
	wr32(RSCC_REG(0x000b), 1);
	wr32(RSCC_REG(0x000e), 0);
	wr32(RSCC_REG(0x000d), 0);
	wr32(RSCC_REG(0x000e + 2), 0);
	wr32(RSCC_REG(0x000d + 2), 0);
	wr32(RSCC_REG(0x000e + 4), 0x80000000);
	wr32(RSCC_REG(0x000d + 4), 0);
	wr32(RSCC_REG(0x0100), 0);
	wr32(RSCC_REG(0x0008), 0x4520);
	wr32(RSCC_REG(0x0009), 0x4510);
	wr32(RSCC_REG(0x000a), 0x4514);
	wr32(RSCC_REG(0x0180), 0xa7a506a0);
	wr32(RSCC_REG(0x0181), 0xa1e6a6e7);
	wr32(RSCC_REG(0x0182), 0xa2e081e1);
	wr32(RSCC_REG(0x0183), 0xe9a982e2);
	wr32(RSCC_REG(0x0184), 0x0020e8a8);

	con_str("STEP", "PDC (0xb280000) / PDC_SEQ (0xb480000)");
	con_hex2("PDC", "enable=", rd32(PDC_REG(0x1140)), 0, 0);
	wr32(PDC_SEQ + 0, 0xfebea1e1);
	wr32(PDC_SEQ + 4, 0xa5a4a3a2);
	wr32(PDC_SEQ + 8, 0x8382a6e0);
	wr32(PDC_SEQ + 12, 0xbce3e284);
	wr32(PDC_SEQ + 16, 0x002081fc);

	wr32(PDC_REG(0x1573), 7);
	wr32(PDC_REG(0x1574), 0);
	wr32(PDC_REG(0x1572), 0);
	wr32(PDC_REG(0x1575), 0x10108);
	wr32(PDC_REG(0x1576), 0x30010);
	wr32(PDC_REG(0x1577), 1);
	wr32(PDC_REG(0x1575 + 4), 0x10108);
	wr32(PDC_REG(0x1576 + 4), 0x30000);
	wr32(PDC_REG(0x1577 + 4), 0);
	wr32(PDC_REG(0x1575 + 8), 0x10108);
	wr32(PDC_REG(0x1576 + 8), PDC_ADDR_OFF);
	wr32(PDC_REG(0x1577 + 8), 0);

	wr32(PDC_REG(0x15d7), 7);
	wr32(PDC_REG(0x15d8), 0);
	wr32(PDC_REG(0x15d6), 0);
	wr32(PDC_REG(0x15d9), 0x10108);
	wr32(PDC_REG(0x15da), 0x30010);
	wr32(PDC_REG(0x15db), 2);
	wr32(PDC_REG(0x15d9 + 4), 0x10108);
	wr32(PDC_REG(0x15da + 4), 0x30000);
	wr32(PDC_REG(0x15db + 4), 3);
	wr32(PDC_REG(0x15d9 + 8), 0x10108);
	wr32(PDC_REG(0x15da + 8), PDC_ADDR_OFF);
	wr32(PDC_REG(0x15db + 8), 3);

	wr32(PDC_REG(0x1148), 0);
	wr32(PDC_REG(0x1140), 0x80000001);
}

static void hfi_table(void)
{
	uintptr_t t = GMEM_HFI;
	wr32(t + 0, 0);
	wr32(t + 4, 6 * 4 + 3 * 12 * 4);
	wr32(t + 8, 6);
	wr32(t + 12, 12);
	wr32(t + 16, 3);
	wr32(t + 20, 3);
	for (int q = 0; q < 3; q++) {
		uintptr_t h = t + 24 + 48 * (uintptr_t)q;
		wr32(h + 0, 1);
		wr32(h + 4, VA_HFI + 0x1000u * (uint32_t)(q + 1));
		/* legacy GMU queue ids: command 0, response 4, debug 5 (mainline a6xx_hfi_init;
		 * the A640's newer firmware uses 0/1/2) */
		static const uint32_t qid[3] = { 0, 4, 5 };
		wr32(h + 8, (10u << 8) | qid[q]);
		wr32(h + 12, 0x1000 >> 2);
		wr32(h + 24, 1);
		wr32(h + 28, 1);
		wr32(h + 32, 1);
	}
}

static void gmu_boot(void)
{
	crumb("stage 2 GMU boot");
	con_line("GPU stage 2  GMU boot (legacy, a630_gmu.bin)", GREEN);
	con_str("STEP", "SMMU context bank for GMU stream 5");
	int cb = smmu_setup();
	if (cb < 0) {
		con_str("SMMU", "no free SMR/context bank - stop");
		return;
	}
	con_str("STEP", "RSCC (0x508d000)");
	rpmh_init();
	hfi_table();

	/* a6xx_rpmh_start only wakes the RSC after a6xx_rpmh_stop put the PDC to
	 * sleep (GMU_STATUS_PDC_SLEEP); a cold boot skips it. Earlier probes sent
	 * the request anyway and it never acked: just report the state now. */
	wr32(GMU_REG(GMU_GENERAL_7), 1);
	con_hex2("RSC", "ack=", rd32(GMU_REG(GMU_RSCC_ACK)), "pdc=", rd32(PDC_REG(0x1140)));
	con_hex2(" rsc", "status0=", rd32(RSCC_REG(0x0004)), "seqbusy=", rd32(RSCC_REG(0x0101)));

	/* legacy: the whole image is ITCM content (<= 32 KiB) */
	const uint8_t *fw = PCREL(gmu_fw), *fw_end = PCREL(gmu_fw_end);
	uint32_t size = (uint32_t)(fw_end - fw);
	con_hex2("FW", "size=", size, "first=", *(const uint32_t *)fw);
	if (size == 0 || size > 0x8000)
		return;
	for (uint32_t o = 0; o < size; o += 4)
		wr32(GMU_REG(GMU_ITCM) + o, *(const volatile uint32_t *)(fw + o));
	con_hex2(" itcm", "[0]=", rd32(GMU_REG(GMU_ITCM)), "[1]=", rd32(GMU_REG(GMU_ITCM) + 4));

	wr32(GMU_REG(GMU_INIT_RESULT), 0);
	wr32(GMU_REG(GMU_BOOT_CFG), 2);
	wr32(GMU_REG(GMU_QTBL_ADDR), VA_HFI);
	wr32(GMU_REG(GMU_QTBL_INFO), 1);
	wr32(GMU_REG(GMU_FENCE0), (1u << 31) | (0xau << 18) | 0xa0);
	wr32(GMU_REG(GMU_CM3_CFG), 0x4052);
	wr32(GMU_REG(GMU_SFR_ADDR), GMU_CHIPID);
	wr32(GMU_REG(GMU_CP_MSG), VA_LOG | (0x4000 / 0x1000 - 1));
	/* a6xx_gmu_power_config: no IFPC, RPMh interface + votes on */
	wr32(GMU_REG(GMU_SYS_BUS), 1);
	wr32(GMU_REG(GMU_ICACHE_CFG), 1);
	wr32(GMU_REG(GMU_DCACHE_CFG), 1);
	wr32(GMU_REG(GMU_PWRCOL_CTRL), 0x9c40400);
	wr32(GMU_REG(GMU_RPMH_CTRL), rd32(GMU_REG(GMU_RPMH_CTRL)) |
	     1 | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11));

	uint32_t v = rd32(GMU_REG(GMU_DTCM) + 0xff8 * 4);
	uint32_t mask = v <= 0x20010004 ? 0xffffffff : 0x1ff;
	uint32_t want = v <= 0x20010004 ? 0xbabeface : 0x100;
	con_hex2("CM3", "dtcm[ff8]=", v, "want=", want);
	con_str("STEP", "releasing CM3 reset");
	wr32(GMU_REG(GMU_SYSRESET), 1);
	wr32(GMU_REG(GMU_CP_RESP), 0);
	wr32(GMU_REG(GMU_SYSRESET), 0);
	uint32_t r = 0;
	int ok = 0;
	for (int i = 0; i < 2000; i++) {
		r = rd32(GMU_REG(GMU_INIT_RESULT));
		if ((r & mask) == want) {
			ok = 1;
			break;
		}
		mdelay(1);
	}
	uintptr_t c = SMMU_CB(cb);
	con_hex2(ok ? "GMU BOOTED" : "GMU TIMEOUT", "init=", r, "hfi=", rd32(GMU_REG(GMU_HFI_CTRL)));
	con_hex2(" fw", "version=", rd32(GMU_REG(GMU_FW_VERSION)), "rpmh=", rd32(GMU_REG(GMU_RPMH_STATE)));
	con_hex2(" cb", "fsr=", rd32(c + CB_FSR), "far=", rd32(c + CB_FAR));
	con_hex2(" cb", "fsynr0=", rd32(c + CB_FSYNR0), "gfsr=", rd32(SMMU + 0x48));
	con_hex2(" log", "[0]=", rd32(GMEM_LOG), "[1]=", rd32(GMEM_LOG + 4));
	if (!ok)
		return;
	con_str("NEXT", "stage 3 on a new page in 20 s (photo this one)");
	mdelay(20000);
	con_clear(RED);
	gmu_power_up();
}

/* ---- stage 3: legacy power-up: GX rail, SPTPRAC, HFI v1, OOB GPU ---- */

#define GMU_BOOT_SLUMBER	0x1cbf8
#define GMU_GX_VOTE_IDX		0x1cbf9
#define GMU_MX_VOTE_IDX		0x1cbfa
#define GMU_SPTPRAC_CTRL	0x1a881
#define GMU_SPTPRAC_STATUS	0x1f8d0
#define GMU_G2H_CLR		0x1f991
#define GMU_G2H_INFO		0x1f992
#define GMU_H2G_SET		0x1f994
#define GMU_HFI_CTRL_INIT	0x1f986
#define G2H_CM3_FAULT		(1u << 23)
#define QHDR(q)			(GMEM_HFI + 24 + 48 * (q))
#define QDATA(q)		(GMEM_HFI + 0x1000 * ((q) + 1))
#define QSIZE			1024
#define CMD_DB			0x85fe0000UL	/* sdm670.dtsi aop_cmd_db_mem (qcom,cmd-db) */

/* sdm670.dtsi GPU OPPs every speed bin has (opp-supported-hw 0xf), "off" first. */
#define NGPU 4
static const uint32_t gpu_khz[NGPU] = { 0, 267000, 355000, 430000 };
static const uint32_t gpu_lvl[NGPU] = { 0, 64 /* LOW_SVS */, 128 /* SVS */, 192 /* SVS_L1 */ };
static const uint32_t gmu_khz[2] = { 0, 200000 };
static const uint32_t gmu_lvl[2] = { 0, 48 /* MIN_SVS */ };

static uint32_t hfi_seq;
static uint32_t hfi_err[4];	/* last F2H_MSG_ERROR (id 100): header, words 1..3 */

/*
 * Breadcrumb in RAM that survives a warm reset (memwalk-verified, outside
 * the shim, GMU buffers and XNU's use during a probe boot): "BRDC" + the
 * step name. The next probe boot shows it before doing anything.
 */
#define CRUMB		0x17e000000UL	/* < 0x17e2c0000, the end of the memwalked range */
#define CRUMB_MAGIC	0x43445242u	/* "BRDC" */

static void __attribute__((noinline)) crumb(const char *what)
{
	/* hide the pointer: no folding of (literal - CRUMB) into absolute constants */
	__asm__ volatile("" : "+r"(what));
	volatile char *c = (volatile char *)(CRUMB + 8);
	int i = 0;
	for (; what[i] && i < 60; i++)
		c[i] = what[i];
	c[i] = 0;
	*(volatile uint32_t *)CRUMB = CRUMB_MAGIC;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* stage-3 step: breadcrumb, line on screen, then a pause to film it */
static void step(const char *what)
{
	crumb(what);
	con_str("STEP", what);
	mdelay(1500);
}

static int cmddb_aux(const char *id, uintptr_t *aux, uint32_t *len)
{
	uintptr_t db = CMD_DB;
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uintptr_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (int r = 0; r < 8; r++) {
		uintptr_t rh = db + 8 + 16 * (uintptr_t)r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uintptr_t ent = data + hoff + 24 * e;
			int match = 1;
			for (int i = 0; i < 8; i++) {
				char c = (char)(rd32(ent + (uintptr_t)(i & ~3)) >> (8 * (i & 3)));
				if (c != id[i]) {
					match = 0;
					break;
				}
				if (!id[i])
					break;
			}
			if (!match)
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uintptr_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

/* a6xx_gmu_rpmh_arc_votes_init */
static int arc_votes(uint32_t *votes, const uint32_t *lvl, int n, const char *pri_id, const char *sec_id)
{
	uintptr_t pri, sec;
	uint32_t plen, slen;
	if (cmddb_aux(pri_id, &pri, &plen) || cmddb_aux(sec_id, &sec, &slen))
		return -1;
	plen /= 2;
	slen /= 2;
	for (int i = 0; i < n; i++) {
		uint32_t pindex = 0, sindex = 0, j;
		for (j = 0; j < plen; j++)
			if (aux16(pri, j) >= lvl[i]) {
				pindex = j;
				break;
			}
		if (j == plen)
			return -2;
		for (j = 0; j < slen; j++) {
			if (aux16(sec, j) >= lvl[i]) {
				sindex = j;
				break;
			} else if (aux16(sec, j)) {
				sindex = j;
			}
		}
		votes[i] = (aux16(pri, pindex) << 16) | (sindex << 8) | pindex;
	}
	return 0;
}

static int hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords)
{
	uint32_t seq = ++hfi_seq;
	msg[0] = (seq << 20) | (dwords << 8) | id;
	uintptr_t h = QHDR(0);
	uint32_t w = rd32(h + 44);
	for (uint32_t i = 0; i < dwords; i++) {
		wr32(QDATA(0) + 4 * w, msg[i]);
		w = (w + 1) % QSIZE;
	}
	for (; w % 4; w = (w + 1) % QSIZE)
		wr32(QDATA(0) + 4 * w, 0xfafafafa);
	wr32(h + 44, w);
	wr32(GMU_REG(GMU_H2G_SET), 1);
	for (int t = 0; t < 1000; t++) {
		if (rd32(GMU_REG(GMU_G2H_INFO)) & 1) {
			wr32(GMU_REG(GMU_G2H_CLR), 1);
			uintptr_t rh = QHDR(1);
			while (rd32(rh + 40) != rd32(rh + 44)) {
				uint32_t r = rd32(rh + 40), hdr = rd32(QDATA(1) + 4 * r);
				uint32_t n = (hdr >> 8) & 0xff, buf[8] = { 0 };
				for (uint32_t i = 0; i < n; i++) {
					uint32_t v = rd32(QDATA(1) + 4 * r);
					if (i < 8)
						buf[i] = v;
					r = (r + 1) % QSIZE;
				}
				r = ((r + 3) & ~3u) % QSIZE;
				wr32(rh + 40, r);
				if ((hdr & 0xff) == 100) {
					for (int k = 0; k < 4; k++)
						hfi_err[k] = buf[k];
					continue;
				}
				if ((buf[1] >> 20) == seq)
					return (int)buf[2];
			}
		}
		mdelay(1);
	}
	return -1000;
}

/* legacy a6xx_gmu_set_oob: request bit -> ack bit in GMU2HOST_INTR_INFO */
static int oob(unsigned req, unsigned ack)
{
	wr32(GMU_REG(GMU_H2G_SET), 1u << req);
	int rc = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << ack, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << ack);
	return rc;
}

static void gmu_power_up(void)
{
	static uint32_t msg[192];
	uint32_t gx[NGPU], cx[2];
	int rc;

	con_line("GPU stage 3  legacy power-up: GX rail, SPTPRAC, HFI v1, OOB GPU", GREEN);
	con_hex2("CMDDB", "at=", (uint32_t)CMD_DB, "magic=", rd32(CMD_DB + 4));
	if ((rc = arc_votes(gx, gpu_lvl, NGPU, "gfx.lvl", "mx.lvl")) ||
	    (rc = arc_votes(cx, gmu_lvl, 2, "cx.lvl", "mx.lvl"))) {
		con_hex("CMDDB fail", (uint32_t)rc);
		return;
	}
	con_hex2("VOTES", "gx_max=", gx[NGPU - 1], "cx=", cx[1]);

	wr32(GMU_REG(GMU_G2H_CLR), 0xffffffff);
	con_hex2("G2H", "info=", rd32(GMU_REG(GMU_G2H_INFO)), "fault=", rd32(GMU_REG(GMU_G2H_INFO)) & G2H_CM3_FAULT);

	/* a6xx_gmu_gfx_rail_on: max-level indices + OOB BOOT_SLUMBER (22 -> 30) */
	step("gfx rail on (OOB boot-slumber)");
	wr32(GMU_REG(GMU_BOOT_SLUMBER), 0);
	wr32(GMU_REG(GMU_GX_VOTE_IDX), gx[NGPU - 1] & 0xff);
	wr32(GMU_REG(GMU_MX_VOTE_IDX), (gx[NGPU - 1] >> 8) & 0xff);
	rc = oob(22, 30);
	con_hex2(rc ? " slumber TIMEOUT" : " slumber acked", "info=", rd32(GMU_REG(GMU_G2H_INFO)), "rpmh=", rd32(GMU_REG(GMU_RPMH_STATE)));

	/* a6xx_sptprac_enable */
	step("SPTPRAC enable");
	wr32(GMU_REG(GMU_SPTPRAC_CTRL), 0x778000);
	rc = -1;
	for (int i = 0; i < 100; i++)
		if ((rd32(GMU_REG(GMU_SPTPRAC_STATUS)) & 0x38) == 0x28) {
			rc = 0;
			break;
		} else
			mdelay(1);
	con_hex2(rc ? " sptprac FAIL" : " sptprac on", "status=", rd32(GMU_REG(GMU_SPTPRAC_STATUS)), 0, 0);

	/* a6xx_gmu_hfi_start: the GMU sets up its queues before any message */
	if (!gmu_smr_ok) {
		con_str("STOP", "GMU stream has no SMR - DRAM access would reset the SoC");
		return;
	}
	step("HFI_CTRL_INIT");
	wr32(GMU_REG(GMU_HFI_CTRL_INIT), 1);
	rc = wait_bit(GMU_REG(GMU_HFI_CTRL), 1, 1, 100);
	con_hex2(rc ? " hfi NOT ready" : " hfi ready", "status=", rd32(GMU_REG(GMU_HFI_CTRL)),
		 "fault=", rd32(GMU_REG(GMU_G2H_INFO)) & G2H_CM3_FAULT);
	if (rc)
		return;

	/* a6xx_hfi_start_v1 */
	step("HFI v1: init, version, perf, bw, test");
	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	msg[1] = 0;			/* seg_id */
	msg[2] = VA_DEBUG;
	msg[3] = 0x4000;
	msg[4] = 1;			/* GMU_COLD_BOOT */
	crumb("HFI msg 0 init");
	int r_init = hfi_send(0, msg, 5);
	con_hex2(" init", "rc=", (uint32_t)r_init, "fault=", rd32(GMU_REG(GMU_G2H_INFO)) & G2H_CM3_FAULT);
	if (gmu_cb >= 0) {
		uintptr_t cbr = SMMU_CB(gmu_cb);
		con_hex2(" cb", "fsr=", rd32(cbr + CB_FSR), "far=", rd32(cbr + CB_FAR));
		con_hex2(" cb", "fsynr0=", rd32(cbr + CB_FSYNR0), "gfsr=", rd32(SMMU + 0x48));
	}
	mdelay(200);	/* let the firmware finish writing whatever it reports */
	con_hex2(" f2h err", "hdr=", hfi_err[0], "w1=", hfi_err[1]);
	con_hex2(" f2h err", "w2=", hfi_err[2], "w3=", hfi_err[3]);
	con_hex2(" gmu", "init_res=", rd32(GMU_REG(GMU_INIT_RESULT)), "rpmh=", rd32(GMU_REG(GMU_RPMH_STATE)));
	con_hex2(" rsc", "status0=", rd32(RSCC_REG(0x0004)), "seqbusy=", rd32(RSCC_REG(0x0101)));
	con_hex2(" pdc", "enable=", rd32(PDC_REG(0x1140)), "cx_busy=", rd32(GMU_REG(GMU_CX_BUSY)));
	con_hex2(" log", "[0]=", rd32(GMEM_LOG), "[1]=", rd32(GMEM_LOG + 4));
	con_hex2(" log", "[2]=", rd32(GMEM_LOG + 8), "[3]=", rd32(GMEM_LOG + 12));
	con_hex2(" dbg", "[0]=", rd32(GMEM_DEBUG), "[1]=", rd32(GMEM_DEBUG + 4));
	con_hex2(" dbg", "[2]=", rd32(GMEM_DEBUG + 8), "[3]=", rd32(GMEM_DEBUG + 12));
	con_hex2(" dcache", "[0]=", rd32(GMEM_DCACHE), "[1]=", rd32(GMEM_DCACHE + 4));
	msg[1] = 1;			/* supported_version */
	crumb("HFI msg 1 version");
	int r_ver = hfi_send(1, msg, 2);
	con_hex2(" ver", "rc=", (uint32_t)r_ver, 0, 0);
	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	msg[1] = NGPU;
	msg[2] = 2;
	for (int i = 0; i < NGPU; i++) {
		msg[3 + 2 * i] = gx[i];
		msg[4 + 2 * i] = gpu_khz[i];
	}
	for (int i = 0; i < 2; i++) {
		msg[3 + 32 + 2 * i] = cx[i];
		msg[4 + 32 + 2 * i] = gmu_khz[i];
	}
	crumb("HFI msg 4 perf table");
	int r_perf = hfi_send(4, msg, 3 + 32 + 8);
	con_hex2(" perf", "rc=", (uint32_t)r_perf, 0, 0);
	for (int i = 0; i < 192; i++)
		msg[i] = 0;
	/* a6xx_build_bw_table (default A6xx gen1, A615 uses it) */
	msg[1] = 1;			/* bw_level_num */
	msg[2] = 3;			/* cnoc_cmds_num */
	msg[3] = 3;			/* ddr_cmds_num */
	msg[4] = 0x05;			/* cnoc_wait_bitmask */
	msg[5] = 0x07;			/* ddr_wait_bitmask */
	msg[6] = 0x50034;		/* cnoc addrs [6] */
	msg[7] = 0x5007c;
	msg[8] = 0x5004c;
	msg[12] = 0x40000000;		/* cnoc data [2][6] */
	msg[13] = 0x00000000;
	msg[14] = 0x40000000;
	msg[18] = 0x60000001;
	msg[19] = 0x20000001;
	msg[20] = 0x60000001;
	msg[24] = 0x50000;		/* ddr addrs [8] */
	msg[25] = 0x5005c;
	msg[26] = 0x5000c;
	msg[32] = 0x40000000;		/* ddr data [16][8] */
	msg[33] = 0x40000000;
	msg[34] = 0x40000000;
	crumb("HFI msg 3 bw table");
	int r_bw = hfi_send(3, msg, 32 + 128);
	con_hex2(" bw", "rc=", (uint32_t)r_bw, 0, 0);
	crumb("HFI msg 5 test");
	int r_test = hfi_send(5, msg, 1);
	con_hex2("HFI", "init=", (uint32_t)r_init, "ver=", (uint32_t)r_ver);
	con_hex2(" hfi", "perf=", (uint32_t)r_perf, "bw=", (uint32_t)r_bw);
	con_hex2(" hfi", "test=", (uint32_t)r_test, "ctrl=", rd32(GMU_REG(GMU_HFI_CTRL)));
	con_hex2(" g2h", "info=", rd32(GMU_REG(GMU_G2H_INFO)), "fault=", rd32(GMU_REG(GMU_G2H_INFO)) & G2H_CM3_FAULT);

	/* OOB GPU_SET (legacy 16 -> 24): the GMU powers the GX side */
	step("OOB GPU_SET");
	rc = oob(16, 24);
	con_hex2(rc ? " gpu_set TIMEOUT" : " gpu_set acked", "gx_gdsc=", rd32(GPUCC + GPUCC_GX_GDSCR), "info=", rd32(GMU_REG(GMU_G2H_INFO)));
	con_hex2(" gcc", "memnoc=", rd32(GCC + 0x7100c), "snoc=", rd32(GCC + 0x71018));
	if (!(rd32(GPUCC + GPUCC_GX_GDSCR) & (1u << 31))) {
		con_str("GX", "not powered - not touching GPU registers");
		return;
	}
	step("GPU registers (GX side)");
	con_hex2("RBBM", "status=", rd32(GPU + 4 * 0x210), "chipid=", rd32(GPU + 4 * 0x885));
	wr32(GPU + 4 * 0x883, 0x41445245);
	con_hex2("SCRATCH0", "now=", rd32(GPU + 4 * 0x883), "want=", 0x41445245);
}

void gpu_probe(void)
{
	if (*(volatile uint32_t *)CRUMB == CRUMB_MAGIC) {
		con_line("LAST PROBE RUN STOPPED AT:", YELLOW);
		con_line((const char *)(CRUMB + 8), WHITE);
		*(volatile uint32_t *)CRUMB = 0;
		con_str("NEXT", "continuing in 15 s (photo this)");
		mdelay(15000);
		con_clear(RED);
	}
	crumb("stage 1");
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

	/* a6xx_gmu_resume: "use a known rate to bring up the GMU" = 200 MHz:
	 * gpucc gmu_clk_src source 6 (GPLL0_OUT_MAIN_DIV, 300 MHz) / 1.5 (HID 2),
	 * fed by gcc_gpu_gpll0_div_clk_src (vote 0x52004 bit 16; bit 15 GPLL0). */
	con_str("STEP", "gmu_clk_src = 200 MHz (GPLL0 div / 1.5)");
	wr32(GCC + 0x52004, rd32(GCC + 0x52004) | (1u << 15) | (1u << 16));
	mdelay(1);
	wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | (6u << 8) | 2u);
	wr32(rcg, rd32(rcg) | 1u);
	rc = wait_bit(rcg, 1, 0, 10);
	if (rc || (rd32(rcg) & (1u << 31))) {	/* update stuck or root off */
		wr32(rcg + 4, (rd32(rcg + 4) & ~0x71fu) | 1u);
		wr32(rcg, rd32(rcg) | 1u);
		wait_bit(rcg, 1, 0, 10);
		con_hex2("  200MHz FAIL", "rcg=", rd32(rcg), "back to XO cfg=", rd32(rcg + 4));
	} else {
		con_hex2("  200MHz ok", "rcg=", rd32(rcg), "cfg=", rd32(rcg + 4));
	}

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

	con_str("GPU", "stage 1 done");
	con_skip();
	gmu_boot();
	crumb("probe finished normally");
	con_str("GPU", "probe done - no GPU commands were run");
}
