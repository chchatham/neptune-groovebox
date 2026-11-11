/*
 * bootloader.h — Bootloader reentry for Launchkey Mini MK4.
 *
 * The stock bootloader checks two magic words at the top of SRAM on reset.
 * If they match, it enters DFU/firmware-update mode instead of jumping to
 * the application.
 *
 * Magic addresses: 0x2000FFF8, 0x2000FFFC (top 8 bytes of 64K SRAM).
 * The linker script reserves these by setting _estack = 0x2000FFF8.
 */

#ifndef BOOTLOADER_H
#define BOOTLOADER_H

/*
 * Reboot into the stock bootloader (firmware update mode).
 *
 * Writes the magic sentinel values, then triggers a system reset via
 * SCB AIRCR. The bootloader will see the magic on the next boot and
 * stay in DFU mode.
 *
 * This function does not return.
 */
void reboot_to_bootloader(void);

#endif /* BOOTLOADER_H */
