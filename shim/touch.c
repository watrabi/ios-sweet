/*
 * bonito touch probe (shim, -DTOUCHPROBE): Synaptics RMI4 at I2C 0x20 on
 * QUP wrapper-1 SE1 (0xa84000) - the same SE, pins (GPIO 6/7, qup9 = mux 1),
 * GCC clocks and TLMM north tile as ios-sweet's touch, so the GENI FIFO I2C
 * engine below is upstream's bsd/dev/sweet_touch.c (patches 0060/0063)
 * transcribed for the shim (MMU off, polled, no scheduler).
 *
 * bonito-specific (mainline sdm670-google-common.dtsi):
 *   touch 1.8 V rail = PM660 GPIO 12 (SID 0, 0xcb00) as output-high
 *   reset GPIO 99 (north, active low), IRQ GPIO 125 (north),
 *   "ts-switch" GPIO 135 (west) output-low
 * Writes: GCC votes/RCG (as upstream), TLMM pin configs, PM660 GPIO 12 mode
 * through the SPMI arbiter's RW channel (refused unless the apps EE owns it),
 * I2C reads plus RMI4 page-select writes (register 0xff) only.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define GCC		0x00100000UL
#define TLMM_NORTH	0x03900000UL
#define TLMM_WEST	0x03500000UL
#define SE		0x00a84000UL
#define SPMI_CORE	0x0c440000UL
#define SPMI_CHNLS	0x0c600000UL
#define SPMI_OBS	0x0e600000UL
#define SPMI_CNFG	0x0c40a000UL

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v)
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

static void udelay(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

static int wait_clear(uintptr_t a, uint32_t mask, uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (rd(a) & mask)
		if (ticks() > end)
			return -1;
	return 0;
}

static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint32_t v, int d)
{
	for (int i = d - 1; i >= 0; i--)
		*p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xf];
	return p;
}

/* ---- SPMI (PMIC arbiter v5) ---- */

static uint32_t spmi_apids;

/* APID of (sid, peripheral) and its write owner EE; -1 if unmapped. */
static int spmi_apid(unsigned sid, unsigned periph, uint32_t *owner)
{
	unsigned ppid = (sid << 8) | periph;
	int found = -1;
	for (uint32_t i = 0; i < spmi_apids; i++) {
		uint32_t m = rd(SPMI_CORE + 0x900 + 4 * i);
		if (!m || ((m >> 8) & 0xfff) != ppid)
			continue;
		uint32_t own = rd(SPMI_CNFG + 0x700 + 4 * i) & 7;
		if (found < 0 || own == 0) {	/* prefer the apps (EE 0) mapping */
			found = (int)i;
			*owner = own;
		}
	}
	return found;
}

static int spmi_cmd(uintptr_t ch, uint32_t opc, unsigned addr)
{
	wr(ch + 0x00, (opc << 27) | ((addr & 0xff) << 4));	/* byte count 0 = 1 byte */
	uint64_t end = ticks() + 19200 * 10;
	uint32_t st;
	while (!((st = rd(ch + 0x08)) & 1))
		if (ticks() > end)
			return -1;
	return (st & 0xe) ? -2 : 0;
}

static int spmi_read(int apid, unsigned addr, uint8_t *v)
{
	uintptr_t ch = SPMI_OBS + 0x80 * (uint32_t)apid;	/* observer, EE 0 */
	if (spmi_cmd(ch, 1 /* EXT_READL */, addr))
		return -1;
	*v = rd(ch + 0x18) & 0xff;
	return 0;
}

static int spmi_write(int apid, unsigned addr, uint8_t v)
{
	uintptr_t ch = SPMI_CHNLS + 0x10000 * (uint32_t)apid;
	wr(ch + 0x10, v);
	return spmi_cmd(ch, 0 /* EXT_WRITEL */, addr);
}

/* ---- TLMM ---- */

