/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Support for the GameCube side companion app: state the app needs to read out
 * of the adapter over the vendor SI opcodes.
 */

#ifndef _GC_APP_H_
#define _GC_APP_H_

#include <stdint.h>

/* Bytes returned by the raw input command. Has to fit a single SI reply, which
 * on the GameCube ports means 128 bits. */
#define GC_APP_INPUT_LEN 12

/* Reported in the device field when nothing has been seen yet. */
#define GC_APP_NO_DEV 0xFF

/* Number of axes reported, in this order:
 * AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, TRIG_L, TRIG_R */
#define GC_APP_AXIS_CNT 6

struct wireless_ctrl;

/* Called from the Bluetooth side each time a report is decoded, before mapping
 * is applied. Cheap by design: it runs on every controller report. */
void gc_app_input_update(uint8_t dev_id, struct wireless_ctrl *ctrl);

/* Called straight from the RMT ISR. */
void gc_app_input_read(uint8_t *out);

#endif /* _GC_APP_H_ */
