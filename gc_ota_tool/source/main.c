/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * BlueRetro firmware updater for GameCube.
 *
 * Streams a firmware image into the adapter's OTA partition over the controller
 * port, using the vendor SI opcodes BlueRetro exposes when built with
 * CONFIG_BLUERETRO_GC_OTA. Intended as a recovery path for when the Bluetooth
 * stack is the thing that needs replacing.
 *
 * Put blueretro.bin on the root of an SD card (SD Gecko or SD2SP2) and run this
 * from Swiss.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>

/* Must match main/wired/nsi.c and main/system/gc_ota.h in the firmware. */
#define GC_OTA_CMD 0x1E
#define GC_OTA_STATUS_CMD 0x1F
#define GC_OTA_VER_CMD 0x20

#define GC_OTA_SUB_START 0x00
#define GC_OTA_SUB_DATA 0x01
#define GC_OTA_SUB_END 0x02
#define GC_OTA_SUB_ABORT 0x03

#define GC_OTA_IDLE 0
#define GC_OTA_READY 1
#define GC_OTA_BUSY 2
#define GC_OTA_DONE 3
#define GC_OTA_ERROR 4

#define GC_OTA_PROTO_VER 1
#define GC_OTA_STATUS_LEN 3
#define GC_OTA_DATA_LEN 8

/* Version string, fetched a slice at a time. Mirrors esp_app_desc_t::version. */
#define GC_OTA_VER_LEN 32
#define GC_OTA_VER_CHUNK 8

/* Layout of esp_app_desc_t inside an ESP-IDF application image. The descriptor
 * sits right behind the 24 byte image header plus one 8 byte segment header. */
#define APP_DESC_OFF 0x20
#define APP_DESC_MAGIC 0xABCD5432
#define APP_DESC_VER_OFF (APP_DESC_OFF + 0x10)
#define APP_DESC_NAME_OFF (APP_DESC_OFF + 0x30)

/* The adapter stages a kilobyte in RAM before flushing it to flash, so a batch
 * of exactly that many frames lines up one flush with one status poll. Sending
 * more than this without checking would just be dropped while it is busy. */
#define GC_OTA_BATCH_FRAMES 128
#define GC_OTA_BATCH_BYTES (GC_OTA_BATCH_FRAMES * GC_OTA_DATA_LEN)

#define FW_PATH "blueretro.bin"

#define SI_TIMEOUT_MS 100
#define READY_TIMEOUT_MS 5000

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static volatile u32 si_done = 0;

static void si_cb(s32 chan, u32 type) {
    (void)chan;
    (void)type;
    si_done = 1;
}

/* libogc drives SI asynchronously. This protocol is strictly request then
 * response, so wrap it back into a blocking call with a timeout. */
static int si_xfer(int chan, void *out, u32 out_len, void *in, u32 in_len) {
    u64 start;

    si_done = 0;
    if (!SI_Transfer(chan, out, out_len, in, in_len, si_cb, 0)) {
        return -1;
    }

    start = gettime();
    while (!si_done) {
        if (ticks_to_millisecs(diff_ticks(start, gettime())) > SI_TIMEOUT_MS) {
            return -1;
        }
    }
    return 0;
}

static int ota_status(int chan, u8 *state, u8 *seq, u8 *ver) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);

    out[0] = GC_OTA_STATUS_CMD;
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, out, 1, in, GC_OTA_STATUS_LEN) < 0) {
        return -1;
    }

    *state = in[0];
    *seq = in[1];
    *ver = in[2];
    return 0;
}

static int ota_send(int chan, u8 sub, u8 seq, const u8 *data) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);

    out[0] = GC_OTA_CMD;
    out[1] = sub;
    out[2] = seq;
    if (data) {
        memcpy(&out[3], data, GC_OTA_DATA_LEN);
    }
    else {
        memset(&out[3], 0, GC_OTA_DATA_LEN);
    }

    /* The adapter never answers this one, same as the game id command. */
    return si_xfer(chan, out, 3 + GC_OTA_DATA_LEN, NULL, 0);
}

/* Returns 0 once the adapter is accepting data again, with the last sequence
 * number it actually stored. */
static int ota_wait_ready(int chan, u8 *last_seq) {
    u64 start = gettime();
    u8 state, seq, ver;

    for (;;) {
        if (ota_status(chan, &state, &seq, &ver) == 0) {
            if (state == GC_OTA_READY) {
                if (last_seq) {
                    *last_seq = seq;
                }
                return 0;
            }
            if (state == GC_OTA_ERROR) {
                printf("  adapter reported an error\n");
                return -1;
            }
        }
        if (ticks_to_millisecs(diff_ticks(start, gettime())) > READY_TIMEOUT_MS) {
            printf("  timed out waiting for ready\n");
            return -1;
        }
        usleep(1000);
    }
}

