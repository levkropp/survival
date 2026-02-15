#!/bin/bash
set -e

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

VARS="/tmp/survival_x64_vars.fd"
if [ -n "$VARS_TEMPLATE" ]; then
    cp "$VARS_TEMPLATE" "$VARS"
else
    # Create zero-filled vars matching firmware size
    FW_SIZE=$(stat -c%s "$FIRMWARE")
    dd if=/dev/zero of="$VARS" bs=1 count="$FW_SIZE" 2>/dev/null
fi

# Create disk image with all files from ESP directory
IMG="$BUILD_DIR/disk.img"
dd if=/dev/zero of="$IMG" bs=1M count=64 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
# Recursively copy everything from build/esp/ onto the FAT32 image
mcopy -i "$IMG" -s "$ESP_DIR"/* ::/

MODE="${1:-graphical}"

# Always recreate test USB drive image (prevents stale cloned data from
# causing UEFI to boot from USB instead of the boot disk)
USB_IMG="/tmp/survival_usb_test_x64.img"
echo "Creating test USB drive image..."
dd if=/dev/zero of="$USB_IMG" bs=1M count=128 2>/dev/null
mkfs.vfat -F 32 -n TESTUSB "$USB_IMG" >/dev/null 2>&1
echo "# Test USB Drive" | mcopy -i "$USB_IMG" - ::/README.MD
USB_ARGS="-drive if=none,id=usbdisk,format=raw,file=$USB_IMG -device usb-storage,drive=usbdisk,removable=on"

echo "=== Survival Workstation x86_64 - QEMU ==="
echo "Mode: $MODE"
echo "Firmware: $FIRMWARE"
echo "USB: $USB_IMG"
echo "============================================"

case "$MODE" in
    graphical|fb)
        echo "Opening GTK window..."
        echo "Close the window or press Ctrl+C to exit."
        qemu-system-x86_64 \
            -m 256M -vga none \
            -drive if=pflash,format=raw,file="$FIRMWARE",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -drive format=raw,file="$IMG" \
            -device ramfb \
            -device qemu-xhci -device usb-kbd -device usb-mouse \
            $USB_ARGS \
            -serial stdio
        ;;
    console|serial)
        echo "Console mode. Press Ctrl+A then X to exit QEMU."
        qemu-system-x86_64 \
            -m 256M \
            -drive if=pflash,format=raw,file="$FIRMWARE",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -drive format=raw,file="$IMG" \
            -device VGA \
            -device qemu-xhci \
            $USB_ARGS \
            -display none \
            -serial mon:stdio
        ;;
    vnc)
        # VNC mode (framebuffer accessible via VNC on port 5900)
        echo "VNC mode. Connect to localhost:5900 to see display."
        echo "Press Ctrl+C to exit."
        qemu-system-x86_64 \
            -m 256M -vga none \
            -drive if=pflash,format=raw,file="$FIRMWARE",readonly=on \
            -drive if=pflash,format=raw,file="$VARS" \
            -drive format=raw,file="$IMG" \
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
