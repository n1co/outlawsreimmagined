/*
 * obt.c - Outlaws OBT object placement parser
 */
#include "obt.h"

typedef struct { const char *p, *end; } Tp;

static void tp_skip(Tp *t) {
    while (t->p < t->end) {
        if (*t->p == '#') { while (t->p < t->end && *t->p != '\n') t->p++; continue; }
        if ((unsigned char)*t->p <= ' ') { t->p++; continue; }
        break;
    }
}

static int tp_tok(Tp *t, char *buf, int sz) {
    tp_skip(t);
    int n = 0;
    while (t->p < t->end && n < sz-1) {
        unsigned char c = (unsigned char)*t->p;
        if (c <= ' ') break;
        buf[n++] = *t->p++;
    }
    buf[n] = '\0'; return n;
}

static f32  tp_float(Tp *t) { char b[32]; tp_tok(t,b,sizeof(b)); return strtof(b,NULL); }
static u32  tp_hex(Tp *t)   { char b[32]; tp_tok(t,b,sizeof(b)); return (u32)strtoul(b,NULL,16); }
static u32  tp_uint(Tp *t)  { char b[32]; tp_tok(t,b,sizeof(b)); return (u32)strtoul(b,NULL,10); }

bool obt_parse(ObtTable *table, const char *text, u32 text_len) {
    memset(table, 0, sizeof(*table));
    Tp t = { text, text + text_len };
    char tok[128];

    /* Header: "OBT 1.0" */
    tp_tok(&t, tok, sizeof(tok));
    if (strcasecmp(tok, "OBT") != 0) return false;
    tp_tok(&t, tok, sizeof(tok)); /* version */

    /* Pre-scan to count objects */
    u32 count = 0;
    const char *scan = text;
    while ((scan = strstr(scan, "OBJECTS")) != NULL) {
        scan += 7;
        while (*scan == ' ' || *scan == '\t') scan++;
        count = (u32)strtoul(scan, NULL, 10);
        break;
    }
    if (count == 0) count = OBT_MAX_OBJECTS;
    if (count > OBT_MAX_OBJECTS) count = OBT_MAX_OBJECTS;

    table->objects = calloc(count, sizeof(ObtObject));
    if (!table->objects) return false;

    while (t.p < t.end) {
        tp_tok(&t, tok, sizeof(tok));
        if (!tok[0]) break;

        if (strcasecmp(tok, "LEVELNAME") == 0) {
            tp_tok(&t, table->level_name, sizeof(table->level_name));
        } else if (strcasecmp(tok, "OBJECTS") == 0) {
            tp_uint(&t); /* skip count - already read above */
        } else if (strcasecmp(tok, "NAME:") == 0) {
            if (table->object_count >= count) break;
            ObtObject *obj = &table->objects[table->object_count++];

            /* NAME: <type> */
            tp_tok(&t, obj->type, OBT_NAME_LEN);

            /* ID: <hex> */
            tp_tok(&t, tok, sizeof(tok)); /* "ID:" */
            obj->id = tp_hex(&t);

            /* SECTOR: <hex> */
            tp_tok(&t, tok, sizeof(tok)); /* "SECTOR:" */
            obj->sector_id = tp_hex(&t);

            /* X: <f> Y: <f> Z: <f> */
            tp_tok(&t, tok, sizeof(tok)); /* "X:" */
            obj->pos.x = tp_float(&t);
            tp_tok(&t, tok, sizeof(tok)); /* "Y:" */
            obj->pos.y = tp_float(&t);
            tp_tok(&t, tok, sizeof(tok)); /* "Z:" */
            obj->pos.z = tp_float(&t);

            /* PCH: <f> YAW: <f> ROL: <f> */
            tp_tok(&t, tok, sizeof(tok)); /* "PCH:" */
            obj->pitch = tp_float(&t);
            tp_tok(&t, tok, sizeof(tok)); /* "YAW:" */
            obj->yaw = tp_float(&t);
            tp_tok(&t, tok, sizeof(tok)); /* "ROL:" */
            obj->roll = tp_float(&t);

            /* FLAGS: <u> <u> */
            tp_tok(&t, tok, sizeof(tok)); /* "FLAGS:" */
            obj->flags[0] = tp_uint(&t);
            obj->flags[1] = tp_uint(&t);
        }
        /* else: skip */
    }

    OL_LOG("OBT '%s': %u objects\n", table->level_name, table->object_count);
    return true;
}

void obt_free(ObtTable *table) {
    if (table->objects) { free(table->objects); table->objects = NULL; }
    table->object_count = 0;
}

u32 obt_find_by_type(const ObtTable *table, const char *type,
                     ObtObject **results, u32 max_results) {
    u32 found = 0;
    for (u32 i = 0; i < table->object_count && found < max_results; i++) {
        if (strcasecmp(table->objects[i].type, type) == 0)
            results[found++] = &table->objects[i];
    }
    return found;
}
