/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_ota_ops.h>
#include "system/manager.h"
#include "gc_ota.h"

/* Firmware is staged in RAM and flushed a chunk at a time. esp_ota_write()
 * erases flash, which stalls cache for tens of milliseconds, and the SI link
 * cannot be serviced while that happens. So the ISR never writes flash: it fills
 * this buffer and hands the work to the task, reporting BUSY until the flush is
 * done. The console is expected to poll the status command and wait. */
#define GC_OTA_CHUNK 1024

enum {
    GC_OTA_REQ_NONE = 0,
    GC_OTA_REQ_START,
    GC_OTA_REQ_FLUSH,
    GC_OTA_REQ_END,
    GC_OTA_REQ_ABORT,
};

/* Written by the ISR, read by the task. The ISR only ever hands over a request
 * after moving the state to BUSY, and only ever raises one when the state is not
 * BUSY, so the two never work on the buffer at the same time. */
static volatile uint8_t ota_state = GC_OTA_IDLE;
static volatile uint8_t ota_req = GC_OTA_REQ_NONE;
static volatile uint8_t ota_seq = 0;
static volatile uint32_t ota_len = 0;
static uint8_t ota_buf[GC_OTA_CHUNK];

/* Task side only. */
static esp_ota_handle_t ota_hdl = 0;
static const esp_partition_t *ota_part = NULL;

void IRAM_ATTR gc_ota_cmd(const uint8_t *payload) {
    uint8_t sub = payload[0];
    uint8_t seq = payload[1];

    switch (sub) {
        case GC_OTA_SUB_START:
            if (ota_state == GC_OTA_IDLE || ota_state == GC_OTA_ERROR
                    || ota_state == GC_OTA_DONE) {
                ota_state = GC_OTA_BUSY;
                ota_req = GC_OTA_REQ_START;
            }
            break;
        case GC_OTA_SUB_DATA:
            if (ota_state == GC_OTA_READY) {
                if (seq == (uint8_t)(ota_seq + 1)
                        && (ota_len + GC_OTA_DATA_LEN) <= GC_OTA_CHUNK) {
                    for (uint32_t i = 0; i < GC_OTA_DATA_LEN; i++) {
                        ota_buf[ota_len + i] = payload[2 + i];
                    }
                    ota_len += GC_OTA_DATA_LEN;
                    ota_seq = seq;
                }
                /* A repeated seq means the console never saw the status that
                 * acked it. Silently dropping the duplicate is what makes the
                 * transfer safe to retry. */

                if ((ota_len + GC_OTA_DATA_LEN) > GC_OTA_CHUNK) {
                    /* No room for another frame, flush before taking more. */
                    ota_state = GC_OTA_BUSY;
                    ota_req = GC_OTA_REQ_FLUSH;
                }
            }
            break;
        case GC_OTA_SUB_END:
            if (ota_state == GC_OTA_READY) {
                ota_state = GC_OTA_BUSY;
                ota_req = GC_OTA_REQ_END;
            }
            break;
        case GC_OTA_SUB_ABORT:
            if (ota_state != GC_OTA_IDLE) {
                ota_state = GC_OTA_BUSY;
                ota_req = GC_OTA_REQ_ABORT;
            }
            break;
    }
}

void IRAM_ATTR gc_ota_status(uint8_t *status) {
    status[0] = ota_state;
    status[1] = ota_seq;
    status[2] = GC_OTA_PROTO_VER;
}

static void gc_ota_task(void *arg) {
    while (1) {
        uint8_t req = ota_req;

        if (req != GC_OTA_REQ_NONE) {
            ota_req = GC_OTA_REQ_NONE;

            switch (req) {
                case GC_OTA_REQ_START:
                    ota_part = esp_ota_get_next_update_partition(NULL);
                    if (ota_part && esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &ota_hdl) == ESP_OK) {
                        ota_len = 0;
                        ota_seq = 0;
                        ota_state = GC_OTA_READY;
                        printf("# %s: started on %s\n", __FUNCTION__, ota_part->label);
                    }
                    else {
                        ota_hdl = 0;
                        ota_state = GC_OTA_ERROR;
                        printf("# %s: begin failed\n", __FUNCTION__);
                    }
                    break;
                case GC_OTA_REQ_FLUSH:
                    if (esp_ota_write(ota_hdl, ota_buf, ota_len) == ESP_OK) {
                        ota_len = 0;
                        ota_state = GC_OTA_READY;
                    }
                    else {
                        ota_state = GC_OTA_ERROR;
                        printf("# %s: write failed\n", __FUNCTION__);
                    }
                    break;
                case GC_OTA_REQ_END:
                    if (ota_len && esp_ota_write(ota_hdl, ota_buf, ota_len) != ESP_OK) {
                        ota_state = GC_OTA_ERROR;
                        printf("# %s: tail write failed\n", __FUNCTION__);
                        break;
                    }
                    ota_len = 0;
                    if (esp_ota_end(ota_hdl) == ESP_OK
                            && esp_ota_set_boot_partition(ota_part) == ESP_OK) {
                        ota_hdl = 0;
                        ota_state = GC_OTA_DONE;
                        printf("# %s: complete, restarting\n", __FUNCTION__);
                        /* Deferred restart, so the console still gets to read
                         * DONE back before we go away. */
                        sys_mgr_cmd(SYS_MGR_CMD_ADAPTER_RST);
                    }
                    else {
                        ota_state = GC_OTA_ERROR;
                        printf("# %s: end failed\n", __FUNCTION__);
                    }
                    break;
                case GC_OTA_REQ_ABORT:
                    if (ota_hdl) {
                        esp_ota_abort(ota_hdl);
                        ota_hdl = 0;
                    }
                    ota_len = 0;
                    ota_state = GC_OTA_IDLE;
                    printf("# %s: aborted\n", __FUNCTION__);
                    break;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_ota_init(void) {
    xTaskCreatePinnedToCore(gc_ota_task, "gc_ota_task", 4096, NULL, 5, NULL, 0);
}
