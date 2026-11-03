/*
 * wland: WLAN bring-up for the Pixel 4 from userland (/dev/pmem), the way
 * the GPU was brought up: the modem DSP hosts the WLAN firmware (icnss /
 * WCN3990 family), so: power votes, PAS-boot the modem, SMP2P, then GLINK +
 * QRTR + QMI to the WLFW service, then the WLAN copy engines for WMI.
 *
 *   wland smem              SMEM partitions and the items we care about
 *   wland smp2p             modem SMP2P state (in: 435, out: 428)
 *   wland vote RES LVL      RPMh ARC level vote (index) for RES (e.g. mss.lvl)
 *   wland levels RES        cmd-db levels of an ARC resource
 *   wland bcm RES X [Y]     RPMh BCM (bus bandwidth) vote, e.g. MC0 (DDR) / SH0 (memory NoC); 0x3fff = top
 *   wland smp2p-init        our outbound SMP2P item + doorbell; shows the modem's entries
 *   wland qmp MSG           message to the AOP over QMP (opens the link if needed)
 *   wland shutdown          PAS shutdown of the modem
 *   wland boot              power votes + PAS boot of the modem + wait for ready
 *   wland smmu              the WLAN DMA stream through a translation-off SMMU bank (radio.c)
 *   wland up [SECS]         all of it from a fresh boot, then the radio: copy engines, WMI, passive scans
 *   wland auto              the boot job (launchd org.iosonpixel.wland): `up 0` once per phone boot, log in
 *                           /var/local/wland.log (last boot's: .prev); off with /var/local/wland.off or sweetwifi=0
 * While joined, SIGUSR1 drops the link (tests the rejoin).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "pmem.h"
#include "smem.h"
#include "link.h"
#include "rmtfs.h"
#include "pdmapper.h"
#include "tftp.h"
#include "wlfw.h"
#include "radio.h"
#include "ipnet.h"
#include "bridge.h"

/* cmd-db aux levels (as in the GPU's arc votes) */
/* Every cmd-db entry: name, RPMh slave (3 ARC, 4 VRM, 5 BCM), address. */
static void cmddb_list(void)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			printf("%-8s slave %u addr %05x\n", name, w0 & 0xffff, rd32(ent + 16));
		}
	}
}

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

static int levels(const char *res, int print)
{
	uint64_t aux;
	uint32_t len;
	if (cmddb_aux(res, &aux, &len)) {
		fprintf(stderr, "%s: not in cmd-db\n", res);
		return -1;
	}
	if (print) {
		printf("%s:", res);
		for (uint32_t i = 0; i < len / 2; i++)
			printf(" [%u]=%u", i, aux16(aux, i));
		printf("\n");
	}
	return (int)(len / 2);
}

static int vote(const char *res, uint32_t level_index)
{
	uint32_t off[1] = { 0 }, val[1] = { level_index };
	int rc = pm_rpmh(res, 1, off, val);
	printf("vote %s level index %u -> %d\n", res, level_index, rc);
	return rc;
}

/*
 * The WLAN supplies (icnss in the stock DT: vdd-cx-mx, vdd-1.8-xo, vdd-1.3-rfa, vdd-3.3-ch0), RPMh VRM
 * votes as Linux qcom-rpmh-regulator: voltage (mV) at 0, enable at 4, mode at 8 (PMIC5 LDO HPM = 7).
 * Without them the WLAN firmware takes an exception once calibration starts (after the cal report).
 */
static int qmp_send(const char *msg);

static int wlan_power(void)
{
	static const struct { const char *res; uint32_t mv; } ldo[] = {
		{ "ldoa5", 800 }, { "ldoa9", 1800 }, { "ldoa6", 1248 }, { "ldoa19", 3000 },	/* bonito icnss vdd-0.8-cx-mx, 1.8-xo, 1.3-rfa, 3.3-ch0 */
	};
	for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
		uint32_t off[3] = { 0, 8, 4 }, val[3] = { ldo[i].mv, 7, 1 };
		int rc = pm_rpmh(ldo[i].res, 3, off, val);
		printf("wlan power: %s %u mV on -> %d\n", ldo[i].res, ldo[i].mv, rc);
		if (rc)
			return -1;
	}
	/* the WCN399x reference clock (mainline: clocks = <&rpmhcc RPMH_RF_CLK2>, "cxo_ref_clk_pin"): VRM enable */
	uint32_t off[1] = { 4 }, val[1] = { 1 };
	int rc = pm_rpmh("rfclka2", 1, off, val);
	printf("wlan power: rfclka2 on -> %d\n", rc);
	if (rc)
		return -1;
	/* mainline sm8150 wifi: clocks "cxo_ref_clk_pin", "qdss" = <&aoss_qmp>: the QDSS clock is an AOSS message */
	return qmp_send("{class: clock, res: qdss, val: 1}") ? -1 : 0;
}

