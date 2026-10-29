#include "savestate.h"

#include "config.h"
#include "core/core.h"
#include "api/luaapi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "lstate.h"
#include "lobject.h"
#include "lfunc.h"
#include "lmem.h"
#include "lgc.h"
#include "ldo.h"

void blip_clear(struct blip_t *buf);

#define SLOT_MAGIC "SLT1"
#define FILE_MAGIC "MPCS"
#define FILE_VERSION 1

enum {
    V_NIL = 1,
    V_FALSE,
    V_TRUE,
    V_INT,
    V_NUM,
    V_STR,
    V_TABLE,
    V_REF,
    V_LFUNC,
    V_CFUNC,
    V_THREAD,
    V_MAIN
};

enum { UP_CLOSED = 1, UP_REF = 2, UP_OPEN = 3 };
enum { C_LIGHT = 1, C_API = 2, C_CLOSURE = 3 };
enum { FRAME_LUA = 1, FRAME_YIELD = 2 };

struct buf {
    uint8_t *p;
    size_t n, cap;
    int failed;
    const char *why;
};

struct idmap {
    const void **key;
    uint32_t *id;
    size_t n, cap;
};

struct open_note {
    uint32_t up_id;
    const TValue *slot;
};

struct span {
    uint32_t id;
    const TValue *base;
    int n;
};

struct up_owner {
    uint32_t up_id;
    uint32_t func_id;
    int n;
    int open;
};

struct writer {
    struct buf b;
    lua_State *L;
    struct idmap objs;
    struct idmap ups;
    struct open_note *opens;
    int nopens, capopens;
    struct span *spans;
    int nspans, capspans;
    uint32_t next_id;
    int depth;
};

struct uvlink {
    uint32_t up_id;
    uint32_t func_id;
    int n;
};

struct reader {
    const uint8_t *p, *end;
    lua_State *L;
    tic_core *core;
    int idtab;
    struct up_owner *owners;
    int nowners, capowners;
    struct uvlink *links;
    int nlinks, caplinks;
    int failed;
    const char *why;
    int depth;
};

struct perm {
    lua_CFunction func;
    char path[96];
};

static lua_State *s_tmpl;
static struct perm *s_perms;
static int s_nperms;
static lua_CFunction s_yield;

static void note_fail(int *failed, const char **why, const char *msg) {
    if (*failed) return;
    *failed = 1;
    *why = msg;
    fprintf(stderr, "savestate: %s\n", msg);
}

static void wfail(struct writer *w, const char *msg) {
    note_fail(&w->b.failed, &w->b.why, msg);
}

static void rfail(struct reader *r, const char *msg) {
    note_fail(&r->failed, &r->why, msg);
}

static int buf_add(struct buf *b, const void *src, size_t n) {
    if (b->failed) return -1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) {
            if (cap > (SIZE_MAX / 2)) {
                b->failed = 1;
                b->why = "out of memory";
                return -1;
            }
            cap *= 2;
        }
        uint8_t *p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            b->why = "out of memory";
            return -1;
        }
        b->p = p;
        b->cap = cap;
    }
    if (n && src) memcpy(b->p + b->n, src, n);
    b->n += n;
    return 0;
}

static void wr_u8(struct buf *b, unsigned v) {
    uint8_t d = (uint8_t)v;
    buf_add(b, &d, 1);
}

static void wr_u32(struct buf *b, uint32_t v) {
    uint8_t d[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    buf_add(b, d, 4);
}

static void wr_u64(struct buf *b, uint64_t v) {
    uint8_t d[8];
    for (int i = 0; i < 8; i++) d[i] = (uint8_t)(v >> (8 * i));
    buf_add(b, d, 8);
}

static void wr_i32(struct buf *b, int32_t v) { wr_u32(b, (uint32_t)v); }

static void wr_bytes(struct buf *b, const void *p, size_t n) {
    wr_u32(b, (uint32_t)n);
    buf_add(b, p, n);
}

static void wr_str(struct buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    wr_bytes(b, s ? s : "", n);
}

static int rd_need(struct reader *r, size_t n) {
    if (r->failed) return -1;
    if ((size_t)(r->end - r->p) < n) {
        rfail(r, "truncated slot");
        return -1;
    }
    return 0;
}

static int rd_u8(struct reader *r, unsigned *o) {
    if (rd_need(r, 1)) return -1;
    *o = *r->p++;
    return 0;
}

static int rd_u32(struct reader *r, uint32_t *o) {
    if (rd_need(r, 4)) return -1;
    *o = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
         ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return 0;
}

static int rd_u64(struct reader *r, uint64_t *o) {
    if (rd_need(r, 8)) return -1;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[i] << (8 * i);
    r->p += 8;
    *o = v;
    return 0;
}

static int rd_i32(struct reader *r, int32_t *o) {
    uint32_t u;
    if (rd_u32(r, &u)) return -1;
    *o = (int32_t)u;
    return 0;
}

static int rd_bytes(struct reader *r, const uint8_t **p, uint32_t *n, uint32_t limit) {
    if (rd_u32(r, n)) return -1;
    if (*n > limit) {
        rfail(r, "value too large");
        return -1;
    }
    if (rd_need(r, *n)) return -1;
    *p = r->p;
    r->p += *n;
    return 0;
}

static int map_find(struct idmap *m, const void *key, uint32_t *id) {
    for (size_t i = 0; i < m->n; i++) {
        if (m->key[i] == key) {
            *id = m->id[i];
            return 1;
        }
    }
    return 0;
}

static int map_add(struct idmap *m, const void *key, uint32_t id) {
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 64;
        const void **k = realloc(m->key, cap * sizeof *k);
        if (!k) return -1;
        m->key = k;
        uint32_t *v = realloc(m->id, cap * sizeof *v);
        if (!v) return -1;
        m->id = v;
        m->cap = cap;
    }
    m->key[m->n] = key;
    m->id[m->n] = id;
    m->n++;
    return 0;
}

static void map_free(struct idmap *m) {
    free(m->key);
    free(m->id);
    m->key = NULL;
    m->id = NULL;
    m->n = m->cap = 0;
}

static int is_light_c(lua_State *L, int idx) {
    if (lua_type(L, idx) != LUA_TFUNCTION || !lua_iscfunction(L, idx)) return 0;
    const char *name = lua_getupvalue(L, idx, 1);
    if (name) lua_pop(L, 1);
    return name == NULL;
}

