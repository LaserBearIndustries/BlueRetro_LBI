/*
 * Copyright (c) 2019-2025, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include "nvs.h"
#include "zephyr/types.h"
#include "tools/util.h"
#include "adapter.h"
#include "config.h"
#include "system/fs.h"
#include "adapter/gameid.h"
#include "bluetooth/mon.h"
#include "system/manager.h"

struct config config;
struct hw_config hw_config = {
    .external_adapter = 0,
    .hotplug = 0,
    .hw1_ports_led_pins = {2, 4, 12, 15},
    .led_flash_duty_cycle = 0x80000,
    .led_flash_hz = {2, 4, 8},
    .led_flash_off_duty_cycle = 0,
    .led_flash_on_duty_cycle = 0xFFFFF,
    .led_pulse_duty_max = 2000,
    .led_pulse_duty_min = 50,
    .led_pulse_fade_cycle_delay_ms = 500,
    .led_pulse_fade_time_ms = 500,
    .led_pulse_hz = 5000,
    .led_pulse_off_duty_cycle = 0,
    .led_pulse_on_duty_cycle = 0x1FFF,
    .port_cnt = 2,
    .ports_sense_input_polarity = 0,
    .ports_sense_output_ms = 1000,
    .ports_sense_output_od = 0,
    .ports_sense_output_polarity = 0,
    .ports_sense_p3_p4_as_output = 0,
    .power_pin_is_hold = 0,
    .power_pin_od = 0,
    .power_pin_polarity = 0,
    .power_pin_pulse_ms = 20,
    .reset_pin_od = 1,
    .reset_pin_polarity = 0,
    .reset_pin_pulse_ms = 500,
    .sw_io0_hold_thres_ms = {1000, 3000, 6000},
    .ps_ctrl_colors = {
        0xFF0000, /* Blue */
        0x0000FF, /* Red */
        0x00FF00, /* Green */
        0xFF00FF, /* Pink */
        0xFFFF00, /* Cyan */
        0x0080FF, /* Orange */
        0x00FFFF, /* Yellow */
        0xFF0080, /* Purple */
    },
};

static char *hw_config_name_idx[] = {
    "ext_adapter",
    "hotplug",
    "hw1_led_pins_0",
    "hw1_led_pins_1",
    "hw1_led_pins_2",
    "hw1_led_pins_3",
    "led_flash_duty",
    "led_flash_hz_0",
    "led_flash_hz_1",
    "led_flash_hz_2",
    "led_f_off_duty",
    "led_f_on_duty",
    "led_p_duty_max",
    "led_p_duty_min",
    "led_p_fade_c_ms",
    "led_p_fade_t_ms",
    "led_pulse_hz",
    "led_p_off_duty",
    "led_p_on_duty",
    "port_cnt",
    "ports_s_in_pol",
    "ports_s_out_ms",
    "ports_s_out_od",
    "ports_s_out_pol",
    "ports_s_out_en",
    "pwr_pin_is_hold",
    "pwr_pin_od",
    "pwr_pin_pol",
    "pwr_pin_p_ms",
    "reset_pin_od",
    "reset_pin_pol",
    "reset_pin_p_ms",
    "sw_thres_ms_0",
    "sw_thres_ms_1",
    "sw_thres_ms_2",
    "ps_ctrl_color_0",
    "ps_ctrl_color_1",
    "ps_ctrl_color_2",
    "ps_ctrl_color_3",
    "ps_ctrl_color_4",
    "ps_ctrl_color_5",
    "ps_ctrl_color_6",
    "ps_ctrl_color_7",
};
static uint32_t config_src = DEFAULT_CFG;
static uint32_t config_version_magic[] = {
    CONFIG_MAGIC_V0,
    CONFIG_MAGIC_V1,
    CONFIG_MAGIC_V2,
    CONFIG_MAGIC_V3,
};
#ifdef CONFIG_BLUERETRO_SYSTEM_GC
/* GameCube: Super Smash Bros. Melee resets a match on L+R+A+Start, which is bit for
 * bit the stock SYS_POWER_OFF combo (MACRO_BASE = LM+RM+MM, action = RB_DOWN), so
 * every match reset also asks the adapter to cut power. Base the combos on PAD_MQ
 * instead of PAD_MM: no GameCube title can assert it, and on the Switch 2 GameCube
 * pad it is the Capture button. Reset and power off also swap onto A and B, putting
 * the more frequently wanted action on the easier button.
 *
 * Caveat: controllers with no PAD_MQ (Wii U Pro, PS3) cannot satisfy MACRO_BASE and
 * lose every combo until remapped via the web config. See README.
 */
