/* Neptune firmware linker script — STM32F4 bare-metal
 * Flash region starts at 0x08030000 (after stock bootloader + app)
 * RAM: 64K minus 8 bytes reserved at top for bootloader stack sentinel
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08030000, LENGTH = 192K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K - 8
}

_estack = 0x2000FFF8;

SECTIONS
{
    /* Vector table must be first in flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Code */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
    } > FLASH

    /* Read-only data */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Initialized data: stored in flash, copied to RAM at startup */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    /* Zero-initialized data in RAM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    _eflash = ORIGIN(FLASH) + LENGTH(FLASH);
}