static int dump_writer(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    return buf_add(ud, p, sz);
}

static uint32_t take_id(struct writer *w) {
    if (w->next_id == UINT32_MAX) {
        wfail(w, "too many values");
        return 0;
    }
    return ++w->next_id;
}

static int id_put(struct reader *r, uint32_t id, int idx) {
    if (r->failed) return -1;
    lua_rawgeti(r->L, LUA_REGISTRYINDEX, r->idtab);
    lua_pushvalue(r->L, idx);
    lua_rawseti(r->L, -2, (lua_Integer)id);
    lua_pop(r->L, 1);
    return 0;
}

static int id_get(struct reader *r, uint32_t id) {
    if (r->failed) return -1;
    lua_rawgeti(r->L, LUA_REGISTRYINDEX, r->idtab);
    lua_rawgeti(r->L, -1, (lua_Integer)id);
    if (lua_isnil(r->L, -1)) {
        lua_pop(r->L, 2);
        rfail(r, "missing value");
        return -1;
    }
    lua_remove(r->L, -2);
    return 0;
}

static int write_value(struct writer *w, int idx);

static int write_upvalues(struct writer *w, int idx) {
    lua_State *L = w->L;
    int nup = 0;
    while (lua_getupvalue(L, idx, nup + 1)) {
        nup++;
        lua_pop(L, 1);
        if (nup > 255) {
            wfail(w, "too many upvalues");
            return -1;
        }
    }
    wr_u8(&w->b, (unsigned)nup);
    for (int n = 1; n <= nup; n++) {
        if (!lua_getupvalue(L, idx, n)) {
            wfail(w, "upvalue disappeared");
            return -1;
        }
        UpVal *uv = (UpVal *)lua_upvalueid(L, idx, n);
        uint32_t id = 0;
        int seen = map_find(&w->ups, uv, &id);
        int open = upisopen(uv);
        if (!seen) {
            id = take_id(w);
            if (map_add(&w->ups, uv, id)) {
                wfail(w, "out of memory");
                lua_pop(L, 1);
                return -1;
            }
            if (open) {
                wr_u8(&w->b, UP_OPEN);
                wr_u32(&w->b, id);
                if (w->nopens == w->capopens) {
                    int cap = w->capopens ? w->capopens * 2 : 16;
                    struct open_note *p = realloc(w->opens, (size_t)cap * sizeof *p);
                    if (!p) {
                        wfail(w, "out of memory");
                        lua_pop(L, 1);
                        return -1;
                    }
                    w->opens = p;
                    w->capopens = cap;
                }
                w->opens[w->nopens].up_id = id;
                w->opens[w->nopens].slot = uv->v;
                w->nopens++;
            } else {
                wr_u8(&w->b, UP_CLOSED);
                wr_u32(&w->b, id);
                if (write_value(w, -1)) {
                    lua_pop(L, 1);
                    return -1;
                }
            }
        } else {
            wr_u8(&w->b, UP_REF);
            wr_u32(&w->b, id);
        }
        lua_pop(L, 1);
        if (w->b.failed) return -1;
    }
    return w->b.failed ? -1 : 0;
}

static int write_cfunc(struct writer *w, int idx) {
    lua_State *L = w->L;
    lua_CFunction f = lua_tocfunction(L, idx);
    const char *api = luaapi_cfunc_name(f);
    int light = is_light_c(L, idx);
    const void *key = light ? (const void *)(((uintptr_t)f) ^ (uintptr_t)1) : lua_topointer(L, idx);
    uint32_t id = 0;
    if (map_find(&w->objs, key, &id)) {
        wr_u8(&w->b, V_REF);
        wr_u32(&w->b, id);
        return w->b.failed ? -1 : 0;
    }
    id = take_id(w);
    if (map_add(&w->objs, key, id)) {
        wfail(w, "out of memory");
        return -1;
    }
    wr_u8(&w->b, V_CFUNC);
    wr_u32(&w->b, id);
    if (api && !light) {
        const char *up = lua_getupvalue(L, idx, 1);
        int ok = up && lua_type(L, -1) == LUA_TLIGHTUSERDATA;
        if (up) lua_pop(L, 1);
        if (ok && lua_getupvalue(L, idx, 2)) {
            lua_pop(L, 1);
            ok = 0;
        }
        if (!ok) {
            wfail(w, "TIC function upvalue changed");
            return -1;
        }
        wr_u8(&w->b, C_API);
        wr_str(&w->b, api);
        return w->b.failed ? -1 : 0;
    }
    const char *path = NULL;
    for (int i = 0; i < s_nperms; i++) {
        if (s_perms[i].func == f) {
            path = s_perms[i].path;
            break;
        }
    }
    if (!path) {
        wfail(w, "unknown C function");
        return -1;
    }
    if (light) {
        wr_u8(&w->b, C_LIGHT);
        wr_str(&w->b, path);
        return w->b.failed ? -1 : 0;
    }
    wr_u8(&w->b, C_CLOSURE);
    wr_str(&w->b, path);
    int nup = 0;
    while (lua_getupvalue(L, idx, nup + 1)) {
        nup++;
        lua_pop(L, 1);
    }
    if (nup > 255) {
        wfail(w, "too many upvalues");
        return -1;
    }
    wr_u8(&w->b, (unsigned)nup);
    for (int n = 1; n <= nup; n++) {
        lua_getupvalue(L, idx, n);
        if (write_value(w, -1)) {
            lua_pop(L, 1);
            return -1;
        }
        lua_pop(L, 1);
    }
    return w->b.failed ? -1 : 0;
}

