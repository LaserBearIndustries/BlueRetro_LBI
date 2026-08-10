/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copying the adapter's settings to and from the card.
 *
 * Restoring an adapter that lost its filesystem is the obvious use, but the one
 * that earns it is provisioning: flashing and configuring a batch of adapters
 * identically, from the same card, without driving a browser once per unit.
 *
 * The whole config blob moves as one, which is deliberate. It is the thing the
 * adapter itself loads and stores, so a backup cannot disagree with the running
 * format, and there is no per field mapping to keep in step with config.h.
 */

#ifndef _GC_CFG_H_
#define _GC_CFG_H_

#include <stdint.h>

/* Bytes per transfer, in either direction. Bounded by the GC port giving each
 * RMT channel two 64 word blocks. */
#define GC_CFG_CHUNK 8

/* Bytes returned by the info command:
 * [0] protocol version
 * [1] state, see below
 * [2..3] config size, little endian
 * [4] why the last restore was refused, see below
 * [5] reserved
 * [6..7] detail for that reason, little endian */
#define GC_CFG_INFO_LEN 8

#define GC_CFG_PROTO_VER 2

/* Which check refused a restore, and what it saw. Reporting only that
 * something was wrong meant the reason lived in a log nobody had, and cost two
 * rounds of guessing from the outside. */
enum {
    GC_CFG_WHY_NONE = 0,
    GC_CFG_WHY_MISSING_CHUNK,   /* detail: the chunk that never arrived */
    GC_CFG_WHY_BAD_MAGIC,       /* detail: low half of the magic that was staged */
    GC_CFG_WHY_BAD_MAP_SIZE,    /* detail: the port whose map_size was too big */
    /* The transfer never had a chance: the task that adopts a config could
     * not be created at boot. Reported rather than left to look like a
     * refusal, which is how it presented for four rounds. */
    GC_CFG_WHY_NO_TASK,
};

/* Sub command, carried in the first payload byte. */
enum {
    GC_CFG_SUB_BEGIN = 0,   /* start staging a restore */
    GC_CFG_SUB_APPLY,       /* validate what was staged, then adopt it */
    GC_CFG_SUB_ABORT,
};

/* Applying writes the config file and reloads every connected controller's
 * mapping, so the console waits out BUSY as it does for an update. */
enum {
    GC_CFG_ST_IDLE = 0,
    GC_CFG_ST_BUSY,
    GC_CFG_ST_OK,
    GC_CFG_ST_ERROR,
    /* Waiting for chunks. Entered by the start command itself, so a restore
     * does not depend on anything running between it and the first chunk. */
    GC_CFG_ST_READY,
};

/* All four called straight from the RMT ISR. */
void gc_cfg_info(uint8_t *out);
void gc_cfg_read(uint16_t chunk, uint8_t *out);
void gc_cfg_write(const uint8_t *payload);
void gc_cfg_cmd(const uint8_t *payload);

void gc_cfg_init(void);

#endif /* _GC_CFG_H_ */
