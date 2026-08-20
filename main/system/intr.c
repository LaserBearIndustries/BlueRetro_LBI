/*
 * Copyright (c) 2021, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <esp_rom_sys.h>
#include <esp_cpu.h>
#include "soc/soc_caps.h"
#include "riscv/interrupt.h"
#include "intr.h"

/* Upstream hand-rolled this against the Xtensa interrupt architecture: it
 * wrote INTENABLE with xsr to mask and unmask, installed the handler into
 * _xt_intexc_hooks[], and only accepted CPU interrupts 19, 20, 21 and 23 --
 * the medium-priority IRAM-safe slots the ESP32 happened to leave free, whose
 * Xtensa priority level the function had to derive from the number itself.
 *
 * None of that shape survives on the S31, and none of it needs to. The core is
 * RISC-V with a CLIC: 32 lines, a flat 1-7 priority scale set per line rather
 * than implied by which line you picked, and level or edge chosen explicitly.
 * So the number a driver passes is now simply a CLIC line number, and the
 * priority is ours to state.
 *
 * The priority below is provisional. What it ought to be depends on where the
 * bare-metal wired driver ends up sitting relative to the Bluetooth
 * controller's own interrupts, and that cannot be settled until core 1 comes
 * up outside FreeRTOS -- see bare_metal_app_cpu.c, which is still an ESP32
 * file. 3 is a mid-scale placeholder that leaves room either side.
 */
#define INTR_PRIORITY 3

int32_t intexc_alloc_iram(uint32_t source, uint32_t intr_num, intr_handler_t handler, void *arg) {
    uint32_t core_id = esp_cpu_get_core_id();

    if (intr_num == ETS_INVALID_INUM || intr_num >= SOC_CPU_INTR_NUM) {
        return -1;
    }

    intr_handler_set(intr_num, handler, arg);
    esprv_int_set_type(intr_num, INTR_TYPE_LEVEL);
    esprv_int_set_priority(intr_num, INTR_PRIORITY);
    esp_rom_route_intr_matrix(core_id, source, intr_num);
    esprv_int_enable(BIT(intr_num));

    return 0;
}

int32_t intexc_free_iram(uint32_t source, uint32_t intr_num) {
    uint32_t core_id = esp_cpu_get_core_id();

    if (intr_num == ETS_INVALID_INUM || intr_num >= SOC_CPU_INTR_NUM) {
        return -1;
    }

    esprv_int_disable(BIT(intr_num));
    esp_rom_route_intr_matrix(core_id, source, ETS_INVALID_INUM);
    intr_handler_set(intr_num, NULL, NULL);

    return 0;
}
