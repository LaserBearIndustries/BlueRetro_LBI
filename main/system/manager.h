/*
 * Copyright (c) 2019-2023, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_MANAGER_H_
#define _SYS_MANAGER_H_ 

#include <stdint.h>

enum {
    SYS_MGR_CMD_RST = 0,
    SYS_MGR_CMD_PWR_ON,
    SYS_MGR_CMD_PWR_OFF,
    SYS_MGR_CMD_INQ_TOOGLE,
    SYS_MGR_CMD_FACTORY_RST,
    SYS_MGR_CMD_DEEP_SLEEP,
    SYS_MGR_CMD_ADAPTER_RST,
    SYS_MGR_CMD_WIRED_RST,
};

/* True when the port sense input says a wired controller is in that port.
 * Always false on hardware without the sense pins, where it is unknowable
 * rather than absent. */
uint32_t sys_mgr_port_is_wired(uint32_t port);

void sys_mgr_cmd(uint8_t cmd);
void sys_mgr_early_pwr_restore(void);
void sys_mgr_init(uint32_t package);

#endif /* _SYS_MANAGER_H_ */
