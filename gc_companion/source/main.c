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

/* Which game is running, and which profile a save writes. */
#define GC_APP_GID_CMD 0x2A
#define GC_APP_GID_LEN 24
#define GC_APP_GID_CHUNK 8
#define GC_APP_SCOPE_GLOBAL 0
#define GC_APP_SCOPE_GAME_BASE 1
#define GID_HIST_MAX 4

#define GID_TITLES_PATH "blueretro_games.txt"
#define GID_TITLE_LEN 40

/* Debug log download. */
#define GC_LOG_CMD 0x24
#define GC_LOG_STATUS_CMD 0x25
#define GC_LOG_READ_CMD 0x26
#define GC_LOG_CHUNK 8
#define GC_LOG_STATUS_LEN 8
#define GC_LOG_PROTO_VER 1

#define GC_LOG_SUB_BANK_OFF 0
#define GC_LOG_SUB_BANK_ON 1
#define GC_LOG_SUB_RESET 2

#define GC_LOG_ST_IDLE 0
#define GC_LOG_ST_BUSY 1
#define GC_LOG_ST_OK 2
#define GC_LOG_ST_ERROR 3

#define LOG_PATH "br_debug_trace.bin"

/* Firmware slot selection. */
#define GC_BOOT_INFO_CMD 0x27
#define GC_BOOT_VER_CMD 0x28
#define GC_BOOT_SEL_CMD 0x29
#define GC_BOOT_INFO_LEN 8
#define GC_BOOT_VER_LEN 32
#define GC_BOOT_VER_CHUNK 8
#define GC_BOOT_PROTO_VER 1
#define GC_BOOT_SLOT_CNT 3

#define GC_BOOT_ST_IDLE 0
#define GC_BOOT_ST_BUSY 1
#define GC_BOOT_ST_OK 2
#define GC_BOOT_ST_ERROR 3

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
static u16 pad_any_held(void);
/* Mounting is not free and libfat is happy to be asked twice, but callers here
 * are on screens where a stall is visible, so do it once and remember. */
static bool fat_ready(void) {
    static int state = 0;

    if (state == 0) {
        state = fatInitDefault() ? 1 : -1;
    }
    return state > 0;
}

static void si_grab(void);
static void pad_settle(void);
static void pad_reattach(void);
static void wait_ack(void);
static int prompt_again(void);
static void app_exit(void);

static void si_cb(s32 chan, u32 type) {
    (void)chan;
    (void)type;
    si_done = 1;
}

/* libogc drives SI asynchronously. This protocol is strictly request then
 * response, so wrap it back into a blocking call with a timeout.
 *
 * SI_Transfer() refuses outright, returning zero, while a packet is already
 * queued on the channel. libogc queues its own whenever it is probing pads,
 * which it does for several seconds after the adapter restarts and its ports
 * come back. Treating that as an error made a busy bus indistinguishable
 * from an adapter that does not implement the opcode. It is neither: it is
 * a wait. */
static int si_xfer(int chan, void *out, u32 out_len, void *in, u32 in_len) {
    u64 start = gettime();

    si_done = 0;

    /* One deadline covering both waiting for the channel and waiting for the
     * reply, not one each. Callers poll in loops of a few hundred, so the cost
     * of a failed transfer is multiplied by that; letting it reach twice the
     * timeout turned a poll that should give up in half a minute into one that
     * took several. */
    while (!SI_Transfer(chan, out, out_len, in, in_len, si_cb, 0)) {
        if (ticks_to_millisecs(diff_ticks(start, gettime())) > SI_TIMEOUT_MS) {
            return -1;
        }
    }

    while (!si_done) {
        if (ticks_to_millisecs(diff_ticks(start, gettime())) > SI_TIMEOUT_MS) {
            return -1;
        }
    }
    return 0;
}

/* Take the bus off libogc. Disabling polling is not enough on its own: a pad
 * probe already in flight keeps its packet queued, and PAD_Sync() is how you
 * ask whether it has finished. Skipping this wait is what made a slot swap
 * look like firmware without slot support. */
static void si_grab(void) {
    int i;

    SI_DisablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);

    /* A second is far longer than a probe needs, and this is paid on every
     * operation, so a longer wait here is silently charged to everything. If
     * libogc still has not finished, carry on regardless: si_xfer() waits out a
     * refused channel by itself, which is the real protection. */
    for (i = 0; i < 60 && !PAD_Sync(); i++) {
        VIDEO_WaitVSync();
    }
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
    si_grab();

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        printf("Is the firmware built with CONFIG_BLUERETRO_GC_APP?\n");
        pad_settle();
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

    pad_settle();
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
static int app_get_gameid(int chan, int idx, char *out) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);
    int chunk;

    memset(out, 0, GC_APP_GID_LEN + 1);

    for (chunk = 0; chunk < GC_APP_GID_LEN / GC_APP_GID_CHUNK; chunk++) {
        req[0] = GC_APP_GID_CMD;
        req[1] = (u8)idx;
        req[2] = (u8)chunk;
        memset(in, 0, sizeof(in));

        if (si_xfer(chan, req, 3, in, GC_APP_GID_CHUNK) < 0) {
            return -1;
        }
        memcpy(out + chunk * GC_APP_GID_CHUNK, in, GC_APP_GID_CHUNK);
    }
    out[GC_APP_GID_LEN] = 0;
    return 0;
}

