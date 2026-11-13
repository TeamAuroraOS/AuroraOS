#ifndef AURORA_BANNER_H
#define AURORA_BANNER_H

#include "aurora.h"

#define BANNER_ARENA_ADDR 0x24000000u
#define BANNER_ARENA_SIZE 0x00800000u
#define BANNER_FILE_MAX   0x00100000u

#define BANNER_NONE     0u
#define BANNER_BESIDE   1u
#define BANNER_DATA     2u
#define BANNER_EMBEDDED 0x80000000u

u32 banner_find(const char *app_path, u16 *icon, u8 *anim, int *has_icon);

int banner_load(const char *app_path, u32 where);
int banner_parse(const u8 *data, u32 size);
void banner_unload(void);
int banner_ready(void);
const char *banner_error(void);

int banner_draw(u32 ms, u32 fade, u32 dst);

int banner_jingle(u32 *addr, u32 *samples, u32 *rate);

int banner_node_matrix(int i, u32 ms, s32 out[12]);
u32 banner_node_count(void);

#endif
