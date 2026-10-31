/*
 * bonito eMMC (sdhc_1, Qualcomm sdhci-msm v5 on SDM670).
 *
 * test_emmc():  READ-ONLY register dump (milestone 1).
 * emmc_probe(): bring the bus back up after ABL and read the card. The CARD
 *   only ever sees CMD0/1/2/3/9/7/13 and the two read commands CMD8
 *   (EXT_CSD) and CMD17 (READ_SINGLE_BLOCK): no write, erase or CMD6 switch,
 *   so nothing on the flash can change. The HOST is reconfigured (clock,
 *   bus power handshake, IO pad) the way Linux sdhci-msm.c does it.
 *
 * ABL leaves: internal clock on + stable, SD clock off, power reg 0,
 * interrupts masked, CQE off (see BONITO.md). Bus clock: Linux keeps the
 * SDHCI divider at 0 and sets the rate in GCC (RCG 0x126028); we first try
 * the standard SDHCI divider, measure the real clock by timing commands,
 * and fall back to programming the GCC RCG to 400 kHz (TCXO/12 * 1/4,
 * gcc-sdm845.c ftbl_gcc_sdcc1_apps_clk_src).
 *
 * Polled PIO through the buffer data port, MMU off (Device memory).
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define SDHC_BASE	0x007c4000UL
#define CQHCI_BASE	0x007c5000UL
#define GCC_BASE	0x00100000UL

/* standard SDHCI */
#define R_BLK		0x04	/* [11:0] block size, [31:16] block count */
#define R_ARG		0x08
#define R_XFER_CMD	0x0c	/* [15:0] transfer mode, [31:16] command */
#define R_RESP0		0x10
#define R_DATA		0x20
#define R_PRES		0x24
#define R_HCTL		0x28	/* [7:0] host ctrl1, [15:8] power ctrl */
#define R_CLK		0x2c	/* [15:0] clock ctrl, [23:16] timeout, [31:24] sw reset */
#define R_INT		0x30	/* [15:0] normal, [31:16] error */
#define R_INTEN		0x34
#define R_SIGEN		0x38
#define R_HCTL2		0x3c	/* [31:16] host ctrl2 */

#define PRES_CMD_INHIBIT	(1u << 0)
#define PRES_DAT_INHIBIT	(1u << 1)
#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERR		(1u << 15)

#define CLK_ICE		(1u << 0)
#define CLK_ICS		(1u << 1)
#define CLK_SDCE	(1u << 2)

/* sdhci-msm v5 vendor registers (offsets from the hc base) */
#define V_VENDOR_SPEC	0x20c
#define V_PWRCTL_STATUS	0x240
#define V_PWRCTL_MASK	0x244
#define V_PWRCTL_CLEAR	0x248
#define V_PWRCTL_CTL	0x24c
#define V_VENDOR_SPEC3	0x250
#define V_DLL_CONFIG	0x200

#define PWR_BUS_OFF	(1u << 0)
#define PWR_BUS_ON	(1u << 1)
#define PWR_IO_LOW	(1u << 2)
#define PWR_IO_HIGH	(1u << 3)
#define PWR_ACK_BUS	(1u << 0)
#define PWR_ACK_IO	(1u << 2)

#define VS_HC_MCLK_SEL_MASK	(3u << 8)
#define VS_HC_MCLK_SEL_DFLT	(2u << 8)
#define VS_IO_PAD_PWR_SWITCH	(1u << 16)
#define VS_HC_SELECT_IN_EN	(1u << 18)
#define VS_HC_SELECT_IN_MASK	(7u << 19)
#define VS3_PWRSAVE_DLL		(1u << 3)

/* GCC SDCC1 */
#define GCC_SDCC1_APPS_CBCR	(GCC_BASE + 0x26004)
#define GCC_SDCC1_AHB_CBCR	(GCC_BASE + 0x26008)
#define GCC_SDCC1_RCG		(GCC_BASE + 0x26028)	/* CMD, +4 CFG, +8 M, +c N, +10 D */

