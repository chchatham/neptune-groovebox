/*
 * usb_midi.c — USB MIDI device (Phase 7.1 / 7.2 stub)
 *
 * Implements bare-metal USB OTG FS device mode on STM32F401.
 * Handles enumeration (SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION).
 *
 * Descriptors match the stock Launchkey Mini MK4 layout (from RE):
 *   - VID 0x1235 (Novation), PID 0x0356 (Launchkey Mini MK4)
 *   - Audio Control interface (iface 0)
 *   - MIDI Streaming interface (iface 1) with 2 virtual cables
 *   - Bulk EP1 OUT + EP1 IN for MIDI data
 *
 * We omit the HID keyboard interface (iface 2) — Neptune does not need it.
 */

#include <stdint.h>
#include "stm32f4xx.h"
#include "usb_midi.h"

/* --------------------------------------------------------------------------
 * USB Standard Request Codes
 * -------------------------------------------------------------------------- */
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09

/* Descriptor types */
#define USB_DESC_TYPE_DEVICE        0x01
#define USB_DESC_TYPE_CONFIG        0x02
#define USB_DESC_TYPE_STRING        0x03
#define USB_DESC_TYPE_INTERFACE     0x04
#define USB_DESC_TYPE_ENDPOINT      0x05
#define USB_DESC_TYPE_DEVICE_QUAL   0x06

/* --------------------------------------------------------------------------
 * Setup Packet
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */
static usb_state_t g_usb_state;
static uint8_t     g_dev_addr;          /* Pending address from SET_ADDRESS */
static uint8_t     g_configuration;     /* Current configuration value */

/* EP0 transfer state */
static const uint8_t *g_ep0_tx_ptr;
static uint16_t       g_ep0_tx_remaining;

/* Setup packet buffer (8 bytes, word-aligned) */
static uint32_t g_setup_buf[2];

/* EP0 max packet size */
#define EP0_MAX_PACKET  64

/* --------------------------------------------------------------------------
 * Descriptors
 * -------------------------------------------------------------------------- */

/* Device descriptor — 18 bytes */
static const uint8_t desc_device[18] = {
    0x12,       /* bLength */
    0x01,       /* bDescriptorType = DEVICE */
    0x00, 0x02, /* bcdUSB = 2.00 */
    0x00,       /* bDeviceClass = 0 (defined at interface level) */
    0x00,       /* bDeviceSubClass */
    0x00,       /* bDeviceProtocol */
    0x40,       /* bMaxPacketSize0 = 64 */
    0x35, 0x12, /* idVendor = 0x1235 (Novation) — little-endian */
    0x56, 0x03, /* idProduct = 0x0356 (Launchkey Mini MK4) */
    0x00, 0x02, /* bcdDevice = 0x0200 (Neptune firmware 2.00) */
    0x01,       /* iManufacturer = string 1 */
    0x02,       /* iProduct = string 2 */
    0x00,       /* iSerialNumber = 0 (none for now) */
    0x01,       /* bNumConfigurations = 1 */
};

/*
 * Configuration descriptor (total = config + AC iface + AC CS header
 *   + MS iface + MS CS header + 4 jack descs + 2 EP descs + 2 CS EP descs)
 *
 * Mirrors stock Config 1 layout from RE (minus HID interface).
 * wTotalLength = 101 bytes.
 */
