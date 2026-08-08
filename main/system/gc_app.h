/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Support for the GameCube side companion app: state the app reads out of the
 * adapter, and the mapping it writes back, over the vendor SI opcodes.
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

/* Sub commands of the mapping write opcode, in the first payload byte. */
enum {
    GC_APP_MAP_BEGIN = 0,   /* [port]  start a fresh mapping for that port */
    GC_APP_MAP_SET,         /* [idx][src][dst][dst_id][max][thr][dz][turbo] */
    GC_APP_MAP_COMMIT,      /* [count] apply and persist */
    GC_APP_MAP_CANCEL,      /* discard */
};

/* Reported in byte 1 of the input reply, so the app learns how a commit went
 * from the poll it is already doing. Bit 7 flags the output being held neutral. */
enum {
    GC_APP_ST_IDLE = 0,
    GC_APP_ST_BUSY,
    GC_APP_ST_OK,
    GC_APP_ST_ERROR,
};
#define GC_APP_ST_MUTED 0x80

struct wireless_ctrl;

/* Called from the Bluetooth side each time a report is decoded, before mapping
 * is applied. Cheap by design: it runs on every controller report. */
void gc_app_input_update(uint8_t dev_id, struct wireless_ctrl *ctrl);

/* True while that port's output is being held neutral for capture. */
uint32_t gc_app_is_muted(uint8_t out_idx);

/* All three called straight from the RMT ISR. */
void gc_app_input_read(uint8_t *out);
void gc_app_map_cmd(const uint8_t *payload);
void gc_app_mode_cmd(const uint8_t *payload);

void gc_app_init(void);

#endif /* _GC_APP_H_ */
