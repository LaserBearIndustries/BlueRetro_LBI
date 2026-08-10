/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "system/manager.h"
#include "gc_boot.h"

#define GC_BOOT_REQ_NONE 0xFF

/* Everything the ISR answers from is cached here at boot. Reading a partition
 * means touching flash, which cannot happen from the ISR at all: cache is
 * disabled during an OTA write and the read would fault. The slot contents
 * cannot change while we are running anyway, bar the boot selection itself. */
static struct {
    char version[GC_BOOT_VER_LEN];
    uint8_t valid;
} slot_info[GC_BOOT_SLOT_CNT];

static volatile uint8_t boot_running = GC_BOOT_SLOT_FACTORY;
static volatile uint8_t boot_next = GC_BOOT_SLOT_FACTORY;
static volatile uint8_t boot_valid_mask = 0;
static volatile uint8_t boot_state = GC_BOOT_ST_IDLE;
static volatile uint8_t boot_req = GC_BOOT_REQ_NONE;

static const esp_partition_t *gc_boot_partition(uint8_t slot) {
    esp_partition_subtype_t sub;

    switch (slot) {
        case GC_BOOT_SLOT_FACTORY:
            sub = ESP_PARTITION_SUBTYPE_APP_FACTORY;
            break;
        case GC_BOOT_SLOT_OTA_0:
            sub = ESP_PARTITION_SUBTYPE_APP_OTA_0;
            break;
        case GC_BOOT_SLOT_OTA_1:
            sub = ESP_PARTITION_SUBTYPE_APP_OTA_1;
            break;
        default:
            return NULL;
    }
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
}

static int32_t gc_boot_slot_of(const esp_partition_t *part) {
    uint32_t i;

    if (part) {
        for (i = 0; i < GC_BOOT_SLOT_CNT; i++) {
            if (gc_boot_partition(i) == part) {
                return i;
            }
        }
    }
    return -1;
}

void IRAM_ATTR gc_boot_info(uint8_t *out) {
    out[0] = GC_BOOT_PROTO_VER;
    out[1] = boot_running;
    out[2] = boot_next;
    out[3] = boot_valid_mask;
    out[4] = boot_state;
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
}

void IRAM_ATTR gc_boot_version(uint8_t slot, uint8_t chunk, uint8_t *out) {
    uint32_t off = (uint32_t)chunk * GC_BOOT_VER_CHUNK;
    uint32_t i;

    if (slot >= GC_BOOT_SLOT_CNT) {
        memset(out, 0, GC_BOOT_VER_CHUNK);
        return;
    }

    for (i = 0; i < GC_BOOT_VER_CHUNK; i++) {
        out[i] = ((off + i) < GC_BOOT_VER_LEN) ? slot_info[slot].version[off + i] : 0;
    }
}

void IRAM_ATTR gc_boot_select(const uint8_t *payload) {
    uint8_t slot = payload[0];

    /* Refuse a slot we could not read an image out of rather than handing the
     * bootloader something that will not come back. */
    if (slot >= GC_BOOT_SLOT_CNT || !(boot_valid_mask & (1 << slot))) {
        boot_state = GC_BOOT_ST_ERROR;
        return;
    }

    boot_state = GC_BOOT_ST_BUSY;
    boot_req = slot;
}

static void gc_boot_task(void *arg) {
    while (1) {
        uint8_t req = boot_req;

        if (req != GC_BOOT_REQ_NONE) {
            const esp_partition_t *part = gc_boot_partition(req);

            boot_req = GC_BOOT_REQ_NONE;

            /* esp_ota_set_boot_partition() verifies the image itself before it
             * commits, so a slot that would not boot is refused here too. */
            if (part && esp_ota_set_boot_partition(part) == ESP_OK) {
                boot_next = req;
                boot_state = GC_BOOT_ST_OK;
                printf("# %s: booting slot %u next, restarting\n", __FUNCTION__, req);
                /* Deferred, so the console still gets to read the result back
                 * before we go away. Same path the updater uses. */
                sys_mgr_cmd(SYS_MGR_CMD_ADAPTER_RST);
            }
            else {
                boot_state = GC_BOOT_ST_ERROR;
                printf("# %s: could not select slot %u\n", __FUNCTION__, req);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_boot_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_boot_partition();
    int32_t slot;
    uint32_t i;

    memset(slot_info, 0, sizeof(slot_info));

    for (i = 0; i < GC_BOOT_SLOT_CNT; i++) {
        const esp_partition_t *part = gc_boot_partition(i);
        esp_app_desc_t desc;

        if (part && esp_ota_get_partition_description(part, &desc) == ESP_OK) {
            memcpy(slot_info[i].version, desc.version, sizeof(slot_info[i].version));
            slot_info[i].valid = 1;
            boot_valid_mask |= (1 << i);
        }
    }

    slot = gc_boot_slot_of(running);
    boot_running = (slot < 0) ? GC_BOOT_SLOT_FACTORY : (uint8_t)slot;
    slot = gc_boot_slot_of(next);
    boot_next = (slot < 0) ? boot_running : (uint8_t)slot;

    printf("# %s: running slot %u, valid mask 0x%02X\n", __FUNCTION__,
        boot_running, boot_valid_mask);

    xTaskCreatePinnedToCore(gc_boot_task, "gc_boot_task", 4096, NULL, 5, NULL, 0);
}
