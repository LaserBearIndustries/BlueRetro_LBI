/*
 * Copyright (c) 2021-2025, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MEMORY_CARD_H_
#define _MEMORY_CARD_H_
#include "sdkconfig.h"
#include "adapter.h"

/* An emulated memory card, held in RAM and mirrored to a file. It backs the
 * N64 Controller Pak, the PlayStation memory card and the Dreamcast VMU -
 * all of which live on the controller bus and are all exactly 128 KB - and,
 * when banksel is CONFIG_BANKSEL_DBG, doubles as the ring the Bluetooth
 * trace is written into.
 *
 * This was 96 KB on a GameCube build for a while, to give 32 KB back to a
 * heap that badly needed it. The reasoning was that the GameCube keeps its
 * memory cards on EXI rather than the controller port, so a GC build never
 * emulates one and the buffer is only ever the trace ring.
 *
 * That stopped being true. A GC build drives N64 controllers now, and an
 * N64 controller brings a Controller Pak, which is emulated in here. The
 * pak path banks four of them across the whole buffer:
 *
 *     addr += ((channel + ctrl_mem_banksel) & 0x3) * 32 * 1024;
 *
 * so it addresses up to 128 KB by design. At 96 KB the top bank indexes
 * past the end of mc_buffer[], which is a stray pointer dereferenced from
 * an interrupt rather than a shorter trace.
 *
 * The heap pressure was real and is still real, but it was answered
 * properly in the meantime - task stacks were cut to what they use, and
 * the tasks are created before this claims its share. Taking 32 KB from a
 * buffer that something else was addressing was never the right half of
 * that trade.
 *
 * One size everywhere now, so nothing depends on include order to agree
 * about it. */
#define MC_BUFFER_SIZE (128 * 1024)
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