static int write_thread(struct writer *w, int idx) {
    lua_State *L = w->L;
    lua_State *co = lua_tothread(L, idx);
    if (co == L) {
        wr_u8(&w->b, V_MAIN);
        return 0;
    }
    const void *key = lua_topointer(L, idx);
    uint32_t id = 0;
    if (map_find(&w->objs, key, &id)) {
        wr_u8(&w->b, V_REF);
        wr_u32(&w->b, id);
        return 0;
    }
    id = take_id(w);
    if (map_add(&w->objs, key, id)) {
        wfail(w, "out of memory");
        return -1;
    }
    int status = lua_status(co);
    int max_end = (int)(co->top - co->stack);
    int lua_frames = 0;
    int yield_frame = 0;
    if (status == LUA_YIELD) {
        CallInfo *ci;
        for (ci = co->base_ci.next; ci; ci = ci->next) {
            if (ci->next) {
                if (!isLua(ci)) {
                    wfail(w, "coroutine is inside a C call");
                    return -1;
                }
                lua_frames++;
            } else {
                StkId real = restorestack(co, ci->extra);
                lua_CFunction f = NULL;
                if (ttislcf(real)) f = fvalue(real);
                else if (ttisCclosure(real)) f = clCvalue(real)->f;
                if (isLua(ci) || f != s_yield || ci->u.c.k != NULL) {
                    wfail(w, "coroutine is inside a C call");
                    return -1;
                }
                yield_frame = 1;
            }
            if ((int)(ci->top - co->stack) > max_end)
                max_end = (int)(ci->top - co->stack);
        }
        if (!yield_frame) {
            wfail(w, "coroutine is inside a C call");
            return -1;
        }
    } else if (status == LUA_OK) {
        if (co->ci != &co->base_ci) {
            wfail(w, "coroutine is running");
            return -1;
        }
    } else if (status > 64) {
        wfail(w, "bad coroutine");
        return -1;
    }
    if (max_end < 0 || max_end > 100000) {
        wfail(w, "coroutine stack");
        return -1;
    }
    if (w->nspans == w->capspans) {
        int cap = w->capspans ? w->capspans * 2 : 8;
        struct span *p = realloc(w->spans, (size_t)cap * sizeof *p);
        if (!p) {
            wfail(w, "out of memory");
            return -1;
        }
        w->spans = p;
        w->capspans = cap;
    }
    w->spans[w->nspans].id = id;
    w->spans[w->nspans].base = co->stack;
    w->spans[w->nspans].n = max_end;
    w->nspans++;

    wr_u8(&w->b, V_THREAD);
    wr_u32(&w->b, id);
    wr_u8(&w->b, (unsigned)status);
    wr_u32(&w->b, (uint32_t)max_end);
    if (!lua_checkstack(L, 4)) {
        wfail(w, "stack overflow");
        return -1;
    }
    for (int i = 0; i < max_end; i++) {
        setobj2s(L, L->top, co->stack + i);
        L->top++;
        int rc = write_value(w, -1);
        lua_pop(L, 1);
        if (rc) return -1;
    }
    if (status != LUA_YIELD) {
        wr_u32(&w->b, 0);
        return w->b.failed ? -1 : 0;
    }
    wr_u32(&w->b, (uint32_t)(lua_frames + 1));
    for (CallInfo *ci = co->base_ci.next; ci; ci = ci->next) {
        wr_u32(&w->b, (uint32_t)(ci->func - co->stack));
        wr_u32(&w->b, (uint32_t)(ci->top - co->stack));
        wr_i32(&w->b, ci->nresults);
        wr_u32(&w->b, ci->callstatus);
        if (isLua(ci)) {
            if (!ttisLclosure(ci->func)) {
                wfail(w, "coroutine frame");
                return -1;
            }
            Proto *p = clLvalue(ci->func)->p;
            int pc = (int)(ci->u.l.savedpc - p->code);
            if (pc < 0 || pc > p->sizecode) {
                wfail(w, "coroutine pc");
                return -1;
            }
            wr_u8(&w->b, FRAME_LUA);
            wr_u32(&w->b, (uint32_t)(ci->u.l.base - co->stack));
            wr_i32(&w->b, pc);
        } else {
            wr_u8(&w->b, FRAME_YIELD);
            wr_i32(&w->b, (int32_t)ci->extra);
            wr_i32(&w->b, (int32_t)ci->u.c.old_errfunc);
        }
    }
    return w->b.failed ? -1 : 0;
}

static int write_value(struct writer *w, int idx) {
    if (w->b.failed) return -1;
    lua_State *L = w->L;
    idx = lua_absindex(L, idx);
    if (++w->depth > 256) {
        wfail(w, "value is too deep");
        w->depth--;
        return -1;
    }
    int rc = 0;
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            wr_u8(&w->b, V_NIL);
            break;
        case LUA_TBOOLEAN:
            wr_u8(&w->b, lua_toboolean(L, idx) ? V_TRUE : V_FALSE);
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                wr_u8(&w->b, V_INT);
                wr_u64(&w->b, (uint64_t)lua_tointeger(L, idx));
            } else {
                double n = lua_tonumber(L, idx);
                wr_u8(&w->b, V_NUM);
                buf_add(&w->b, &n, sizeof n);
            }
            break;
        case LUA_TSTRING: {
            size_t n = 0;
            const char *s = lua_tolstring(L, idx, &n);
            wr_u8(&w->b, V_STR);
            if (n > 8 * 1024 * 1024) {
                wfail(w, "string too large");
                rc = -1;
                break;
            }
            wr_bytes(&w->b, s, n);
            break;
        }
        case LUA_TTABLE: {
            const void *key = lua_topointer(L, idx);
            uint32_t id = 0;
            if (map_find(&w->objs, key, &id)) {
                wr_u8(&w->b, V_REF);
                wr_u32(&w->b, id);
                break;
            }
            id = take_id(w);
            if (map_add(&w->objs, key, id)) {
                wfail(w, "out of memory");
                rc = -1;
                break;
            }
            wr_u8(&w->b, V_TABLE);
            wr_u32(&w->b, id);
            uint32_t np = 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                np++;
                lua_pop(L, 1);
                if (np > 1000000) {
                    lua_pop(L, 1);
                    wfail(w, "table too large");
                    rc = -1;
                    break;
                }
            }
            if (rc) break;
            wr_u32(&w->b, np);
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (write_value(w, -2) || write_value(w, -1)) {
                    lua_pop(L, 2);
                    rc = -1;
                    break;
                }
                lua_pop(L, 1);
            }
            if (rc) break;
            if (lua_getmetatable(L, idx)) {
                wr_u8(&w->b, 1);
                rc = write_value(w, -1);
                lua_pop(L, 1);
            } else {
                wr_u8(&w->b, 0);
            }
            break;
        }
        case LUA_TFUNCTION:
            if (lua_iscfunction(L, idx)) {
                rc = write_cfunc(w, idx);
                break;
            }
            {
                const void *key = lua_topointer(L, idx);
                uint32_t id = 0;
                if (map_find(&w->objs, key, &id)) {
                    wr_u8(&w->b, V_REF);
                    wr_u32(&w->b, id);
                    break;
                }
                id = take_id(w);
                if (map_add(&w->objs, key, id)) {
                    wfail(w, "out of memory");
                    rc = -1;
                    break;
                }
                struct buf code = {0};
                lua_pushvalue(L, idx);
                if (lua_dump(L, dump_writer, &code, 0) != 0 || code.failed) {
                    lua_pop(L, 1);
                    free(code.p);
                    wfail(w, "could not dump function");
                    rc = -1;
                    break;
                }
                lua_pop(L, 1);
                wr_u8(&w->b, V_LFUNC);
                wr_u32(&w->b, id);
                wr_bytes(&w->b, code.p, code.n);
                free(code.p);
                rc = write_upvalues(w, idx);
            }
            break;
        case LUA_TTHREAD:
            rc = write_thread(w, idx);
            break;
        default:
            wfail(w, "unsupported value");
            rc = -1;
            break;
    }
    w->depth--;
    return (rc || w->b.failed) ? -1 : 0;
}

