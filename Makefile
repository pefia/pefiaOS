# Makefile - one command (`make`) turns the kernel sources into a bootable ISO.
#
# Needs the i686-elf cross toolchain on PATH (toolchain/build-i686-elf.sh builds
# one), plus nasm, grub-mkrescue (grub-pc-bin + grub-common), xorriso, and qemu
# for `make run`.

CC := i686-elf-gcc
AS := nasm

CFLAGS  := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Ikernel -MMD -MP
LDFLAGS := -ffreestanding -O2 -nostdlib -T linker.ld
LDLIBS  := -lgcc

BUILD  := build
KERNEL := $(BUILD)/pefiaos.bin
ISO    := pefiaOS.iso
ISODIR := iso

# Every kernel/*.c gets built automatically - adding a new source file just
# means dropping it in kernel/, no list to remember to update here.
KERNEL_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_SRCS))
OBJS        := $(BUILD)/boot.o $(BUILD)/doom_wad.o $(KERNEL_OBJS)

all: $(ISO)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

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
$(ISO): $(KERNEL) $(ISODIR)/boot/grub/grub.cfg
	@if ! grub-file --is-x86-multiboot $(KERNEL); then \
		echo "ERROR: $(KERNEL) is NOT a valid Multiboot kernel"; exit 1; \
	fi
	cp $(KERNEL) $(ISODIR)/boot/pefiaos.bin
	grub-mkrescue -o $(ISO) $(ISODIR)

# NAT-networked RTL8139 so the browser can reach the real internet
# (SLIRP hands out 10.0.2.15, gateway 10.0.2.2).
run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 \
		-netdev user,id=n0 -device rtl8139,netdev=n0

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

.PHONY: all run run-net clean
