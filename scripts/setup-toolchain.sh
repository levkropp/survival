#!/bin/bash
set -e

echo "=== Survival Workstation - Toolchain Setup ==="

# Detect package manager
if command -v apt-get &>/dev/null; then
    PM="apt-get"
    sudo apt-get update
    sudo apt-get install -y \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        gnu-efi \
        qemu-system-arm \
        qemu-efi-aarch64 \
        ovmf
elif command -v pacman &>/dev/null; then
    PM="pacman"
    sudo pacman -S --noconfirm \
        aarch64-linux-gnu-gcc \
        aarch64-linux-gnu-binutils \
        qemu-system-aarch64 \
        edk2-aarch64
elif command -v dnf &>/dev/null; then
    PM="dnf"
    sudo dnf install -y \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        gnu-efi-devel \
        qemu-system-aarch64 \
        edk2-aarch64
else
    echo "ERROR: No supported package manager found (apt/pacman/dnf)"
    exit 1
fi

echo ""
echo "=== Verifying installation ==="
echo -n "Cross-compiler: "
aarch64-linux-gnu-gcc --version | head -1

echo -n "QEMU: "
qemu-system-aarch64 --version | head -1

# Find gnu-efi headers
for dir in /usr/include/efi /usr/include/gnu-efi; do
    if [ -d "$dir" ]; then
        echo "GNU-EFI headers: $dir"
        break
    fi
done

# Find UEFI firmware for QEMU
for fw in /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
          /usr/share/edk2/aarch64/QEMU_EFI.fd \
          /usr/share/AAVMF/AAVMF_CODE.fd \
          /usr/share/edk2-aarch64/QEMU_EFI.fd; do
    if [ -f "$fw" ]; then
        echo "UEFI firmware: $fw"
        break
    fi
done

echo ""
echo "=== Toolchain setup complete ==="