/* command register flags: [1:0] response type, 3 CRC, 4 index, 5 data */
#define RSP_NONE	0x00
#define RSP_R2		0x09
#define RSP_R3		0x02
#define RSP_R1		0x1a
#define RSP_R1B		0x1b
#define DATA		0x20

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline void wr16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
#define HC(r)	(SDHC_BASE + (r))

static uint64_t ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void udelay(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

/* ---- small formatting helpers (console only has fixed hex) ---- */

static char *put_s(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *put_dec(char *p, uint64_t v)
{
	char tmp[21];
	int n = 0;
	do
		tmp[n++] = '0' + v % 10;
	while (v /= 10);
	while (n)
		*p++ = tmp[--n];
	return p;
}

static char *put_hex(char *p, uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		*p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xf];
	return p;
}

static char *pad_to(char *start, char *p, int col)
{
	while (p - start < col)
		*p++ = ' ';
	return p;
}

/* ---- milestone 1: read-only dump ---- */

void test_emmc(void)
{
	con_str("EMMC", "reading sdhc_1 @7c4000 (read-only)");

	uint32_t ver   = rd(HC(0xfc));
	uint32_t pres  = rd(HC(R_PRES));
	uint32_t hctl  = rd(HC(R_HCTL));
	uint32_t clk   = rd(HC(R_CLK));
	uint32_t ists  = rd(HC(R_INT));
	uint32_t iena  = rd(HC(R_INTEN));
	uint32_t hctl2 = rd(HC(R_HCTL2));
	uint32_t cap0  = rd(HC(0x40));
	uint32_t cap1  = rd(HC(0x44));
	uint32_t adma  = rd(HC(0x58));
	uint32_t admah = rd(HC(0x5c));
	uint32_t blk   = rd(HC(R_BLK));
	uint32_t resp0 = rd(HC(R_RESP0));

	con_hex2("SDHCI", "ver=", ver, "pres=", pres);
	con_hex2(" ctl", "hctl=", hctl, "clk=", clk);
	con_hex2(" int", "sts=", ists, "ena=", iena);
	con_hex2(" ctl2", "hctl2=", hctl2, "blk=", blk);
	con_hex2(" cap", "lo=", cap0, "hi=", cap1);
	con_hex2(" adma", "lo=", adma, "hi=", admah);
	con_hex("  resp0", resp0);
	con_str(" state",
		!(clk & 2) ? "int clock NOT stable" :
		!(clk & 4) ? "clock stable, SD clock off" :
		!(hctl & 0x100) ? "SD clock on, bus power off" :
		"clock + bus power on: ABL left it up");

	con_str("CQHCI", "reading @7c5000");
	uint32_t cqver = rd(CQHCI_BASE + 0x00);
	uint32_t cqcap = rd(CQHCI_BASE + 0x04);
	uint32_t cqcfg = rd(CQHCI_BASE + 0x08);
	con_hex2("CQHCI", "ver=", cqver, "cap=", cqcap);
	con_hex2(" cfg", "cfg=", cqcfg, "cqe=", cqcfg & 1);
}

/* ---- host side ---- */

static int wait_clear(uintptr_t reg, uint32_t mask, uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (rd(reg) & mask)
		if (ticks() > end)
			return -1;
	return 0;
}

/* Answer the msm power IRQ the way sdhci_msm_handle_pwr_irq() does. */
static uint32_t pwr_irq_service(uint32_t wait_us)
{
	uint64_t end = ticks() + (uint64_t)wait_us * 192 / 10;
	uint32_t st;

	while (!((st = rd(HC(V_PWRCTL_STATUS)) & 0xf)) && ticks() < end)
		;
	if (!st)
		return 0;
	for (int i = 0; i < 10 && (rd(HC(V_PWRCTL_STATUS)) & st); i++) {
		wr(HC(V_PWRCTL_CLEAR), st);
		udelay(10);
	}
	uint32_t ack = 0;
	if (st & (PWR_BUS_ON | PWR_BUS_OFF))
		ack |= PWR_ACK_BUS;	/* regulators: ABL left vmmc/vqmmc on */
	if (st & (PWR_IO_LOW | PWR_IO_HIGH | PWR_BUS_ON | PWR_BUS_OFF))
		ack |= PWR_ACK_IO;
	wr(HC(V_PWRCTL_CTL), ack);
	/* eMMC vccq is 1.8 V only on this board: keep the IO pad in 1.8 V mode */
	wr(HC(V_VENDOR_SPEC), rd(HC(V_VENDOR_SPEC)) | VS_IO_PAD_PWR_SWITCH);
	return st;
}

static void reset_cmd_data(void)
{
	wr8(HC(R_CLK) + 3, 0x06);	/* SW reset CMD + DAT (not ALL: no pwr irq) */
	wait_clear(HC(R_CLK), 0x06u << 24, 100000);
}

/* SD clock = base / (2 * div10) (div10 = 0: undivided). */
static int set_sd_clock(uint32_t div10)
{
	uint16_t c = rd(HC(R_CLK)) & 0xffff;
	wr16(HC(R_CLK), c & ~CLK_SDCE);
	c = ((div10 & 0xff) << 8) | ((div10 >> 8 & 3) << 6) | CLK_ICE;
	wr16(HC(R_CLK), c);
	uint64_t end = ticks() + 19200 * 20;	/* 20 ms */
	while (!(rd(HC(R_CLK)) & CLK_ICS))
		if (ticks() > end)
			return -1;
	wr16(HC(R_CLK), c | CLK_SDCE);
	udelay(1000);	/* >= 74 clocks before the first command at 100 kHz */
	return 0;
}

static uint32_t last_err;
static uint64_t last_ticks;

/* Returns 0 or the error-interrupt bits (never 0 on failure). */
static uint32_t cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t resp[4])
{
	uint32_t inhibit = PRES_CMD_INHIBIT;
	if ((flags & DATA) || (flags & 3) == 3)
		inhibit |= PRES_DAT_INHIBIT;
	if (wait_clear(HC(R_PRES), inhibit, 100000)) {
		last_err = 0xdead0001;
		return last_err;
	}
	wr(HC(R_INT), 0xffffffff);
	wr(HC(R_ARG), arg);
	uint32_t mode = ((flags & DATA) ? (1u << 4) : 0) | (flags >> 8);	/* single block read, PIO */
	uint64_t t0 = ticks();
	wr(HC(R_XFER_CMD), (idx << 24) | ((flags & 0x3f) << 16) | mode);

	uint64_t end = t0 + 19200ULL * 1000;	/* 1 s */
	uint32_t st;
	while (!((st = rd(HC(R_INT))) & (INT_CMD_DONE | INT_ERR)))
		if (ticks() > end) {
			reset_cmd_data();
			last_err = 0xdead0002;
			return last_err;
		}
	last_ticks = ticks() - t0;
	if (st & INT_ERR) {
		last_err = (st >> 16) | 0x10000;
		reset_cmd_data();
		return last_err;
	}
	wr(HC(R_INT), INT_CMD_DONE);
	if (resp)
		for (int i = 0; i < 4; i++)
			resp[i] = rd(HC(R_RESP0) + 4 * i);
	if ((flags & 3) == 3) {	/* R1b: wait for busy end */
		while (!((st = rd(HC(R_INT))) & (INT_XFER_DONE | INT_ERR)))
			if (ticks() > end) {
				last_err = 0xdead0003;
				return last_err;
			}
		wr(HC(R_INT), INT_XFER_DONE);
	}
	return 0;
}

