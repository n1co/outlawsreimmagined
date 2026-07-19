/*
 * msg.c - Outlaws LOCAL.MSG string table parser
 */
#include "msg.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void msg_init(MsgTable *t) {
    if (t) t->count = 0;
}

u32 msg_load(MsgTable *t, const char *data, u32 len) {
    if (!t || !data) return 0;
    t->count = 0;

    const char *p = data, *end = data + len;
    while (p < end && t->count < MSG_MAX_ENTRIES) {
        /* Start of a line. */
        const char *ls = p;
        const char *le = ls;
        while (le < end && *le != '\n') le++;
        p = (le < end) ? le + 1 : le;   /* advance to next line */

        /* Trim leading whitespace. */
        const char *q = ls;
        while (q < le && isspace((unsigned char)*q)) q++;
        if (q >= le) continue;
        if (*q == '#' || *q == ';') continue;   /* comment */
        if (!isdigit((unsigned char)*q)) continue;

        /* Parse the id (leading integer). */
        i32 id = (i32)strtol(q, NULL, 10);
        while (q < le && isdigit((unsigned char)*q)) q++;

        /* Find the first double-quote — text is between the quotes. */
        const char *qs = NULL;
        for (const char *r = q; r < le; r++) {
            if (*r == '"') { qs = r + 1; break; }
        }
        if (!qs) continue;
        const char *qe = qs;
        while (qe < le && *qe != '"') qe++;
        if (qe >= le) continue;

        u32 n = (u32)(qe - qs);
        if (n >= MSG_TEXT_LEN) n = MSG_TEXT_LEN - 1;
        memcpy(t->text[t->count], qs, n);
        t->text[t->count][n] = '\0';
        t->ids[t->count] = id;
        t->count++;
    }
    return t->count;
}

const char *msg_get(const MsgTable *t, i32 id) {
    if (!t) return NULL;
    for (u32 i = 0; i < t->count; i++)
        if (t->ids[i] == id) return t->text[i];
    return NULL;
}