/* Titles are optional and come off the card rather than being built in, so the
 * list can be regenerated from BlueRetroWebCfg's gameid.db without rebuilding
 * anything here.
 *
 * Ids are looked up exactly as the adapter reports them. An earlier version
 * decoded them as hex encoded text, on the assumption a GameCube id was its
 * disc code. The database says otherwise: they are eight opaque bytes, and of
 * the three thousand odd GameCube and N64 entries only a handful decode to
 * anything printable, every one of them by accident.
 *
 * One pass resolves every id, because the file runs past a hundred kilobytes
 * and reopening it per id would mean scanning all of it four times over. */
static void gid_titles(char ids[][GC_APP_GID_LEN + 1],
        char names[][GID_TITLE_LEN + 1], int cnt) {
    char line[160];
    FILE *f;
    int i, left = cnt;

    for (i = 0; i < cnt; i++) {
        snprintf(names[i], GID_TITLE_LEN + 1, "%.*s", GID_TITLE_LEN, ids[i]);
    }

    /* The card is mounted on demand, by whichever screen needs it. Remapping a
     * controller needs nothing off the card except this, so nothing on the path
     * here had ever mounted it, and every lookup was failing on the open rather
     * than on the contents. */
    if (!fat_ready()) {
        return;
    }

    f = fopen(GID_TITLES_PATH, "r");
    if (!f) {
        f = fopen("sd:/" GID_TITLES_PATH, "r");
    }
    if (!f) {
        return;
    }

    while (left && fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        int n;

        if (!eq) {
            continue;
        }
        *eq++ = 0;

        n = strlen(eq);
        while (n && (eq[n - 1] == '\n' || eq[n - 1] == '\r')) {
            eq[--n] = 0;
        }
        if (!n) {
            continue;
        }

        for (i = 0; i < cnt; i++) {
            /* Skipping ids already resolved means a duplicate later in the
             * file cannot overwrite the first match. */
            if (strcmp(names[i], ids[i]) == 0 && strcmp(line, ids[i]) == 0) {
                snprintf(names[i], GID_TITLE_LEN + 1, "%s", eq);
                left--;
                break;
            }
        }
    }
    fclose(f);
}

/* The list is recent launches, newest first, and the newest is almost always
 * this app: running it is a launch like any other. So the game being mapped
 * is usually the second entry, and the cursor starts there. */
static u8 scope_menu(char names[GID_HIST_MAX][GID_TITLE_LEN + 1], int cnt) {
    int sel = (cnt > 1) ? 1 : 0;
    int i;

    for (;;) {
        u16 down;

        printf("\x1b[2J\x1b[1;1H");
        printf("Save this mapping for\n");
        printf("=====================\n\n");

        for (i = 0; i < cnt; i++) {
            printf("   %s %-40s%s\n", sel == i ? ">" : " ", names[i],
                i == 0 ? " (just launched)" : "");
        }
        printf("   %s Every game\n", sel == cnt ? ">" : " ");

        printf("\n  A per game profile wins over the general one while\n");
        printf("  that game is running. The general one is left alone\n");
        printf("  either way.\n");
        printf("\n  The top entry is usually this app rather than a game.\n");
        printf("\n  D-pad to choose, A to save.\n");

        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_UP) {
            sel = (sel + cnt) % (cnt + 1);
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % (cnt + 1);
        }
        if (down & PAD_BUTTON_A) {
            return (sel == cnt) ? GC_APP_SCOPE_GLOBAL
                : (u8)(GC_APP_SCOPE_GAME_BASE + sel);
        }
    }
}

static int mapping_wizard(void) {
    struct map_entry map[64];
    u8 dev, st, args[8];
    u32 btns;
    s8 ax[GC_APP_AXIS_CNT];
    int chan, i, s, n = 0, port = 0;
    int trig_mode, total, shown = 0;
    u64 commit_start;

    /* Asked while libogc can still read the GameCube pad, before the manual
     * transfers take the bus. */
    trig_mode = trigger_mode_menu();
    if (trig_mode < 0) {
        return 0;
    }
    total = CAPTURE_STEP_CNT + ((trig_mode == TRIG_SEPARATE) ? 2 : 0);

    si_grab();

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        pad_settle();
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
    {
        char ids[GID_HIST_MAX][GC_APP_GID_LEN + 1];
        char names[GID_HIST_MAX][GID_TITLE_LEN + 1];
        u8 scope = GC_APP_SCOPE_GLOBAL;
        int cnt = 0, k;

        for (k = 0; k < GID_HIST_MAX; k++) {
            if (app_get_gameid(chan, k, ids[cnt]) < 0 || !ids[cnt][0]) {
                break;
            }
            cnt++;
        }

        /* Capture mute off before anything asks for a button press. It holds
         * the mapped controller's port neutral, and that controller is normally
         * the only one connected, so a menu drawn while it is still muted
         * cannot be answered on the very pad that was just configured.
         *
         * It also makes the port look like an idle controller rather than an
         * absent one, so leaving it on reads as a pad that is connected but
         * does nothing, which survives unplugging and reconnecting it. */
        app_mode_send(chan, 0, (u8)port);

        if (cnt) {
            /* Off the bus before touching the card, and the names have to be in
             * hand before the menu can draw. */
            pad_settle();
            gid_titles(ids, names, cnt);
            scope = scope_menu(names, cnt);
            si_grab();
        }

        args[0] = (u8)n;
        args[1] = scope;
        app_map_send(chan, GC_APP_MAP_COMMIT, args, 2);
    }

    /* Bounded by the clock rather than by a count of attempts. An attempt costs
     * whatever a failed transfer costs, which is not fixed and is not something
     * this loop should be quietly paying a multiple of. */
    st = GC_APP_ST_BUSY;
    commit_start = gettime();
    while (ticks_to_millisecs(diff_ticks(commit_start, gettime())) < 10000) {
        if (app_read_status(chan, &st) == 0
                && (st == GC_APP_ST_OK || st == GC_APP_ST_ERROR)) {
            break;
        }
        usleep(10000);
    }

    pad_settle();

    printf("\x1b[2J\x1b[1;1H");
    if (st == GC_APP_ST_OK) {
        printf("Mapping saved.\n\n  %d entries written to port %d.\n", n, port + 1);
        printf("  It takes effect immediately, no restart.\n");
    }
    else {
        printf("Mapping NOT saved.\n\n  The adapter reported status %u.\n", st);
        printf("  Your previous mapping is untouched.\n");
    }

    printf("\n\n  Hold A to map another controller.\n");
    printf("  Hold B or START to finish.\n");
    return prompt_again();

cancelled:
    app_map_send(chan, GC_APP_MAP_CANCEL, NULL, 0);
    app_mode_send(chan, 0, (u8)port);
    pad_settle();
    printf("\x1b[2J\x1b[1;1H");
    printf("Cancelled. Nothing was changed.\n");
    printf("\n\n  Hold A to map another controller.\n");
    printf("  Hold B or START to finish.\n");
    return prompt_again();
}

