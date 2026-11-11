SECTIONS {
  .x_byte  0x08013F8E (NOLOAD) : { *(.x_byte) }
  .x_begin 0x08014078 (NOLOAD) : { *(.x_begin) }
  .x_hdr   0x080282D6 (NOLOAD) : { *(.x_hdr) }
  .handler 0x08031052 : { *(.handler) }
  .cave    0x08031866 : { *(.cave) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
