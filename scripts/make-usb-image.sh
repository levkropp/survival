#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

ARCH="${1:-x86_64}"
ESP_DIR="$PROJECT_DIR/build/$ARCH/esp"
OUT="$PROJECT_DIR/build/$ARCH/survival-usb.img"

# Determine boot binary name
if [ "$ARCH" = "aarch64" ]; then
    BOOT_EFI="EFI/BOOT/BOOTAA64.EFI"
else
    BOOT_EFI="EFI/BOOT/BOOTX64.EFI"
fi

if [ ! -f "$ESP_DIR/$BOOT_EFI" ]; then
    echo "ERROR: Build first with 'make ARCH=$ARCH'"
    exit 1
fi

# Calculate needed size: ESP contents + 16MB headroom for partition table/alignment
ESP_SIZE=$(du -sm "$ESP_DIR" | cut -f1)
IMG_SIZE=$(( ESP_SIZE + 16 ))
if [ "$IMG_SIZE" -lt 64 ]; then
    IMG_SIZE=64
fi

echo "=== Making bootable USB image ($ARCH) ==="
echo "ESP size: ${ESP_SIZE} MB"
echo "Image size: ${IMG_SIZE} MB"

# Create empty image
dd if=/dev/zero of="$OUT" bs=1M count=$IMG_SIZE 2>/dev/null

# Create GPT with single EFI System Partition
# Partition starts at sector 2048 (1MB aligned), fills the rest
sgdisk -Z "$OUT" >/dev/null 2>&1
sgdisk -n 1:2048:0 -t 1:EF00 -c 1:SURVIVAL "$OUT" >/dev/null 2>&1

# Attach as loop device with partition scanning
LOOP=$(sudo losetup --find --show --partscan "$OUT")
PART="${LOOP}p1"

# Wait for partition device to appear
for i in $(seq 1 10); do
    [ -b "$PART" ] && break
    sleep 0.2
done

if [ ! -b "$PART" ]; then
    echo "ERROR: Partition device $PART not found"
    sudo losetup -d "$LOOP"
    exit 1
fi

# Format partition as FAT32
sudo mkfs.vfat -F 32 -n SURVIVAL "$PART" >/dev/null 2>&1

# Mount and copy files
MOUNT_DIR=$(mktemp -d)
sudo mount "$PART" "$MOUNT_DIR"
sudo cp -r "$ESP_DIR"/* "$MOUNT_DIR"/
sudo umount "$MOUNT_DIR"
rmdir "$MOUNT_DIR"

# Detach loop device
sudo losetup -d "$LOOP"

echo "=== Done: $OUT ==="
echo ""
echo "Flash to USB with:"
echo "  sudo dd if=$OUT of=/dev/sdX bs=4M status=progress"
echo ""
echo "Replace /dev/sdX with your USB drive (check with 'lsblk')."
