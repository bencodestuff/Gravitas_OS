#!/bin/sh
set -e # Stop on error

mkdir -p iso_root

# 1. Copy the Kernel
# We copy 'gravitas.elf' to the root of the ISO
cp bin/gravitas.elf iso_root/gravitas.elf

# 2. Copy the Config
cp limine.conf iso_root/

# 3. Copy Limine System Files
cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/

# 4. Create ISO
xorriso -as mkisofs -b limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  iso_root -o gravitas.iso

# 5. Install Limine
./limine/limine bios-install gravitas.iso