/* SMP2P item: magic "$SMP", version/features, local/remote pid, total/valid entries, flags, entries[16+4] */
static void smp2p_show(const char *what, unsigned item)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, item, &pa, &sz)) {
		printf("smp2p %s (item %u): absent\n", what, item);
		return;
	}
	uint32_t w1 = rd32(pa + 4), pids = rd32(pa + 8), counts = rd32(pa + 12);
	printf("smp2p %s (item %u) at %llx: magic %08x ver %u feat %x pids %u->%u entries %u/%u flags %x\n", what, item,
	       (unsigned long long)pa, rd32(pa), w1 & 0xff, w1 >> 8, pids & 0xffff, pids >> 16, counts >> 16,
	       counts & 0xffff, rd32(pa + 16));
	for (uint32_t i = 0; i < (counts >> 16) && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char name[17];
		for (int k = 0; k < 16; k++)
			name[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		name[16] = 0;
		printf("  entry %u: %-16s value %08x\n", i, name, rd32(e + 16));
	}
}

/* ---- SMP2P outbound (Linux qcom_smp2p_alloc_outbound_item) and the APCS doorbell ---- */

#define APCS_IPC	0x1799000cull	/* bonito: apss_shared (0x17990000) + 12: write BIT(n) */
#define IPC_MODEM_SMP2P	14
#define IPC_MODEM_GLINK	12
#define SMP2P_OUT	428
#define SMP2P_IN	435

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

static uint64_t smp2p_out_pa;

/* Our outbound item (Linux allocates it at probe, before the modem boots: the modem needs it). */
static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(SMEM_HOST_MODEM, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		fprintf(stderr, "smp2p: cannot allocate item %u\n", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (1u << 16));		/* local pid 0, remote pid 1 */
		wr32(pa + 12, 16 | (1u << 16));		/* total 16 entries, 1 valid */
		wr32(pa + 16, 0);			/* flags */
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);		/* "$SMP" */
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1 last, features: SSR ack */
	}
	smp2p_out_pa = pa;
	kick(IPC_MODEM_SMP2P);
	return 0;
}

/* Address of the value of the modem's inbound entry NAME (0 = found). */
static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static int smp2p_in_value(const char *name, uint32_t *v)
{
	uint64_t va;
	if (smp2p_in_entry(name, &va))
		return -1;
	*v = rd32(va);
	return 0;
}

/*
 * Keep the SMP2P side going: SSR ack (Linux qcom_smp2p_do_ssr_ack: after a
 * restart the modem flips RESTART_DONE, flags bit 0, and waits for the same
 * in our RESTART_ACK, bit 1), and report changes of the modem's state bits
 * (slave-kernel: 0 err-fatal, 1 ready, 2 handover, 3 stop-ack).
 */
static uint32_t modem_state = 0xffffffff;
static void smp2p_service(void)
{
	uint64_t ip;
	uint32_t sz, v;
	if (!smp2p_out_pa && smem_get(SMEM_HOST_MODEM, SMP2P_OUT, &smp2p_out_pa, &sz))
		return;
	if (smem_get(SMEM_HOST_MODEM, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out_pa + 16) & 2) != want) {
			wr32(smp2p_out_pa + 16, (rd32(smp2p_out_pa + 16) & ~2u) | want);
			kick(IPC_MODEM_SMP2P);
			printf("smp2p: restart %s\n", want ? "acked" : "ack cleared");
		}
	}
	if (smp2p_in_value("slave-kernel", &v) == 0 && v != modem_state) {
		printf("[%6llu] smp2p: modem state %x%s%s%s%s\n", (unsigned long long)link_ms(), v, v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "",
		       v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
		modem_state = v;
	}
}

static int smp2p_init(void)
{
	if (smp2p_create())
		return -1;
	printf("smp2p: outbound item %u at %llx ready, modem kicked\n", SMP2P_OUT, (unsigned long long)smp2p_out_pa);
	for (int t = 0; t < 50; t++) {
		smp2p_service();
		usleep(100000);
	}
	smp2p_show("in", SMP2P_IN);
	return 0;
}

/* ---- AOSS QMP (Linux qcom_aoss.c): messages to the always-on processor ---- */

#define QMP_RAM		0x0c300000ull
#define IPC_AOSS	0
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_open(void)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1) {
		fprintf(stderr, "qmp: magic %08x version %u\n", rd32(QMP_RAM), rd32(QMP_RAM + 4));
		return -1;
	}
	if (rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)
		return 0;	/* already open */
	wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));	/* ack ucore link state */
	wr32(QMP_RAM + 0x24, QMP_UP);			/* mcore link up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: link not acked\n");
		return -1;
	}
	wr32(QMP_RAM + 0x2c, QMP_UP);			/* mcore channel up */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: ucore channel not up\n");
		return -1;
	}
	wr32(QMP_RAM + 0x18, QMP_UP);			/* ack ucore channel */
	kick(IPC_AOSS);
	if (wait_eq(QMP_RAM + 0x30, QMP_UP, 1000)) {
		fprintf(stderr, "qmp: mcore channel not acked\n");
		return -1;
	}
	return 0;
}

