/*
 * inf.c - Simplified INF interactive scripting system
 *
 * The Outlaws INF format is text-based. Example:
 *
 *   ELEVATOR MOVE_FLOOR 17
 *     SECTOR      main_corridor
 *     SPEED       20.0
 *     MASTER      ON
 *     SLAVE       (none)
 *     STOP:  0.00  10.0
 *     STOP:  80.00 10.0
 *
 *   CLASS: DOOR
 *     SECTOR      door_01
 *     SPEED       20.0
 *     MASTER      ON
 *
 * We only implement the most common constructs.
 */
#include "inf.h"
#include "lvt.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Tiny text parser (same style as obt.c)
 * ---------------------------------------------------------------------- */
typedef struct { const char *p, *end; } Tp;

static void tp_skip_ws(Tp *t) {
    while (t->p < t->end) {
        if (*t->p == '#' || *t->p == ';') {
            while (t->p < t->end && *t->p != '\n') t->p++;
            continue;
        }
        if ((unsigned char)*t->p <= ' ') { t->p++; continue; }
        break;
    }
}

static int tp_tok(Tp *t, char *buf, int sz) {
    tp_skip_ws(t);
    int n = 0;
    while (t->p < t->end && n < sz-1) {
        char c = *t->p;
        if (c == '#' || c == ';' || (unsigned char)c <= ' ') break;
        buf[n++] = c; t->p++;
    }
    buf[n] = '\0'; return n;
}

static f32 tp_float(Tp *t) { char b[32]; tp_tok(t,b,sizeof(b)); return strtof(b,NULL); }

/* -------------------------------------------------------------------------
 * INF API
 * ---------------------------------------------------------------------- */
void inf_init(InfSystem *inf) {
    memset(inf, 0, sizeof(*inf));
    inf->pending_user_msg = -1;
    inf->pending_lock_msg = -1;
    inf->pending_explode_sector = -1;
}

/*
 * Classify elevator type string into ElevType.
 */
static ElevType classify_elev_type(const char *s) {
    if (strcasecmp(s, "DOOR")         == 0 ||
        strcasecmp(s, "BASIC_AUTO")   == 0) return ELEV_TYPE_DOOR;
    if (strcasecmp(s, "INV_DOOR")     == 0) return ELEV_TYPE_INV_DOOR;
    if (strcasecmp(s, "MOVE_FLOOR")   == 0 ||
        strcasecmp(s, "BASIC")        == 0) return ELEV_TYPE_MOVE_FLOOR;
    if (strcasecmp(s, "MOVE_CEILING") == 0) return ELEV_TYPE_CEILING;
    if (strcasecmp(s, "SCROLL_FLOOR") == 0) return ELEV_TYPE_SCROLL_FLOOR;
    if (strcasecmp(s, "SCROLL_WALL")  == 0) return ELEV_TYPE_SCROLL_WALL;
    if (strcasecmp(s, "VELOCITY_Z")   == 0) return ELEV_TYPE_VELOCITY_Z;
    /* MORPH_MOVE / MORPH_MOVE1 / MORPH_MOVE2 — sliding doors that translate the
     * whole sector's vertices along ANGLE by the STOP distance. */
    if (strncasecmp(s, "MORPH_MOVE", 10) == 0) return ELEV_TYPE_MORPH_MOVE;
    /* MORPH_SPIN / MORPH_SPIN1 / MORPH_SPIN2 — swinging doors that rotate the
     * sector's vertices around CENTER by the STOP angle (degrees). */
    if (strncasecmp(s, "MORPH_SPIN", 10) == 0) return ELEV_TYPE_MORPH_SPIN;
    if (strncasecmp(s, "CHANGE_LIGHT", 12) == 0) return ELEV_TYPE_CHANGE_LIGHT;
    if (strncasecmp(s, "EXPLODE", 7) == 0) return ELEV_TYPE_EXPLODE;
    return ELEV_TYPE_FLOOR;
}

/*
 * Parse a STOP delay field which can be a float or a keyword:
 *   HOLD      → large delay (effectively infinite)
 *   TERMINATE → 0 delay (stop permanently at last stop)
 */
static f32 parse_stop_delay(Tp *t) {
    char b[32];
    tp_tok(t, b, sizeof(b));
    if (strcasecmp(b, "HOLD") == 0)      return 1e9f;  /* very long hold */
    if (strcasecmp(b, "TERMINATE") == 0) return -1.0f;  /* sentinel: no loop */
    return strtof(b, NULL);
}

/*
 * Parse INF file.
 *
 * Handles two formats:
 *
 * Format A (older): direct ELEVATOR/CLASS: sections
 *   ELEVATOR MOVE_FLOOR
 *     SECTOR main_hall
 *     SPEED  10
 *     STOP:  0  5.0
 *     STOP:  80 0
 *
 * Format B (INF 1.0): ITEM/SEQ/SEQEND blocks
 *   ITEM: SECTOR NAME: door01
 *     SEQ
 *     CLASS: ELEVATOR DOOR
 *       SPEED: 20
 *       STOP: 0 HOLD
 *       STOP: 50 5.0
 *     SEQEND
 */
