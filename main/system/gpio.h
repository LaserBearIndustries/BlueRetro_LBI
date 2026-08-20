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

#ifndef _GPIO_H_
#define _GPIO_H_

#include "soc/gpio_sig_map.h"
#include "soc/interrupts.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_struct.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"

/* The ESP32 gave each core its own GPIO interrupt status register, and the
 * wired drivers read the APP CPU's pair at ISR entry -- what upstream spelled
 * as the acpu_int register for GPIO0-31, and acpu_int1 for GPIO32-39.
 *
 * The S31 has no per-core status. It has one status register per GPIO
 * interrupt line instead -- GPIO_INT_0 through GPIO_INT_3 -- each with the
 * same two-bank shape: intr_0 covers GPIO0-31 and intr_01 covers GPIO32-63.
 * So the substitution is structural rather than semantic: the drivers want
 * "what is pending for the line my handler is on", which used to be spelled
 * "what is pending for my core".
 *
 * The peripheral interrupt source and the status register are two halves of
 * the same choice -- ETS_GPIO_INTR0_SOURCE is the line whose pending bits show
 * up in intr_0 -- so they are defined together here. Changing the line means
 * changing all three, and nothing outside this header should name either half.
 *
 * INT_0 is provisional. Which line the wired driver should own depends on the
 * interrupt routing that comes with core 1 bring-up, which is not ported yet.
 */
#define BR_GPIO_INTR_SOURCE ETS_GPIO_INTR0_SOURCE
#define gpio_intr_status()  (GPIO.intr_0.val)
#define gpio_intr_status1() (GPIO.intr_01.val)

extern const uint32_t GPIO_PIN_MUX_REG_IRAM[];

int32_t gpio_set_level_iram(gpio_num_t gpio_num, uint32_t level);
int32_t gpio_set_pull_mode_iram(gpio_num_t gpio_num, gpio_pull_mode_t pull);
int32_t gpio_set_direction_iram(gpio_num_t gpio_num, gpio_mode_t mode);
int32_t gpio_config_iram(const gpio_config_t *pGPIOConfig);
int32_t gpio_reset_iram(gpio_num_t gpio_num);

#endif /* _GPIO_H_ */