static void gpio_cfg(uintptr_t tile, unsigned n, unsigned mux, unsigned pull,
		     unsigned drive_ma, int out, int high)
{
	uintptr_t cfg = tile + n * 0x1000, io = cfg + 4;
	if (out)
		wr(io, (rd(io) & ~2u) | (high ? 2u : 0u));
	uint32_t c = rd(cfg) & ~0x3ffu;
	c |= (mux << 2) | pull | (((drive_ma / 2) - 1) << 6) | (out ? 0x200u : 0u);
	wr(cfg, c);
}

/* ---- GENI I2C (upstream sweet_touch.c) ---- */

#define SE_STATUS	0x040
#define SE_MCLK		0x048
#define SE_FW		0x068
#define SE_CLK_SEL	0x07c
#define SE_DMA		0x258
#define SE_TX_LEN	0x26c
#define SE_RX_LEN	0x270
#define SE_SCL		0x278
#define SE_CMD		0x600
#define SE_CTRL		0x604
#define SE_IRQ		0x610
#define SE_IRQ_EN	0x614
#define SE_IRQ_CLR	0x618
#define SE_TX		0x700
#define SE_RX		0x780
#define SE_TX_COUNT	0x800
#define SE_RX_COUNT	0x804
#define SE_TX_WM	0x80c
#define SE_HW_TX	0xe24
#define SE_HW_RX	0xe28
#define SE_RX_WM_IRQ	(1u << 26)
#define SE_RX_LAST	(1u << 27)
#define SE_TX_WM_IRQ	(1u << 30)
#define SE_DONE		(1u << 0)
#define SE_NACK		(1u << 10)
#define SE_ERRORS	(0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS	(SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
#define SE_PACK0	0x0007f8feu
#define SE_PACK1	0x000ffefeu

static int clocks(void)
{
	wr(GCC + 0x5200c, rd(GCC + 0x5200c) | (0x0fu << 18) | (1u << 23));
	uintptr_t rcg = GCC + 0x18148;
	uint32_t cfg = rd(rcg + 4);
	if (rd(rcg + 0x14) & 1) {	/* DFS mode: level 0 must already be XO/1 */
		if ((rd(rcg + 0x1c) & 0x371fu) != 1u)
			return -1;
	} else {
		wr(rcg + 4, (cfg & ~(0x371fu | (1u << 20))) | 1u);	/* XO, /1 */
		wr(rcg, rd(rcg) | 1u);
		if (wait_clear(rcg, 1u, 100000))
			return -2;
	}
	static const uint32_t br[] = {0x18004, 0x18008, 0x1800c, 0x18010, 0x18144};
	for (unsigned i = 0; i < 5; i++)
		if (wait_clear(GCC + br[i], 1u << 31, 100000))
			return -3 - (int)i;
	return 0;
}

static int se_init(void)
{
	uint32_t tx = rd(SE + SE_HW_TX), rx = rd(SE + SE_HW_RX);
	if (((rd(SE + SE_FW) >> 8) & 0xff) != 3 || (rd(SE + 0x64) & 1) ||
	    ((tx >> 24) & 0x3f) != 32 || ((rx >> 24) & 0x3f) != 32)
		return -1;
	if (rd(SE + SE_STATUS) & 0x1001u)
		return -2;
	wr(SE + 0xe18, 0);
	wr(SE + 0xe1c, 0);
	wr(SE + 0x644, 0);
	wr(SE + SE_IRQ_CLR, 0xffffffffu);
	wr(SE + 0x648, 0xffffffffu);
	wr(SE + SE_DMA, 0);
	wr(SE + 0x28, rd(SE + 0x28) | 0x7fu);
	wr(SE + 0xe30, rd(SE + 0xe30) | 0x0fu);
	wr(SE + 0x24, 0x7f);
	wr(SE + 0x20, 1);
	wr(SE + SE_CLK_SEL, 0);
	wr(SE + SE_MCLK, (7u << 4) | 1u);
	wr(SE + SE_SCL, (10u << 20) | (11u << 10) | 26u);	/* 100 kHz from 19.2 MHz */
	wr(SE + 0x254, 0);
	wr(SE + 0x260, SE_PACK0);
	wr(SE + 0x264, SE_PACK1);
	wr(SE + 0x284, SE_PACK0);
	wr(SE + 0x288, SE_PACK1);
	wr(SE + SE_TX_WM, 0);
	wr(SE + 0x810, 0);
	wr(SE + 0x814, ((rx >> 16) & 0x3f) - 1u);
	wr(SE + SE_IRQ_EN, SE_EVENTS);
	if (rd(SE + 0x260) != SE_PACK0 || rd(SE + 0x284) != SE_PACK0)
		return -3;
	if ((rd(SE + SE_TX_COUNT) & 0xfffffff) || (rd(SE + SE_RX_COUNT) & 0x1ffffff))
		return -4;
	return 0;
}