/*
 * CMD8 / CMD17: one 512-byte block into buf via ADMA2 (32-bit descriptors).
 * PIO does not work on this controller: the second probe saw "buffer read
 * enable" stay set while the data port returned zeros, and caps advertise
 * ADMA2 but not SDMA - Linux sdhci-msm always uses ADMA. MMU is off, so
 * every access is uncached and DMA needs no cache maintenance. The shim's
 * buffers are below 4 GiB. sdhc_1 sits behind apps_smmu (SID 0x140); ABL
 * uses DMA on it too, so whatever it left must pass our buffers.
 */
#define R_ADMA_ERR	0x54
#define R_ADMA_LO	0x58
#define R_ADMA_HI	0x5c
#define ADMA_VALID	(1u << 0)
#define ADMA_END	(1u << 1)
#define ADMA_TRAN	(2u << 4)

static uint32_t adma_desc[2] __attribute__((aligned(8)));
static uint32_t extra_words, diag_int, diag_pres, diag_blk, diag_xfer, diag_adma;

static uint32_t read_block(uint32_t idx, uint32_t arg, uint32_t *buf)
{
	adma_desc[0] = (512u << 16) | ADMA_TRAN | ADMA_END | ADMA_VALID;
	adma_desc[1] = (uint32_t)(uintptr_t)buf;
	for (int i = 0; i < 128; i++)
		buf[i] = 0xdeadbeef;		/* shows whether DMA wrote anything */
	__asm__ volatile("dsb sy" ::: "memory");
	wr(HC(R_ADMA_LO), (uint32_t)(uintptr_t)adma_desc);
	wr(HC(R_ADMA_HI), 0);
	wr8(HC(R_HCTL), (rd(HC(R_HCTL)) & ~0x18u) | (2u << 3));	/* DMA select: ADMA2 32-bit */
	wr(HC(R_BLK), 512 | (1u << 16));

	uint64_t t0 = ticks();
	uint32_t e = cmd(idx, arg, RSP_R1 | DATA | (1u << 8), 0);	/* mode bit0 = DMA enable */
	if (e)
		return e;
	uint64_t end = ticks() + 19200ULL * 1000;
	uint32_t st;
	while (!((st = rd(HC(R_INT))) & (INT_XFER_DONE | INT_ERR)))
		if (ticks() > end) {
			diag_int = st, diag_pres = rd(HC(R_PRES));
			diag_blk = rd(HC(R_BLK)), diag_xfer = rd(HC(R_XFER_CMD));
			diag_adma = rd(HC(R_ADMA_ERR));
			reset_cmd_data();
			return last_err = 0xdead0005;
		}
	if (st & INT_ERR) {
		diag_int = st, diag_pres = rd(HC(R_PRES));
		diag_blk = rd(HC(R_ADMA_LO)), diag_xfer = rd(HC(R_XFER_CMD));
		diag_adma = rd(HC(R_ADMA_ERR));
		reset_cmd_data();
		return last_err = (st >> 16) | 0x30000;	/* bit 9 of the high half = ADMA error */
	}
	wr(HC(R_INT), 0xffffffff);
	__asm__ volatile("dsb sy" ::: "memory");
	last_ticks = ticks() - t0;
	return 0;
}

