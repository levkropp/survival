#!/bin/bash
set -e

# exFAT/NTFS copy test (aarch64): boots workstation with three USB drives
#   USB1 — NTFS with an ISO file (read-only source)
#   USB2 — exFAT, empty (first copy target: NTFS → exFAT)
#   USB3 — exFAT, empty (second copy target: exFAT → exFAT)
#
# Usage: ./scripts/run-qemu-exfat-ntfs-test.sh [path-to-iso]
#   Default: uses Alpine aarch64 (downloads if needed)
#
# Requires: mkfs.ntfs (ntfs-3g), mkfs.exfat (exfatprogs), sudo (for loop mounts)
# Do NOT run this script with sudo — it will ask for sudo when needed.

if [ "$(id -u)" = "0" ]; then
    echo "ERROR: Do not run this script as root. It will sudo when needed."
    exit 1
fi

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

# Check tools
for tool in mkfs.ntfs mkfs.exfat; do
    if ! command -v "$tool" &>/dev/null; then
        echo "ERROR: $tool not found. Install ntfs-3g and exfatprogs."
        exit 1
    fi
done

# Find or download ISO
if [ -n "$1" ] && [ -f "$1" ]; then
    ISO_SRC="$1"
elif [ -f "/tmp/alpine-virt-aarch64.iso" ]; then
    ISO_SRC="/tmp/alpine-virt-aarch64.iso"
else
    echo "Downloading Alpine Linux aarch64 ISO..."
    ISO_SRC="/tmp/alpine-virt-aarch64.iso"
    wget -q --show-progress -O "$ISO_SRC" \
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

# Image sizes: ISO + overhead, rounded up to 64MB
USB_SIZE_MB=$(( ISO_SIZE_MB + 128 ))
USB_SIZE_MB=$(( ((USB_SIZE_MB + 63) / 64) * 64 ))
# Minimum 256MB for NTFS (mkfs.ntfs needs reasonable size)
if [ "$USB_SIZE_MB" -lt 256 ]; then USB_SIZE_MB=256; fi

echo "USB1: ${USB_SIZE_MB}MB NTFS (source, holds $ISO_NAME)"
echo "USB2: ${USB_SIZE_MB}MB exFAT (target 1: NTFS → exFAT)"
echo "USB3: ${USB_SIZE_MB}MB exFAT (target 2: exFAT → exFAT)"

# Temp file paths
FW_COPY="/tmp/survival_ent_fw.fd"
VARS="/tmp/survival_ent_vars.fd"
USB1="/tmp/survival_ntfs_src.img"
USB2="/tmp/survival_exfat_dst1.img"
USB3="/tmp/survival_exfat_dst2.img"
MNT="/tmp/survival_mnt_$$"

# Clean up any root-owned leftovers from previous runs
for f in "$FW_COPY" "$VARS" "$USB1" "$USB2" "$USB3"; do
    if [ -f "$f" ] && [ ! -w "$f" ]; then
        echo "Removing stale root-owned $f..."
        sudo rm -f "$f"
    fi
done

# Prepare firmware files
cp "$FIRMWARE" "$FW_COPY"
truncate -s 64M "$FW_COPY"
dd if=/dev/zero of="$VARS" bs=1M count=64 2>/dev/null

# Create boot disk image
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

cleanup_mount() {
    sudo umount "$MNT" 2>/dev/null || true
    rmdir "$MNT" 2>/dev/null || true
}
trap cleanup_mount EXIT

# Prompt for sudo upfront so the mount commands don't stall
echo "Need sudo for loop-mounting NTFS/exFAT images..."
sudo true

# ---- USB1: NTFS with ISO file ----
echo "Creating USB1 (${USB_SIZE_MB}MB NTFS) with ISO file..."
dd if=/dev/zero of="$USB1" bs=1M count="$USB_SIZE_MB" 2>/dev/null
mkfs.ntfs -f -L NTFSSRC -s 512 "$USB1" >/dev/null 2>&1
mkdir -p "$MNT"
sudo mount -o loop "$USB1" "$MNT"
sudo cp "$ISO_SRC" "$MNT/$ISO_NAME"
sudo bash -c "echo 'Test file on NTFS volume.' > '$MNT/readme.txt'"
sync
sudo umount "$MNT"
echo "  Copied $ISO_NAME + readme.txt to NTFS"

# ---- USB2: exFAT target 1 (empty) ----
echo "Creating USB2 (${USB_SIZE_MB}MB exFAT) target 1..."
dd if=/dev/zero of="$USB2" bs=1M count="$USB_SIZE_MB" 2>/dev/null
mkfs.exfat -n EXFAT1 "$USB2" >/dev/null 2>&1
sudo mount -o loop "$USB2" "$MNT"
sudo bash -c "echo 'exFAT target drive 1.' > '$MNT/readme.txt'"
sync
sudo umount "$MNT"

# ---- USB3: exFAT target 2 (empty) ----
echo "Creating USB3 (${USB_SIZE_MB}MB exFAT) target 2..."
dd if=/dev/zero of="$USB3" bs=1M count="$USB_SIZE_MB" 2>/dev/null
mkfs.exfat -n EXFAT2 "$USB3" >/dev/null 2>&1
sudo mount -o loop "$USB3" "$MNT"
sudo bash -c "echo 'exFAT target drive 2.' > '$MNT/readme.txt'"
sync
sudo umount "$MNT"

rmdir "$MNT" 2>/dev/null || true
trap - EXIT

echo ""
echo "=== exFAT/NTFS Copy Test - QEMU aarch64 ==="
echo "Boot disk: $IMG (64MB FAT32, workstation)"
echo "USB1: $USB1 (${USB_SIZE_MB}MB NTFS, has $ISO_NAME)"
echo "USB2: $USB2 (${USB_SIZE_MB}MB exFAT, target 1)"
echo "USB3: $USB3 (${USB_SIZE_MB}MB exFAT, target 2)"
echo ""
echo "Test steps — NTFS → exFAT copy:"
echo "  1. Browse to [NTFS] NTFSSRC volume"
echo "  2. Select $ISO_NAME, press F3 (Copy)"
echo "  3. Press ESC to return to root, select [exFAT] EXFAT1"
echo "  4. Press F8 (Paste) — wait for copy"
echo "  5. Verify file appears and size matches"
echo ""
echo "Test steps — exFAT → exFAT copy:"
echo "  6. Select the copied $ISO_NAME on EXFAT1, press F3 (Copy)"
echo "  7. Press ESC to return to root, select [exFAT] EXFAT2"
echo "  8. Press F8 (Paste) — wait for copy"
echo "  9. Verify file appears and size matches"
echo ""
echo "Test steps — F6 rebuild:"
echo "  10. Open any .c file (or create one with F4)"
echo "  11. Press F6 to rebuild the workstation"
echo "================================================"
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
    -drive if=none,id=usb3,format=raw,file="$USB3" \
    -device usb-storage,drive=usb3,removable=on \
    -serial stdio
