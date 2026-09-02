/* SPDX-License-Identifier: MIT */

#ifndef USB_DWC3_HANDOFF_H
#define USB_DWC3_HANDOFF_H

#include "iodev.h"
#include "types.h"
#include "usb_dwc3.h"

#define USB_DWC3_HANDOFF_MAGIC   0x33435744314e314dULL
#define USB_DWC3_HANDOFF_VERSION 2
#define USB_DWC3_HANDOFF_READY   BIT(0)

#define USB_DWC3_HANDOFF_TX_ENDPOINT 9
#define USB_DWC3_HANDOFF_RX_ENDPOINT 8

#define USB_DWC3_HANDOFF_FDT_PROPERTY    "linux-enablement-mac,m1n1-dwc3-handoff"
#define USB_DWC3_HANDOFF_LEGACY_PROPERTY "tinyos,m1n1-dwc3-handoff"

struct usb_dwc3_handoff {
    u64 magic;
    u32 version;
    u32 size;
    u64 flags;
    u64 regs_phys;
    u64 event_buffer_phys;
    u32 event_buffer_size;
    u32 event_buffer_offset;
    u64 scratchpad_phys;
    u64 scratchpad_size;
    u64 xfer_buffer_phys;
    u64 xfer_buffer_size;
    u64 trb_buffer_phys;
    u64 trb_buffer_size;
    u64 tx_buffer_phys;
    u64 tx_buffer_iova;
    u64 tx_trb_phys;
    u64 tx_trb_iova;
    u32 tx_endpoint;
    u32 tx_max_packet;
    u32 tx_busy;
    u32 stage;
    u64 rx_buffer_phys;
    u64 rx_buffer_iova;
    u64 rx_trb_phys;
    u64 rx_trb_iova;
    u64 wdt_regs_phys;
    u32 rx_endpoint;
    u32 rx_busy;
} PACKED;

enum usb_dwc3_handoff_result {
    USB_DWC3_HANDOFF_ABSENT,
    USB_DWC3_HANDOFF_ADOPTED,
    USB_DWC3_HANDOFF_INVALID,
};

enum usb_dwc3_handoff_result usb_dwc3_handoff_adopt(const u64 *entry_args, size_t entry_arg_count);
bool usb_dwc3_handoff_supported(void);
int usb_dwc3_handoff_export(dwc3_dev_t *dev);
bool usb_dwc3_handoff_active(void);
bool usb_dwc3_handoff_console_healthy(void);
bool usb_dwc3_handoff_owns_controller(u32 index);
bool usb_dwc3_handoff_is_iodev(iodev_id_t id);
int usb_dwc3_handoff_protect_heap(void);
void usb_dwc3_handoff_prepare_next_stage(void);
int usb_dwc3_handoff_publish_fdt(void *fdt, int chosen);

#endif
