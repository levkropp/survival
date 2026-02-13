# Bare-Metal S905X Roadmap

This document tracks research for eventually replacing UEFI with direct hardware control on the Amlogic S905X SoC. This is a long-term project — the UEFI path works now and is what we ship.

## Why go bare-metal?

- Remove dependency on U-Boot firmware
- Full control over boot time (sub-second cold boot possible)
- Deeper understanding of the hardware
- Educational value — reverse engineering is fun

## Boot Chain (current)

```
BL1 (ROM) → BL2 (DDR init) → BL3/U-Boot → UEFI app (us)
```

## Boot Chain (bare-metal goal)

```
BL1 (ROM) → BL2 (DDR init, keep this) → Our code
```

We keep Amlogic's BL1+BL2 because DDR initialization is proprietary and extremely complex. We replace U-Boot entirely.

## Peripherals to reverse-engineer

### 1. UART (easiest — start here)
- **Controller**: Standard 8250/16550-compatible
- **Base address**: 0xC81004C0 (UART_AO_A)
- **Linux driver**: `drivers/tty/serial/meson_uart.c`
- **DTS**: `arch/arm64/boot/dts/amlogic/meson-gxl.dtsi`
- **Status**: Well-documented, first target

### 2. Framebuffer / HDMI
- **Display pipeline**: VPU → OSD → Video Encoder → HDMI TX
- **HDMI TX**: Synopsys DesignWare HDMI + Amlogic PHY
- **Base addresses**:
  - VPU: 0xD0100000
  - HDMI TX: 0xC883A000
- **Linux drivers**:
  - `drivers/gpu/drm/meson/meson_drv.c` — main driver
  - `drivers/gpu/drm/meson/meson_dw_hdmi.c` — HDMI TX
  - `drivers/gpu/drm/meson/meson_venc.c` — video encoder
  - `drivers/gpu/drm/meson/meson_plane.c` — OSD planes
- **Approach**: Configure one OSD plane as a simple linear framebuffer
- **Difficulty**: HIGH — many interdependent registers

### 3. USB (for keyboard)
- **Controller**: Synopsys DesignWare USB2 OTG (DWC2)
- **Base address**: 0xC9000000
- **Linux driver**: `drivers/usb/dwc2/`
- **Need**: USB HID class driver on top of DWC2
- **Difficulty**: HIGH — need full USB stack (enumeration, HID parsing)

### 4. SD/MMC
- **Controller**: Amlogic custom
- **Base address**: 0xD0072000 (SD_EMMC_B for SD card)
- **Linux driver**: `drivers/mmc/host/meson-gx-mmc.c`
- **Need**: Read/write sectors, FAT32 on top
- **Difficulty**: MEDIUM — well-documented in Linux driver

### 5. GIC (Generic Interrupt Controller)
- **Type**: ARM GICv2
- **Base addresses**:
  - Distributor: 0xC4301000
  - CPU interface: 0xC4302000
- **Documentation**: ARM GICv2 Architecture Specification (public)
- **Difficulty**: LOW — standard ARM peripheral

## Key Linux kernel files to study

```
arch/arm64/boot/dts/amlogic/meson-gxl-s905x.dtsi
arch/arm64/boot/dts/amlogic/meson-gxl.dtsi
arch/arm64/boot/dts/amlogic/meson-gx.dtsi
drivers/gpu/drm/meson/
drivers/usb/dwc2/
drivers/mmc/host/meson-gx-mmc.c
drivers/tty/serial/meson_uart.c
drivers/clk/meson/gxbb.c          (clock tree)
drivers/pinctrl/meson/            (pin muxing)
```

## Phase plan

1. Get UART working (serial output from bare-metal code)
2. Set up GIC for interrupt handling
3. Framebuffer — configure VPU + OSD for simple 32-bit linear FB
4. HDMI — get video output (can start with lower resolution)
5. SD/MMC — read/write sectors
6. USB HID — keyboard input (this is the hardest part)

## Register dump strategy

While running U-Boot, dump key register ranges:
```
md 0xC81004C0 0x10   # UART
md 0xD0100000 0x100  # VPU
md 0xC883A000 0x100  # HDMI TX
md 0xC9000000 0x100  # USB
md 0xD0072000 0x100  # SD/MMC
```

This shows us what U-Boot configured, so we can replicate it.