static const uint8_t desc_config[] = {
    /* Configuration descriptor (9 bytes) */
    0x09,       /* bLength */
    0x02,       /* bDescriptorType = CONFIGURATION */
    101, 0x00,  /* wTotalLength = 101 */
    0x02,       /* bNumInterfaces = 2 (AudioControl + MIDIStreaming) */
    0x01,       /* bConfigurationValue = 1 */
    0x00,       /* iConfiguration = 0 */
    0x80,       /* bmAttributes = bus-powered */
    0xFA,       /* bMaxPower = 500mA */

    /* Interface 0: Audio Control (9 bytes) */
    0x09,       /* bLength */
    0x04,       /* bDescriptorType = INTERFACE */
    0x00,       /* bInterfaceNumber = 0 */
    0x00,       /* bAlternateSetting */
    0x00,       /* bNumEndpoints = 0 */
    0x01,       /* bInterfaceClass = Audio */
    0x01,       /* bInterfaceSubClass = AudioControl */
    0x00,       /* bInterfaceProtocol */
    0x00,       /* iInterface */

    /* AC CS Interface Header (9 bytes) */
    0x09,       /* bLength */
    0x24,       /* bDescriptorType = CS_INTERFACE */
    0x01,       /* bDescriptorSubtype = HEADER */
    0x00, 0x01, /* bcdADC = 1.00 */
    0x09, 0x00, /* wTotalLength = 9 */
    0x01,       /* bInCollection = 1 */
    0x01,       /* baInterfaceNr[0] = 1 (MIDI Streaming) */

    /* Interface 1: MIDI Streaming (9 bytes) */
    0x09,       /* bLength */
    0x04,       /* bDescriptorType = INTERFACE */
    0x01,       /* bInterfaceNumber = 1 */
    0x00,       /* bAlternateSetting */
    0x02,       /* bNumEndpoints = 2 */
    0x01,       /* bInterfaceClass = Audio */
    0x03,       /* bInterfaceSubClass = MIDIStreaming */
    0x00,       /* bInterfaceProtocol */
    0x00,       /* iInterface */

    /* MS CS Interface Header (7 bytes) */
    0x07,       /* bLength */
    0x24,       /* bDescriptorType = CS_INTERFACE */
    0x01,       /* bDescriptorSubtype = HEADER */
    0x00, 0x01, /* bcdMSC = 1.00 */
    0x41, 0x00, /* wTotalLength = 65 (all MS class-specific) */

    /* MIDI IN Jack: Embedded, ID=1 (Port 1 host->dev) (6 bytes) */
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,

    /* MIDI OUT Jack: Embedded, ID=7 (Port 1 dev->host) (9 bytes) */
    0x09, 0x24, 0x03, 0x01, 0x07, 0x01, 0x01, 0x01, 0x00,

    /* MIDI IN Jack: Embedded, ID=9 (Port 2 host->dev) (6 bytes) */
    0x06, 0x24, 0x02, 0x01, 0x09, 0x00,

    /* MIDI OUT Jack: Embedded, ID=15 (Port 2 dev->host) (9 bytes) */
    0x09, 0x24, 0x03, 0x01, 0x0F, 0x01, 0x09, 0x01, 0x00,

    /* EP1 OUT: Bulk, 64 bytes (9 bytes — audio class endpoint) */
    0x09,       /* bLength */
    0x05,       /* bDescriptorType = ENDPOINT */
    0x01,       /* bEndpointAddress = 0x01 (EP1 OUT) */
    0x02,       /* bmAttributes = Bulk */
    0x40, 0x00, /* wMaxPacketSize = 64 */
    0x00,       /* bInterval */
    0x00, 0x00, /* (padding for audio class) */

    /* CS Endpoint: associated IN jacks 1, 9 (6 bytes) */
    0x06, 0x25, 0x01, 0x02, 0x01, 0x09,

    /* EP1 IN: Bulk, 64 bytes (9 bytes — audio class endpoint) */
    0x09,       /* bLength */
    0x05,       /* bDescriptorType = ENDPOINT */
    0x81,       /* bEndpointAddress = 0x81 (EP1 IN) */
    0x02,       /* bmAttributes = Bulk */
    0x40, 0x00, /* wMaxPacketSize = 64 */
    0x00,       /* bInterval */
    0x00, 0x00, /* (padding for audio class) */

    /* CS Endpoint: associated OUT jacks 7, 15 (6 bytes) */
    0x06, 0x25, 0x01, 0x02, 0x07, 0x0F,
};

