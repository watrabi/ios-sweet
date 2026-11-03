# ios-sweet on bonito (Pixel 3a XL, SDM670) - port notes

Branch `bonito` of a clone of dipa-studio/ios-sweet. Goal: same XNU + iOS 16 userland,
new hardware layer. Upstream log for everything SoC-independent: `NOTES.md`.

## Device (observed over adb, 2026-10-31)
- Pixel 3a XL `bonito`, stock Android 12 `SP2A.220505.008`, bootloader `b4s4-0.4-8048689`.
- **Bootloader LOCKED** (`ro.boot.flash.locked=1`), OEM unlocking allowed (`sys.oem_unlock_allowed=1`),
  not a Verizon-variant unit (`ro.carrier=unknown`; the Verizon is just the SIM).
- No root: live FDT, /proc/iomem, panel id unreadable from Android.
- Storage **eMMC** (`/dev/block/platform/soc/7c4000.sdhci`), not UFS.
- Panel 1080x2160@60. Mainline drives both bonito panels (Tianma NT37700F, Samsung) in
  DSI **video** burst mode -> CPU writes to ABL's splash framebuffer should show without a kick (unverified).
- USB port is flaky.

## Static facts (stock boot.img DTB + mainline sdm670.dtsi; ABL fills memory sizes at boot)
- Stock boot.img: header v2, base 0, kernel 0x8000, ramdisk 0x1000000, tags 0x100, dtb 0x1f00000,
  page 4096 = identical layout to sweet. Stock DTB copied to `dtb/bonito-stock.dtb` (gitignored;
  fetched by HTTP range from the factory zip, ~31 MB).
- Framebuffer `cont_splash_region` 0x9c000000 (+0x2400000), a8r8g8b8, stride 4320.
- Fixed reserved-memory: all below 0x9f800000 (qseecom 0x9e400000+0x1400000); mainline also lists
  debug-info/ramoops 0xa1800000+0x411000 (dtbo overlay). Dynamic pools (adsp, sdsp, qseecom_ta, sp,
  secure_display 92M, cma) are Linux allocations with alloc-ranges <= 4G.
- GICv3 GICD 0x17a00000 / GICR 0x17a60000, timer PPIs 1/2/3/0 (vtimer INTID 19) - same as sweet.
- PSCI 1.0 via smc. CPUs MPIDR Aff1 0..7 (6x A55 + 2x A75).
- sdhc_1 0x7c4000 (hc), 0x7c5000 (cqhci), 0x7c8000 (ice); IRQ SPI 641/644.
- Touch: Synaptics RMI4 (F01, F12) at I2C 0x20 on i2c9 = QUP SE @0xa84000 (same SE address as sweet's
  touch), IRQ tlmm 125, 1.8V via PM660 GPIO 12. Vol-up PM660L GPIO 7, vol-down = PON RESIN.
- SPMI arbiter 0xc440000 (same as sweet). PS_HOLD assumed 0xc264000 (unverified on sdm670);
  IMEM restart-reason address unknown -> shim no longer writes it.

## Boot image requirements (verified 2026-10-31)
- Bootloader unlocked by the user 2026-10-31 (`fastboot flashing unlock`; Android data wiped).
- Raw arm64 Image -> `Error verifying the received boot.img: Unsupported`. ABL wants a compressed
  kernel: LZ4 frame works (`KCOMP=lz4`, default).
- LZ4 with header text_offset 0 -> `... Invalid Parameter`. text_offset 0x80000 (as stock) -> `Booting OKAY`.
  `start.S` now defaults TEXT_OFFSET to 0x80000. No AVB footer needed when unlocked.

## Milestone 1 RESULTS (boot of shim-boot-to80k.img, 2026-10-31, photos from the user)
- Display: splash fb at 0x9c000000 updates from plain CPU writes (video mode confirmed), 1080x2160.
- ABL: DTB at 0x84500000, initrd 0x84700000, image loaded at 0x80080000 (base + text_offset).
  Model "Google Inc. MSM sdm670 B4 PVT v1.0".
- **Memory banks: 0x80000000 +0x80000000 (2 GiB) and 0x100000000 +0x7e2c0000 (~1.97 GiB).
  The banks are physically adjacent (low ends exactly at 4 GiB).** Highest fixed carve-out ends at
  0xa1c11000 (ramoops_meta). => candidate contiguous XNU range 0xa2000000..0x17e2c0000 = 0xdc2c0000
  (~3.44 GiB), IF no undeclared XPU/TZ protection sits in it (dynamic pools secure_display 144M,
  qseecom_ta 16M, sp 8M, adsp 8M, sdsp 4M, mem_dump 36M, cma 32M are only claimed by Linux drivers;
  verify with a memory-walk test before trusting it). Conservative fallback: upper bank only (~1.97 GiB).