static int resolve_opens(struct writer *w) {
    for (int i = 0; i < w->nopens; i++) {
        const TValue *slot = w->opens[i].slot;
        int found = 0;
        for (int s = 0; s < w->nspans; s++) {
            const struct span *sp = &w->spans[s];
            if (slot >= sp->base && slot < sp->base + sp->n) {
                wr_u32(&w->b, w->opens[i].up_id);
                wr_u32(&w->b, sp->id);
                wr_u32(&w->b, (uint32_t)(slot - sp->base));
                found = 1;
                break;
            }
        }
        if (!found) {
            wfail(w, "open upvalue has no stack");
            return -1;
        }
    }
    return w->b.failed ? -1 : 0;
}

static void perm_add(lua_CFunction f, const char *path) {
    if (!f || !path || s_nperms >= 512) return;
    for (int i = 0; i < s_nperms; i++)
        if (s_perms[i].func == f) return;
    struct perm *p = realloc(s_perms, (size_t)(s_nperms + 1) * sizeof *p);
    if (!p) return;
    s_perms = p;
    s_perms[s_nperms].func = f;
    snprintf(s_perms[s_nperms].path, sizeof s_perms[s_nperms].path, "%s", path);
    s_nperms++;
}

struct seen {
    const void **p;
    int n, cap;
};

static int seen_has(struct seen *s, const void *key) {
    for (int i = 0; i < s->n; i++)
        if (s->p[i] == key) return 1;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 32;
        const void **p = realloc(s->p, (size_t)cap * sizeof *p);
        if (!p) return 1;
        s->p = p;
        s->cap = cap;
    }
    s->p[s->n++] = key;
    return 0;
}

static void perm_walk(lua_State *L, int idx, const char *path, struct seen *seen, int depth) {
    if (depth > 8) return;
    idx = lua_absindex(L, idx);
    int t = lua_type(L, idx);
    if (t == LUA_TFUNCTION && lua_iscfunction(L, idx)) {
        perm_add(lua_tocfunction(L, idx), path);
        return;
    }
    if (t != LUA_TTABLE) return;
    if (seen_has(seen, lua_topointer(L, idx))) return;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        char child[96];
        child[0] = 0;
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (k && strlen(path) + strlen(k) + 2 < sizeof child)
                snprintf(child, sizeof child, "%s%s%s", path, path[0] ? "." : "", k);
        } else if (lua_isinteger(L, -2)) {
            snprintf(child, sizeof child, "%s.%lld", path, (long long)lua_tointeger(L, -2));
        }
        if (child[0]) perm_walk(L, -1, child, seen, depth + 1);
        lua_pop(L, 1);
    }
}

static int ensure_perms(void) {
    if (s_tmpl) return 0;
    s_tmpl = luaL_newstate();
    if (!s_tmpl) return -1;
    luaapi_open(s_tmpl);
    lua_getglobal(s_tmpl, "coroutine");
    lua_getfield(s_tmpl, -1, "yield");
    s_yield = lua_tocfunction(s_tmpl, -1);
    lua_pop(s_tmpl, 2);
    struct seen seen = {0};
    lua_pushglobaltable(s_tmpl);
    perm_walk(s_tmpl, -1, "", &seen, 0);
    lua_pop(s_tmpl, 1);
    free(seen.p);
    if (!s_yield) return -1;
    return 0;
}

static int capture_lua(lua_State *L, struct buf *out) {
    if (ensure_perms()) return -1;
    struct writer w = {0};
    w.L = L;
    int top = lua_gettop(L);
    lua_gc(L, LUA_GCSTOP, 0);
    lua_pushglobaltable(L);
    int rc = write_value(&w, -1);
    lua_pop(L, 1);
    if (!rc) {
        wr_u32(&w.b, (uint32_t)w.nopens);
        rc = resolve_opens(&w);
    }
    lua_settop(L, top);
    lua_gc(L, LUA_GCRESTART, 0);
    map_free(&w.objs);
    map_free(&w.ups);
    free(w.opens);
    free(w.spans);
    if (rc || w.b.failed) {
        free(w.b.p);
        return -1;
    }
    *out = w.b;
    return 0;
}

static int read_value(struct reader *r);

static lua_CFunction perm_func(const char *path) {
    for (int i = 0; i < s_nperms; i++)
        if (strcmp(s_perms[i].path, path) == 0) return s_perms[i].func;
    return NULL;
}

static int remember_owner(struct reader *r, uint32_t up, uint32_t func, int n, int open) {
    if (r->nowners == r->capowners) {
        int cap = r->capowners ? r->capowners * 2 : 16;
        struct up_owner *p = realloc(r->owners, (size_t)cap * sizeof *p);
        if (!p) {
            rfail(r, "out of memory");
            return -1;
        }
        r->owners = p;
        r->capowners = cap;
    }
    r->owners[r->nowners++] = (struct up_owner){up, func, n, open};
    return 0;
}

static struct up_owner *find_owner(struct reader *r, uint32_t up) {
    for (int i = 0; i < r->nowners; i++)
        if (r->owners[i].up_id == up) return &r->owners[i];
    return NULL;
}

static int remember_link(struct reader *r, uint32_t up, uint32_t func, int n) {
    if (r->nlinks == r->caplinks) {
        int cap = r->caplinks ? r->caplinks * 2 : 16;
        struct uvlink *p = realloc(r->links, (size_t)cap * sizeof *p);
        if (!p) {
            rfail(r, "out of memory");
            return -1;
        }
        r->links = p;
        r->caplinks = cap;
    }
    r->links[r->nlinks++] = (struct uvlink){up, func, n};
    return 0;
}