/* String descriptor 0: Language ID */
static const uint8_t desc_string0[] = {
    0x04,       /* bLength */
    0x03,       /* bDescriptorType = STRING */
    0x09, 0x04, /* English (US) */
};

/* Helper: ASCII string to USB string descriptor (max 31 chars + header) */
#define USB_STR_DESC_SIZE(n) (2 + 2*(n))

/* Pre-built string descriptors (UTF-16LE) */

/* String 1: "Neptune" (manufacturer) */
static const uint8_t desc_string1[] = {
    USB_STR_DESC_SIZE(7), 0x03,
    'N',0, 'e',0, 'p',0, 't',0, 'u',0, 'n',0, 'e',0,
};

/* String 2: "Launchkey Mini MK4" (product — matches stock for compatibility) */
static const uint8_t desc_string2[] = {
    USB_STR_DESC_SIZE(18), 0x03,
    'L',0, 'a',0, 'u',0, 'n',0, 'c',0, 'h',0, 'k',0, 'e',0,
    'y',0, ' ',0, 'M',0, 'i',0, 'n',0, 'i',0, ' ',0, 'M',0,
    'K',0, '4',0,
};

/* --------------------------------------------------------------------------
 * Internal: write data to EP0 IN FIFO
 * -------------------------------------------------------------------------- */
static void ep0_tx_packet(const uint8_t *data, uint16_t len)
{
    if (len > EP0_MAX_PACKET)
        len = EP0_MAX_PACKET;

    /* Program transfer size */
    OTG_DIEPTSIZ0 = OTG_DIEPTSIZ0_PKTCNT(1) | (len & OTG_DIEPTSIZ0_XFRSIZ_MASK);

    /* Enable endpoint */
    OTG_DIEPCTL0 |= OTG_EPCTL_EPENA | OTG_EPCTL_CNAK;

    /* Write data to TX FIFO (word at a time) */
    volatile uint32_t *fifo = (volatile uint32_t *)(USB_OTG_FS_BASE + 0x1000);
    uint16_t words = (len + 3) / 4;
    for (uint16_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4 && (i * 4 + b) < len; b++) {
            w |= (uint32_t)data[i * 4 + b] << (b * 8);
        }
        *fifo = w;
    }
}

/*
 * Continue an ongoing EP0 IN transfer (for multi-packet descriptors).
 */
static void ep0_tx_continue(void)
{
    if (g_ep0_tx_remaining > 0) {
        uint16_t chunk = g_ep0_tx_remaining;
        if (chunk > EP0_MAX_PACKET)
            chunk = EP0_MAX_PACKET;
        ep0_tx_packet(g_ep0_tx_ptr, chunk);
        g_ep0_tx_ptr += chunk;
        g_ep0_tx_remaining -= chunk;
    }
}

/* Send a zero-length status packet on EP0 IN */
static void ep0_tx_status(void)
{
    OTG_DIEPTSIZ0 = OTG_DIEPTSIZ0_PKTCNT(1) | 0;
    OTG_DIEPCTL0 |= OTG_EPCTL_EPENA | OTG_EPCTL_CNAK;
}

/* Prepare EP0 OUT to receive the next SETUP packet */
static void ep0_prepare_setup(void)
{
    OTG_DOEPTSIZ0 = OTG_DOEPTSIZ0_STUPCNT(3) | OTG_DOEPTSIZ0_PKTCNT | (3 * 8);
    OTG_DOEPCTL0 |= OTG_EPCTL_EPENA;
}

/* Stall EP0 */
static void ep0_stall(void)
{
    OTG_DIEPCTL0 |= OTG_EPCTL_STALL;
    OTG_DOEPCTL0 |= OTG_EPCTL_STALL;
    ep0_prepare_setup();
}

/* --------------------------------------------------------------------------
 * Setup packet handler
 * -------------------------------------------------------------------------- */
