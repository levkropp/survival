#!/bin/bash
set -e

# ISO write test (x86_64): boots workstation with two USB drives
#   USB1 — has a Linux .iso file on FAT32
#   USB2 — empty target for ISO write
#
# Usage: ./run-qemu-iso-test-x86_64.sh [path-to-iso]
#   Default: uses Alpine x86_64 (downloads if needed)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build/x86_64"
ESP_DIR="$BUILD_DIR/esp"

# Find UEFI firmware
FIRMWARE=""
for fw in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/edk2/x64/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/qemu/OVMF.fd; do
    if [ -f "$fw" ]; then
        FIRMWARE="$fw"
        break
    fi
done

if [ -z "$FIRMWARE" ]; then
    echo "ERROR: OVMF firmware not found. Install ovmf package."
    exit 1
fi

if [ ! -f "$ESP_DIR/EFI/BOOT/BOOTX64.EFI" ]; then
    echo "ERROR: Build first with 'make ARCH=x86_64'"
    exit 1
fi

# Find matching VARS template
VARS_TEMPLATE=""
FW_DIR="$(dirname "$FIRMWARE")"
for vf in \
    "${FW_DIR}/OVMF_VARS_4M.fd" \
    "${FW_DIR}/OVMF_VARS.fd"; do
    if [ -f "$vf" ]; then
        VARS_TEMPLATE="$vf"
        break
    fi
done

VARS="/tmp/survival_iso_x64_vars.fd"
if [ -n "$VARS_TEMPLATE" ]; then
    cp "$VARS_TEMPLATE" "$VARS"
else
    FW_SIZE=$(stat -c%s "$FIRMWARE")
    dd if=/dev/zero of="$VARS" bs=1 count="$FW_SIZE" 2>/dev/null
fi

# Find or download ISO
if [ -n "$1" ] && [ -f "$1" ]; then
    ISO_SRC="$1"
elif [ -f "/tmp/alpine-virt-x86_64.iso" ]; then
    ISO_SRC="/tmp/alpine-virt-x86_64.iso"
else
    echo "Downloading Alpine Linux x86_64 ISO..."
    ISO_SRC="/tmp/alpine-virt-x86_64.iso"
    wget -q --show-progress -O "$ISO_SRC" \
        "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/alpine-virt-3.23.3-x86_64.iso"
fi

ISO_SIZE=$(stat -c%s "$ISO_SRC")
ISO_NAME=$(basename "$ISO_SRC")
ISO_SIZE_MB=$(( ISO_SIZE / 1024 / 1024 ))
echo "ISO: $ISO_SRC ($ISO_SIZE_MB MB)"

if [ "$ISO_SIZE" -lt 1048576 ]; then
    echo "ERROR: ISO file is too small (${ISO_SIZE} bytes). Delete $ISO_SRC and re-run."
    exit 1
fi

# Compute USB image sizes
USB1_SIZE_MB=$(( ISO_SIZE_MB + 256 ))
USB2_SIZE_MB=$(( ISO_SIZE_MB + 128 ))
USB1_SIZE_MB=$(( ((USB1_SIZE_MB + 63) / 64) * 64 ))
USB2_SIZE_MB=$(( ((USB2_SIZE_MB + 63) / 64) * 64 ))

echo "USB1: ${USB1_SIZE_MB}MB (source, holds ISO file)"
echo "USB2: ${USB2_SIZE_MB}MB (target, receives raw ISO)"

# Create boot disk image
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

# ---- USB1: source with ISO file ----
USB1="/tmp/survival_usb_iso_x64.img"
echo "Creating USB1 (${USB1_SIZE_MB}MB) with ISO file..."
dd if=/dev/zero of="$USB1" bs=1M count="$USB1_SIZE_MB" 2>/dev/null
mkfs.vfat -F 32 -n ISOFILES "$USB1" >/dev/null 2>&1
mcopy -i "$USB1" "$ISO_SRC" "::/$ISO_NAME"
echo "  Copied $ISO_NAME to USB1"

# ---- USB2: target (empty except README) ----
USB2="/tmp/survival_usb_target_x64.img"
echo "Creating USB2 (${USB2_SIZE_MB}MB) target drive..."
dd if=/dev/zero of="$USB2" bs=1M count="$USB2_SIZE_MB" 2>/dev/null
mkfs.vfat -F 32 -n TARGET "$USB2" >/dev/null 2>&1
echo "This drive is the ISO write target." | mcopy -i "$USB2" - ::/README.TXT

echo ""
echo "=== ISO Write Test - QEMU x86_64 ==="
echo "Boot disk: $IMG (64MB, workstation)"
echo "USB1: $USB1 (${USB1_SIZE_MB}MB, has $ISO_NAME)"
echo "USB2: $USB2 (${USB2_SIZE_MB}MB, write target)"
echo ""
echo "Test steps:"
echo "  1. Browse to [USB] ISOFILES volume"
echo "  2. Select $ISO_NAME"
echo "  3. Press F10 → WriteISO"
echo "  4. Select the target USB device, press ENTER"
echo "  5. Press Y to confirm"
echo "  6. Wait for write to complete"
echo "  7. Reboot to boot from the written USB"
echo "======================================="
echo ""

qemu-system-x86_64 \
    -m 2G -vga none \
    -drive if=pflash,format=raw,file="$FIRMWARE",readonly=on \
    -drive if=pflash,format=raw,file="$VARS" \
    -hda "$IMG" \
    -device ramfb \
    -device qemu-xhci -device usb-kbd -device usb-mouse \
    -drive if=none,id=usb1,format=raw,file="$USB1" \
    -device usb-storage,drive=usb1,removable=on \
    -drive if=none,id=usb2,format=raw,file="$USB2" \
    -device usb-storage,drive=usb2,removable=on \
    -serial stdio