bool inf_load(InfSystem *inf, const char *text, u32 text_len) {
    if (!text || text_len == 0) return false;
    Tp t = { text, text + text_len };
    char tok[128];
    char pending_sector[64] = {0};  /* sector name from ITEM: SECTOR NAME: */
    bool pending_is_line = false;   /* current ITEM is a LINE (vs SECTOR) */
    u32  pending_num = 0;           /* ITEM NUM: #hex (wall id for LINE) */

    while (t.p < t.end) {
        if (!tp_tok(&t, tok, sizeof(tok))) break;

        /* ---- Format B: ITEM: keyword ---- */
        if (strcasecmp(tok, "ITEM:") == 0) {
            /* Expected: ITEM: SECTOR NAME: <name> (optionally NUM: #hex) */
            char item_type[32];
            tp_tok(&t, item_type, sizeof(item_type));  /* "SECTOR" or "LINE" */
            /* Read key-value pairs until SEQ or SEQEND */
            pending_sector[0] = '\0';
            pending_is_line = (strcasecmp(item_type, "LINE") == 0);
            pending_num = 0;
            while (t.p < t.end) {
                const char *save = t.p;
                tp_tok(&t, tok, sizeof(tok));
                if (!tok[0]) break;
                if (strcasecmp(tok, "SEQ") == 0 ||
                    strcasecmp(tok, "SEQEND") == 0) {
                    t.p = save; break;  /* entered/exited a SEQ block */
                }
                if (strcasecmp(tok, "NAME:") == 0) {
                    tp_tok(&t, pending_sector, sizeof(pending_sector));
                } else if (strcasecmp(tok, "NUM:") == 0) {
                    /* NUM: #A8B2 — wall id hash a LINE trigger attaches to.
                     * The value starts with '#', which the generic tokenizer
                     * would treat as a comment; read it raw here. Skip only
                     * spaces/tabs (not newlines), then take the hex token. */
                    while (t.p < t.end && (*t.p == ' ' || *t.p == '\t')) t.p++;
                    if (t.p < t.end && *t.p == '#') t.p++;   /* hex prefix */
                    char numtok[32]; int nn = 0;
                    while (t.p < t.end && nn < 31 &&
                           isxdigit((unsigned char)*t.p)) numtok[nn++] = *t.p++;
                    numtok[nn] = '\0';
                    pending_num = (u32)strtoul(numtok, NULL, 16);
                }
            }
            continue;
        }

        /* ---- SEQ block start ---- */
        if (strcasecmp(tok, "SEQ") == 0) {
            /* Will parse CLASS: inside */
            continue;
        }

        /* ---- SEQEND: clear pending sector ---- */
        if (strcasecmp(tok, "SEQEND") == 0) {
            pending_sector[0] = '\0';
            continue;
        }

        /* ---- CLASS: keyword (Format B) ---- */
        /* CLASS: ELEVATOR <type> or CLASS: TRIGGER */
        if (strcasecmp(tok, "CLASS:") == 0) {
            char class_word[64];
            tp_tok(&t, class_word, sizeof(class_word));
            if (strcasecmp(class_word, "TRIGGER") == 0) {
                /* CLASS: TRIGGER <type> — parse a trigger (remote switch). */
                char trig_type[32] = {0};
                tp_tok(&t, trig_type, sizeof(trig_type));
                if (inf->trigger_count >= INF_MAX_TRIGGERS) continue;
                InfTrigger *tr = &inf->triggers[inf->trigger_count];
                memset(tr, 0, sizeof(*tr));
                tr->active = true;
                tr->sector_idx = 0xFFFFFFFF;
                tr->client_sector = -1;
                tr->event_mask = INF_EVENT_NUDGE; /* default: nudge */
                tr->single = (strcasecmp(trig_type, "SINGLE") == 0);
                snprintf(tr->sector_name, sizeof(tr->sector_name), "%s", pending_sector);
                tr->is_line = pending_is_line;
                tr->line_id = pending_num;

                while (t.p < t.end) {
                    const char *save = t.p;
                    tp_tok(&t, tok, sizeof(tok));
                    if (!tok[0]) break;
                    if (strcasecmp(tok, "SEQEND") == 0 ||
                        strcasecmp(tok, "ITEM:")  == 0 ||
                        strcasecmp(tok, "CLASS:") == 0) { t.p = save; break; }
                    if (strcasecmp(tok, "CLIENT:") == 0) {
                        tp_tok(&t, tr->client_name, sizeof(tr->client_name));
                        if (strcasecmp(tr->client_name, "SYSTEM") == 0) tr->to_system = true;
                    } else if (strcasecmp(tok, "EVENT_MASK:") == 0) {
                        /* Explicit mask (0 = script-only, never fired by the
                         * player's enter/nudge/shoot). */
                        tr->event_mask = (u32)(i32)tp_float(&t);
                    } else if (strcasecmp(tok, "MESSAGE:") == 0) {
                        char mt[32]; tp_tok(&t, mt, sizeof(mt));
                        if      (strcasecmp(mt, "NEXT_STOP")  == 0) tr->msg = INF_MSG_NEXT_STOP;
                        else if (strcasecmp(mt, "PREV_STOP")  == 0) tr->msg = INF_MSG_PREV_STOP;
                        else if (strcasecmp(mt, "GOTO_STOP")  == 0) { tr->msg = INF_MSG_GOTO_STOP; tr->msg_param = (i32)tp_float(&t); }
                        else if (strcasecmp(mt, "MASTER_ON")  == 0) tr->msg = INF_MSG_MASTER_ON;
                        else if (strcasecmp(mt, "MASTER_OFF") == 0) tr->msg = INF_MSG_MASTER_OFF;
                        else if (strcasecmp(mt, "COMPLETE")   == 0) tr->msg = INF_MSG_COMPLETE;
                        else if (strcasecmp(mt, "WAKEUP")     == 0) tr->msg = INF_MSG_WAKEUP;
                        else if (strcasecmp(mt, "USER_MSG")   == 0) { tr->msg = INF_MSG_USER_MSG; tr->msg_param = (i32)tp_float(&t); }
                        else if (strcasecmp(mt, "END_LEVEL")  == 0) tr->msg = INF_MSG_END_LEVEL;
                        else if (strcasecmp(mt, "SPAWN_LEVEL") == 0) {
                            /* SPAWN_LEVEL <level> [startpos] — load a mission
                             * (OFFICE hub porches). */
                            tr->msg = INF_MSG_SPAWN_LEVEL;
                            tp_tok(&t, tr->spawn_level, sizeof(tr->spawn_level));
                            /* Optional named start; stop before the next keyword. */
                            const char *sv = t.p;
                            char s2[32]; tp_tok(&t, s2, sizeof(s2));
                            if (s2[0] && strcasecmp(s2, "EVENT_MASK:") != 0 &&
                                strcasecmp(s2, "CLIENT:") != 0 &&
                                strcasecmp(s2, "SEQEND") != 0 &&
                                strcasecmp(s2, "CLASS:") != 0 &&
                                strcasecmp(s2, "ITEM:") != 0)
                                snprintf(tr->spawn_start, sizeof(tr->spawn_start), "%s", s2);
                            else t.p = sv;   /* no startpos: rewind */
                        }
                        else if (strcasecmp(mt, "DONE")       == 0) tr->msg = INF_MSG_DONE;
                    }
                    /* Ignore OBJECT:, OBJECT_EXCLUDE:, CENTER:, etc. */
                }
                inf->trigger_count++;
                continue;
            }
            /* If "ELEVATOR", read actual type next */
            char type_str[64] = {0};
            if (strcasecmp(class_word, "ELEVATOR") == 0 ||
                strcasecmp(class_word, "ELVATOR")  == 0) {
                tp_tok(&t, type_str, sizeof(type_str));
            } else {
                /* CLASS: <type> directly */
                snprintf(type_str, sizeof(type_str), "%s", class_word);
            }

            if (inf->count >= INF_MAX_ELEVS) continue;
            Elevator *el = &inf->elevs[inf->count];
            memset(el, 0, sizeof(*el));
            el->active = true;
            el->master = true;
            el->speed  = 20.0f;
            el->type   = classify_elev_type(type_str);

            /* Sector from pending (Format B) */
            snprintf(el->sector_name, sizeof(el->sector_name), "%s", pending_sector);

            /* Parse body of this SEQ item until SEQEND or new ITEM/CLASS */
            while (t.p < t.end) {
                const char *save = t.p;
                tp_tok(&t, tok, sizeof(tok));
                if (!tok[0]) break;
                if (strcasecmp(tok, "SEQEND") == 0 ||
                    strcasecmp(tok, "ITEM:")   == 0 ||
                    strcasecmp(tok, "CLASS:")  == 0) {
                    t.p = save; break;
                }
                if (strcasecmp(tok, "SECTOR")  == 0 ||
                    strcasecmp(tok, "SECTOR:")  == 0) {
                    tp_tok(&t, el->sector_name, sizeof(el->sector_name));
                } else if (strcasecmp(tok, "SPEED")  == 0 ||
                           strcasecmp(tok, "SPEED:")  == 0) {
                    el->speed = tp_float(&t);
                } else if (strcasecmp(tok, "ANGLE")  == 0 ||
                           strcasecmp(tok, "ANGLE:")  == 0) {
                    el->angle_deg = tp_float(&t);
                } else if (strcasecmp(tok, "CENTER:") == 0 ||
                           strcasecmp(tok, "CENTER")  == 0) {
                    el->center_x = tp_float(&t);
                    el->center_z = tp_float(&t);
                } else if (strcasecmp(tok, "STOP:")  == 0 ||
                           strcasecmp(tok, "STOP")   == 0) {
                    if (el->stop_count < INF_MAX_STOPS) {
                        ElevStop *s = &el->stops[el->stop_count++];
                        s->y     = tp_float(&t);
                        s->delay = parse_stop_delay(&t);
                    }
                } else if (strcasecmp(tok, "KEY")    == 0 ||
                           strcasecmp(tok, "KEY:")   == 0) {
                    tp_tok(&t, tok, sizeof(tok));
                    el->key_trigger = true;
                } else if (strcasecmp(tok, "SOUND:") == 0 ||
                           strcasecmp(tok, "SOUND")  == 0) {
                    char stype[8];
                    tp_tok(&t, stype, sizeof(stype)); /* type: 1=trigger, 2=loop (we treat as trigger) */
                    tp_tok(&t, el->sound_file, sizeof(el->sound_file));
                } else if (strcasecmp(tok, "SLAVE:") == 0 ||
                           strcasecmp(tok, "SLAVE")  == 0) {
                    /* SLAVE: <sector name> — a sector that moves with this one. */
                    char sn[64]; tp_tok(&t, sn, sizeof(sn));
                    if (sn[0] && strcasecmp(sn, "(none)") != 0 &&
                        el->slave_count < 8) {
                        snprintf(el->slave_names[el->slave_count], 64, "%s", sn);
                        el->slave_sectors[el->slave_count] = -1;
                        el->slave_count++;
                    }
                } else if (strcasecmp(tok, "MASTER") == 0 ||
                           strcasecmp(tok, "MASTER:") == 0) {
                    char on[8]; tp_tok(&t, on, sizeof(on));
                    el->master = (strcasecmp(on, "OFF") != 0);
                } else if (strcasecmp(tok, "OBJECT:") == 0) {
                    /* OBJECT: <id> <mask> [mode] — restricts the elevator to a
                     * class of activator. id 110 = the shovel (dig spots): gate
                     * the elevator on the SHOVEL item. */
                    i32 oid = (i32)tp_float(&t);
                    if (oid == 110) el->required_key = INF_KEY_SHOVEL;
                } else if (strcasecmp(tok, "EVENT_MASK:") == 0) {
                    el->self_event_mask = (u32)(i32)tp_float(&t);
                } else if (strcasecmp(tok, "CLIENT:") == 0) {
                    tp_tok(&t, el->self_client, sizeof(el->self_client));
                    if (strcasecmp(el->self_client, "SYSTEM") == 0)
                        el->self_to_system = true;
                } else if (strcasecmp(tok, "MESSAGE:") == 0) {
                    char mt[32]; tp_tok(&t, mt, sizeof(mt));
                    /* Two forms:
                     *   MESSAGE: <n> <client> <TYPE> <param>  — fires when the
                     *     current (last) stop is reached (scripted sequences).
                     *   MESSAGE: <TYPE> [param]               — elevator-as-trigger. */
                    bool numeric = (mt[0] == '-' || (mt[0] >= '0' && mt[0] <= '9'));
                    if (numeric && el->stop_count > 0) {
                        ElevStop *st = &el->stops[el->stop_count - 1];
                        char client[64], type[32];
                        tp_tok(&t, client, sizeof(client));
                        tp_tok(&t, type, sizeof(type));
                        if (st->msg_count < INF_MAX_STOP_MSGS) {
                            StopMsg *m = &st->msgs[st->msg_count++];
                            snprintf(m->client, sizeof(m->client), "%s", client);
                            m->client_sector = -1;
                            m->to_system = (strcasecmp(client, "SYSTEM") == 0);
                            m->param = 0;
                            if      (strcasecmp(type, "NEXT_STOP") == 0) m->type = INF_MSG_NEXT_STOP;
                            else if (strcasecmp(type, "PREV_STOP") == 0) m->type = INF_MSG_PREV_STOP;
                            else if (strcasecmp(type, "GOTO_STOP") == 0) { m->type = INF_MSG_GOTO_STOP; m->param = (i32)tp_float(&t); }
                            else if (strcasecmp(type, "MASTER_ON")  == 0) m->type = INF_MSG_MASTER_ON;
                            else if (strcasecmp(type, "MASTER_OFF") == 0) m->type = INF_MSG_MASTER_OFF;
                            else if (strcasecmp(type, "USER_MSG")   == 0) { m->type = INF_MSG_USER_MSG; m->param = (i32)tp_float(&t); }
                            else if (strcasecmp(type, "END_LEVEL")  == 0) m->type = INF_MSG_END_LEVEL;
                            else m->type = INF_MSG_NEXT_STOP;
                        }
                    }
                    else if (strcasecmp(mt, "NEXT_STOP") == 0) el->self_msg = INF_MSG_NEXT_STOP;
                    else if (strcasecmp(mt, "PREV_STOP") == 0) el->self_msg = INF_MSG_PREV_STOP;
                    else if (strcasecmp(mt, "GOTO_STOP") == 0) { el->self_msg = INF_MSG_GOTO_STOP; el->self_msg_param = (i32)tp_float(&t); }
                    else if (strcasecmp(mt, "MASTER_ON")  == 0) el->self_msg = INF_MSG_MASTER_ON;
                    else if (strcasecmp(mt, "MASTER_OFF") == 0) el->self_msg = INF_MSG_MASTER_OFF;
                    else if (strcasecmp(mt, "USER_MSG")   == 0) { el->self_msg = INF_MSG_USER_MSG; el->self_msg_param = (i32)tp_float(&t); }
                    else if (strcasecmp(mt, "END_LEVEL")  == 0) el->self_msg = INF_MSG_END_LEVEL;
                }
                /* Skip OBJECT:, OBJECT_EXCLUDE:, CENTER handled above. */
            }

            el->sector_idx  = 0xFFFFFFFF;
            if (el->stop_count > 0) el->current_y = el->stops[0].y;
            el->current_stop = 0;
            el->next_stop    = 0;
            el->delay_timer  = (el->stop_count > 0) ? el->stops[0].delay : 0;
            if (el->delay_timer < 0) el->delay_timer = 0;  /* TERMINATE */
            inf->count++;
            continue;
        }

        /* ---- Format A: ELEVATOR <type> directly ---- */
        if (strcasecmp(tok, "ELEVATOR") == 0) {
            if (inf->count >= INF_MAX_ELEVS) continue;
            Elevator *el = &inf->elevs[inf->count];
            memset(el, 0, sizeof(*el));
            el->active = true;
            el->master = true;
            el->speed  = 20.0f;

            char type_str[64];
            tp_tok(&t, type_str, sizeof(type_str));
            el->type = classify_elev_type(type_str);

            /* Body: SECTOR, SPEED, STOP, KEY */
            while (t.p < t.end) {
                const char *save = t.p;
                tp_tok(&t, tok, sizeof(tok));
                if (!tok[0]) break;
                if (strcasecmp(tok, "ELEVATOR") == 0 ||
                    strcasecmp(tok, "CLASS:")   == 0 ||
                    strcasecmp(tok, "ITEM:")    == 0 ||
                    strcasecmp(tok, "TRIGGER")  == 0) {
                    t.p = save; break;
                }
                if (strcasecmp(tok, "SECTOR")  == 0) {
                    tp_tok(&t, el->sector_name, sizeof(el->sector_name));
                } else if (strcasecmp(tok, "SPEED")   == 0) {
                    el->speed = tp_float(&t);
                } else if (strcasecmp(tok, "ANGLE")   == 0) {
                    el->angle_deg = tp_float(&t);
                } else if (strcasecmp(tok, "STOP:")   == 0 ||
                           strcasecmp(tok, "STOP")    == 0) {
                    if (el->stop_count < INF_MAX_STOPS) {
                        ElevStop *s = &el->stops[el->stop_count++];
                        s->y     = tp_float(&t);
                        s->delay = parse_stop_delay(&t);
                    }
                } else if (strcasecmp(tok, "KEY")  == 0) {
                    tp_tok(&t, tok, sizeof(tok));
                    el->key_trigger = true;
                } else if (strcasecmp(tok, "SOUND") == 0) {
                    char stype[8];
                    tp_tok(&t, stype, sizeof(stype));
                    tp_tok(&t, el->sound_file, sizeof(el->sound_file));
                }
            }

            el->sector_idx  = 0xFFFFFFFF;
            if (el->stop_count > 0) el->current_y = el->stops[0].y;
            el->current_stop = 0;
            el->next_stop    = 0;
            el->delay_timer  = (el->stop_count > 0) ? el->stops[0].delay : 0;
            if (el->delay_timer < 0) el->delay_timer = 0;
            inf->count++;
        }
    }

    return inf->count > 0;
}

