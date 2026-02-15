#!/bin/bash
set -e

# ISO write test: boots workstation with two USB drives
#   USB1 — has a Linux .iso file on FAT32
#   USB2 — empty target for ISO write
#
# Usage: ./run-qemu-iso-test.sh [path-to-iso]
#   Default: uses Ubuntu Desktop ARM64 if available, else Alpine

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build/aarch64"
ESP_DIR="$BUILD_DIR/esp"

# Find UEFI firmware
FIRMWARE=""
for fw in \
    /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
    /usr/share/edk2/aarch64/QEMU_EFI.fd \
    /usr/share/AAVMF/AAVMF_CODE.fd \
    /usr/share/edk2-aarch64/QEMU_EFI.fd; do
    if [ -f "$fw" ]; then
        FIRMWARE="$fw"
        break
    fi
done

if [ -z "$FIRMWARE" ]; then
    echo "ERROR: UEFI firmware not found. Install qemu-efi-aarch64."
    exit 1
fi

if [ ! -f "$ESP_DIR/EFI/BOOT/BOOTAA64.EFI" ]; then
    echo "ERROR: Build first with 'make'"
    exit 1
fi

# Find or download ISO
if [ -n "$1" ] && [ -f "$1" ]; then
    ISO_SRC="$1"
elif [ -f "/tmp/ubuntu-24.04.4-desktop-arm64.iso" ]; then
    ISO_SRC="/tmp/ubuntu-24.04.4-desktop-arm64.iso"
elif [ -f "/tmp/alpine-virt-aarch64.iso" ]; then
    ISO_SRC="/tmp/alpine-virt-aarch64.iso"
else
    echo "Downloading Alpine Linux aarch64 ISO..."
    ISO_SRC="/tmp/alpine-virt-aarch64.iso"
    wget -q -O "$ISO_SRC" \
        "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/aarch64/alpine-virt-3.23.3-aarch64.iso"
fi

ISO_SIZE=$(stat -c%s "$ISO_SRC")
ISO_NAME=$(basename "$ISO_SRC")
ISO_SIZE_MB=$(( ISO_SIZE / 1024 / 1024 ))
echo "ISO: $ISO_SRC ($ISO_SIZE_MB MB)"

if [ "$ISO_SIZE" -lt 1048576 ]; then
    echo "ERROR: ISO file is too small (${ISO_SIZE} bytes). Delete $ISO_SRC and re-run."
    exit 1
fi

# Compute USB image sizes:
#   USB1 needs to hold the ISO on FAT32 (ISO + FAT overhead + slack)
#   USB2 needs to be at least as big as the ISO
USB1_SIZE_MB=$(( ISO_SIZE_MB + 256 ))   # ISO + 256MB for FAT32 overhead
USB2_SIZE_MB=$(( ISO_SIZE_MB + 128 ))   # ISO + 128MB slack
# Round up to nearest 64MB for clean FAT32 geometry
USB1_SIZE_MB=$(( ((USB1_SIZE_MB + 63) / 64) * 64 ))
USB2_SIZE_MB=$(( ((USB2_SIZE_MB + 63) / 64) * 64 ))

echo "USB1: ${USB1_SIZE_MB}MB (source, holds ISO file)"
echo "USB2: ${USB2_SIZE_MB}MB (target, receives raw ISO)"

# Prepare firmware files
FW_COPY="/tmp/survival_iso_fw.fd"
VARS="/tmp/survival_iso_vars.fd"
cp "$FIRMWARE" "$FW_COPY"
truncate -s 64M "$FW_COPY"
dd if=/dev/zero of="$VARS" bs=1M count=64 2>/dev/null

# Create boot disk image
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

# ---- USB1: source with ISO file ----
USB1="/tmp/survival_usb_iso.img"
echo "Creating USB1 (${USB1_SIZE_MB}MB) with ISO file..."
dd if=/dev/zero of="$USB1" bs=1M count="$USB1_SIZE_MB" 2>/dev/null
mkfs.vfat -F 32 -n ISOFILES "$USB1" >/dev/null 2>&1
mcopy -i "$USB1" "$ISO_SRC" "::/$ISO_NAME"
echo "  Copied $ISO_NAME to USB1"

# ---- USB2: target (empty except README) ----
USB2="/tmp/survival_usb_target.img"
echo "Creating USB2 (${USB2_SIZE_MB}MB) target drive..."
dd if=/dev/zero of="$USB2" bs=1M count="$USB2_SIZE_MB" 2>/dev/null
mkfs.vfat -F 32 -n TARGET "$USB2" >/dev/null 2>&1
echo "This drive is the ISO write target." | mcopy -i "$USB2" - ::/README.TXT

echo ""
echo "=== ISO Write Test - QEMU aarch64 ==="
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

qemu-system-aarch64 \
    -M virt -cpu cortex-a53 -m 2G \
    -drive if=pflash,format=raw,file="$FW_COPY",readonly=on \
    -drive if=pflash,format=raw,file="$VARS" \
    -hda "$IMG" \
    -device ramfb \
    -device qemu-xhci -device usb-kbd -device usb-mouse \
    -drive if=none,id=usb1,format=raw,file="$USB1" \
    -device usb-storage,drive=usb1,removable=on \
    -drive if=none,id=usb2,format=raw,file="$USB2" \
    -device usb-storage,drive=usb2,removable=on \
    -serial stdio
