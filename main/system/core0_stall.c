// Copyright 2010-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <string.h>
#include "core0_stall.h"

/* On the ESP32 this handed core 0 an interrupt that spun it in a high-priority
 * Xtensa handler, so that a cycle-counted bit-bang running bare metal on core 1
 * could not be disturbed by the other core touching the bus. It was built out
 * of DPORT_CPU_INTR_FROM_CPU_2_REG, intr_matrix_set() and a level-4 assembly
 * vector in core0_stall_highint_hdl.S.
 *
 * None of those parts exist on the S31, and the strongest reason for the
 * mechanism does not either: the ESP32's DPORT/APB read arbitration erratum,
 * where a simultaneous DPORT access from both cores could corrupt a read, was
 * specific to that silicon.
 *
 * What does not automatically follow is that stalling core 0 was pointless.
 * The second effect -- keeping the other core off the bus so it cannot add
 * jitter to a timing loop -- is a plain contention argument and may still hold.
 * That is a question for a logic analyser, not for reasoning: if the Maple
 * waits turn out to jitter with core 0 busy, this comes back, and it comes back
 * as a RISC-V mechanism rather than a translation of the Xtensa one.
 *
 * Until then these are no-ops. The API stays because roughly two dozen call
 * sites across maple, sega_io, pce_io and jag_io bracket their timed sections
 * with it, and those brackets are exactly the documentation of where the
 * mechanism would need to go back.
 */

void core0_stall_start(void)
{
}

void core0_stall_end(void)
{
}

void core0_stall_init(void)
{
}