/* -------------------------------------------------------------------------
 * Sector name resolution
 * ---------------------------------------------------------------------- */
void inf_resolve_sectors(InfSystem *inf, const LvtLevel *level) {
    u32 resolved = 0, unresolved = 0, morph = 0;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (el->sector_idx != 0xFFFFFFFF) continue;
        if (!el->sector_name[0]) { unresolved++; continue; }
        i32 idx = lvt_find_sector_by_name(level, el->sector_name);
        if (idx < 0) {
            unresolved++;
            OL_LOG("INF: unresolved sector '%s'\n", el->sector_name);
        }
        if (idx >= 0) {
            resolved++;
            /* Locked door? Encode required key from the sector name — but don't
             * clobber a requirement already set from an OBJECT filter (dig/shovel). */
            if (el->required_key == INF_KEY_NONE)
                el->required_key = inf_key_from_name(el->sector_name);
            if (el->required_key != INF_KEY_NONE)
                OL_LOG("INF: locked door sector '%s' (sec %d) needs key %d\n",
                       el->sector_name, idx, el->required_key);
            if (el->type == ELEV_TYPE_MORPH_MOVE || el->type == ELEV_TYPE_MORPH_SPIN) morph++;
            el->sector_idx = (u32)idx;
            /* Initialize current_y from actual sector floor/ceiling */
            const LvtSector *sec = &level->sectors[idx];
            if (el->stop_count == 0) {
                /* Door with no explicit stops: floor=closed, ceil=open */
                el->current_y = (el->type == ELEV_TYPE_DOOR) ? sec->ceil_y : sec->floor_y;
            }
            /* Morph doors: snapshot the authored (closed) sector vertices. The
             * loaded geometry corresponds to the first stop's morph value. */
            if (el->type == ELEV_TYPE_MORPH_MOVE || el->type == ELEV_TYPE_MORPH_SPIN) {
                if (sec->vertex_count <= INF_MORPH_MAX_VERTS) {
                    el->base_vert_count = sec->vertex_count;
                    for (u32 v = 0; v < sec->vertex_count; v++)
                        el->base_verts[v] = sec->vertices[v];
                    el->base_captured = true;
                }
                el->current_y = (el->stop_count > 0) ? el->stops[0].y : 0.0f;
            }
        }
    }
    /* Resolve trigger sectors + their CLIENT (target) sectors. */
    u32 tr_res = 0;
    for (u32 i = 0; i < inf->trigger_count; i++) {
        InfTrigger *tr = &inf->triggers[i];
        if (tr->sector_name[0] && tr->sector_idx == 0xFFFFFFFF) {
            i32 idx = lvt_find_sector_by_name(level, tr->sector_name);
            if (idx >= 0) tr->sector_idx = (u32)idx;
        }
        if (!tr->to_system && tr->client_name[0] && tr->client_sector < 0)
            tr->client_sector = lvt_find_sector_by_name(level, tr->client_name);
        if (tr->sector_idx != 0xFFFFFFFF) tr_res++;
    }

    /* Resolve SLAVE sectors and elevator-as-trigger CLIENTs. */
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        for (u32 s = 0; s < el->slave_count; s++)
            el->slave_sectors[s] = lvt_find_sector_by_name(level, el->slave_names[s]);
        el->self_client_sector = -1;
        if (el->self_msg && !el->self_to_system && el->self_client[0])
            el->self_client_sector = lvt_find_sector_by_name(level, el->self_client);
        /* Resolve per-stop message clients. */
        for (u32 s = 0; s < el->stop_count; s++)
            for (u32 m = 0; m < el->stops[s].msg_count; m++) {
                StopMsg *sm = &el->stops[s].msgs[m];
                if (!sm->to_system && sm->client[0])
                    sm->client_sector = lvt_find_sector_by_name(level, sm->client);
            }
    }

    /* Derive door locks from co-located USER_MSG triggers. A locked door has a
     * MORPH elevator plus a TRIGGER on the same sector that shows a "you need
     * the X key" / "it's locked" message. The message id tells us which key (or
     * that it is permanently stuck). The trigger is then absorbed (deactivated)
     * so nudging shows the hint via the door logic, not a duplicate SYSTEM msg. */
    u32 locks = 0;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (el->type != ELEV_TYPE_MORPH_MOVE && el->type != ELEV_TYPE_MORPH_SPIN &&
            el->type != ELEV_TYPE_DOOR && el->type != ELEV_TYPE_INV_DOOR) continue;
        if (el->sector_idx == 0xFFFFFFFF) continue;
        for (u32 j = 0; j < inf->trigger_count; j++) {
            InfTrigger *tr = &inf->triggers[j];
            if (!tr->active || tr->sector_idx != el->sector_idx) continue;
            if (tr->msg != INF_MSG_USER_MSG) continue;
            bool perm = false;
            int k = inf_key_from_msg(tr->msg_param, &perm);
            el->lock_msg_id = tr->msg_param;
            if (perm)          el->perm_locked = true;
            else if (k != INF_KEY_NONE) el->required_key = k;
            tr->active = false;   /* absorbed into the door lock */
            locks++;
        }
    }

    OL_LOG("INF resolve: %u resolved (%u morph doors, %u locks), %u unresolved; %u/%u triggers\n",
           resolved, morph, locks, unresolved, tr_res, inf->trigger_count);

}

