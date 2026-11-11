/*
 * main.c — Neptune firmware main loop (Phase 7.1).
 *
 * - Initializes USB MIDI device
 * - Sets up SysTick at 1ms
 * - Polls USB in the main loop
 * - Checks for bootloader reentry trigger (stub)
 * - Heartbeat counter proves the loop is running
 */

#include <stdint.h>
#include "stm32f4xx.h"
#include "usb_midi.h"
#include "bootloader.h"

/* SysTick millisecond counter (incremented by SysTick_Handler) */
volatile uint32_t g_tick_ms;

/* Heartbeat counter — incremented every main-loop iteration */
volatile uint32_t g_heartbeat;

/* Flag to trigger bootloader reentry (Phase 7.3+ will set this via button combo) */
volatile uint8_t g_bootloader_request;

/*
 * SysTick interrupt handler — called every 1 ms.
 * Override the weak Default_Handler alias from vector_table.s.
 */
void SysTick_Handler(void)
{
    g_tick_ms++;
}

/*
 * HardFault handler — reboot to bootloader as a safety measure.
 * If the firmware crashes, we can still recover by re-flashing.
 */
void HardFault_Handler(void)
{
    reboot_to_bootloader();
}

/*
 * Set up SysTick for 1ms ticks.
 * At 84 MHz SYSCLK with processor clock source: reload = 84000 - 1
 */
static void SysTick_Init(void)
{
    SYSTICK_LOAD = 84000UL - 1;     /* 84 MHz / 84000 = 1 kHz */
    SYSTICK_VAL  = 0;               /* Clear current value */
    SYSTICK_CTRL = SYSTICK_CTRL_CLKSOURCE
                 | SYSTICK_CTRL_TICKINT
                 | SYSTICK_CTRL_ENABLE;
}

void main(void)
{
    /* Initialize 1ms tick timer */
    SysTick_Init();

    /* Initialize USB MIDI device (handles enumeration) */
    usb_midi_init();

    /* Main loop */
    while (1) {
        /* Poll USB (handles setup packets, enumeration) */
        usb_midi_poll();

        /* Check bootloader reentry request (stub — no input scanning yet) */
        if (g_bootloader_request) {
            reboot_to_bootloader();
        }

        /* Heartbeat: proves main loop is running */
        g_heartbeat++;
    }
}
