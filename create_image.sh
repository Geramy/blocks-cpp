#!/bin/bash

# Exit on any error
set -e

# Define variables
IMAGE_FILE="test.img"
MOUNT_POINT="/mnt/testmount"
SIZE_MB=1000

# Create a 100MB image file
dd if=/dev/zero of="$IMAGE_FILE" bs=1M count="$SIZE_MB" || { echo "Failed to create image"; exit 1; }

# Set up a loop device with partition scanning enabled
LOOPDEV=$(losetup -f -P --show "$IMAGE_FILE") || { echo "Failed to set up loop device"; exit 1; }
echo "Setup Loop Device $LOOPDEV";
# Create a partition table with a single primary partition
echo -e "n\np\n1\n\n\nw" | fdisk "$LOOPDEV" || { echo "Failed to create partition"; exit 1; }

# Create an ext4 filesystem on the first partition
mkfs.ext4 "${LOOPDEV}p1" || { echo "Failed to create filesystem"; exit 1; }

# Create the mount point directory
mkdir -p "$MOUNT_POINT" || { echo "Failed to create mount point"; exit 1; }

# Mount the partition
#mount "${LOOPDEV}p1" "$MOUNT_POINT" || { echo "Failed to mount"; exit 1; }

# Confirm success
echo "Setup complete. Image mounted at $MOUNT_POINT"