void inf_create_flag_doors(InfSystem *inf, LvtLevel *level) {
    u32 created = 0;
    for (u32 s = 0; s < level->sector_count; s++) {
        LvtSector *sec = &level->sectors[s];
        /* Only the real door flag (0x100). 0x200 WITHOUT 0x100 is NOT a door
         * (vestigial direction bit — e.g. HIDEOUT 77/82/83 sealed panels). */
        if (!(sec->flags & LVT_SEC_FLAG_DOOR)) continue;
        /* Skip if an INF elevator already controls this sector (scripted doors,
         * lifts, morphs take precedence over the auto-door flag). */
        bool taken = false;
        for (u32 i = 0; i < inf->count; i++)
            if (inf->elevs[i].active && inf->elevs[i].sector_idx == s) { taken = true; break; }
        if (taken) continue;
        if (inf->count >= INF_MAX_ELEVS) {
            OL_WARN("INF: elevator table full, %u door sectors left without auto-doors\n",
                    level->sector_count - s);
            break;
        }
        Elevator *el = &inf->elevs[inf->count++];
        memset(el, 0, sizeof(*el));
        el->active      = true;
        el->master      = true;
        el->sector_idx  = s;
        el->type        = ELEV_TYPE_FLAG_DOOR;   /* mask-scroll door */
        el->speed       = 2.0f;                  /* open amount/s → ~0.5s slide */
        el->stop_count  = 2;
        el->stops[0].y     = 0.0f;   el->stops[0].delay = 1e9f;  /* closed, HOLD */
        el->stops[1].y     = 1.0f;   el->stops[1].delay = 1e9f;  /* open,   HOLD */
        el->current_stop = 0; el->next_stop = 0;
        el->current_y = 0.0f; el->target_y = 0.0f;
        el->delay_timer = 1e9f; el->moving = false;
        el->required_key = INF_KEY_NONE;
        el->stack_partner = -1;
        snprintf(el->sound_file, sizeof(el->sound_file), "DOOR3.WAV");
        /* Mark the sector so collision blocks its panel walls while shut and the
         * renderer draws/hides the panel by door state. Start CLOSED. */
        sec->is_flag_door = true;
        sec->door_open    = false;
        sec->door_slide   = 0.0f;

        /* Find a stacked partner: a sector sharing this door's XZ footprint whose
         * ceiling meets this floor (or floor meets this ceiling) — the doorway is
         * split vertically (walk-through leaf + transom). It opens as one. */
        f32 cx = 0, cz = 0;
        for (u32 v = 0; v < sec->vertex_count; v++) { cx += sec->vertices[v].x; cz += sec->vertices[v].y; }
        if (sec->vertex_count) { cx /= sec->vertex_count; cz /= sec->vertex_count; }
        for (u32 p = 0; p < level->sector_count; p++) {
            if (p == s) continue;
            LvtSector *ps = &level->sectors[p];
            if (ps->vertex_count == 0) continue;
            f32 px = 0, pz = 0;
            for (u32 v = 0; v < ps->vertex_count; v++) { px += ps->vertices[v].x; pz += ps->vertices[v].y; }
            px /= ps->vertex_count; pz /= ps->vertex_count;
            if (fabsf(px - cx) > 2.0f || fabsf(pz - cz) > 2.0f) continue;  /* same column */
            if (fabsf(ps->ceil_y - sec->floor_y) < 0.5f ||
                fabsf(ps->floor_y - sec->ceil_y) < 0.5f) {
                el->stack_partner = (i32)p;
                ps->is_flag_door = true;   /* opens/closes with the master */
                ps->door_open    = false;
                ps->door_slide   = 0.0f;
                break;
            }
        }
        created++;
    }
    if (created)
        OL_LOG("INF: auto-created %u flag-door(s) (sector DOOR flag 0x%X)\n",
               created, LVT_SEC_FLAG_DOOR);
}

/* ---- Locked doors: key classification from the door sector name ---- */
const char *inf_key_name(int key) {
    switch (key) {
    case INF_KEY_STEEL:   return "STEEL KEY";
    case INF_KEY_IRON:    return "IRON KEY";
    case INF_KEY_BRASS:   return "BRASS KEY";
    case INF_KEY_ROUND:   return "ROUND KEY";
    case INF_KEY_SQUARE:  return "SQUARE KEY";
    case INF_KEY_CROWBAR: return "CROWBAR";
    case INF_KEY_SHOVEL:  return "SHOVEL";
    case INF_KEY_BADGE:   return "BADGE";
    default:              return "KEY";
    }
}

