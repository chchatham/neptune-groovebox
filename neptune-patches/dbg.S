/* Neptune debug command for patched stock MK4 firmware (true base 0x08008000).
 *
 *   Host -> device:  F0 00 20 29 00 7D <addr:5> <a0:5> <a1:5> F7
 *   Device -> host:  F0 00 20 29 00 7D <r0:5> F7
 *
 * Each 32-bit value is 5 bytes of 7 bits, most significant first.
 * The device calls addr(a0, a1) (addr must have the Thumb bit set) and returns r0.
 * Replaces the stock "43 00" handler (which erased external flash and rebooted)
 * and reuses that handler's now-dead erase routine as the code cave.
 */
    .syntax unified
    .thumb
    /* Existing firmware functions: empty labels placed at their addresses by dbg.ld */
    .section .x_hdr, "ax"
    .thumb_func
hdr_check:                           /* file 0x202D6: validates 00 20 29 header   */
    .section .x_begin, "ax"
    .thumb_func
sx_begin:                            /* file 0x0C078: emit F0 00 20 29 on ctx     */
    .section .x_byte, "ax"
    .thumb_func
sx_byte:                             /* file 0x0BF8E: emit one byte (ctx, byte)   */

    .section .handler, "ax"          /* file 0x29052, max 44 bytes */
    .thumb_func
handler:                             /* r0 = reply ctx, r1 = data (F0 first), r2 = len */
    push  {r4, r5, r6, lr}
    mov   r6, r0
    mov   r4, r1
    cmp   r2, #22
    blo   1f
    mov   r0, r4
    bl    hdr_check
    cbz   r0, 1f
    ldrb  r0, [r4, #5]
    cmp   r0, #0x7D
    bne   1f
    mov   r0, r4
    mov   r1, r6
    bl    cave
    movs  r0, #1
    pop   {r4, r5, r6, pc}
1:  movs  r0, #0
    pop   {r4, r5, r6, pc}

    .section .cave, "ax"             /* file 0x29866, max 94 bytes */
    .thumb_func
cave:                                /* r0 = data, r1 = reply ctx */
    push  {r4, r5, r6, r7, lr}
    mov   r6, r1
    adds  r4, r0, #6
    movs  r5, #3
2:  movs  r1, #0
    movs  r3, #5
3:  ldrb  r2, [r4], #1
    orr   r1, r2, r1, lsl #7
    subs  r3, #1
    bne   3b
    push  {r1}
    subs  r5, #1
    bne   2b
    pop   {r1, r2, r3}               /* r1 = a1, r2 = a0, r3 = addr */
    mov   r0, r2
    blx   r3
    mov   r4, r0
    mov   r0, r6
    bl    sx_begin
    movs  r1, #0
    bl    emit
    movs  r1, #0x7D
    bl    emit
    movs  r5, #28
4:  lsr   r1, r4, r5
    and   r1, r1, #0x7F
    bl    emit
    subs  r5, #7
    bpl   4b
    movs  r1, #0xF7
    bl    emit
    pop   {r4, r5, r6, r7, pc}
emit:
    mov   r0, r6
    b     sx_byte