/* Returns 0 mapping wizard, 1 input viewer, 2 firmware update. */
static int log_status(int chan, u8 *bank, u8 *state, u32 *len) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);

    req[0] = GC_LOG_STATUS_CMD;
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, req, 1, in, GC_LOG_STATUS_LEN) < 0) {
        return -1;
    }
    if (in[0] != GC_LOG_PROTO_VER) {
        return -1;
    }

    *bank = in[1];
    *state = in[2];
    *len = (u32)in[4] | ((u32)in[5] << 8) | ((u32)in[6] << 16) | ((u32)in[7] << 24);
    return 0;
}

static int log_send(int chan, u8 sub) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);

    out[0] = GC_LOG_CMD;
    out[1] = sub;

    /* Write only, like the mapping commands. */
    return si_xfer(chan, out, 2, NULL, 0);
}

static int log_read(int chan, u16 chunk, u8 *out8) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);

    req[0] = GC_LOG_READ_CMD;
    req[1] = (u8)chunk;
    req[2] = (u8)(chunk >> 8);
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, req, 3, in, GC_LOG_CHUNK) < 0) {
        return -1;
    }
    memcpy(out8, in, GC_LOG_CHUNK);
    return 0;
}

/* Both bank changes write the adapter's config file, so wait the busy out
 * the same way the firmware update does. */
static int log_wait_idle(int chan) {
    u64 start = gettime();
    u8 bank, state;
    u32 len;

    for (;;) {
        if (log_status(chan, &bank, &state, &len) == 0 && state != GC_LOG_ST_BUSY) {
            return (state == GC_LOG_ST_ERROR) ? -1 : 0;
        }
        if (ticks_to_millisecs(diff_ticks(start, gettime())) > READY_TIMEOUT_MS) {
            return -1;
        }
        VIDEO_WaitVSync();
    }
}

static void log_download(int chan, u32 len) {
    static u8 buf[GC_LOG_CHUNK];
    FILE *f;
    u32 done = 0;
    int last_pct = -1;

    if (len == 0) {
        printf("\nNothing captured yet.\n");
        return;
    }

    /* FAT is only needed here, so it is not mounted until now: the rest of
     * the app runs fine with no card in the slot. */
    if (!fat_ready()) {
        printf("\nNo FAT device. Need an SD Gecko or SD2SP2.\n");
        return;
    }

    f = fopen(LOG_PATH, "wb");
    if (!f) {
        f = fopen("sd:/" LOG_PATH, "wb");
    }
    if (!f) {
        printf("\nCould not open %s for writing.\n", LOG_PATH);
        return;
    }

    printf("\nSaving %lu bytes to %s\n", (unsigned long)len, LOG_PATH);

    while (done < len) {
        u32 want = len - done;
        int pct;

        if (want > GC_LOG_CHUNK) {
            want = GC_LOG_CHUNK;
        }

        /* Chunks are addressed, so a dropped transaction is just a re-read
         * rather than every later byte landing at the wrong offset. */
        if (log_read(chan, (u16)(done / GC_LOG_CHUNK), buf) < 0) {
            printf("\nRead failed at %lu bytes.\n", (unsigned long)done);
            fclose(f);
            return;
        }
        if (fwrite(buf, 1, want, f) != want) {
            printf("\nWrite failed at %lu bytes.\n", (unsigned long)done);
            fclose(f);
            return;
        }
        done += want;

        pct = (int)((done * 100) / len);
        if (pct != last_pct) {
            last_pct = pct;
            printf("\r  %d%%  ", pct);
        }
    }

    fclose(f);
    printf("\nDone. Copy %s off the card and send it over.\n", LOG_PATH);
}

/* Hold a message on screen until the user acknowledges it. Without this the
 * menu redraw wipes it within a frame or two. Assumes polling is already on. */