static uint8_t config_default_combo[BR_COMBO_CNT] = {
    PAD_LM, PAD_RM, PAD_MQ, PAD_RB_UP, PAD_RB_DOWN, PAD_RB_RIGHT, PAD_RB_LEFT, PAD_LD_UP, PAD_LD_DOWN, PAD_MS
};

/* A GameCube build only ever drives GameCube pads, so the stock keyboard and
 * mouse identity map is dead weight. Map exactly what the console has, plus the
 * two derived entries that turn a full analog trigger pull into the digital
 * click: the GameCube reports L and R both as an axis and as a switch, and games
 * expect the switch once the trigger bottoms out. */
struct config_default_map {
    uint8_t src_btn;
    uint8_t dst_btn;
    uint8_t perc_threshold;
};

static const struct config_default_map config_default_map[] = {
    {PAD_LX_LEFT,  PAD_LX_LEFT,  50},
    {PAD_LX_RIGHT, PAD_LX_RIGHT, 50},
    {PAD_LY_DOWN,  PAD_LY_DOWN,  50},
    {PAD_LY_UP,    PAD_LY_UP,    50},
    {PAD_RX_LEFT,  PAD_RX_LEFT,  50},
    {PAD_RX_RIGHT, PAD_RX_RIGHT, 50},
    {PAD_RY_DOWN,  PAD_RY_DOWN,  50},
    {PAD_RY_UP,    PAD_RY_UP,    50},
    {PAD_LD_LEFT,  PAD_LD_LEFT,  50},
    {PAD_LD_RIGHT, PAD_LD_RIGHT, 50},
    {PAD_LD_DOWN,  PAD_LD_DOWN,  50},
    {PAD_LD_UP,    PAD_LD_UP,    50},
    {PAD_RB_LEFT,  PAD_RB_LEFT,  50},
    {PAD_RB_RIGHT, PAD_RB_RIGHT, 50},
    {PAD_RB_DOWN,  PAD_RB_DOWN,  50},
    {PAD_RB_UP,    PAD_RB_UP,    50},
    {PAD_MM,       PAD_MM,       50},
    {PAD_LM,       PAD_LM,       50},
    {PAD_LM,       PAD_LT,       95},
    {PAD_LS,       PAD_LS,       50},
    {PAD_RM,       PAD_RM,       50},
    {PAD_RM,       PAD_RT,       95},
    {PAD_RS,       PAD_RS,       50},
};
#define CONFIG_DEFAULT_MAP_CNT ARRAY_SIZE(config_default_map)
#else
static uint8_t config_default_combo[BR_COMBO_CNT] = {
    PAD_LM, PAD_RM, PAD_MM, PAD_RB_UP, PAD_RB_LEFT, PAD_RB_RIGHT, PAD_RB_DOWN, PAD_LD_UP, PAD_LD_DOWN, PAD_MS
};
#endif
static bool config_rst_bare_core = false;

static void config_init_struct(struct config *data);
static void config_init_nvs_patch(struct config *data);
static int32_t config_load_from_file(struct config *data, char *filename);
static int32_t config_store_on_file(struct config *data, char *filename);
static int32_t config_v0_update(struct config *data, char *filename);
static int32_t config_v1_update(struct config *data, char *filename);
static int32_t config_v2_update(struct config *data, char *filename);

