/*
 * Copyright (c) 2026, Laser Bear Industries
 * SPDX-License-Identifier: Apache-2.0
 *
 * BlueRetro Companion for GameCube.
 *
 * Talks to a BlueRetro adapter over a controller port using the vendor SI
 * opcodes it exposes: remap a controller on screen, watch raw controller input,
 * and update the adapter's firmware with no Bluetooth or serial adapter needed.
 *
 * The first two want firmware built with CONFIG_BLUERETRO_GC_APP, the last with
 * CONFIG_BLUERETRO_GC_OTA. Run it from Swiss; updating additionally wants
 * blueretro.bin on the root of the SD card.
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

/* Companion app opcodes. Must match main/system/gc_app.h. */
#define GC_APP_INPUT_CMD 0x21
#define GC_APP_MAP_CMD 0x22
#define GC_APP_MODE_CMD 0x23
#define GC_APP_INPUT_LEN 12
#define GC_APP_AXIS_CNT 6
#define GC_APP_NO_DEV 0xFF

#define GC_APP_MAP_BEGIN 0
#define GC_APP_MAP_SET 1
#define GC_APP_MAP_COMMIT 2
#define GC_APP_MAP_CANCEL 3

#define GC_APP_ST_IDLE 0
#define GC_APP_ST_BUSY 1
#define GC_APP_ST_OK 2
#define GC_APP_ST_ERROR 3
#define GC_APP_ST_MUTED 0x80

/* Generic button ids the GameCube driver understands, from adapter.h:108. */
#define PAD_LX_LEFT 0
#define PAD_LX_RIGHT 1
#define PAD_LY_DOWN 2
#define PAD_LY_UP 3
#define PAD_RX_LEFT 4
#define PAD_RX_RIGHT 5
#define PAD_RY_DOWN 6
#define PAD_RY_UP 7
#define PAD_LD_LEFT 8
#define PAD_LD_RIGHT 9
#define PAD_LD_DOWN 10
#define PAD_LD_UP 11
#define PAD_RB_LEFT 16
#define PAD_RB_RIGHT 17
#define PAD_RB_DOWN 18
#define PAD_RB_UP 19
#define PAD_MM 20
#define PAD_MQ 23
#define PAD_LM 24
#define PAD_LS 25
#define PAD_LT 26
#define PAD_RM 28
#define PAD_RS 29
#define PAD_RT 30

/* An axis has to move this far, of 127, to count as a deliberate push. */
#define AXIS_CAPTURE_THRESHOLD 70

/* Frames a button must stay down to mean cancel rather than a mapping. Long
 * enough that no ordinary tap reaches it. */
#define CAPTURE_CANCEL_FRAMES 150

/* Analog triggers also assert their digital click once bottomed out. */
#define TRIGGER_CLICK_THRESHOLD 95

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

static u16 pad_any_down(void);

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

/* Generic button ids, from the enum at main/adapter/adapter.h:108. */
static const char *btn_name[32] = {
    "LX_LEFT", "LX_RIGHT", "LY_DOWN", "LY_UP",
    "RX_LEFT", "RX_RIGHT", "RY_DOWN", "RY_UP",
    "LD_LEFT", "LD_RIGHT", "LD_DOWN", "LD_UP",
    "RD_LEFT", "RD_RIGHT", "RD_DOWN", "RD_UP",
    "RB_LEFT", "RB_RIGHT", "RB_DOWN", "RB_UP",
    "MM", "MS", "MT", "MQ",
    "LM", "LS", "LT", "LJ",
    "RM", "RS", "RT", "RJ",
};

static const char *axis_name[GC_APP_AXIS_CNT] = { "LX", "LY", "RX", "RY", "L ", "R " };

static int app_read_input(int chan, u8 *dev, u32 *btns, s8 *axes) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);
    int i;

    req[0] = GC_APP_INPUT_CMD;
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, req, 1, in, GC_APP_INPUT_LEN) < 0) {
        return -1;
    }

    *dev = in[0];
    *btns = (u32)in[2] | ((u32)in[3] << 8) | ((u32)in[4] << 16) | ((u32)in[5] << 24);
    for (i = 0; i < GC_APP_AXIS_CNT; i++) {
        axes[i] = (s8)in[6 + i];
    }
    return 0;
}

