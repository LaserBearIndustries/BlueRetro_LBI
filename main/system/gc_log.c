/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "adapter/config.h"
#include "adapter/memory_card.h"
#include "bluetooth/mon.h"
#include "gc_log.h"

#define GC_LOG_REQ_NONE 0xFF

/* Touched from the ISR, so both live in DRAM rather than flash. */
static volatile uint8_t log_req = GC_LOG_REQ_NONE;
static volatile uint8_t log_state = GC_LOG_ST_IDLE;

void IRAM_ATTR gc_log_cmd(const uint8_t *payload) {
    switch (payload[0]) {
        case GC_LOG_SUB_BANK_OFF:
        case GC_LOG_SUB_BANK_ON:
            /* Persisting means writing the config file, which stalls cache and
             * cannot happen here. Hand it to the task and report BUSY until it
             * lands. */
            log_state = GC_LOG_ST_BUSY;
            log_req = payload[0];
            break;
        case GC_LOG_SUB_RESET:
            /* Just an offset, safe to do inline. */
            bt_mon_log_reset();
            log_state = GC_LOG_ST_OK;
            break;
        default:
            log_state = GC_LOG_ST_ERROR;
            break;
    }
}

void IRAM_ATTR gc_log_status(uint8_t *out) {
    uint32_t len = bt_mon_get_log_len();

    out[0] = GC_LOG_PROTO_VER;
    out[1] = (config.global_cfg.banksel == CONFIG_BANKSEL_DBG) ? 1 : 0;
    out[2] = log_state;
    out[3] = 0;
    out[4] = (uint8_t)len;
    out[5] = (uint8_t)(len >> 8);
    out[6] = (uint8_t)(len >> 16);
    out[7] = (uint8_t)(len >> 24);
}

void IRAM_ATTR gc_log_read(uint16_t chunk, uint8_t *out) {
    uint32_t addr = (uint32_t)chunk * GC_LOG_CHUNK;
    uint32_t len = bt_mon_get_log_len();

    /* Addressed rather than sequential on purpose. A cursor would turn one
     * dropped transaction into every later chunk landing at the wrong offset,
     * which is the failure the SW2 SPI reads already had to be hardened
     * against. Addressed means a lost chunk is just a re-read. */
    if (addr + GC_LOG_CHUNK > len) {
        memset(out, 0, GC_LOG_CHUNK);
        if (addr >= len) {
            return;
        }
        mc_read(addr, out, len - addr);
        return;
    }

    mc_read(addr, out, GC_LOG_CHUNK);
}

static void gc_log_task(void *arg) {
    while (1) {
        uint8_t req = log_req;

        if (req != GC_LOG_REQ_NONE) {
            log_req = GC_LOG_REQ_NONE;

            config.global_cfg.banksel = (req == GC_LOG_SUB_BANK_ON)
                ? CONFIG_BANKSEL_DBG : 0;
            config_update(DEFAULT_CFG);

            /* Entering the bank rewinds too. The write offset only ever grows,
             * so without this a second capture would append to the first and
             * run out of buffer partway through whatever we are chasing. */
            if (req == GC_LOG_SUB_BANK_ON) {
                bt_mon_log_reset();
            }

            printf("# %s: debug bank %s\n", __FUNCTION__,
                (req == GC_LOG_SUB_BANK_ON) ? "on" : "off");
            log_state = GC_LOG_ST_OK;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_log_init(void) {
    /* 2048 rather than 4096, measured rather than guessed: 420 bytes used at idle.
     *
     * These four tasks were given 4096 each without measuring, which is 16 KB
     * of heap against upstream's 13.5 KB for every task it has. Meanwhile
     * hid_parser could not find 788 contiguous bytes to parse a controller's
     * descriptor, and silently gave up - so the pad paired and did nothing.
     * See the heap notes in adapter.h. */
    xTaskCreatePinnedToCore(gc_log_task, "gc_log_task", 2048, NULL, 5, NULL, 0);
}