static int read_upvalues(struct reader *r, int func_idx, uint32_t func_id) {
    lua_State *L = r->L;
    unsigned nup = 0;
    if (rd_u8(r, &nup)) return -1;
    int loaded = 0;
    while (lua_getupvalue(L, func_idx, loaded + 1)) {
        loaded++;
        lua_pop(L, 1);
    }
    if ((unsigned)loaded != nup) {
        rfail(r, "upvalue count");
        return -1;
    }
    for (unsigned n = 1; n <= nup; n++) {
        unsigned kind = 0;
        uint32_t id = 0;
        if (rd_u8(r, &kind) || rd_u32(r, &id)) return -1;
        if (kind == UP_CLOSED) {
            if (read_value(r)) return -1;
            lua_setupvalue(L, func_idx, (int)n);
            if (remember_owner(r, id, func_id, (int)n, 0)) return -1;
        } else if (kind == UP_REF) {
            struct up_owner *o = find_owner(r, id);
            if (!o) {
                rfail(r, "shared upvalue");
                return -1;
            }
            if (!o->open) {
                if (id_get(r, o->func_id)) return -1;
                lua_pushvalue(L, func_idx);
                lua_upvaluejoin(L, -1, (int)n, -2, o->n);
                lua_pop(L, 2);
            }
        } else if (kind == UP_OPEN) {
            if (remember_owner(r, id, func_id, (int)n, 1)) return -1;
            if (remember_link(r, id, func_id, (int)n - 1)) return -1;
        } else {
            rfail(r, "bad upvalue");
            return -1;
        }
        if (kind == UP_REF) {
            struct up_owner *o = find_owner(r, id);
            if (o && o->open && remember_link(r, id, func_id, (int)n - 1)) return -1;
        }
    }
    return r->failed ? -1 : 0;
}

static int read_thread(struct reader *r, uint32_t id) {
    lua_State *L = r->L;
    unsigned status = 0;
    uint32_t length = 0, nframes = 0;
    if (rd_u8(r, &status) || rd_u32(r, &length)) return -1;
    if (length > 100000 || status > 64) {
        rfail(r, "coroutine stack");
        return -1;
    }
    lua_State *co = lua_newthread(L);
    int co_idx = lua_absindex(L, -1);
    if (id_put(r, id, co_idx)) return -1;
    if (!lua_checkstack(co, (int)length + 300)) {
        rfail(r, "coroutine stack");
        return -1;
    }
    while ((uint32_t)(co->top - co->stack) < length) {
        setnilvalue(co->top);
        co->top++;
    }
    for (uint32_t i = 0; i < length; i++) {
        if (read_value(r)) return -1;
        setobj2s(co, co->stack + i, L->top - 1);
        lua_pop(L, 1);
    }
    co->top = co->stack + length;
    co->base_ci.func = co->stack;
    if (rd_u32(r, &nframes)) return -1;
    if (nframes > 256) {
        rfail(r, "coroutine frames");
        return -1;
    }
    co->ci = &co->base_ci;
    for (uint32_t fi = 0; fi < nframes; fi++) {
        uint32_t func = 0, top = 0, callstatus = 0;
        int32_t nresults = 0;
        unsigned kind = 0;
        if (rd_u32(r, &func) || rd_u32(r, &top) || rd_i32(r, &nresults) ||
            rd_u32(r, &callstatus) || rd_u8(r, &kind))
            return -1;
        if (func >= length || top > length + 1000) {
            rfail(r, "coroutine frame");
            return -1;
        }
        CallInfo *ci = luaE_extendCI(co);
        co->ci = ci;
        ci->func = co->stack + func;
        ci->top = co->stack + top;
        ci->nresults = (short)nresults;
        ci->callstatus = (unsigned short)callstatus;
        if (kind == FRAME_LUA) {
            uint32_t base = 0;
            int32_t pc = 0;
            if (rd_u32(r, &base) || rd_i32(r, &pc)) return -1;
            if (!ttisLclosure(ci->func) || base > length) {
                rfail(r, "coroutine frame");
                return -1;
            }
            Proto *p = clLvalue(ci->func)->p;
            if (pc < 0 || pc > p->sizecode) {
                rfail(r, "coroutine pc");
                return -1;
            }
            ci->u.l.base = co->stack + base;
            ci->u.l.savedpc = p->code + pc;
            ci->extra = 0;
        } else if (kind == FRAME_YIELD) {
            int32_t extra = 0, old_err = 0;
            if (rd_i32(r, &extra) || rd_i32(r, &old_err)) return -1;
            ci->extra = extra;
            ci->u.c.k = NULL;
            ci->u.c.ctx = 0;
            ci->u.c.old_errfunc = old_err;
        } else {
            rfail(r, "coroutine frame");
            return -1;
        }
    }
    if (status == LUA_YIELD)
        co->status = LUA_YIELD;
    else if (status != LUA_OK)
        co->status = (lu_byte)status;
    return 0;
}