static void handle_setup(void)
{
    usb_setup_t setup;
    uint8_t *p = (uint8_t *)g_setup_buf;
    setup.bmRequestType = p[0];
    setup.bRequest      = p[1];
    setup.wValue        = (uint16_t)(p[2] | (p[3] << 8));
    setup.wIndex        = (uint16_t)(p[4] | (p[5] << 8));
    setup.wLength       = (uint16_t)(p[6] | (p[7] << 8));

    g_ep0_tx_ptr = 0;
    g_ep0_tx_remaining = 0;

    /* Standard device requests */
    if ((setup.bmRequestType & 0x60) == 0x00) {
        switch (setup.bRequest) {

        case USB_REQ_SET_ADDRESS: {
            /* Address is applied AFTER the status stage completes */
            g_dev_addr = (uint8_t)(setup.wValue & 0x7F);
            ep0_tx_status();
            /* We apply the address after the IN status packet is sent.
             * The OTG FS core applies DAD immediately, which is fine for
             * FS devices — the host does not send another packet before
             * reading our ZLP status. */
            OTG_DCFG = (OTG_DCFG & ~OTG_DCFG_DAD_MASK) | OTG_DCFG_DAD(g_dev_addr);
            if (g_dev_addr)
                g_usb_state = USB_STATE_ADDRESS;
            else
                g_usb_state = USB_STATE_DEFAULT;
            return;
        }

        case USB_REQ_GET_DESCRIPTOR: {
            uint8_t desc_type  = (uint8_t)(setup.wValue >> 8);
            uint8_t desc_index = (uint8_t)(setup.wValue & 0xFF);
            const uint8_t *desc = 0;
            uint16_t desc_len = 0;

            switch (desc_type) {
            case USB_DESC_TYPE_DEVICE:
                desc = desc_device;
                desc_len = sizeof(desc_device);
                break;

            case USB_DESC_TYPE_CONFIG:
                desc = desc_config;
                desc_len = sizeof(desc_config);
                break;

            case USB_DESC_TYPE_STRING:
                switch (desc_index) {
                case 0:
                    desc = desc_string0;
                    desc_len = sizeof(desc_string0);
                    break;
                case 1:
                    desc = desc_string1;
                    desc_len = sizeof(desc_string1);
                    break;
                case 2:
                    desc = desc_string2;
                    desc_len = sizeof(desc_string2);
                    break;
                default:
                    /* Unknown string index */
                    ep0_stall();
                    return;
                }
                break;

            case USB_DESC_TYPE_DEVICE_QUAL:
                /* Full-speed only device: stall device qualifier requests */
                ep0_stall();
                return;

            default:
                ep0_stall();
                return;
            }

            /* Clamp to requested length */
            if (desc_len > setup.wLength)
                desc_len = setup.wLength;

            /* Send first chunk, save remainder for continuation */
            uint16_t chunk = desc_len;
            if (chunk > EP0_MAX_PACKET)
                chunk = EP0_MAX_PACKET;

            ep0_tx_packet(desc, chunk);
            g_ep0_tx_ptr = desc + chunk;
            g_ep0_tx_remaining = desc_len - chunk;
            return;
        }

        case USB_REQ_SET_CONFIGURATION: {
            g_configuration = (uint8_t)(setup.wValue & 0xFF);
            if (g_configuration == 1) {
                g_usb_state = USB_STATE_CONFIGURED;

                /* Phase 7.2 TODO: activate EP1 IN/OUT for MIDI data here.
                 * For now, just ACK the request so enumeration completes. */
            } else if (g_configuration == 0) {
                g_usb_state = USB_STATE_ADDRESS;
            }
            ep0_tx_status();
            return;
        }

        case USB_REQ_GET_CONFIGURATION: {
            static uint8_t cfg_val;
            cfg_val = g_configuration;
            ep0_tx_packet(&cfg_val, 1);
            return;
        }

        case USB_REQ_GET_STATUS: {
            static uint16_t status_val;
            status_val = 0; /* Self-powered = 0, remote wakeup = 0 */
            ep0_tx_packet((const uint8_t *)&status_val, 2);
            return;
        }

        case USB_REQ_SET_FEATURE:
        case USB_REQ_CLEAR_FEATURE:
            /* Minimal: just ACK */
            ep0_tx_status();
            return;

        default:
            break;
        }
    }

    /* Unrecognized request: STALL */
    ep0_stall();
}