static void wait_ack(void) {
    int i;

    printf("\nPress any button to continue.\n");

    /* Any button, and a timeout behind that. Twice now a screen has been
     * escapable only by a pad that had stopped being polled, and a message
     * you cannot dismiss is a power cycle. */
    for (i = 0; i < 60 * 30; i++) {
        if (pad_any_down()) {
            return;
        }
        VIDEO_WaitVSync();
    }
}

/* The GameCube pad and our manual transfers cannot share the bus. libogc's PAD
 * driver only refreshes while SI polling runs, and polling fights SI_Transfer,
 * so every adapter access has to grab the bus and hand it straight back. That
 * is what pad_settle() is for, and its twenty frames also swallow the button
 * edge that selected the action, which would otherwise be read again by the
 * menu below and fire whatever the cursor had landed on.
 *
 * This screen is the only one that drives the GameCube pad rather than the
 * Bluetooth one, so it is the only one that has to do this per operation. */
static int log_bus_op(int chan, int op, u8 *bank, u8 *state, u32 *len) {
    int ret;

    si_grab();

    if (op < 0) {
        ret = log_status(chan, bank, state, len);
    }
    else {
        ret = log_send(chan, (u8)op);
        if (ret == 0) {
            ret = log_wait_idle(chan);
        }
    }

    pad_settle();
    return ret;
}

static void debug_log_menu(void) {
    u8 bank = 0, state = 0;
    u32 len = 0;
    int chan, sel = 0;

    si_grab();
    chan = ota_find_adapter();
    pad_settle();

    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        wait_ack();
        return;
    }

    for (;;) {
        u16 down;

        if (log_bus_op(chan, -1, &bank, &state, &len) < 0) {
            printf("\nThis firmware has no debug log support.\n");
            printf("Rebuild with CONFIG_BLUERETRO_GC_LOG.\n");
            wait_ack();
            return;
        }

        printf("\x1b[2J\x1b[1;1H");
        printf("BlueRetro Companion - debug log\n");
        printf("===============================\n\n");
        printf("  Adapter on port %d\n", chan + 1);
        printf("  Capturing: %s\n", bank ? "YES" : "no");
        printf("  Captured:  %lu bytes\n\n", (unsigned long)len);
        printf("   %s Start a fresh capture\n", sel == 0 ? ">" : " ");
        printf("   %s Stop capturing\n", sel == 1 ? ">" : " ");
        printf("   %s Save log to SD card\n", sel == 2 ? ">" : " ");
        printf("   %s Back\n", sel == 3 ? ">" : " ");
        printf("\n  D-pad to choose, A to select, B to go back.\n");
        printf("\n  Capture survives a reboot, so start one, reproduce the\n");
        printf("  fault, then come back here and save.\n");

        /* Polling is on here, courtesy of the pad_settle() in log_bus_op(). */
        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_UP) {
            sel = (sel + 3) % 4;
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % 4;
        }
        if (down & PAD_BUTTON_B) {
            return;
        }
        if (!(down & PAD_BUTTON_A)) {
            continue;
        }

        switch (sel) {
            case 0:
                /* Enabling rewinds on the adapter side, so this always
                 * starts from an empty buffer. */
                if (log_bus_op(chan, GC_LOG_SUB_BANK_ON, &bank, &state, &len) < 0) {
                    printf("\nCould not enable capture.\n");
                    wait_ack();
                }
                break;
            case 1:
                if (log_bus_op(chan, GC_LOG_SUB_BANK_OFF, &bank, &state, &len) < 0) {
                    printf("\nCould not disable capture.\n");
                    wait_ack();
                }
                break;
            case 2:
                si_grab();
                log_download(chan, len);
                pad_settle();
                wait_ack();
                break;
            default:
                return;
        }
    }
}

/* Both halves are shipped and updated separately, so a support request needs
 * both to be answerable. Read the adapter's once on the way in rather than per
 * redraw: it is three transactions and it does not change while we sit here. */
static char adapter_ver[GC_OTA_VER_LEN + 1] = "";

static void read_adapter_version(void) {
    int chan;

    si_grab();
    chan = ota_find_adapter();
    if (chan >= 0 && ota_get_version(chan, adapter_ver) < 0) {
        adapter_ver[0] = 0;
    }
    pad_settle();
}

