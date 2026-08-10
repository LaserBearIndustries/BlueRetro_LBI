/*
 * Copyright (c) 2019-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "gameid.h"
#include "system/fs.h"
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
static uint32_t gid_app_marked = 1;

#define GID_HIST_MAGIC 0x54534948 /* HIST */
#define GID_HIST_VER 1

struct gid_hist_file {
    uint32_t magic;
    uint8_t version;
    uint8_t used;
    uint8_t reserved[2];
    char entry[GID_HIST_MAX][GAME_ID_BUF_LEN];
} __attribute__((packed));

/* Written on every change, which is once per game launch. Rare enough that the
 * flash wear is not worth thinking about, and losing the list is precisely the
 * failure this exists to prevent. */
static void gid_hist_save(void) {
    struct gid_hist_file data = {
        .magic = GID_HIST_MAGIC,
        .version = GID_HIST_VER,
        .used = (uint8_t)gid_hist_used,
    };
    FILE *file;

    memcpy(data.entry, gid_hist, sizeof(data.entry));

    file = fopen(GID_HIST_FILE, "wb");
    if (file == NULL) {
        printf("# %s: failed to open %s for writing\n", __FUNCTION__, GID_HIST_FILE);
        return;
    }
    fwrite(&data, sizeof(data), 1, file);
    fclose(file);
}

void gid_hist_init(void) {
    struct gid_hist_file data = {0};
    FILE *file = fopen(GID_HIST_FILE, "rb");

    if (file == NULL) {
        return;
    }

    if (fread(&data, sizeof(data), 1, file) == 1
            && data.magic == GID_HIST_MAGIC && data.version == GID_HIST_VER
            && data.used <= GID_HIST_MAX) {
        memcpy(gid_hist, data.entry, sizeof(gid_hist));
        gid_hist_used = data.used;
        printf("# %s: %lu recent games\n", __FUNCTION__, gid_hist_used);
    }
    fclose(file);
}

/* Newest first, no duplicates. Relaunching the same thing repeatedly is normal,
 * and without the dedupe it would push everything worth remembering off the
 * end, which is exactly the entry anyone is here to find. */
void gid_hist_push(const char *gameid_in) {
    uint32_t i, slot = GID_HIST_MAX;

    if (gameid_in == NULL || !strlen(gameid_in)) {
        return;
    }

    /* An all zero id is what a disc that never filled the field in reports, and
     * everything reporting it shares the one id, so it names nothing and a
     * profile saved against it would apply to all of them at once. */
    if (strspn(gameid_in, "0") == strlen(gameid_in)) {
        return;
    }

    /* Whatever is current when the companion app first speaks is the app's own
     * id, so let the next mark drop it again. */
    gid_app_marked = 0;

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
    gid_hist_save();
}

/* Running the companion app is a launch like any other, so its own id lands at
 * the top of the history every single time. Hardcoding it does not work: the id
 * is derived from the executable, so it changes with every build of the app.
 *
 * The app announces itself by talking to us, though, and nothing else does.
 * Whatever id is current the moment it first speaks is therefore the app's, and
 * dropping that entry leaves a list of actual games. */
void gid_hist_mark_app(void) {
    uint32_t i;

    if (gid_app_marked || !gid_hist_used) {
        return;
    }
    gid_app_marked = 1;

    if (strncmp(gid_hist[0], gameid, GAME_ID_BUF_LEN) != 0) {
        return;
    }

    printf("# %s: %s is this app, dropping\n", __FUNCTION__, gid_hist[0]);

    for (i = 0; i + 1 < gid_hist_used; i++) {
        memcpy(gid_hist[i], gid_hist[i + 1], GAME_ID_BUF_LEN);
    }
    gid_hist_used--;
    memset(gid_hist[gid_hist_used], 0, GAME_ID_BUF_LEN);

    /* Persisted as well, or the app would be sitting at the top of the list as
     * the most recently launched thing after the next power cycle. */
    gid_hist_save();
}

const char *gid_hist_get(uint32_t idx) {
    return (idx < gid_hist_used) ? gid_hist[idx] : "";
}

uint32_t gid_hist_cnt(void) {
    return gid_hist_used;
}