static int32_t (*config_ver_update[])(struct config *data, char *filename) = {
    config_v0_update,
    config_v1_update,
    config_v2_update,
    NULL,
};

static int32_t config_v0_update(struct config *data, char *filename) {
    memmove((uint8_t *)data + 7, (uint8_t *)data + 6, sizeof(*data) - 7);

    data->magic = CONFIG_MAGIC;
    data->global_cfg.inquiry_mode = INQ_AUTO;

    return config_store_on_file(data, filename);
}

static int32_t config_v1_update(struct config *data, char *filename) {
    memmove((uint8_t *)data + 8, (uint8_t *)data + 7, 31 - 8);

    data->magic = CONFIG_MAGIC;
    data->global_cfg.banksel = 0;

    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        printf("%s: failed to open file for reading\n", __FUNCTION__);
        goto fail;
    }
    else {
        uint32_t count = 0;
        for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
            fseek(file, 31 + (3 + 255 * 8) * i, SEEK_SET);
            count += fread((uint8_t *)&data->in_cfg[i], sizeof(struct in_cfg), 1, file);
            if (data->in_cfg[i].map_size > ADAPTER_MAPPING_MAX) {
                data->in_cfg[i].map_size = ADAPTER_MAPPING_MAX;
            }
        }
        fclose(file);

        if (count != WIRED_MAX_DEV) {
            goto fail;
        }
    }
    return config_store_on_file(data, filename);

fail:
    printf("%s: Update failed, reset config (Sorry!)\n", __FUNCTION__);
    config_init_struct(data);
    config_init_nvs_patch(data);
    return config_store_on_file(data, filename);
}

static int32_t config_get_version(uint32_t magic) {
    for (uint32_t i = 0; i < ARRAY_SIZE(config_version_magic); i++) {
        if (magic == config_version_magic[i]) {
            return i;
        }
    }
    return -1;
}

static int32_t config_v2_update(struct config *data, char *filename) {
    data->magic = CONFIG_MAGIC;

    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        uint32_t j = data->in_cfg[i].map_size;

        /* map_size came out of the file, so it is not to be trusted -
         * config_v1_update clamps it for the same reason. Without this,
         * appending the combos to a mapping that was already near full
         * writes past map_cfg[] and into the next port's in_cfg, and
         * leaves a map_size larger than the array it indexes.
         *
         * That corruption then persists: this function stamps the current
         * magic and stores the file, so every later boot sees a config
         * that looks valid and nothing ever re-checks it. The only way out
         * was a factory reset, which is what one customer needed after
         * updating from an older build.
         *
         * Dropping the tail of an over-length mapping is the lesser loss:
         * the combos are what let anyone reset or power off the adapter
         * from the pad. */
        if (j > ADAPTER_MAPPING_MAX - BR_COMBO_CNT) {
            printf("%s: dev %lu map_size %lu, clamping to fit the combos\n",
                __FUNCTION__, i, j);
            j = ADAPTER_MAPPING_MAX - BR_COMBO_CNT;
        }

        data->in_cfg[i].map_size = j + BR_COMBO_CNT;
        for (uint32_t k = 0; k < BR_COMBO_CNT; j++, k++) {
            data->in_cfg[i].map_cfg[j].src_btn = config_default_combo[k];
            data->in_cfg[i].map_cfg[j].dst_btn = k + BR_COMBO_BASE_1;
            data->in_cfg[i].map_cfg[j].dst_id = i;
            data->in_cfg[i].map_cfg[j].perc_max = 100;
            data->in_cfg[i].map_cfg[j].perc_threshold = 50;
            data->in_cfg[i].map_cfg[j].perc_deadzone = 135;
            data->in_cfg[i].map_cfg[j].turbo = 0;
            data->in_cfg[i].map_cfg[j].algo = 0;
        }
    }

    return config_store_on_file(data, filename);
}

static int32_t hw_config_lookup_key_name(const char* key) {
    for (uint32_t i = 0; i < sizeof(hw_config_name_idx)/sizeof(*hw_config_name_idx); i++) {
        if (strstr(key, hw_config_name_idx[i]) != NULL) {
            return i;
        }
    }
    return -1;
}

