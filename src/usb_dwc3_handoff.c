/* SPDX-License-Identifier: MIT */

#include <assert.h>

#include "usb_dwc3_handoff.h"
#include "adt.h"
#include "heapblock.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "usb_dwc3_regs.h"
#include "utils.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"

#define HANDOFF_DESCRIPTOR_ALIGNMENT SZ_16K
#define HANDOFF_BUFFER_ALIGNMENT     SZ_16K
#define HANDOFF_TRB_ALIGNMENT        16
#define HANDOFF_TX_CHUNK             511
#define HANDOFF_IO_TIMEOUT_US        250000

static_assert(sizeof(struct usb_dwc3_handoff) == 192, "DWC3 handoff descriptor size");
static_assert(offsetof(struct usb_dwc3_handoff, flags) == 0x10, "DWC3 handoff flags offset");
static_assert(offsetof(struct usb_dwc3_handoff, event_buffer_phys) == 0x20,
              "DWC3 handoff event ring offset");
static_assert(offsetof(struct usb_dwc3_handoff, tx_buffer_phys) == 0x60, "DWC3 handoff TX offset");
static_assert(offsetof(struct usb_dwc3_handoff, rx_buffer_phys) == 0x90, "DWC3 handoff RX offset");
static_assert(offsetof(struct usb_dwc3_handoff, rx_busy) == 0xbc, "DWC3 handoff tail offset");

struct usb_dwc3_handoff_state {
    struct usb_dwc3_handoff *descriptor;
    struct usb_dwc3_handoff snapshot;
    struct dwc3_trb *tx_trb;
    struct dwc3_trb *rx_trb;
    u8 *tx_buffer;
    u8 *rx_buffer;
    u32 event_cursor;
    u32 rx_offset;
    u32 rx_length;
    u64 retained_heap_start;
    u64 retained_heap_size;
    bool tx_busy;
    bool rx_busy;
    bool active;
    bool io_failed;
};

static struct usb_dwc3_handoff_state handoff;

static bool add_overflows(u64 start, u64 size, u64 *end)
{
    if (size > UINT64_MAX - start)
        return true;

    *end = start + size;
    return false;
}

static bool range_contains(u64 outer_start, u64 outer_size, u64 inner_start, u64 inner_size)
{
    u64 outer_end;
    u64 inner_end;

    if (!outer_size || !inner_size || add_overflows(outer_start, outer_size, &outer_end) ||
        add_overflows(inner_start, inner_size, &inner_end))
        return false;

    return inner_start >= outer_start && inner_end <= outer_end;
}

static bool range_in_ram(u64 start, u64 size)
{
    return range_contains(cur_boot_args.phys_base, cur_boot_args.mem_size, start, size);
}

static bool ranges_overlap(u64 first_start, u64 first_size, u64 second_start, u64 second_size)
{
    u64 first_end;
    u64 second_end;

    if (add_overflows(first_start, first_size, &first_end) ||
        add_overflows(second_start, second_size, &second_end))
        return true;

    return first_start < second_end && second_start < first_end;
}

static bool is_j713_t8132(void)
{
    int chosen = adt_path_offset(adt, "/chosen");
    u32 adt_chip_id = 0;
    const char *target = adt_getprop(adt, 0, "target-type", NULL);

    if (chosen < 0 || ADT_GETPROP(adt, chosen, "chip-id", &adt_chip_id) < 0 || adt_chip_id != T8132)
        return false;

    return (target && (!strcmp(target, "J713AP") || !strcmp(target, "J713"))) ||
           adt_is_compatible(adt, 0, "J713AP");
}

static int get_adt_reg(const char *path_name, int index, u64 *base)
{
    int path[8];

    if (adt_path_offset_trace(adt, path_name, path) < 0)
        return -1;

    return adt_get_reg(adt, path, "reg", index, base, NULL);
}