static int read_value(struct reader *r) {
    if (r->failed) return -1;
    if (++r->depth > 256) {
        rfail(r, "value is too deep");
        r->depth--;
        return -1;
    }
    lua_State *L = r->L;
    if (!lua_checkstack(L, 8)) {
        rfail(r, "stack overflow");
        r->depth--;
        return -1;
    }
    unsigned tag = 0;
    int rc = 0;
    if (rd_u8(r, &tag)) {
        r->depth--;
        return -1;
    }
    switch (tag) {
        case V_NIL: lua_pushnil(L); break;
        case V_FALSE: lua_pushboolean(L, 0); break;
        case V_TRUE: lua_pushboolean(L, 1); break;
        case V_INT: {
            uint64_t v = 0;
            if (rd_u64(r, &v)) rc = -1;
            else lua_pushinteger(L, (lua_Integer)v);
            break;
        }
        case V_NUM: {
            double n = 0;
            if (rd_need(r, sizeof n)) rc = -1;
            else {
                memcpy(&n, r->p, sizeof n);
                r->p += sizeof n;
                lua_pushnumber(L, n);
            }
            break;
        }
        case V_STR: {
            const uint8_t *p = NULL;
            uint32_t n = 0;
            if (rd_bytes(r, &p, &n, 8 * 1024 * 1024)) rc = -1;
            else lua_pushlstring(L, (const char *)p, n);
            break;
        }
        case V_REF: {
            uint32_t id = 0;
            if (rd_u32(r, &id) || id_get(r, id)) rc = -1;
            break;
        }
        case V_TABLE: {
            uint32_t id = 0, np = 0;
            if (rd_u32(r, &id) || rd_u32(r, &np)) {
                rc = -1;
                break;
            }
            if (np > 1000000) {
                rfail(r, "table too large");
                rc = -1;
                break;
            }
            lua_newtable(L);
            int tidx = lua_absindex(L, -1);
            if (id_put(r, id, tidx)) {
                rc = -1;
                break;
            }
            for (uint32_t i = 0; i < np; i++) {
                if (read_value(r) || read_value(r)) {
                    rc = -1;
                    break;
                }
                lua_rawset(L, tidx);
            }
            if (rc) break;
            unsigned has = 0;
            if (rd_u8(r, &has)) {
                rc = -1;
                break;
            }
            if (has) {
                if (read_value(r)) rc = -1;
                else lua_setmetatable(L, tidx);
            }
            break;
        }
        case V_LFUNC: {
            uint32_t id = 0;
            const uint8_t *code = NULL;
            uint32_t n = 0;
            if (rd_u32(r, &id) || rd_bytes(r, &code, &n, 8 * 1024 * 1024)) {
                rc = -1;
                break;
            }
            if (luaL_loadbuffer(L, (const char *)code, n, "savestate") != LUA_OK) {
                rfail(r, "bad function");
                lua_pop(L, 1);
                rc = -1;
                break;
            }
            int fidx = lua_absindex(L, -1);
            if (id_put(r, id, fidx) || read_upvalues(r, fidx, id)) rc = -1;
            break;
        }
        case V_CFUNC: {
            uint32_t id = 0;
            unsigned kind = 0;
            const uint8_t *name = NULL;
            uint32_t nlen = 0;
            if (rd_u32(r, &id) || rd_u8(r, &kind) || rd_bytes(r, &name, &nlen, 200)) {
                rc = -1;
                break;
            }
            char path[201];
            if (nlen > 200) {
                rfail(r, "bad C function");
                rc = -1;
                break;
            }
            memcpy(path, name, nlen);
            path[nlen] = 0;
            if (kind == C_API) {
                lua_CFunction f = luaapi_cfunc(path);
                if (!f) {
                    rfail(r, "unknown TIC function");
                    rc = -1;
                    break;
                }
                lua_pushlightuserdata(L, r->core);
                lua_pushcclosure(L, f, 1);
            } else if (kind == C_LIGHT || kind == C_CLOSURE) {
                lua_CFunction f = perm_func(path);
                if (!f) {
                    rfail(r, "unknown C function");
                    rc = -1;
                    break;
                }
                if (kind == C_LIGHT) {
                    lua_pushcfunction(L, f);
                } else {
                    unsigned nup = 0;
                    if (rd_u8(r, &nup)) {
                        rc = -1;
                        break;
                    }
                    for (unsigned i = 0; i < nup; i++) {
                        if (read_value(r)) {
                            rc = -1;
                            break;
                        }
                    }
                    if (rc) break;
                    lua_pushcclosure(L, f, (int)nup);
                }
            } else {
                rfail(r, "bad C function");
                rc = -1;
                break;
            }
            if (!rc && id_put(r, id, -1)) rc = -1;
            break;
        }
        case V_THREAD: {
            uint32_t id = 0;
            if (rd_u32(r, &id) || read_thread(r, id)) rc = -1;
            break;
        }
        case V_MAIN:
            lua_pushthread(L);
            break;
        default:
            rfail(r, "bad value");
            rc = -1;
            break;
    }
    r->depth--;
    return (rc || r->failed) ? -1 : 0;
}

static int apply_opens(struct reader *r) {
    uint32_t n = 0;
    if (rd_u32(r, &n)) return -1;
    if (n > 100000) {
        rfail(r, "too many upvalues");
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t up = 0, thread = 0, slot = 0;
        if (rd_u32(r, &up) || rd_u32(r, &thread) || rd_u32(r, &slot)) return -1;
        if (id_get(r, thread)) return -1;
        lua_State *co = lua_tothread(r->L, -1);
        if (!co || slot >= (uint32_t)(co->top - co->stack)) {
            lua_pop(r->L, 1);
            rfail(r, "open upvalue");
            return -1;
        }
        UpVal *uv = luaF_findupval(co, co->stack + slot);
        int linked = 0;
        for (int k = 0; k < r->nlinks; k++) {
            if (r->links[k].up_id != up) continue;
            if (id_get(r, r->links[k].func_id)) {
                lua_pop(r->L, 1);
                return -1;
            }
            if (!lua_isfunction(r->L, -1) || lua_iscfunction(r->L, -1)) {
                lua_pop(r->L, 2);
                rfail(r, "open upvalue");
                return -1;
            }
            LClosure *cl = clLvalue(r->L->top - 1);
            int nup = r->links[k].n;
            if (nup < 0 || nup >= cl->nupvalues) {
                lua_pop(r->L, 2);
                rfail(r, "open upvalue");
                return -1;
            }
            UpVal *old = cl->upvals[nup];
            if (old != uv) {
                uv->refcount++;
                cl->upvals[nup] = uv;
                if (old) {
                    old->refcount--;
                    if (old->refcount == 0 && !upisopen(old))
                        luaM_free(co, old);
                }
            }
            lua_pop(r->L, 1);
            linked++;
        }
        lua_pop(r->L, 1);
        if (!linked) {
            rfail(r, "open upvalue");
            return -1;
        }
    }
    return r->failed ? -1 : 0;
}

static int restore_lua(tic_core *core, const uint8_t *p, size_t n) {
    lua_State *L = core->currentVM;
    if (!L) return -1;
    if (ensure_perms()) return -1;
    struct reader r = {0};
    r.p = p;
    r.end = p + n;
    r.L = L;
    r.core = core;
    int top = lua_gettop(L);
    lua_gc(L, LUA_GCSTOP, 0);
    lua_newtable(L);
    r.idtab = luaL_ref(L, LUA_REGISTRYINDEX);
    int rc = read_value(&r);
    if (!rc) rc = apply_opens(&r);
    if (!rc && r.p != r.end) {
        rfail(&r, "trailing bytes");
        rc = -1;
    }
    if (!rc) {
        if (!lua_istable(L, -1)) {
            rfail(&r, "root is not a table");
            rc = -1;
        } else {
            lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        }
    }
    luaL_unref(L, LUA_REGISTRYINDEX, r.idtab);
    lua_settop(L, top);
    lua_gc(L, LUA_GCRESTART, 0);
    free(r.owners);
    free(r.links);
    return rc;
}