/* Reads the running firmware version out of the adapter, one slice per
 * transaction because a whole SI reply has to fit in 128 bits. */
static int ota_get_version(int chan, char *out) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);
    int chunk;

    memset(out, 0, GC_OTA_VER_LEN + 1);

    for (chunk = 0; chunk < GC_OTA_VER_LEN / GC_OTA_VER_CHUNK; chunk++) {
        req[0] = GC_OTA_VER_CMD;
        req[1] = (u8)chunk;
        memset(in, 0, sizeof(in));

        if (si_xfer(chan, req, 2, in, GC_OTA_VER_CHUNK) < 0) {
            return -1;
        }
        memcpy(out + chunk * GC_OTA_VER_CHUNK, in, GC_OTA_VER_CHUNK);
    }

    out[GC_OTA_VER_LEN] = '\0';
    return 0;
}

/* Pulls the version and project name straight out of the image's own
 * esp_app_desc_t, so the card is described by what it actually contains rather
 * than by its filename. */
static int image_get_version(FILE *f, char *ver, char *name) {
    u8 hdr[APP_DESC_NAME_OFF + 32];
    u32 magic;

    memset(ver, 0, GC_OTA_VER_LEN + 1);
    memset(name, 0, 32 + 1);

    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        return -1;
    }

    magic = (u32)hdr[APP_DESC_OFF]
          | ((u32)hdr[APP_DESC_OFF + 1] << 8)
          | ((u32)hdr[APP_DESC_OFF + 2] << 16)
          | ((u32)hdr[APP_DESC_OFF + 3] << 24);
    if (magic != APP_DESC_MAGIC) {
        return -1;
    }

    memcpy(ver, &hdr[APP_DESC_VER_OFF], GC_OTA_VER_LEN);
    ver[GC_OTA_VER_LEN] = '\0';
    memcpy(name, &hdr[APP_DESC_NAME_OFF], 32);
    name[32] = '\0';

    fseek(f, 0, SEEK_SET);
    return 0;
}

/* A real controller does not answer 0x1F at all, so a good reply with a
 * recognised protocol version is a solid enough fingerprint. */
static int ota_find_adapter(void) {
    u8 state, seq, ver;
    int chan;

    for (chan = 0; chan < 4; chan++) {
        if (ota_status(chan, &state, &seq, &ver) == 0 && ver == GC_OTA_PROTO_VER) {
            printf("Found BlueRetro on port %d (state %u)\n", chan + 1, state);
            return chan;
        }
    }
    return -1;
}

