# blocks-cpp

## Untested everything except for LVM/to-lvm
This program was converted from the original one in python into C++
Original https://github.com/g2p/blocks/tree/master
Then to-lvm was tested, the python one had all kinds of versioning problems so I said im done with this i'll just convert it to C++
So here we are!

If someone wants to continue my work they may fork and send PRs.

## I have used this on my linux system it was lots of fun.
I ended up having to chroot like it said and everything else but you need to be careful to mount the boot partition if using EFI
to /boot/efi and not anything else and then as long as the UUID of the partition is the same as the old one you wont need to worry about NVRAM
when you update FSTAB make sure to use the /dev/mapper/ vg path instead.

## Description

Conversion tools for block devices.

Convert between raw partitions, logical volumes, and bcache devices
without moving data.  `blocks` shuffles blocks and sprouts superblocks.

# New Instructions - Generated

Here’s the revised step-by-step procedure for converting your Pop!_OS root filesystem to LVM using our C++ blocks binary, formatted in Markdown (.md).
This assumes you’re using the Pop!_OS Live ISO and the binary at ~/Documents/Development/lvmify_full/cmake-build-debug/blocks, avoiding the Python3 version entirely.
Converting Pop!_OS Root Filesystem to LVM with C++ Blocks Binary
This guide details how to convert your Pop!_OS root filesystem to LVM using our custom C++ blocks binary, executed from a Pop!_OS Live ISO. It includes preparing the live environment, running the conversion, and updating the systemd-boot bootloader to boot from the LVM logical volume.
Prerequisites
Pop!_OS Live ISO: Download from System76’s website and create a bootable USB (e.g., with dd or Etcher).
Backup: Ensure your data is backed up. This is an in-place conversion; errors could lead to data loss.
Target Device: Identify your root filesystem’s block device (e.g., /dev/nvme0n1p2 or /dev/sda1). Replace this with your actual root partition in the steps below.
C++ blocks Binary: Located at ~/Documents/Development/lvmify_full/cmake-build-debug/blocks on your development machine.
Step-by-Step Procedure
1. Boot into Pop!_OS Live ISO
   Insert the Pop!_OS Live USB and boot your system into the live environment.
   Select "Try Pop!_OS" to enter the live session.
2. Prepare the Live Environment
   Open a Terminal: Launch the terminal application in the live session.
   Mount Your Root Filesystem: Identify and mount your root partition to access its contents.
```
sudo mkdir /mnt/root
sudo mount /dev/nvme0n1p2 /mnt/root  # Replace /dev/nvme0n1p2 with your root device
ls /mnt/root  # Verify contents (e.g., /bin, /etc, /home)
If encrypted (e.g., LUKS):
bash
sudo cryptsetup luksOpen /dev/nvme0n1p2 root_crypt
sudo mount /dev/mapper/root_crypt /mnt/root
Mount Additional Filesystems: For a full chroot later:
bash
sudo mount --bind /dev /mnt/root/dev
sudo mount --bind /proc /mnt/root/proc
sudo mount --bind /sys /mnt/root/sys
sudo mount --bind /run /mnt/root/run
If you have a separate /boot partition (e.g., /dev/nvme0n1p1):
bash
sudo mkdir /mnt/root/boot
sudo mount /dev/nvme0n1p1 /mnt/root/boot
```


# In live session (after plugging in USB)
```
sudo cp /media/user/usb/blocks /tmp/blocks
sudo chmod +x /tmp/blocks
```

4. Convert the Root Filesystem to LVM
   Unmount the Root Filesystem: Ensure it’s not mounted for the conversion.
```
sudo umount /mnt/root/dev /mnt/root/proc /mnt/root/sys /mnt/root/run /mnt/root/boot /mnt/root
Run the Conversion: Use our C++ blocks binary.
bash
sudo /tmp/blocks to-lvm /dev/nvme0n1p2  # Replace with your root device
Expected output:
Checking the filesystem before resizing it
Executing: e2fsck -f -y -- /dev/nvme0n1p2
```