- Live reserved-memory (after dtbo overlay; differs from boot.img DTB): hyp 85700000+600000,
  xbl 85e00000+100000, removed 85fc0000+2f40000, camera 8ab00000+500000, modem 8b000000+9800000,
  video 94800000+500000, wlan_msa 94d00000+100000, cdsp 94e00000+800000, mba 95600000+200000,
  adsp 95800000+2200000, ipa_fw 97a00000+10000, ipa_gsi 97a10000+5000, gpu 97a15000+2000,
  qseecom 9e400000+1400000, cont_splash 9c000000+2400000, debug_info a1800000+10000,
  ramoops a1810000+200000, alt_ramoops a1a10000+200000, ramoops_meta a1c10000+1000.
- CPU: boot core MIDR 0x517f803c (Qualcomm Kryo 3xx Silver = A55), MPIDR 0x81000000, EL1.
  MMFR0 0x101122 (16K granule yes, 40-bit PA - identical to sweet), ISAR1 0x100001 (no PAC),
  PFR0 0x11112222 (GIC sysregs), ISAR0 0x10211120 (LSE atomics), CNTFRQ 19.2 MHz.
- PSCI 1.1, SMCCC_VERSION not supported (-1; sweet had 1.1), SYSTEM_SUSPEND/RESET2 unsupported.
- SMP: CPU_ON ok for all 7 secondaries, MPIDR 0x81000100..0x81000700 (CPU6/7 = the A75s).
- GIC: GICD_CTLR 0x10 -> 0x12, GICR 0x17a60000, ICC_CTLR 0x8440, vtimer INTID 19 = 30 IRQs/300 ms,
  SGI 10/10. Identical to sweet => SWEET kernel GIC/timer/SMP code should run unchanged.
- MMU: 16K tables ok, image WB, fb Normal-NC; 1 MiB memory loop 481 us.
- eMMC (sdhc_1): SDHCI 3.00 (ver 0x6c02), pres 0x01f800f0 (card inserted/stable, DAT/CMD idle, no inhibit),
  clk 0x0003 = internal clock on + stable but **SD card clock OFF**, power reg 0, ints all masked,
  caps 0x742dc8b2/0x0000808f (200 MHz base, 8-bit, ADMA2, 64-bit bus), CQHCI 5.10 present but disabled.
  => ABL tears the bus down before jumping. Registers are safe to touch; a driver must re-enable the
  SD clock and probably re-init the card (CMD0..CMD7, then 8-bit HS52; skip HS200/HS400 tuning).
  Watch for the sdhci-msm power-IRQ handshake on power/reset register writes.
- Cosmetic: "RESET in s" label too long (prints "RESET in00..B4" = 180 s).

## Memory walk (shim-memwalk.img, -DMEMWALK, 2026-10-31): PASS
0xa2000000..0x17e2c0000, 2 words per 64 KiB, MMU off (Device): 0x1b858 words, 0 mismatches, no
SError/abort, no reset. => ~3.44 GiB contiguous usable (uncached access verified; cached use by XNU
still to be confirmed). `tools/mkadt.py --bonito-wide` uses it; `--bonito` = upper bank only.

## Existing SWEET kernel on bonito (tools/bonito-kernel-image.sh KERNEL [upper|wide])
- Kernel: upstream CI artifact (run 39158169212, commit fdad0de, 2026-10-18), SHA256 d9cb893b...e1e5,
  in xnu/out/. Init: tools/sweet-init.S via GNU as + `mkinit.py --raw` (no Apple clang needed).
- Images: shim/out/shim-boot-xnu-wide.img, shim-boot-xnu-upper.img. Boot-args disable sweet touch/keys/usb.
- Result 2026-10-31 (shim-boot-xnu-wide.img, user report): **userspace runs** - unmodified sweet kernel
  boots on bonito with the wide 0xa2000000..0x17e2c0000 RAM range, md0 RAM-disk root, sweet-init as PID 1.
  So memory (cached), GIC, timer, early fbcon at 0x9c000000 and the loader all work as-is.
- Next: storage (eMMC driver replacing sweet_ufs; ABL leaves SD clock off), log channel, then the iOS root.

## eMMC probe (shim-emmc.img, -DEMMCPROBE, shim/emmc.c emmc_probe) - WORKS, read-only
Three boots 2026-10-31:
1. Host bring-up ok: GCC sdcc1 RCG left by ABL = src 2 (GPLL6 384 MHz) div 1, CBCR 0x4221 (on).
   **SDHCI divider works** (/1024 -> ~375 kHz), no GCC programming needed. msm power IRQ: BUS_ON (2)
   then IO_LOW (4) after 1.8 V signalling, both acked; IO pad switch set. Card: OCR c0ff8080 (ready,
   sector mode), CID mid 0x90 = SK hynix ("hC8aP>"), RCA 2, CMD7 -> state 4 (tran).
2. **PIO does not work**: after BRR, data port 0x20 reads 0 and PRES keeps buffer-read-enable (0x03f80af6),
   no xfer-complete. Caps 0x742dc8b2: ADMA2 yes, SDMA no. (Linux always uses ADMA here.)
3. **ADMA2 32-bit, one descriptor** (attr valid|end|tran, 512 B, buffers < 4 GiB, MMU off): works; the
   SMMU (sdhc_1 SID 0x140) lets ABL's state through. EXT_CSD rev 8 (eMMC 5.1), SEC_COUNT 122142720
   (58 GiB), PARTITION_CONFIG 0x38, CARD_TYPE 0x57, bus 1-bit legacy. GPT: 72 partitions.
   **userdata = LBA 17359872..122142686 (51163 MiB)**.