static int validate_descriptor(const struct usb_dwc3_handoff *desc)
{
    u64 expected_regs;
    u64 expected_wdt;
    u64 ignored_end;

    if (desc->magic != USB_DWC3_HANDOFF_MAGIC || desc->version != USB_DWC3_HANDOFF_VERSION ||
        desc->size < sizeof(*desc) || desc->size > SZ_16K ||
        !(desc->flags & USB_DWC3_HANDOFF_READY))
        return -1;

    if (!range_in_ram((u64)handoff.descriptor, desc->size) ||
        ((u64)handoff.descriptor & (HANDOFF_DESCRIPTOR_ALIGNMENT - 1)))
        return -1;

    if (get_adt_reg("/arm-io/usb-drd0", 0, &expected_regs) < 0 ||
        get_adt_reg("/arm-io/wdt", 0, &expected_wdt) < 0 || desc->regs_phys != expected_regs ||
        desc->wdt_regs_phys != expected_wdt || (desc->regs_phys & (SZ_4K - 1)))
        return -1;

    if (!desc->event_buffer_size || (desc->event_buffer_size & 3) ||
        desc->event_buffer_size > DWC3_GEVNTCOUNT_MASK ||
        desc->event_buffer_offset >= desc->event_buffer_size / sizeof(u32) ||
        (desc->event_buffer_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        !range_in_ram(desc->event_buffer_phys, desc->event_buffer_size))
        return -1;

    if (!desc->scratchpad_size || !desc->xfer_buffer_size || !desc->trb_buffer_size ||
        (desc->scratchpad_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->xfer_buffer_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->trb_buffer_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        !range_in_ram(desc->scratchpad_phys, desc->scratchpad_size) ||
        !range_in_ram(desc->xfer_buffer_phys, desc->xfer_buffer_size) ||
        !range_in_ram(desc->trb_buffer_phys, desc->trb_buffer_size))
        return -1;

    if (desc->tx_endpoint != USB_DWC3_HANDOFF_TX_ENDPOINT ||
        desc->rx_endpoint != USB_DWC3_HANDOFF_RX_ENDPOINT || desc->tx_max_packet < 512 ||
        desc->tx_max_packet > SZ_1M || (desc->tx_max_packet & 3))
        return -1;

    if ((desc->tx_buffer_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->rx_buffer_phys & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->tx_buffer_iova & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->rx_buffer_iova & (HANDOFF_BUFFER_ALIGNMENT - 1)) ||
        (desc->tx_trb_phys & (HANDOFF_TRB_ALIGNMENT - 1)) ||
        (desc->rx_trb_phys & (HANDOFF_TRB_ALIGNMENT - 1)) ||
        (desc->tx_trb_iova & (HANDOFF_TRB_ALIGNMENT - 1)) ||
        (desc->rx_trb_iova & (HANDOFF_TRB_ALIGNMENT - 1)))
        return -1;

    if (!range_contains(desc->xfer_buffer_phys, desc->xfer_buffer_size, desc->tx_buffer_phys,
                        desc->tx_max_packet) ||
        !range_contains(desc->xfer_buffer_phys, desc->xfer_buffer_size, desc->rx_buffer_phys,
                        desc->tx_max_packet) ||
        !range_contains(desc->trb_buffer_phys, desc->trb_buffer_size, desc->tx_trb_phys,
                        sizeof(struct dwc3_trb)) ||
        !range_contains(desc->trb_buffer_phys, desc->trb_buffer_size, desc->rx_trb_phys,
                        sizeof(struct dwc3_trb)))
        return -1;

    if (ranges_overlap(desc->tx_buffer_phys, desc->tx_max_packet, desc->rx_buffer_phys,
                       desc->tx_max_packet) ||
        ranges_overlap(desc->tx_buffer_iova, desc->tx_max_packet, desc->rx_buffer_iova,
                       desc->tx_max_packet) ||
        ranges_overlap(desc->tx_trb_phys, sizeof(struct dwc3_trb), desc->rx_trb_phys,
                       sizeof(struct dwc3_trb)) ||
        ranges_overlap(desc->tx_trb_iova, sizeof(struct dwc3_trb), desc->rx_trb_iova,
                       sizeof(struct dwc3_trb)))
        return -1;

    if (add_overflows(desc->tx_buffer_iova, desc->tx_max_packet, &ignored_end) ||
        add_overflows(desc->rx_buffer_iova, desc->tx_max_packet, &ignored_end) ||
        add_overflows(desc->tx_trb_iova, sizeof(struct dwc3_trb), &ignored_end) ||
        add_overflows(desc->rx_trb_iova, sizeof(struct dwc3_trb), &ignored_end))
        return -1;

    return 0;
}

static void publish_runtime_state(void)
{
    handoff.descriptor->event_buffer_offset = handoff.event_cursor;
    handoff.descriptor->tx_busy = handoff.tx_busy;
    handoff.descriptor->rx_busy = handoff.rx_busy;
    dc_cvac_range(handoff.descriptor, sizeof(*handoff.descriptor));
    dma_wmb();
}

static int endpoint_command(u8 endpoint, u32 command, u64 trb_iova)
{
    u64 regs = handoff.snapshot.regs_phys;

    write32(regs + DWC3_DEPCMDPAR0(endpoint), trb_iova >> 32);
    write32(regs + DWC3_DEPCMDPAR1(endpoint), (u32)trb_iova);
    write32(regs + DWC3_DEPCMDPAR2(endpoint), 0);
    write32(regs + DWC3_DEPCMD(endpoint), command | DWC3_DEPCMD_CMDACT);

    if (poll32(regs + DWC3_DEPCMD(endpoint), DWC3_DEPCMD_CMDACT, 0, 1000))
        return -1;

    return DWC3_DEPCMD_STATUS(read32(regs + DWC3_DEPCMD(endpoint))) ? -1 : 0;
}

static int start_transfer(u8 endpoint, struct dwc3_trb *trb, u64 trb_iova, u64 buffer_iova,
                          u32 length)
{
    trb->bpl = buffer_iova;
    trb->bph = buffer_iova >> 32;
    trb->size = DWC3_TRB_SIZE_LENGTH(length);
    trb->ctrl = DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRBCTL_NORMAL;
    dc_cvac_range(trb, sizeof(*trb));
    dma_wmb();

    return endpoint_command(endpoint, DWC3_DEPCMD_STARTTRANSFER, trb_iova);
}

static void fail_io(void)
{
    handoff.io_failed = true;
}

static void handle_event(union dwc3_event event)
{
    if (event.type.is_devspec) {
        if (event.type.type == DWC3_EVENT_TYPE_DEV &&
            (event.devt.type == DWC3_DEVT_DISCONN || event.devt.type == DWC3_DEVT_USBRST))
            fail_io();
        return;
    }

    if (event.depevt.endpoint_event != DWC3_DEPEVT_XFERCOMPLETE)
        return;

    if (event.depevt.endpoint_number == handoff.snapshot.tx_endpoint) {
        dc_ivac_range(handoff.tx_trb, sizeof(*handoff.tx_trb));
        dma_rmb();
        handoff.tx_busy = false;
    } else if (event.depevt.endpoint_number == handoff.snapshot.rx_endpoint) {
        u32 remaining;

        dc_ivac_range(handoff.rx_trb, sizeof(*handoff.rx_trb));
        dma_rmb();
        remaining = handoff.rx_trb->size & DWC3_TRB_SIZE_MASK;
        if (remaining > handoff.snapshot.tx_max_packet) {
            fail_io();
            return;
        }

        handoff.rx_busy = false;
        handoff.rx_offset = 0;
        handoff.rx_length = handoff.snapshot.tx_max_packet - remaining;
        if (handoff.rx_length) {
            dc_ivac_range(handoff.rx_buffer, handoff.rx_length);
            dma_rmb();
        }
    }
}

static void handle_events(void)
{
    u32 pending;

    if (!handoff.active || handoff.io_failed)
        return;

    pending = read32(handoff.snapshot.regs_phys + DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
    if ((pending & 3) || pending > handoff.snapshot.event_buffer_size) {
        fail_io();
        return;
    }

    while (pending) {
        union dwc3_event event;
        u32 *event_word = (u32 *)(handoff.snapshot.event_buffer_phys +
                                  handoff.event_cursor * sizeof(*event_word));

        dc_ivac_range(event_word, sizeof(*event_word));
        dma_rmb();
        event.raw = *event_word;

        handoff.event_cursor =
            (handoff.event_cursor + 1) % (handoff.snapshot.event_buffer_size / sizeof(u32));
        write32(handoff.snapshot.regs_phys + DWC3_GEVNTCOUNT(0), sizeof(u32));
        pending -= sizeof(u32);

        handle_event(event);
        publish_runtime_state();
        if (handoff.io_failed)
            return;
    }
}

static int wait_for_tx(void)
{
    u64 timeout = timeout_calculate(HANDOFF_IO_TIMEOUT_US);

    while (handoff.tx_busy && !handoff.io_failed) {
        handle_events();
        if (timeout_expired(timeout)) {
            fail_io();
            return -1;
        }
    }

    return handoff.io_failed ? -1 : 0;
}

static int start_rx(void)
{
    if (handoff.rx_busy || handoff.rx_length || handoff.io_failed)
        return 0;

    dc_civac_range(handoff.rx_buffer, handoff.snapshot.tx_max_packet);
    if (start_transfer(handoff.snapshot.rx_endpoint, handoff.rx_trb, handoff.snapshot.rx_trb_iova,
                       handoff.snapshot.rx_buffer_iova, handoff.snapshot.tx_max_packet) < 0) {
        fail_io();
        return -1;
    }

    handoff.rx_busy = true;
    publish_runtime_state();
    return 0;
}

static ssize_t handoff_can_read(void *opaque)
{
    UNUSED(opaque);
    handle_events();
    if (!handoff.rx_length)
        start_rx();
    return handoff.rx_length;
}

static bool handoff_can_write(void *opaque)
{
    UNUSED(opaque);
    handle_events();
    return handoff.active && !handoff.io_failed;
}

static ssize_t handoff_read(void *opaque, void *buffer, size_t count)
{
    size_t length;

    UNUSED(opaque);
    handle_events();
    length = min(count, (size_t)handoff.rx_length);
    if (!length) {
        start_rx();
        return 0;
    }

    memcpy(buffer, handoff.rx_buffer + handoff.rx_offset, length);
    handoff.rx_offset += length;
    handoff.rx_length -= length;
    if (!handoff.rx_length) {
        handoff.rx_offset = 0;
        start_rx();
    }

    return length;
}

static ssize_t handoff_write(void *opaque, const void *buffer, size_t count)
{
    const u8 *source = buffer;
    size_t written = 0;

    UNUSED(opaque);
    while (written < count) {
        size_t length;

        if (wait_for_tx() < 0)
            return written;

        length = min(count - written, (size_t)HANDOFF_TX_CHUNK);
        memcpy(handoff.tx_buffer, source + written, length);
        dc_cvac_range(handoff.tx_buffer, length);
        if (start_transfer(handoff.snapshot.tx_endpoint, handoff.tx_trb,
                           handoff.snapshot.tx_trb_iova, handoff.snapshot.tx_buffer_iova,
                           length) < 0) {
            fail_io();
            return written;
        }

        handoff.tx_busy = true;
        publish_runtime_state();
        written += length;
    }

    return written;
}

static void handoff_flush(void *opaque)
{
    UNUSED(opaque);
    wait_for_tx();
}

static void handoff_iodev_handle_events(void *opaque)
{
    UNUSED(opaque);
    handle_events();
}

static const struct iodev_ops handoff_iodev_ops = {
    .can_read = handoff_can_read,
    .can_write = handoff_can_write,
    .read = handoff_read,
    .write = handoff_write,
    .queue = handoff_write,
    .flush = handoff_flush,
    .handle_events = handoff_iodev_handle_events,
};

static struct iodev handoff_iodev = {
    .ops = &handoff_iodev_ops,
    .lock = SPINLOCK_INIT,
    .usage = USAGE_CONSOLE | USAGE_UARTPROXY,
    .opaque = &handoff,
};

static bool plausible_descriptor_pointer(u64 pointer)
{
    return !(pointer & (HANDOFF_DESCRIPTOR_ALIGNMENT - 1)) &&
           range_in_ram(pointer, sizeof(struct usb_dwc3_handoff));
}

static int try_descriptor(u64 pointer)
{
    if (!plausible_descriptor_pointer(pointer))
        return 0;

    dc_ivac_range((void *)pointer, sizeof(struct usb_dwc3_handoff));
    dma_rmb();
    handoff.descriptor = (struct usb_dwc3_handoff *)pointer;
    memcpy(&handoff.snapshot, handoff.descriptor, sizeof(handoff.snapshot));

    if (handoff.snapshot.magic != USB_DWC3_HANDOFF_MAGIC) {
        handoff.descriptor = NULL;
        return 0;
    }

    if (validate_descriptor(&handoff.snapshot) < 0)
        return -1;

    return 1;
}

static u64 read_be64(const u8 *value)
{
    u64 result = 0;

    for (int i = 0; i < 8; ++i)
        result = (result << 8) | value[i];
    return result;
}

static int try_adt_property(const char *name, bool legacy, bool *present)
{
    int chosen = adt_path_offset(adt, "/chosen");
    u32 length = 0;
    const u8 *property;
    u64 pointer;
    int ret;

    if (chosen < 0)
        return 0;

    property = adt_getprop(adt, chosen, name, &length);
    if (!property)
        return 0;

    *present = true;
    if (length != sizeof(u64))
        return -1;

    pointer = read_be64(property);
    ret = try_descriptor(pointer);
    if (ret || !legacy)
        return ret;

    memcpy(&pointer, property, sizeof(pointer));
    return try_descriptor(pointer);
}

enum usb_dwc3_handoff_result usb_dwc3_handoff_adopt(const u64 *entry_args, size_t entry_arg_count)
{
    bool property_present = false;
    u32 required_endpoints;
    int ret;

    if (!is_j713_t8132())
        return USB_DWC3_HANDOFF_ABSENT;

    ret = try_adt_property(USB_DWC3_HANDOFF_FDT_PROPERTY, false, &property_present);
    if (!ret)
        ret = try_adt_property(USB_DWC3_HANDOFF_LEGACY_PROPERTY, true, &property_present);

    if (!ret) {
        for (size_t i = 0; i < entry_arg_count && !ret; ++i)
            ret = try_descriptor(entry_args[i]);
    }

    if (ret < 0 || (!ret && property_present)) {
        printf("Inherited DWC3 handoff descriptor is invalid\n");
        return USB_DWC3_HANDOFF_INVALID;
    }
    if (!ret)
        return USB_DWC3_HANDOFF_ABSENT;

    required_endpoints = DWC3_DALEPENA_EP(handoff.snapshot.tx_endpoint) |
                         DWC3_DALEPENA_EP(handoff.snapshot.rx_endpoint);
    if ((read32(handoff.snapshot.regs_phys + DWC3_GSNPSID) & DWC3_GSNPSID_MASK) != 0x33310000 ||
        !(read32(handoff.snapshot.regs_phys + DWC3_DCTL) & DWC3_DCTL_RUN_STOP) ||
        (read32(handoff.snapshot.regs_phys + DWC3_DALEPENA) & required_endpoints) !=
            required_endpoints ||
        (read32(handoff.snapshot.regs_phys + DWC3_GEVNTSIZ(0)) & 0xffff) !=
            handoff.snapshot.event_buffer_size) {
        printf("Inherited DWC3 hardware state is inconsistent\n");
        return USB_DWC3_HANDOFF_INVALID;
    }

    handoff.tx_trb = (struct dwc3_trb *)handoff.snapshot.tx_trb_phys;
    handoff.rx_trb = (struct dwc3_trb *)handoff.snapshot.rx_trb_phys;
    handoff.tx_buffer = (u8 *)handoff.snapshot.tx_buffer_phys;
    handoff.rx_buffer = (u8 *)handoff.snapshot.rx_buffer_phys;
    handoff.event_cursor = handoff.snapshot.event_buffer_offset;
    handoff.tx_busy = !!handoff.snapshot.tx_busy;
    handoff.rx_busy = !!handoff.snapshot.rx_busy;
    handoff.active = true;
    spin_init(&handoff_iodev.lock);
    iodev_register_device(IODEV_USB0, &handoff_iodev);

    handle_events();
    if (!handoff.rx_busy && !handoff.rx_length)
        start_rx();

    if (handoff.io_failed)
        return USB_DWC3_HANDOFF_INVALID;

    publish_runtime_state();
    return USB_DWC3_HANDOFF_ADOPTED;
}

bool usb_dwc3_handoff_active(void)
{
    return handoff.active;
}

bool usb_dwc3_handoff_console_healthy(void)
{
    handle_events();
    return handoff.active && !handoff.io_failed;
}

bool usb_dwc3_handoff_owns_controller(u32 index)
{
    return handoff.active && index == 0;
}

bool usb_dwc3_handoff_is_iodev(iodev_id_t id)
{
    return handoff.active && id == IODEV_USB0 && iodev_get_opaque(id) == &handoff;
}

int usb_dwc3_handoff_protect_heap(void)
{
    const u64 starts[] = {
        (u64)handoff.descriptor,          handoff.snapshot.event_buffer_phys,
        handoff.snapshot.scratchpad_phys, handoff.snapshot.xfer_buffer_phys,
        handoff.snapshot.trb_buffer_phys,
    };
    const u64 sizes[] = {
        handoff.snapshot.size,
        handoff.snapshot.event_buffer_size,
        handoff.snapshot.scratchpad_size,
        handoff.snapshot.xfer_buffer_size,
        handoff.snapshot.trb_buffer_size,
    };
    u64 heap_start = ALIGN_UP(cur_boot_args.top_of_kernel_data, 64);
    u64 ram_end;
    u64 retained_start = UINT64_MAX;
    u64 retained_end = 0;

    if (!handoff.active)
        return 0;
    if (add_overflows(cur_boot_args.phys_base, cur_boot_args.mem_size, &ram_end))
        return -1;

    for (size_t i = 0; i < ARRAY_SIZE(starts); ++i) {
        u64 end;

        if (add_overflows(starts[i], sizes[i], &end))
            return -1;
        retained_start = min(retained_start, starts[i]);
        retained_end = max(retained_end, end);
    }

    if (retained_start <= heap_start && retained_end > heap_start)
        return -1;

    if (retained_start > heap_start) {
        handoff.retained_heap_start = ALIGN_DOWN(retained_start, SZ_16K);
        handoff.retained_heap_size = ram_end - handoff.retained_heap_start;
        heapblock_set_limit((void *)handoff.retained_heap_start);
    }

    return 0;
}

void usb_dwc3_handoff_prepare_next_stage(void)
{
    if (!handoff.active)
        return;

    iodev_flush(IODEV_USB0);
    publish_runtime_state();
    dma_wmb();
}

static int reserve_fdt_range(void *fdt, u64 start, u64 size)
{
    return fdt_add_mem_rsv(fdt, start, size);
}

int usb_dwc3_handoff_publish_fdt(void *fdt, int chosen)
{
    if (!handoff.active)
        return 0;

    if (fdt_setprop_u64(fdt, chosen, USB_DWC3_HANDOFF_FDT_PROPERTY, (u64)handoff.descriptor) < 0)
        return -1;

    if (handoff.retained_heap_size)
        return reserve_fdt_range(fdt, handoff.retained_heap_start, handoff.retained_heap_size);

    if (reserve_fdt_range(fdt, (u64)handoff.descriptor, handoff.snapshot.size) < 0 ||
        reserve_fdt_range(fdt, handoff.snapshot.event_buffer_phys,
                          handoff.snapshot.event_buffer_size) < 0 ||
        reserve_fdt_range(fdt, handoff.snapshot.scratchpad_phys, handoff.snapshot.scratchpad_size) <
            0 ||
        reserve_fdt_range(fdt, handoff.snapshot.xfer_buffer_phys,
                          handoff.snapshot.xfer_buffer_size) < 0 ||
        reserve_fdt_range(fdt, handoff.snapshot.trb_buffer_phys, handoff.snapshot.trb_buffer_size) <
            0)
        return -1;

    return 0;
}