Will shrink the filesystem (ext4) by 7340032 bytes

Executing: resize2fs -- /dev/nvme0n1p2 253952

Installing LVM metadata...
Writing 4194304 bytes of metadata to physical device at offset 0
ok
Activating volume group vg.nvme0n1p2... ok
LVM conversion successful!
Volume group name: vg.nvme0n1p2
Logical volume name: nvme0n1p2
Filesystem uuid: fc07dd43-5d65-4806-97ff-7745ce118cf0 <uuid>

5. Update the Bootloader (systemd-boot)
   Pop!_OS uses systemd-boot, so we need to update the kernel and initramfs to recognize the LVM root.
   Chroot into the Converted System:
```
sudo mount /dev/vg.nvme0n1p2/nvme0n1p2 /mnt/root  # Use the LV path from the output
sudo mount --bind /dev /mnt/root/dev
sudo mount --bind /proc /mnt/root/proc
sudo mount --bind /sys /mnt/root/sys
sudo mount --bind /run /mnt/root/run
sudo mount /dev/nvme0n1p1 /mnt/root/boot  # If separate boot partition
sudo chroot /mnt/root
```
Install LVM2 in the Chroot:

```
apt update
apt install -y lvm2
Update /etc/fstab:
Edit /etc/fstab to use the LVM logical volume:
```

```
nano /etc/fstab
# Replace the root entry (e.g., /dev/nvme0n1p2) with:
/dev/vg.nvme0n1p2/nvme0n1p2  /  ext4  defaults  0  1
```
Save and exit (Ctrl+O, Enter, Ctrl+X).
Update Initramfs:
Ensure the initramfs includes LVM support:

```
echo "lvm" >> /etc/initramfs-tools/modules
update-initramfs -u -k all
```
Update systemd-boot:
Pop!_OS uses kernelstub to manage systemd-boot entries. Update the root parameter:
bash
```
kernelstub -r /dev/vg.nvme0n1p2/nvme0n1p2
```
Verify the loader entry:
```
cat /boot/loader/entries/Pop_OS-current.conf
```
Ensure options includes root=/dev/vg.nvme0n1p2/nvme0n1p2 (or UUID if preferred):
```
blkid /dev/vg.nvme0n1p2/nvme0n1p2  # Get UUID
# Edit if needed
nano /boot/loader/entries/Pop_OS-current.conf
# Example:
# options root=UUID=<uuid> rw quiet loglevel=3 ...
```
Exit Chroot:
```
exit
sudo umount /mnt/root/dev /mnt/root/proc /mnt/root/sys /mnt/root/run /mnt/root/boot /mnt/root
```

6. Reboot and Verify
   Reboot:
```
sudo reboot
```
Check Boot:
If successful, Pop!_OS should boot normally from /dev/vg.nvme0n1p2/nvme0n1p2.
Verify:
```
df -h /  # Should show /dev/vg.nvme0n1p2/nvme0n1p2 mounted on /
lsblk    # Should show LVM structure
```
Troubleshooting Boot Issues
Boot Failure:
If the system doesn’t boot, use the Pop!_OS Live ISO again, chroot back in, and check:
```
cat /boot/loader/entries/Pop_OS-current.conf  # Verify root= parameter
update-initramfs -u -k all
kernelstub -r /dev/vg.nvme0n1p2/nvme0n1p2
```
Initramfs Missing LVM:
Ensure lvm is in /etc/initramfs-tools/modules and regenerate if not:
```
echo "lvm" >> /etc/initramfs-tools/modules
update-initramfs -u -k all
```
Why This Works
Custom C++ Binary: Our blocks binary performs the in-place conversion, shrinking the filesystem and setting up LVM without Python dependencies.
LVM Recognition: Installing lvm2 and updating the initramfs ensures the kernel can mount the LVM root.
systemd-boot Update: kernelstub adjusts the bootloader to point to the new LVM logical volume, aligning with Pop!_OS conventions.
Post-Conversion
Expand LVM: Add more disks if desired:
```
sudo vgextend vg.nvme0n1p2 /dev/sdb1
sudo lvextend -l +100%FREE /dev/vg.nvme0n1p2/nvme0n1p2
sudo resize2fs /dev/vg.nvme0n1p2/nvme0n1p2
```
## Notes

