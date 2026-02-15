#!/bin/bash
set -e

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

# Create disk image with all files from ESP directory
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
# Recursively copy everything from build/esp/ onto the FAT32 image
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

MODE="${1:-graphical}"

# Always recreate test USB drive image (prevents stale cloned data from
# causing UEFI to boot from USB instead of the boot disk)
USB_IMG="/tmp/survival_usb_test.img"
echo "Creating test USB drive image..."
dd if=/dev/zero of="$USB_IMG" bs=1M count=128 2>/dev/null
mkfs.vfat -F 32 -n TESTUSB "$USB_IMG" >/dev/null 2>&1
echo "# Test USB Drive" | mcopy -i "$USB_IMG" - ::/README.MD
USB_ARGS="-drive if=none,id=usbdisk,format=raw,file=$USB_IMG -device usb-storage,drive=usbdisk,removable=on"

echo "=== Survival Workstation - QEMU ==="
echo "Mode: $MODE"
echo "Firmware: $FIRMWARE"
echo "USB: $USB_IMG"
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
            $USB_ARGS \
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
            $USB_ARGS \
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
            $USB_ARGS \
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