/* Case-insensitive substring test. */
static bool name_has(const char *s, const char *sub) {
    size_t ls = strlen(s), lu = strlen(sub);
    for (size_t i = 0; i + lu <= ls; i++) {
        size_t j = 0;
        for (; j < lu; j++) {
            char a = s[i+j], b = sub[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (j == lu) return true;
    }
    return false;
}

int inf_key_from_name(const char *n) {
    if (!n || !n[0]) return INF_KEY_NONE;
    if (name_has(n, "STEEL"))  return INF_KEY_STEEL;
    if (name_has(n, "IRON"))   return INF_KEY_IRON;
    if (name_has(n, "BRASS"))  return INF_KEY_BRASS;
    if (name_has(n, "SQUARE") || name_has(n, "SQR")) return INF_KEY_SQUARE;
    if (name_has(n, "ROUND"))  return INF_KEY_ROUND;
    if (name_has(n, "CROW") || name_has(n, "BOARD") || name_has(n, "PLANK") ||
        name_has(n, "PRY"))    return INF_KEY_CROWBAR;
    if (name_has(n, "LOCKED")) return INF_KEY_GENERIC;
    return INF_KEY_NONE;
}

/*
 * Classify a door lock from the LOCAL.MSG id its USER_MSG trigger shows.
 * This is the authoritative Outlaws mechanism (a locked door carries a
 * USER_MSG trigger for players lacking the key). Sets *perm for permanently
 * stuck/locked doors (message-only, no key opens them). Returns InfKeyType.
 *   800 steel · 801 brass · 802 iron · 803 round stone · 804 stone keys
 *   805 stuck · 807 locked from inside · 808 locked
 */
int inf_key_from_msg(i32 msg_id, bool *perm) {
    if (perm) *perm = false;
    switch (msg_id) {
    case 800: return INF_KEY_STEEL;
    case 801: return INF_KEY_BRASS;
    case 802: return INF_KEY_IRON;
    case 803: return INF_KEY_ROUND;
    case 804: return INF_KEY_SQUARE;
    case 817: return INF_KEY_CROWBAR;  /* "Got a crowbar, pardner?" — boarded door */
    case 805: case 807: case 808:
        if (perm) *perm = true;
        return INF_KEY_NONE;
    default:  return INF_KEY_NONE;
    }
}

/* Drive a master elevator's SLAVE sectors to toggle in sync with it. Chains
 * through nested slaves; the `moving` guard prevents cycles from re-entering. */
static void inf_move_slaves(InfSystem *inf, Elevator *master) {
    for (u32 s = 0; s < master->slave_count; s++) {
        i32 sec = master->slave_sectors[s];
        if (sec < 0) continue;
        for (u32 i = 0; i < inf->count; i++) {
            Elevator *sl = &inf->elevs[i];
            if (sl == master || (i32)sl->sector_idx != sec) continue;
            if (sl->stop_count >= 2 && !sl->moving) {
                sl->next_stop = (sl->current_stop == 0) ? (sl->stop_count - 1) : 0;
                sl->target_y  = sl->stops[sl->next_stop].y;
                sl->moving = true; sl->just_triggered = true;
                inf_move_slaves(inf, sl);
            }
        }
    }
}

InfDoorResult inf_nudge_door(InfSystem *inf, u32 sector_idx,
                             u32 have_keys, int *needed_key) {
    InfDoorResult res = INF_DOOR_NONE;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        /* Match the door's own sector, or a flag-door whose stacked partner is
         * this sector (so nudging the walk-through leaf opens the whole door). */
        if (!el->active ||
            (el->sector_idx != sector_idx && el->stack_partner != (i32)sector_idx))
            continue;
        bool is_door = (el->type == ELEV_TYPE_MORPH_MOVE ||
                        el->type == ELEV_TYPE_MORPH_SPIN ||
                        el->type == ELEV_TYPE_DOOR ||
                        el->type == ELEV_TYPE_INV_DOOR ||
                        el->type == ELEV_TYPE_FLAG_DOOR ||
                        el->type == ELEV_TYPE_MOVE_FLOOR);
        if (!is_door) continue;

        /* Shovel dig spot: a MOVE_FLOOR gated on the shovel with a chain of
         * stops. Each nudge (with the shovel) digs one step deeper until the
         * last stop; without the shovel, nothing happens. */
        if (el->required_key == INF_KEY_SHOVEL &&
            el->type == ELEV_TYPE_MOVE_FLOOR && el->stop_count > 2) {
            bool have = (have_keys & (1u << INF_KEY_SHOVEL)) != 0;
            if (!have) {
                if (needed_key) *needed_key = INF_KEY_SHOVEL;
                if (res == INF_DOOR_NONE) res = INF_DOOR_LOCKED;
                continue;
            }
            if (!el->moving && el->current_stop + 1 < el->stop_count) {
                el->next_stop = el->current_stop + 1;
                el->target_y  = el->stops[el->next_stop].y;
                el->moving = true; el->just_triggered = true;
            }
            res = INF_DOOR_UNLOCKED;   /* uses the "unlock" feedback (dig sound) */
            continue;
        }

        /* Permanently stuck / locked-from-inside: never opens, just the hint. */
        if (el->perm_locked) {
            if (el->lock_msg_id) inf->pending_lock_msg = el->lock_msg_id;
            if (res == INF_DOOR_NONE) res = INF_DOOR_LOCKED;
            continue;
        }

        if (el->required_key != INF_KEY_NONE && !el->unlocked) {
            /* Generic locks accept any key/tool the player holds. */
            bool have = (el->required_key == INF_KEY_GENERIC)
                        ? (have_keys & ~1u) != 0
                        : (have_keys & (1u << el->required_key)) != 0;
            if (!have) {
                if (needed_key) *needed_key = el->required_key;
                if (el->lock_msg_id) inf->pending_lock_msg = el->lock_msg_id;
                if (res == INF_DOOR_NONE) res = INF_DOOR_LOCKED;
                continue;
            }
            el->unlocked = true;
            res = INF_DOOR_UNLOCKED;
        } else if (res != INF_DOOR_UNLOCKED) {
            res = INF_DOOR_OPENED;
        }

        /* Drive any SLAVE sectors to match this master door. */
        inf_move_slaves(inf, el);

        /* Toggle the door between its two stops. */
        if (el->stop_count >= 2 && !el->moving) {
            el->next_stop = (el->current_stop == 0) ? 1u : 0u;
            el->target_y  = el->stops[el->next_stop].y;
            el->moving = true; el->just_triggered = true;
        }
    }
    return res;
}

/* Is `el` a nudge-openable door type? */
static bool elev_is_door(const Elevator *el) {
    return el->type == ELEV_TYPE_MORPH_MOVE || el->type == ELEV_TYPE_MORPH_SPIN ||
           el->type == ELEV_TYPE_DOOR || el->type == ELEV_TYPE_INV_DOOR ||
           el->type == ELEV_TYPE_FLAG_DOOR || el->type == ELEV_TYPE_MOVE_FLOOR;
}

static void door_centroid(const LvtLevel *level, const Elevator *el,
                          f32 *cx, f32 *cz) {
    const LvtSector *sec = &level->sectors[el->sector_idx];
    f32 sx = 0, sz = 0; u32 n = sec->vertex_count;
    for (u32 v = 0; v < n; v++) { sx += sec->vertices[v].x; sz += sec->vertices[v].y; }
    *cx = n ? sx / n : 0; *cz = n ? sz / n : 0;
}

/* A door leaf that a single USE should co-open with its neighbour: a visible
 * swinging/sliding panel — NOT a MOVE_FLOOR controller/dummy (those are invisible
 * and must not be toggled by proximity). */
static bool elev_is_leaf(const Elevator *el) {
    return el->type == ELEV_TYPE_MORPH_MOVE || el->type == ELEV_TYPE_MORPH_SPIN ||
           el->type == ELEV_TYPE_FLAG_DOOR  || el->type == ELEV_TYPE_DOOR ||
           el->type == ELEV_TYPE_INV_DOOR;
}

static void door_bbox(const LvtLevel *level, const Elevator *el,
                      f32 *minx, f32 *maxx, f32 *minz, f32 *maxz) {
    const LvtSector *sec = &level->sectors[el->sector_idx];
    f32 nx = 1e30f, xx = -1e30f, nz = 1e30f, xz = -1e30f;
    for (u32 v = 0; v < sec->vertex_count; v++) {
        f32 x = sec->vertices[v].x, z = sec->vertices[v].y;
        if (x < nx) nx = x; if (x > xx) xx = x;
        if (z < nz) nz = z; if (z > xz) xz = z;
    }
    *minx = nx; *maxx = xx; *minz = nz; *maxz = xz;
}

/* Two leaves belong to ONE doorway when their sector footprints touch/overlap
 * (a split door's halves meet along the jamb). This replaces the old fixed-radius
 * cluster, which wrongly swept in the NEXT doorway a few units away — e.g. TRAIN's
 * wagon doors are ~9u apart, so pressing USE on one opened the whole car. */
#define DOOR_ADJ_GAP 2.0f
static bool doors_same_doorway(const LvtLevel *level,
                               f32 anx, f32 axx, f32 anz, f32 axz,
                               const Elevator *b) {
    f32 bnx, bxx, bnz, bxz;
    door_bbox(level, b, &bnx, &bxx, &bnz, &bxz);
    bool xov = (anx - DOOR_ADJ_GAP <= bxx) && (bnx - DOOR_ADJ_GAP <= axx);
    bool zov = (anz - DOOR_ADJ_GAP <= bxz) && (bnz - DOOR_ADJ_GAP <= axz);
    return xov && zov;
}

InfDoorResult inf_nudge_door_near(InfSystem *inf, const LvtLevel *level,
                                  f32 px, f32 pz, f32 radius,
                                  u32 have_keys, int *needed_key) {
    /* Find the nearest door within radius. */
    i32 best = -1; f32 best_d2 = radius * radius; f32 ncx = 0, ncz = 0;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active || !elev_is_door(el)) continue;
        if (el->sector_idx >= level->sector_count) continue;
        if (level->sectors[el->sector_idx].vertex_count == 0) continue;
        f32 cx, cz; door_centroid(level, el, &cx, &cz);
        f32 dx = px - cx, dz = pz - cz, d2 = dx*dx + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = (i32)i; ncx = cx; ncz = cz; }
    }
    if (best < 0) return INF_DOOR_NONE;
    (void)ncx; (void)ncz;

    /* Open the nearest door plus the other leaf of the SAME doorway (touching
     * footprint), so a split/double door opens both halves — but not a separate
     * doorway nearby. Aggregate: UNLOCKED wins, else LOCKED, else OPENED. */
    f32 anx, axx, anz, axz;
    door_bbox(level, &inf->elevs[best], &anx, &axx, &anz, &axz);
    InfDoorResult res = INF_DOOR_NONE;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active || !elev_is_door(el)) continue;
        if (el->sector_idx >= level->sector_count) continue;
        if (level->sectors[el->sector_idx].vertex_count == 0) continue;
        /* the primary itself, or a leaf sharing its doorway */
        if ((i32)i != best &&
            !(elev_is_leaf(el) && doors_same_doorway(level, anx, axx, anz, axz, el)))
            continue;
        int nk = INF_KEY_NONE;
        InfDoorResult r = inf_nudge_door(inf, el->sector_idx, have_keys, &nk);
        if (r == INF_DOOR_UNLOCKED) res = INF_DOOR_UNLOCKED;
        else if (r == INF_DOOR_LOCKED && res != INF_DOOR_UNLOCKED) {
            res = INF_DOOR_LOCKED; if (needed_key) *needed_key = nk;
        } else if (r == INF_DOOR_OPENED && res == INF_DOOR_NONE) {
            res = INF_DOOR_OPENED;
        }
    }
    return res;
}

/* Point-in-sector 2D test (local copy — collision.c has its own). */
static bool inf_point_in_sector(const LvtSector *s, f32 x, f32 z) {
    int c = 0;
    for (u32 i = 0; i < s->wall_count; i++) {
        const LvtWall *w = &s->walls[i];
        if (w->v1 < 0 || w->v2 < 0 ||
            w->v1 >= (i32)s->vertex_count || w->v2 >= (i32)s->vertex_count) continue;
        f32 x0 = s->vertices[w->v1].x, z0 = s->vertices[w->v1].y;
        f32 x1 = s->vertices[w->v2].x, z1 = s->vertices[w->v2].y;
        if ((z0 <= z && z < z1) || (z1 <= z && z < z0)) {
            f32 t = (z - z0) / (z1 - z0);
            if (x < x0 + t * (x1 - x0)) c++;
        }
    }
    return (c & 1) != 0;
}

/* Nudge the door on sector `target` plus the other leaf of the same doorway. */
static InfDoorResult inf_nudge_door_group(InfSystem *inf, const LvtLevel *level,
                                          u32 target, u32 have_keys,
                                          int *needed_key) {
    if (target >= level->sector_count) return INF_DOOR_NONE;
    /* Find a door elevator on `target` (or a flag-door whose stacked partner is
     * `target` — nudging the walk-through leaf opens its transom's door); if
     * none, nothing to group around. */
    i32 tgt = -1;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (el->active && elev_is_door(el) &&
            el->sector_idx < level->sector_count &&
            (el->sector_idx == target || el->stack_partner == (i32)target)) {
            tgt = (i32)i; break;
        }
    }
    if (tgt < 0) return INF_DOOR_NONE;

    f32 anx, axx, anz, axz;
    door_bbox(level, &inf->elevs[tgt], &anx, &axx, &anz, &axz);
    InfDoorResult res = INF_DOOR_NONE;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active || !elev_is_door(el)) continue;
        if (el->sector_idx >= level->sector_count) continue;
        /* the target itself, or another leaf sharing its doorway (touching
         * footprint) — NOT a separate doorway a few units away. */
        if ((i32)i != tgt &&
            !(elev_is_leaf(el) && doors_same_doorway(level, anx, axx, anz, axz, el)))
            continue;
        int nk = INF_KEY_NONE;
        InfDoorResult r = inf_nudge_door(inf, el->sector_idx, have_keys, &nk);
        if (r == INF_DOOR_UNLOCKED) res = INF_DOOR_UNLOCKED;
        else if (r == INF_DOOR_LOCKED && res != INF_DOOR_UNLOCKED) {
            res = INF_DOOR_LOCKED; if (needed_key) *needed_key = nk;
        } else if (r == INF_DOOR_OPENED && res == INF_DOOR_NONE) res = INF_DOOR_OPENED;
    }
    return res;
}

