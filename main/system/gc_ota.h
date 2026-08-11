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

/* Which board this is, as the string "hw1" or "hw2", so the console can
 * refuse an image built for the other one. Taken from
 * esp_app_desc_t::project_name, which CMakeLists builds as
 * BlueRetro$ENV{BR_HW}$ENV{BR_SYS} and is the only thing the running
 * firmware knows about its own hardware.
 *
 * Served through the existing version command as chunk 4, rather than a new
 * opcode. Firmware that predates this returns zeros for that chunk already -
 * the handler bounds checks against GC_OTA_VER_LEN - so an empty answer means
 * old firmware rather than an unnamed board, and no console has to guess
 * which case it is looking at.
 *
 * Held as a single digit rather than a copy of the name, and rebuilt into
 * text when asked. A 32 byte copy of the project name cost 32 bytes of .bss,
 * which moved the heap boundary by exactly enough that hid_parser could no
 * longer allocate its two report buffers - so controllers paired, their
 * descriptors were never parsed, and nothing reached the console at all.
 * There is no headroom here to spend. */
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
