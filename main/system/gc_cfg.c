/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "bluetooth/host.h"
#include "gc_cfg.h"

#define GC_CFG_REQ_NONE 0xFF

#define GC_CFG_CHUNK_CNT \
    ((sizeof(struct config) + GC_CFG_CHUNK - 1) / GC_CFG_CHUNK)
#define GC_CFG_BITMAP_LEN ((GC_CFG_CHUNK_CNT + 7) / 8)

/* Staged in full before any of it is adopted. A restore interrupted partway
 * would otherwise leave the adapter running half of one config and half of
 * another, which is worse than either. */
static uint8_t stage[sizeof(struct config)];

/* Which chunks actually arrived. Two jobs: a dropped transaction in the middle
 * of a restore would otherwise be adopted as a config with a hole in it,
 * wherever the transfer happened to stumble; and requiring every chunk is what
 * makes clearing the staging buffer unnecessary, since nothing left over in it
 * can reach the config. */
static uint8_t received[GC_CFG_BITMAP_LEN];

static volatile uint8_t cfg_state = GC_CFG_ST_IDLE;
static volatile uint8_t cfg_req = GC_CFG_REQ_NONE;
static volatile uint8_t cfg_why = GC_CFG_WHY_NONE;
static volatile uint16_t cfg_detail = 0;

/* Bumped every pass of the task. Reported so the console can tell a task
 * that is working slowly from one that is not running at all, which are
 * indistinguishable from the outside and have nothing in common. */
static volatile uint8_t cfg_tick = 0;

void IRAM_ATTR gc_cfg_info(uint8_t *out) {
    uint32_t size = sizeof(struct config);

    out[0] = GC_CFG_PROTO_VER;
    out[1] = cfg_state;
    out[2] = (uint8_t)size;
    out[3] = (uint8_t)(size >> 8);
    out[4] = cfg_why;
    out[5] = cfg_tick;
    out[6] = (uint8_t)cfg_detail;
    out[7] = (uint8_t)(cfg_detail >> 8);
}

void IRAM_ATTR gc_cfg_read(uint16_t chunk, uint8_t *out) {
    uint32_t addr = (uint32_t)chunk * GC_CFG_CHUNK;
    const uint8_t *src = (const uint8_t *)&config;
    uint32_t i;

    /* Read straight out of the live struct rather than a copy. It is what the
     * adapter is actually running, which is the thing worth backing up. */
    for (i = 0; i < GC_CFG_CHUNK; i++) {
        out[i] = ((addr + i) < sizeof(struct config)) ? src[addr + i] : 0;
    }
}

void IRAM_ATTR gc_cfg_write(const uint8_t *payload) {
    uint32_t chunk = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8);
    uint32_t addr = chunk * GC_CFG_CHUNK;
    uint32_t i;

    if (cfg_state != GC_CFG_ST_READY || chunk >= GC_CFG_CHUNK_CNT) {
        return;
    }

    for (i = 0; i < GC_CFG_CHUNK; i++) {
        if ((addr + i) < sizeof(stage)) {
            stage[addr + i] = payload[2 + i];
        }
    }
    received[chunk >> 3] |= (uint8_t)(1 << (chunk & 7));
}

void IRAM_ATTR gc_cfg_cmd(const uint8_t *payload) {
    switch (payload[0]) {
        case GC_CFG_SUB_BEGIN:
            /* Only the arrival bitmap is cleared, not the twelve kilobytes it
             * describes. Nothing is adopted unless every chunk arrived, so
             * whatever the buffer held before cannot survive into a restore,
             * which makes wiping it work for its own sake.
             *
             * Two hundred bytes is little enough to do here, and doing it here
             * means a restore no longer depends on a task running between the
             * start command and the first chunk. */
            memset(received, 0, sizeof(received));
            cfg_why = GC_CFG_WHY_NONE;
            cfg_detail = 0;
            cfg_state = GC_CFG_ST_READY;
            break;
        case GC_CFG_SUB_APPLY:
            /* Handed over, because adopting it writes flash. */
            if (cfg_state == GC_CFG_ST_READY) {
                cfg_state = GC_CFG_ST_BUSY;
                cfg_req = GC_CFG_SUB_APPLY;
            }
            break;
        case GC_CFG_SUB_ABORT:
            cfg_state = GC_CFG_ST_IDLE;
            break;
    }
}