static void config_init_struct(struct config *data) {
    data->magic = CONFIG_MAGIC;
    data->global_cfg.system_cfg = WIRED_AUTO;
    data->global_cfg.multitap_cfg = MT_NONE;
    data->global_cfg.inquiry_mode = INQ_AUTO;
    data->global_cfg.banksel = 0;

    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        data->out_cfg[i].dev_mode = DEV_PAD;
#ifdef CONFIG_BLUERETRO_SYSTEM_GC
        data->out_cfg[i].acc_mode = ACC_RUMBLE;
#else
        data->out_cfg[i].acc_mode = ACC_NONE;
#endif
        data->in_cfg[i].bt_dev_id = 0x00; /* Not used placeholder */
        data->in_cfg[i].bt_subdev_id = 0x00;  /* Not used placeholder */
        uint32_t j = 0;
#ifdef CONFIG_BLUERETRO_SYSTEM_GC
        data->in_cfg[i].map_size = CONFIG_DEFAULT_MAP_CNT + BR_COMBO_CNT;
        for (; j < CONFIG_DEFAULT_MAP_CNT; j++) {
            data->in_cfg[i].map_cfg[j].src_btn = config_default_map[j].src_btn;
            data->in_cfg[i].map_cfg[j].dst_btn = config_default_map[j].dst_btn;
            data->in_cfg[i].map_cfg[j].dst_id = i;
            data->in_cfg[i].map_cfg[j].perc_max = 100;
            data->in_cfg[i].map_cfg[j].perc_threshold = config_default_map[j].perc_threshold;
            data->in_cfg[i].map_cfg[j].perc_deadzone = 135;
            data->in_cfg[i].map_cfg[j].turbo = 0;
            data->in_cfg[i].map_cfg[j].algo = 0;
        }
#else
        data->in_cfg[i].map_size = KBM_MAX + BR_COMBO_CNT;
        for (; j < KBM_MAX; j++) {
            data->in_cfg[i].map_cfg[j].src_btn = j;
            data->in_cfg[i].map_cfg[j].dst_btn = j;
            data->in_cfg[i].map_cfg[j].dst_id = i;
            data->in_cfg[i].map_cfg[j].perc_max = 100;
            data->in_cfg[i].map_cfg[j].perc_threshold = 50;
            data->in_cfg[i].map_cfg[j].perc_deadzone = 135;
            data->in_cfg[i].map_cfg[j].turbo = 0;
            data->in_cfg[i].map_cfg[j].algo = 0;
        }
#endif
        for (uint32_t k = 0; k < BR_COMBO_CNT; j++, k++) {
            data->in_cfg[i].map_cfg[j].src_btn = config_default_combo[k];
            data->in_cfg[i].map_cfg[j].dst_btn = k + BR_COMBO_BASE_1;
            data->in_cfg[i].map_cfg[j].dst_id = i;
            data->in_cfg[i].map_cfg[j].perc_max = 100;
            data->in_cfg[i].map_cfg[j].perc_threshold = 50;
            data->in_cfg[i].map_cfg[j].perc_deadzone = 135;
            data->in_cfg[i].map_cfg[j].turbo = 0;
            data->in_cfg[i].map_cfg[j].algo = 0;
        }
    }
}

