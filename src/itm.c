/*
 * itm.c - Outlaws ITM item-definition parser
 */
#include "itm.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Advance past spaces/tabs (not newlines). */
static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
    return p;
}

/* Read one whitespace-delimited token into buf. Returns pointer after it. */
static const char *read_tok(const char *p, const char *end,
                            char *buf, u32 buflen) {
    p = skip_ws(p, end);
    u32 n = 0;
    while (p < end && *p && !isspace((unsigned char)*p)) {
        if (n + 1 < buflen) buf[n++] = *p;
        p++;
    }
    buf[n] = '\0';
    return p;
}

static const char *next_line(const char *p, const char *end) {
    while (p < end && *p != '\n') p++;
    return (p < end) ? p + 1 : end;
}

bool itm_parse(ItmFile *itm, const char *text, u32 len) {
    memset(itm, 0, sizeof(*itm));
    const char *p = text, *end = text + len;
    char tok[ITM_STR_LEN];

    bool in_data = false;
    while (p < end) {
        const char *line_start = p;
        p = skip_ws(p, end);
        if (p >= end) break;
        if (*p == '\n' || *p == '#') { p = next_line(line_start, end); continue; }

        const char *q = read_tok(p, end, tok, sizeof(tok));

        if (!in_data) {
            if (strcasecmp(tok, "NAME") == 0) {
                read_tok(q, end, itm->name, sizeof(itm->name));
            } else if (strcasecmp(tok, "FUNC") == 0) {
                read_tok(q, end, itm->func, sizeof(itm->func));
            } else if (strcasecmp(tok, "ANIM") == 0) {
                read_tok(q, end, itm->anim, sizeof(itm->anim));
            } else if (strcasecmp(tok, "DATA") == 0) {
                in_data = true;   /* count token is informative only */
            }
            /* "ITEM 1.0" header and unknown lines: skipped */
        } else {
            /* DATA block: TYPE KEY VALUE per line */
            ItmFieldType t;
            if      (strcasecmp(tok, "STR")   == 0) t = ITM_STR;
            else if (strcasecmp(tok, "INT")   == 0) t = ITM_INT;
            else if (strcasecmp(tok, "FLOAT") == 0) t = ITM_FLOAT;
            else { p = next_line(line_start, end); continue; }

            if (itm->field_count >= ITM_MAX_FIELDS) break;
            ItmField *f = &itm->fields[itm->field_count];
            f->type = t;
            q = read_tok(q, end, f->key, sizeof(f->key));
            q = read_tok(q, end, f->sval, sizeof(f->sval));
            if (f->key[0]) {
                f->ival = (i32)strtol(f->sval, NULL, 10);
                f->fval = (f32)strtod(f->sval, NULL);
                itm->field_count++;
            }
        }
        p = next_line(line_start, end);
    }
    return itm->name[0] != '\0';
}

const ItmField *itm_find(const ItmFile *itm, const char *key) {
    for (u32 i = 0; i < itm->field_count; i++)
        if (strcasecmp(itm->fields[i].key, key) == 0) return &itm->fields[i];
    return NULL;
}

f32 itm_get_float(const ItmFile *itm, const char *key, f32 def) {
    const ItmField *f = itm_find(itm, key);
    return f ? f->fval : def;
}

i32 itm_get_int(const ItmFile *itm, const char *key, i32 def) {
    const ItmField *f = itm_find(itm, key);
    return f ? f->ival : def;
}

const char *itm_get_str(const ItmFile *itm, const char *key, const char *def) {
    const ItmField *f = itm_find(itm, key);
    return f ? f->sval : def;
}
