/*
 * msg.h - Outlaws LOCAL.MSG string table
 *
 * LOCAL.MSG maps numeric message IDs to display text. INF USER_MSG triggers
 * reference these IDs (e.g. USER_MSG 801 → "You need the brass key."). Format:
 *
 *   # comment
 *   801  5:  "You need the brass key."
 *   <id> <priority>: "<text>"
 */
#pragma once

#include "engine.h"

#define MSG_MAX_ENTRIES 2048
#define MSG_TEXT_LEN    128

typedef struct {
    i32  ids[MSG_MAX_ENTRIES];
    char text[MSG_MAX_ENTRIES][MSG_TEXT_LEN];
    u32  count;
} MsgTable;

void        msg_init(MsgTable *t);
/* Parse a LOCAL.MSG text buffer into the table. Returns entries parsed. */
u32         msg_load(MsgTable *t, const char *data, u32 len);
/* Look up text by id. Returns NULL if not present. */
const char *msg_get(const MsgTable *t, i32 id);
