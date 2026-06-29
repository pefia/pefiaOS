#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return (uint32_t)(0x80000000u |
        ((uint32_t)bus  << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8)  |
        ((uint32_t)(offset & 0xFC)));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_make_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, slot, func, offset);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

static int pci_probe_device(uint8_t bus, uint8_t slot, uint8_t func, PciDeviceInfo *out)
{
    uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return 0;

    if (!out) return 1;

    out->bus         = bus;
    out->slot        = slot;
    out->func        = func;
    out->vendor_id   = vendor;
    out->device_id   = pci_config_read16(bus, slot, func, 0x02);
    out->prog_if     = pci_config_read8(bus, slot, func, 0x09);
    out->subclass    = pci_config_read8(bus, slot, func, 0x0A);
    out->class_code  = pci_config_read8(bus, slot, func, 0x0B);
    out->header_type = pci_config_read8(bus, slot, func, 0x0E);
    return 1;
}

int pci_find_device_by_id(uint16_t vendor, uint16_t device, PciDeviceInfo *out)
{
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            for (uint8_t f = 0; f < 8; f++) {
                PciDeviceInfo d;
                if (!pci_probe_device((uint8_t)b, s, f, &d)) {
                    if (f == 0) break;
                    continue;
                }
                if (d.vendor_id == vendor && d.device_id == device) {
                    if (out) *out = d;
                    return 1;
                }
                if (f == 0 && ((d.header_type & 0x80) == 0)) break;
            }
        }
    }
    return 0;
}

int pci_find_first_network_controller(PciDeviceInfo *out)
{
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            for (uint8_t f = 0; f < 8; f++) {
                PciDeviceInfo d;
                if (!pci_probe_device((uint8_t)b, s, f, &d)) {
                    if (f == 0) break;
                    continue;
                }
                if (d.class_code == 0x02) {
                    if (out) *out = d;
                    return 1;
                }
                if (f == 0 && ((d.header_type & 0x80) == 0)) break;
            }
        }
    }
    return 0;
}
