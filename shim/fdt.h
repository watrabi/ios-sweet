#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Draw the root "model", every /memory bank and every /reserved-memory child
 * (fixed "reg" regions, and "size"-only dynamic pools) on the console.
 * Returns the number of memory banks found.
 */
int fdt_dump_memmap(uintptr_t fdt);
