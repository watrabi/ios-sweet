/* Host check of fdt_dump_memmap: ./out/host_fdt DTB -> out/host_fdt.ppm */
#include <stdio.h>
#include <stdlib.h>
#include "console.h"
#include "fdt.h"

static uint32_t host_fb[FB_H * FB_STRIDE / 4];

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	static uint32_t dtb[4 << 20];	/* 16 MiB, 4-byte aligned */
	size_t n = fread(dtb, 1, sizeof dtb, f);
	fclose(f);
	printf("read %zu bytes\n", n);
	con_fb = host_fb;
	con_clear(RED);
	con_line("PAGE 1/2  live FDT memory map (base 0 = dynamic pool)", GREEN);
	printf("banks=%d\n", fdt_dump_memmap((uintptr_t)dtb));
	f = fopen("out/host_fdt.ppm", "wb");
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}