static void read_diag(const char *what, uint32_t e, const uint32_t *buf)
{
	con_hex2(what, "err=", e, "extra=", extra_words);
	con_hex2("  at", "int=", diag_int, "pres=", diag_pres);
	con_hex2("  at", "blk=", diag_blk, "xfer=", diag_xfer);
	con_hex2("  at", "admaerr=", diag_adma, "desc=", (uint32_t)(uintptr_t)adma_desc);
	con_hex2("  data", "w0=", buf[0], "w1=", buf[1]);
	con_hex2("  data", "w48=", buf[48], "w53=", buf[53]);	/* EXT_CSD 192.., 212.. */
}
static void gcc_dump(const char *label)
{
	uintptr_t r = GCC_SDCC1_RCG;
	uint32_t cfg = rd(r + 4);
	char line[64], *p = line;
	p = put_s(p, label);
	p = pad_to(line, p, 8);
	p = put_s(p, "src=");
	p = put_dec(p, cfg >> 8 & 7);
	p = put_s(p, " div=");
	p = put_dec(p, cfg & 0x1f);
	p = put_s(p, " mode=");
	p = put_dec(p, cfg >> 12 & 3);
	p = put_s(p, " m=");
	p = put_hex(p, rd(r + 8) & 0xff, 2);
	p = put_s(p, " n=");
	p = put_hex(p, rd(r + 0xc) & 0xff, 2);
	p = put_s(p, " cbcr=");
	p = put_hex(p, rd(GCC_SDCC1_APPS_CBCR), 8);
	*p = 0;
	con_line(line, WHITE);
}