InfDoorResult inf_nudge_door_ray(InfSystem *inf, const LvtLevel *level,
                                 f32 ex, f32 ez, f32 ey,
                                 f32 dx, f32 dz, f32 dy, f32 reach,
                                 u32 have_keys, int *needed_key) {
    /* Locate the starting sector (2D). */
    int cur = -1;
    for (u32 si = 0; si < level->sector_count; si++)
        if (inf_point_in_sector(&level->sectors[si], ex, ez)) { cur = (i32)si; break; }
    if (cur < 0) return INF_DOOR_NONE;

    f32 rlen = sqrtf(dx*dx + dz*dz);
    if (rlen < 1e-4f) return INF_DOOR_NONE;

    f32 cx = ex, cz = ez;
    f32 travelled = 0.0f;
    /* Walk the ray through portals; at each wall crossing, try to nudge the
     * wall's own sector and its adjoin/dadjoin (a door leaf is a thin sector
     * adjoined to the room). Stop at the first door actually nudged, or when
     * the ray hits a solid wall or exceeds `reach`. */
    for (int step = 0; step < 64 && travelled < reach; step++) {
        const LvtSector *sec = &level->sectors[cur];
        f32 best_t = 1e30f; i32 best_wi = -1;
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            const LvtWall *w = &sec->walls[wi];
            if (w->v1 < 0 || w->v2 < 0) continue;
            f32 ax = sec->vertices[w->v1].x, az = sec->vertices[w->v1].y;
            f32 bx = sec->vertices[w->v2].x, bz = sec->vertices[w->v2].y;
            f32 edx = bx - ax, edz = bz - az;
            f32 den = dx * edz - dz * edx;
            if (fabsf(den) < 1e-8f) continue;
            f32 t = ((ax - cx) * edz - (az - cz) * edx) / den;
            f32 u = ((ax - cx) * dz - (az - cz) * dx) / den;
            if (t < 1e-4f || u < -0.001f || u > 1.001f) continue;
            if (t < best_t) { best_t = t; best_wi = (i32)wi; }
        }
        if (best_wi < 0) break;
        travelled = best_t * rlen;
        if (travelled > reach) break;
        const LvtWall *bw = &sec->walls[best_wi];

        /* Try to nudge a door on this wall's own sector, its adjoin, or dadjoin
         * — whichever hosts an INF door. */
        int candidates[3] = { cur, bw->adjoin, bw->dadjoin };
        for (int c = 0; c < 3; c++) {
            if (candidates[c] < 0 || candidates[c] >= (i32)level->sector_count) continue;
            InfDoorResult r = inf_nudge_door_group(inf, level, (u32)candidates[c],
                                                   have_keys, needed_key);
            if (r != INF_DOOR_NONE) return r;
        }

        /* Continue through open portals only; a solid wall stops the reach. */
        if (bw->adjoin < 0 || bw->adjoin >= (i32)level->sector_count) break;
        /* Height gate: the ray must clear the portal opening. */
        const LvtSector *adj = &level->sectors[bw->adjoin];
        f32 pf = (sec->floor_y > adj->floor_y) ? sec->floor_y : adj->floor_y;
        f32 pc = (sec->ceil_y  < adj->ceil_y)  ? sec->ceil_y  : adj->ceil_y;
        f32 yat = ey + dy * best_t * rlen / (rlen > 1e-4f ? 1.0f : 1.0f);
        (void)yat; (void)pf; (void)pc;   /* height gate optional for USE */
        cx = ex + dx * best_t;
        cz = ez + dz * best_t;
        cur = bw->adjoin;
    }
    return INF_DOOR_NONE;
}

/* Apply a message to every elevator controlling `target_sector`. */
void inf_send_message(InfSystem *inf, i32 target_sector,
                      InfMsgType msg, i32 param) {
    if (target_sector < 0) return;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active || el->sector_idx != (u32)target_sector) continue;
        if (el->type == ELEV_TYPE_EXPLODE) {
            /* One-shot detonation — signal the game loop and disable. */
            inf->pending_explode_sector = (i32)el->sector_idx;
            el->just_triggered = true;
            el->active = false;
            continue;
        }
        switch (msg) {
        case INF_MSG_MASTER_ON:  el->master = true;  break;
        case INF_MSG_MASTER_OFF: el->master = false; break;
        case INF_MSG_GOTO_STOP:
            if (el->master && el->stop_count > 0 && param >= 0 &&
                param < (i32)el->stop_count && !el->moving) {
                el->next_stop = (u32)param;
                el->target_y  = el->stops[param].y;
                el->moving = true; el->just_triggered = true;
            }
            break;
        case INF_MSG_PREV_STOP:
            if (el->master && el->stop_count > 0 && !el->moving) {
                el->next_stop = (el->current_stop == 0)
                                ? el->stop_count - 1 : el->current_stop - 1;
                el->target_y  = el->stops[el->next_stop].y;
                el->moving = true; el->just_triggered = true;
            }
            break;
        case INF_MSG_NEXT_STOP:
        default:
            if (el->master && el->stop_count > 0 && !el->moving) {
                el->next_stop = (el->current_stop + 1) % el->stop_count;
                el->target_y  = el->stops[el->next_stop].y;
                el->moving = true; el->just_triggered = true;
            }
            break;
        }
        if (el->just_triggered) inf_move_slaves(inf, el);
    }
}

/* Resolve each LINE trigger's segment endpoints from its wall NUM hash. */
void inf_resolve_lines(InfSystem *inf, const LvtLevel *level) {
    for (u32 i = 0; i < inf->trigger_count; i++) {
        InfTrigger *tr = &inf->triggers[i];
        if (!tr->is_line || tr->line_resolved || tr->line_id == 0) continue;
        for (u32 si = 0; si < level->sector_count && !tr->line_resolved; si++) {
            const LvtSector *s = &level->sectors[si];
            for (u32 wi = 0; wi < s->wall_count; wi++) {
                const LvtWall *w = &s->walls[wi];
                if (w->id != tr->line_id) continue;
                if (w->v1 < 0 || w->v2 < 0) continue;
                tr->lx0 = s->vertices[w->v1].x; tr->lz0 = s->vertices[w->v1].y;
                tr->lx1 = s->vertices[w->v2].x; tr->lz1 = s->vertices[w->v2].y;
                tr->line_resolved = true;
                break;
            }
        }
    }
}

/* 2D segment-segment intersection test. */
static bool segs_cross(f32 ax, f32 az, f32 bx, f32 bz,
                       f32 cx, f32 cz, f32 dx, f32 dz) {
    f32 d1x = bx - ax, d1z = bz - az;
    f32 d2x = dx - cx, d2z = dz - cz;
    f32 den = d1x * d2z - d1z * d2x;
    if (fabsf(den) < 1e-9f) return false;
    f32 t = ((cx - ax) * d2z - (cz - az) * d2x) / den;
    f32 u = ((cx - ax) * d1z - (cz - az) * d1x) / den;
    return t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f;
}

static void inf_fire_line(InfSystem *inf, InfTrigger *tr);

void inf_check_line_cross(InfSystem *inf, f32 x0, f32 z0, f32 x1, f32 z1) {
    /* No movement → nothing crossed. */
    if (fabsf(x1 - x0) < 1e-5f && fabsf(z1 - z0) < 1e-5f) return;
    for (u32 i = 0; i < inf->trigger_count; i++) {
        InfTrigger *tr = &inf->triggers[i];
        if (!tr->active || !tr->is_line || !tr->line_resolved) continue;
        if (tr->single && tr->fired_once) continue;
        if (!segs_cross(x0, z0, x1, z1, tr->lx0, tr->lz0, tr->lx1, tr->lz1))
            continue;
        inf_fire_line(inf, tr);
    }
}

/* Fire one line trigger's message (shared by cross + nudge paths). */
static void inf_fire_line(InfSystem *inf, InfTrigger *tr) {
    tr->fired_once = true;
    if (tr->to_system) {
        switch (tr->msg) {
        case INF_MSG_USER_MSG:  inf->pending_user_msg = tr->msg_param; break;
        case INF_MSG_END_LEVEL: inf->pending_end_level = true; break;
        case INF_MSG_SPAWN_LEVEL:
            snprintf(inf->pending_spawn_level, sizeof(inf->pending_spawn_level),
                     "%s", tr->spawn_level);
            snprintf(inf->pending_spawn_start, sizeof(inf->pending_spawn_start),
                     "%s", tr->spawn_start);
            break;
        default: break;
        }
    } else {
        inf_send_message(inf, tr->client_sector, tr->msg, tr->msg_param);
    }
}

