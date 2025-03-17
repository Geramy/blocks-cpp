#!/bin/bash

# Exit on any error
set -e

# Define variables
IMAGE_FILE="test.img"
MOUNT_POINT="/mnt/testmount"

# Find the loop device associated with the image file
LOOPDEV=$(losetup -j "$IMAGE_FILE" | cut -d: -f1)
if [ -z "$LOOPDEV" ]; then
    echo "No loop device found for $IMAGE_FILE"
    exit 1
fi

# Unmount the partition
umount "$MOUNT_POINT" || { echo "Failed to unmount"; exit 1; }

# Detach the loop device
losetup -d "$LOOPDEV" || { echo "Failed to detach loop device"; exit 1; }

# Delete the image file
rm "$IMAGE_FILE" || { echo "Failed to delete image"; exit 1; }

# Remove the mount point directory
rmdir "$MOUNT_POINT" || { echo "Failed to remove mount point"; exit 1; }

# Confirm success
echo "Cleanup complete."
