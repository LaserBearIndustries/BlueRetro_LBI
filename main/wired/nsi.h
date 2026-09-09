/*
 * Copyright (c) 2019-2023, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _NSI_H_
#define _NSI_H_

#include <stdint.h>

void nsi_init(uint32_t package);
void nsi_port_cfg(uint16_t mask);
/* Which ports have something behind them. Ports outside this mask stay
 * attached and answer the vendor opcodes, but do not answer as a
 * controller. See the note above gc_port_is_present(). */
void nsi_port_present(uint16_t mask);

#endif  /* _NSI_H_ */
