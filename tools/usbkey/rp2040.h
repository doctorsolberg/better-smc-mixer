/*
 * The RP2040 registers the USB key firmware uses, from the RP2040 datasheet. Our own definitions:
 * the firmware does not use the Raspberry Pi Pico SDK (SPEC D-1).
 */
#ifndef RP2040_H
#define RP2040_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
/* Atomic aliases of peripheral registers (not SIO): +0x2000 sets bits, +0x3000 clears them. */
#define REG_SET(a) REG((a) + 0x2000u)
#define REG_CLR(a) REG((a) + 0x3000u)

/* Resets */
#define RESETS_RESET          0x4000c000u
#define RESETS_RESET_DONE     0x4000c008u
#define RESET_IO_BANK0        (1u << 5)
#define RESET_PADS_BANK0      (1u << 8)
#define RESET_PLL_USB         (1u << 13)
#define RESET_TIMER           (1u << 21)
#define RESET_USBCTRL         (1u << 24)

/* Crystal oscillator (12 MHz on the Pico and the RP2040-Zero) */
#define XOSC_CTRL             0x40024000u
#define XOSC_STATUS           0x40024004u
#define XOSC_STARTUP          0x4002400cu
#define XOSC_CTRL_1_15MHZ     0xaa0u
#define XOSC_CTRL_ENABLE      (0xfabu << 12)
#define XOSC_STATUS_STABLE    (1u << 31)

/* Clock generators: CTRL, DIV, SELECTED per clock */
#define CLK_REF_CTRL          0x40008030u
#define CLK_REF_SELECTED      0x40008038u
#define CLK_SYS_CTRL          0x4000803cu
#define CLK_SYS_SELECTED      0x40008044u
#define CLK_USB_CTRL          0x40008054u
#define CLK_USB_DIV           0x40008058u
#define CLK_REF_SRC_XOSC      2u
#define CLK_SYS_SRC_AUX       1u
#define CLK_SYS_AUXSRC_PLL_USB (1u << 5)
#define CLK_USB_ENABLE        (1u << 11)
#define CLK_USB_AUXSRC_PLL_USB (0u << 5)

/* USB PLL */
#define PLL_USB_CS            0x4002c000u
#define PLL_USB_PWR           0x4002c004u
#define PLL_USB_FBDIV_INT     0x4002c008u
#define PLL_USB_PRIM          0x4002c00cu
#define PLL_CS_LOCK           (1u << 31)
#define PLL_PWR_PD            (1u << 0)
#define PLL_PWR_POSTDIVPD     (1u << 3)
#define PLL_PWR_VCOPD         (1u << 5)

/* Watchdog tick (drives the timer) and the microsecond timer */
#define WATCHDOG_TICK         0x4005802cu
#define WATCHDOG_TICK_ENABLE  (1u << 9)
#define TIMER_TIMERAWL        0x40054028u

/* GPIO */
#define IO_BANK0_CTRL(n)      (0x40014004u + 8u * (n))
#define GPIO_FUNC_SIO         5u
#define PADS_BANK0_GPIO(n)    (0x4001c004u + 4u * (n))
#define PAD_SCHMITT           (1u << 1)
#define PAD_PDE               (1u << 2)
#define PAD_PUE               (1u << 3)
#define PAD_DRIVE_4MA         (1u << 4)
#define PAD_IE                (1u << 6)
#define PAD_OD                (1u << 7)
#define IO_QSPI_SS_CTRL       0x4001800cu
#define QSPI_SS_OEOVER_MASK   (3u << 12)
#define QSPI_SS_OEOVER_DISABLE (2u << 12)

/* Single-cycle IO: GPIO in/out/output enable, with their own set and clear registers */
#define SIO_GPIO_IN           0xd0000004u
#define SIO_GPIO_HI_IN        0xd0000008u
#define SIO_GPIO_OUT_SET      0xd0000014u
#define SIO_GPIO_OUT_CLR      0xd0000018u
#define SIO_GPIO_OUT_XOR      0xd000001cu
#define SIO_GPIO_OE_SET       0xd0000024u
#define SIO_GPIO_OE_CLR       0xd0000028u

/* USB controller registers */
#define USB_ADDR_ENDP         0x50110000u
#define USB_MAIN_CTRL         0x50110040u
#define USB_SIE_CTRL          0x5011004cu
#define USB_SIE_STATUS        0x50110050u
#define USB_BUFF_STATUS       0x50110058u
#define USB_EP_STALL_ARM      0x50110068u
#define USB_MUXING            0x50110074u
#define USB_PWR               0x50110078u
#define USB_MAIN_CTRL_CONTROLLER_EN  (1u << 0)
#define USB_SIE_CTRL_PULLUP_EN       (1u << 16)
#define USB_SIE_CTRL_EP0_INT_1BUF    (1u << 29)
#define USB_SIE_STATUS_SETUP_REC     (1u << 17)
#define USB_SIE_STATUS_BUS_RESET     (1u << 19)
#define USB_MUXING_TO_PHY            (1u << 0)
#define USB_MUXING_SOFTCON           (1u << 3)
#define USB_PWR_VBUS_DETECT          (1u << 2)
#define USB_PWR_VBUS_DETECT_OVERRIDE_EN (1u << 3)

/* USB controller memory (4 KB): setup packet, endpoint and buffer control, data buffers */
#define USB_DPRAM             0x50100000u
#define USB_DPRAM_SIZE        0x1000u
#define USB_EP_CTRL(n, in)    (USB_DPRAM + 8u * (n) + ((in) ? 0u : 4u))  /* n >= 1 */
#define USB_BUF_CTRL(n, in)   (USB_DPRAM + 0x80u + 8u * (n) + ((in) ? 0u : 4u))
#define USB_EP0_BUF           0x100u   /* offsets in the DPRAM */
#define USB_DATA_BUF_START    0x180u
#define EP_CTRL_ENABLE        (1u << 31)
#define EP_CTRL_INT_PER_BUFF  (1u << 29)
#define EP_CTRL_TYPE_SHIFT    26
#define BUF_CTRL_FULL         (1u << 15)
#define BUF_CTRL_DATA1        (1u << 13)
#define BUF_CTRL_STALL        (1u << 11)
#define BUF_CTRL_AVAILABLE    (1u << 10)
#define BUF_CTRL_LEN_MASK     0x3ffu

/* Cortex-M0+ */
#define PPB_VTOR              0xe000ed08u

#endif /* RP2040_H */