static int qmp_send(const char *msg)
{
	if (qmp_open())
		return -1;
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	printf("qmp: \"%s\" -> %s\n", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- modem boot: MBA self-authentication (Linux qcom_q6v5_mss, MSS_SDM845 / downstream qcom,qdsp6v65-1-0) ----
 * bonito's TZ refuses PAS for the modem (wland pas-probe: PAS_IS_SUPPORTED(4) -> 0xffcfffc0) and the stock DT says
 * qcom,pil-self-auth: the Q6 boots the MBA, which authenticates the MPSS image via the RMB registers.
 * Addresses: stock DT qcom,mss@4080000 and the live (dtbo) reserved-memory that ABL hands over.
 */
#define PAS_MODEM	4
#define MPSS_PA		0x8b000000ull	/* live modem_region; modem.mdt spans exactly 0x8b000000..0x94800000 */
#define MPSS_SIZE	0x09800000ull
#define MBA_PA		0x95600000ull	/* live mba_region */
#define MBA_SIZE	0x00200000ull
#define FW_DIR		"/var/local/fw/modem"
#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

#define QDSP6SS		0x04080000ull	/* qdsp6_base */
#define  Q6_SLEEP		0x03c
#define  Q6_BOOT_CORE_START	0x400
#define  Q6_BOOT_CMD		0x404
#define RMB		0x04180000ull	/* rmb_base */
#define  RMB_MBA_IMAGE		0x00
#define  RMB_PBL_STATUS		0x04
#define  RMB_MBA_COMMAND	0x08
#define  RMB_MBA_STATUS		0x0c
#define  RMB_PMI_META_DATA	0x10
#define  RMB_PMI_CODE_START	0x14
#define  RMB_PMI_CODE_LENGTH	0x18
#define  RMB_MBA_MSS_STATUS	0x40
#define  RMB_MBA_ALT_RESET	0x44
#define HALT_Q6		0x01f63000ull
#define HALT_MODEM	0x01f65000ull
#define HALT_NC		0x01f64000ull
#define AOSS_MSS_RESTART 0x0c2b0000ull	/* aoss_cc 0xc2a0000 + 0x10000 */
#define PDC_SYNC_RESET	0x0b2e0100ull	/* pdc_reset + 0x100, bit qcom,mss_pdc_offset = 9 */
#define PDC_MODEM_BIT	(1u << 9)
#define GCC		0x00100000ull
#define  GCC_VOTE		0x52004	/* APCS clock branch ena vote */

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 1);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static void sleep_us(unsigned us)
{
	usleep(us);
}

/* A GCC branch clock: enable bit 0, wait for CLK_OFF (bit 31) to clear unless it is a voted branch. */
static int gcc_branch(const char *name, uint32_t off, int check)
{
	wr32(GCC + off, rd32(GCC + off) | 1);
	int rc = check ? wait_bit(GCC + off, 1u << 31, 0, 10) : 0;
	printf("mss: clk %-10s cbcr %05x = %08x%s\n", name, off, rd32(GCC + off), rc ? "  STILL OFF" : "");
	return rc;
}

/* A clock enabled through the APCS vote register; halt register only to report. */
static void gcc_vote(const char *name, unsigned bit, uint32_t halt)
{
	wr32(GCC + GCC_VOTE, rd32(GCC + GCC_VOTE) | (1u << bit));
	sleep_us(10);
	if (halt)
		printf("mss: clk %-10s vote bit %2u, cbcr %05x = %08x\n", name, bit, halt, rd32(GCC + halt));
	else
		printf("mss: clk %-10s vote bit %2u\n", name, bit);
}

/* Linux q6v5_reset_deassert with has_alt_reset: PDC sync reset held around an AOSS MSS restart pulse. */
static void mss_reset_deassert(void)
{
	wr32(PDC_SYNC_RESET, rd32(PDC_SYNC_RESET) | PDC_MODEM_BIT);
	wr32(RMB + RMB_MBA_ALT_RESET, 1);
	wr32(AOSS_MSS_RESTART, 1);
	sleep_us(300);
	wr32(AOSS_MSS_RESTART, 0);
	sleep_us(300);
	wr32(RMB + RMB_MBA_ALT_RESET, 0);
	wr32(PDC_SYNC_RESET, rd32(PDC_SYNC_RESET) & ~PDC_MODEM_BIT);
	printf("mss: reset deasserted (alt reset), pdc %08x\n", rd32(PDC_SYNC_RESET));
}

static void halt_axi(const char *name, uint64_t base)
{
	if (rd32(base + 8)) {			/* AXI_IDLE */
		printf("mss: halt %s: already idle\n", name);
		return;
	}
	wr32(base + 0, 1);			/* AXI_HALTREQ */
	int rc = wait_bit(base + 4, 1, 1, 100);	/* AXI_HALTACK */
	printf("mss: halt %s: ack %s, idle %u\n", name, rc ? "TIMEOUT" : "ok", rd32(base + 8));
	wr32(base + 0, 0);
}

/* Linux q6v5_xfer_mem_ownership: *perm = bitmask of VMIDs now owning the range. */
static int mem_owner(const char *what, uint64_t pa, uint64_t size, uint32_t *owners, int local, int remote)
{
	if (local == !!(*owners & (1u << VMID_HLOS)) && remote == !!(*owners & (1u << VMID_MSS_MSA)))
		return 0;
	uint32_t src[2], dst[2], perm[2];
	int ns = 0, nd = 0;
	if (*owners & (1u << VMID_HLOS))
		src[ns++] = VMID_HLOS;
	if (*owners & (1u << VMID_MSS_MSA))
		src[ns++] = VMID_MSS_MSA;
	if (local) {
		dst[nd] = VMID_HLOS;
		perm[nd++] = PERM_RWX;
	}
	if (remote) {
		dst[nd] = VMID_MSS_MSA;
		perm[nd++] = PERM_RW;
	}
	int rc = hyp_assign_perm(pa, (size + 0xfff) & ~0xfffull, src, ns, dst, perm, nd);
	printf("mss: %s -> %s%s: %s\n", what, local ? "apps " : "", remote ? "modem" : "", rc ? "FAILED" : "ok");
	if (!rc) {
		*owners = (local ? 1u << VMID_HLOS : 0) | (remote ? 1u << VMID_MSS_MSA : 0);
		owners_save();
	}
	return rc;
}

static uint32_t mba_owners = 1u << VMID_HLOS, mpss_owners = 1u << VMID_HLOS;

/*
 * Who owns the MBA and MPSS regions survives this process ("boot mba" leaves the MBA with the modem): a run that
 * wrote into a region the modem owns would take an XPU fault. Kept in a file until the phone reboots.
 */
#define OWNERS_FILE	"/var/local/wland-mss-owners"
static void owners_load(void)
{
	FILE *f = fopen(OWNERS_FILE, "r");
	if (f) {
		if (fscanf(f, "%x %x", &mba_owners, &mpss_owners) == 2)
			printf("mss: from a previous run: mba owners %#x, mpss owners %#x\n", mba_owners, mpss_owners);
		fclose(f);
	}
}
static void owners_save(void)
{
	FILE *f = fopen(OWNERS_FILE, "w");
	if (f) {
		fprintf(f, "%x %x\n", mba_owners, mpss_owners);
		fclose(f);
	}
}

/* q6v5_rmb_mba_wait: 0 = any non-zero status; a negative status is an error code from the MBA. */
static int32_t rmb_wait(uint32_t want, unsigned ms)
{
	int32_t v = 0;
	for (unsigned i = 0; i <= ms; i++) {
		v = (int32_t)rd32(RMB + RMB_MBA_STATUS);
		if (v < 0 || (!want && v) || (want && (uint32_t)v == want))
			return v;
		delay_ms(1);
	}
	return v ? v : -110;			/* -ETIMEDOUT */
}

static void rmb_show(const char *when)
{
	printf("mss: rmb %s: image %08x pbl %08x cmd %08x mba %08x meta %08x start %08x len %08x mss %08x\n", when,
	       rd32(RMB + RMB_MBA_IMAGE), rd32(RMB + RMB_PBL_STATUS), rd32(RMB + RMB_MBA_COMMAND), rd32(RMB + RMB_MBA_STATUS),
	       rd32(RMB + RMB_PMI_META_DATA), rd32(RMB + RMB_PMI_CODE_START), rd32(RMB + RMB_PMI_CODE_LENGTH),
	       rd32(RMB + RMB_MBA_MSS_STATUS));
}

static void mss_fail(void)
{
	rmb_show("at failure");
	halt_axi("q6", HALT_Q6);
	halt_axi("modem", HALT_MODEM);
	halt_axi("nc", HALT_NC);
	mem_owner("mba", MBA_PA, MBA_SIZE, &mba_owners, 1, 0);
}

/*
 * Stage 1 ("boot clocks"): load_state, rail votes, clocks, reset release. Stage 2 ("boot mba"): + MBA running.
 * Stage 3 (plain "boot"): + MPSS authenticated and running, SMP2P ready.
 */
static int boot_modem_stage(int stage)
{
	owners_load();
	/* 1. q6v5_prepare: the AOP gets load_state before the Q6 runs */
	if (qmp_send("{class: image, res: load_state, name: modem, val: on}"))
		return -1;

	/* 2. proxy power domains cx, mx, mss: the top levels (downstream votes vdd >= 385 on all three) */
	static const char *const rails[] = { "cx.lvl", "mx.lvl", "mss.lvl" };
	for (unsigned r = 0; r < 3; r++) {
		uint64_t aux;
		uint32_t len, top = 0;
		if (cmddb_aux(rails[r], &aux, &len)) {
			fprintf(stderr, "mss: %s not in cmd-db\n", rails[r]);
			return -1;
		}
		for (uint32_t i = 0; i < len / 2; i++)
			if (aux16(aux, i))
				top = i;
		if (vote(rails[r], top))
			return -1;
	}

	/* 3. proxy clocks (xo is the always-on CXO here), reset clocks, reset release, active clocks */
	gcc_vote("prng", 13, 0x34004);
	if (gcc_branch("iface", 0x8a000, 1) | gcc_branch("snoc_axi", 0x8a150, 1))
		return -1;
	mss_reset_deassert();
	gcc_branch("bus", 0x8a154, 0);		/* BRANCH_VOTED: no halt check */
	gcc_vote("mem", 10, 0x38004);
	gcc_vote("gpll0_mss", 17, 0);
	gcc_branch("mnoc_axi", 0x8a004, 0);	/* BRANCH_VOTED */
	rmb_show("after clocks");
	if (stage <= 1)
		return 0;

	/* 4. MBA into its region (q6v5_load), then both regions to the modem's VM */
	size_t mba_len;
	uint8_t *mba = slurp(FW_DIR "/mba.mbn", &mba_len);
	if (!mba || mba_len > 0x100000) {
		fprintf(stderr, "mss: %s/mba.mbn missing or over 1 MB\n", FW_DIR);
		return -1;
	}
	if (mem_owner("mba", MBA_PA, MBA_SIZE, &mba_owners, 1, 0))	/* a previous run may have left it assigned */
		return -1;
	pm_wr(MBA_PA, mba, (mba_len + 3) & ~(size_t)3);
	free(mba);
	printf("mss: mba.mbn (%zu bytes) at %llx\n", mba_len, MBA_PA);
	if (mem_owner("mpss", MPSS_PA, MPSS_SIZE, &mpss_owners, 0, 1) ||
	    mem_owner("mba", MBA_PA, MBA_SIZE, &mba_owners, 0, 1))
		return -1;
	wr32(RMB + RMB_MBA_IMAGE, (uint32_t)MBA_PA);

	/* 5. q6v5proc_reset, MSS_SDM845: sleep clock, core start, boot FSM, PBL */
	wr32(QDSP6SS + Q6_SLEEP, rd32(QDSP6SS + Q6_SLEEP) | 1);
	if (wait_bit(QDSP6SS + Q6_SLEEP, 1u << 31, 0, 1)) {
		printf("mss: QDSP6SS sleep clock stays off (%08x)\n", rd32(QDSP6SS + Q6_SLEEP));
		mss_fail();
		return -1;
	}
	wr32(QDSP6SS + Q6_BOOT_CORE_START, 1);
	wr32(QDSP6SS + Q6_BOOT_CMD, 1);
	if (wait_bit(RMB + RMB_MBA_MSS_STATUS, 1, 1, 10)) {
		printf("mss: boot FSM did not complete\n");
		mss_reset_deassert();
		mss_fail();
		return -1;
	}
	uint32_t pbl = 0;
	for (int t = 0; t <= 1000 && !(pbl = rd32(RMB + RMB_PBL_STATUS)); t++)
		delay_ms(1);
	printf("mss: PBL status %u%s\n", pbl, pbl == 1 ? " (success)" : "");
	if (pbl != 1) {
		mss_fail();
		return -1;
	}
	int32_t st = rmb_wait(0, 5000);
	printf("mss: MBA status %d%s\n", st, st == 1 || st == 2 ? " (XPU unlocked: MBA running)" : "");
	if (st != 1 && st != 2) {
		mss_fail();
		return -1;
	}
	if (stage <= 2)
		return 0;

	/* 6. MPSS metadata (ELF header + program headers + hash segment) authenticated by the MBA */
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/modem.mdt", &mlen);
	if (!mdt) {
		fprintf(stderr, "mss: no %s/modem.mdt\n", FW_DIR);
		mss_fail();
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		fprintf(stderr, "mss: no hash segment\n");
		mss_fail();
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = malloc(meta_len);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/modem.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			fprintf(stderr, "mss: hash segment %s missing\n", path);
			mss_fail();
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa) {
		mss_fail();
		return -1;
	}
	pm_wr(meta_pa, meta, (meta_len + 3) & ~(size_t)3);
	free(meta);
	uint32_t meta_owners = 1u << VMID_HLOS;
	wr32(RMB + RMB_PMI_CODE_LENGTH, 0);
	if (mem_owner("metadata", meta_pa, meta_len, &meta_owners, 0, 1)) {
		mss_fail();
		return -1;
	}
	wr32(RMB + RMB_PMI_META_DATA, (uint32_t)meta_pa);
	wr32(RMB + RMB_MBA_COMMAND, 1);			/* RMB_CMD_META_DATA_READY */
	st = rmb_wait(3, 1000);
	printf("mss: metadata (%zu bytes at %llx) -> status %d%s\n", meta_len, (unsigned long long)meta_pa, st,
	       st == 3 ? " (authenticated)" : "");
	int back = mem_owner("metadata", meta_pa, meta_len, &meta_owners, 1, 0);
	if (!back)
		pm_free(meta_pa);			/* never free memory the modem still owns */
	if (st != 3) {
		mss_fail();
		return -1;
	}

	/* 7. segments: MPSS back to apps, shared during the copy; RMB told the code start once, then the length so far */
	uint64_t lo = ~0ull;
	for (uint32_t i = 0; i < phnum; i++)
		if (ph[i].type == 1 && (int)i != hash && ph[i].memsz && ph[i].paddr < lo)
			lo = ph[i].paddr;
	if (lo != MPSS_PA)
		printf("mss: image starts at %llx, region at %llx (relocated)\n", (unsigned long long)lo, MPSS_PA);
	mem_owner("mpss", MPSS_PA, MPSS_SIZE, &mpss_owners, 1, 0);
	if (mem_owner("mpss", MPSS_PA, MPSS_SIZE, &mpss_owners, 1, 1)) {
		mss_fail();
		return -1;
	}
	static uint8_t zeros[0x10000];
	uint64_t total = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t off = ph[i].paddr - lo, pa = MPSS_PA + off;
		if (off + ph[i].memsz > MPSS_SIZE || ph[i].filesz > ph[i].memsz) {
			fprintf(stderr, "mss: segment %u outside the region or filesz > memsz\n", i);
			mss_fail();
			return -1;
		}
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/modem.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl != ph[i].filesz) {
				fprintf(stderr, "mss: %s missing or wrong size\n", path);
				free(seg);
				mss_fail();
				return -1;
			}
			pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(pa + o, zeros, (n + 3) & ~3ull);
		}
		total += ph[i].memsz;
		if (!rd32(RMB + RMB_PMI_CODE_LENGTH)) {
			wr32(RMB + RMB_PMI_CODE_START, (uint32_t)MPSS_PA);	/* relocatable: the region start */
			wr32(RMB + RMB_MBA_COMMAND, 2);				/* RMB_CMD_LOAD_READY */
		}
		wr32(RMB + RMB_PMI_CODE_LENGTH, (uint32_t)total);
		st = (int32_t)rd32(RMB + RMB_MBA_STATUS);
		if (st < 0) {
			printf("mss: segment %u: MPSS authentication failed: %d\n", i, st);
			mss_fail();
			return -1;
		}
	}
	printf("mss: %llu bytes of segments loaded\n", (unsigned long long)total);

	/* 8. MPSS to the modem only, wait for the MBA's verdict */
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0)
		wr32(ve, 0);				/* forget a previous run's ready/fatal bits */
	modem_state = 0xffffffff;
	if (mem_owner("mpss", MPSS_PA, MPSS_SIZE, &mpss_owners, 0, 1)) {
		mss_fail();
		return -1;
	}
	st = rmb_wait(4, 10000);
	printf("mss: MPSS authentication -> status %d%s\n", st, st == 4 ? " (complete)" : "");
	if (st != 4) {
		mss_fail();
		return -1;
	}

	/* 9. wait for SMP2P in: "ready" (bit 1), then the MBA region back to apps */
	for (int t = 0; t < 300; t++) {
		uint32_t v;
		smp2p_service();
		if (smp2p_in_value("slave-kernel", &v) == 0 && (v & 2) && !(v & 1)) {
			printf("mss: modem ready after %d00 ms\n", t);
			mem_owner("mba", MBA_PA, MBA_SIZE, &mba_owners, 1, 0);
			return 0;
		}
		usleep(100000);
	}
	printf("mss: no ready from the modem in 30 s\n");
	smp2p_show("in", 435);
	mem_owner("mba", MBA_PA, MBA_SIZE, &mba_owners, 1, 0);
	return -1;
}