- Card only ever received CMD0/1/2/3/9/7/13 + reads CMD8/CMD17.
- Speed test (4th boot): CMD6 HS_TIMING=1 + host HS bit, SDHCI div 4 (384/8 = 48 MHz), CMD6 BUS_WIDTH=2 +
  host 8-bit (hctl 0x0d34, clk 0x0e0407); GPT header re-read via CMD18 matches; **16 MiB CMD18 + ADMA
  (64 KiB descriptors, auto-CMD12) in 367 ms = 43.5 MiB/s**. HS200 (~2x) is a later option.

## XNU eMMC driver: patch 0082 (commit 4c9b9d8) - WORKS on hardware (read-only)
- CI run 41254845073 (macos-14, 2026-11-01 00:00-00:06 UTC) success; kernel in xnu/out-0082.
- Boot shim-boot-0082-emmc-ro.img (wide RAM, rd=ufsroot0, no ufsrw): `SWEET EMMC: power irq 0x2/0x4, ocr
  0xc0ff8080, EXT_CSD rev 8, 122142720 sectors, cache off` / `high speed 48 MHz, 8-bit bus` / `"userdata" on
  LUN 0: first block 0x211c80, 51163 MiB, read-only, DMA at 0xa5f1c000` / `BSD root: ufsroot0` ->
  hfs: unknown Volume Signature e93f -> cannot mount root (expected: userdata holds no HFS yet).
- **FIRST BOOT FROM INTERNAL eMMC (2026-11-01 ~00:20 UTC).** `sudo tools/bonito-test-root.sh` -> 8 MiB
  case-sensitive HFS+ (HX v5, /sbin/launchd = sweet-init, /dev); user flashed it: `fastboot flash userdata
  shim/out/root-hfs.img` (userdata now = this test volume). Boot shim-boot-0082-emmc-ro.img: SWEET EMMC
  48 MHz 8-bit -> BSD root ufsroot0 -> `hfs: mounted SweetRoot on device b(1, 0)` -> root not authenticated,
  continuing unsealed -> load_init_program /sbin/launchd -> `*** SWEET userspace: init running as PID 1 ***`
  + heartbeat. Read-only (no ufsrw); write path (CMD24/25) untested.
- **WRITE PATH WORKS (2026-11-01 ~00:20 UTC)**: shim-boot-0082-emmc-rw.img (+ `ufsrw=1 rootrw=1 ufslogsecs=5`):
  `"userdata" ... read-write`, `SWEET LOG: console log to the last 4100 KiB of the partition (block 0xe8f3fa)`,
  HFS mounted rw, init PID 1. Pulled from TWRP with the unchanged `tools/sweet-log-pull.sh` (userdata =
  /dev/block/mmcblk0p72, 53648801280 bytes): SWEETLOG v1, flush #8, 6555 bytes -> logs/boot-0082-rw.txt.
  HFS volume afterwards: 3 changed 4K blocks (header + 2 catalog), lastMountedVersion '10.0', unmounted bit
  clear (hard reset), fsck.hfsplus -n OK. Modify date = 1970 (no RTC; ios-flame 0099 has a PMIC RTC driver).
- Log workflow from now on: boot -> Power+VolDown -> `fastboot boot` TWRP -> `tools/sweet-log-pull.sh OUT.txt`.
- hfsprogs: built from AUR in /tmp/hfsprogs, installed by the user with sudo pacman -U.
- Patch 0083 = ios-flame's iig fix. **macos-15 build works**: run 41255314009 (commit 953e524, macOS 15.8.2,
  Xcode 16.0) success in ~7 min. Its kernel is not hardware-tested yet; then make macos-15 the default runner.
- Transport inside sweet_ufs.c selected by ADT /sweet-emmc (mkadt --bonito); same sequence as the shim.
- First test (read-only, safe): `ROOT=emmc tools/bonito-kernel-image.sh xnu/out/<new kernel> wide` ->
  expect `SWEET EMMC: ... high speed 48 MHz, 8-bit bus` and `SWEET EMMC: "userdata" ... read-only`,
  then a root mount failure (userdata holds no HFS yet).
- Build: needs macos-14 window (Nov 1 00:00-14:00 UTC).

