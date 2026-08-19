/*
 * Copyright (c) 2019-2024, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "bluetooth/host.h"
#include "bluetooth/att_hid.h"
#include "bluetooth/mon.h"
#include "adapter/hid_parser.h"
#include "tools/util.h"
#include "generic.h"

/* A report descriptor for controllers that will not hand one over.
 *
 * A generic HID device is decoded from the descriptor it publishes over
 * SDP, so a device that never answers SDP has no report map and every
 * report it sends is dropped. A RetroFighters BattlerGC Pro in xinput mode
 * does exactly that: it opens the SDP channel, half configures it, ignores
 * the query that follows, and then streams perfectly good input forever.
 *
 * It calls itself an Xbox Wireless Controller and sends the layout that
 * goes with the name, so that is what this describes - taken from a trace
 * of the pad itself rather than from a datasheet:
 *
 *   bytes 0-1   X      left stick, 16 bit, centred at 0x8000
 *   bytes 2-3   Y
 *   bytes 4-5   Z      right stick
 *   bytes 6-7   Rz
 *   bytes 8-9   Brake        left trigger, 10 bits in a 16 bit field
 *   bytes 10-11 Accelerator  right trigger
 *   byte  12    Hat    4 bits, 0 centred, 1-8 clockwise, then 4 padding
 *   bytes 13-14 Buttons 1-15, then 1 padding
 *
 * Those are the usages hid_generic.c already maps, which is the point of
 * describing it rather than writing another handler: a real Xbox pad
 * publishes the same shape over SDP, so everything downstream of here is
 * the path that already works for one.
 */
static const uint8_t xb1_fallback_hid_desc[] = {
    0x05, 0x01,                    /* Usage Page (Generic Desktop)      */
    0x09, 0x05,                    /* Usage (Game Pad)                  */
    0xA1, 0x01,                    /* Collection (Application)          */
    0x85, 0x01,                    /*   Report ID (1)                   */
    0x09, 0x01,                    /*   Usage (Pointer)                 */
    0xA1, 0x00,                    /*   Collection (Physical)           */
    0x09, 0x30,                    /*     Usage (X)                     */
    0x09, 0x31,                    /*     Usage (Y)                     */
    0x15, 0x00,                    /*     Logical Minimum (0)           */
    0x27, 0xFF, 0xFF, 0x00, 0x00,  /*     Logical Maximum (65535)       */
    0x95, 0x02,                    /*     Report Count (2)              */
    0x75, 0x10,                    /*     Report Size (16)              */
    0x81, 0x02,                    /*     Input (Data,Var,Abs)          */
    0xC0,                          /*   End Collection                  */
    0x09, 0x01,                    /*   Usage (Pointer)                 */
    0xA1, 0x00,                    /*   Collection (Physical)           */
    0x09, 0x32,                    /*     Usage (Z)                     */
    0x09, 0x35,                    /*     Usage (Rz)                    */
    0x15, 0x00,                    /*     Logical Minimum (0)           */
    0x27, 0xFF, 0xFF, 0x00, 0x00,  /*     Logical Maximum (65535)       */
    0x95, 0x02,                    /*     Report Count (2)              */
    0x75, 0x10,                    /*     Report Size (16)              */
    0x81, 0x02,                    /*     Input (Data,Var,Abs)          */
    0xC0,                          /*   End Collection                  */
    0x05, 0x02,                    /*   Usage Page (Simulation)         */
    0x09, 0xC5,                    /*   Usage (Brake)                   */
    0x15, 0x00,                    /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x03,              /*   Logical Maximum (1023)          */
    0x95, 0x01,                    /*   Report Count (1)                */
    0x75, 0x10,                    /*   Report Size (16)                */
    0x81, 0x02,                    /*   Input (Data,Var,Abs)            */
    0x09, 0xC4,                    /*   Usage (Accelerator)             */
    0x15, 0x00,                    /*   Logical Minimum (0)             */
    0x26, 0xFF, 0x03,              /*   Logical Maximum (1023)          */
    0x95, 0x01,                    /*   Report Count (1)                */
    0x75, 0x10,                    /*   Report Size (16)                */
    0x81, 0x02,                    /*   Input (Data,Var,Abs)            */
    0x05, 0x01,                    /*   Usage Page (Generic Desktop)    */
    0x09, 0x39,                    /*   Usage (Hat switch)              */
    0x15, 0x01,                    /*   Logical Minimum (1)             */
    0x25, 0x08,                    /*   Logical Maximum (8)             */
    0x35, 0x00,                    /*   Physical Minimum (0)            */
    0x46, 0x3B, 0x01,              /*   Physical Maximum (315)          */
    0x66, 0x14, 0x00,              /*   Unit (degrees)                  */
    0x75, 0x04,                    /*   Report Size (4)                 */
    0x95, 0x01,                    /*   Report Count (1)                */
    0x81, 0x42,                    /*   Input (Data,Var,Abs,Null State) */
    0x75, 0x04,                    /*   Report Size (4)                 */
    0x95, 0x01,                    /*   Report Count (1)                */
    0x65, 0x00,                    /*   Unit (None)                     */
    0x81, 0x03,                    /*   Input (Const,Var,Abs) - padding */
    0x05, 0x09,                    /*   Usage Page (Button)             */
    0x19, 0x01,                    /*   Usage Minimum (1)               */
    0x29, 0x0F,                    /*   Usage Maximum (15)              */
    0x15, 0x00,                    /*   Logical Minimum (0)             */
    0x25, 0x01,                    /*   Logical Maximum (1)             */
    0x75, 0x01,                    /*   Report Size (1)                 */
    0x95, 0x0F,                    /*   Report Count (15)               */
    0x81, 0x02,                    /*   Input (Data,Var,Abs)            */
    0x75, 0x01,                    /*   Report Size (1)                 */
    0x95, 0x01,                    /*   Report Count (1)                */
    0x81, 0x03,                    /*   Input (Const,Var,Abs) - padding */
    0xC0,                          /* End Collection                    */
};

