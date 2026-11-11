SECTIONS {
  .x_send    0x08013F6E (NOLOAD) : { *(.x_send) }
  .x_origval 0x08015E74 (NOLOAD) : { *(.x_origval) }
  .x_ledmode 0x0801BC92 (NOLOAD) : { *(.x_ledmode) }
  .pat       0x08022794 : { *(.pat) }
  .pat2      0x0802280C : { *(.pat2) }
  .snd       0x080228A8 : { *(.snd) }
  .x_ledset  0x0802A612 (NOLOAD) : { *(.x_ledset) }
  .h43       0x08031020 : { *(.h43) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
