/*
 * Copyright (c) 2019-2022, Jacques Gagnon
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _GAMEID_H_
#define _GAMEID_H_

#include "adapter/adapter.h"

/* Recently seen game ids, newest first.
 *
 * A game id only turns up when the loader launches something, and launching the
 * companion app is itself a launch, so by the time anyone is in a position to
 * assign a mapping, the current id is the app's own. The game they mean is the
 * one before it. Keeping a few answers that without shipping a database of
 * every title ever pressed. */
#define GID_HIST_MAX 4

int32_t gid_update(struct raw_fb *fb_data);
int32_t gid_update_sys(struct raw_fb *fb_data);
char *gid_get(void);
void gid_hist_push(const char *gameid);
const char *gid_hist_get(uint32_t idx);
uint32_t gid_hist_cnt(void);

#endif /* _GAMEID_H_ */
