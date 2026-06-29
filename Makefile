# Makefile - builds pefiaOS into a bootable ISO with one command: `make`
# -----------------------------------------------------------------------------
# Requires the i686-elf cross toolchain on PATH (see toolchain/build-i686-elf.sh)
# plus: nasm, grub-mkrescue (grub-pc-bin + grub-common), xorriso, and qemu
# (optional, for `make run`).
# -----------------------------------------------------------------------------

# --- toolchain ---
CC  := i686-elf-gcc
AS  := nasm

CFLAGS  := -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Ikernel
LDFLAGS := -ffreestanding -O2 -nostdlib -T linker.ld
LDLIBS  := -lgcc

# --- layout ---
BUILD   := build
KERNEL  := $(BUILD)/pefiaos.bin
ISO     := pefiaOS.iso
ISODIR  := iso

OBJS := \
    $(BUILD)/boot.o \
    $(BUILD)/kernel.o \
    $(BUILD)/framebuffer.o \
    $(BUILD)/console.o \
    $(BUILD)/mouse.o \
    $(BUILD)/input.o \
    $(BUILD)/heap.o \
    $(BUILD)/wm.o \
    $(BUILD)/vfs.o \
    $(BUILD)/rtc.o \
    $(BUILD)/explorer.o \
    $(BUILD)/taskbar.o \
    $(BUILD)/shell.o \
	$(BUILD)/terminal.o \
	$(BUILD)/notepad.o \
	$(BUILD)/browser.o \
	$(BUILD)/net.o \
	$(BUILD)/pci.o \
	$(BUILD)/rtl8139.o \
	$(BUILD)/e1000.o \
	$(BUILD)/nic.o \
	$(BUILD)/clock.o \
	$(BUILD)/netstack.o \
	$(BUILD)/crypto.o \
	$(BUILD)/tls.o \
	$(BUILD)/htmlrender.o \
	$(BUILD)/inflate.o \
	$(BUILD)/image.o \
	$(BUILD)/jpeg.o \
	$(BUILD)/bitmap.o \
	$(BUILD)/domrt.o \
	$(BUILD)/domparse.o \
	$(BUILD)/css.o \
	$(BUILD)/js.o \
	$(BUILD)/games.o \
	$(BUILD)/puredoom.o \
	$(BUILD)/doom_app.o \
	$(BUILD)/doom_wad.o \


# --- top-level target ---
all: $(ISO)

$(BUILD):
	mkdir -p $(BUILD)

# Assemble the NASM boot stub to a 32-bit ELF object.
$(BUILD)/boot.o: boot/boot.asm | $(BUILD)
	$(AS) -f elf32 $< -o $@

# Compile each C source to an object.
$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# PureDOOM is third-party and warns a lot under -Wall -Wextra; build it quietly
# (and without the stack protector, which we have no runtime support for).
$(BUILD)/puredoom.o: kernel/puredoom.c kernel/PureDOOM.h | $(BUILD)
	$(CC) $(CFLAGS) -w -fno-stack-protector -c $< -o $@

# Embed the DOOM shareware IWAD into the image (incbin is relative to here).
$(BUILD)/doom_wad.o: boot/doom_wad.asm doom1.wad | $(BUILD)
	$(AS) -f elf32 $< -o $@

# Link everything into the flat kernel binary per the linker script.
$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

# Sanity check: confirm the binary is a valid Multiboot kernel, then build ISO.
$(ISO): $(KERNEL) $(ISODIR)/boot/grub/grub.cfg
	@if ! grub-file --is-x86-multiboot $(KERNEL); then \
		echo "ERROR: $(KERNEL) is NOT a valid Multiboot kernel"; exit 1; \
	fi
	cp $(KERNEL) $(ISODIR)/boot/pefiaos.bin
	grub-mkrescue -o $(ISO) $(ISODIR)

# Convenience: boot the ISO in QEMU with a user-mode-networked RTL8139 NIC so
# the browser can reach the real internet (SLIRP gives 10.0.2.15 / gw 10.0.2.2).
run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 \
		-netdev user,id=n0 -device rtl8139,netdev=n0

# Same, but capture all traffic to net.pcap for debugging.
run-net: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -m 256 \
		-netdev user,id=n0 -device rtl8139,netdev=n0 \
		-object filter-dump,id=f0,netdev=n0,file=net.pcap

clean:
	rm -rf $(BUILD) $(ISO) $(ISODIR)/boot/pefiaos.bin

.PHONY: all run clean
