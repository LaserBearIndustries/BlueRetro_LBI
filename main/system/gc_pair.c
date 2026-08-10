/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "bluetooth/host.h"
#include "gc_pair.h"

#define GC_PAIR_REQ_NONE 0xFF

/* DRAM mirror of the list, refreshed by the task. The key stores and everything
 * that reads them live in flash mapped memory, which is gone from under an
 * interrupt while flash is being written. */
static uint8_t snap[GC_PAIR_MAX][GC_PAIR_ENTRY_LEN];
static volatile uint8_t snap_cnt = 0;

static volatile uint8_t pair_state = GC_PAIR_ST_IDLE;
static volatile uint8_t pair_req = GC_PAIR_REQ_NONE;
static volatile uint8_t pair_arg = 0;

/* Bumped every pass. A task grinding through flash and a task that never
 * started look identical from the console without it, and telling those apart
 * took four rounds once already. */
static volatile uint8_t pair_tick = 0;

void IRAM_ATTR gc_pair_info(uint8_t *out) {
    out[0] = GC_PAIR_PROTO_VER;
    out[1] = snap_cnt;
    out[2] = pair_state;
    out[3] = pair_tick;
    out[4] = 0;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

void IRAM_ATTR gc_pair_entry(uint8_t idx, uint8_t *out) {
    if (idx >= snap_cnt) {
        memset(out, 0, GC_PAIR_ENTRY_LEN);
        return;
    }
    memcpy(out, snap[idx], GC_PAIR_ENTRY_LEN);
}

void IRAM_ATTR gc_pair_cmd(const uint8_t *payload) {
    switch (payload[0]) {
        case GC_PAIR_SUB_FORGET:
        case GC_PAIR_SUB_FORGET_ALL:
            /* Handed over: both rewrite the key files. */
            if (pair_state != GC_PAIR_ST_BUSY) {
                pair_arg = payload[1];
                pair_state = GC_PAIR_ST_BUSY;
                pair_req = payload[0];
            }
            break;
    }
}

static void gc_pair_refresh(void) {
    uint32_t cnt = bt_host_pair_cnt();
    uint32_t n = 0;

    if (cnt > GC_PAIR_MAX) {
        cnt = GC_PAIR_MAX;
    }

    for (uint32_t i = 0; i < cnt; i++) {
        uint8_t is_le = 0, bdaddr[6] = {0};
        int32_t port;

        if (bt_host_pair_get(i, &is_le, bdaddr) < 0) {
            continue;
        }

        port = bt_host_pair_port(bdaddr);

        snap[n][0] = is_le;
        snap[n][1] = (port < 0) ? GC_PAIR_NO_PORT : (uint8_t)port;
        memcpy(&snap[n][2], bdaddr, 6);
        n++;
    }
    snap_cnt = (uint8_t)n;
}

static void gc_pair_task(void *arg) {
    uint32_t refresh = 0;

    while (1) {
        uint8_t req = pair_req;

        pair_tick++;

        if (req != GC_PAIR_REQ_NONE) {
            pair_req = GC_PAIR_REQ_NONE;

            if (req == GC_PAIR_SUB_FORGET_ALL) {
                bt_host_pair_forget_all();
                pair_state = GC_PAIR_ST_OK;
            }
            else if (bt_host_pair_forget(pair_arg) == 0) {
                pair_state = GC_PAIR_ST_OK;
            }
            else {
                pair_state = GC_PAIR_ST_ERROR;
            }
            gc_pair_refresh();
        }
        else if (refresh++ >= 50) {
            /* Twice a second is enough for a list someone is reading, and it
             * keeps the connected column honest as controllers come and go. */
            refresh = 0;
            gc_pair_refresh();
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_pair_init(void) {
    if (xTaskCreatePinnedToCore(gc_pair_task, "gc_pair_task", 4096, NULL, 5, NULL, 0)
            != pdPASS) {
        pair_state = GC_PAIR_ST_ERROR;
        printf("# %s: task create failed, pairing list unavailable\n", __FUNCTION__);
        return;
    }
    printf("# %s: ready\n", __FUNCTION__);
}
