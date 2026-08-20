/*
 * Copyright (c) 2021-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include "adapter/adapter.h"
#include "wired_rtos.h"

/* As in wired_bare.c: a driver the selected system does not need is not
 * compiled, and its table slots resolve to NULL.
 */
#ifdef CONFIG_BLUERETRO_DRV_PARALLEL
#include "parallel.h"
#else
#define parallel_io_init NULL
#endif
#ifdef CONFIG_BLUERETRO_DRV_SEA_IO
#include "sea_io.h"
#else
#define sea_init NULL
#endif

typedef void (*wired_init_t)(void);

static const wired_init_t wired_init[WIRED_MAX] = {
    NULL, /* WIRED_AUTO */
    parallel_io_init, /* PARALLEL_1P */
    parallel_io_init, /* PARALLEL_2P */
    NULL, /* NES */
    NULL, /* PCE */
    NULL, /* GENESIS */
    NULL, /* SNES */
    NULL, /* CDI */
    NULL, /* CD32 */
    NULL, /* REAL_3DO */
    NULL, /* JAGUAR */
    NULL, /* PSX */
    NULL, /* SATURN */
    NULL, /* PCFX */
    NULL, /* JVS */
    NULL, /* N64 */
    NULL, /* DC */
    NULL, /* PS2 */
    NULL, /* GC */
    NULL, /* WII_EXT */
    NULL, /* VB */
    parallel_io_init, /* PARALLEL_1P_OD */
    parallel_io_init, /* PARALLEL_2P_OD */
    sea_init, /* SEA_BOARD */
};

void wired_rtos_init(void) {
    if (wired_init[wired_adapter.system_id]) {
        wired_init[wired_adapter.system_id]();
    }
}

