/*
 * usb_midi.h — USB MIDI device for Launchkey Mini MK4 Neptune firmware.
 *
 * Phase 7.1: Initialize USB OTG FS peripheral and handle basic enumeration.
 * Phase 7.2 will add full MIDI streaming.
 *
 * VID/PID from RE: Novation 0x1235 / Launchkey Mini MK4 0x0356
 */

#ifndef USB_MIDI_H
#define USB_MIDI_H

#include <stdint.h>

/* USB device state */
typedef enum {
    USB_STATE_DEFAULT = 0,
    USB_STATE_ADDRESS,
    USB_STATE_CONFIGURED,
    USB_STATE_SUSPENDED
} usb_state_t;

/*
 * Initialize USB OTG FS peripheral in device mode.
 * Configures the PHY, FIFOs, and enables the interrupt.
 */
void usb_midi_init(void);

/*
 * Poll-based USB handler. Call from main loop.
 * Processes USB interrupts (reset, enumeration, setup packets).
 * Returns nonzero if any event was processed.
 */
int usb_midi_poll(void);

/*
 * Get current USB device state.
 */
usb_state_t usb_midi_get_state(void);

#endif /* USB_MIDI_H */