/* Commit progress rides in byte 1 of the input reply, so there is no separate
 * status opcode to poll. */
static int app_read_status(int chan, u8 *st) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);

    req[0] = GC_APP_INPUT_CMD;
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, req, 1, in, GC_APP_INPUT_LEN) < 0) {
        return -1;
    }
    *st = in[1] & 0x0F;
    return 0;
}

/* Signed bar, -127..127, centre marked. */
static void draw_axis(const char *name, s8 v) {
    char bar[33];
    int i, mid = 16, pos = mid + ((int)v * 15) / 127;

    for (i = 0; i < 32; i++) {
        bar[i] = (i == mid) ? '|' : '.';
    }
    if (pos < 0) { pos = 0; }
    if (pos > 31) { pos = 31; }
    bar[pos] = '#';
    bar[32] = '\0';
    printf("  %s [%s] %4d\n", name, bar, (int)v);
}

/* Reads the controller through opcode 0x21 rather than libogc's PAD driver.
 * Polling has to stay off for the manual transfers, so the raw state we are
 * already fetching doubles as the way back out of this screen. */
static void input_viewer(void) {
    u8 dev = GC_APP_NO_DEV;
    u32 btns = 0;
    s8 axes[GC_APP_AXIS_CNT];
    int chan, i, held = 0;

    memset(axes, 0, sizeof(axes));
    SI_DisablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        printf("Is the firmware built with CONFIG_BLUERETRO_GC_APP?\n");
        SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
        return;
    }

    for (;;) {
        if (app_read_input(chan, &dev, &btns, axes) == 0) {
            printf("\x1b[2J\x1b[1;1H");
            printf("BlueRetro live input viewer\n");
            printf("===========================\n\n");

            if (dev == GC_APP_NO_DEV) {
                printf("  Nothing has reported yet.\n");
                printf("  Connect a controller and press something.\n");
            }
            else {
                printf("  device %u        buttons 0x%08lx\n\n", dev, (unsigned long)btns);
                for (i = 0; i < GC_APP_AXIS_CNT; i++) {
                    draw_axis(axis_name[i], axes[i]);
                }
                printf("\n  pressed:");
                for (i = 0; i < 32; i++) {
                    if (btns & (1u << i)) {
                        printf(" %s", btn_name[i]);
                    }
                }
                printf("\n");
            }
            printf("\n  Hold Start to go back.\n");

            /* A hold, not a tap. This screen exists to watch taps. */
            held = (btns & (1u << 20)) ? held + 1 : 0;
            if (held > 45) {
                break;
            }
        }
        VIDEO_WaitVSync();
    }

    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    printf("\x1b[2J\x1b[1;1H");
}

/* ------------------------------------------------------------------ *
 * Mapping wizard
 * ------------------------------------------------------------------ */

struct map_entry {
    u8 src, dst, dst_id, max, thr, dz, turbo;
};

/* What the wizard asks for, in order. A stick or trigger prompt captures a
 * whole control rather than one direction: asking for eight stick directions
 * separately is both tedious and easy to get wrong. */
enum { CAP_BTN = 0, CAP_STICK, CAP_TRIGGER };

struct capture_step {
    const char *prompt;
    u8 kind;
    u8 dst;         /* CAP_BTN: the generic id to drive */
    u8 dst_axis;    /* CAP_STICK/CAP_TRIGGER: first of the target direction ids */
};

static const struct capture_step capture_steps[] = {
    { "Main stick  - move it in a full circle", CAP_STICK,   0, PAD_LX_LEFT },
    { "C stick     - move it in a full circle", CAP_STICK,   0, PAD_RX_LEFT },
    { "D-pad UP",                               CAP_BTN,     PAD_LD_UP,    0 },
    { "D-pad DOWN",                             CAP_BTN,     PAD_LD_DOWN,  0 },
    { "D-pad LEFT",                             CAP_BTN,     PAD_LD_LEFT,  0 },
    { "D-pad RIGHT",                            CAP_BTN,     PAD_LD_RIGHT, 0 },
    { "A",                                      CAP_BTN,     PAD_RB_DOWN,  0 },
    { "B",                                      CAP_BTN,     PAD_RB_LEFT,  0 },
    { "X",                                      CAP_BTN,     PAD_RB_RIGHT, 0 },
    { "Y",                                      CAP_BTN,     PAD_RB_UP,    0 },
    { "START",                                  CAP_BTN,     PAD_MM,       0 },
    { "Z",                                      CAP_BTN,     PAD_RS,       0 },
    { "L  - pull it all the way in",            CAP_TRIGGER, 0, PAD_LM },
    { "R  - pull it all the way in",            CAP_TRIGGER, 0, PAD_RM },
};
#define CAPTURE_STEP_CNT (int)(sizeof(capture_steps) / sizeof(capture_steps[0]))

