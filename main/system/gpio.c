// Copyright 2015-2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdbool.h>
#include "hal/cpu_ll.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "esp_bit_defs.h"
#include "hal/gpio_ll.h"
#include "gpio.h"

/* The ESP32 kept pull-up and pull-down for its RTC-capable pins in the RTC IO
 * block rather than the IO MUX, so an IRAM-safe pull had to know which pins
 * those were. That is what the three tables here used to be: a hand-copied RTC
 * channel map, RTC pad descriptors, and the per-pin IO MUX register addresses.
 *
 * The S31 puts every pull in the IO MUX, indexed straight by GPIO number --
 * gpio_ll_pullup_en() is one register write, IO_MUX.gpio[n].fun_wpu = 1 -- so
 * the split, the tables and the dispatch below them all go. gpio_ll is header
 * inline, so calling it stays IRAM-safe.
 *
 * Worth noting the old table was also a latent bug waiting for this port: it
 * had 40 entries, one per ESP32 pin, and the S31 has 60. Anything above 39
 * would have read a zero and written to address 0.
 */

static int32_t gpio_pullup_en_iram(gpio_num_t gpio_num)
{
    gpio_ll_pullup_en(&GPIO, gpio_num);
    return ESP_OK;
}

static int32_t gpio_pullup_dis_iram(gpio_num_t gpio_num)
{
    gpio_ll_pullup_dis(&GPIO, gpio_num);
    return ESP_OK;
}

static int32_t gpio_pulldown_en_iram(gpio_num_t gpio_num)
{
    gpio_ll_pulldown_en(&GPIO, gpio_num);
    return ESP_OK;
}

static int32_t gpio_pulldown_dis_iram(gpio_num_t gpio_num)
{
    gpio_ll_pulldown_dis(&GPIO, gpio_num);
    return ESP_OK;
}


int32_t gpio_set_level_iram(gpio_num_t gpio_num, uint32_t level)
{
    gpio_ll_set_level(&GPIO, gpio_num, level);
    return ESP_OK;
}