/* --------------------------------------------------------------------------
 * USB OTG FS initialization
 * -------------------------------------------------------------------------- */
void usb_midi_init(void)
{
    g_usb_state = USB_STATE_DEFAULT;
    g_dev_addr = 0;
    g_configuration = 0;

    /* Enable USB OTG FS clock */
    RCC_AHB2ENR |= RCC_AHB2ENR_OTGFSEN;

    /* Configure PA11 (DM) and PA12 (DP) as AF10 (USB OTG FS) */
    /* PA11 */
    GPIOA_MODER  = (GPIOA_MODER & ~(3UL << (11 * 2))) | (2UL << (11 * 2));  /* AF */
    GPIOA_OSPEEDR |= (3UL << (11 * 2));   /* Very high speed */
    GPIOA_PUPDR  &= ~(3UL << (11 * 2));   /* No pull */
    GPIOA_AFRH   = (GPIOA_AFRH & ~(0xFUL << ((11 - 8) * 4))) | (10UL << ((11 - 8) * 4));

    /* PA12 */
    GPIOA_MODER  = (GPIOA_MODER & ~(3UL << (12 * 2))) | (2UL << (12 * 2));
    GPIOA_OSPEEDR |= (3UL << (12 * 2));
    GPIOA_PUPDR  &= ~(3UL << (12 * 2));
    GPIOA_AFRH   = (GPIOA_AFRH & ~(0xFUL << ((12 - 8) * 4))) | (10UL << ((12 - 8) * 4));

    /* Wait for AHB master idle before core reset */
    while (!(OTG_GRSTCTL & OTG_GRSTCTL_AHBIDL))
        ;

    /* Core soft reset */
    OTG_GRSTCTL |= OTG_GRSTCTL_CSRST;
    while (OTG_GRSTCTL & OTG_GRSTCTL_CSRST)
        ;
    /* Small delay for PHY clock stabilization */
    for (volatile int i = 0; i < 1000; i++)
        ;

    /* Select FS embedded PHY */
    OTG_GUSBCFG |= OTG_GUSBCFG_PHYSEL;

    /* Wait for AHB idle again after reset */
    while (!(OTG_GRSTCTL & OTG_GRSTCTL_AHBIDL))
        ;

    /* Power on the USB transceiver */
    OTG_GCCFG = OTG_GCCFG_PWRDWN | OTG_GCCFG_NOVBUSSENS;

    /* Force device mode */
    OTG_GUSBCFG |= OTG_GUSBCFG_FDMOD;
    OTG_GUSBCFG = (OTG_GUSBCFG & ~OTG_GUSBCFG_TRDT_MASK) | OTG_GUSBCFG_TRDT(6);
    /* Small delay for mode change */
    for (volatile int i = 0; i < 100000; i++)
        ;

    /* Device configuration: Full-Speed */
    OTG_DCFG = (OTG_DCFG & ~(3UL)) | OTG_DCFG_DSPD_FS;

    /* Flush all TX FIFOs */
    OTG_GRSTCTL = OTG_GRSTCTL_TXFFLSH | OTG_GRSTCTL_TXFNUM(0x10);
    while (OTG_GRSTCTL & OTG_GRSTCTL_TXFFLSH)
        ;

    /* Flush RX FIFO */
    OTG_GRSTCTL = OTG_GRSTCTL_RXFFLSH;
    while (OTG_GRSTCTL & OTG_GRSTCTL_RXFFLSH)
        ;

    /* Clear all pending device interrupts */
    OTG_GINTSTS = 0xFFFFFFFFUL;
    OTG_DIEPINT0 = 0xFF;
    OTG_DOEPINT0 = 0xFF;

    /* Configure FIFO sizes (in 32-bit words)
     * RX FIFO: 128 words (512 bytes) — shared for all OUT EPs
     * TX FIFO 0 (EP0): 64 words (256 bytes)
     * TX FIFO 1 (EP1 IN): 64 words (256 bytes)
     * Total: 256 words = 1024 bytes (within the 1.25K FS FIFO) */
    OTG_GRXFSIZ  = 128;
    OTG_DIEPTXF0 = (64UL << 16) | 128;    /* Size=64, Start=128 */
    OTG_DIEPTXF1 = (64UL << 16) | 192;    /* Size=64, Start=192 */

    /* Unmask interrupts we care about (polled, not NVIC-driven for now) */
    OTG_GINTMSK = OTG_GINTSTS_USBRST
                 | OTG_GINTSTS_ENUMDNE
                 | OTG_GINTSTS_RXFLVL
                 | OTG_GINTSTS_IEPINT
                 | OTG_GINTSTS_OEPINT
                 | OTG_GINTSTS_USBSUSP
                 | OTG_GINTSTS_WKUINT;

    /* Enable global interrupt mask in GAHBCFG (needed even for polling) */
    OTG_GAHBCFG |= OTG_GAHBCFG_GINTMSK;

    /* Unmask EP0 IN/OUT interrupts */
    OTG_DAINTMSK = (1UL << 0) | (1UL << 16);  /* EP0 IN + EP0 OUT */
    OTG_DIEPMSK  = OTG_DEPINT_XFRC;           /* Transfer complete */
    OTG_DOEPMSK  = OTG_DEPINT_XFRC | OTG_DEPINT_STUP;

    /* Connect: clear soft disconnect */
    OTG_DCTL &= ~OTG_DCTL_SDIS;

    /* Prepare EP0 OUT to receive SETUP packets */
    ep0_prepare_setup();
}

