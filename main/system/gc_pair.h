/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Seeing and forgetting pairings from the GameCube side.
 *
 * Slots are handed out in order and turn straight into console ports, so a
 * phone or a watch that once paired keeps taking port one from the controller
 * that should have it. The connection watchdog reclaims the port after a few
 * seconds, which is a race worth winning but still a race; forgetting the thing
 * outright is the answer that stays answered.
 */

#ifndef _GC_PAIR_H_
#define _GC_PAIR_H_

#include <stdint.h>

/* Both key stores hold sixteen. */
#define GC_PAIR_MAX 32

/* Bytes returned by the info command:
 * [0] protocol version
 * [1] number of pairings
 * [2] state, see below
 * [3] task heartbeat, so a stuck task is visible rather than inferred
 * [4..7] reserved */
#define GC_PAIR_INFO_LEN 8

/* Bytes returned per entry:
 * [0] non zero if this one is Bluetooth LE
 * [1] port it is driving now, or 0xFF if it is not connected
 * [2..7] address, low byte first */
#define GC_PAIR_ENTRY_LEN 8

#define GC_PAIR_PROTO_VER 1
#define GC_PAIR_NO_PORT 0xFF

/* Sub command, carried in the first payload byte. */
enum {
    GC_PAIR_SUB_FORGET = 0,     /* second byte picks which */
    GC_PAIR_SUB_FORGET_ALL,
};

enum {
    GC_PAIR_ST_IDLE = 0,
    GC_PAIR_ST_BUSY,
    GC_PAIR_ST_OK,
    GC_PAIR_ST_ERROR,
};

/* All three called straight from the RMT ISR. */
void gc_pair_info(uint8_t *out);
void gc_pair_entry(uint8_t idx, uint8_t *out);
void gc_pair_cmd(const uint8_t *payload);

/* Run from an existing task, for the same reason as gc_cfg_service(). */
void gc_pair_service(void);

void gc_pair_init(void);

#endif /* _GC_PAIR_H_ */
