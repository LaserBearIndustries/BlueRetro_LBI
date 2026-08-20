/*
 * Copyright (c) 2021, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "delay.h"

void delay_us(uint32_t delay_us) {
    uint32_t start, cur, timeout;
    start = esp_cpu_get_cycle_count();
    timeout = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * delay_us;
    do {
        cur = esp_cpu_get_cycle_count();
    } while (cur - start < timeout);
}
