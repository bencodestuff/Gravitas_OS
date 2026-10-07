#!/bin/sh
set -e # Stop on error

mkdir -p iso_root

# 1. Copy the Kernel
# We copy 'gravitas.elf' to the root of the ISO
cp bin/gravitas.elf iso_root/gravitas.elf

# 2. Pack the tarfs. THE TAR LAYOUT IS THE FILESYSTEM TREE: the tar is
#   mounted at "/" and its contents appear 1:1 as paths.
#     bin/       userland programs (grv_spawn resolves bare names here)
#     bin/tests/ platform test programs (also on the program search path)
#     fonts/     console font (PSF2, loaded by Font::load_from_disk)
#     Pictures/  demo images
#     tmp/       EMPTY dir: mountpoint placeholder for the writable
#                ramfs (mounted over it at boot) so `ls /` shows /tmp
#   (Clean tar_root first: it must mirror files/ exactly, and /tmp is
#   regenerated fresh each build since the tar is read-only.)
rm -rf tar_root
mkdir -p tar_root/bin tar_root/tmp
cp -r files/* tar_root/
for f in bin/*.elf; do
    [ "$(basename "$f")" = "gravitas.elf" ] && continue
    cp "$f" tar_root/bin/
done
if [ -d bin/tests ]; then
    mkdir -p tar_root/bin/tests
    cp bin/tests/*.elf tar_root/bin/tests/
fi
# 2b. The symbol table the fault handler names a RIP with: every function in the
#     kernel and in every program, sorted by address. Generated here because it
#     is only knowable once they are linked (and it cannot live in the kernel it
#     describes).
python3 tools/mksyms.py tar_root/symbols.bin bin/gravitas.elf bin/*.elf bin/tests/*.elf

tar --format=ustar -cf iso_root/files.tar -C tar_root .

# 3. Copy the Config
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
