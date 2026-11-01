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

	.p2align 4
	.globl sqe_fw, sqe_fw_end
sqe_fw:
	.incbin "../research/gpu/fw/a630_sqe.fw"
sqe_fw_end:

	.p2align 4
	.globl zap_mdt, zap_mdt_end
zap_mdt:
	.incbin "../research/gpu/fw/a615_zap.mdt"
zap_mdt_end:

	.p2align 4
	.globl zap_b02, zap_b02_end
zap_b02:
	.incbin "../research/gpu/fw/a615_zap.b02"
zap_b02_end:
