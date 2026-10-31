#pragma once
#include <stdint.h>

#define FB_BASE		0x9c000000UL	/* reserved-memory cont_splash_region */
#define FB_W		1080
#define FB_H		2160	/* bonito (Pixel 3a XL); sweet was 2400 */
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
/* "name  base size" row with 40-bit physical addresses */
void con_reg(const char *name, uint64_t base, uint64_t size);
void con_line(const char *s, uint32_t fg);
