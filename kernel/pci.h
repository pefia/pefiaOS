/* PCI config-space access via the legacy 0xCF8/0xCFC I/O ports (mechanism
 * #1). Good enough for the bus scan we need to find the NIC. */
#ifndef PEFIA_PCI_H
#define PEFIA_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
} PciDeviceInfo;

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_config_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

int pci_find_device_by_id(uint16_t vendor, uint16_t device, PciDeviceInfo *out);
int pci_find_first_network_controller(PciDeviceInfo *out);

#endif