/* GCC SDCC1 apps RCG -> 400 kHz: TCXO (src 0) / 12, M/N = 1/4, dual edge. */
static int gcc_set_400k(void)
{
	uintptr_t r = GCC_SDCC1_RCG;
	wr(r + 8, (rd(r + 8) & ~0xffu) | 1);			/* M */
	wr(r + 0xc, (rd(r + 0xc) & ~0xffu) | (~(4u - 1) & 0xff));	/* N: ~(n - m) */
	wr(r + 0x10, (rd(r + 0x10) & ~0xffu) | (~4u & 0xff));	/* D: ~n (50 %) */
	uint32_t cfg = rd(r + 4) & ~(0x1fu | (7u << 8) | (3u << 12));
	cfg |= (2 * 12 - 1) | (0u << 8) | (2u << 12);
	wr(r + 4, cfg);
	wr(r, rd(r) | 1);					/* UPDATE */
	return wait_clear(r, 1, 100000);
}

static void err_line(const char *what, uint32_t e)
{
	con_hex2(what, "err=", e, "ticks=", (uint32_t)last_ticks);
}

/* Estimated bus clock in kHz from a command of `clocks` bus cycles. */
static uint32_t est_khz(uint32_t clocks, uint64_t t)
{
	return t ? (uint32_t)((uint64_t)clocks * 19200 / t) : 0;
}

static uint32_t blk[128];
static uint32_t ext_csd[128];