static void video_init(void) {
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
        rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

static void wait_exit(void) {
    printf("\nPress START to exit.\n");
    for (;;) {
        PAD_ScanPads();
        if (PAD_ButtonsDown(0) & PAD_BUTTON_START) {
            exit(0);
        }
        VIDEO_WaitVSync();
    }
}

int main(int argc, char **argv) {
    static u8 batch[GC_OTA_BATCH_BYTES];
    FILE *f;
    long fw_size, sent = 0;
    int chan, i;
    u8 seq = 0, last_seq = 0;

    (void)argc;
    (void)argv;

    video_init();
    PAD_Init();

    printf("\n\nBlueRetro GameCube firmware updater\n");
    printf("===================================\n\n");

    if (!fatInitDefault()) {
        printf("No FAT device. Need an SD Gecko or SD2SP2.\n");
        wait_exit();
    }

    f = fopen(FW_PATH, "rb");
    if (!f) {
        f = fopen("sd:/" FW_PATH, "rb");
    }
    if (!f) {
        printf("Could not open %s from the card root.\n", FW_PATH);
        wait_exit();
    }

    fseek(f, 0, SEEK_END);
    fw_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* libogc's PAD driver keeps SI auto polling running, which fights manual
     * transfers. Hand the bus over for the duration of the update. */
    SI_DisablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("No BlueRetro adapter answered on any port.\n");
        printf("Is the firmware built with CONFIG_BLUERETRO_GC_OTA?\n");
        SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
        fclose(f);
        wait_exit();
    }

    {
        char installed[GC_OTA_VER_LEN + 1];
        char card_ver[GC_OTA_VER_LEN + 1];
        char card_name[32 + 1];
        int have_installed = (ota_get_version(chan, installed) == 0);
        int have_card = (image_get_version(f, card_ver, card_name) == 0);

        printf("\n");
        printf("  installed : %s\n", have_installed && installed[0] ? installed : "(unknown)");
        if (have_card) {
            printf("  on card   : %s\n", card_ver[0] ? card_ver : "(unnamed)");
            printf("  image     : %s, %ld bytes\n", card_name, fw_size);
        }
        else {
            printf("  on card   : (no valid ESP-IDF image header)\n");
            printf("  image     : %s, %ld bytes\n", FW_PATH, fw_size);
            printf("\n  That file does not look like adapter firmware.\n");
        }

        if (have_installed && have_card && strcmp(installed, card_ver) == 0) {
            printf("\n  These are the same version.\n");
        }

        /* Reading buttons means letting libogc poll again, so hand the bus back
         * for the prompt and take it again before the transfer starts. */
        SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

        printf("\nPress A to flash, B to cancel.\n");
        for (;;) {
            u16 down;

            PAD_ScanPads();
            down = PAD_ButtonsDown(0);

            if (down & PAD_BUTTON_A) {
                break;
            }
            if (down & PAD_BUTTON_B) {
                printf("\nCancelled. Nothing was written.\n");
                SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
                fclose(f);
                wait_exit();
            }
            VIDEO_WaitVSync();
        }

        SI_DisablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    }

    printf("\nDo NOT power off until this finishes.\n\n");

    if (ota_send(chan, GC_OTA_SUB_START, 0, NULL) < 0
            || ota_wait_ready(chan, &last_seq) < 0) {
        printf("Failed to start the update.\n");
        goto fail;
    }

    while (sent < fw_size) {
        size_t got = fread(batch, 1, sizeof(batch), f);
        size_t frames, off;

        if (got == 0) {
            break;
        }
        /* The adapter always takes whole frames, so pad the tail. The image is
         * a fixed size the ESP side already knows, so trailing bytes are
         * harmless. */
        if (got % GC_OTA_DATA_LEN) {
            size_t pad = GC_OTA_DATA_LEN - (got % GC_OTA_DATA_LEN);
            memset(batch + got, 0xFF, pad);
            got += pad;
        }
        frames = got / GC_OTA_DATA_LEN;

        for (off = 0, i = 0; i < (int)frames; i++, off += GC_OTA_DATA_LEN) {
            seq++;
            if (ota_send(chan, GC_OTA_SUB_DATA, seq, &batch[off]) < 0) {
                printf("\nSI transfer failed at %ld bytes.\n", sent + (long)off);
                goto fail;
            }
        }

        /* Let the flush finish, then find out what actually landed. */
        if (ota_wait_ready(chan, &last_seq) < 0) {
            goto fail;
        }

        if (last_seq != seq) {
            /* Frames went missing, most likely sent while the adapter was busy.
             * Rewind to the last byte it acknowledged and carry on from there. */
            long lost = (long)(u8)(seq - last_seq) * GC_OTA_DATA_LEN;

            printf("\nResync: %ld bytes lost, rewinding\n", lost);
            sent += (long)got - lost;
            seq = last_seq;
            fseek(f, sent, SEEK_SET);
        }
        else {
            sent += (long)got;
        }

        printf("\r%ld / %ld bytes (%ld%%)", sent, fw_size, (sent * 100) / fw_size);
    }

    printf("\n\nFinishing...\n");
    if (ota_send(chan, GC_OTA_SUB_END, ++seq, NULL) < 0) {
        printf("Failed to send end.\n");
        goto fail;
    }

    /* The adapter restarts itself shortly after reporting DONE, so a lost reply
     * here is expected rather than a failure. */
    {
        u8 state = 0, s = 0, ver = 0;
        int tries;

        for (tries = 0; tries < 200; tries++) {
            if (ota_status(chan, &state, &s, &ver) == 0 && state == GC_OTA_DONE) {
                break;
            }
            usleep(10000);
        }
        if (state == GC_OTA_DONE) {
            printf("Update complete. The adapter is restarting.\n");
        }
        else {
            printf("No DONE seen. If the adapter rebooted, it probably worked;\n");
            printf("check the reported version once it is back.\n");
        }
    }

    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    fclose(f);
    wait_exit();
    return 0;

fail:
    ota_send(chan, GC_OTA_SUB_ABORT, 0, NULL);
    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    fclose(f);
    printf("\nUpdate aborted. The adapter kept its current firmware.\n");
    wait_exit();
    return 1;
}