static int boot_modem(void)
{
	return boot_modem_stage(3);
}

static void net_cb(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	uint32_t w[5] = { 0 };
	memcpy(w, d, n < sizeof(w) ? n : sizeof(w));
	if (h->type == QRTR_TYPE_DATA) {
		if (h->confirm_rx)
			qrtr_resume_tx(h);
		if (h->dst_port == RMTFS_PORT) {
			rmtfs_handle(h, d, n);
			return;
		}
		if (h->dst_port == WLFW_PORT) {
			wlfw_handle(h, d, n);
			return;
		}
		if (h->dst_port == PDMAPPER_PORT) {
			pdmapper_handle(h, d, n);
			return;
		}
		if (h->dst_port == TFTP_PORT || (h->dst_port >= TFTP_XFER_PORT0 && h->dst_port < TFTP_XFER_PORT0 + TFTP_XFERS)) {
			tftp_handle(h, d, n);
			return;
		}
		printf("qrtr: < DATA %u:%x -> %u:%x, %u bytes\n", h->src_node, h->src_port, h->dst_node, h->dst_port, n);
		return;
	}
	printf("[%6llu] qrtr: < ctrl type %u (cmd %u) from %u:%x: %x %x %x %x\n", (unsigned long long)link_ms(), h->type, w[0], h->src_node, h->src_port,
	       w[1], w[2], w[3], w[4]);
	if (w[0] == QRTR_TYPE_HELLO) {
		uint32_t hello[5] = { QRTR_TYPE_HELLO };	/* struct qrtr_ctrl_pkt: always 20 bytes */
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h->src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
	} else if (w[0] == QRTR_TYPE_NEW_SERVER) {
		printf("qrtr:   server service %#x instance %#x at %u:%x\n", w[1], w[2], w[3], w[4]);
		if (w[1] == WLFW_SERVICE)
			wlfw_new_server(w[3], w[4]);
	}
}

