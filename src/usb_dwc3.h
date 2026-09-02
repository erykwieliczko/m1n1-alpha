/* SPDX-License-Identifier: MIT */

#ifndef USB_DWC3_H
#define USB_DWC3_H

#include "dart.h"
#include "types.h"

typedef struct dwc3_dev dwc3_dev_t;

typedef enum _cdc_acm_pipe_id_t {
    CDC_ACM_PIPE_0,
    CDC_ACM_PIPE_1,
    CDC_ACM_PIPE_MAX
} cdc_acm_pipe_id_t;

#define USB_DWC3_MAX_RETAINED_PAGE_TABLES 16

struct usb_dwc3_export_state {
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
    u64 rx_buffer_phys;
    u64 rx_buffer_iova;
    u64 rx_trb_phys;
    u64 rx_trb_iova;
    u32 rx_endpoint;
    u32 rx_busy;
    void *page_tables[USB_DWC3_MAX_RETAINED_PAGE_TABLES];
    size_t page_table_count;
};

dwc3_dev_t *usb_dwc3_init(uintptr_t regs, dart_dev_t *dart);
void usb_dwc3_shutdown(dwc3_dev_t *dev);

void usb_dwc3_handle_events(dwc3_dev_t *dev);

ssize_t usb_dwc3_can_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc3_can_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);

u8 usb_dwc3_getbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc3_putbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);

size_t usb_dwc3_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc3_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc3_queue(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc3_flush(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
int usb_dwc3_export_state(dwc3_dev_t *dev, struct usb_dwc3_export_state *state);

#endif
