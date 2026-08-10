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

/* Staged in full before any of it is adopted. A restore that was interrupted
 * partway would otherwise leave the adapter running half of one config and half
 * of another, which is worse than either. */
static uint8_t stage[sizeof(struct config)];
static volatile uint8_t cfg_state = GC_CFG_ST_IDLE;
static volatile uint8_t cfg_req = GC_CFG_REQ_NONE;

void IRAM_ATTR gc_cfg_info(uint8_t *out) {
    uint32_t size = sizeof(struct config);

    out[0] = GC_CFG_PROTO_VER;
    out[1] = cfg_state;
    out[2] = (uint8_t)size;
    out[3] = (uint8_t)(size >> 8);
    out[4] = (uint8_t)(size >> 16);
    out[5] = (uint8_t)(size >> 24);
    out[6] = 0;
    out[7] = 0;
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

    if (cfg_state != GC_CFG_ST_BUSY) {
        return;
    }

    for (i = 0; i < GC_CFG_CHUNK; i++) {
        if ((addr + i) < sizeof(stage)) {
            stage[addr + i] = payload[2 + i];
        }
    }
}

void IRAM_ATTR gc_cfg_cmd(const uint8_t *payload) {
    switch (payload[0]) {
        case GC_CFG_SUB_BEGIN:
            memset(stage, 0, sizeof(stage));
            cfg_state = GC_CFG_ST_BUSY;
            break;
        case GC_CFG_SUB_APPLY:
            if (cfg_state == GC_CFG_ST_BUSY) {
                cfg_req = GC_CFG_SUB_APPLY;
            }
            break;
        case GC_CFG_SUB_ABORT:
            cfg_state = GC_CFG_ST_IDLE;
            break;
    }
}

static uint32_t gc_cfg_staged_is_sane(void) {
    const struct config *in = (const struct config *)stage;
    uint32_t i;

    /* Refused rather than migrated. The version update path in config.c works
     * on a file it can rewrite in place, and an adapter is a poor place to
     * discover that an old backup needed converting. */
    if (in->magic != CONFIG_MAGIC) {
        printf("# %s: magic %08lX, expected %08X\n", __FUNCTION__,
            (unsigned long)in->magic, CONFIG_MAGIC);
        return 0;
    }

    for (i = 0; i < WIRED_MAX_DEV; i++) {
        if (in->in_cfg[i].map_size > ADAPTER_MAPPING_MAX) {
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
    xTaskCreatePinnedToCore(gc_cfg_task, "gc_cfg_task", 4096, NULL, 5, NULL, 0);
}