## Milestone 1 probe shim
`shim/` now builds a probe (`shim/out/shim-boot.img`, ~1.4 MB):
- Page 1 (90 s): model, /memory banks as filled by ABL, every reserved-memory node.
- Page 2 (180 s, then PSCI reset): CPU ID regs, PSCI features, SMP CPU_ON x7 (MMU off),
  GIC group-1 vtimer + SGI, 16K MMU enable, then READ-ONLY sdhc_1/cqhci register dump (last,
  because a clock-gated block could stall the bus - if the screen freezes at "EMMC reading", that's why).
- Host check: `out/host_fdt DTB` renders page 1 from a DTB file (verified against the stock DTB).

Build: `CROSS=<toolchain>/bin/aarch64-none-elf- shim/build.sh` (Arm GNU 13.3 bare-metal works; an
`aarch64-linux-gnu-gcc` cross gcc too).

Run (after the user unlocks the bootloader - `fastboot flashing unlock` WIPES Android userdata):
`fastboot boot shim/out/shim-boot.img` - nothing is flashed. Photograph both pages.

## Open questions the probe answers
1. Memory banks (expect ~2 G low + ~2 G at 0x100000000 on a 4 GB part?) -> how much contiguous RAM XNU gets.
2. Does the splash framebuffer update from CPU writes (video mode)?
3. Did ABL leave sdhc_1 clocked/powered (pres/clk/hctl) and CQE on/off?
4. 16K granule, no PAC, PSCI/SMP/GIC behave like sweet?

## Kernel CI (fork watrabi/ios-sweet, branch bonito)
- Fork created 2026-10-31, Actions enabled, `bonito` pushed (46ba1b0). Builds: `gh workflow run xnu.yml
  -R watrabi/ios-sweet --ref bonito` (~7 min upstream), artifact `xnu-kernel`.
- **macos-14 runner is being retired**: brownouts Oct 31 14:00-Nov 1 00:00 UTC and Nov 1 14:00-Nov 2
  00:00 UTC, gone Nov 2 2026 (actions/runner-images#13518). First run 41250103082 was cancelled by the
  brownout. Window that works: Nov 1 00:00-14:00 UTC.
- After that: macos-15 has only Xcode 16.x/26.x, whose iig emits IORPC.kernelContent (xnu-8796 lacks it,
  see NOTES.md). TODO: make the tree accept Xcode 16 iig, or obtain an Xcode 15 toolchain otherwise.

## Backups and storage
- 2026-10-31: per-device partitions pulled from a temporarily booted official TWRP 3.7.1_12-1-bonito
  (`tools/bonito-backup.sh`, read-only): persist (40 MiB ext4), modemst1/2, fsg, fsc (data), modemcal (all
  zero), devinfo. In backup/ (gitignored) + user's drive, SHA256SUMS verified on both.
- Big files (iOS root image) can go on the user's FUSE drive "~/watr.lol Drive" (25 GB free); local / has
  ~0.6 GB. Untested: root/loop access through that FUSE mount.

## FIRST iOS BOOT (2026-11-01 ~01:25 UTC) - reaches SpringBoard, gated on data migration
- Image: tools/mkiosroot.sh on the SD card (/mnt/iosbuild, ext4, 14G, 12G used) from 20H392 OS/SystemOS/
  AppOS, then sweet-root-tweaks.py (warning: no mobileactivationd system container -> data ark skipped),
  fsck OK apart from the known journaled-volume header quirk. Flashed with tools/simg-chunks.py in 58
  pieces (~16 min, SD-card bound). ADT now has the real iPhone X /product (research/ipsw-x/d22ap.adt via
  tools/adt.py unwrap + lzfse CLI in ~/.local/bin).
- Boot shim-boot-0082-ios-cpus6.img (0082 kernel, wide RAM, cpus=6, upstream iOS boot-args + ufsrw/rootrw,
  sweetfb_noconsole=1 sweetfb_hz=30). Apple logo on screen. ~11 min, then pulled from TWRP:
  logs/boot-ios1.txt (164 KB, 69 flushes). 6/6 CPUs up, eMMC 48 MHz 8-bit rw, `hfs: mounted iOSRoot`,
  Apple launchd + boot tasks, keybagd (AKS stub), backboardd opens AppleCLCD (1080x2160 presented as
  1125x2436, `display on`), `exec com.apple.datamigrator`, SpringBoard running.
- SpringBoard main thread blocked at one pc in every sample = upstream NOTES (boot 0055n): SpringBoard
  waits in -[BKSSystemShellService _checkIn]; backboardd holds the check-in until the first-boot data
  migration ends (~30 min on sweet with cpus=1). Only 2 crashing daemons (driverkitd sig 9, spindump
  SIGBUS); bluetoothd and fairplayd.H2 relaunch in a loop (expected, no hardware).
- **LOCK SCREEN (2026-11-01, boot started 21:27 EST = 01:27 UTC, same image + shim-boot-0082-ios-cpus6.img):**
  iOS 16.7.16 lock screen on bonito - date/clock, camera button, black background (no wallpaper). Shows
  "Wednesday, December 31, 4:21" = Unix epoch in US Pacific + uptime: no RTC (port ios-flame 0099, PM660
  RTC). Clock digits look white with a red fringe in the photo (likely OLED/camera; check channel order
  with a known-blue UI element later). No touch/keys yet (drivers disabled by boot-args).
- Next: touch (Synaptics RMI4 @ i2c9 0x20, 1.8 V via PM660 GPIO 12), keys (PON + PM660L GPIO 7), RTC,
  USB networking (ios-flame 0088-0091), cpufreq (OSM, ios-flame 0086), wallpaper investigation.

## TOUCH WORKS (2026-11-01 ~02:10 UTC) - patch 0084, CI run 41263318374 (kernel xnu/out-0084)
- Shim probe first (shim-touch.img, -DTOUCHPROBE): PM660 GPIO12 apid 0x4c (apps-owned, LV subtype 0x10,
  was enabled input) -> mode 1 / DIG_OUT_SOURCE 0x80 / EN 0x80 = 1.8 V on; GENI I2C fw 0x300 left by ABL;
  RMI4 @0x20: F34 (q 0x24), F01 (q 0x2c, cmd 0x23, ctrl 0x0d, data 0x06), F12 (q 0x4b, ctrl 0x13,
  data 0x08), product S3706B, F01 status 0x81; F12 data = 8-byte objects in panel pixels.
- iOS boot shim-boot-0084-ios-touch.img (TOUCH=1 CPUS=6): `SWEET TOUCH: selected Synaptics S3706B (RMI4,
  status 0x81), 1080x2160, poll=10ms`; user unlocked with an edge swipe, touches land correctly, opened
  Settings, swiped home. Log logs/boot-0084-touch.txt.
- Re-opening Settings (resume of the suspended app, no new exec) showed a black screen: display on, no
  crash in SpringBoard/backboardd, touches still delivered. Upstream NOTES records the same class of
  issue on sweet ("Phone/Photos/Messages show black, no crash") - userland CPU rendering, not the port.
- PosterBoard crashes (sig 5) -> no lock-screen wallpaper (ios-flame HANDOFF: default wallpaper is HEIC,
  CPU renderer can't draw it). mediaserverd sig 6 once; vmd burns ~60% CPU.
- **KEYS WORK** (patch 0085, CI run 41264425685, kernel xnu/out-0085, image shim-boot-0085-ios.img with
  TOUCH=1 KEYS=1 CPUS=6): user confirmed power and volume up/down on iOS.
- Next: RTC; USB networking; cpufreq; black-app investigation with sweetsample=; GPU (see below).

## GPU bring-up (Adreno 615), shim probes - port of ios-flame's shim/gpuprobe.c
- Firmware pulled from the phone's vendor_b (TWRP, read-only mount): a615_zap.{mdt,b00,b01,b02,elf},
  a630_gmu.bin (32768 B), a630_sqe.fw -> research/gpu/fw (gitignored).
- **Stage 1 OK** (shim-gpu1.img): CX GDSC on (0xf8222000), gmu_clk_src = XO, cxo/cx_gmu on, GMU ITCM/DTCM
  writable, Adreno SMMU (0x5040000) ID regs readable: cr0 0x200626 idr0 0x48016605 (5 SMRs) idr1
  0x20008005 (5 CBs). gpu_memnoc_gfx / gpu_snoc_dvm_gfx branches stay halted (GX side off).
- **Stage 2 OK** (shim-gpu2.img, commit dfd5908): SMR0/CB0 for GMU stream 5 (all 5 SMRs were free,
  s2cr 0x200ff), RSCC at 0x508d000 + PDC 0xb280000/PDC_SEQ 0xb480000 programmed (A615 PDC offset
  0x30080), PDC enable 0x80000001; legacy load of a630_gmu.bin into ITCM; **GMU BOOTED, init 0xbabeface**,
  no SMMU fault (FSR 0x400 = format bits only). **RSC wake ack (GMU_RSCC_CONTROL_ACK bit 1) TIMED OUT**
  (status0 0x01000000, seq idle) - mainline treats that as an error; likely relevant for GX power (stage 3).
- RSC ack: mainline only sends the RSC wake request when it put the PDC to sleep earlier
  (a6xx_rpmh_start tests GMU_STATUS_PDC_SLEEP), so on a cold boot the timeout is probably benign.
- **Stage 3 run 1** (shim-gpu3.img): cmd-db found at 0x85fe0000 (magic 0x0c0330db); votes gx_max
  0x00c00305 (SVS_L1), cx 0x00300302; OOB BOOT_SLUMBER acked; SPTPRAC on (0x28); **GX GDSC ON
  (0xf8222000)**, gpu_memnoc_gfx/snoc_dvm_gfx branches now running; RBBM_STATUS 1 and **CP_SCRATCH0
  write/read OK** - GPU core registers answer. But all 5 HFI v1 messages got no reply (ctrl 0) and
  GMU2HOST_INTR_INFO showed bit 23 = CM3_FAULT. Cause: skipped a6xx_gmu_hfi_start (HFI_CTRL_INIT=1,
  poll HFI_CTRL_STATUS bit 0), which mainline runs after sptprac and before any message.
  shim-gpu3b.img adds it plus a CM3_FAULT readout after each step.
- Stage 3 iterations 2026-11-01 (all committed, images shim-gpu3b..3i):
  * gpu3b/3c: HFI_CTRL_INIT -> **SoC reset**; RAM breadcrumbs (0x17e000000) + slow steps localised it.
  * gpu3d: SMR write 0x84000005 (sid 5 + mask 0x400, ios-flame's) reads back 0 - dropped; 0x80000005
    sticks. IDR0 NUMSIDB = 3 (3-bit stream ids) -> no mask. Unmatched-stream global fault was the reset.
  * gpu3e: no reset; HFI_CTRL_STATUS 0xfe -> legacy queue ids are 0/4/5 (not 0/1/2).
  * gpu3f: **HFI ready (status 1), INIT acked rc 0**, then GMU2HOST bit 23 CM3_FAULT, later msgs time out.
  * gpu3g/3h: mapped icache 0x4000 / dcache 0x44000 windows: no change. No SMMU fault (FSR 0x400 = format
    only, FAR 0, GFSR 0), no F2H error msg, log/debug/dcache untouched, init result still 0xbabeface,
    cx_busy 0x30000000 -> 0x71000000, RSC status0 0x01000000, PDC 0x80000001.
  * gpu3i: GMU clock 200 MHz (GPLL0_OUT_MAIN_DIV/1.5, cfg 0x602, ok) and no cold-boot RSC wake (as
    mainline): fault unchanged.
  * Remaining difference from mainline: no interconnect (GFX3D->EBI1) bandwidth vote. GX stays powered
    after the fault. Plan: skip HFI and try stage 4-5 (zap via SCM PAS id 13 into gpu_region 0x97a15000,
    SQE, ring, CP_ME_INIT, CP_MEM_WRITE 0xcafef00d) like ios-flame's gpu_run; return to the fault later.
- **STAGE 4-5: THE GPU EXECUTES COMMANDS** (shim-gpu45.img, commit 5bbbde3, 2026-11-01 ~03:10 UTC):
  HFI messages skipped (GX already on), OOB GPU_SET, GPU stream SMR1 = 0x80000000 (sid 0, no mask) -> CB1,
  A615 gen1 hw_init (VBIF QoS, ROQ 0x010000c0/0x8040362c, pool 128, PC_DBG_ECO 0x18000, GMEM 1M+512K),
  a630_sqe.fw (0x7d84 B) at CP_SQE_INSTR_BASE 0x830, 32K ring, CP_ME_INIT idle (rptr = wptr = 9);
  zap via SCM PAS 13: supported / INIT_IMAGE / MEM_SETUP(0x97a15000, 0x2000) / AUTH_AND_RESET all rc 0;
  CP_SET_SECURE_MODE 0; **CP_MEM_WRITE -> target = 0xcafef00d**; no SMMU fault (FSR 0x400, FAR 0).
- **FIRST GPU PIXELS ON THE PANEL** (shim-gpu6.img, commit f5d47eb, 2026-11-01 ~03:12 UTC): framebuffer
  0x9c000000 mapped into the GPU as 2 MB blocks at GPU VA 0x20000000; one gradient row by CP_MEM_WRITE,
  599 more by CP_MEMCPY at y 1250: PAINTED rows 600, FSR 0x400, user photo shows the blue->red striped band.
- GPU next: port the bring-up into XNU as a kernel service (ios-flame 0095 sweet_a6xx + 0096/0097 Metal
  accelerator/user client + fixes 0105-0122, with this file's A615 values: legacy GMU, no HFI messages
  for now, SID 0/5 without SMR masks, gen1 hw_init, a615 zap in gpu_region 0x97a15000); then build Turnip
  + the metal2vulkan bridge for iOS on a macOS CI runner and install them into the root image.
- Earlier plan for stage 3: then gfx_rail_on + sptprac_enable, legacy HFI
  (v1) start, OOB GPU set -> GX GDSC on; then zap shader (TZ PAS, a615_zap), SQE, ring, first commands.

### Kernel GPU driver (patch 0086)

Port of ios-flame's 0095 to the A615, using the sequence proven by shim/gpu.c:
- legacy GMU: the whole a630_gmu.bin is loaded into ITCM, queue ids are 0/4/5, no HFI messages are sent;
- GX power comes from the BOOT_SLUMBER OOB, then SPTPRAC, then the GPU_SET OOB;
- SMRs have no mask; PDC 0xb280000 with offset 0x30080; gen1 VBIF hw_init;
- zap (a615) at 0x97a15000; the SMC is inline (bonito has no /dev/pmem);
- also includes flame's `-mcpu=cortex-a55+crypto` fix (Apple clang otherwise emits EOR3).

Present: the GPU's 2D engine scales the full 1125x2436 frame into a 998x2160 box at x 41. The panel stride becomes 4352: every DPU pipe whose SRC0 points into the framebuffer gets the new stride, then CTL0 is flushed.

Opt-in: `GPU=1 tools/bonito-kernel-image.sh` adds the firmware to /chosen (`mkadt.py --gpufw`) and passes the boot-arg `sweetgpu=1`. Log lines start with "SWEET GPU:" and "SWEET FB:".
Image: shim/out/shim-boot-0086-gpu.img (emmc root, touch, keys, 6 CPUs, GPU).

**GPU PRESENT WORKS (2026-11-01, 0086 kernel, shim-boot-0086-gpu.img):** the kernel brings up the A615 and
iOS frames are scaled onto the panel by the GPU's 2D engine (full status bar + dock visible).
- zap PAS: INIT_IMAGE returned QCOM_SCM_INTERRUPTED (1) with interrupts on. Resuming means re-issuing
  x0 = 1, the same args, and x6 = the x6 that TZ returned (Linux __scm_smc_do_quirk, ARM_SMCCC_QUIRK_QCOM_A6).
  Using the returned x1 (0x82) as x0 HUNG THE WHOLE SoC. The fixed call needed 5 resumes; MEM_SETUP and AUTH_AND_RESET needed none.
- ABL scans the splash out through DPU VIG0 (src 0x9c000000, stride 4320); the stride switches to 4352.
- Bring-up ~1.2 s after the GMU boots. The GMU clock stays on XO (the 200 MHz switch doesn't take), and there is no perf/bw vote yet.
- Debug aid: GPUDBG=<s> (boot-args sweetgpudbg=<s> ufslogsecs=1) delays the bring-up and logs plus paces every SMC.
- Pitfall: iOS images need the iOS boot-args (now the default in bonito-kernel-image.sh). Without
  unentitled_ios_sim_launch=1, launchd's exec fails with WRONG_PLATFORM and the kernel panics with CS_KILLED.
- Pitfall: the bigger payload (firmware in the ADT) is placed by ABL inside the kernel's RAM range. shim/loader.c now
  checks only the range it writes, and moves the payload otherwise.
- Boot time: /sbin/mount (mount-phase-2) spends ~98 s looking for the APFS data volume our HFS+ root lacks.

### Metal / wallpaper plan (2026-11-01)
Wallpaper (ios-flame HANDOFF.md, "Black wallpaper fixed"), not dependent on Metal:
1. MAC addresses in the ADT /chosen. Done in mkadt.py.
2. A SweetAMFI stand-in that answers trust queries (flame 0103).
3. Rewrite the default wallpaper's HEIC layers as PNG (there's no HEVC decoder), then run `lsreg -r`.

Metal, in order:
1. A file path onto the running phone: USB ECM plus flame's 0091 file server. sweet_usb.c needs the sdm845 GCC CBCR offsets:
   master 0xf00c, sleep 0xf010, utmi 0xf014, cfg-noc 0x502c, aggre 0x8201c, ahb2phy 0x6a004, clkref 0x8c008.
2. Kernel: flame 0096, 0097, 0115 and 0116, plus the GPU fixes 0104–0109, 0113 and 0117–0119.
3. macOS CI: Turnip for iOS (A615 is gen1), the metal2vulkan bridge, and SweetMetal2.bundle.
4. Install the bundle and boot with sweetmtl=2. Speed also needs a GPU frequency vote and a DDR bandwidth vote (the GMU HFI fault blocks these).

### USB, file server, AMFI, Metal (2026-11-01)
- **0087** (tested): USB Ethernet works (phone 192.168.7.2; `tools/bonito-usbnet.sh` sets the host side and keeps NetworkManager off the link).
  - The QUSB2 PHY must be re-initialised (Linux qusb2_phy_init v2 table + GCC_QUSB2PHY_PRIM_BCR 0x12000). ABL powers it down on hand-over, and the first ep0 command then timed out.
  - Other bonito specifics: SMMU stream 0x740, already matched by ABL's SMR4 (used as is); sdm845 GCC CBCR offsets.
  - File server on tcp 2223 (`tools/bonito-files.py` get/put/mkdir/rm/chmod/link, plus list since rev 2); paths are percent-encoded (spaces).
  - Command server on udp 5141 (`tools/bonito-cmd.py sync|reboot|<poke lines>`), and a live log on udp 5140.
  - SweetAMFI answers trust-cache queries (verified in the log). The ADT carries the wifi/bt/ethernet MACs.
- Wallpaper: 4 of the 5 WWDC 2022 HEIC layers have been replaced with PNG on the phone. "Head Mask" (space in the name) needs 0087 rev 2: run `tools/bonito-wallpaper.sh`.
- **0088** (built, untested): ios-flame 0096–0118 (SweetAccelerator v1/v2, the NativeAdreno ABI for Turnip's darwin backend, recovery, async submits, ...).
  - A615 values: chip id 0x06010500, GMEM 512 KB, hbb 14.
  - v2 BOs come from a pool carved off the top of RAM. `mkadt.py --gpupool 512` puts it in /chosen sweet-gpu-pool; it is 0x15e2c0000 + 512 MB with --bonito-wide.
  - Skipped: 0107, 0110–0112, 0114 (dyld policy, needed for third-party apps), 0119, 0120, 0122.
- **Userland Metal** (.github/workflows/metal.yml, macos-15; built OK): Mesa 85fda3b3 + UniXNU e19e517 patches/mesa + flame 0100, metal2vulkan 720ef1ce + flame bridge patches, and SweetMetal2.bundle (TU_DEBUG=noubwc forced: UBWC is unconfigured on our A615).
  - Install: `tools/bonito-metal-install.sh` (phone up with USB=2). Then boot `METAL=1` (sweetmtl=2).
  - Images: /mnt/iosbuild/boot/shim-boot-0088-{metal,nometal}.img.
- Root disk is full (≤80 MB). Kernel artifacts are downloaded into the scratchpad (RAM), boot images go to /mnt/iosbuild/boot.
- CI: macOS 14 brownouts have started. The xnu workflow default is now macos-15.
- **Metal on the phone (2026-11-01):** backboardd gets "Turnip Adreno (TM) 615" and its frames render (lock screen OK, touch OK).
  - Fix 1: shader cache dirs /var/local/m2v-cache[/proc] must be mode 777 (backboardd isn't root).
  - Fix 2: the **GPU core clock was XO 19.2 MHz** (gx_gfx3d CFG 0). The GMU never sets it without the HFI perf table, and Turnip's frames tripped the 500 ms hang check.
    - Kernel sequence (proven first with pokes): Fabia PLL0 at GPUCC+0x0, MODE bits OUTCTRL0 / RESET_N2 / UPDATE_BYPASS23 (not 21 = VOTE_FSM_RESET), CAL_L and L 0x2c, FRAC 0xcaaa (860 MHz), OPMODE run, lock (bit31) is immediate.
    - Then gfx3d RCG +0x101c: cfg 0x101 = source 1 (crc_div = pll0/2), div 1 → 430 MHz; 0x103 → 215 MHz. Boot-arg sweetgpumhz.
  - Hang timeout is now 5 s (sweetgpuhang).
- Still slow. Next levers: DDR/interconnect bandwidth vote (flame 0122, BCM via RPMh), cpufreq + big cores, apps on Metal (mtl2-clients).
- USB bug: after the cable re-enumerates, the phone's network RX stays dead until reboot.

### Wi-Fi groundwork (2026-11-01/02)
- **Modem boot.** SDM670's modem is `qcom,pil-q6v55-mss` at 0x4080000. Linux boots it with q6v5_mss and the MBA (mba.mbn) through the RMB registers at 0x4180000, with hyp_assign of the MBA and MPSS memory. This is NOT flame's TrustZone PAS path. Other mss registers: halt_q6 0x1f63000, halt_modem 0x1f65000, halt_nc 0x1f64000, restart 0xc2b0000, pdc_sync 0xb2e0100, alt_reset 0x4180044.
- **Reserved memory:** modem 0x8b000000 (0x7e00000), MBA 0x93c00000 (2 MB), WLAN MSA 0x93300000 (1 MB). rmtfs is a dynamic 2 MB buffer, client-id 1.
- **Wi-Fi block:** icnss membase 0x18800000. Apps SMMU stream 0x40 / mask 0x1 (flame: 0x640). Rails: ldoa5 800 mV, ldoa9 1800 mV, ldoa6 1248 mV, ldoa19 3000 mV.
- **IPC:**
  - SMEM 0x86000000 (2 MB); IPC register 0x1799000c.
  - Modem SMP2P: irq-bitmask 0x4000, GIC SPI 451.
  - GLINK (label "mpss"): irq-mask 0x1000, SPI 449.
  - AOP QMP: 0xc300000, mask 0x1.
- Patch 0090 adds /dev/pmem (flame 0094 plus its SMC and RPMh ioctls) for userland bring-up.
- **@2x and iPhone-XR layouts don't work** with the iPhone X IPSW: PosterBoard aborts, and the XR layout shows only a flickering clock. Stay at @3x.

### New host: ChromeOS Crostini (Debian 13 VM), 2026-11-03
- Earlier artifacts (/mnt/iosbuild, xnu/out-*, research/, dtb/, work/ssh) stayed on the old Arch PC. Rebuilt here:
  kernel 0090 from CI run 41345791602 (d371bcd) in xnu/out-0090; research/ipsw-x/d22ap.adt via remotezip from the
  20H392 IPSW; GPU firmware (research/gpu/fw) and dtb/bonito-stock.dtb from the SP2A.220505.008 factory zip
  (image zip -> vendor.img -> simg2img -> debugfs dump /firmware/a6*; boot.img -> unpack_bootv2.py). Same sizes as before.
- Big files on the shared SD card: /mnt/chromeos/shared/removable/Scratchpad (factory/, twrp/, boot/).
- **GitHub Actions: free minutes exhausted** (dispatch -> HTTP 422 "Actions has been disabled"). The wland push never
  built (no phone-tools run after 2026-11-01 16:33). Builds move to a self-hosted macOS 15 + Xcode 16.0 VM.
- **0090 boots iOS to the lock screen** (shim-boot-0090-metal.img: emmc root, touch, keys, 6 CPUs, GPU, Metal, USB=2);
  /dev/pmem present.
- Crostini's kernel has no usbnet/cdc_ether, and ChromeOS's own cdc_ether claims a CDC ECM device so it can't be
  shared with Linux. Fix: `tools/usb-vendor-class.py KIN KOUT` patches 5 descriptor bytes in a built kernel to class
  0xff (wire protocol unchanged), and `sudo tools/ecm-tap.py` is a pyusb CDC-ECM host bridging to TAP sweet0
  (192.168.7.1). Boot shim-boot-0090v-metal.img -> share 1209:0001 with Linux -> ping 192.168.7.2 OK (5-10 ms),
  bonito-files.py list OK.
- TWRP 3.7.1_12-1 (sha256 e0781b59...) hung at the Google logo once on this host (not needed so far).
- SSH: dropbear port 22 refused on this boot; the old work/ssh key is on the old PC.
