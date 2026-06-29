/* kernel/io.h
 * -----------------------------------------------------------------------------
 * Thin wrappers around the x86 IN/OUT instructions for talking to hardware
 * ports (VGA cursor registers, the PS/2 keyboard controller, etc.).
 * -----------------------------------------------------------------------------
 */
#ifndef PEFIA_IO_H
#define PEFIA_IO_H

#include <stdint.h>

/* Write one byte to an I/O port. */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one byte from an I/O port. */
static inline uint8_t inb(uint16_t port)
{
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write one 16-bit word to an I/O port. */
static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one 16-bit word from an I/O port. */
static inline uint16_t inw(uint16_t port)
{
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write one 32-bit dword to an I/O port. */
static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* Read one 32-bit dword from an I/O port. */
static inline uint32_t inl(uint16_t port)
{
    uint32_t result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Tiny delay used around legacy device reset paths. */
static inline void io_wait(void)
{
    __asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

#endif /* PEFIA_IO_H */