static void se_abort(void)
{
	wr(SE + SE_CTRL, 2);
	for (int i = 0; i < 5000 && !(rd(SE + SE_IRQ) & (1u << 5)); i++)
		udelay(1);
	for (int i = 0; i < 64 && (rd(SE + SE_RX_COUNT) & 0x1ffffff); i++)
		(void)rd(SE + SE_RX);
	wr(SE + SE_IRQ_CLR, 0xffffffffu);
}

static uint32_t last_irq;

/* One message: write nt (<= 3) bytes or read nr (<= 90) bytes. 0, -1 NACK, -2 error. */
static int i2c_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rxb, unsigned nr, int stretch)
{
	unsigned sent = 0, got = 0;
	if (nt > 3 || nr > 90 || (nt && nr) || (!nt && !nr))
		return -2;
	if (wait_clear(SE + SE_STATUS, 1u, 5000))
		return -3;
	wr(SE + SE_IRQ_CLR, 0xffffffffu);
	wr(SE + SE_TX_WM, nt ? 1u : 0u);
	wr(SE + (nr ? SE_RX_LEN : SE_TX_LEN), nr ? nr : nt);
	wr(SE + SE_CMD, ((nr ? 2u : 1u) << 27) | ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	for (int i = 0; i < 20000; i++) {
		uint32_t irq = rd(SE + SE_IRQ);
		last_irq = irq;
		if (irq & SE_ERRORS) {
			int err = (irq & SE_NACK) ? -1 : -2;
			for (int j = 0; j < 5000 && !(irq & SE_DONE); j++) {
				udelay(1);
				irq = rd(SE + SE_IRQ);
			}
			if ((irq & SE_DONE) && !(rd(SE + SE_STATUS) & 1)) {
				wr(SE + SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (nt && !sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t w = 0;
			for (unsigned j = 0; j < nt; j++)
				w |= (uint32_t)tx[j] << (8 * j);
			wr(SE + SE_TX, w);
			sent = nt;
			wr(SE + SE_TX_WM, 0);
		}
		uint32_t cnt = rd(SE + SE_RX_COUNT) & 0x1ffffff;
		if (cnt > 64)
			break;
		while (cnt--) {
			uint32_t w = rd(SE + SE_RX);
			for (unsigned j = 0; j < 4 && got < nr; j++)
				rxb[got++] = (uint8_t)(w >> (8 * j));
		}
		if (irq & SE_DONE) {
			irq = rd(SE + SE_IRQ);
			if (irq & SE_ERRORS)
				continue;
			wr(SE + SE_IRQ_CLR, irq);
			return (got == nr && sent == nt) ? 0 : -2;
		}
		wr(SE + SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		udelay(1);
	}
	se_abort();
	return -4;
}

#define RMI	0x20

static int rmi_page;

static int rmi_set_page(int page)
{
	if (page == rmi_page)
		return 0;
	uint8_t w[2] = {0xff, (uint8_t)page};
	int e = i2c_msg(RMI, w, 2, 0, 0, 0);
	if (!e)
		rmi_page = page;
	return e;
}

/* Read len bytes at 16-bit RMI address (page << 8 | offset). */
static int rmi_read(unsigned a, uint8_t *buf, unsigned len)
{
	int e = rmi_set_page((int)(a >> 8));
	if (e)
		return e;
	uint8_t r = (uint8_t)a;
	if ((e = i2c_msg(RMI, &r, 1, 0, 0, 1)))
		return e;
	return i2c_msg(RMI, 0, 0, buf, len, 0);
}

static void show_bytes(const char *label, const uint8_t *b, unsigned n)
{
	char line[72], *p = line;
	p = put_s(p, label);
	while (p - line < 8)
		*p++ = ' ';
	for (unsigned i = 0; i < n && p - line < 62; i++) {
		p = put_hex(p, b[i], 2);
		*p++ = ' ';
	}
	*p = 0;
	con_line(line, WHITE);
}

void touch_probe(void)
{
	int e;
	con_line("TOUCH PROBE  RMI4 @ i2c9 0x20 (SE 0xa84000)", GREEN);

	/* 1. state as ABL left it */
	con_hex2("TLMM", "g6=", rd(TLMM_NORTH + 6 * 0x1000), "g7=", rd(TLMM_NORTH + 7 * 0x1000));
	con_hex2(" rst99", "cfg=", rd(TLMM_NORTH + 99 * 0x1000), "io=", rd(TLMM_NORTH + 99 * 0x1000 + 4));
	con_hex2(" irq125", "cfg=", rd(TLMM_NORTH + 125 * 0x1000), "io=", rd(TLMM_NORTH + 125 * 0x1000 + 4));
	con_hex2(" sw135", "cfg=", rd(TLMM_WEST + 135 * 0x1000), "io=", rd(TLMM_WEST + 135 * 0x1000 + 4));
	con_hex2("GCC", "vote=", rd(GCC + 0x5200c), "s1br=", rd(GCC + 0x18144));
	con_hex2("SE", "fw=", rd(SE + SE_FW), "status=", rd(SE + SE_STATUS));

	uint32_t ver = rd(SPMI_CORE), owner = 7;
	spmi_apids = rd(SPMI_CORE + 4) & 0x7ff;
	con_hex2("SPMI", "arb=", ver, "apids=", spmi_apids);
	if (ver < 0x50000000u || ver >= 0x70000000u || !spmi_apids || spmi_apids > 512) {
		con_str("SPMI", "unsupported arbiter - stop");
		return;
	}
	int apid = spmi_apid(0, 0xcb, &owner);
	con_hex2("PM660G12", "apid=", (uint32_t)apid, "owner=", owner);
	if (apid < 0)
		return;
	static const uint8_t regs[] = {0x04, 0x05, 0x08, 0x40, 0x41, 0x42, 0x44, 0x45, 0x46};
	uint8_t g[9];
	for (int i = 0; i < 9; i++)
		if (spmi_read(apid, regs[i], &g[i]))
			g[i] = 0xee;
	show_bytes(" regs", g, 9);	/* type subtype status mode vin pull outsrc outctl en */

	/* 2. clocks, SE */
	if ((e = clocks())) {
		con_hex("CLOCKS", (uint32_t)e);
		return;
	}
	if ((e = se_init())) {
		con_hex("SE INIT", (uint32_t)e);
		return;
	}
	con_str("SE", "GENI I2C ready, 100 kHz");

	/* 3. pins + power: reset low, switch low, IRQ input, I2C mux */
	gpio_cfg(TLMM_NORTH, 99, 0, 3, 2, 1, 0);	/* reset asserted, pull-up */
	gpio_cfg(TLMM_WEST, 135, 0, 0, 2, 1, 0);	/* ts-switch low */
	gpio_cfg(TLMM_NORTH, 125, 0, 0, 2, 0, 0);	/* IRQ input, no pull */
	gpio_cfg(TLMM_NORTH, 6, 1, 0, 2, 0, 0);		/* qup9 SDA */
	gpio_cfg(TLMM_NORTH, 7, 1, 0, 2, 0, 0);		/* qup9 SCL */
	if (owner != 0) {
		con_str("POWER", "PM660 GPIO12 not owned by apps EE - not writing");
	} else {
		int lvmv = g[1] >= 0x10;
		int w1 = spmi_write(apid, 0x40, lvmv ? 1 : ((1u << 4) | 1u));	/* digital output (high) */
		int w2 = lvmv ? spmi_write(apid, 0x44, 0x80) : 0;		/* LV/MV: output high */
		int w3 = spmi_write(apid, 0x46, 0x80);				/* enable */
		uint8_t m = 0, s = 0, en = 0;
		spmi_read(apid, 0x40, &m);
		spmi_read(apid, 0x44, &s);
		spmi_read(apid, 0x46, &en);
		con_hex2("POWER", "wr=", (uint32_t)((w1 & 0xff) << 16 | (w2 & 0xff) << 8 | (w3 & 0xff)),
			 "now=", (uint32_t)m << 16 | (uint32_t)s << 8 | en);
	}
	udelay(20000);
	gpio_cfg(TLMM_NORTH, 99, 0, 3, 2, 1, 1);	/* release reset */
	udelay(300000);					/* syna,reset-delay-ms 200 */
	con_hex2(" after", "irq125=", rd(TLMM_NORTH + 125 * 0x1000 + 4) & 1, "rst=", rd(TLMM_NORTH + 99 * 0x1000 + 4));

	/* 4. RMI4: page select, then the Page Description Table at 0xe9 downwards */
	rmi_page = -1;
	if ((e = rmi_set_page(0))) {
		con_hex2("RMI4", "page0 err=", (uint32_t)e, "irq=", last_irq);
		return;
	}
	con_str("RMI4", "ACK at 0x20 - PDT:");
	uint32_t f01_q = 0, f01_d = 0, f12_q = 0, f12_d = 0, f12_c = 0;
	for (unsigned a = 0xe9; a >= 0x0a; a -= 6) {
		uint8_t d[6];
		if ((e = rmi_read(a, d, 6))) {
			con_hex2(" pdt", "err=", (uint32_t)e, "at=", a);
			break;
		}
		if (!d[5])
			break;
		/* query, command, control, data bases; int count/version; function */
		char line[64], *p = line;
		p = put_s(p, " F");
		p = put_hex(p, d[5], 2);
		p = put_s(p, "  q=");
		p = put_hex(p, d[0], 2);
		p = put_s(p, " cmd=");
		p = put_hex(p, d[1], 2);
		p = put_s(p, " ctl=");
		p = put_hex(p, d[2], 2);
		p = put_s(p, " data=");
		p = put_hex(p, d[3], 2);
		p = put_s(p, " ints=");
		p = put_hex(p, d[4] & 7, 1);
		p = put_s(p, " ver=");
		p = put_hex(p, (d[4] >> 5) & 3, 1);
		*p = 0;
		con_line(line, WHITE);
		if (d[5] == 0x01)
			f01_q = d[0], f01_d = d[3];
		if (d[5] == 0x12)
			f12_q = d[0], f12_d = d[3], f12_c = d[2];
	}
	if (f01_q) {
		uint8_t q[21];
		if (!rmi_read(f01_q, q, 21)) {
			show_bytes("F01 q", q, 21);	/* mfr, props, ..., product id at 11 */
			char line[32], *p = line;
			p = put_s(p, "PRODUCT ");
			for (int i = 11; i < 21 && q[i]; i++)
				*p++ = (q[i] >= 0x20 && q[i] < 0x7f) ? (char)q[i] : '.';
			*p = 0;
			con_line(line, YELLOW);
		}
		uint8_t st[2];
		if (!rmi_read(f01_d, st, 2))
			show_bytes("F01 d", st, 2);	/* device status, int status */
	}
	if (f12_q) {
		uint8_t q[16];
		if (!rmi_read(f12_q, q, 16))
			show_bytes("F12 q", q, 16);
		con_hex2("F12", "ctl=", f12_c, "data=", f12_d);
		/* Sample the data area a few times: touch the screen now. */
		con_str("TOUCH", "put a finger on the screen: 10 samples, 1 s apart");
		for (int n = 0; n < 10; n++) {
			uint8_t d[24];
			if (!rmi_read(f12_d, d, 24))
				show_bytes(" data", d, 24);
			udelay(1000000);
		}
	}
	con_str("TOUCH", "probe done");
}