/* --------------------------------------------------------------------------
 * USB Reset handler (called when host resets bus)
 * -------------------------------------------------------------------------- */
static void handle_usb_reset(void)
{
    g_usb_state = USB_STATE_DEFAULT;
    g_dev_addr = 0;
    g_configuration = 0;

    /* Clear device address */
    OTG_DCFG &= ~OTG_DCFG_DAD_MASK;

    /* Flush TX FIFOs */
    OTG_GRSTCTL = OTG_GRSTCTL_TXFFLSH | OTG_GRSTCTL_TXFNUM(0x10);
    while (OTG_GRSTCTL & OTG_GRSTCTL_TXFFLSH)
        ;

    /* Clear endpoint interrupts */
    OTG_DIEPINT0 = 0xFF;
    OTG_DOEPINT0 = 0xFF;

    /* Reset EP0 max packet size (64 for FS) */
    OTG_DIEPCTL0 = (OTG_DIEPCTL0 & ~OTG_EPCTL_MPSIZ_MASK) | 0;  /* 0 = 64 bytes */
    OTG_DOEPCTL0 = (OTG_DOEPCTL0 & ~OTG_EPCTL_MPSIZ_MASK) | 0;

    /* Re-enable EP0 OUT for SETUP reception */
    ep0_prepare_setup();
}

/* --------------------------------------------------------------------------
 * RX FIFO handler — reads received data and SETUP packets
 * -------------------------------------------------------------------------- */