static int main_menu(void) {
    int sel = 0;

    for (;;) {
        u16 down;

        printf("\x1b[2J\x1b[1;1H");
        printf("BlueRetro Companion\n");
        printf("===================\n\n");
        printf("  app      %s\n", APP_VERSION);
        printf("  adapter  %s\n\n", adapter_ver[0] ? adapter_ver : "not detected");
        printf("   %s Remap a controller\n", sel == 0 ? ">" : " ");
        printf("   %s Live input viewer\n", sel == 1 ? ">" : " ");
        printf("   %s Debug log\n", sel == 2 ? ">" : " ");
        printf("   %s Firmware slots\n", sel == 3 ? ">" : " ");
        printf("   %s Update firmware\n", sel == 4 ? ">" : " ");
        printf("\n  D-pad to choose, A to select, START to exit.\n");

        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_START) {
            app_exit();
        }
        if (down & PAD_BUTTON_UP) {
            sel = (sel + 4) % 5;
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % 5;
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

static u16 pad_any_held(void) {
    u16 held = 0;
    int i;

    PAD_ScanPads();
    for (i = 0; i < 4; i++) {
        held |= PAD_ButtonsHeld(i);
    }
    return held;
}

/* libogc's pad driver needs a moment after polling is handed back before its
 * edge state means anything, and the freshly remapped controller is driving
 * these same ports. Burn a few frames and throw the edges away, or the first
 * read can answer a prompt nobody has seen yet. */
static void pad_settle(void) {
    int i;

    SI_EnablePolling(SI_CHAN0_BIT|SI_CHAN1_BIT|SI_CHAN2_BIT|SI_CHAN3_BIT);
    for (i = 0; i < 20; i++) {
        PAD_ScanPads();
        VIDEO_WaitVSync();
    }
}

/* For after the adapter restarts, which takes all four ports away and brings
 * them back.
 *
 * libogc does re-probe a controller that vanished, but only from inside
 * PAD_ScanPads(), only once it has seen PAD_ERR_NO_CONTROLLER, and only
 * asynchronously: it calls PAD_Reset(), which disables SI polling on that
 * channel for the duration of the probe. pad_settle() spends its twenty frames
 * forcing polling back on across all four channels, outside that bookkeeping,
 * which lands straight on top of a probe that has only just started. The pad
 * then never comes back and there is no way left to press anything.
 *
 * So ask for the probe deliberately and then leave libogc alone long enough to
 * finish it, scanning but not touching polling. */
static void pad_reattach(void) {
    int i;

    SI_EnablePolling(SI_CHAN0_BIT|SI_CHAN1_BIT|SI_CHAN2_BIT|SI_CHAN3_BIT);
    PAD_Reset(PAD_CHAN0_BIT|PAD_CHAN1_BIT|PAD_CHAN2_BIT|PAD_CHAN3_BIT);

    for (i = 0; i < 240; i++) {
        PAD_ScanPads();
        VIDEO_WaitVSync();
    }
}

/* Waits for a sustained hold rather than an edge. A remapped controller can
 * produce spurious edges, from a stick sitting near a threshold or a button
 * still down from the last capture, and a single stray edge should not decide
 * whether the wizard runs again. Returns 1 for A, 0 for B or Start. */
static int prompt_again(void) {
    int a = 0, b = 0;

    for (;;) {
        u16 held = pad_any_held();

        if (held & PAD_BUTTON_A) {
            if (++a > 25) {
                return 1;
            }
        }
        else {
            a = 0;
        }

        if (held & (PAD_BUTTON_B | PAD_BUTTON_START)) {
            if (++b > 25) {
                return 0;
            }
        }
        else {
            b = 0;
        }
        VIDEO_WaitVSync();
    }
}

/* ------------------------------------------------------------------ *
 * Returning to the loader
 *
 * exit() drops back to whatever launched us, which works from Swiss but
 * leaves PicoLoader and friends with nowhere to go: PicoLoader in
 * particular needs the console power cycled to get out of. So look for a
 * loader DOL on the card and chainload it, and only fall back to exit()
 * when there is nothing to chainload.
 * ------------------------------------------------------------------ */

/* Both well clear of where any DOL loads. Our own image ends just past
 * 0x80081a80 and a target links from 0x80003100, so anything staged in the
 * heap would sit right in the path of the copy. */
#define DOL_STAGE_ADDR 0x80C00000
#define DOL_TRAMP_ADDR 0x81200000
#define DOL_MAX_SIZE (6 * 1024 * 1024)
#define DOL_TRAMP_CODE 256

#define DOL_TEXT_CNT 7
#define DOL_DATA_CNT 11

typedef struct {
    u32 text_off[DOL_TEXT_CNT];
    u32 data_off[DOL_DATA_CNT];
    u32 text_addr[DOL_TEXT_CNT];
    u32 data_addr[DOL_DATA_CNT];
    u32 text_size[DOL_TEXT_CNT];
    u32 data_size[DOL_DATA_CNT];
    u32 bss_addr;
    u32 bss_size;
    u32 entry;
    u32 pad[7];
} dol_header;

/* Candidates in order of how likely they are to be a loader that wants us
 * back. igr.dol is Swiss's own return stub, so it wins when present. */
static const char *dol_names[] = {
    "/igr.dol",
    "/swiss/igr.dol",
    "/swiss/boot.dol",
    "/apps/swiss/boot.dol",
    "/boot.dol",
};

/* Runs from a relocated copy, because by the time the section copy is under
 * way the original of this code has been overwritten by the incoming DOL.
 * That makes it position independent by necessity: no globals, no calls, no
 * return. r3 arrives pointing at the entry address followed by {dst, src,
 * bytes} triples, terminated by a zero dst.
 *
 * Cache maintenance is per word rather than per line. Redundant, since a
 * line covers eight of them, but a few hundred thousand extra dcbf on a
 * multi megabyte image still costs only milliseconds and it removes any
 * question of a partial line being left stale. */
static void dol_trampoline(void) {
    __asm__ volatile (
        "lwz     31, 0(3)\n"
        "addi    3, 3, 4\n"
        "1:\n"
        "lwz     4, 0(3)\n"
        "lwz     5, 4(3)\n"
        "lwz     6, 8(3)\n"
        "addi    3, 3, 12\n"
        "cmpwi   4, 0\n"
        "beq     3f\n"
        "cmpwi   6, 0\n"
        "beq     1b\n"
        "2:\n"
        "lwz     7, 0(5)\n"
        "stw     7, 0(4)\n"
        "dcbf    0, 4\n"
        "icbi    0, 4\n"
        "addi    4, 4, 4\n"
        "addi    5, 5, 4\n"
        "addic.  6, 6, -4\n"
        "bgt     2b\n"
        "b       1b\n"
        "3:\n"
        "sync\n"
        "isync\n"
        "li      3, 0\n"
        "mtlr    31\n"
        "blr\n"
    );
}

/* Opens the first candidate that exists, trying the device we were loaded
 * from first. Swiss fills argv[0] in with our own path, which is the only
 * reliable way to tell an SD Gecko in slot A from one in slot B. */
static FILE *dol_open(char *shown, int shown_len) {
    char path[128];
    char dev[32];
    unsigned i, d;

    dev[0] = 0;
    if (__system_argv && __system_argv->argvMagic == ARGV_MAGIC
            && __system_argv->argc > 0 && __system_argv->argv[0]) {
        const char *a = __system_argv->argv[0];
        const char *c = strchr(a, ':');

        if (c && (size_t)(c - a) < sizeof(dev) - 2) {
            memcpy(dev, a, c - a + 1);
            dev[c - a + 1] = 0;
        }
    }

    for (d = 0; d < 3; d++) {
        const char *prefix = (d == 0) ? dev : ((d == 1) ? "" : "sd:");

        if (d == 0 && !dev[0]) {
            continue;
        }

        for (i = 0; i < sizeof(dol_names) / sizeof(dol_names[0]); i++) {
            FILE *f;

            snprintf(path, sizeof(path), "%s%s", prefix, dol_names[i]);
            f = fopen(path, "rb");
            if (f) {
                snprintf(shown, shown_len, "%s", path);
                return f;
            }
        }
    }
    return NULL;
}

/* Returns only on failure. */
static void dol_chainload(void) {
    char shown[128];
    dol_header *h = (dol_header *)DOL_STAGE_ADDR;
    u32 *desc = (u32 *)(DOL_TRAMP_ADDR + DOL_TRAMP_CODE);
    u32 *d = desc;
    void (*tramp)(u32 *) = (void (*)(u32 *))DOL_TRAMP_ADDR;
    FILE *f;
    long size;
    int i;

    if (!fat_ready()) {
        return;
    }

    f = dol_open(shown, sizeof(shown));
    if (!f) {
        return;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < (long)sizeof(dol_header) || size > DOL_MAX_SIZE) {
        fclose(f);
        return;
    }
    if (fread((void *)DOL_STAGE_ADDR, 1, size, f) != (size_t)size) {
        fclose(f);
        return;
    }
    fclose(f);

    printf("\nReturning via %s\n", shown);
    VIDEO_WaitVSync();

    *d++ = h->entry;
    for (i = 0; i < DOL_TEXT_CNT; i++) {
        if (h->text_size[i] && h->text_addr[i]) {
            *d++ = h->text_addr[i];
            *d++ = DOL_STAGE_ADDR + h->text_off[i];
            *d++ = (h->text_size[i] + 3) & ~3u;
        }
    }
    for (i = 0; i < DOL_DATA_CNT; i++) {
        if (h->data_size[i] && h->data_addr[i]) {
            *d++ = h->data_addr[i];
            *d++ = DOL_STAGE_ADDR + h->data_off[i];
            *d++ = (h->data_size[i] + 3) & ~3u;
        }
    }
    *d++ = 0;
    *d++ = 0;
    *d++ = 0;

    /* bss is left alone deliberately. Zeroing it here would mean writing
     * over ourselves before the copy loop is even in place, and every DOL
     * devkitPPC builds clears its own in crt0. */

    memcpy((void *)DOL_TRAMP_ADDR, (const void *)dol_trampoline, DOL_TRAMP_CODE);

    /* Hand the pads back before the bus goes away with us. */
    SI_EnablePolling(SI_CHAN0_BIT | SI_CHAN1_BIT | SI_CHAN2_BIT | SI_CHAN3_BIT);
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    SYS_ResetSystem(SYS_SHUTDOWN, 0, FALSE);

    DCFlushRange((void *)DOL_STAGE_ADDR, size);
    DCFlushRange((void *)DOL_TRAMP_ADDR, DOL_TRAMP_CODE + (u32)((d - desc) * 4));
    ICInvalidateRange((void *)DOL_TRAMP_ADDR, DOL_TRAMP_CODE);

    IRQ_Disable();
    tramp(desc);
}

static void app_exit(void) {
    dol_chainload();
    /* Nothing to chainload, or it would not load. Out the usual way. */
    exit(0);
}

/* ------------------------------------------------------------------ *
 * Firmware slots
 * ------------------------------------------------------------------ */

static const char *slot_name[GC_BOOT_SLOT_CNT] = { "Factory", "Slot A ", "Slot B " };

static int boot_info(int chan, u8 *running, u8 *next, u8 *mask, u8 *state) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);

    req[0] = GC_BOOT_INFO_CMD;
    memset(in, 0, sizeof(in));

    if (si_xfer(chan, req, 1, in, GC_BOOT_INFO_LEN) < 0) {
        return -1;
    }
    if (in[0] != GC_BOOT_PROTO_VER) {
        return -1;
    }

    *running = in[1];
    *next = in[2];
    *mask = in[3];
    *state = in[4];
    return 0;
}