/* Microsoft, and the controller this stands in for. Normally these come
 * from the PNP record over SDP, which is equally not on offer here. */
#define XB1_FALLBACK_VID 0x045E
#define XB1_FALLBACK_PID 0x02FD

/* Applied only to a device whose name was matched against the table in
 * hidp.c, and only when that name says Xbox. Guessing this layout for a
 * device that never said what it was would map an unknown controller to
 * the wrong axes, which is worse than mapping it to none: it would look
 * like it worked. Returns 0 when it was applied.
 *
 * Note this is not a copy of a real controller's descriptor - no such
 * capture exists here. It is a description of what this pad was observed
 * to send, which is the same thing if the pad is honest about its name
 * and nothing if it is not.
 */
int32_t bt_hid_generic_fallback_desc(struct bt_dev *device) {
    struct bt_data *bt_data = &bt_adapter.data[device->ids.id];

    if (device->name == NULL || device->ids.type != BT_HID_GENERIC) {
        return -1;
    }

    if (strcasestr(device->name->name, "Xbox") == NULL) {
        return -1;
    }

    printf("# %s: dev: %ld no SDP, using the built in Xbox report map\n",
        __FUNCTION__, device->ids.id);
    bt_mon_log(true, "%s: dev: %ld no SDP, using the built in Xbox report map\n",
        __FUNCTION__, device->ids.id);

    bt_data->base.vid = XB1_FALLBACK_VID;
    bt_data->base.pid = XB1_FALLBACK_PID;

    hid_parser(bt_data, (uint8_t *)xb1_fallback_hid_desc,
        sizeof(xb1_fallback_hid_desc));

    return 0;
}

void bt_hid_cmd_generic_rumble(struct bt_dev *device, void *report) {
    struct bt_hidp_generic_rumble *rumble_report = (struct bt_hidp_generic_rumble *)report;

    if (rumble_report->report_id && rumble_report->report_size) {
        if (atomic_test_bit(&device->flags, BT_DEV_IS_BLE)) {
            bt_att_write_hid_report(device, rumble_report->report_id, rumble_report->state, rumble_report->report_size);
        }
        else {
            memcpy(bt_hci_pkt_tmp.hidp_data, rumble_report->state, rumble_report->report_size);
            bt_hid_cmd(device->acl_handle, device->intr_chan.dcid, BT_HIDP_DATA_OUT, rumble_report->report_id, rumble_report->report_size);
        }
    }
}

void bt_hid_generic_init(struct bt_dev *device) {
    atomic_set_bit(&device->flags, BT_DEV_HID_INIT_DONE);
}

void bt_hid_generic_hdlr(struct bt_dev *device, struct bt_hci_pkt *bt_hci_acl_pkt, uint32_t len) {
    uint32_t hidp_data_len = len - (BT_HCI_H4_HDR_SIZE + BT_HCI_ACL_HDR_SIZE
                                    + sizeof(struct bt_l2cap_hdr) + sizeof(struct bt_hidp_hdr));

    switch (bt_hci_acl_pkt->sig_hdr.code) {
        case BT_HIDP_DATA_IN:
            bt_host_bridge(device, bt_hci_acl_pkt->hidp_hdr.protocol, bt_hci_acl_pkt->hidp_data, hidp_data_len);
            break;
    }
}
