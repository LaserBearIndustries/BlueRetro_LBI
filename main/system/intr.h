/*
 * Copyright (c) 2021, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _INTR_H_
#define _INTR_H_

#include "soc/soc.h"
#include "esp_attr.h"
#include "riscv/interrupt.h"

/* Upstream took an XT_INTEXC_HOOK, which is a bare Xtensa hook returning
 * unsigned and taking the INTERRUPT register as its argument. The RISC-V
 * equivalent is intr_handler_t -- void (*)(void *) -- so the handler now
 * receives whatever was registered alongside it rather than a pending mask.
 *
 * The drivers that shared one handler across several sources used that mask to
 * work out which had fired. On the S31 each source is routed to its own CLIC
 * line and dispatched separately, so they register the same function once per
 * source and pass the bit they used to test for as arg.
 */
int32_t intexc_alloc_iram(uint32_t source, uint32_t intr_num, intr_handler_t handler, void *arg);
int32_t intexc_free_iram(uint32_t source, uint32_t intr_num);

#endif /* _INTR_H_ */