/* the network to join: "ssid=NAME" and "psk=PASSPHRASE" lines (on the phone only, never in a repo) */
#define WLAN_CONF	"/var/local/wlan.conf"
static char conf_ssid[33], conf_psk[65];

static void read_conf(void)
{
	char line[128];
	FILE *f = fopen(WLAN_CONF, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "ssid=", 5))
			snprintf(conf_ssid, sizeof(conf_ssid), "%s", line + 5);
		else if (!strncmp(line, "psk=", 4))
			snprintf(conf_psk, sizeof(conf_psk), "%s", line + 4);
	}
	fclose(f);
	printf("net: will join '%s' (%s)\n", conf_ssid, conf_psk[0] ? "WPA2-PSK" : "open");
}

/*
 * Join after the first scan and stay joined: a failed try again after 5 s (4 times), then every 30 s with a
 * fresh scan first; a lost link (deauth, kickout, beacon miss) joins again after 3 s.
 */
static void join_poll(void)
{
	static int asked, joined, fails;
	static uint64_t next;
	if (!conf_ssid[0] || !radio_scans() || bridge_joins)
		return;				/* (wifid has taken over: bridge.c) */
	uint64_t now = link_ms();
	int r = radio_join_result();
	if (asked) {
		if (r == -1)
			return;				/* running */
		asked = 0;
		if (r == 0) {
			joined = 1;
			fails = 0;
			return;
		}
		fails++;
		next = now + (fails < 4 ? 5000 : 30000);
		if (fails >= 4)
			radio_rescan(25000);
		printf("[%6llu] net: join failed (%s), try %d, next in %llu s\n", (unsigned long long)now, strerror(r), fails,
		       (unsigned long long)(next - now) / 1000);
		return;
	}
	if (joined) {
		if (r != ENOTCONN)
			return;
		joined = 0;
		fails = 0;
		next = now + 3000;
		printf("[%6llu] net: link lost, joining again in 3 s\n", (unsigned long long)now);
		return;
	}
	if (next && now < next)
		return;
	r = radio_join(conf_ssid, conf_psk);
	printf("[%6llu] net: join '%s' (try %d) -> %d\n", (unsigned long long)now, conf_ssid, fails + 1, r);
	if (r == EINVAL || r == EIO) {
		printf("net: cannot join with this wlan.conf; not trying again\n");
		conf_ssid[0] = 0;
		return;
	}
	asked = 1;
}