bool inf_fire_line_ray(InfSystem *inf, const LvtLevel *level,
                       f32 ex, f32 ez, f32 dx, f32 dz, f32 reach, u32 event_bit) {
    int cur = -1;
    for (u32 si = 0; si < level->sector_count; si++)
        if (inf_point_in_sector(&level->sectors[si], ex, ez)) { cur = (i32)si; break; }
    if (cur < 0) return false;
    f32 rlen = sqrtf(dx*dx + dz*dz);
    if (rlen < 1e-4f) return false;

    f32 cx = ex, cz = ez, travelled = 0.0f;
    bool fired = false;
    for (int step = 0; step < 64 && travelled < reach; step++) {
        const LvtSector *sec = &level->sectors[cur];
        f32 best_t = 1e30f; i32 best_wi = -1;
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            const LvtWall *w = &sec->walls[wi];
            if (w->v1 < 0 || w->v2 < 0) continue;
            f32 ax = sec->vertices[w->v1].x, az = sec->vertices[w->v1].y;
            f32 bx = sec->vertices[w->v2].x, bz = sec->vertices[w->v2].y;
            f32 edx = bx - ax, edz = bz - az;
            f32 den = dx * edz - dz * edx;
            if (fabsf(den) < 1e-8f) continue;
            f32 t = ((ax - cx) * edz - (az - cz) * edx) / den;
            f32 u = ((ax - cx) * dz - (az - cz) * dx) / den;
            if (t < 1e-4f || u < -0.001f || u > 1.001f) continue;
            if (t < best_t) { best_t = t; best_wi = (i32)wi; }
        }
        if (best_wi < 0) break;
        travelled = best_t * rlen;
        if (travelled > reach) break;
        const LvtWall *bw = &sec->walls[best_wi];

        /* Fire any line trigger attached to this wall (by id) for the given
         * event bit (NUDGE for USE, SHOOT for a bullet). */
        for (u32 i = 0; i < inf->trigger_count; i++) {
            InfTrigger *tr = &inf->triggers[i];
            if (!tr->active || !tr->is_line) continue;
            if (tr->single && tr->fired_once) continue;
            if (tr->line_id != bw->id) continue;
            if (!(tr->event_mask & event_bit)) continue;
            inf_fire_line(inf, tr);
            fired = true;
        }
        if (fired) return true;
        /* Continue through open portals only. */
        if (bw->adjoin < 0 || bw->adjoin >= (i32)level->sector_count) break;
        cx = ex + dx * best_t; cz = ez + dz * best_t;
        cur = bw->adjoin;
    }
    return fired;
}

u32 inf_fire_triggers(InfSystem *inf, u32 sector_idx, u32 event_bits) {
    u32 fired = 0;
    for (u32 i = 0; i < inf->trigger_count; i++) {
        InfTrigger *tr = &inf->triggers[i];
        if (!tr->active || tr->sector_idx != sector_idx) continue;
        if (!(tr->event_mask & event_bits)) continue;
        if (tr->single && tr->fired_once) continue;
        tr->fired_once = true;
        fired++;

        if (tr->to_system) {
            /* SYSTEM messages: objectives / level flow. */
            switch (tr->msg) {
            case INF_MSG_USER_MSG:  inf->pending_user_msg = tr->msg_param; break;
            case INF_MSG_END_LEVEL: inf->pending_end_level = true; break;
            case INF_MSG_SPAWN_LEVEL:
                snprintf(inf->pending_spawn_level, sizeof(inf->pending_spawn_level),
                         "%s", tr->spawn_level);
                snprintf(inf->pending_spawn_start, sizeof(inf->pending_spawn_start),
                         "%s", tr->spawn_start);
                break;
            default: break;
            }
        } else {
            /* Route to the target elevator. */
            inf_send_message(inf, tr->client_sector, tr->msg, tr->msg_param);
        }
    }

    /* Elevator-as-trigger: an ELEVATOR SEQ that also carries EVENT_MASK + a
     * MESSAGE + CLIENT acts as a trigger on its own sector. */
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active || el->sector_idx != sector_idx) continue;
        if (!el->self_msg || !(el->self_event_mask & event_bits)) continue;
        fired++;
        if (el->self_to_system) {
            if (el->self_msg == INF_MSG_USER_MSG)  inf->pending_user_msg = el->self_msg_param;
            else if (el->self_msg == INF_MSG_END_LEVEL) inf->pending_end_level = true;
        } else {
            inf_send_message(inf, el->self_client_sector,
                             (InfMsgType)el->self_msg, el->self_msg_param);
        }
    }
    return fired;
}

/* -------------------------------------------------------------------------
 * Morph door geometry transform
 * ---------------------------------------------------------------------- */
/* Apply a morph door's current position to its sector vertices.
 *   MORPH_MOVE: translate baseline vertices by (current - stop0) along ANGLE.
 *   MORPH_SPIN: rotate baseline vertices around CENTER by (current - stop0) deg.
 * Returns true if the geometry changed. */
static u32 morph_open_stop(const Elevator *el);

