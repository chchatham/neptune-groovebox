/*
 * startup.c — Reset handler and clock configuration for Neptune firmware.
 *
 * Clock config replicates stock Launchkey Mini MK4 settings (from RE):
 *   HSE = 8 MHz crystal
 *   SYSCLK = 84 MHz (via PLL: /8 * 336 / 4)
 *   HCLK   = 84 MHz (AHB /1)
 *   PCLK1  = 42 MHz (APB1 /2)
 *   PCLK2  = 84 MHz (APB2 /1)
 *   USB    = 48 MHz (PLL48CLK, PLLQ = 7)
 */

#include <stdint.h>
#include "stm32f4xx.h"

/* Symbols defined by the linker script */
extern uint32_t _sidata;   /* start of .data initializers in flash */
extern uint32_t _sdata;    /* start of .data section in RAM */
extern uint32_t _edata;    /* end of .data section in RAM */
extern uint32_t _sbss;     /* start of .bss section in RAM */
extern uint32_t _ebss;     /* end of .bss section in RAM */

extern void main(void);

/*
 * SystemClock_Config — Replicate stock MK4 clock tree.
 *
 * HSE 8 MHz -> PLL -> 84 MHz SYSCLK, 48 MHz USB.
 * Direct register writes from RE disassembly at VMA 0x0803857C.
 */
static void SystemClock_Config(void)
{
    uint32_t tmp;

    /* ---- Step 0: Enable flash caches and prefetch ---- */
    FLASH_ACR |= FLASH_ACR_ICEN;       /* Instruction cache */
    FLASH_ACR |= FLASH_ACR_DCEN;       /* Data cache */
    FLASH_ACR |= FLASH_ACR_PRFTEN;     /* Prefetch */

    /* ---- Step 1: Flash wait states (2 WS for 84 MHz at 2.7-3.6V) ---- */
    tmp = FLASH_ACR;
    tmp = (tmp & ~FLASH_ACR_LATENCY_MASK) | 2UL;
    FLASH_ACR = tmp;
    (void)FLASH_ACR;    /* Dummy read for synchronization */

    /* ---- Step 2: Enable PWR clock and set voltage scaling ---- */
    RCC_APB1ENR |= RCC_APB1ENR_PWREN;
    tmp = PWR_CR;
    tmp = (tmp & ~PWR_CR_VOS_MASK) | PWR_CR_VOS_SCALE2;
    PWR_CR = tmp;

    /* ---- Step 3: Enable HSE ---- */
    RCC_CR |= RCC_CR_HSEON;
    while (!(RCC_CR & RCC_CR_HSERDY))
        ;   /* Wait for HSE ready */

    /* ---- Step 4: Configure PLL ---- */
    /*
     * PLLM = 8      (HSE/8 = 1 MHz)
     * PLLN = 336     (VCO = 336 MHz)
     * PLLP = 4       (field 1 = /4 -> SYSCLK = 84 MHz)
     * PLLQ = 7       (PLL48CLK = 48 MHz)
     * PLLSRC = HSE
     *
     * PLLCFGR = (7 << 24) | (1 << 22) | (1 << 16) | (336 << 6) | 8
     *         = 0x07415408
     */
    RCC_PLLCFGR = (RCC_PLLCFGR & 0xF0BC8000UL) | 0x07415408UL;

    /* ---- Step 5: Enable PLL ---- */
    RCC_CR |= RCC_CR_PLLON;
    while (!(RCC_CR & RCC_CR_PLLRDY))
        ;   /* Wait for PLL lock */

    /* ---- Step 6: Bus prescalers ---- */
    /* AHB /1 */
    tmp = RCC_CFGR;
    tmp &= ~(0xF0UL);          /* HPRE[7:4] = 0 */
    RCC_CFGR = tmp;

    /* APB1 /2 */
    tmp = RCC_CFGR;
    tmp = (tmp & ~(7UL << 10)) | (4UL << 10);  /* PPRE1 = 0b100 */
    RCC_CFGR = tmp;

    /* APB2 /1 */
    tmp = RCC_CFGR;
    tmp &= ~(7UL << 13);       /* PPRE2 = 0 */
    RCC_CFGR = tmp;

    /* ---- Step 7: Switch system clock to PLL ---- */
    tmp = RCC_CFGR;
    tmp = (tmp & ~3UL) | RCC_CFGR_SW_PLL;
    RCC_CFGR = tmp;
    while ((RCC_CFGR & RCC_CFGR_SWS_MASK) != (RCC_CFGR_SWS_PLL << 0))
        ;   /* Wait for PLL as system clock */

    /* ---- Step 8: SysTick uses processor clock ---- */
    SYSTICK_CTRL |= SYSTICK_CTRL_CLKSOURCE;

    /* ---- Step 9: Timer clock configuration ---- */
    RCC_DCKCFGR &= ~(1UL << 24);   /* TIMPRE = 0 (standard multiplier) */
}

/*
 * Enable GPIO clocks for all ports used by the firmware.
 */
static void GPIO_ClockEnable(void)
{
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN     /* PA11/PA12 = USB, PA8 = I2C3 SCL */
                 | RCC_AHB1ENR_GPIOBEN      /* PB13-15 = SPI2 (encoders) */
                 | RCC_AHB1ENR_GPIOCEN;     /* PC9 = I2C3 SDA */
}

void Reset_Handler(void)
{
    uint32_t *src, *dst;

    /* Copy .data initializers from flash to RAM */
    src = &_sidata;
    dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* Zero-fill .bss */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0;
    }

    /* Set vector table offset to our flash region */
    SCB_VTOR = 0x08030000UL;

    /* Configure clocks (84 MHz from HSE + PLL) */
    SystemClock_Config();

    /* Enable GPIO peripheral clocks */
    GPIO_ClockEnable();

    /* Jump to application */
    main();

    /* Should never return */
    while (1) {
        __asm volatile("nop");
    }
}

void Default_Handler(void) __attribute__((weak));
void Default_Handler(void)
{
    while (1) {
        __asm volatile("nop");
    }
}