static void handle_rxflvl(void)
{
    uint32_t grxstsp = OTG_GRXSTSP;
    uint32_t pktsts = grxstsp & OTG_GRXSTS_PKTSTS_MASK;
    uint32_t bcnt   = (grxstsp & OTG_GRXSTS_BCNT_MASK) >> OTG_GRXSTS_BCNT_SHIFT;
    /* uint32_t epnum  = grxstsp & OTG_GRXSTS_EPNUM_MASK; */

    if (pktsts == OTG_GRXSTS_PKTSTS_SETUP_RX) {
        /* SETUP packet received — read 8 bytes from RX FIFO */
        volatile uint32_t *fifo = (volatile uint32_t *)(USB_OTG_FS_BASE + 0x1000);
        g_setup_buf[0] = *fifo;
        g_setup_buf[1] = *fifo;
    } else if (pktsts == OTG_GRXSTS_PKTSTS_OUT_RX) {
        /* OUT data packet — read and discard for now (Phase 7.2 will use) */
        volatile uint32_t *fifo = (volatile uint32_t *)(USB_OTG_FS_BASE + 0x1000);
        uint32_t words = (bcnt + 3) / 4;
        for (uint32_t i = 0; i < words; i++) {
            (void)*fifo;  /* Read and discard */
        }
    }
    /* SETUP_DONE and OUT_DONE do not carry data — nothing to read */
}

/* --------------------------------------------------------------------------
 * Poll-based USB processing
 * -------------------------------------------------------------------------- */
int usb_midi_poll(void)
{
    uint32_t gintsts = OTG_GINTSTS & OTG_GINTMSK;

    if (gintsts == 0)
        return 0;

    /* USB Reset */
    if (gintsts & OTG_GINTSTS_USBRST) {
        handle_usb_reset();
        OTG_GINTSTS = OTG_GINTSTS_USBRST;
    }

    /* Enumeration done */
    if (gintsts & OTG_GINTSTS_ENUMDNE) {
        /* FS enumeration complete — EP0 configured for 64 bytes */
        OTG_DIEPCTL0 = (OTG_DIEPCTL0 & ~OTG_EPCTL_MPSIZ_MASK) | 0;
        OTG_GUSBCFG = (OTG_GUSBCFG & ~OTG_GUSBCFG_TRDT_MASK) | OTG_GUSBCFG_TRDT(6);
        OTG_GINTSTS = OTG_GINTSTS_ENUMDNE;
    }

    /* RX FIFO non-empty — must be handled before checking EP interrupts */
    if (gintsts & OTG_GINTSTS_RXFLVL) {
        /* Mask RXFLVL while handling to prevent re-entry */
        OTG_GINTMSK &= ~OTG_GINTSTS_RXFLVL;
        handle_rxflvl();
        OTG_GINTMSK |= OTG_GINTSTS_RXFLVL;
    }

    /* OUT endpoint interrupts */
    if (gintsts & OTG_GINTSTS_OEPINT) {
        uint32_t doepint = OTG_DOEPINT0;
        if (doepint & OTG_DEPINT_STUP) {
            /* SETUP phase done — process the setup packet */
            OTG_DOEPINT0 = OTG_DEPINT_STUP;
            handle_setup();
        }
        if (doepint & OTG_DEPINT_XFRC) {
            OTG_DOEPINT0 = OTG_DEPINT_XFRC;
            ep0_prepare_setup();
        }
    }

    /* IN endpoint interrupts */
    if (gintsts & OTG_GINTSTS_IEPINT) {
        uint32_t diepint = OTG_DIEPINT0;
        if (diepint & OTG_DEPINT_XFRC) {
            OTG_DIEPINT0 = OTG_DEPINT_XFRC;
            /* If there is more data to send (multi-packet descriptor) */
            if (g_ep0_tx_remaining > 0) {
                ep0_tx_continue();
            }
        }
    }

    /* USB Suspend */
    if (gintsts & OTG_GINTSTS_USBSUSP) {
        OTG_GINTSTS = OTG_GINTSTS_USBSUSP;
    }

    /* Wakeup */
    if (gintsts & OTG_GINTSTS_WKUINT) {
        OTG_GINTSTS = OTG_GINTSTS_WKUINT;
    }

    return 1;
}

usb_state_t usb_midi_get_state(void)
{
    return g_usb_state;
}
