#!/usr/bin/env bash
#
# mkimg.sh — Create a bootable USB disk image from superboot.efi
#
# Usage:
#   ./tools/mkimg.sh [path/to/superboot.efi] [output.img]
#
# Creates a GPT disk image with a single FAT32 EFI System Partition
# containing the SuperBoot binary at /EFI/BOOT/BOOTX64.EFI.
#
# Requires: parted, mkfs.fat, mtools (mmd, mcopy)

set -euo pipefail

EFI_BINARY="${1:-build/superboot.efi}"
OUTPUT="${2:-build/superboot-usb.img}"
SIZE_MB=128

if [ ! -f "$EFI_BINARY" ]; then
    echo "Error: $EFI_BINARY not found. Run 'make' first."
    exit 1
fi

echo "Creating ${SIZE_MB}MiB disk image..."
dd if=/dev/zero of="$OUTPUT" bs=1M count="$SIZE_MB" 2>/dev/null

# Create GPT with a single ESP partition.
parted -s "$OUTPUT" \
    mklabel gpt \
    mkpart ESP fat32 1MiB 100% \
    set 1 esp on

# Set up a loop device to format the partition.
LOOP=$(sudo losetup --find --show --partscan "$OUTPUT")
trap "sudo losetup -d $LOOP" EXIT

# Wait for partition device to appear.
sleep 1
PART="${LOOP}p1"

sudo mkfs.fat -F 32 -n "SUPERBOOT" "$PART"

# Mount and copy.
MOUNT=$(mktemp -d)
sudo mount "$PART" "$MOUNT"

sudo mkdir -p "$MOUNT/EFI/BOOT"
sudo mkdir -p "$MOUNT/EFI/superboot/drivers"
sudo cp "$EFI_BINARY" "$MOUNT/EFI/BOOT/BOOTX64.EFI"
sudo cp "$EFI_BINARY" "$MOUNT/EFI/superboot/superboot.efi"

sudo umount "$MOUNT"
rmdir "$MOUNT"

echo "==> Image created: $OUTPUT"
echo "    Write to USB:  sudo dd if=$OUTPUT of=/dev/sdX bs=4M status=progress"
