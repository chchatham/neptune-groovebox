/*
 * bootloader.c — Bootloader reentry implementation.
 *
 * The stock Launchkey MK4 bootloader (0x08000000-0x0802FFFF) checks
 * two magic words at the top of SRAM on every reset. If both match,
 * it enters firmware-update (DFU/SysEx) mode.
 *
 * Magic words (from RE of stock firmware behavior):
 *   0x2000FFF8 = 0x9669A55A
 *   0x2000FFFC = 0x3CC3F00F
 */

#include "stm32f4xx.h"
#include "bootloader.h"

/* Boot magic sentinel addresses (top 8 bytes of 64K SRAM) */
#define BOOT_MAGIC_ADDR0    ((volatile uint32_t *)0x2000FFF8UL)
#define BOOT_MAGIC_ADDR1    ((volatile uint32_t *)0x2000FFFCUL)

#define BOOT_MAGIC_WORD0    0x9669A55AUL
#define BOOT_MAGIC_WORD1    0x3CC3F00FUL

void reboot_to_bootloader(void)
{
    __disable_irq();

    /* Write sentinel values that the bootloader checks on reset */
    *BOOT_MAGIC_ADDR0 = BOOT_MAGIC_WORD0;
    *BOOT_MAGIC_ADDR1 = BOOT_MAGIC_WORD1;

    /* Ensure writes complete before reset */
    __DSB();

    /* Request system reset via SCB AIRCR */
    SCB_AIRCR = SCB_AIRCR_VECTKEY | SCB_AIRCR_SYSRESETREQ;

    /* Wait for reset to take effect */
    while (1) {
        __NOP();
    }
}