static int boot_version(int chan, u8 slot, char *out) {
    static u8 req[32] ATTRIBUTE_ALIGN(32);
    static u8 in[32] ATTRIBUTE_ALIGN(32);
    int chunk;

    memset(out, 0, GC_BOOT_VER_LEN + 1);

    for (chunk = 0; chunk < GC_BOOT_VER_LEN / GC_BOOT_VER_CHUNK; chunk++) {
        req[0] = GC_BOOT_VER_CMD;
        req[1] = slot;
        req[2] = (u8)chunk;
        memset(in, 0, sizeof(in));

        if (si_xfer(chan, req, 3, in, GC_BOOT_VER_CHUNK) < 0) {
            return -1;
        }
        memcpy(out + chunk * GC_BOOT_VER_CHUNK, in, GC_BOOT_VER_CHUNK);
    }
    out[GC_BOOT_VER_LEN] = 0;
    return 0;
}

static int boot_select(int chan, u8 slot) {
    static u8 out[32] ATTRIBUTE_ALIGN(32);

    out[0] = GC_BOOT_SEL_CMD;
    out[1] = slot;

    return si_xfer(chan, out, 2, NULL, 0);
}

/* Same bus discipline as the log screen: grab it for the transfers, hand it
 * straight back so the pad keeps working. */
