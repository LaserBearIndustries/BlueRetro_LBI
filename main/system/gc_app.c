/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include "adapter/adapter.h"
#include "adapter/config.h"
#include "adapter/gameid.h"
#include "bluetooth/host.h"
#include "gc_app.h"

/* A snapshot rather than a pointer to ctrl_input. That buffer is scratch space
 * the bridge reuses for whichever device reported last, and it is filled while
 * the ISR could be reading it. Copying a compact form out at bridge time gives
 * the ISR something stable, in DRAM, and unambiguous about which device it
 * describes. */
static volatile uint8_t input_snap[GC_APP_INPUT_LEN] = { GC_APP_NO_DEV };

/* Mapping is staged here rather than written straight into config.in_cfg. The
 * bridge reads that on every controller report, so writing it entry by entry
 * would put half a mapping in front of the player, and persisting it needs a
 * flash write the ISR cannot do anyway. */
static struct map_cfg stage[ADAPTER_MAPPING_MAX];
static volatile uint8_t stage_port = 0;
static volatile uint8_t stage_cnt = 0;
static volatile uint8_t stage_scope = GC_APP_SCOPE_GLOBAL;
static volatile uint8_t map_status = GC_APP_ST_IDLE;

/* DRAM copy of the current game id, refreshed by the task. The ISR cannot read
 * gid_get()'s storage, which is flash mapped and gone during an OTA write. */
static char gid_snap[GC_APP_GID_LEN] = {0};

enum {
    GC_APP_REQ_NONE = 0,
    GC_APP_REQ_COMMIT,
    GC_APP_REQ_MUTE,
};
static volatile uint8_t app_req = GC_APP_REQ_NONE;

/* Port whose output is held neutral for capture, or GC_APP_NO_DEV for none. */
static volatile uint8_t mute_port = GC_APP_NO_DEV;

/* Generic axis indices, in the order the app expects them. */
static const uint8_t gc_app_axis_idx[GC_APP_AXIS_CNT] = {
    AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY, TRIG_L, TRIG_R,
};

/* Axes are raw controller units with a per device range, so normalise to a
 * signed scale the app can draw without knowing the source. */
static int8_t axis_to_int8(struct ctrl_axis *axis) {
    int32_t max, val;

    if (axis == NULL || axis->meta == NULL) {
        return 0;
    }

    val = axis->value;
    max = (val < 0) ? axis->meta->abs_min : axis->meta->abs_max;
    if (max <= 0) {
        return 0;
    }

    val = (val * 127) / max;
    if (val > 127) {
        val = 127;
    }
    else if (val < -127) {
        val = -127;
    }
    return (int8_t)val;
}

void gc_app_input_update(uint8_t dev_id, struct wireless_ctrl *ctrl) {
    uint8_t tmp[GC_APP_INPUT_LEN];
    uint32_t btns;

    if (ctrl == NULL) {
        return;
    }

    btns = (uint32_t)ctrl->btns[0].value;

    tmp[0] = dev_id;
    tmp[1] = map_status | ((mute_port != GC_APP_NO_DEV) ? GC_APP_ST_MUTED : 0);
    tmp[2] = (uint8_t)(btns >> 0);
    tmp[3] = (uint8_t)(btns >> 8);
    tmp[4] = (uint8_t)(btns >> 16);
    tmp[5] = (uint8_t)(btns >> 24);

    for (uint32_t i = 0; i < GC_APP_AXIS_CNT; i++) {
        tmp[6 + i] = (uint8_t)axis_to_int8(&ctrl->axes[gc_app_axis_idx[i]]);
    }

    /* Single pass copy. The ISR can still catch this mid update and see a mix
     * of two reports, which for a live readout costs at most one frame of
     * wrong pixels and is not worth locking the Bluetooth path for. */
    memcpy((void *)input_snap, tmp, sizeof(tmp));
}

uint32_t gc_app_is_muted(uint8_t out_idx) {
    return (mute_port == out_idx);
}

void IRAM_ATTR gc_app_input_read(uint8_t *out) {
    for (uint32_t i = 0; i < GC_APP_INPUT_LEN; i++) {
        out[i] = input_snap[i];
    }
}

/* Mirrored into DRAM rather than read from gid_get() here, because this is
 * reached from the ISR and the game id lives in flash mapped memory. */
void IRAM_ATTR gc_app_gameid(uint8_t chunk, uint8_t *out) {
    uint32_t off = (uint32_t)chunk * GC_APP_GID_CHUNK;

    for (uint32_t i = 0; i < GC_APP_GID_CHUNK; i++) {
        out[i] = ((off + i) < GC_APP_GID_LEN) ? gid_snap[off + i] : 0;
    }
}

