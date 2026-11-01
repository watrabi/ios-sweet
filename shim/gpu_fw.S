/*
 * GMU firmware for the bonito GPU probe, embedded from research/gpu/fw/
 * (pulled from the phone's own /vendor/firmware; never committed).
 */
	.section .rodata
	.p2align 4
	.globl gmu_fw, gmu_fw_end
gmu_fw:
	.incbin "../research/gpu/fw/a630_gmu.bin"
gmu_fw_end:
