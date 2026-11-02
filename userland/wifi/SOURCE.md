From ios-flame (https://github.com/NWylynko/ios-flame, MIT) phone/wifi: wland, the WLAN bring-up from userland over /dev/pmem.
bonito constants: APCS IPC 0x1799000c, modem region 0x8b000000/0x7e00000, WLAN MSA 0x93300000, SMMU stream 0x40,
rails ldoa5/9/6/19. The SDM670 modem may need the MBA boot (wland pas-probe tells).

Build: .github/workflows/phone-tools.yml (artifact phone-tools, file wland).