### Replace /dev/nvme0n1p2 with your actual root device throughout the steps.

For testing with a loop device (e.g., /dev/loop26p1), adapt the paths, but use your physical root partition for the real system.
Follow these steps with your actual root device, and let me know how it goes or if you encounter any issues!
Your data integrity verification is a great foundation, and this should ensure a smooth transition to booting from LVM.


# Old Instructions
## LVM conversion

`blocks to-lvm` (alias: `lvmify`) takes a block device (partition or
whole disk) containing a filesystem, shrinks the filesystem by a small
amount, and converts it to LVM in place.

The block device is converted to a physical volume and the filesystem is
converted to a logical volume.  If `--join=<VG>` is used the volumes
join an existing volume group.

An LVM conversion can be followed by other changes to the volume,
growing it to multiple disks with `vgextend` and `lvextend`, or
converting it to various RAID levels with `lvconvert --type=raidN
-m<extra-copies>`.

## bcache conversion

`blocks to-bcache` converts a block device (partition, logical volume,
LUKS device) to use bcache.  If `--join=<cset-uuid>` is used the device
joins an existing cache set.  Otherwise you will need to [create
and attach the cache device
manually](http://evilpiepirate.org/git/linux-bcache.git/tree/Documentation/bcache.txt?h=bcache-dev#n80).

`blocks` will pick one of several conversion strategies:

* one for partitions, which requires a shrinkable filesystem or free space
immediately before the partition to convert.  Converting a [logical partition](
https://en.wikipedia.org/wiki/Extended_boot_record)
to bcache is not supported:  if `blocks` complains about overlapping metadata
in the middle of the disk, please [use gdisk to convert your MBR disk to GPT](
http://falstaff.agner.ch/2012/11/20/convert-mbr-partition-table-to-gpt-ubuntu/)
and reinstall your bootloader before proceeding with the bcache conversion.
* one for LUKS volumes
* one for LVM logical volumes

When the first two strategies are unavailable, you can still convert
to bcache by converting to LVM first, then converting the new LV to
bcache.

You will need to install bcache-tools, which is available here:

* <http://evilpiepirate.org/git/bcache-tools.git/>
* <https://launchpad.net/~g2p/+archive/storage/> (`sudo add-apt-repository ppa:g2p/storage`; for ubuntu 13.10 and newer)

Conversion makes no demands on the kernel, but to use bcache, you need
Linux 3.10 or newer.  [My own branch](https://github.com/g2p/linux/commits/for-3.11/bcache) currently adds
resizing support on top of [Kent Overstreet's upstream branch](http://evilpiepirate.org/git/linux-bcache.git/).

### maintboot mode

Maintboot mode (`blocks to-bcache --maintboot`) is an easier way
to convert in-use devices that doesn't require a LiveCD.
[maintboot](https://github.com/g2p/maintboot) will run
the conversion from an in-memory boot environment.
This is currently tested on Ubuntu; ports to other
distributions are welcome.

# Ubuntu PPA (13.10 and newer)

You can install python3-blocks from a PPA and skip the rest
of the installation section.

    sudo apt-get install software-properties-common
    sudo add-apt-repository ppa:g2p/storage
    sudo apt-get update
    sudo apt-get install python3-blocks bcache-tools

# Requirements

Python 3.3, pip and Git are required before installing.

You will also need libparted (2.3 or newer, library and headers) and
libaugeas (library only, 1.0 or newer).

On Debian/Ubuntu (Ubuntu 13.04 or newer is recommended):

    sudo aptitude install python3.3 python3-pip git libparted-dev libaugeas0 \
        pkg-config libpython3.3-dev gcc
    sudo aptitude install cryptsetup lvm2 liblzo2-dev \
        nilfs-tools reiserfsprogs xfsprogs e2fsprogs btrfs-tools  # optional
    type pip-3.3 || alias pip-3.3='python3.3 -m pip.runner'

Command-line tools for LVM2, LUKS, bcache (see above), filesystem
resizing (see below for btrfs) are needed if those formats are involved.
Kernel support isn't required however, so you can do bcache conversions
from a live-cd/live-usb for example.

For btrfs resizing, you need a package that provides `btrfs-show-super`,
or you can install from source:

* <http://git.kernel.org/cgit/linux/kernel/git/mason/btrfs-progs.git>

# Installation

    pip-3.3 install --user -r <(wget -O- https://raw.github.com/g2p/blocks/master/requirements.txt)
    cp -lt ~/bin ~/.local/bin/blocks

# Usage

## Converting your root filesystem to LVM

Install LVM.

Edit your `/etc/fstab` to refer to filesystems by UUID, and regenerate
your initramfs so that it picks up the new tools.

With grub2, you don't need to switch to a separate boot
partition, but make sure grub2 installs `lvm.mod` inside your `/boot`.

Make sure your backups are up to date, boot to live media ([Ubuntu raring
liveusb](http://cdimage.ubuntu.com/daily-live/current/) is a good
choice), install blocks, and convert.

## Converting your root filesystem to bcache

Install bcache-tools and a recent kernel (3.10 or newer).
If your distribution uses Dracut (Fedora), you need Dracut 0.31 or newer.

Edit your `/etc/fstab` to refer to filesystems by UUID, and regenerate
your initramfs so that it picks up the new tools.
On Debian and Ubuntu, this is done with `update-initramfs -u -k all`.
With Dracut (Fedora), this is done with `dracut -f`.
Arch Linux users should enable the bcache hook in `mkinitcpio.conf`
and rerun `mkinitcpio`.
If you don't see your distribution in this list, you are welcome to
port [this hook](https://github.com/g2p/bcache-tools/blob/master/initcpio/install)
to your distribution's preferred tools and contribute a patch to bcache-tools.
Having working bcache support in your initramfs is important, as your system
will be unbootable without.

Edit your `grub.cfg` to refer to filesystems by UUID on the kernel
command-line (this is often the case, except when you are already using
LVM, in which case `update-grub` tends to write a logical path).  Make
sure you have a separate `/boot` partition.

1. If you don't have a cache device yet, create it on an empty SSD (or on a
   properly aligned partition or LV on top of it; LVM's 4MiB alignment is
   sufficient, as is the 1MiB alignment of modern partitioning tools).

        sudo make-bcache -C /dev/<cache-device>
   This will give you a cache-set uuid.

2. If you already have a cache device

        ls /sys/fs/bcache
   And copy the cache-set uuid.

3. Finally, if you have a maintboot-compatible distribution, run:

        sudo blocks to-bcache --maintboot /dev/<root-device> --join <cset-uuid>
   If you are using encryption, use the encrypted device as the root device so
   that cache contents are also encrypted.

4. Otherwise,
make sure your backups are up to date, boot to live media ([Ubuntu raring
liveusb](http://cdimage.ubuntu.com/daily-live/current/) is a good
choice), install blocks, and convert.

## bcache on a fresh install

When using a distribution installer that doesn't support bcache
at the partitioning stage, make sure the installer creates a
separate `/boot` partition.  Install everything on the HDD,
using whatever layout you prefer (but I suggest LVM if you want
multiple partitions).

Once the installer is done, you can follow the steps at
[converting your root filesystem to bcache](#converting-your-root-filesystem-to-bcache).

## Subcommand help

    blocks --help
    blocks <subcommand> --help

If `blocks` isn't in the shell's command path, replace with:

    sudo python3.3 -m blocks

# Build status

[![Build Status](https://travis-ci.org/g2p/blocks.png)](https://travis-ci.org/g2p/blocks)