/* Combos, so the adapter keeps its reset, pairing and power off gestures. Same
 * set config.c bakes in for GameCube builds. */
static const u8 combo_src[10] = {
    PAD_LM, PAD_RM, PAD_MQ, PAD_RB_UP, PAD_RB_DOWN,
    PAD_RB_RIGHT, PAD_RB_LEFT, PAD_LD_UP, PAD_LD_DOWN, 21 /* PAD_MS */
};
#define BR_COMBO_BASE_1 118
#define BR_COMBO_CNT 10

static int app_map_send(int chan, u8 sub, const u8 *args, int n) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);
    int i;

    out[0] = GC_APP_MAP_CMD;
    out[1] = sub;
    for (i = 0; i < 8; i++) {
        out[2 + i] = (i < n) ? args[i] : 0;
    }
    return si_xfer(chan, out, 10, NULL, 0);
}

static int app_mode_send(int chan, u8 enable, u8 port) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);

    out[0] = GC_APP_MODE_CMD;
    out[1] = enable;
    out[2] = port;
    return si_xfer(chan, out, 3, NULL, 0);
}

/* Waits until nothing is held, so one press cannot satisfy two prompts. */
static void wait_neutral(int chan) {
    u8 dev; u32 btns; s8 ax[GC_APP_AXIS_CNT];
    int quiet = 0, i;

    while (quiet < 8) {
        if (app_read_input(chan, &dev, &btns, ax) == 0) {
            int moved = 0;

            for (i = 0; i < GC_APP_AXIS_CNT; i++) {
                if (ax[i] > AXIS_CAPTURE_THRESHOLD || ax[i] < -AXIS_CAPTURE_THRESHOLD) {
                    moved = 1;
                }
            }
            quiet = (btns == 0 && !moved) ? quiet + 1 : 0;
        }
        VIDEO_WaitVSync();
    }
}

/* Captures on release rather than press, so a long hold can mean cancel without
 * any button being off limits. Reserving one for cancel does not work here:
 * 0x21 only ever reports the Bluetooth pad being remapped, and every button on
 * it is a legitimate mapping target. An Xbox Menu button is PAD_MM, the same id
 * a reserved Start would have used.
 *
 * Returns the bit index pressed, or -1 if the user held to cancel. */
static int capture_button(int chan, const char *prompt, int step, int total) {
    u8 dev; u32 btns; s8 ax[GC_APP_AXIS_CNT];
    int i, pressed = -1, held = 0;

    wait_neutral(chan);
    for (;;) {
        if (app_read_input(chan, &dev, &btns, ax) == 0) {
            int first = -1;

            for (i = 0; i < 32; i++) {
                if (btns & (1u << i)) {
                    first = i;
                    break;
                }
            }

            printf("\x1b[2J\x1b[1;1H");
            printf("Mapping wizard   (%d/%d)\n", step, total);
            printf("=======================\n\n");
            printf("  Press the control you want for:\n\n     %s\n", prompt);
            if (pressed >= 0) {
                printf("\n  holding %s ... keep holding to cancel\n", btn_name[pressed]);
            }
            printf("\n\n  Tap to map it. Hold anything to cancel.\n");

            if (first >= 0) {
                if (first != pressed) {
                    pressed = first;
                    held = 0;
                }
                if (++held > CAPTURE_CANCEL_FRAMES) {
                    return -1;
                }
            }
            else if (pressed >= 0) {
                return pressed;
            }
        }
        VIDEO_WaitVSync();
    }
}

/* Returns the index of the axis that moved, or -1 on cancel. Cancel is any
 * button held, for the same reason as capture_button(). */
