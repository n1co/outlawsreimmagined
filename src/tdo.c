/*
 * tdo.c - Outlaws 3DO object format parser
 */
#include "tdo.h"
#include <ctype.h>

/* Minimal text parser (reuse same pattern as lvt.c) */
typedef struct { const char *p, *end; } Tp;

static void tp_skip(Tp *t) {
    while (t->p < t->end) {
        if (*t->p == '#') { while (t->p < t->end && *t->p != '\n') t->p++; continue; }
        if (*t->p <= ' ') { t->p++; continue; }
        break;
    }
}

static int tp_tok(Tp *t, char *buf, int sz) {
    tp_skip(t);
    int n = 0;
    while (t->p < t->end && n < sz-1) {
        char c = *t->p;
        if (c == '#' || c <= ' ') break;
        /* Handle quoted strings. Real files have unterminated quotes
         * (`OBJECT "DEFAULT`) — stop at end of line too. */
        if (c == '"') {
            t->p++;
            while (t->p < t->end && *t->p != '"' && *t->p != '\n' && n < sz-1)
                buf[n++] = *t->p++;
            if (t->p < t->end && *t->p == '"') t->p++;
            break;
        }
        buf[n++] = c; t->p++;
    }
    buf[n] = '\0';
    return n;
}

static int tp_int(Tp *t)   { char b[32]; tp_tok(t,b,sizeof(b)); return (int)strtol(b,NULL,10); }
static f32 tp_float(Tp *t) { char b[32]; tp_tok(t,b,sizeof(b)); return strtof(b,NULL); }

bool tdo_parse(TdoModel *model, const char *text, u32 text_len) {
    memset(model, 0, sizeof(*model));
    Tp t = { text, text + text_len };
    char tok[128];

    /* Header: "3DO 2.0" */
    tp_tok(&t, tok, sizeof(tok));
    if (strcasecmp(tok, "3DO") != 0) return false;
    tp_tok(&t, tok, sizeof(tok)); /* version */

    while (t.p < t.end) {
        tp_tok(&t, tok, sizeof(tok));
        if (!tok[0]) break;

        if (strcasecmp(tok, "3DONAME") == 0) {
            tp_tok(&t, model->name, TDO_MAX_NAME);
        } else if (strcasecmp(tok, "PALETTE") == 0) {
            tp_tok(&t, model->palette, TDO_MAX_NAME);
        } else if (strcasecmp(tok, "OBJECTS") == 0 ||
                   strcasecmp(tok, "VERTICES") == 0 ||
                   strcasecmp(tok, "POLYGONS") == 0) {
            tp_int(&t); /* skip totals */
        } else if (strcasecmp(tok, "TEXTURES") == 0) {
            int n = tp_int(&t);
            for (int i = 0; i < n && model->texture_count < TDO_MAX_TEXTURES; i++) {
                tp_tok(&t, tok, sizeof(tok)); /* "TEXTURE:" */
                tp_tok(&t, model->textures[model->texture_count++], TDO_MAX_NAME);
                /* Skip "# index" comment */
                tp_skip(&t);
                if (t.p < t.end && *t.p == '#') { while (t.p < t.end && *t.p != '\n') t.p++; }
            }
        } else if (strcasecmp(tok, "OBJECT") == 0) {
            if (model->object_count >= TDO_MAX_OBJECTS) { tp_skip(&t); continue; }
            TdoObject *obj = &model->objects[model->object_count++];
            tp_tok(&t, obj->name, TDO_MAX_NAME);
            obj->texture_idx = -1;

            /* Parse object body */
            while (t.p < t.end) {
                const char *save = t.p;
                tp_tok(&t, tok, sizeof(tok));

                /* Next top-level keyword → end of this object */
                if (strcasecmp(tok, "OBJECT") == 0 ||
                    strcasecmp(tok, "3DONAME") == 0) { t.p = save; break; }

                if (strcasecmp(tok, "TEXTURE") == 0) {
                    /* Check for "TEXTURE VERTICES" / "TEXTURE TRIANGLES" */
                    const char *sp = t.p;
                    tp_tok(&t, tok, sizeof(tok));
                    if (strcasecmp(tok, "VERTICES") == 0) {
                        int n = tp_int(&t);
                        for (int i = 0; i < n && obj->tex_vert_count < TDO_MAX_VERTS; i++) {
                            tp_int(&t); /* "idx:" */
                            f32 u = tp_float(&t), v = tp_float(&t);
                            obj->tex_verts[obj->tex_vert_count++] = (Vec2){u, v};
                        }
                    } else if (strcasecmp(tok, "TRIANGLES") == 0) {
                        int n = tp_int(&t);
                        for (int i = 0; i < n && (u32)i < TDO_MAX_TRIS; i++) {
                            tp_int(&t); /* "idx:" */
                            int a = tp_int(&t), b = tp_int(&t), c = tp_int(&t);
                            /* Update existing triangle's tex indices */
                            if (i < (int)obj->tri_count) {
                                obj->tris[i].ta = a;
                                obj->tris[i].tb = b;
                                obj->tris[i].tc = c;
                            }
                        }
                    } else {
                        /* Single texture index */
                        t.p = sp;
                        obj->texture_idx = tp_int(&t);
                        /* Skip optional comment */
                        tp_skip(&t);
                        if (t.p < t.end && *t.p == '#') { while (t.p < t.end && *t.p != '\n') t.p++; }
                    }
                } else if (strcasecmp(tok, "VERTICES") == 0) {
                    int n = tp_int(&t);
                    for (int i = 0; i < n && obj->vert_count < TDO_MAX_VERTS; i++) {
                        tp_int(&t); /* "idx:" */
                        f32 x = tp_float(&t), y = tp_float(&t), z = tp_float(&t);
                        obj->verts[obj->vert_count++] = (TdoVertex){{x, y, z}};
                    }
                } else if (strcasecmp(tok, "TRIANGLES") == 0) {
                    int n = tp_int(&t);
                    for (int i = 0; i < n && obj->tri_count < TDO_MAX_TRIS; i++) {
                        tp_int(&t); /* "idx:" */
                        TdoTriangle *tri = &obj->tris[obj->tri_count++];
                        tri->a = tp_int(&t); tri->b = tp_int(&t); tri->c = tp_int(&t);
                        tri->color   = tp_int(&t);
                        tp_tok(&t, tok, sizeof(tok)); /* shading string */
                        if (strcasecmp(tok, "FLAT")    == 0) tri->shading = 0;
                        else if (strcasecmp(tok, "GOURAUD") == 0) tri->shading = 1;
                        else tri->shading = 0;
                        /* Rest of the line = material name (often WITHOUT a
                         * leading '#') and/or a comment — one triangle per
                         * line, so skip to end of line unconditionally. */
                        while (t.p < t.end && *t.p != '\n') t.p++;
                    }
                } else if (!tok[0]) {
                    break;
                }
                /* else: skip unknown token */
            }
        }
        /* else: skip unknown top-level token */
    }

    return model->object_count > 0;
}

void tdo_free(TdoModel *model) {
    memset(model, 0, sizeof(*model));
}
