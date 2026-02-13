#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
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
    echo "ERROR: UEFI firmware not found. Install qemu-efi-aarch64 or edk2-aarch64."
    exit 1
fi

if [ ! -f "$ESP_DIR/EFI/BOOT/BOOTAA64.EFI" ]; then
    echo "ERROR: Build first with 'make'"
    exit 1
fi

# Prepare firmware files
FW_COPY="/tmp/survival_uefi_fw.fd"
VARS="/tmp/survival_uefi_vars.fd"
cp "$FIRMWARE" "$FW_COPY"
truncate -s 64M "$FW_COPY"
dd if=/dev/zero of="$VARS" bs=1M count=64 2>/dev/null

# Create disk image
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mmd -i "$IMG" "::EFI"
mmd -i "$IMG" "::EFI/BOOT"
mcopy -i "$IMG" "$ESP_DIR/EFI/BOOT/BOOTAA64.EFI" "::EFI/BOOT/BOOTAA64.EFI"

MODE="${1:-graphical}"

echo "=== Survival Workstation - QEMU ==="
echo "Mode: $MODE"
echo "Firmware: $FIRMWARE"
echo "==================================="

case "$MODE" in
    graphical|fb)
        # Graphical mode with ramfb (real framebuffer)
        echo "Opening GTK window..."
        echo "Close the window or press Ctrl+C to exit."
        qemu-system-aarch64 \
            -M virt -cpu cortex-a53 -m 256M \
            -drive if=pflash,format=raw,file="$FW_COPY",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -hda "$IMG" \
            -device ramfb \
            -device qemu-xhci -device usb-kbd -device usb-mouse \
            -serial stdio
        ;;
    console|serial)
        # Console-only mode (no framebuffer, text over serial)
        echo "Console mode. Press Ctrl+A then X to exit QEMU."
        qemu-system-aarch64 \
            -M virt -cpu cortex-a53 -m 256M \
            -drive if=pflash,format=raw,file="$FW_COPY",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -hda "$IMG" \
            -device virtio-gpu-pci \
            -device qemu-xhci -device usb-kbd \
            -display none \
            -serial mon:stdio
        ;;
    vnc)
        # VNC mode (framebuffer accessible via VNC on port 5900)
        echo "VNC mode. Connect to localhost:5900 to see display."
        echo "Press Ctrl+C to exit."
        qemu-system-aarch64 \
            -M virt -cpu cortex-a53 -m 256M \
            -drive if=pflash,format=raw,file="$FW_COPY",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -hda "$IMG" \
            -device ramfb \
            -device qemu-xhci -device usb-kbd -device usb-mouse \
            -display vnc=:0 \
            -serial stdio
        ;;
    *)
        echo "Usage: $0 [graphical|console|vnc]"
        echo "  graphical  - GTK window with framebuffer (default)"
        echo "  console    - Serial console only, no display"
        echo "  vnc        - VNC server on port 5900"
        exit 1
        ;;
esac