static volatile sig_atomic_t drop_asked;

static void on_usr1(int sig)
{
	drop_asked = 1;
}

static void net_idle(void)
{
	if (drop_asked) {
		drop_asked = 0;
		radio_disconnect();
	}
	smp2p_service();
	radio_poll();
	ipnet_poll();
	bridge_poll();
	join_poll();
}

static int net(unsigned secs)
{
	if (getenv("WLAND_VERBOSE"))
		verbose_link = atoi(getenv("WLAND_VERBOSE"));
	link_idle = net_idle;
	signal(SIGUSR1, on_usr1);
	signal(SIGCHLD, SIG_IGN);	/* netsvc children (ipnet.c) reap themselves */
	if (link_up(net_cb, 5000))
		return -1;
	if (rmtfs_init())
		printf("net: no rmtfs\n");
	if (pdmapper_init() || tftp_init())
		printf("net: no pd-mapper/tftp\n");
	read_conf();
	uint64_t end = link_ms() + (uint64_t)secs * 1000, stats = link_ms() + 30000;
	while (!secs || link_ms() < end) {
		link_poll(100);
		if (link_ms() > stats) {
			radio_stats();
			stats = link_ms() + 30000;
		}
		if (!wlfw_pending())
			continue;
		if (wlfw_start()) {
			printf("net: WLFW bring-up failed\n");
		} else if (!getenv("WLAND_NO_RADIO")) {
			delay_ms(100);	/* topaz: only after mission mode; before it a CE access hangs the bus */
			if (radio_start())
				printf("net: radio start failed\n");
			else if (bridge_open() != 0 && conf_ssid[0] && ipnet_init())
				printf("net: no utun: joining without IP\n");
		}
	}
	radio_summary();
	radio_stats();
	return 0;
}