int32_t gpio_set_pull_mode_iram(gpio_num_t gpio_num, gpio_pull_mode_t pull)
{
    int32_t ret = ESP_OK;

    switch (pull) {
        case GPIO_PULLUP_ONLY:
            gpio_pulldown_dis_iram(gpio_num);
            gpio_pullup_en_iram(gpio_num);
            break;

        case GPIO_PULLDOWN_ONLY:
            gpio_pulldown_en_iram(gpio_num);
            gpio_pullup_dis_iram(gpio_num);
            break;

        case GPIO_PULLUP_PULLDOWN:
            gpio_pulldown_en_iram(gpio_num);
            gpio_pullup_en_iram(gpio_num);
            break;

        case GPIO_FLOATING:
            gpio_pulldown_dis_iram(gpio_num);
            gpio_pullup_dis_iram(gpio_num);
            break;

        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    return ret;
}

int32_t gpio_set_direction_iram(gpio_num_t gpio_num, gpio_mode_t mode)
{
    if (mode & GPIO_MODE_DEF_INPUT) {
        gpio_ll_input_enable(&GPIO, gpio_num);
    } else {
        gpio_ll_input_disable(&GPIO, gpio_num);
    }

    if (mode & GPIO_MODE_DEF_OUTPUT) {
        gpio_ll_output_enable(&GPIO, gpio_num);
    } else {
        gpio_ll_output_disable(&GPIO, gpio_num);
        gpio_ll_set_output_signal_matrix_source(&GPIO, gpio_num, SIG_GPIO_OUT_IDX, false);
    }

    if (mode & GPIO_MODE_DEF_OD) {
        gpio_ll_od_enable(&GPIO, gpio_num);
    } else {
        gpio_ll_od_disable(&GPIO, gpio_num);
    }

    return ESP_OK;
}

int32_t gpio_config_iram(const gpio_config_t *pGPIOConfig)
{
    uint64_t gpio_pin_mask = (pGPIOConfig->pin_bit_mask);
    uint32_t io_num = 0;
    uint8_t input_en = 0;
    uint8_t output_en = 0;
    uint8_t od_en = 0;
    uint8_t pu_en = 0;
    uint8_t pd_en = 0;

    if (pGPIOConfig->pin_bit_mask == 0 ||
        pGPIOConfig->pin_bit_mask & ~SOC_GPIO_VALID_GPIO_MASK) {
        esp_rom_printf("# GPIO_PIN mask error\n");
        return ESP_ERR_INVALID_ARG;
    }

    if (pGPIOConfig->mode & GPIO_MODE_DEF_OUTPUT &&
        pGPIOConfig->pin_bit_mask & ~SOC_GPIO_VALID_OUTPUT_GPIO_MASK) {
        esp_rom_printf("# GPIO can only be used as input mode\n");
        return ESP_ERR_INVALID_ARG;
    }

    do {
        if (((gpio_pin_mask >> io_num) & BIT(0))) {
            if ((pGPIOConfig->mode) & GPIO_MODE_DEF_INPUT) {
                input_en = 1;
                gpio_ll_input_enable(&GPIO, io_num);
            } else {
                gpio_ll_input_disable(&GPIO, io_num);
            }

            if ((pGPIOConfig->mode) & GPIO_MODE_DEF_OD) {
                od_en = 1;
                gpio_ll_od_enable(&GPIO, io_num);
            } else {
                gpio_ll_od_disable(&GPIO, io_num);
            }

            if ((pGPIOConfig->mode) & GPIO_MODE_DEF_OUTPUT) {
                output_en = 1;
                gpio_ll_output_enable(&GPIO, io_num);
            } else {
                gpio_ll_output_disable(&GPIO, io_num);
                gpio_ll_set_output_signal_matrix_source(&GPIO, io_num, SIG_GPIO_OUT_IDX, false);
            }

            if (pGPIOConfig->pull_up_en) {
                pu_en = 1;
                gpio_pullup_en_iram(io_num);
            } else {
                gpio_pullup_dis_iram(io_num);
            }

            if (pGPIOConfig->pull_down_en) {
                pd_en = 1;
                gpio_pulldown_en_iram(io_num);
            } else {
                gpio_pulldown_dis_iram(io_num);
            }

            esp_rom_printf("# GPIO[%d]| InputEn: %d| OutputEn: %d| OpenDrain: %d| Pullup: %d| Pulldown: %d| Intr:%d\n",
                io_num, input_en, output_en, od_en, pu_en, pd_en, pGPIOConfig->intr_type);
            gpio_ll_set_intr_type(&GPIO, io_num, pGPIOConfig->intr_type);

            if (pGPIOConfig->intr_type) {
                if (io_num < 32) {
                    gpio_ll_clear_intr_status(&GPIO, BIT(io_num));
                } else {
                    gpio_ll_clear_intr_status_high(&GPIO, BIT(io_num - 32));
                }
                /* Always set interrupt to core 1 */
                gpio_ll_intr_enable_on_core(&GPIO, 1, io_num);
            } else {
                gpio_ll_intr_disable(&GPIO, io_num);
                if (io_num < 32) {
                    gpio_ll_clear_intr_status(&GPIO, BIT(io_num));
                } else {
                    gpio_ll_clear_intr_status_high(&GPIO, BIT(io_num - 32));
                }
            }

            gpio_ll_func_sel(&GPIO, io_num, PIN_FUNC_GPIO);
        }

        io_num++;
    } while (io_num < GPIO_PIN_COUNT);

    return ESP_OK;
}

int32_t gpio_reset_iram(gpio_num_t gpio_num) {
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(gpio_num),
        .mode = GPIO_MODE_DISABLE,
        //for powersave reasons, the GPIO should not be floating, select pullup
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config_iram(&cfg);
}
