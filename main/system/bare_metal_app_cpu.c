/*
 * MIT License
 *
 * Copyright (c) 2020 Daniel Frejek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Runs the wired driver on core 1 outside FreeRTOS, so a cycle-counted bit-bang
 * is never preempted. The original was written for the ESP32 and did not
 * survive the move to the S31 in any recognisable form -- almost none of what
 * it did exists on this part:
 *
 *   - it wrote the Xtensa TLB with wdtlb/witlb and set MEMCTL for the hardware
 *     loop erratum. RISC-V has neither; region protection is PMP and ESP-IDF
 *     sets it up itself.
 *   - it drove core 1 through DPORT_APPCPU_CTRL_A/B/C/D. There is no DPORT
 *     here; IDF exposes cpu_utility_ll_* and esp_cpu_unstall() instead.
 *   - it loaded the stack pointer with "l32i a1", and reserved a window of
 *     ESP32 ROM data at 0x3ffe3f20 so ROM printf had somewhere to scribble.
 *
 * The other thing that went is the reason the original was shaped so oddly. It
 * started core 1 early, let it warm up, then clock-gated it off, and only
 * un-gated it once a stack had been malloc'd -- because, in the author's words,
 * the APP CPU "will wreak havoc on the heap when it starts", even given a
 * trivial loop. That is why the call had to be threaded into cpu_start.c ahead
 * of heap_caps_init() in the first place.
 *
 * Giving core 1 a static stack removes the problem rather than working around
 * it: nothing is allocated, so there is no heap to corrupt and no ordering
 * constraint to respect. Core 1 comes up, parks on a flag, and starts the
 * driver when core 0 hands it one.
 *
 * NOT YET RUN ON HARDWARE. This compiles and follows what IDF's own
 * call_start_cpu1() does on RISC-V, but bringing a core up outside the RTOS is
 * exactly the kind of thing that needs a scope and a JTAG probe to believe.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_cpu.h>
#include <esp_attr.h>
#include "hal/cpu_utility_ll.h"
#include "rom/ets_sys.h"
#include "bare_metal_app_cpu.h"

typedef void (*wired_init_t)(void);

/* RISC-V frames are larger than the Xtensa ones this used to size for. */
#ifndef APP_CPU_STACK_SIZE
#define APP_CPU_STACK_SIZE 2048
#endif

/* Interrupt vectors, from the SDK. _mtvt_table is the CLIC vectored table. */
extern int _vector_table;
extern int _mtvt_table;

/* Not static: the entry stub below loads sp from it before any C runs, and
 * needs a symbol the assembler can name. The RISC-V ABI wants sp 16-byte
 * aligned. */
uint8_t app_cpu_stack[APP_CPU_STACK_SIZE] __attribute__((aligned(16)));

static volatile wired_init_t app_cpu_user = NULL;
static volatile uint8_t app_cpu_initial_start = 0;

/* IRAM_ATTR goes on the definition only -- it expands to a counter-numbered
 * section, so repeating it here would name a different one. */
void app_cpu_main(void);

/*
 * Where core 1 lands out of reset. Naked because there is no stack yet, so the
 * compiler must not emit a prologue.
 *
 * gp has to be set before anything else: the linker relaxes global accesses to
 * be gp-relative, so any C touching a global before this point reads rubbish.
 */
static void IRAM_ATTR __attribute__((naked, used)) app_cpu_entry(void)
{
    __asm__ __volatile__(
        ".option push\n"
        ".option norelax\n"
        "   la    gp, __global_pointer$\n"
        ".option pop\n"
        "   la    sp, app_cpu_stack\n"
        "   li    t0, %0\n"
        "   add   sp, sp, t0\n"
        "   andi  sp, sp, -16\n"
        "   j     app_cpu_main\n"
        :: "i" (APP_CPU_STACK_SIZE)
    );
}

void IRAM_ATTR app_cpu_main(void)
{
    esp_cpu_intr_set_ivt_addr(&_vector_table);
#if SOC_INT_CLIC_SUPPORTED
    /* With CLIC hardware vectoring the core jumps to mtvt + 4 * interrupt_id. */
    esp_cpu_intr_set_mtvt_addr(&_mtvt_table);
#endif
#if SOC_CPU_SUPPORT_WFE
    esp_cpu_disable_wfe_mode();
#endif
#if SOC_BRANCH_PREDICTOR_SUPPORTED
    /* Left off deliberately. This core exists to hit sub-microsecond edges on
     * a 2 Mbps bus by counting instructions, and a predictor makes the cost of
     * a loop depend on history rather than on the code. Throughput is worth
     * nothing here; a repeatable cycle count is worth everything. The ESP32
     * had no predictor, so the timing this driver inherited assumes there is
     * none. Revisit only with a scope on the pins. */
    esp_cpu_branch_prediction_disable();
#endif

    app_cpu_initial_start = 1;

    /* Park until core 0 has a driver for us. No RTOS on this core, so this is
     * a plain spin -- it is a dedicated core and has nothing else to do. */
    while (app_cpu_user == NULL) {
    }

    app_cpu_user();

    while (1) {
    }
}

int32_t start_app_cpu(wired_init_t user)
{
    if (!app_cpu_initial_start) {
        printf("# %s: APP CPU was not initialized!\n", __FUNCTION__);
        return -1;
    }

    if (app_cpu_user) {
        printf("# %s: APP CPU is already running!\n", __FUNCTION__);
        return -1;
    }

    app_cpu_user = user;
    return 0;
}

/*
 * Called from start_cpu0_default() in our copy of startup.c. It no longer has
 * to run before heap_caps_init() -- the stack is static -- but it is left there
 * so the wired side is up as early as possible.
 */
void init_app_cpu_baremetal(void)
{
    app_cpu_initial_start = 0;
    app_cpu_user = NULL;

    ets_set_appcpu_boot_addr((uint32_t)&app_cpu_entry);

    esp_cpu_unstall(1);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();

    while (!app_cpu_initial_start) {
    }
}