static int capture_axis(int chan, const char *prompt, int first, int last,
                        int step, int total) {
    u8 dev; u32 btns; s8 ax[GC_APP_AXIS_CNT];
    int i, held = 0;

    wait_neutral(chan);
    for (;;) {
        if (app_read_input(chan, &dev, &btns, ax) == 0) {
            printf("\x1b[2J\x1b[1;1H");
            printf("Mapping wizard   (%d/%d)\n", step, total);
            printf("=======================\n\n");
            printf("  Move the control you want for:\n\n     %s\n", prompt);
            printf("\n\n  Hold any button to cancel.\n");

            if (btns) {
                if (++held > CAPTURE_CANCEL_FRAMES) {
                    return -1;
                }
            }
            else {
                held = 0;
                for (i = first; i <= last; i++) {
                    if (ax[i] > AXIS_CAPTURE_THRESHOLD || ax[i] < -AXIS_CAPTURE_THRESHOLD) {
                        return i;
                    }
                }
            }
        }
        VIDEO_WaitVSync();
    }
}

/* How the digital L/R click is produced. Controllers differ: a GameCube or NSO
 * pad has a real microswitch under the trigger and reports it as its own
 * button, while an Xbox pad is analog all the way down and has nothing to
 * capture. Deriving the click from the axis is right for the latter and wrong
 * for the former, where it would fire before the switch does. */
enum { TRIG_DERIVED = 0, TRIG_SEPARATE, TRIG_DIGITAL, TRIG_MODE_CNT };

static int trigger_mode_menu(void) {
    int sel = 0;

    for (;;) {
        u16 down;

        printf("\x1b[2J\x1b[1;1H");
        printf("Mapping wizard\n==============\n\n");
        printf("  How do your L and R triggers work?\n\n");
        printf("   %s Analog, click derived at %d%%\n", sel == 0 ? ">" : " ",
            TRIGGER_CLICK_THRESHOLD);
        printf("     Xbox and similar, no switch at the bottom\n\n");
        printf("   %s Analog plus a real full pull click\n", sel == 1 ? ">" : " ");
        printf("     GameCube and NSO pads, click captured separately\n\n");
        printf("   %s Digital only, no analog travel\n", sel == 2 ? ">" : " ");
        printf("     Switch Pro and similar, buttons rather than triggers\n");
        printf("\n  D-pad to choose, A to start, B to go back.\n");

        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_B) {
            return -1;
        }
        if (down & PAD_BUTTON_UP) {
            sel = (sel + TRIG_MODE_CNT - 1) % TRIG_MODE_CNT;
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % TRIG_MODE_CNT;
        }
        if (down & PAD_BUTTON_A) {
            return sel;
        }
    }
}

