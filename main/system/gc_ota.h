/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GC_OTA_H_
#define _GC_OTA_H_

#include <stdint.h>

/* Firmware payload bytes carried by one GC_OTA_CMD transaction. Small on
 * purpose: the GC port gives each RMT channel two 64 word blocks, so a whole
 * transaction has to fit in 128 bits. */
#define GC_OTA_DATA_LEN 8

/* Bytes returned by the status command. */
#define GC_OTA_STATUS_LEN 3

#define GC_OTA_PROTO_VER 1

/* Sub command, carried in the first payload byte. */
enum {
    GC_OTA_SUB_START = 0,
    GC_OTA_SUB_DATA,
    GC_OTA_SUB_END,
    GC_OTA_SUB_ABORT,
};

/* Reported by gc_ota_status(). The console must hold off while we say BUSY:
 * flash writes stall cache and nothing can service the SI link meanwhile. */
enum {
    GC_OTA_IDLE = 0,
    GC_OTA_READY,
    GC_OTA_BUSY,
    GC_OTA_DONE,
    GC_OTA_ERROR,
};

/* Both called straight from the RMT ISR. */
void gc_ota_cmd(const uint8_t *payload);
void gc_ota_status(uint8_t *status);

void gc_ota_init(void);

#endif /* _GC_OTA_H_ */