static int boot_read_all(int chan, u8 *running, u8 *next, u8 *mask, u8 *state,
                          char ver[GC_BOOT_SLOT_CNT][GC_BOOT_VER_LEN + 1]) {
    int ret, i;

    si_grab();

    ret = boot_info(chan, running, next, mask, state);
    if (ret == 0) {
        for (i = 0; i < GC_BOOT_SLOT_CNT; i++) {
            if (*mask & (1 << i)) {
                if (boot_version(chan, (u8)i, ver[i]) < 0) {
                    snprintf(ver[i], GC_BOOT_VER_LEN + 1, "%s", "(unreadable)");
                }
            }
            else {
                snprintf(ver[i], GC_BOOT_VER_LEN + 1, "%s", "-- empty --");
            }
        }
    }

    pad_settle();
    return ret;
}

/* The boot choice is persistent: it lives in the OTA data partition, and
 * picking the factory image works by erasing that, after which the
 * bootloader falls back to factory on every boot. A power cycle does not
 * undo it. So if the slot being selected holds firmware without these
 * opcodes, this app cannot bring you back, and that is worth spelling out
 * rather than discovering. Hence a hold rather than a tap. */
static int boot_confirm(int slot, const char *ver) {
    int a = 0, b = 0;

    printf("\x1b[2J\x1b[1;1H");
    printf("Boot %s?\n", slot_name[slot]);
    printf("=============\n\n");
    printf("  %s\n\n", ver);
    printf("  This sticks. The boot slot is stored in flash, so a\n");
    printf("  power cycle will not undo it.\n\n");
    printf("  If that firmware predates this app, you cannot come\n");
    printf("  back from this screen. Ways back, best first:\n");
    printf("    - Update firmware here, if it has the updater\n");
    printf("    - The Bluetooth web config\n");
    printf("    - A serial flash\n\n");
    printf("  Hold A to boot it, B to cancel.\n");

    for (;;) {
        u16 held = pad_any_held();

        if (held & PAD_BUTTON_A) {
            if (++a > 60) {
                return 1;
            }
        }
        else {
            a = 0;
        }

        if (held & (PAD_BUTTON_B | PAD_BUTTON_START)) {
            if (++b > 15) {
                return 0;
            }
        }
        else {
            b = 0;
        }
        VIDEO_WaitVSync();
    }
}

/* The adapter is in no hurry. It validates the image it has been told to
 * boot, checksumming the better part of a megabyte, and only then queues the
 * restart, which waits a further second of its own before calling
 * esp_restart(). The ports do not drop until that fires. So for several
 * seconds after the command the adapter is alive and answering, and then it
 * is gone for several more.
 *
 * Guessing a duration got this wrong twice. Watch the two edges instead: the
 * moment it stops answering, and the moment it answers again.
 *
 * The bus stays held for the whole wait. Re-probing the pads here is what the
 * previous attempt did, and libogc's probe packets sit on exactly the
 * channels these reads need, so every read came back refused and the screen
 * concluded the firmware had no slot support. Pads are reattached once, at
 * the end, when nothing else wants the bus. */
static void boot_wait_restart(int chan) {
    u8 running, next, mask, state;
    u64 start = gettime();
    int gone = 0, dots = 0;

    printf("\n  Waiting for the adapter to restart");

    while (ticks_to_millisecs(diff_ticks(start, gettime())) < 40000) {
        int ok = (boot_info(chan, &running, &next, &mask, &state) == 0);

        if (ok && !gone && state == GC_BOOT_ST_ERROR) {
            /* Refused before it ever got as far as restarting, so there is
             * nothing here to wait for. */
            printf("\n\n  The adapter refused that slot.\n");
            return;
        }

        if (!gone && !ok) {
            gone = 1;
            dots = 0;
            printf("\n  Ports dropped, waiting for them to come back");
        }
        else if (gone && ok) {
            printf("\n\n  Back on slot %u.\n", running);
            return;
        }

        if (++dots >= 20) {
            dots = 0;
            printf(".");
        }
        VIDEO_WaitVSync();
    }

    printf("\n\n  Gave up waiting. If the adapter came back on its own,\n");
    printf("  leave and re-enter this screen to see where it landed.\n");
}

