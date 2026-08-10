/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Choosing which firmware slot boots, from the GameCube side.
 *
 * The partition table carries a factory image and two OTA slots, and the
 * updater alternates between the OTA pair. That means the firmware that was
 * running before the last update is still sitting there intact, and the factory
 * image behind it has never been written since the adapter was built. This
 * exposes both, so a bad update is a menu entry rather than a soldering job.
 */

#ifndef _GC_BOOT_H_
#define _GC_BOOT_H_

#include <stdint.h>

/* Slot numbering used on the wire. */
enum {
    GC_BOOT_SLOT_FACTORY = 0,
    GC_BOOT_SLOT_OTA_0,
    GC_BOOT_SLOT_OTA_1,
    GC_BOOT_SLOT_CNT,
};

/* Bytes returned by the info command:
 * [0] protocol version
 * [1] slot running right now
 * [2] slot that is set to boot next
 * [3] bitmap of slots holding an image that will boot
 * [4] state, see below
 * [5..7] reserved */
#define GC_BOOT_INFO_LEN 8

/* Per slot version string, handed back a slice at a time like the OTA one. */
#define GC_BOOT_VER_LEN 32
#define GC_BOOT_VER_CHUNK 8

#define GC_BOOT_PROTO_VER 1

/* Selecting a slot writes the OTA data partition, so the console has to wait
 * out BUSY the same way it does for an update. */
enum {
    GC_BOOT_ST_IDLE = 0,
    GC_BOOT_ST_BUSY,
    GC_BOOT_ST_OK,
    GC_BOOT_ST_ERROR,
};

/* All three called straight from the RMT ISR. */
void gc_boot_info(uint8_t *out);
void gc_boot_version(uint8_t slot, uint8_t chunk, uint8_t *out);
void gc_boot_select(const uint8_t *payload);

void gc_boot_init(void);

#endif /* _GC_BOOT_H_ */
