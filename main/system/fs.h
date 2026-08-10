/*
 * Copyright (c) 2019-2020, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _FS_H_
#define _FS_H_

#define ROOT "/fs"
#define LINK_KEYS_FILE "/fs/linkkeys.bin"
#define LE_LINK_KEYS_FILE "/fs/le_linkkeys.bin"
#define BDADDR_FILE "/fs/bdaddr.bin"
#define CONFIG_FILE "/fs/config.bin"
#define MEMORY_CARD_FILE "/fs/mc.bin"
#define PWR_STATE_FILE "/fs/pwr_state.bin"
#define BITSTREAM_FILE "/fs/bitstream.bit"
/* Per controller mappings are this prefix plus the address in hex. */
#define CTRL_MAP_FILE_PFX "/fs/m_"

/* Per game variant of the above. The game id is hashed rather than spelled
 * out: ids run to 22 characters and SPIFFS names are capped at 31, which a
 * bdaddr has already claimed half of. */
#define CTRL_MAP_GAME_FILE_PFX "/fs/g"

/* Recently launched games. Kept on the filesystem because the adapter is
 * powered by the console, and power cycling to get back to the loader is the
 * normal way to do it, which would otherwise wipe the list every time, right
 * before it was wanted. */
#define GID_HIST_FILE "/fs/gidhist"

int32_t fs_init(void);
void fs_reset(void);

#endif /* _FS_H_ */