/* Returns 1 if the user wants to map another controller. */
static int mapping_wizard(void) {
    struct map_entry map[64];
    u8 dev, st, args[8];
    u32 btns;
    s8 ax[GC_APP_AXIS_CNT];
    int chan, i, s, n = 0, port = 0, tries;
    int trig_mode, total, shown = 0;

    /* Asked while libogc can still read the GameCube pad, before the manual
     * transfers take the bus. */
    trig_mode = trigger_mode_menu();
    if (trig_mode < 0) {
        return 0;
    }
    total = CAPTURE_STEP_CNT + ((trig_mode == TRIG_SEPARATE) ? 2 : 0);

    SI_DisablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
        return 0;
    }

    /* Whichever device last reported is the one being remapped. */
    if (app_read_input(chan, &dev, &btns, ax) == 0 && dev != GC_APP_NO_DEV) {
        port = dev;
    }

    printf("\x1b[2J\x1b[1;1H");
    printf("Mapping wizard\n==============\n\n");
    printf("  Remapping the controller on port %d.\n", port + 1);
    printf("  It stops driving the game until this finishes.\n\n");
    printf("  Press something on it to begin.\n");

    /* Holding the output neutral also proves the adapter understood us. */
    app_mode_send(chan, 1, (u8)port);
    wait_neutral(chan);

    for (s = 0; s < CAPTURE_STEP_CNT; s++) {
        const struct capture_step *cs = &capture_steps[s];

        if (cs->kind == CAP_BTN) {
            int src = capture_button(chan, cs->prompt, ++shown, total);

            if (src < 0) { goto cancelled; }
            map[n].src = (u8)src;   map[n].dst = cs->dst;  map[n].dst_id = (u8)port;
            map[n].max = 100;       map[n].thr = 50;       map[n].dz = 135;
            map[n].turbo = 0;       n++;
        }
        else if (cs->kind == CAP_STICK) {
            /* One sweep gives us the pair, so fill all four directions. */
            int a = capture_axis(chan, cs->prompt, 0, 3, ++shown, total);
            int base;

            if (a < 0) { goto cancelled; }
            base = (a < 2) ? 0 : 4;     /* source pair: LX/LY or RX/RY */
            for (i = 0; i < 4; i++) {
                map[n].src = (u8)(base + i);      map[n].dst = (u8)(cs->dst_axis + i);
                map[n].dst_id = (u8)port;         map[n].max = 100;
                map[n].thr = 50;                  map[n].dz = 135;
                map[n].turbo = 0;                 n++;
            }
        }
        else if (trig_mode == TRIG_DIGITAL) {
            /* Nothing analog to wait for, so take it as a button. The console
             * still reads L and R as an axis as well as a switch, so drive both
             * from it: the axis to full, and the click. Without the axis entry
             * anything that reads the analog value sees a trigger at rest. */
            const char *dp = (cs->dst_axis == PAD_LM) ? "L  - press it" : "R  - press it";
            u8 click = (cs->dst_axis == PAD_LM) ? PAD_LT : PAD_RT;
            int db = capture_button(chan, dp, ++shown, total);

            if (db < 0) { goto cancelled; }

            map[n].src = (u8)db;  map[n].dst = cs->dst_axis;  map[n].dst_id = (u8)port;
            map[n].max = 100;     map[n].thr = 50;            map[n].dz = 135;
            map[n].turbo = 0;     n++;

            map[n].src = (u8)db;  map[n].dst = click;         map[n].dst_id = (u8)port;
            map[n].max = 100;     map[n].thr = 50;            map[n].dz = 135;
            map[n].turbo = 0;     n++;
        }
        else {
            int a = capture_axis(chan, cs->prompt, 4, 5, ++shown, total);
            u8 src, click;

            if (a < 0) { goto cancelled; }
            src = (a == 4) ? PAD_LM : PAD_RM;
            click = (cs->dst_axis == PAD_LM) ? PAD_LT : PAD_RT;

            map[n].src = src;  map[n].dst = cs->dst_axis;  map[n].dst_id = (u8)port;
            map[n].max = 100;  map[n].thr = 50;            map[n].dz = 135;
            map[n].turbo = 0;  n++;

            if (trig_mode == TRIG_DERIVED) {
                /* No switch to capture, so fire the click off the axis. */
                map[n].src = src;  map[n].dst = click;     map[n].dst_id = (u8)port;
                map[n].max = 100;  map[n].thr = TRIGGER_CLICK_THRESHOLD;
                map[n].dz = 135;   map[n].turbo = 0;       n++;
            }
            else {
                /* Real microswitch: take it as its own button, so the click
                 * lands exactly where the hardware says rather than wherever
                 * the analog range happens to put 95%. */
                const char *cp = (cs->dst_axis == PAD_LM)
                    ? "L click - press past the bump, until it clicks"
                    : "R click - press past the bump, until it clicks";
                int cb = capture_button(chan, cp, ++shown, total);

                if (cb < 0) { goto cancelled; }

                map[n].src = (u8)cb;  map[n].dst = click;  map[n].dst_id = (u8)port;
                map[n].max = 100;     map[n].thr = 50;     map[n].dz = 135;
                map[n].turbo = 0;     n++;
            }
        }
    }

    for (i = 0; i < BR_COMBO_CNT; i++) {
        map[n].src = combo_src[i];  map[n].dst = (u8)(BR_COMBO_BASE_1 + i);
        map[n].dst_id = (u8)port;   map[n].max = 100;
        map[n].thr = 50;            map[n].dz = 135;
        map[n].turbo = 0;           n++;
    }

    printf("\x1b[2J\x1b[1;1H");
    printf("Mapping wizard\n==============\n\n  Saving %d entries...\n", n);

    args[0] = (u8)port;
    app_map_send(chan, GC_APP_MAP_BEGIN, args, 1);
    for (i = 0; i < n; i++) {
        args[0] = (u8)i;            args[1] = map[i].src;
        args[2] = map[i].dst;       args[3] = map[i].dst_id;
        args[4] = map[i].max;       args[5] = map[i].thr;
        args[6] = map[i].dz;        args[7] = map[i].turbo;
        if (app_map_send(chan, GC_APP_MAP_SET, args, 8) < 0) {
            printf("\n  Transfer failed at entry %d.\n", i);
            goto cancelled;
        }
    }
    args[0] = (u8)n;
    app_map_send(chan, GC_APP_MAP_COMMIT, args, 1);

    st = GC_APP_ST_BUSY;
    for (tries = 0; tries < 300; tries++) {
        if (app_read_status(chan, &st) == 0
                && (st == GC_APP_ST_OK || st == GC_APP_ST_ERROR)) {
            break;
        }
        usleep(10000);
    }

    app_mode_send(chan, 0, (u8)port);
    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

    printf("\x1b[2J\x1b[1;1H");
    if (st == GC_APP_ST_OK) {
        printf("Mapping saved.\n\n  %d entries written to port %d.\n", n, port + 1);
        printf("  It takes effect immediately, no restart.\n");
    }
    else {
        printf("Mapping NOT saved.\n\n  The adapter reported status %u.\n", st);
        printf("  Your previous mapping is untouched.\n");
    }

    printf("\n\n  A to map another controller, B to finish.\n");
    for (;;) {
        u16 down = pad_any_down();

        if (down & PAD_BUTTON_A) {
            return 1;
        }
        if (down & (PAD_BUTTON_B | PAD_BUTTON_START)) {
            return 0;
        }
        VIDEO_WaitVSync();
    }

