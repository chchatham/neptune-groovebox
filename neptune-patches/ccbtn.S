/* Host CCs for buttons the stock firmware keeps to itself (true base 0x08008000).
 *
 * Settings (0x1A) -> CC 0x6C, Arp (0x1E) -> CC 0x6D, Scale (0x42) -> CC 0x6E,
 * status 0xB0 (channel 1), value 127 on press and 0 on release.
 *
 * Hooked in through two listener stubs that stock registers but leaves as no-ops:
 *   file 0x1A6DC (list 0x2000F024, called on PRESS   with r0 = control id)
 *   file 0x1A6E0 (list 0x2000F06C, called on RELEASE with r0 = control id)
 * Code lives in the stock "43 01" SysEx handler (file 0x29020, disabled here) and
 * its now-dead helper (file 0x1A8A8).
 */
    .syntax unified
    .equ  PORT_LIST, 0x1C     /* 0x1C = DAW port list, 0x20 = MIDI port list (measured) */
    .thumb
    .section .x_send, "ax"
    .thumb_func
midi_send:                            /* file 0x0BF6E: (port list, status, d1, d2) */

    .section .h43, "ax"               /* file 0x29020, max 50 bytes */
    .thumb_func
h43_disabled:                         /* was: 43 01 scheduled-reboot prompt */
    movs  r0, #0
    bx    lr
    .global btn_press
    .thumb_func
btn_press:
    cmp   r0, #0x10
    bhs   0f
    b.w   pad_at                      /* pads 0x00-0x0F: enable aftertouch for this pad */
0:  movs  r1, #127
    b     1f
    .global btn_release
    .thumb_func
btn_release:
    cmp   r0, #0x10
    bhs   0f
    b.w   pad_release                 /* pads: hand the LED back to the host colour */
0:  movs  r1, #0
1:  movs  r2, #0x6C
    cmp   r0, #0x1A
    beq   2f
    movs  r2, #0x6D
    cmp   r0, #0x1E
    beq   2f
    movs  r2, #0x6E
    cmp   r0, #0x42
    beq   2f
    bx    lr
2:  b.w   send_cc

    .section .snd, "ax"               /* file 0x1A8A8, max 36 bytes */
    .thumb_func
send_cc:                              /* r2 = CC number, r1 = value */
    push  {r4, r5, r6, lr}
    mov   r5, r2
    mov   r6, r1
    ldr   r4, =0x2000EE24
    ldr   r0, [r4, #PORT_LIST]        /* 0x1C and 0x20 are the two USB port lists; see docs/launchkey-mk4-firmware-map.md 0.6 */
    bl    3f
    pop   {r4, r5, r6, pc}
3:  movs  r1, #0xB0
    mov   r2, r5
    mov   r3, r6
    b.w   midi_send
    .ltorg

/* Pads in the DAW layout: aftertouch + pressure rainbow.
 *
 * Aftertouch: a control sends aftertouch only if byte 0x10 of its 0x14-byte descriptor
 * (0x20002D1C + id*0x14) has high nibble 1. The Drum layout sets it (10 40), the DAW layout leaves
 * 00 00. On every pad press, if the pad's host mapping is a DAW note (class byte 0xE0 in
 * 0x20008170[id]), write 10 40 there. On/off and channel-vs-poly still follow feature 0x6C.
 *
 * Rainbow: while a DAW pad is held its LED follows the pressure through red, orange, yellow, green,
 * blue, indigo, violet (pressure is the 16-bit value the firmware passes to value listeners;
 * index = value*7 >> 16). Red on press. On release, or when the value returns to 0, the LED goes
 * back to the host's colour.  led_set(id, 0xRRGGBB) = file 0x22612; led_mode(id, 3) = file 0x13C92
 * selects the host colour again.
 */
    .section .x_ledset, "ax"
    .thumb_func
led_set:
    .section .x_ledmode, "ax"
    .thumb_func
led_mode:
    .section .x_origval, "ax"
    .thumb_func
orig_value:                           /* stock pressure listener at file 0xDE74 (generates aftertouch) */

    .section .pat, "ax"               /* file 0x1A794, max 116 bytes (dead 43 01 prompt UI) */
    .global daw_pad
    .thumb_func
daw_pad:                              /* r0 = id; Z set if it is a DAW-layout pad. Clobbers r2, r3 */
    ldr   r2, =0x20008170
    ldr   r3, [r2, r0, lsl #2]
    ubfx  r3, r3, #16, #8
    cmp   r3, #0xE0
    bx    lr
    .thumb_func
pad_at:                               /* press: r0 = id (< 0x10) */
    push  {r4, lr}
    bl    daw_pad
    bne   9f
    ldr   r2, =0x20002D1C
    movs  r3, #0x14
    muls  r3, r0, r3
    adds  r2, r2, r3
    movw  r3, #0x4010
    strh  r3, [r2, #0x10]
    mov.w r1, #0xFF0000               /* red at the start of a press */
    bl    led_set
9:  pop   {r4, pc}
    .global pad_release
    .thumb_func
pad_release:                          /* release: r0 = id (< 0x10) */
    push  {r4, lr}
    bl    daw_pad
    bne   9b
    movs  r1, #3
    bl    led_mode
    b     9b
    .ltorg

    .section .pat2, "ax"              /* file 0x1A80C, max 86 bytes (43 01 prompt callbacks, stubbed) */
    .global pad_value
    .thumb_func
pad_value:                            /* pressure listener (list 0x2000F048): r0 = id, r1 = 16-bit pressure */
    cmp   r0, #0x10
    bhs   8f
    push  {r4, r5, r6, lr}
    mov   r4, r1
    mov   r5, r0
    bl    daw_pad
    bne   7f
    cbz   r4, 6f
    movs  r1, #7
    muls  r1, r4, r1
    lsrs  r1, r1, #16
    adr   r2, rainbow
    ldr   r1, [r2, r1, lsl #2]
    bl    led_set
    b     7f
6:  movs  r1, #3
    bl    led_mode
7:  mov   r0, r5
    mov   r1, r4
    pop.w {r4, r5, r6, lr}
8:  b.w   orig_value                  /* always chain: the stock listener generates the aftertouch */
    .align 2
rainbow:
    .word 0xFF0000, 0xFF5000, 0xFFFF00, 0x00FF00, 0x0000FF, 0x4B0082, 0x9400D3