static int ptr_off(const void *p, const void *base, size_t n, int32_t *out) {
    if (!p) {
        *out = -1;
        return 0;
    }
    const char *c = p;
    const char *b = base;
    if (c < b || (size_t)(c - b) >= n) return -1;
    *out = (int32_t)(c - b);
    return 0;
}

static int off_ptr(int32_t off, void *base, size_t n, void **out) {
    if (off < 0) {
        *out = NULL;
        return 0;
    }
    if ((size_t)off >= n) return -1;
    *out = (char *)base + off;
    return 0;
}

static uint64_t hash_bytes(const void *data, size_t n) {
    const uint8_t *p = data;
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int capture(struct tic_runner *r, uint8_t **out, size_t *out_n) {
    tic_core *core = (tic_core *)r->tic;
    if (!core || !core->memory.ram || !core->currentVM) return -1;
    tic_core_state_data st = core->state;
    int32_t sfx_pos[TIC_SOUND_CHANNELS];
    int32_t music_pos[TIC_SOUND_CHANNELS];
    int32_t delay_row[TIC_SOUND_CHANNELS];
    for (int i = 0; i < TIC_SOUND_CHANNELS; i++) {
        if (ptr_off(st.sfx.channels[i].pos, core->memory.ram, sizeof(tic_ram), &sfx_pos[i]))
            return -1;
        if (ptr_off(core->state.music.channels[i].pos, &core->state, sizeof core->state, &music_pos[i]))
            return -1;
        if (ptr_off(st.music.commands[i].delay.row, core->memory.ram, sizeof(tic_ram), &delay_row[i]))
            return -1;
        st.sfx.channels[i].pos = NULL;
        st.music.channels[i].pos = NULL;
        st.music.commands[i].delay.row = NULL;
    }
    st.tick = NULL;
    memset(&st.callback, 0, sizeof st.callback);

    struct buf lua = {0};
    if (capture_lua(core->currentVM, &lua)) return -1;

    struct buf b = {0};
    buf_add(&b, SLOT_MAGIC, 4);
    wr_u32(&b, (uint32_t)sizeof(tic_ram));
    buf_add(&b, core->memory.ram, sizeof(tic_ram));
    wr_u32(&b, (uint32_t)sizeof st);
    buf_add(&b, &st, sizeof st);
    for (int i = 0; i < TIC_SOUND_CHANNELS; i++) {
        wr_i32(&b, sfx_pos[i]);
        wr_i32(&b, music_pos[i]);
        wr_i32(&b, delay_row[i]);
    }
    wr_bytes(&b, lua.p, lua.n);
    free(lua.p);
    if (b.failed) {
        free(b.p);
        return -1;
    }
    *out = b.p;
    *out_n = b.n;
    return 0;
}

static int apply_slot(struct tic_runner *r, const uint8_t *p, size_t n) {
    tic_core *core = (tic_core *)r->tic;
    if (!core || !core->memory.ram || n < 4 || memcmp(p, SLOT_MAGIC, 4) != 0) return -1;
    struct reader rd = {0};
    rd.p = p + 4;
    rd.end = p + n;
    uint32_t ram_n = 0, state_n = 0;
    if (rd_u32(&rd, &ram_n)) return -1;
    if (ram_n != sizeof(tic_ram) || rd_need(&rd, ram_n)) return -1;
    const uint8_t *ram = rd.p;
    rd.p += ram_n;
    if (rd_u32(&rd, &state_n)) return -1;
    if (state_n != sizeof(tic_core_state_data) || rd_need(&rd, state_n)) return -1;
    const uint8_t *state = rd.p;
    rd.p += state_n;
    int32_t sfx_pos[TIC_SOUND_CHANNELS], music_pos[TIC_SOUND_CHANNELS], delay_row[TIC_SOUND_CHANNELS];
    for (int i = 0; i < TIC_SOUND_CHANNELS; i++) {
        if (rd_i32(&rd, &sfx_pos[i]) || rd_i32(&rd, &music_pos[i]) || rd_i32(&rd, &delay_row[i]))
            return -1;
        if (sfx_pos[i] >= (int32_t)sizeof(tic_ram) || music_pos[i] >= (int32_t)sizeof(tic_core_state_data) ||
            delay_row[i] >= (int32_t)sizeof(tic_ram))
            return -1;
    }
    const uint8_t *lua = NULL;
    uint32_t lua_n = 0;
    if (rd_bytes(&rd, &lua, &lua_n, 32 * 1024 * 1024)) return -1;
    if (rd.p != rd.end) return -1;

    tic_tick tick = core->state.tick;
    tic_blit_callback callback = core->state.callback;
    if (restore_lua(core, lua, lua_n)) return -1;

    memcpy(core->memory.ram, ram, sizeof(tic_ram));
    memcpy(&core->state, state, sizeof core->state);
    core->state.tick = tick;
    core->state.callback = callback;
    for (int i = 0; i < TIC_SOUND_CHANNELS; i++) {
        void *ptr = NULL;
        if (off_ptr(sfx_pos[i], core->memory.ram, sizeof(tic_ram), &ptr)) return -1;
        core->state.sfx.channels[i].pos = ptr;
        if (off_ptr(music_pos[i], &core->state, sizeof core->state, &ptr)) return -1;
        core->state.music.channels[i].pos = ptr;
        if (off_ptr(delay_row[i], core->memory.ram, sizeof(tic_ram), &ptr)) return -1;
        core->state.music.commands[i].delay.row = ptr;
    }
    if (core->blip.left) blip_clear(core->blip.left);
    if (core->blip.right) blip_clear(core->blip.right);
    r->sound_pos = 0;
    r->sound_left = 0;
    return 0;
}

struct slot {
    uint8_t *data;
    size_t n;
};

struct savestate {
    struct slot slots[SAVESTATE_SLOTS];
    int current;
    int bound;
    const uint8_t *cart;
    int cart_n;
    uint64_t hash;
    char path[CONFIG_PATH_MAX];
};

static void clear_slots(struct savestate *s) {
    for (int i = 0; i < SAVESTATE_SLOTS; i++) {
        free(s->slots[i].data);
        s->slots[i].data = NULL;
        s->slots[i].n = 0;
    }
}

static int state_file_name(const char *cart_path, char *out, size_t n) {
    const char *base = cart_path ? cart_path : "";
    const char *slash = strrchr(base, '/');
    if (slash && slash[1]) base = slash + 1;
    char stem[160];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) *dot = '\0';
    if (!stem[0] || strcmp(stem, ".") == 0 || strcmp(stem, "..") == 0)
        snprintf(stem, sizeof stem, "cart");
    for (char *p = stem; *p; p++) {
        if (*p == '/' || *p == '\\') *p = '_';
    }
    char file[200];
    snprintf(file, sizeof file, "states-%s.bin", stem);
    return config_make_path(out, n, file);
}