static int card_init(uint32_t *rca_out)
{
	uint32_t r[4], e;

	cmd(0, 0, RSP_NONE, 0);			/* GO_IDLE */
	udelay(2000);

	/* CMD1 until ready: OCR 1.7-1.95 V + 2.7-3.6 V, sector mode */
	uint64_t t_first = 0;
	int tries;
	for (tries = 0; tries < 1000; tries++) {
		e = cmd(1, 0x40ff8080, RSP_R3, r);
		if (e) {
			err_line("CMD1", e);
			return -1;
		}
		if (!t_first)
			t_first = last_ticks;
		if (r[0] & 0x80000000u)
			break;
		udelay(1000);
	}
	/* a CMD1 round trip is ~48 + Ncr (5..64) + 48 = ~110..160 bus clocks */
	con_hex2("CMD1", "ocr=", r[0], "tries=", tries);
	con_hex2(" rt", "ticks=", (uint32_t)t_first, "~khz=", est_khz(130, t_first));
	if (!(r[0] & 0x80000000u))
		return -1;

	if ((e = cmd(2, 0, RSP_R2, r))) {
		err_line("CMD2", e);
		return -1;
	}
	char line[64], *p = line;
	p = put_s(p, "CID     mid=");
	p = put_hex(p, r[3] >> 16 & 0xff, 2);
	p = put_s(p, " name=");
	uint32_t pn[6] = { r[2] >> 24, r[2] >> 16, r[2] >> 8, r[2], r[1] >> 24, r[1] >> 16 };
	for (int i = 0; i < 6; i++) {
		char c = pn[i] & 0xff;
		*p++ = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	*p = 0;
	con_line(line, WHITE);

	uint32_t rca = 2;
	if ((e = cmd(3, rca << 16, RSP_R1, r))) {
		err_line("CMD3", e);
		return -1;
	}
	if ((e = cmd(9, rca << 16, RSP_R2, r))) {
		err_line("CMD9", e);
		return -1;
	}
	if ((e = cmd(7, rca << 16, RSP_R1B, r))) {
		err_line("CMD7", e);
		return -1;
	}
	if ((e = cmd(13, rca << 16, RSP_R1, r))) {
		err_line("CMD13", e);
		return -1;
	}
	con_hex2("STATUS", "r1=", r[0], "state=", r[0] >> 9 & 0xf);	/* 4 = tran */
	*rca_out = rca;
	return 0;
}

static void show_ext_csd(void)
{
	const uint8_t *x = (const uint8_t *)ext_csd;
	uint64_t sec = (uint32_t)x[212] | (uint32_t)x[213] << 8 | (uint32_t)x[214] << 16 | (uint32_t)x[215] << 24;
	char line[64], *p = line;
	p = put_s(p, "EXTCSD  rev=");
	p = put_dec(p, x[192]);
	p = put_s(p, " sectors=");
	p = put_dec(p, sec);
	p = put_s(p, " (");
	p = put_dec(p, sec >> 21);
	p = put_s(p, " GiB)");
	*p = 0;
	con_line(line, WHITE);
	p = line;
	p = put_s(p, "        partcfg=");
	p = put_hex(p, x[179], 2);
	p = put_s(p, " buswidth=");
	p = put_hex(p, x[183], 2);
	p = put_s(p, " hs_timing=");
	p = put_hex(p, x[185], 2);
	p = put_s(p, " type=");
	p = put_hex(p, x[196], 2);
	*p = 0;
	con_line(line, WHITE);
}

static void show_gpt(void)
{
	const uint8_t *b = (const uint8_t *)blk;
	uint32_t e;

	if ((e = read_block(17, 1, blk))) {	/* LBA 1: GPT header */
		err_line("GPT hdr", e);
		return;
	}
	con_hex2("LBA1", "ticks=", (uint32_t)last_ticks, "~khz=", est_khz(4096 + 200, last_ticks));
	if (blk[0] != 0x20494645 || blk[1] != 0x54524150) {	/* "EFI PART" */
		con_hex2("GPT", "sig0=", blk[0], "sig1=", blk[1]);
		con_str("GPT", "no EFI PART signature");
		return;
	}
	uint64_t ent_lba = *(const uint64_t *)(b + 72);
	uint32_t n_ent = *(const uint32_t *)(b + 80);
	uint32_t ent_sz = *(const uint32_t *)(b + 84);
	con_hex2("GPT", "entries=", n_ent, "size=", ent_sz);
	if (ent_sz != 128 || n_ent > 256)
		return;

	con_line("PARTITIONS  name / MiB   (userdata detail below)", GREEN);
	char line[72], *p = line;
	int col = 0, shown = 0;
	uint64_t ud_first = 0, ud_last = 0;
	for (uint32_t i = 0; i < n_ent; i++) {
		if (i % 4 == 0 && (e = read_block(17, (uint32_t)ent_lba + i / 4, blk))) {
			err_line("GPT ent", e);
			return;
		}
		const uint8_t *en = b + (i % 4) * 128;
		const uint32_t *w = (const uint32_t *)en;
		if (!(w[0] | w[1] | w[2] | w[3]))
			continue;
		uint64_t first = *(const uint64_t *)(en + 32), last = *(const uint64_t *)(en + 40);
		char name[17];
		int k;
		for (k = 0; k < 16; k++) {
			uint16_t c = en[56 + 2 * k] | en[57 + 2 * k] << 8;
			if (!c)
				break;
			name[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
		}
		name[k] = 0;
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' &&
		    name[4] == 'd' && name[5] == 'a' && name[6] == 't' && name[7] == 'a' && !name[8]) {
			ud_first = first;
			ud_last = last;
		}
		char *start = p;
		p = put_s(p, name);
		p = pad_to(start, p, 15);
		uint64_t mib = (last - first + 1) >> 11;
		if (mib)
			p = put_dec(p, mib);
		else {
			p = put_dec(p, (last - first + 1) / 2);
			*p++ = 'K';
		}
		shown++;
		if (++col == 3) {
			*p = 0;
			con_line(line, WHITE);
			p = line;
			col = 0;
		} else
			p = pad_to(start, p, 21);
	}
	if (col) {
		*p = 0;
		con_line(line, WHITE);
	}
	con_hex2("SHOWN", "parts=", shown, 0, 0);
	if (ud_first) {
		p = line;
		p = put_s(p, "USERDATA first=");
		p = put_dec(p, ud_first);
		p = put_s(p, " last=");
		p = put_dec(p, ud_last);
		p = put_s(p, " = ");
		p = put_dec(p, (ud_last - ud_first + 1) >> 11);
		p = put_s(p, " MiB");
		*p = 0;
		con_line(line, YELLOW);
	}
}

void emmc_probe(void)
{
	uint32_t rca;

	con_line("EMMC PROBE  read-only card access (CMD8/CMD17 only)", GREEN);
	con_hex2("VENDOR", "spec=", rd(HC(V_VENDOR_SPEC)), "spec3=", rd(HC(V_VENDOR_SPEC3)));
	con_hex2(" pwrctl", "sts=", rd(HC(V_PWRCTL_STATUS)), "mask=", rd(HC(V_PWRCTL_MASK)));
	con_hex2(" dll", "cfg=", rd(HC(V_DLL_CONFIG)), "clk=", rd(HC(R_CLK)));
	gcc_dump("GCC");

	/* host setup */
	wr(HC(R_SIGEN), 0);			/* no interrupt lines, we poll */
	wr(HC(R_INTEN), 0xffffffff);		/* but latch every status bit */
	wr(HC(R_INT), 0xffffffff);
	wr(HC(V_PWRCTL_MASK), 0xf);		/* let the pwrctl status bits latch */
	reset_cmd_data();

	/* msm_hc_select_default(): legacy timing, default MCLK, DLL power save off */
	wr(HC(V_VENDOR_SPEC3), rd(HC(V_VENDOR_SPEC3)) & ~VS3_PWRSAVE_DLL);
	uint32_t vs = rd(HC(V_VENDOR_SPEC));
	vs = (vs & ~VS_HC_MCLK_SEL_MASK) | VS_HC_MCLK_SEL_DFLT;
	vs &= ~(VS_HC_SELECT_IN_EN | VS_HC_SELECT_IN_MASK);
	wr(HC(V_VENDOR_SPEC), vs);

	/* host ctrl1: 1-bit, no HS, no DMA; timeout max */
	wr8(HC(R_HCTL), 0);
	wr8(HC(R_CLK) + 2, 0x0e);

	/* bus power on (3.0 V code for vmmc 2.95 V) -> msm power irq handshake */
	wr8(HC(R_HCTL) + 1, (6u << 1) | 1);
	uint32_t p1 = pwr_irq_service(20000);
	/* 1.8 V signalling (vccq is 1.8 V) -> IO_LOW request */
	wr16(HC(R_HCTL2) + 2, (rd(HC(R_HCTL2)) >> 16) | (1u << 3));
	uint32_t p2 = pwr_irq_service(20000);
	con_hex2("POWER", "irq1=", p1, "irq2=", p2);
	con_hex2(" now", "hctl=", rd(HC(R_HCTL)), "vspec=", rd(HC(V_VENDOR_SPEC)));

	/* attempt 1: SDHCI divider /1024 on whatever GCC rate ABL left */
	if (set_sd_clock(0x200)) {
		con_str("CLOCK", "internal clock not stable");
		return;
	}
	con_hex("CLK try1", rd(HC(R_CLK)));
	int ok = card_init(&rca) == 0;
	if (!ok) {
		/* attempt 2: Linux's way - GCC RCG at 400 kHz, SDHCI divider 0 */
		con_str("RETRY", "GCC sdcc1 -> 400 kHz, divider 0");
		uint16_t c = rd(HC(R_CLK)) & 0xffff;
		wr16(HC(R_CLK), c & ~CLK_SDCE);
		if (gcc_set_400k())
			con_str("GCC", "RCG update did not complete");
		gcc_dump("GCC now");
		if (set_sd_clock(0)) {
			con_str("CLOCK", "internal clock not stable");
			return;
		}
		ok = card_init(&rca) == 0;
	}
	if (!ok) {
		con_str("EMMC", "card init FAILED");
		return;
	}

	uint32_t e = 1, r[4];
	for (int v = 0; v < 1; v++) {
		e = read_block(8, 0, ext_csd);	/* SEND_EXT_CSD */
		if (!e)
			break;
		read_diag("EXTCSD", e, ext_csd);
		cmd(13, rca << 16, RSP_R1, r);
		con_hex2("  card", "r1=", r[0], "state=", r[0] >> 9 & 0xf);
	}
	if (e) {
		con_str("EXTCSD", "ADMA read failed (see above)");
		show_ext_csd();
		return;
	}
	con_hex2("EXTCSD", "adma ok, ticks=", (uint32_t)last_ticks, 0, 0);
	show_ext_csd();
	show_gpt();
	con_str("EMMC", "probe done - nothing was written to the card");
}