static void config_init_nvs_patch(struct config *data) {
    esp_err_t err;
    nvs_handle_t nvs;
    uint8_t value;

    err = nvs_open("global", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs, "system", &value);
        if (err == ESP_OK) {
            data->global_cfg.system_cfg = value;
        }
        err = nvs_get_u8(nvs, "multitap", &value);
        if (err == ESP_OK) {
            data->global_cfg.multitap_cfg = value;
        }
        err = nvs_get_u8(nvs, "inquiry", &value);
        if (err == ESP_OK) {
            data->global_cfg.inquiry_mode = value;
        }
        err = nvs_get_u8(nvs, "bank", &value);
        if (err == ESP_OK) {
            data->global_cfg.banksel = value;
        }
        nvs_close(nvs);
    }

    err = nvs_open("output", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs, "mode", &value);
        if (err == ESP_OK) {
            for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
                data->out_cfg[i].dev_mode = value;
            }
        }
        err = nvs_get_u8(nvs, "accessories", &value);
        if (err == ESP_OK) {
            for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
                data->out_cfg[i].acc_mode = value;
            }
        }
        nvs_close(nvs);
    }

    err = nvs_open("mapping", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        nvs_iterator_t it = NULL;
        err = nvs_entry_find_in_handle(nvs, NVS_TYPE_ANY, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            errno = 0;
            uint32_t index = strtol(info.key, NULL, 10);
            if (!errno) {
                struct map_cfg mapping = {0};
                size_t size = sizeof(struct map_cfg);
                err = nvs_get_blob(nvs, info.key, &mapping, &size);
                if (err == ESP_OK) {
                    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
                        memcpy(&data->in_cfg[i].map_cfg[index], &mapping, sizeof(struct map_cfg));
                        data->in_cfg[i].map_cfg[index].dst_id = i;
                    }
                }
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(nvs);
    }
}

/* Whatever the magic said, the mapping counts have to index the array
 * that holds them.
 *
 * Checked on every load rather than only while upgrading, because a
 * config that was corrupted once carries a current, valid magic
 * afterwards - so no upgrade runs on it again and nothing else looks. An
 * adapter in that state needed reflashing to recover, which is a lot to
 * ask for a number that can be checked in a dozen instructions.
 *
 * Returns true when something had to be corrected. */
static bool config_sanitise(struct config *data) {
    bool fixed = false;

    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        if (data->in_cfg[i].map_size > ADAPTER_MAPPING_MAX) {
            printf("%s: dev %lu map_size %u > %d, clamping\n",
                __FUNCTION__, i, data->in_cfg[i].map_size,
                ADAPTER_MAPPING_MAX);
            data->in_cfg[i].map_size = ADAPTER_MAPPING_MAX;
            fixed = true;
        }
    }

    return fixed;
}

static int32_t config_load_from_file(struct config *data, char *filename) {
#ifdef CONFIG_BLUERETRO_QEMU
    config_init_struct(data);
    config_init_nvs_patch(data);
    return 0;
#else
    struct stat st;
    int32_t ret = -1;

    if (stat(filename, &st) != 0) {
        printf("%s: No config on FS. Creating...\n", __FUNCTION__);
        config_init_struct(data);
        config_init_nvs_patch(data);
        ret = config_store_on_file(data, filename);
    }
    else {
        FILE *file = fopen(filename, "rb");
        if (file == NULL) {
            printf("%s: failed to open file for reading\n", __FUNCTION__);
        }
        else {
            /* Byte granularity, onto a cleared buffer, so a file shorter
             * than the current struct leaves a deterministic tail rather
             * than whatever was in RAM. An older layout can legitimately
             * be shorter, and the upgrade path below is about to shuffle
             * the whole struct and store it. */
            size_t got;

            memset((void *)data, 0, sizeof(*data));
            got = fread((void *)data, 1, sizeof(*data), file);
            fclose(file);

            if (got == sizeof(*data)) {
                ret = 0;
            }
            else {
                printf("%s: short config, %u of %u bytes\n",
                    __FUNCTION__, (unsigned)got, (unsigned)sizeof(*data));
            }
        }
    }

    if (data->magic != CONFIG_MAGIC) {
        int32_t file_ver = config_get_version(data->magic);
        if (file_ver == -1) {
            printf("%s: Bad magic, reset config\n", __FUNCTION__);
            config_init_struct(data);
            ret = config_store_on_file(data, filename);
        }
        else {
            printf("%s: Upgrading cfg v%ld to v%d\n", __FUNCTION__, file_ver, CONFIG_VERSION);
            for (uint32_t i = file_ver; i < CONFIG_VERSION; i++) {
                if (config_ver_update[i]) {
                    ret = config_ver_update[i](data, filename);
                }
            }
        }
    }

    /* Written back when it had to change, so the repair is done once
     * rather than on every boot for the life of the adapter. */
    if (config_sanitise(data)) {
        config_store_on_file(data, filename);
    }

    return ret;
#endif /* CONFIG_BLUERETRO_QEMU */
}