static void firmware_slots_menu(void) {
    char ver[GC_BOOT_SLOT_CNT][GC_BOOT_VER_LEN + 1];
    u8 running = 0, next = 0, mask = 0, state = 0;
    /* Starts on Back, so a stray A does not boot whatever is first. */
    int chan, sel = GC_BOOT_SLOT_CNT, i, tries;

    si_grab();
    chan = ota_find_adapter();
    pad_settle();

    if (chan < 0) {
        printf("\nNo BlueRetro adapter answered on any port.\n");
        wait_ack();
        return;
    }

    for (;;) {
        u16 down;

        for (tries = 0; tries < 10; tries++) {
            if (boot_read_all(chan, &running, &next, &mask, &state, ver) == 0) {
                break;
            }
            VIDEO_WaitVSync();
        }
        if (tries == 10) {
            /* Only reachable on the way in. After a swap the wait below has
             * already established the adapter is back, and reporting missing
             * support there was simply wrong: the firmware had just answered
             * these same reads a moment earlier. */
            printf("\nThe adapter is not answering the slot commands.\n");
            printf("If this firmware predates them, rebuild it with\n");
            printf("CONFIG_BLUERETRO_GC_BOOT.\n");
            wait_ack();
            return;
        }

        printf("\x1b[2J\x1b[1;1H");
        printf("BlueRetro Companion - firmware slots\n");
        printf("====================================\n\n");
        printf("  Adapter on port %d\n\n", chan + 1);

        for (i = 0; i < GC_BOOT_SLOT_CNT; i++) {
            printf("   %s %s %-24s%s%s\n",
                sel == i ? ">" : " ", slot_name[i], ver[i],
                (running == i) ? " [running]" : "",
                (next == i && running != i) ? " [boots next]" : "");
        }
        printf("   %s Back\n", sel == GC_BOOT_SLOT_CNT ? ">" : " ");

        printf("\n  A boots the highlighted slot. The adapter restarts.\n");
        printf("  The choice sticks: a power cycle will not undo it.\n");
        printf("\n  Factory is whatever was last flashed over serial and\n");
        printf("  is never touched by an update. Check its version above\n");
        printf("  before going there, since older builds cannot come back.\n");

        do {
            down = pad_any_down();
            VIDEO_WaitVSync();
        } while (!down);

        if (down & PAD_BUTTON_UP) {
            sel = (sel + GC_BOOT_SLOT_CNT) % (GC_BOOT_SLOT_CNT + 1);
        }
        if (down & PAD_BUTTON_DOWN) {
            sel = (sel + 1) % (GC_BOOT_SLOT_CNT + 1);
        }
        if (down & PAD_BUTTON_B) {
            return;
        }
        if (!(down & PAD_BUTTON_A)) {
            continue;
        }
        if (sel == GC_BOOT_SLOT_CNT) {
            return;
        }

        if (!(mask & (1 << sel))) {
            printf("\nThat slot is empty.\n");
            wait_ack();
            continue;
        }
        if (running == sel) {
            printf("\nThat slot is already running.\n");
            wait_ack();
            continue;
        }
        if (!boot_confirm(sel, ver[sel])) {
            continue;
        }

        printf("\x1b[2J\x1b[1;1H");
        printf("Booting %s.\n", slot_name[sel]);

        si_grab();
        boot_select(chan, (u8)sel);
        boot_wait_restart(chan);

        /* Only now, with the adapter back and nothing else on the bus. */
        pad_reattach();
    }
}

static void wait_exit(void) {
    printf("\nPress START to exit.\n");
    for (;;) {
        if (pad_any_down() & PAD_BUTTON_START) {
            app_exit();
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

    read_adapter_version();

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
        else if (choice == 2) {
            debug_log_menu();
            wait_exit();
        }
        else if (choice == 3) {
            firmware_slots_menu();
            wait_exit();
        }
        /* Anything else falls through to the firmware update below. Both of the
         * branches above end in wait_exit(), which does not return. */
    }

    printf("\n\nBlueRetro Companion - firmware update\n");
    printf("=====================================\n\n");

    if (!fat_ready()) {
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
    si_grab();

    chan = ota_find_adapter();
    if (chan < 0) {
        printf("No BlueRetro adapter answered on any port.\n");
        printf("Is the firmware built with CONFIG_BLUERETRO_GC_OTA?\n");
        pad_settle();
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
        pad_settle();

        printf("\nPress A to flash, B to cancel.\n");
        for (;;) {
            u16 down = pad_any_down();

            if (down & PAD_BUTTON_A) {
                break;
            }
            if (down & PAD_BUTTON_B) {
                printf("\nCancelled. Nothing was written.\n");
                pad_settle();
                fclose(f);
                wait_exit();
            }
            VIDEO_WaitVSync();
        }

        si_grab();
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
        u64 wait_start = gettime();

        /* Clock bounded for the same reason as the mapping commit: an attempt
         * costs whatever a failed transfer costs, and here the adapter is on
         * its way out, so every attempt is a failed one. */
        while (ticks_to_millisecs(diff_ticks(wait_start, gettime())) < 3000) {
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

    /* Same restart as a slot swap, so the ports need the same re-probe before
     * anything can be pressed. */
    pad_reattach();
    fclose(f);
    wait_exit();
    return 0;

fail:
    ota_send(chan, GC_OTA_SUB_ABORT, 0, NULL);
    pad_settle();
    fclose(f);
    printf("\nUpdate aborted. The adapter kept its current firmware.\n");
    wait_exit();
    return 1;
}
