/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "bluetooth/host.h"
#include "manager.h"
#include "dev_status.h"

void dev_status_get(struct dev_status *out) {
    uint32_t port_cnt;

    if (out == NULL) {
        return;
    }

    /* Zeroed up front so every field a port does not set reads as empty, and
     * so the name of a device that matched nothing is an empty string rather
     * than whatever the caller's buffer held. Also what makes an unchanged
     * snapshot compare equal to the previous one. */
    memset(out, 0, sizeof(*out));

    out->version = DEV_STATUS_VERSION;
    out->system_id = (uint8_t)wired_adapter.system_id;

    port_cnt = hw_config.port_cnt;
    if (port_cnt > DEV_STATUS_MAX_PORT) {
        port_cnt = DEV_STATUS_MAX_PORT;
    }
    out->port_cnt = (uint8_t)port_cnt;

    for (uint32_t i = 0; i < port_cnt; i++) {
        struct dev_status_port *port = &out->port[i];
        struct bt_dev *device = NULL;

        /* Wired is asked first. wired_port_hdl() steps over occupied ports
         * when it hands out out_idx, so a port cannot legitimately be both -
         * but it reassigns those indices as devices come and go, and asking
         * in this order means a stale one can never make a port with a real
         * controller in it report as wireless. */
        if (sys_mgr_port_is_wired(i)) {
            port->state = DEV_STATUS_WIRED;
            continue;
        }

        /* The active form, so a device still negotiating HID does not show up
         * as a controller the player can use. */
        if (bt_host_get_active_dev_from_out_idx(i, &device) > -1 && device) {
            port->state = DEV_STATUS_BT;
            /* BT_NONE is -1, so this lands on zero for a device whose type was
             * never resolved - the same "not known" the far end expects. */
            port->type = (uint8_t)(device->ids.type + 1);
            port->subtype = (uint8_t)device->ids.subtype;

            /* NULL when the remote name matched no entry in the table, which
             * is a real outcome and not an error: the pad still works, it just
             * has to be drawn as a generic one. The string itself lives in
             * flash, which is fine here because nothing calls this from an
             * ISR - unlike the GameCube app path, which copies it to DRAM for
             * exactly that reason. */
            if (device->name) {
                strncpy(port->name, device->name->name, DEV_STATUS_NAME_LEN - 1);
            }
        }
    }
}