cancelled:
    app_map_send(chan, GC_APP_MAP_CANCEL, NULL, 0);
    app_mode_send(chan, 0, (u8)port);
    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    printf("\x1b[2J\x1b[1;1H");
    printf("Cancelled. Nothing was changed.\n");
    printf("\n\n  A to map another controller, B to finish.\n");
    for (;;) {
        u16 down = pad_any_down();

        if (down & PAD_BUTTON_A) {
            return 1;
        }
        if (down & (PAD_BUTTON_B | PAD_BUTTON_START)) {
            return 0;
        }
        VIDEO_WaitVSync();
    }
}

/* Returns 0 mapping wizard, 1 input viewer, 2 firmware update. */
static int main_menu(void) {
    int sel = 0;

    for (;;) {
        u16 down;

        printf("\x1b[2J\x1b[1;1H");
        printf("BlueRetro Companion\n");
        printf("===================\n\n");
        printf("   %s Remap a controller\n", sel == 0 ? ">" : " ");
        printf("   %s Live input viewer\n", sel == 1 ? ">" : " ");
        printf("   %s Update firmware\n", sel == 2 ? ">" : " ");
        printf("\n  D-pad to choose, A to select, START to exit.\n");

        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_START) {
            exit(0);
        }
        if (down & PAD_BUTTON_UP) {
            sel = (sel + 2) % 3;
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % 3;
        }
        if (down & PAD_BUTTON_A) {
            printf("\x1b[2J\x1b[1;1H");
            return sel;
        }
    }
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

/* Every port, not just the first. A controller that was just remapped onto port
 * 2 is the one in the user's hands, and reading only pad 0 leaves them holding
 * something the app ignores. */
static u16 pad_any_down(void) {
    u16 down = 0;
    int i;

    PAD_ScanPads();
    for (i = 0; i < 4; i++) {
        down |= PAD_ButtonsDown(i);
    }
    return down;
}

static void wait_exit(void) {
    printf("\nPress START to exit.\n");
    for (;;) {
        if (pad_any_down() & PAD_BUTTON_START) {
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

    /* Neither the viewer nor the wizard needs an SD card, so offer the menu
     * before touching FAT. */
    {
        int choice = main_menu();

        if (choice == 0) {
            while (mapping_wizard()) {
                /* Round again for the next controller. Press something on it
                 * first: the wizard remaps whichever device reported last. */
            }
            wait_exit();
        }
        else if (choice == 1) {
            input_viewer();
            wait_exit();
        }
        /* Anything else falls through to the firmware update below. Both of the
         * branches above end in wait_exit(), which does not return. */
    }

    printf("\n\nBlueRetro Companion - firmware update\n");
    printf("=====================================\n\n");

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