/* Returns the first chunk that never arrived, or 0xFFFF if none did. */
static uint16_t gc_cfg_first_missing(void) {
    uint32_t i;

    for (i = 0; i < GC_CFG_CHUNK_CNT; i++) {
        if (!(received[i >> 3] & (1 << (i & 7)))) {
            return (uint16_t)i;
        }
    }
    return 0xFFFF;
}

static uint32_t gc_cfg_staged_is_sane(void) {
    const struct config *in = (const struct config *)stage;
    uint32_t i;

    uint16_t missing = gc_cfg_first_missing();

    if (missing != 0xFFFF) {
        cfg_why = GC_CFG_WHY_MISSING_CHUNK;
        cfg_detail = missing;
        printf("# %s: chunk %u never arrived\n", __FUNCTION__, missing);
        return 0;
    }

    /* Refused rather than migrated. The version update path in config.c works
     * on a file it can rewrite in place, and an adapter is a poor place to
     * discover that an old backup needed converting. */
    if (in->magic != CONFIG_MAGIC) {
        cfg_why = GC_CFG_WHY_BAD_MAGIC;
        /* The low half distinguishes a buffer that stayed empty from one that
         * received something other than what was sent. */
        cfg_detail = (uint16_t)in->magic;
        printf("# %s: magic %08lX, expected %08X\n", __FUNCTION__,
            (unsigned long)in->magic, CONFIG_MAGIC);
        return 0;
    }

    for (i = 0; i < WIRED_MAX_DEV; i++) {
        if (in->in_cfg[i].map_size > ADAPTER_MAPPING_MAX) {
            cfg_why = GC_CFG_WHY_BAD_MAP_SIZE;
            cfg_detail = (uint16_t)i;
            printf("# %s: port %lu map_size %u\n", __FUNCTION__,
                i, in->in_cfg[i].map_size);
            return 0;
        }
    }
    return 1;
}

static void gc_cfg_task(void *arg) {
    while (1) {
        uint8_t req = cfg_req;

        cfg_tick++;

        if (req != GC_CFG_REQ_NONE) {
            cfg_req = GC_CFG_REQ_NONE;

            if (gc_cfg_staged_is_sane()) {
                memcpy(&config, stage, sizeof(config));
                config_update(DEFAULT_CFG);

                /* The mapping arrays just changed underneath every connected
                 * controller, same as a game id change does. */
                bt_host_reload_ctrl_maps();

                cfg_state = GC_CFG_ST_OK;
                printf("# %s: settings restored\n", __FUNCTION__);
            }
            else {
                cfg_state = GC_CFG_ST_ERROR;
                printf("# %s: staged config rejected, keeping current\n", __FUNCTION__);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_cfg_init(void) {
    /* Reported over the wire, not just logged. Without the task nothing ever
     * adopts a config, so every restore waits on something that will never
     * happen, and from the console that is indistinguishable from a refusal.
     * It presented as one for four rounds. */
    if (xTaskCreatePinnedToCore(gc_cfg_task, "gc_cfg_task", 4096, NULL, 5, NULL, 0)
            != pdPASS) {
        cfg_state = GC_CFG_ST_ERROR;
        cfg_why = GC_CFG_WHY_NO_TASK;
        printf("# %s: task create failed, settings transfer unavailable\n",
            __FUNCTION__);
        return;
    }
    printf("# %s: ready, %u byte config in %u chunks\n", __FUNCTION__,
        (unsigned)sizeof(struct config), (unsigned)GC_CFG_CHUNK_CNT);
}
