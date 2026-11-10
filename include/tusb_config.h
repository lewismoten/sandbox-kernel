/* SPDX-License-Identifier: MIT */
#pragma once
#define CFG_TUSB_MCU OPT_MCU_BCM2835
#define CFG_TUSB_OS OPT_OS_NONE
#ifdef USB_ROLE_HOST
/* USB_ROLE=host: the one port is a host: HID keyboards, one hub, PIO. Devices behind the hub are the receiver and up to two keyboards. */
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED 0
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED
#define CFG_TUH_DWC2_DMA_ENABLE 0
#define CFG_TUH_DWC2_SLAVE_ENABLE 1
#define CFG_TUH_HUB 1 /* one hub (tinyusb counts it apart from the devices) */
#define CFG_TUH_HID 8 /* R2: interfaces, not devices: a dongle may list mouse, consumer and keyboard in any order */
#define CFG_TUH_DEVICE_MAX 4 /* keyboard, keyboard, a receiver, one spare; the hub is extra */
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_ENDPOINT_MAX 16
/* TinyUSB 0.21's dwc2_bcm.h has the device interrupt switch only; hcd_dwc2.c wants this one. */
#define dwc2_int_set(_rhport, _role, _on) ((_on) ? BP_EnableIRQ(_dwc2_controller[_rhport].irqnum) : BP_DisableIRQ(_dwc2_controller[_rhport].irqnum))
#else
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#define CFG_TUD_DWC2_DMA_ENABLE 0
#endif
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 2048
#define CFG_TUD_CDC_TX_BUFSIZE 4096
#define CFG_TUD_CDC_EP_BUFSIZE 512
#define CFG_TUD_TASK_QUEUE_SZ 32
#define CFG_TUSB_DEBUG 0