static int write_file(const struct savestate *s) {
    if (!s->path[0]) return -1;
    FILE *f = fopen(s->path, "wb");
    if (!f) return -1;
    uint8_t hdr[4 + 4 + 8 + 4];
    memcpy(hdr, FILE_MAGIC, 4);
    uint32_t ver = FILE_VERSION;
    memcpy(hdr + 4, &ver, 4);
    memcpy(hdr + 8, &s->hash, 8);
    uint32_t slot = (uint32_t)s->current;
    memcpy(hdr + 16, &slot, 4);
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    for (int i = 0; ok && i < SAVESTATE_SLOTS; i++) {
        uint32_t n = (uint32_t)s->slots[i].n;
        ok = fwrite(&n, 4, 1, f) == 1;
        if (ok && n) ok = fwrite(s->slots[i].data, 1, n, f) == n;
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static void read_file(struct savestate *s) {
    FILE *f = fopen(s->path, "rb");
    if (!f) return;
    uint8_t hdr[4 + 4 + 8 + 4];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, FILE_MAGIC, 4) != 0) {
        fclose(f);
        return;
    }
    uint32_t ver = 0, slot = 0;
    uint64_t hash = 0;
    memcpy(&ver, hdr + 4, 4);
    memcpy(&hash, hdr + 8, 8);
    memcpy(&slot, hdr + 16, 4);
    if (ver != FILE_VERSION || hash != s->hash) {
        fclose(f);
        return;
    }
    if (slot < SAVESTATE_SLOTS) s->current = (int)slot;
    for (int i = 0; i < SAVESTATE_SLOTS; i++) {
        uint32_t n = 0;
        if (fread(&n, 4, 1, f) != 1 || n > 32 * 1024 * 1024) {
            clear_slots(s);
            break;
        }
        if (!n) continue;
        uint8_t *p = malloc(n);
        if (!p || fread(p, 1, n, f) != n) {
            free(p);
            clear_slots(s);
            break;
        }
        s->slots[i].data = p;
        s->slots[i].n = n;
    }
    fclose(f);
}

struct savestate *savestate_create(void) {
    struct savestate *s = calloc(1, sizeof *s);
    return s;
}

void savestate_destroy(struct savestate *s) {
    if (!s) return;
    clear_slots(s);
    free(s);
}

void savestate_bind(struct savestate *s, struct tic_runner *r) {
    if (!s) return;
    const uint8_t *cart = NULL;
    int n = 0;
    const char *path = "";
    if (r && r->is_loaded && r->cart_data && r->cart_size > 0) {
        cart = r->cart_data;
        n = r->cart_size;
        path = r->cart_path;
    }
    if (s->bound && s->cart == cart && s->cart_n == n) return;
    clear_slots(s);
    s->current = 0;
    s->bound = cart != NULL;
    s->cart = cart;
    s->cart_n = n;
    s->hash = cart ? hash_bytes(cart, (size_t)n) : 0;
    s->path[0] = 0;
    if (!cart) return;
    if (state_file_name(path, s->path, sizeof s->path) != 0) {
        s->path[0] = 0;
        return;
    }
    read_file(s);
}

int savestate_slot(const struct savestate *s) {
    return s ? s->current : 0;
}

void savestate_set_slot(struct savestate *s, int slot) {
    if (!s) return;
    if (slot < 0) slot = 0;
    if (slot >= SAVESTATE_SLOTS) slot = SAVESTATE_SLOTS - 1;
    if (s->current == slot) return;
    s->current = slot;
    if (s->bound) write_file(s);
}

int savestate_filled(const struct savestate *s, int slot) {
    if (!s || slot < 0 || slot >= SAVESTATE_SLOTS) return 0;
    return s->slots[slot].data != NULL;
}

unsigned savestate_filled_mask(const struct savestate *s) {
    unsigned m = 0;
    if (!s) return 0;
    for (int i = 0; i < SAVESTATE_SLOTS; i++)
        if (s->slots[i].data) m |= 1u << i;
    return m;
}

void savestate_hud(const struct savestate *s, int choosing, struct save_hud *out) {
    if (!out) return;
    out->slot = savestate_slot(s);
    out->filled = savestate_filled_mask(s);
    out->choosing = choosing ? 1 : 0;
}

int savestate_save(struct savestate *s, struct tic_runner *r) {
    if (!s || !r || !r->is_loaded || !s->bound) return -1;
    uint8_t *blob = NULL;
    size_t n = 0;
    pthread_mutex_lock(&r->sound_lock);
    int rc = capture(r, &blob, &n);
    pthread_mutex_unlock(&r->sound_lock);
    if (rc) return -1;
    int slot = s->current;
    uint8_t *old = s->slots[slot].data;
    size_t old_n = s->slots[slot].n;
    s->slots[slot].data = blob;
    s->slots[slot].n = n;
    if (write_file(s) != 0) {
        s->slots[slot].data = old;
        s->slots[slot].n = old_n;
        free(blob);
        return -1;
    }
    free(old);
    return 0;
}

int savestate_load(struct savestate *s, struct tic_runner *r) {
    if (!s || !r || !r->is_loaded || !s->bound) return -1;
    int slot = s->current;
    if (!s->slots[slot].data) return -1;
    pthread_mutex_lock(&r->sound_lock);
    int rc = apply_slot(r, s->slots[slot].data, s->slots[slot].n);
    pthread_mutex_unlock(&r->sound_lock);
    return rc;
}
