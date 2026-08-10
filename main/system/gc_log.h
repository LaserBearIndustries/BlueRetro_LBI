/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pulling the Bluetooth debug log out over the GameCube port.
 *
 * The log already exists: with banksel set to the debug value, bt_mon_tx()
 * writes a btmon stream into the memory card buffer instead of the UART. Until
 * now the only way to turn that on or read it back was the Bluetooth web
 * config, which is no use when Bluetooth is the thing being diagnosed. This
 * puts both ends on the wired side.
 */

#ifndef _GC_LOG_H_
#define _GC_LOG_H_

#include <stdint.h>

/* Log bytes carried by one read transaction. Bounded by the GC port giving each
 * RMT channel two 64 word blocks, so command, reply and CRC together have to
 * fit in 128 bits. */
#define GC_LOG_CHUNK 8

/* Bytes returned by the status command:
 * [0] protocol version
 * [1] bank, non zero while capturing
 * [2] state, see below
 * [3] reserved
 * [4..7] captured length, little endian */
#define GC_LOG_STATUS_LEN 8

#define GC_LOG_PROTO_VER 1

/* Sub command, carried in the first payload byte. */
enum {
    GC_LOG_SUB_BANK_OFF = 0,    /* leave the debug bank, persist */
    GC_LOG_SUB_BANK_ON,         /* enter the debug bank, persist */
    GC_LOG_SUB_RESET,           /* rewind the capture to the start */
};

/* Reported in byte 2 of the status reply. Both bank changes write the config
 * file, so the app has to wait out BUSY the same way it does for an update. */
enum {
    GC_LOG_ST_IDLE = 0,
    GC_LOG_ST_BUSY,
    GC_LOG_ST_OK,
    GC_LOG_ST_ERROR,
};

/* All three called straight from the RMT ISR. */
void gc_log_cmd(const uint8_t *payload);
void gc_log_status(uint8_t *out);
void gc_log_read(uint16_t chunk, uint8_t *out);

void gc_log_init(void);

#endif /* _GC_LOG_H_ */