static int32_t config_store_on_file(struct config *data, char *filename) {
    int32_t ret = -1;

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        printf("%s: failed to open file for writing\n", __FUNCTION__);
    }
    else {
        fwrite((void *)data, sizeof(*data), 1, file);
        fclose(file);
        ret = 0;
    }
    return ret;
}

static bool config_is_rst_required(void) {
    static uint32_t magic = 0;
    static uint8_t multitap_cfg = 0;
    static uint8_t dev_mode[WIRED_MAX_DEV] = {0};
    bool ret = false;

    if (multitap_cfg != config.global_cfg.multitap_cfg) {
        ret = true;
    }
    multitap_cfg = config.global_cfg.multitap_cfg;

    for (uint32_t i = 0; i < WIRED_MAX_DEV; i++) {
        if (dev_mode[i] != config.out_cfg[i].dev_mode) {
            ret = true;
        }
        dev_mode[i] = config.out_cfg[i].dev_mode;
    }

    if (magic != config.magic) {
        ret = false;
    }
    magic = config.magic;

    return ret;
} 

void IRAM_ATTR config_set_rst_bare_core(bool value) {
    config_rst_bare_core = value;
}

void hw_config_patch(void) {
    esp_err_t err;
    nvs_handle_t nvs;

    err = nvs_open("hw", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        nvs_iterator_t it = NULL;
        err = nvs_entry_find_in_handle(nvs, NVS_TYPE_ANY, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            int32_t index = hw_config_lookup_key_name(info.key);
            if (index > -1) {
                uint32_t value;
                err = nvs_get_u32(nvs, info.key, &value);
                if (err == ESP_OK) {
                    hw_config.data32[index] = value;
                }
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(nvs);
    }
}

void config_init(uint32_t src) {
    char tmp_str[32] = "/fs/";
    char *filename = CONFIG_FILE;
    char *gameid = gid_get();
    config_src = DEFAULT_CFG;

    if (src == GAMEID_CFG && strlen(gameid)) {
        struct stat st;

        strcat(tmp_str, gameid);
        if (stat(tmp_str, &st) == 0) {
            filename = tmp_str;
            config_src = GAMEID_CFG;
        }
    }

    config_load_from_file(&config, filename);
    if (config_rst_bare_core && config_is_rst_required()) {
        sys_mgr_cmd(SYS_MGR_CMD_WIRED_RST);
        printf("# %s: Reloaded wired core cfg: %s\n", __FUNCTION__, filename);
    }
}

/* Everything a mapping can be stored in, short of a factory wipe.
 *
 * config.bin holds the settings and the profile that applies to every
 * game; the per controller and per game mappings are separate files. A
 * reset that skipped those would leave the exact thing somebody is trying
 * to get rid of, on whichever pad or game it was attached to.
 *
 * fs_reset() would do all of this and more, and the more is the problem:
 * it takes the link keys and the memory card image with it. */
void config_reset_defaults(void) {
    DIR *d;

    config_init_struct(&config);
    config_init_nvs_patch(&config);
    config_store_on_file(&config, CONFIG_FILE);

    d = opendir(ROOT);
    if (d) {
        struct dirent *dir;

        while ((dir = readdir(d)) != NULL) {
            char path[32] = ROOT "/";

            strncat(path, dir->d_name, sizeof(path) - strlen(path) - 1);

            /* The recent games list starts with the same letter as the
             * per game mappings and is not one of them. Losing it would
             * only cost the names on the profile menu, but there is no
             * reason to. */
            if (strcmp(path, GID_HIST_FILE) == 0) {
                continue;
            }

            if (strncmp(path, CTRL_MAP_FILE_PFX, strlen(CTRL_MAP_FILE_PFX)) == 0
                    || strncmp(path, CTRL_MAP_GAME_FILE_PFX,
                        strlen(CTRL_MAP_GAME_FILE_PFX)) == 0) {
                if (remove(path) == 0) {
                    printf("# %s: removed %s\n", __FUNCTION__, path);
                }
            }
        }
        closedir(d);
    }

    printf("%s: settings back to defaults\n", __FUNCTION__);
}

void config_update(uint32_t dst) {
    char tmp_str[32] = "/fs/";
    char *filename = CONFIG_FILE;
    char *gameid = gid_get();
    config_src = DEFAULT_CFG;

    if (dst == GAMEID_CFG && strlen(gameid)) {
        strcat(tmp_str, gameid);
        filename = tmp_str;
        config_src = GAMEID_CFG;
    }

    config_store_on_file(&config, filename);
    if (config_rst_bare_core && config_is_rst_required()) {
        sys_mgr_cmd(SYS_MGR_CMD_WIRED_RST);
        printf("# %s: Reloaded wired core cfg: %s\n", __FUNCTION__, filename);
    }
}

uint32_t config_get_src(void) {
    return config_src;
}

#ifdef CONFIG_BLUERETRO_CTRL_MAP
/* Per controller mappings live in their own small files keyed by address, the
 * same way per game configs key off the game id. Deliberately kept out of
 * struct config: adding a field there changes the layout the web config and the
 * config version machinery both depend on, for what is really a side table. */
#define CTRL_MAP_MAGIC 0x50414D42 /* BMAP */
#define CTRL_MAP_VER 1

struct ctrl_map_hdr {
    uint32_t magic;
    uint8_t version;
    uint8_t map_size;
    uint8_t reserved[2];
} __packed;

/* FNV-1a. Only needs to separate a handful of games from each other, and a
 * collision costs a wrong mapping that remapping fixes, so 32 bits is ample
 * and it keeps the name inside the SPIFFS limit. */
static uint32_t gid_hash(const char *str) {
    uint32_t hash = 2166136261u;

    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

/* A NULL or empty gameid names the profile that applies to every game. */
static void ctrl_map_filename(char *out, uint32_t len, const uint8_t *bdaddr,
        const char *gameid) {
    if (gameid && strlen(gameid)) {
        snprintf(out, len, "%s%08lX_%02X%02X%02X%02X%02X%02X",
            CTRL_MAP_GAME_FILE_PFX, (unsigned long)gid_hash(gameid),
            bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);
        return;
    }

    snprintf(out, len, "%s%02X%02X%02X%02X%02X%02X", CTRL_MAP_FILE_PFX,
        bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);
}

/* Which scope the map currently on a port came from, so the app can say so and
 * default the next save to the same place. */
static uint8_t ctrl_map_scope[WIRED_MAX_DEV] = {0};

uint32_t config_ctrl_map_is_game(uint32_t out_idx) {
    return (out_idx < WIRED_MAX_DEV) ? ctrl_map_scope[out_idx] : 0;
}

static int32_t config_load_ctrl_map_scope(uint32_t out_idx, const uint8_t *bdaddr,
        const char *gameid) {
    char filename[32];
    struct ctrl_map_hdr hdr;
    FILE *file;

    if (out_idx >= WIRED_MAX_DEV || bdaddr == NULL) {
        return -1;
    }

    ctrl_map_filename(filename, sizeof(filename), bdaddr, gameid);
    file = fopen(filename, "rb");
    if (file == NULL) {
        /* Never mapped, so whatever the port already has stands. */
        return -1;
    }

    if (fread(&hdr, sizeof(hdr), 1, file) != 1 || hdr.magic != CTRL_MAP_MAGIC
            || hdr.version != CTRL_MAP_VER
            || hdr.map_size == 0 || hdr.map_size > ADAPTER_MAPPING_MAX) {
        fclose(file);
        printf("# %s: %s unusable, ignoring\n", __FUNCTION__, filename);
        return -1;
    }

    if (fread(config.in_cfg[out_idx].map_cfg, sizeof(struct map_cfg),
            hdr.map_size, file) != hdr.map_size) {
        fclose(file);
        printf("# %s: %s truncated, ignoring\n", __FUNCTION__, filename);
        return -1;
    }
    fclose(file);

    config.in_cfg[out_idx].map_size = hdr.map_size;

    /* A stored map carries the dst_id of whatever port it was made on, so
     * retarget it or the controller ends up driving a different port. */
    for (uint32_t i = 0; i < hdr.map_size; i++) {
        config.in_cfg[out_idx].map_cfg[i].dst_id = out_idx;
    }

    ctrl_map_scope[out_idx] = (gameid && strlen(gameid)) ? 1 : 0;

    printf("# %s: %s -> port %lu, %u entries\n", __FUNCTION__, filename,
        out_idx, hdr.map_size);
    return 0;
}

/* A profile made for the game that is running beats the one made for the pad in
 * general, so try that first and fall back. Nothing here writes the fallback,
 * so a game specific profile never shadows the general one permanently. */
int32_t config_load_ctrl_map(uint32_t out_idx, const uint8_t *bdaddr) {
    if (config_load_ctrl_map_scope(out_idx, bdaddr, gid_get()) == 0) {
        return 0;
    }
    return config_load_ctrl_map_scope(out_idx, bdaddr, NULL);
}

int32_t config_save_ctrl_map(uint32_t out_idx, const uint8_t *bdaddr,
        const char *gameid) {
    char filename[32];
    struct ctrl_map_hdr hdr = {
        .magic = CTRL_MAP_MAGIC,
        .version = CTRL_MAP_VER,
        .reserved = {0, 0},
    };
    FILE *file;
    uint32_t cnt;

    if (out_idx >= WIRED_MAX_DEV || bdaddr == NULL) {
        return -1;
    }

    cnt = config.in_cfg[out_idx].map_size;
    if (cnt == 0 || cnt > ADAPTER_MAPPING_MAX) {
        return -1;
    }
    hdr.map_size = (uint8_t)cnt;

    ctrl_map_filename(filename, sizeof(filename), bdaddr, gameid);
    file = fopen(filename, "wb");
    if (file == NULL) {
        printf("# %s: failed to open %s for writing\n", __FUNCTION__, filename);
        return -1;
    }

    if (fwrite(&hdr, sizeof(hdr), 1, file) != 1
            || fwrite(config.in_cfg[out_idx].map_cfg, sizeof(struct map_cfg),
                cnt, file) != cnt) {
        fclose(file);
        printf("# %s: short write to %s\n", __FUNCTION__, filename);
        return -1;
    }
    fclose(file);

    ctrl_map_scope[out_idx] = (gameid && strlen(gameid)) ? 1 : 0;

    printf("# %s: %s saved, %lu entries\n", __FUNCTION__, filename, cnt);
    return 0;
}
#endif /* CONFIG_BLUERETRO_CTRL_MAP */

void config_debug_log(void) {
        bt_mon_log(true,
            "Global config: system: 0x%02X multitap: 0x%02X inquiry: 0x%02X banksel: 0x%02X",
            config.global_cfg.system_cfg, config.global_cfg.multitap_cfg,
            config.global_cfg.inquiry_mode, config.global_cfg.banksel);
        bt_mon_log(true, "Output config #0: device_mode: 0x%02X acc_mode: 0x%02X",
            config.out_cfg[0].dev_mode, config.out_cfg[0].acc_mode);
        bt_mon_log(true, "Mapping config #0");
        bt_mon_tx(BT_MON_SYS_NOTE, (uint8_t *)config.in_cfg[0].map_cfg,
            config.in_cfg[0].map_size * sizeof(config.in_cfg[0].map_cfg[0]));
}
