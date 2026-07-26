# Makefile - one command (`make`) turns the kernel sources into a bootable ISO.
#
# Needs the i686-elf cross toolchain on PATH (toolchain/build-i686-elf.sh builds
# one), plus nasm, grub-mkrescue (grub-pc-bin + grub-common), xorriso, and qemu
# for `make run`.

AS := nasm

# Prefer the i686-elf cross compiler, but fall back to a host gcc driven in
# 32-bit mode - it emits the same ELF32 objects GRUB needs, and it means a
# plain Ubuntu box with gcc-multilib can build the ISO without spending half
# an hour building binutils+gcc from source first. The extra flags are the
# ones a distro gcc defaults to that a freestanding kernel must not have
# (PIE, stack protector, red-zone-ish stack checks).
CROSS := $(shell command -v i686-elf-gcc 2>/dev/null)
ifeq ($(CROSS),)
  CC        := gcc
  HOSTFLAGS := -m32 -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables
  LDEXTRA   := -m32 -no-pie
  # A host gcc without gcc-multilib has no 32-bit libgcc to link against;
  # kernel/intdiv.c supplies the 64-bit division helpers it would provide.
  LDLIBS    :=
else
  CC        := i686-elf-gcc
  HOSTFLAGS :=
  LDEXTRA   :=
  LDLIBS    := -lgcc
endif

CFLAGS  := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Ikernel -MMD -MP $(HOSTFLAGS)
LDFLAGS := -ffreestanding -O2 -nostdlib -T linker.ld $(LDEXTRA)

BUILD  := build
KERNEL := $(BUILD)/pefiaos.bin
ISO    := pefiaOS.iso
ISODIR := iso

# Every kernel/*.c gets built automatically - adding a new source file just
# means dropping it in kernel/, no list to remember to update here. kernel/*.asm
# (IDT stubs, context switch) is assembled the same hands-off way.
KERNEL_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_SRCS))
KERNEL_ASM  := $(wildcard kernel/*.asm)
KERNEL_AOBJ := $(patsubst kernel/%.asm,$(BUILD)/%.o,$(KERNEL_ASM))
OBJS        := $(BUILD)/boot.o $(BUILD)/doom_wad.o $(KERNEL_OBJS) $(KERNEL_AOBJ)

all: $(ISO)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

# PureDOOM is vendored and noisy under -Wall -Wextra; build it quietly and
# without the stack protector, since we have no runtime support for one.
$(BUILD)/puredoom.o: kernel/puredoom.c kernel/PureDOOM.h | $(BUILD)
	$(CC) $(CFLAGS) -w -fno-stack-protector -c $< -o $@

# Embeds the DOOM shareware IWAD into the image; incbin is relative to the repo
# root, which is where `make` runs from.
$(BUILD)/doom_wad.o: boot/doom_wad.asm doom1.wad | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# grub-file catches a malformed Multiboot header before we burn an ISO around it.
#
# grub-mkrescue only puts an EFI boot path on the image when GRUB's EFI
# modules are installed (grub-efi-amd64-bin / grub-efi-ia32-bin). Without
# them the ISO is BIOS/CSM only, which is invisible until someone boots it
# in a UEFI VM and gets nothing - so say it at build time instead.
$(ISO): $(KERNEL) $(ISODIR)/boot/grub/grub.cfg
	@if ! grub-file --is-x86-multiboot $(KERNEL); then \
		echo "ERROR: $(KERNEL) is NOT a valid Multiboot kernel"; exit 1; \
	fi
	cp $(KERNEL) $(ISODIR)/boot/pefiaos.bin
	grub-mkrescue -o $(ISO) $(ISODIR)
	@if [ ! -d /usr/lib/grub/x86_64-efi ] && [ ! -d /usr/lib/grub/i386-efi ]; then \
		echo ""; \
		echo "NOTE: built a BIOS-only ISO (no GRUB EFI modules installed)."; \
		echo "      Set the VM firmware to BIOS/legacy, or install"; \
		echo "      grub-efi-amd64-bin for a hybrid BIOS+UEFI image."; \
	fi

# A raw disk image for the ATA PIO driver to read/write. Created once if absent;
# `disk` in the terminal dumps its first sector.
disk.img:
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	printf 'PEFIADISK v1 - hello from sector 0 of the ATA PIO driver' \
		| dd of=$@ conv=notrunc 2>/dev/null

# NAT-networked RTL8139 so the browser can reach the real internet
# (SLIRP hands out 10.0.2.15, gateway 10.0.2.2); primary IDE disk for ATA.
run: $(ISO) disk.img
	qemu-system-i386 -cdrom $(ISO) -m 256 \
		-drive file=disk.img,format=raw,if=ide \
		-netdev user,id=n0 -device rtl8139,netdev=n0

# The other common VM shape: q35/ICH9 ("Standard PC (Q35 + ICH9, 2009)") with
# an e1000 NIC and the disk on AHCI, since q35 has no legacy IDE controller.
# Everything still comes up there; the disk just reports "none" in the boot
# self-test, because the ATA PIO driver only speaks to the legacy ports.
run-q35: $(ISO) disk.img
	qemu-system-i386 -machine q35 -cdrom $(ISO) -m 512 \
		-drive file=disk.img,format=raw,if=none,id=d0 -device ide-hd,drive=d0 \
		-netdev user,id=n0 -device e1000,netdev=n0

# Same as `run`, but also captures traffic to net.pcap for debugging.
run-net: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 \
		-object filter-dump,id=f0,netdev=n0,file=net.pcap

clean:
	rm -rf $(BUILD) $(ISO) $(ISODIR)/boot/pefiaos.bin

# Per-object header dependencies from -MMD: touching a .h rebuilds only the
# objects that include it instead of needing a full clean every time.
-include $(OBJS:.o=.d)

.PHONY: all run run-q35 run-net clean