static bool apply_morph(const Elevator *el, LvtSector *sec) {
    if (!el->base_captured || el->base_vert_count == 0) return false;
    f32 base = (el->stop_count > 0) ? el->stops[0].y : 0.0f;
    f32 delta = el->current_y - base;
    u32 n = el->base_vert_count;
    if (n > sec->vertex_count) n = sec->vertex_count;

    if (el->type == ELEV_TYPE_MORPH_MOVE) {
        /* Jedi/Outlaws angle convention: 0°=+Z (north), 90°=+X (east), measured
         * clockwise — so the travel direction is (sin, cos), NOT (cos, sin).
         * (Matches Dark Forces' moving-wall math: delta.x=sin·d, delta.z=cos·d.)
         * The old (cos,sin) rotated every slide 90°, so a door authored to slide
         * left/right along its length instead crept along its thin axis — i.e.
         * toward/away from the player (TRAIN's sliding doors "receding"). */
        f32 a  = el->angle_deg * (3.14159265f / 180.0f);
        f32 dx = sinf(a) * delta;
        f32 dz = cosf(a) * delta;
        for (u32 v = 0; v < n; v++) {
            sec->vertices[v].x = el->base_verts[v].x + dx;
            sec->vertices[v].y = el->base_verts[v].y + dz; /* Vec2.y = LVT Z */
        }
    } else { /* ELEV_TYPE_MORPH_SPIN */
        f32 a = delta * (3.14159265f / 180.0f);
        f32 ca = cosf(a), sa = sinf(a);
        for (u32 v = 0; v < n; v++) {
            f32 rx = el->base_verts[v].x - el->center_x;
            f32 rz = el->base_verts[v].y - el->center_z;
            sec->vertices[v].x = el->center_x + rx * ca - rz * sa;
            sec->vertices[v].y = el->center_z + rx * sa + rz * ca;
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Update
 * ---------------------------------------------------------------------- */
void inf_update(InfSystem *inf, f32 dt, LvtLevel *level) {
    inf->dirty = false;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active) continue;
        el->just_triggered = false; /* Reset each frame */

        /* Scroll elevators: accumulate UV offset continuously, no geometry change.
         * Same Jedi/Outlaws angle convention as morph moves (0°=+Z, 90°=+X →
         * dir=(sin,cos)); floor UV is u=worldX/64, v=worldZ/64. TRAIN's moving
         * ground is SCROLL_FLOOR ANGLE 0 → it must scroll along Z (the train's
         * length = forward), i.e. into V. The old (cos,sin) scrolled U (world X),
         * so the floor crept sideways instead of rushing forward; SCROLL_WALL
         * (ANGLE 90/270) likewise scrolled vertically instead of along the wall. */
        if (el->type == ELEV_TYPE_SCROLL_FLOOR ||
            el->type == ELEV_TYPE_SCROLL_WALL) {
            f32 a = el->angle_deg * (3.14159265f / 180.0f);
            f32 rate = 1.0f / 64.0f;
            el->scroll_u += sinf(a) * el->speed * rate * dt;
            el->scroll_v += cosf(a) * el->speed * rate * dt;
            continue;  /* No sector geometry modification */
        }
        /* VELOCITY_Z: advance through stops to cycle velocity value, no geometry change */
        if (el->type == ELEV_TYPE_VELOCITY_Z) {
            if (!el->moving) {
                /* Auto-start cycling on level load */
                if (el->stop_count > 0 && el->delay_timer > 0) {
                    el->delay_timer -= dt;
                    if (el->delay_timer <= 0) {
                        el->next_stop = (el->current_stop + 1) % el->stop_count;
                        el->current_y = el->stops[el->next_stop].y;
                        el->current_stop = el->next_stop;
                        el->delay_timer  = el->stops[el->current_stop].delay;
                    }
                } else if (el->stop_count > 0) {
                    el->current_y = el->stops[0].y;
                    el->delay_timer = el->stops[0].delay;
                }
            }
            continue;  /* No sector geometry modification */
        }

        /* Non-moving: count down the current stop's delay and auto-advance when
         * it expires. This drives oscillating doors that auto-close, looping
         * platforms that cycle forever, and flickering CHANGE_LIGHT — anything
         * with numeric STOP delays. A HOLD delay (stored as 1e9) never expires,
         * so the elevator simply waits for an external trigger/nudge. */
        if (!el->moving) {
            f32 d = el->stops[el->current_stop].delay;
            /* TERMINATE (stored as -1): self-destruct at this stop. */
            if (d < 0.0f) { el->active = false; continue; }
            /* Numeric delay (including 0) auto-advances to the next stop; HOLD
             * (1e9) waits for a trigger. Only multi-stop elevators cycle. */
            if (el->stop_count >= 2 && d < 1e8f) {
                el->delay_timer -= dt;
                if (el->delay_timer <= 0.0f) {
                    el->next_stop = (el->current_stop + 1 < el->stop_count)
                                    ? el->current_stop + 1 : 0;
                    el->target_y  = el->stops[el->next_stop].y;
                    el->moving = true; el->just_triggered = true;
                }
            }
            if (!el->moving) continue;
        }
        if (el->stop_count == 0) continue;
        if (el->sector_idx >= level->sector_count) continue;

        f32 target = el->stops[el->next_stop].y;
        f32 diff   = target - el->current_y;
        f32 step   = el->speed * dt;

        /* SPEED 0 = instantaneous (a message-relay controller like TRAIN's
         * LOADED*DUMMY: it snaps to the stop and fires its GOTO_STOP messages
         * the same frame). Without this it never reaches the stop, so it stays
         * `moving` forever and flags the mesh dirty every frame. */
        if (step <= 0.0f || fabsf(diff) <= step) {
            /* Reached stop */
            el->current_y = target;
            el->current_stop = el->next_stop;
            el->delay_timer = el->stops[el->current_stop].delay;
            el->moving = false;
            /* Fire this stop's scripted messages (sequence chaining). */
            ElevStop *rs = &el->stops[el->current_stop];
            for (u32 m = 0; m < rs->msg_count; m++) {
                StopMsg *sm = &rs->msgs[m];
                if (sm->to_system) {
                    if (sm->type == INF_MSG_USER_MSG)  inf->pending_user_msg = sm->param;
                    else if (sm->type == INF_MSG_END_LEVEL) inf->pending_end_level = true;
                } else if (sm->client_sector >= 0) {
                    inf_send_message(inf, sm->client_sector,
                                     (InfMsgType)sm->type, sm->param);
                }
            }
        } else {
            el->current_y += (diff > 0 ? step : -step);
        }
        inf->dirty = true;  /* elevator moved this frame */

        /* Apply to sector */
        LvtSector *sec = &level->sectors[el->sector_idx];
        switch (el->type) {
        case ELEV_TYPE_FLOOR:
        case ELEV_TYPE_MOVE_FLOOR:
            sec->floor_y = el->current_y;
            break;
        case ELEV_TYPE_CEILING:
            sec->ceil_y = el->current_y;
            break;
        case ELEV_TYPE_DOOR:
            /* Door: move ceiling up to max, floor stays */
            sec->ceil_y = el->current_y;
            break;
        case ELEV_TYPE_INV_DOOR:
            sec->floor_y = el->current_y;
            break;
        case ELEV_TYPE_FLAG_DOOR:
            /* current_y is the 0..1 open amount: slide the panel and open the
             * passage (collision uses door_open) once it is ≥ half open. */
            sec->door_slide = el->current_y;
            sec->door_open  = (el->current_y >= 0.5f);
            /* The stacked partner (walk-through leaf under a transom, etc.)
             * opens with the master so the whole doorway clears. */
            if (el->stack_partner >= 0 && el->stack_partner < (i32)level->sector_count) {
                level->sectors[el->stack_partner].door_slide = el->current_y;
                level->sectors[el->stack_partner].door_open  = (el->current_y >= 0.5f);
            }
            break;
        case ELEV_TYPE_MORPH_MOVE:
        case ELEV_TYPE_MORPH_SPIN: {
            apply_morph(el, sec);  /* Translate/rotate sector vertices */
            /* Flag the doorway passable once the leaf has swung >~40% open, so
             * collision lets the player walk through. */
            f32 base = (el->stop_count > 0) ? el->stops[0].y : 0.0f;
            f32 openv = el->stops[morph_open_stop(el)].y;
            f32 span = fabsf(openv - base);
            sec->door_open = (span > 0.01f) &&
                             (fabsf(el->current_y - base) >= span * 0.4f);
            break;
        }
        case ELEV_TYPE_CHANGE_LIGHT:
            /* Animate the sector's ambient light. Stops carry light levels
             * (0..31); current_y interpolates between them. Only rebuild the
             * mesh when the integer light actually changes. */
            {
                i32 lv = (i32)(el->current_y + 0.5f);
                if (lv < 0) lv = 0;
                if (lv > 31) lv = 31;
                if (sec->ambient != lv) { sec->ambient = lv; inf->dirty = true; }
            }
            break;
        case ELEV_TYPE_EXPLODE:
            /* One-shot: signal the game loop, then stop (handled on trigger). */
            break;
        case ELEV_TYPE_SCROLL_FLOOR:
        case ELEV_TYPE_SCROLL_WALL:
        case ELEV_TYPE_VELOCITY_Z:
            break; /* Handled separately before this switch */
        }

        /* (Auto-advance between stops is handled at the top of the loop for
         * non-moving elevators — see the delay countdown above.) */
    }
}

/* Index of the stop furthest (by morph value) from the closed stop 0. */
static u32 morph_open_stop(const Elevator *el) {
    u32 best = 0; f32 bestd = 0.0f;
    f32 base = (el->stop_count > 0) ? el->stops[0].y : 0.0f;
    for (u32 s = 1; s < el->stop_count; s++) {
        f32 d = fabsf(el->stops[s].y - base);
        if (d > bestd) { bestd = d; best = s; }
    }
    return best;
}

void inf_auto_doors(InfSystem *inf, const LvtLevel *level,
                    int player_sector, f32 radius) {
    (void)player_sector;
    bool force = (radius >= 1e5f);   /* --open-doors debug */
    /* Doors are USE-driven (nudge/E), exactly like the original — they are NOT
     * proximity-automatic. The old auto-open/auto-CLOSE here fought the USE path:
     * a door the player opened with E was force-closed again the next frame
     * because they weren't standing inside the door sector, so "no door worked".
     * We now only act under the --open-doors debug override (force every door
     * open); normal play leaves doors entirely to inf_nudge_door. */
    if (!force) return;
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active) continue;
        if (el->type != ELEV_TYPE_MORPH_MOVE && el->type != ELEV_TYPE_MORPH_SPIN)
            continue;
        if (el->sector_idx >= level->sector_count) continue;
        if (el->stop_count < 2) continue;
        if (el->perm_locked) continue;

        u32 want = morph_open_stop(el);
        if (el->next_stop != want || (!el->moving && el->current_stop != want)) {
            el->next_stop = want;
            el->target_y  = el->stops[want].y;
            el->moving    = true;
            if (!el->just_triggered && el->current_stop != want)
                el->just_triggered = true;
        }
    }
}

void inf_trigger(InfSystem *inf, u32 sector_idx) {
    for (u32 i = 0; i < inf->count; i++) {
        Elevator *el = &inf->elevs[i];
        if (!el->active) continue;
        if (el->sector_idx != sector_idx) continue;
        /* Doors are opened by the directional USE ray (inf_nudge_door_ray) and
         * the short proximity fallback — NOT by this blanket per-sector trigger.
         * Otherwise pressing USE inside a train vestibule (which adjoins several
         * door sectors) advanced every one of them → "one door opens them all". */
        if (elev_is_door(el)) continue;
        /* An elevator with fewer than two stops has nowhere to step; stepping it
         * would divide by zero (stop_count 0) — e.g. a stopless controller on the
         * sector adjoining TRAIN's first-wagon back door (the reported crash). */
        if (el->stop_count < 2) continue;

        if (!el->moving) {
            /* Advance to next stop */
            el->next_stop = (el->current_stop + 1) % el->stop_count;
            el->target_y  = el->stops[el->next_stop].y;
            el->moving    = true;
            el->just_triggered = true;
        }
    }
}

bool inf_is_morph_door(const InfSystem *inf, u32 sector_idx) {
    if (!inf) return false;
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (el->active && el->sector_idx == sector_idx &&
            (el->type == ELEV_TYPE_MORPH_SPIN || el->type == ELEV_TYPE_MORPH_MOVE))
            return true;
    }
    return false;
}

const char *inf_door_name_for_sector(const InfSystem *inf, u32 sector_idx,
                                     int *needed_key) {
    if (needed_key) *needed_key = INF_KEY_NONE;
    if (!inf) return NULL;
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (!el->active || el->sector_idx != sector_idx) continue;
        if (el->type != ELEV_TYPE_MORPH_SPIN && el->type != ELEV_TYPE_MORPH_MOVE &&
            el->type != ELEV_TYPE_DOOR && el->type != ELEV_TYPE_INV_DOOR) continue;
        if (needed_key && el->required_key != INF_KEY_NONE && !el->unlocked)
            *needed_key = el->required_key;
        return el->sector_name[0] ? el->sector_name : "door";
    }
    return NULL;
}

bool inf_get_floor(const InfSystem *inf, u32 sector_idx, f32 *floor_y) {
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (el->sector_idx == sector_idx &&
            (el->type == ELEV_TYPE_FLOOR || el->type == ELEV_TYPE_MOVE_FLOOR ||
             el->type == ELEV_TYPE_INV_DOOR)) {
            *floor_y = el->current_y;
            return true;
        }
    }
    return false;
}

bool inf_get_ceil(const InfSystem *inf, u32 sector_idx, f32 *ceil_y) {
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (el->sector_idx == sector_idx &&
            (el->type == ELEV_TYPE_CEILING || el->type == ELEV_TYPE_DOOR)) {
            *ceil_y = el->current_y;
            return true;
        }
    }
    return false;
}

bool inf_get_scroll(const InfSystem *inf, u32 sector_idx, f32 *u, f32 *v) {
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (el->sector_idx == sector_idx && el->type == ELEV_TYPE_SCROLL_FLOOR) {
            *u = el->scroll_u;
            *v = el->scroll_v;
            return true;
        }
    }
    return false;
}

bool inf_is_scroll_floor(const InfSystem *inf, u32 sector_idx) {
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (el->sector_idx == sector_idx && el->type == ELEV_TYPE_SCROLL_FLOOR)
            return true;
    }
    return false;
}
