/*
 * Just enough flattened-device-tree parsing to find what ABL put in
 * /chosen (initrd location). Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"
#include "console.h"

#define RED_TEXT 0xffff6060U

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

static int starts(const char *s, const char *pfx)
{
	while (*pfx && *s == *pfx)
		s++, pfx++;
	return !*pfx;
}

static uint64_t be_cells(uintptr_t a, uint32_t n)
{
	uint64_t v = 0;
	for (uint32_t i = 0; i < n; i++)
		v = v << 32 | be32(a + 4 * i);
	return v;
}

#define MAX_DEPTH 16

int fdt_dump_memmap(uintptr_t fdt)
{
	if (!fdt || be32(fdt) != FDT_MAGIC) {
		con_line("FDT: bad magic", RED_TEXT);
		return 0;
	}
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	/* #address-cells/#size-cells declared by the node at each depth (for its children) */
	uint32_t ac[MAX_DEPTH], sc[MAX_DEPTH];
	int depth = 0, banks = 0, nres = 0;
	int in_mem = 0, in_rsv_parent = 0, in_rsv_child = 0;
	const char *name = 0;
	uint64_t dyn_size = 0;
	int had_reg = 0;

	ac[0] = 2, sc[0] = 1;	/* spec defaults */
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			name = (const char *)p;
			depth++;
			if (depth < MAX_DEPTH)
				ac[depth] = 2, sc[depth] = 1;
			if (depth == 2) {
				in_mem = streq(name, "memory") || starts(name, "memory@");
				in_rsv_parent = streq(name, "reserved-memory");
			}
			in_rsv_child = depth == 3 && in_rsv_parent;
			dyn_size = 0;
			had_reg = 0;
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			if (in_rsv_child && depth == 3) {
				if (!had_reg && dyn_size)
					con_reg(name, 0, dyn_size);	/* base 0 = dynamic pool */
				in_rsv_child = 0;
			}
			if (depth == 2)
				in_mem = in_rsv_parent = 0;
			depth--;
			if (depth == 0) {
				con_hex2("TOTAL", "banks=", banks, "rsv=", nres);
				return banks;
			}
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pn = (const char *)(strings + nameoff);
			int d = depth < MAX_DEPTH ? depth : MAX_DEPTH - 1;
			if (streq(pn, "#address-cells"))
				ac[d] = be32(val);
			else if (streq(pn, "#size-cells"))
				sc[d] = be32(val);
			else if (depth == 1 && streq(pn, "model"))
				con_str("MODEL", (const char *)val);
			else if (streq(pn, "reg") && ((in_mem && depth == 2) || in_rsv_child)) {
				/* cells come from the parent node (depth - 1) */
				uint32_t a = ac[d - 1], s = sc[d - 1], step = 4 * (a + s);
				for (uint32_t off = 0; step && off + step <= len; off += step) {
					uint64_t base = be_cells(val + off, a);
					uint64_t size = be_cells(val + off + 4 * a, s);
					con_reg(in_mem ? "MEM bank" : name, base, size);
					if (in_mem)
						banks++;
					else
						nres++;
				}
				had_reg = 1;
			} else if (in_rsv_child && streq(pn, "size"))
				dyn_size = be_cells(val, len / 4 > 2 ? 2 : len / 4);
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			con_line("FDT: parse stopped", RED_TEXT);
			return banks;
		}
	}
}
