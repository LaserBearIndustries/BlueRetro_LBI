/*
 * Copyright (c) 2021-2025, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MEMORY_CARD_H_
#define _MEMORY_CARD_H_
#include "sdkconfig.h"
#include "adapter.h"

/* sdkconfig.h is included here rather than left to whoever includes this,
 * because the size below depends on it and every file that touches the
 * buffer has to agree on it. One translation unit seeing 128 KB while
 * another sees 96 KB would be a bounds check against the wrong end of an
 * allocation, which is not a thing to leave to include order. */

/* An emulated memory card, held in RAM and mirrored to a file. It backs the
 * N64 Controller Pak, the PlayStation memory card and the Dreamcast VMU -
 * all of which live on the controller bus and are all exactly 128 KB - and,
 * when banksel is CONFIG_BANKSEL_DBG, doubles as the ring the Bluetooth
 * trace is written into.
 *
 * The GameCube keeps its memory cards on EXI rather than the controller
 * port, so a GC build never emulates one and the buffer is only ever the
 * trace ring. 96 KB of trace is a great deal of HCI - the download reports
 * bt_mon_get_log_len(), what was actually captured, so a smaller ring costs
 * depth and nothing else - and the 32 KB it gives back is heap this
 * firmware very much needs: hid_parser could not find 788 contiguous bytes
 * to parse a controller descriptor, and a controller that pairs but never
 * reaches the console is indistinguishable from a dead adapter.
 *
 * Only for GC. Anything that emulates a real card needs all 128 KB, and a
 * universal build does not know yet which it will be. */
#if defined(CONFIG_BLUERETRO_SYSTEM_GC)
#define MC_BUFFER_SIZE (96 * 1024)
#else
#define MC_BUFFER_SIZE (128 * 1024)
#endif
#define MC_BUFFER_BLOCK_SIZE (4 * 1024)
#define MC_BUFFER_BLOCK_CNT (MC_BUFFER_SIZE / MC_BUFFER_BLOCK_SIZE)

int32_t mc_init(void);
int32_t mc_init_mem(void);
void mc_storage_update(void);
void mc_read(uint32_t addr, uint8_t *data, uint32_t size);
void mc_write(uint32_t addr, uint8_t *data, uint32_t size);
uint8_t *mc_get_ptr(uint32_t addr);
uint32_t mc_get_state(void);
bool mc_get_ready(void);

#endif /* _MEMORY_CARD_H_ */
