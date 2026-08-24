/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * A snapshot of what is connected to the adapter, in a form that can be sent
 * somewhere else and drawn.
 *
 * The adapter already knows all of this - manager.c reads the port sense pins
 * every cycle to decide which ports are free for Bluetooth pads, and the
 * Bluetooth layer has the type and the matched name. What was missing is one
 * place that puts it together and one layout that both ends agree on. This is
 * that; it holds no state of its own and starts nothing, so it costs nothing
 * until something asks.
 *
 * Deliberately independent of the transport. The first consumer is the Jewel
 * over BLE, but nothing here knows that.
 */

#ifndef _DEV_STATUS_H_
#define _DEV_STATUS_H_

#include <stdint.h>

/* Defined the same guarded way in adapter.h. Repeated rather than included
 * because this header describes a wire layout that the other end of the link
 * has to agree on byte for byte, and that end is a different project with no
 * adapter.h in it. Depending on nothing but stdint.h means the identical file
 * can live in both trees and the two cannot drift. */
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

/* Bump on any layout change. The receiving end has its own release cycle and
 * will meet older adapters, so it checks this before trusting the rest. */
#define DEV_STATUS_VERSION 1

/* Matches GC_APP_DEV_NAME_LEN. The names being matched against are labels
 * like "Xbox Wireless Controller" or "DualSense Wireless Controller", and 32
 * has been enough for the GameCube app. */
#define DEV_STATUS_NAME_LEN 32

/* GameCube is four. WIRED_MAX_DEV is 12 for the Saturn, but nothing that far
 * out is reachable on a console with four ports, and sending twelve of these
 * would triple the payload to describe eight ports that are always empty. */
#define DEV_STATUS_MAX_PORT 4

/* What is in a port. */
enum {
    DEV_STATUS_EMPTY = 0,
    DEV_STATUS_WIRED,   /* sense pin says a real controller is plugged in */
    DEV_STATUS_BT,      /* a Bluetooth device is mapped to this port */
};

/* type follows the gc_app convention of BT type + 1, so that zero means "not
 * known" and stays distinct from BT_HID_GENERIC, which is zero.
 *
 * The name is sent as well as the type, and it is not redundant: BT_HID_GENERIC
 * is where every pad that is not PlayStation, Wii or Switch ends up, Xbox
 * included. Type alone cannot tell an Xbox pad from any other generic HID
 * device, so anything wanting to draw an Xbox icon has to read the name. It is
 * empty when the remote name matched nothing in the table. */
struct dev_status_port {
    uint8_t state;
    uint8_t type;
    uint8_t subtype;
    uint8_t rsvd;
    char name[DEV_STATUS_NAME_LEN];
} __packed;

struct dev_status {
    uint8_t version;
    uint8_t port_cnt;   /* ports this console actually has, <= DEV_STATUS_MAX_PORT */
    uint8_t system_id;  /* wired_adapter.system_id, so the far end can say "GameCube" */
    uint8_t rsvd;
    struct dev_status_port port[DEV_STATUS_MAX_PORT];
} __packed;

/* Fill out with the current state. Always writes the whole struct, so an
 * unchanged snapshot compares equal to the previous one and a caller that only
 * wants to transmit on change can memcmp rather than track anything here. */
void dev_status_get(struct dev_status *out);

#endif /* _DEV_STATUS_H_ */