void IRAM_ATTR gc_app_map_cmd(const uint8_t *payload) {
    uint8_t idx;

    switch (payload[0]) {
        case GC_APP_MAP_BEGIN:
            if (payload[1] < WIRED_MAX_DEV) {
                stage_port = payload[1];
                stage_cnt = 0;
                map_status = GC_APP_ST_IDLE;
            }
            break;
        case GC_APP_MAP_SET:
            idx = payload[1];
            if (idx < ADAPTER_MAPPING_MAX) {
                stage[idx].src_btn = payload[2];
                stage[idx].dst_btn = payload[3];
                stage[idx].dst_id = payload[4];
                stage[idx].perc_max = payload[5];
                stage[idx].perc_threshold = payload[6];
                stage[idx].perc_deadzone = payload[7];
                stage[idx].turbo = payload[8];
                stage[idx].algo = 0;
                if (idx >= stage_cnt) {
                    stage_cnt = idx + 1;
                }
            }
            break;
        case GC_APP_MAP_COMMIT:
            if (payload[1] <= ADAPTER_MAPPING_MAX && map_status != GC_APP_ST_BUSY) {
                stage_cnt = payload[1];
                stage_scope = payload[2];
                map_status = GC_APP_ST_BUSY;
                app_req = GC_APP_REQ_COMMIT;
            }
            break;
        case GC_APP_MAP_CANCEL:
            stage_cnt = 0;
            map_status = GC_APP_ST_IDLE;
            break;
    }
}

void IRAM_ATTR gc_app_mode_cmd(const uint8_t *payload) {
    if (payload[0]) {
        if (payload[1] < WIRED_MAX_DEV) {
            mute_port = payload[1];
            /* Ask the task to park the wired output, so whatever was held when
             * capture started does not stay held for the whole session. */
            app_req = GC_APP_REQ_MUTE;
        }
    }
    else {
        uint8_t was = mute_port;

        mute_port = GC_APP_NO_DEV;
        if (was != GC_APP_NO_DEV) {
            app_req = GC_APP_REQ_MUTE;
        }
    }
}

static void gc_app_task(void *arg) {
    while (1) {
        uint8_t req = app_req;

        if (req != GC_APP_REQ_NONE) {
            app_req = GC_APP_REQ_NONE;

            switch (req) {
                case GC_APP_REQ_COMMIT:
                {
                    uint8_t port = stage_port;
                    uint8_t cnt = stage_cnt;

                    if (port < WIRED_MAX_DEV && cnt <= ADAPTER_MAPPING_MAX) {
                        memcpy(config.in_cfg[port].map_cfg, stage,
                            cnt * sizeof(struct map_cfg));
                        config.in_cfg[port].map_size = cnt;
                        config_update(DEFAULT_CFG);
                        map_status = GC_APP_ST_OK;
                        printf("# %s: port %u mapping committed, %u entries\n",
                            __FUNCTION__, port, cnt);
#ifdef CONFIG_BLUERETRO_CTRL_MAP
                        /* Also keep it against the controller itself, so it
                         * follows the pad rather than the port it happened to
                         * land on this session. */
                        {
                            struct bt_dev *dev = NULL;

                            if (bt_host_get_dev_from_out_idx(port, &dev) >= 0) {
                                config_save_ctrl_map(port, bt_host_dev_bdaddr(dev),
                                    stage_scope);
                            }
                        }
#endif
                    }
                    else {
                        map_status = GC_APP_ST_ERROR;
                        printf("# %s: bad commit, port %u cnt %u\n",
                            __FUNCTION__, port, cnt);
                    }
                    break;
                }
                case GC_APP_REQ_MUTE:
                    /* Reset to neutral on the way in and on the way out: going
                     * in so nothing sticks, coming out so the console does not
                     * inherit whatever was last staged. */
                    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
                        adapter_init_buffer(i);
                    }
                    printf("# %s: capture mute %s\n", __FUNCTION__,
                        (mute_port == GC_APP_NO_DEV) ? "off" : "on");
                    break;
            }
        }

        /* Cheap, and it means the ISR never has to reach into flash mapped
         * memory for a string that changes when a game boots. */
        strncpy(gid_snap, gid_get(), sizeof(gid_snap) - 1);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void gc_app_init(void) {
    memset(stage, 0, sizeof(stage));
    xTaskCreatePinnedToCore(gc_app_task, "gc_app_task", 4096, NULL, 5, NULL, 0);
}
