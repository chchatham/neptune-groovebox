/*
 * stm32f4xx.h — Minimal STM32F401 register definitions for Neptune firmware.
 *
 * No CMSIS, no HAL. Direct register access via volatile pointers.
 * Covers: RCC, GPIO, FLASH, PWR, USB OTG FS, SCB, SysTick, NVIC.
 */

#ifndef STM32F4XX_H
#define STM32F4XX_H

#include <stdint.h>

/* ========================================================================
 * Helper macros
 * ======================================================================== */

#define __IO    volatile
#define __I     volatile const
#define REG32(addr)   (*(volatile uint32_t *)(addr))

/* Compiler intrinsics */
static inline void __disable_irq(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void __enable_irq(void)  { __asm volatile ("cpsie i" ::: "memory"); }
static inline void __DSB(void)         { __asm volatile ("dsb 0xF" ::: "memory"); }
static inline void __ISB(void)         { __asm volatile ("isb 0xF" ::: "memory"); }
static inline void __WFI(void)         { __asm volatile ("wfi"); }
static inline void __NOP(void)         { __asm volatile ("nop"); }

/* ========================================================================
 * RCC — Reset and Clock Control (base 0x40023800)
 * ======================================================================== */

#define RCC_BASE            0x40023800UL

#define RCC_CR              REG32(RCC_BASE + 0x00)
#define RCC_PLLCFGR         REG32(RCC_BASE + 0x04)
#define RCC_CFGR            REG32(RCC_BASE + 0x08)
#define RCC_CIR             REG32(RCC_BASE + 0x0C)
#define RCC_AHB1RSTR        REG32(RCC_BASE + 0x10)
#define RCC_AHB2RSTR        REG32(RCC_BASE + 0x14)
#define RCC_APB1RSTR        REG32(RCC_BASE + 0x20)
#define RCC_APB2RSTR        REG32(RCC_BASE + 0x24)
#define RCC_AHB1ENR         REG32(RCC_BASE + 0x30)
#define RCC_AHB2ENR         REG32(RCC_BASE + 0x34)
#define RCC_APB1ENR         REG32(RCC_BASE + 0x40)
#define RCC_APB2ENR         REG32(RCC_BASE + 0x44)
#define RCC_DCKCFGR         REG32(RCC_BASE + 0x8C)

/* RCC_CR bits */
#define RCC_CR_HSION        (1UL << 0)
#define RCC_CR_HSIRDY       (1UL << 1)
#define RCC_CR_HSEON        (1UL << 16)
#define RCC_CR_HSERDY       (1UL << 17)
#define RCC_CR_PLLON        (1UL << 24)
#define RCC_CR_PLLRDY       (1UL << 25)

/* RCC_PLLCFGR bits */
#define RCC_PLLCFGR_PLLSRC_HSE  (1UL << 22)

/* RCC_CFGR: SW field */
#define RCC_CFGR_SW_PLL     (2UL << 0)
#define RCC_CFGR_SWS_PLL    (2UL << 2)
#define RCC_CFGR_SWS_MASK   (3UL << 2)

/* RCC_AHB1ENR bits */
#define RCC_AHB1ENR_GPIOAEN (1UL << 0)
#define RCC_AHB1ENR_GPIOBEN (1UL << 1)
#define RCC_AHB1ENR_GPIOCEN (1UL << 2)

/* RCC_AHB2ENR bits */
#define RCC_AHB2ENR_OTGFSEN (1UL << 7)

/* RCC_APB1ENR bits */
#define RCC_APB1ENR_PWREN   (1UL << 28)

/* ========================================================================
 * GPIO — General Purpose I/O
 * ======================================================================== */

#define GPIOA_BASE          0x40020000UL
#define GPIOB_BASE          0x40020400UL
#define GPIOC_BASE          0x40020800UL

/* GPIO register offsets */
#define GPIO_MODER_OFF      0x00
#define GPIO_OTYPER_OFF     0x04
#define GPIO_OSPEEDR_OFF    0x08
#define GPIO_PUPDR_OFF      0x0C
#define GPIO_IDR_OFF        0x10
#define GPIO_ODR_OFF        0x14
#define GPIO_BSRR_OFF       0x18
#define GPIO_AFRL_OFF       0x20
#define GPIO_AFRH_OFF       0x24

/* GPIOA registers */
#define GPIOA_MODER         REG32(GPIOA_BASE + GPIO_MODER_OFF)
#define GPIOA_OTYPER        REG32(GPIOA_BASE + GPIO_OTYPER_OFF)
#define GPIOA_OSPEEDR       REG32(GPIOA_BASE + GPIO_OSPEEDR_OFF)
#define GPIOA_PUPDR         REG32(GPIOA_BASE + GPIO_PUPDR_OFF)
#define GPIOA_IDR           REG32(GPIOA_BASE + GPIO_IDR_OFF)
#define GPIOA_ODR           REG32(GPIOA_BASE + GPIO_ODR_OFF)
#define GPIOA_BSRR          REG32(GPIOA_BASE + GPIO_BSRR_OFF)
#define GPIOA_AFRL          REG32(GPIOA_BASE + GPIO_AFRL_OFF)
#define GPIOA_AFRH          REG32(GPIOA_BASE + GPIO_AFRH_OFF)

/* GPIOB registers */
#define GPIOB_MODER         REG32(GPIOB_BASE + GPIO_MODER_OFF)
#define GPIOB_OTYPER        REG32(GPIOB_BASE + GPIO_OTYPER_OFF)
#define GPIOB_OSPEEDR       REG32(GPIOB_BASE + GPIO_OSPEEDR_OFF)
#define GPIOB_PUPDR         REG32(GPIOB_BASE + GPIO_PUPDR_OFF)
#define GPIOB_IDR           REG32(GPIOB_BASE + GPIO_IDR_OFF)
#define GPIOB_ODR           REG32(GPIOB_BASE + GPIO_ODR_OFF)
#define GPIOB_BSRR          REG32(GPIOB_BASE + GPIO_BSRR_OFF)
#define GPIOB_AFRL          REG32(GPIOB_BASE + GPIO_AFRL_OFF)
#define GPIOB_AFRH          REG32(GPIOB_BASE + GPIO_AFRH_OFF)

/* GPIOC registers */
#define GPIOC_MODER         REG32(GPIOC_BASE + GPIO_MODER_OFF)
#define GPIOC_OTYPER        REG32(GPIOC_BASE + GPIO_OTYPER_OFF)
#define GPIOC_OSPEEDR       REG32(GPIOC_BASE + GPIO_OSPEEDR_OFF)
#define GPIOC_PUPDR         REG32(GPIOC_BASE + GPIO_PUPDR_OFF)
#define GPIOC_IDR           REG32(GPIOC_BASE + GPIO_IDR_OFF)
#define GPIOC_ODR           REG32(GPIOC_BASE + GPIO_ODR_OFF)
#define GPIOC_BSRR          REG32(GPIOC_BASE + GPIO_BSRR_OFF)
#define GPIOC_AFRL          REG32(GPIOC_BASE + GPIO_AFRL_OFF)
#define GPIOC_AFRH          REG32(GPIOC_BASE + GPIO_AFRH_OFF)

/* ========================================================================
 * FLASH — Flash Access Control Register
 * ======================================================================== */

#define FLASH_BASE          0x40023C00UL
#define FLASH_ACR           REG32(FLASH_BASE + 0x00)

/* FLASH_ACR bits */
#define FLASH_ACR_LATENCY_MASK  0x0FUL
#define FLASH_ACR_PRFTEN    (1UL << 8)
#define FLASH_ACR_ICEN      (1UL << 9)
#define FLASH_ACR_DCEN      (1UL << 10)

/* ========================================================================
 * PWR — Power Control
 * ======================================================================== */

#define PWR_BASE            0x40007000UL
#define PWR_CR              REG32(PWR_BASE + 0x00)
#define PWR_CSR             REG32(PWR_BASE + 0x04)

/* PWR_CR: VOS field [15:14] */
#define PWR_CR_VOS_SCALE2   (2UL << 14)
#define PWR_CR_VOS_MASK     (3UL << 14)

/* ========================================================================
 * USB OTG FS — Full-Speed USB On-The-Go (base 0x50000000)
 * ======================================================================== */

#define USB_OTG_FS_BASE     0x50000000UL

/* Global registers (offset from base) */
#define OTG_GOTGCTL         REG32(USB_OTG_FS_BASE + 0x0000)
#define OTG_GOTGINT         REG32(USB_OTG_FS_BASE + 0x0004)
#define OTG_GAHBCFG         REG32(USB_OTG_FS_BASE + 0x0008)
#define OTG_GUSBCFG         REG32(USB_OTG_FS_BASE + 0x000C)
#define OTG_GRSTCTL         REG32(USB_OTG_FS_BASE + 0x0010)
#define OTG_GINTSTS         REG32(USB_OTG_FS_BASE + 0x0014)
#define OTG_GINTMSK         REG32(USB_OTG_FS_BASE + 0x0018)
#define OTG_GRXSTSR         REG32(USB_OTG_FS_BASE + 0x001C)
#define OTG_GRXSTSP         REG32(USB_OTG_FS_BASE + 0x0020)
#define OTG_GRXFSIZ         REG32(USB_OTG_FS_BASE + 0x0024)
#define OTG_DIEPTXF0        REG32(USB_OTG_FS_BASE + 0x0028)
#define OTG_GCCFG           REG32(USB_OTG_FS_BASE + 0x0038)
#define OTG_CID             REG32(USB_OTG_FS_BASE + 0x003C)
#define OTG_DIEPTXF1        REG32(USB_OTG_FS_BASE + 0x0104)

/* Device-mode registers */
#define OTG_DCFG            REG32(USB_OTG_FS_BASE + 0x0800)
#define OTG_DCTL            REG32(USB_OTG_FS_BASE + 0x0804)
#define OTG_DSTS            REG32(USB_OTG_FS_BASE + 0x0808)
#define OTG_DIEPMSK         REG32(USB_OTG_FS_BASE + 0x0810)
#define OTG_DOEPMSK         REG32(USB_OTG_FS_BASE + 0x0814)
#define OTG_DAINT           REG32(USB_OTG_FS_BASE + 0x0818)
#define OTG_DAINTMSK        REG32(USB_OTG_FS_BASE + 0x081C)

/* Device IN endpoint 0 */
#define OTG_DIEPCTL0        REG32(USB_OTG_FS_BASE + 0x0900)
#define OTG_DIEPINT0        REG32(USB_OTG_FS_BASE + 0x0908)
#define OTG_DIEPTSIZ0       REG32(USB_OTG_FS_BASE + 0x0910)
#define OTG_DTXFSTS0        REG32(USB_OTG_FS_BASE + 0x0918)

/* Device IN endpoint 1 */
#define OTG_DIEPCTL1        REG32(USB_OTG_FS_BASE + 0x0920)
#define OTG_DIEPINT1        REG32(USB_OTG_FS_BASE + 0x0928)
#define OTG_DIEPTSIZ1       REG32(USB_OTG_FS_BASE + 0x0930)
#define OTG_DTXFSTS1        REG32(USB_OTG_FS_BASE + 0x0938)

/* Device OUT endpoint 0 */
#define OTG_DOEPCTL0        REG32(USB_OTG_FS_BASE + 0x0B00)
#define OTG_DOEPINT0        REG32(USB_OTG_FS_BASE + 0x0B08)
#define OTG_DOEPTSIZ0       REG32(USB_OTG_FS_BASE + 0x0B10)

/* Device OUT endpoint 1 */
#define OTG_DOEPCTL1        REG32(USB_OTG_FS_BASE + 0x0B20)
#define OTG_DOEPINT1        REG32(USB_OTG_FS_BASE + 0x0B28)
#define OTG_DOEPTSIZ1       REG32(USB_OTG_FS_BASE + 0x0B30)

/* FIFO access (word-aligned, each EP has a 4K region) */
#define OTG_FIFO(ep)        REG32(USB_OTG_FS_BASE + 0x1000 + ((ep) * 0x1000))

/* Power and clock gating */
#define OTG_PCGCCTL         REG32(USB_OTG_FS_BASE + 0x0E00)

/* GAHBCFG bits */
#define OTG_GAHBCFG_GINTMSK    (1UL << 0)
#define OTG_GAHBCFG_TXFELVL    (1UL << 7)

/* GUSBCFG bits */
#define OTG_GUSBCFG_FDMOD       (1UL << 30)  /* Force device mode */
#define OTG_GUSBCFG_PHYSEL      (1UL << 6)   /* FS PHY selection */
#define OTG_GUSBCFG_TRDT_MASK   (0xFUL << 10)
#define OTG_GUSBCFG_TRDT(n)     (((n) & 0xF) << 10)

/* GRSTCTL bits */
#define OTG_GRSTCTL_CSRST       (1UL << 0)   /* Core soft reset */
#define OTG_GRSTCTL_AHBIDL      (1UL << 31)  /* AHB master idle */
#define OTG_GRSTCTL_TXFFLSH     (1UL << 5)   /* TX FIFO flush */
#define OTG_GRSTCTL_RXFFLSH     (1UL << 4)   /* RX FIFO flush */
#define OTG_GRSTCTL_TXFNUM(n)   (((n) & 0x1F) << 6)

/* GINTSTS / GINTMSK bits */
#define OTG_GINTSTS_CMOD        (1UL << 0)   /* Current mode (0=device) */
#define OTG_GINTSTS_MMIS        (1UL << 1)   /* Mode mismatch */
#define OTG_GINTSTS_OTGINT      (1UL << 2)
#define OTG_GINTSTS_SOF         (1UL << 3)
#define OTG_GINTSTS_RXFLVL      (1UL << 4)   /* RX FIFO non-empty */
#define OTG_GINTSTS_USBSUSP     (1UL << 11)  /* USB suspend */
#define OTG_GINTSTS_USBRST      (1UL << 12)  /* USB reset */
#define OTG_GINTSTS_ENUMDNE     (1UL << 13)  /* Enumeration done */
#define OTG_GINTSTS_IEPINT      (1UL << 18)  /* IN endpoint interrupt */
#define OTG_GINTSTS_OEPINT      (1UL << 19)  /* OUT endpoint interrupt */
#define OTG_GINTSTS_IISOIXFR    (1UL << 20)
#define OTG_GINTSTS_INCOMPISOOUT (1UL << 21)
#define OTG_GINTSTS_SRQINT      (1UL << 30)
#define OTG_GINTSTS_WKUINT      (1UL << 31)

/* GCCFG bits */
#define OTG_GCCFG_PWRDWN        (1UL << 16)  /* Power down deactivated */
#define OTG_GCCFG_VBUSBSEN      (1UL << 19)  /* VBUS sensing B enable */
#define OTG_GCCFG_NOVBUSSENS    (1UL << 21)  /* No VBUS sensing */

/* DCFG bits */
#define OTG_DCFG_DSPD_FS        (3UL << 0)   /* Device speed: Full-Speed */
#define OTG_DCFG_DAD_MASK       (0x7FUL << 4)
#define OTG_DCFG_DAD(n)         (((n) & 0x7F) << 4)

/* DCTL bits */
#define OTG_DCTL_SDIS           (1UL << 1)   /* Soft disconnect */
#define OTG_DCTL_RWUSIG         (1UL << 0)   /* Remote wakeup signaling */

/* DIEPCTL / DOEPCTL bits (shared layout) */
#define OTG_EPCTL_EPENA         (1UL << 31)  /* Endpoint enable */
#define OTG_EPCTL_EPDIS         (1UL << 30)  /* Endpoint disable */
#define OTG_EPCTL_SNAK          (1UL << 27)  /* Set NAK */
#define OTG_EPCTL_CNAK          (1UL << 26)  /* Clear NAK */
#define OTG_EPCTL_STALL         (1UL << 21)  /* Stall */
#define OTG_EPCTL_MPSIZ_MASK    0x7FFUL      /* Max packet size (EP0: [1:0]) */
#define OTG_EPCTL_USBAEP        (1UL << 15)  /* USB active endpoint */
#define OTG_EPCTL_EPTYP_MASK    (3UL << 18)
#define OTG_EPCTL_EPTYP_BULK    (2UL << 18)
#define OTG_EPCTL_SD0PID        (1UL << 28)  /* Set DATA0 PID */

/* DOEPTSIZ0 bits */
#define OTG_DOEPTSIZ0_STUPCNT(n)  (((n) & 3) << 29)
#define OTG_DOEPTSIZ0_PKTCNT     (1UL << 19)
#define OTG_DOEPTSIZ0_XFRSIZ_MASK 0x7FUL

/* DIEPTSIZ0 bits */
#define OTG_DIEPTSIZ0_PKTCNT(n)  (((n) & 3) << 19)
#define OTG_DIEPTSIZ0_XFRSIZ_MASK 0x7FUL

/* DIEPINT / DOEPINT bits */
#define OTG_DEPINT_XFRC         (1UL << 0)   /* Transfer complete */
#define OTG_DEPINT_EPDISD       (1UL << 1)
#define OTG_DEPINT_TOC          (1UL << 3)   /* Timeout condition */
#define OTG_DEPINT_ITTXFE       (1UL << 4)
#define OTG_DEPINT_INEPNE       (1UL << 6)
#define OTG_DEPINT_STUP         (1UL << 3)   /* SETUP phase done (OUT EP0) */

/* GRXSTSP packet status values */
#define OTG_GRXSTS_PKTSTS_MASK      (0xFUL << 17)
#define OTG_GRXSTS_PKTSTS_SETUP_RX  (0x6UL << 17)
#define OTG_GRXSTS_PKTSTS_SETUP_DONE (0x4UL << 17)
#define OTG_GRXSTS_PKTSTS_OUT_RX   (0x2UL << 17)
#define OTG_GRXSTS_PKTSTS_OUT_DONE (0x3UL << 17)
#define OTG_GRXSTS_BCNT_MASK       (0x7FFUL << 4)
#define OTG_GRXSTS_BCNT_SHIFT      4
#define OTG_GRXSTS_EPNUM_MASK      0xFUL

/* ========================================================================
 * SCB — System Control Block
 * ======================================================================== */

#define SCB_BASE            0xE000ED00UL
#define SCB_CPUID           REG32(SCB_BASE + 0x00)
#define SCB_ICSR            REG32(SCB_BASE + 0x04)
#define SCB_VTOR            REG32(SCB_BASE + 0x08)
#define SCB_AIRCR           REG32(SCB_BASE + 0x0C)
#define SCB_SCR             REG32(SCB_BASE + 0x10)

/* SCB_AIRCR bits */
#define SCB_AIRCR_VECTKEY   (0x05FAUL << 16)
#define SCB_AIRCR_SYSRESETREQ (1UL << 2)

/* ========================================================================
 * SysTick — System Timer
 * ======================================================================== */

#define SYSTICK_BASE        0xE000E010UL
#define SYSTICK_CTRL        REG32(SYSTICK_BASE + 0x00)
#define SYSTICK_LOAD        REG32(SYSTICK_BASE + 0x04)
#define SYSTICK_VAL         REG32(SYSTICK_BASE + 0x08)
#define SYSTICK_CALIB       REG32(SYSTICK_BASE + 0x0C)

/* SYSTICK_CTRL bits */
#define SYSTICK_CTRL_ENABLE     (1UL << 0)
#define SYSTICK_CTRL_TICKINT    (1UL << 1)
#define SYSTICK_CTRL_CLKSOURCE  (1UL << 2)   /* 1 = processor clock */
#define SYSTICK_CTRL_COUNTFLAG  (1UL << 16)

/* ========================================================================
 * NVIC — Nested Vectored Interrupt Controller
 * ======================================================================== */

#define NVIC_BASE           0xE000E100UL
#define NVIC_ISER(n)        REG32(NVIC_BASE + 0x000 + 4*(n))  /* Set-enable */
#define NVIC_ICER(n)        REG32(NVIC_BASE + 0x080 + 4*(n))  /* Clear-enable */
#define NVIC_ISPR(n)        REG32(NVIC_BASE + 0x100 + 4*(n))  /* Set-pending */
#define NVIC_ICPR(n)        REG32(NVIC_BASE + 0x180 + 4*(n))  /* Clear-pending */
#define NVIC_IPR(n)         REG32(0xE000E400UL + 4*(n))        /* Priority */

/* IRQ numbers relevant to Neptune */
#define IRQ_OTG_FS          67   /* USB OTG FS global interrupt */

/* ========================================================================
 * Convenience: NVIC enable/disable for a specific IRQ
 * ======================================================================== */

static inline void NVIC_EnableIRQ(uint32_t irq)
{
    NVIC_ISER(irq / 32) = (1UL << (irq % 32));
}

static inline void NVIC_DisableIRQ(uint32_t irq)
{
    NVIC_ICER(irq / 32) = (1UL << (irq % 32));
}

static inline void NVIC_SetPriority(uint32_t irq, uint32_t priority)
{
    /* STM32F4 uses 4 priority bits (top nibble of byte) */
    volatile uint8_t *ipr = (volatile uint8_t *)(0xE000E400UL + irq);
    *ipr = (uint8_t)((priority & 0xF) << 4);
}

#endif /* STM32F4XX_H */
