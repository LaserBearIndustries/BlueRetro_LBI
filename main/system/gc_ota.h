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

/* The running firmware version, handed back a slice at a time because a whole
 * SI transaction has to fit in 128 bits. Matches esp_app_desc_t::version. */
#define GC_OTA_VER_LEN 32
#define GC_OTA_VER_CHUNK 8

/* The project name, from esp_app_desc_t::project_name, which is
 * "BlueRetro" with the hardware and system appended - BlueRetro_hw2_gamecube.
 * It is the only thing the adapter knows about itself that says which
 * hardware it is, and the console needs that to refuse an image built for
 * the other one.
 *
 * Served through the existing version command rather than a new opcode, as
 * chunks 4 to 7. Firmware that predates this returns zeros for those chunks
 * already - the handler bounds checks against GC_OTA_VER_LEN - so an empty
 * name means old firmware rather than an unnamed one, and no console has to
 * guess which case it is looking at. */
#define GC_OTA_NAME_LEN 32
#define GC_OTA_NAME_CHUNK0 4

/* 2 adds the project name. 1 is still answered, and still spoken by every
 * adapter in the field, so the console has to cope with both. */
#define GC_OTA_PROTO_VER 2

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

/* All three called straight from the RMT ISR. */
void gc_ota_cmd(const uint8_t *payload);
void gc_ota_status(uint8_t *status);
void gc_ota_version(uint8_t chunk, uint8_t *out);

void gc_ota_init(void);

#endif /* _GC_OTA_H_ */
