/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <esp_attr.h>
#include "adapter/adapter.h"
#include "gc_app.h"

/* A snapshot rather than a pointer to ctrl_input. That buffer is scratch space
 * the bridge reuses for whichever device reported last, and it is filled while
 * the ISR could be reading it. Copying a compact form out at bridge time gives
 * the ISR something stable, in DRAM, and unambiguous about which device it
 * describes. */
static volatile uint8_t input_snap[GC_APP_INPUT_LEN] = { GC_APP_NO_DEV };

/* Generic axis indices, in the order the app expects them. */
static const uint8_t gc_app_axis_idx[GC_APP_AXIS_CNT] = {
    AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, TRIG_L, TRIG_R,
};

/* Axes are raw controller units with a per device range, so normalise to a
 * signed percentage-ish scale the app can draw without knowing the source. */
static int8_t axis_to_int8(struct ctrl_axis *axis) {
    int32_t max, val;

    if (axis == NULL || axis->meta == NULL) {
        return 0;
    }

    val = axis->value;
    max = (val < 0) ? axis->meta->abs_min : axis->meta->abs_max;
    if (max <= 0) {
        return 0;
    }

    val = (val * 127) / max;
    if (val > 127) {
        val = 127;
    }
    else if (val < -127) {
        val = -127;
    }
    return (int8_t)val;
}

void gc_app_input_update(uint8_t dev_id, struct wireless_ctrl *ctrl) {
    uint8_t tmp[GC_APP_INPUT_LEN];
    uint32_t btns;

    if (ctrl == NULL) {
        return;
    }

    btns = (uint32_t)ctrl->btns[0].value;

    tmp[0] = dev_id;
    tmp[1] = 0; /* reserved, keeps the payload word aligned for the app */
    tmp[2] = (uint8_t)(btns >> 0);
    tmp[3] = (uint8_t)(btns >> 8);
    tmp[4] = (uint8_t)(btns >> 16);
    tmp[5] = (uint8_t)(btns >> 24);

    for (uint32_t i = 0; i < GC_APP_AXIS_CNT; i++) {
        tmp[6 + i] = (uint8_t)axis_to_int8(&ctrl->axes[gc_app_axis_idx[i]]);
    }

    /* Single pass copy. The ISR can still catch this mid update and see a mix
     * of two reports, which for a live readout costs at most one frame of
     * wrong pixels and is not worth locking the Bluetooth path for. */
    memcpy((void *)input_snap, tmp, sizeof(tmp));
}

void IRAM_ATTR gc_app_input_read(uint8_t *out) {
    for (uint32_t i = 0; i < GC_APP_INPUT_LEN; i++) {
        out[i] = input_snap[i];
    }
}
