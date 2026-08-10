/*
 * Copyright (c) 2019-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "gameid.h"
#include "tools/ps1_gameid.h"

#define GAME_ID_BUF_LEN 23

static char gameid[GAME_ID_BUF_LEN] = {0};
static char tmp_gameid[GAME_ID_BUF_LEN] = {0};

static void set_pfx_gameid(uint8_t *data, uint32_t len) {
    if (len * 2 >= sizeof(gameid)) {
        len = (sizeof(gameid) - 1) / 2;
    }

    snprintf(tmp_gameid, GAME_ID_BUF_LEN, "%02X%02X%02X%02X%02X%02X%02X%02X",
        data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
}

static void set_ps1_gameid(uint8_t *data, uint32_t len) {
    memcpy(tmp_gameid, data, len);
    ps1_gid_sanitize(tmp_gameid);
}

int32_t gid_update(struct raw_fb *fb_data) {
    memset(tmp_gameid, 0, sizeof(gameid));

    switch (wired_adapter.system_id) {
        case N64:
        case GC:
            set_pfx_gameid(fb_data->data, fb_data->header.data_len);
            break;
        case PSX:
        case PS2:
            set_ps1_gameid(fb_data->data, fb_data->header.data_len);
            break;
    }

    if (memcmp(gameid, tmp_gameid, sizeof(gameid)) != 0) {
        memcpy(gameid, tmp_gameid, sizeof(gameid));
        printf("# %s: %s\n", __FUNCTION__, gameid);
        return 1;
    }
    return 0;
}

int32_t gid_update_sys(struct raw_fb *fb_data) {
    memset(tmp_gameid, 0, sizeof(gameid));
    memcpy(tmp_gameid, fb_data->data, fb_data->header.data_len);

    if (memcmp(gameid, tmp_gameid, sizeof(gameid)) != 0) {
        memcpy(gameid, tmp_gameid, sizeof(gameid));
        printf("# %s: %s\n", __FUNCTION__, gameid);
        return 1;
    }
    return 0;
}

char *gid_get(void) {
    return gameid;
}

static char gid_hist[GID_HIST_MAX][GAME_ID_BUF_LEN] = {{0}};
static uint32_t gid_hist_used = 0;

/* Newest first, no duplicates. Relaunching the same thing repeatedly is normal,
 * and without the dedupe it would push everything worth remembering off the
 * end, which is exactly the entry anyone is here to find. */
void gid_hist_push(const char *gameid_in) {
    uint32_t i, slot = GID_HIST_MAX;

    if (gameid_in == NULL || !strlen(gameid_in)) {
        return;
    }

    for (i = 0; i < gid_hist_used; i++) {
        if (strncmp(gid_hist[i], gameid_in, GAME_ID_BUF_LEN) == 0) {
            if (i == 0) {
                return;
            }
            slot = i;
            break;
        }
    }

    if (slot == GID_HIST_MAX) {
        if (gid_hist_used < GID_HIST_MAX) {
            gid_hist_used++;
        }
        slot = gid_hist_used - 1;
    }

    for (i = slot; i > 0; i--) {
        memcpy(gid_hist[i], gid_hist[i - 1], GAME_ID_BUF_LEN);
    }
    memset(gid_hist[0], 0, GAME_ID_BUF_LEN);
    strncpy(gid_hist[0], gameid_in, GAME_ID_BUF_LEN - 1);

    printf("# %s: %s\n", __FUNCTION__, gid_hist[0]);
}

const char *gid_hist_get(uint32_t idx) {
    return (idx < gid_hist_used) ? gid_hist[idx] : "";
}

uint32_t gid_hist_cnt(void) {
    return gid_hist_used;
}
