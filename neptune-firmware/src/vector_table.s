    .syntax unified
    .cpu cortex-m4
    .thumb

    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object

g_pfnVectors:
    .word _estack               /* 0:  Initial SP */
    .word Reset_Handler         /* 1:  Reset */
    .word NMI_Handler           /* 2:  NMI */
    .word HardFault_Handler     /* 3:  HardFault */
    .word MemManage_Handler     /* 4:  MemManage */
    .word BusFault_Handler      /* 5:  BusFault */
    .word UsageFault_Handler    /* 6:  UsageFault */
    .word 0                     /* 7:  Reserved */
    .word 0                     /* 8:  Reserved */
    .word 0                     /* 9:  Reserved */
    .word 0                     /* 10: Reserved */
    .word SVC_Handler           /* 11: SVCall */
    .word DebugMon_Handler      /* 12: Debug Monitor */
    .word 0                     /* 13: Reserved */
    .word PendSV_Handler        /* 14: PendSV */
    .word SysTick_Handler       /* 15: SysTick */

    /* IRQs 0-24: Default_Handler */
    .rept 25
    .word Default_Handler
    .endr

    /* IRQ25 (SPI1): bootloader shared handler — stock vector table entry 41 */
    .word 0x080110DD            /* Thumb bit set, must match stock exactly */

    /* IRQs 26-27: Default_Handler */
    .word Default_Handler
    .word Default_Handler

    /* IRQ28 (USART1): bootloader shared handler — stock vector table entry 44 */
    .word 0x0800FC4D            /* Thumb bit set, must match stock exactly */

    /* IRQs 29-66: Default_Handler (38 entries) */
    .rept 38
    .word Default_Handler
    .endr

    /* IRQ67: OTG_FS — USB OTG FS global interrupt */
    .word OTG_FS_IRQHandler

    /* IRQs 68-95: Default_Handler (28 entries) */
    .rept 28
    .word Default_Handler
    .endr

    .size g_pfnVectors, .-g_pfnVectors

    /* ------------------------------------------------------------------ */
    /* Weak handler aliases — startup.c or application code can override  */
    /* ------------------------------------------------------------------ */

    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler

    /* HardFault_Handler is defined in main.c — do NOT alias to Default */

    .weak MemManage_Handler
    .thumb_set MemManage_Handler, Default_Handler

    .weak BusFault_Handler
    .thumb_set BusFault_Handler, Default_Handler

    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, Default_Handler

    .weak SVC_Handler
    .thumb_set SVC_Handler, Default_Handler

    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, Default_Handler

    .weak PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler

    /* SysTick_Handler is defined in main.c — do NOT alias to Default */

    .weak OTG_FS_IRQHandler
    .thumb_set OTG_FS_IRQHandler, Default_Handler

    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b .
    .size Default_Handler, .-Default_Handler