/* everything `up` does, from a fresh boot */
static int up(unsigned secs)
{
	if (smmu_wlan_identity())
		printf("up: no WLAN SMMU stream: the radio will not start\n");
	if (wlan_power() || smp2p_create() || boot_modem())
		return 1;
	return net(secs) ? 1 : 0;
}

/*
 * The boot job: `up 0` at most once per phone boot (a second modem boot in the same phone boot is untested and
 * MSA/rmtfs hangs wedge a CPU), unless switched off. stdout/stderr go to /var/local/wland.log.
 */
#define WLAND_LOG	"/var/local/wland.log"
#define WLAND_STAMP	"/var/local/wland.boot"
/* libsystem_kernel: jetsam control (root may change its own limits); MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES */
int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t size);
struct memlimit_properties { int32_t active; uint32_t active_attr; int32_t inactive; uint32_t inactive_attr; };

static int auto_up(void)
{
	/* launchd's per-process limit for a job killed us while the modem firmware loaded (jetsam reason 7) */
	struct memlimit_properties ml = { 2048, 0, 2048, 0 };
	int mrc = memorystatus_control(7, getpid(), 0, &ml, sizeof(ml));
	char args[1024] = "";
	size_t n = sizeof(args) - 1;
	sysctlbyname("kern.bootargs", args, &n, NULL, 0);
	if (!access("/var/local/wland.off", F_OK) || strstr(args, "sweetwifi=0")) {
		fprintf(stderr, "wland: off (/var/local/wland.off or sweetwifi=0)\n");
		return 0;
	}
	struct timeval bt = { 0 };
	n = sizeof(bt);
	sysctlbyname("kern.boottime", &bt, &n, NULL, 0);
	char stamp[32] = "", now[32];
	snprintf(now, sizeof(now), "%ld\n", (long)bt.tv_sec);
	FILE *f = fopen(WLAND_STAMP, "r");
	if (f) {
		if (!fgets(stamp, sizeof(stamp), f))
			stamp[0] = 0;
		fclose(f);
	}
	if (!strcmp(stamp, now)) {
		fprintf(stderr, "wland: already started in this boot\n");
		return 1;
	}
	if ((f = fopen(WLAND_STAMP, "w"))) {
		fputs(now, f);
		fclose(f);
	}
	rename(WLAND_LOG, WLAND_LOG ".prev");
	if (freopen(WLAND_LOG, "a", stdout))
		dup2(fileno(stdout), 2);
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("wland auto: boot %ld, memory limit 2048 MB -> %d\n", (long)bt.tv_sec, mrc);
	return up(0);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: wland smem | smp2p | vote RES LVL | levels RES | boot\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);	/* a stalled bus access must not swallow the output */
	pm_init();
	if (smem_init())
		return 1;
	const char *c = argv[1];
	if (!strcmp(c, "smem")) {
		smem_dump();
	} else if (!strcmp(c, "smp2p")) {
		smp2p_show("in", 435);	/* qcom,smem = <435>, <428>: inbound, outbound */
		smp2p_show("out", 428);
	} else if (!strcmp(c, "levels") && argc > 2) {
		levels(argv[2], 1);
	} else if (!strcmp(c, "qmp") && argc > 2) {
		return qmp_send(argv[2]) ? 1 : 0;
	} else if (!strcmp(c, "smp2p-init")) {
		return smp2p_init() ? 1 : 0;
	} else if (!strcmp(c, "pas-probe")) {
		/* bonito: can TrustZone boot the modem through PAS (as on flame), or does it need the MBA path
		 * (Linux qcom_q6v5_mss)? SCM PAS_IS_SUPPORTED (svc 2, cmd 7) for the modem (4) and, as a control,
		 * the GPU zap shader (13, known to work). */
		static const unsigned ids[] = { 4, 13 };
		for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
			uint64_t r1 = 0;
			int64_t rc = scm(7, 1, ids[i], 0, 0, &r1);
			printf("pas %u supported -> rc %lld r1 %llu\n", ids[i], (long long)rc, (unsigned long long)r1);
		}
	} else if (!strcmp(c, "shutdown")) {
		uint64_t r1 = 0;
		int64_t rc = scm(6, 1, PAS_MODEM, 0, 0, &r1);	/* PAS_SHUTDOWN */
		printf("shutdown -> %lld/%llx\n", (long long)rc, (unsigned long long)r1);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "items")) {
		smem_list();
	} else if (!strcmp(c, "item") && argc > 2) {
		uint64_t pa;
		uint32_t sz;
		unsigned host = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : SMEM_HOST_MODEM;
		if (smem_get(host, (unsigned)atoi(argv[2]), &pa, &sz)) {
			printf("item %s: not found\n", argv[2]);
			return 1;
		}
		printf("item %s: pa %llx size %u\n", argv[2], (unsigned long long)pa, sz);
		for (uint32_t o = 0; o < sz && o < 4096; o += 16) {
			uint8_t b[16];
			for (int k = 0; k < 16; k += 4) {
				uint32_t w = rd32(pa + o + k);
				memcpy(b + k, &w, 4);
			}
			printf("%04x:", o);
			for (int k = 0; k < 16; k++)
				printf(" %02x", b[k]);
			printf("  ");
			for (int k = 0; k < 16; k++)
				putchar(b[k] >= 32 && b[k] < 127 ? b[k] : '.');
			printf("\n");
		}
	} else if (!strcmp(c, "net")) {
		return net(argc > 2 ? (unsigned)atoi(argv[2]) : 10) ? 1 : 0;
	} else if (!strcmp(c, "auto")) {
		return auto_up();
	} else if (!strcmp(c, "up")) {
		/* everything from a fresh phone boot: SMP2P, the modem, GLINK + QRTR; the WLAN SMMU stream first */
		return up(argc > 2 ? (unsigned)atoi(argv[2]) : 20);
	} else if (!strcmp(c, "smmu")) {
		return smmu_wlan_identity() ? 1 : 0;
	} else if (!strcmp(c, "cmddb")) {
		cmddb_list();
	} else if (!strcmp(c, "wlan-power")) {
		return wlan_power() ? 1 : 0;
	} else if (!strcmp(c, "boot")) {
		/* "boot clocks" / "boot mba" stop after that stage; plain "boot" runs the modem */
		int stage = argc > 2 && !strcmp(argv[2], "clocks") ? 1 : argc > 2 && !strcmp(argv[2], "mba") ? 2 : 3;
		return boot_modem_stage(stage) ? 1 : 0;
	} else if (!strcmp(c, "bcm") && argc > 3) {
		/* a BCM (bus) vote: x = average, y = peak, in the BCM's own units (0x3fff = its top level); commit + valid */
		uint32_t x = (uint32_t)strtoul(argv[2 + 1], NULL, 0) & 0x3fff, y = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) & 0x3fff : x;
		uint32_t off[1] = { 0 }, val[1] = { 1u << 30 | 1u << 29 | x << 14 | y };
		int rc = pm_rpmh(argv[2], 1, off, val);
		printf("bcm %s x %#x y %#x (%#x) -> %d\n", argv[2], x, y, val[0], rc);
		return rc ? 1 : 0;
	} else if (!strcmp(c, "vote") && argc > 3) {
		return vote(argv[2], (uint32_t)strtoul(argv[3], NULL, 0)) ? 1 : 0;
	} else {
		fprintf(stderr, "wland: unknown command\n");
		return 2;
	}
	return 0;
}
