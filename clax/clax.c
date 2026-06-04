/*
 * clax — Clax V2 transpiler
 *
 * Usage:  clax <project-dir>
 *
 * Scans <project-dir> for .cx files. For each class file (one `class Name { ... };`
 * block, file name matches class name), emits Name.h and Name.c. For each program
 * file (no class block; contains plain C code, includes, and possibly int main),
 * emits a corresponding .c. Also emits clax_runtime.h and clax_runtime.c shared
 * by the whole project.
 *
 * Implements Clax-Specification-V2.md: ::, new/delete, this, Object, typeof, clone,
 * NULL safety, hidden header (magic + type tag), generic dispatch of clone/delete
 * on Object, deep clone of class-typed members.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

/* ===== fatal ===== */
static const char* g_current_file = "<?>";
static void die(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "clax: %s: ", g_current_file);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ===== string buffer ===== */
typedef struct { char* s; int n; int cap; } SB;
static void sb_init(SB* b) { b->s = NULL; b->n = 0; b->cap = 0; }
static void sb_ensure(SB* b, int need) {
    if (b->n + need + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + need + 1) nc *= 2;
        b->s = realloc(b->s, nc);
        b->cap = nc;
    }
}
static void sb_putn(SB* b, const char* p, int n) {
    sb_ensure(b, n);
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void sb_puts(SB* b, const char* p) { sb_putn(b, p, (int)strlen(p)); }
static void sb_putc(SB* b, char c) { sb_ensure(b, 1); b->s[b->n++] = c; b->s[b->n] = 0; }
static void sb_printf(SB* b, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    sb_ensure(b, n);
    vsnprintf(b->s + b->n, n + 1, fmt, ap);
    b->n += n;
    va_end(ap);
}
static void sb_free(SB* b) { free(b->s); b->s = NULL; b->n = b->cap = 0; }

/* ===== file IO =====
 * Globals used by write_file; defined here so write_file can see them. */
static bool g_lint_only = false;

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(n + 1);
    size_t r = fread(buf, 1, n, f);
    buf[r] = 0;
    fclose(f);
    return buf;
}
static void write_file(const char* path, const char* data, int n) {
    if (g_lint_only) return;   /* lint mode: validate without emitting. */
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "clax: cannot write %s\n", path); exit(1); }
    fwrite(data, 1, n, f);
    fclose(f);
}

/* ===== string list ===== */
typedef struct { char** v; int n; int cap; } StrList;
static void sl_add(StrList* l, const char* s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, sizeof(char*) * l->cap);
    }
    l->v[l->n++] = strdup(s);
}
static bool sl_has(const StrList* l, const char* s) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->v[i], s) == 0) return true;
    return false;
}

/* ===== global class registry ===== */
static StrList g_classes;

/* ===== include search dirs + output dir ===== */
static StrList g_include_dirs;
static char g_output_dir[1024] = {0};
/* g_lint_only is declared near write_file (see top of file). */

/* ===== input file list ===== */
typedef struct InputFile { char* dir; char* fname; char* base; } InputFile;
static InputFile* g_inputs = NULL;
static int g_inputs_n = 0, g_inputs_cap = 0;

/* ===== tokens ===== */
typedef enum { T_EOF, T_WS, T_COMMENT, T_PP, T_ID, T_NUM, T_STR, T_CHR, T_PUNCT } TokType;
typedef struct { TokType t; char* s; int len; } Tok;
typedef struct { Tok* v; int n; int cap; } Toks;

static void toks_push(Toks* ts, TokType t, const char* p, int n) {
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 128;
        ts->v = realloc(ts->v, sizeof(Tok) * ts->cap);
    }
    Tok* k = &ts->v[ts->n++];
    k->t = t; k->len = n;
    k->s = malloc(n + 1);
    memcpy(k->s, p, n);
    k->s[n] = 0;
}

static int is_idstart(int c) { return isalpha(c) || c == '_'; }
static int is_idcont(int c) { return isalnum(c) || c == '_'; }

static void tokenize(const char* src, Toks* out) {
    const char* p = src;
    while (*p) {
        const char* st = p;
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            toks_push(out, T_COMMENT, st, (int)(p - st));
        } else if (*p == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            toks_push(out, T_COMMENT, st, (int)(p - st));
        } else if (isspace((unsigned char)*p)) {
            while (*p && isspace((unsigned char)*p)) p++;
            toks_push(out, T_WS, st, (int)(p - st));
        } else if (*p == '#') {
            while (*p) {
                if (*p == '\\' && p[1] == '\n') { p += 2; continue; }
                if (*p == '\n') break;
                p++;
            }
            toks_push(out, T_PP, st, (int)(p - st));
        } else if (is_idstart((unsigned char)*p)) {
            while (is_idcont((unsigned char)*p)) p++;
            toks_push(out, T_ID, st, (int)(p - st));
        } else if (isdigit((unsigned char)*p)) {
            while (isalnum((unsigned char)*p) || *p == '.' || *p == '_') p++;
            toks_push(out, T_NUM, st, (int)(p - st));
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (*p == '"') p++;
            toks_push(out, T_STR, st, (int)(p - st));
        } else if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                if (*p == '\\' && p[1]) p += 2; else p++;
            }
            if (*p == '\'') p++;
            toks_push(out, T_CHR, st, (int)(p - st));
        } else {
            const char* multi[] = {"::","->","<<=",">>=","...","<<",">>","<=",">=","==","!=","&&","||","++","--","+=","-=","*=","/=","%=","&=","|=","^=",NULL};
            int m = 0;
            for (int i = 0; multi[i]; i++) {
                int L = (int)strlen(multi[i]);
                if (strncmp(p, multi[i], L) == 0) {
                    toks_push(out, T_PUNCT, p, L);
                    p += L; m = 1; break;
                }
            }
            if (!m) { toks_push(out, T_PUNCT, p, 1); p++; }
        }
    }
    toks_push(out, T_EOF, "", 0);
}

static int nxt(Toks* ts, int i) {
    while (i < ts->n && (ts->v[i].t == T_WS || ts->v[i].t == T_COMMENT)) i++;
    return i;
}
static int teq(Tok* t, const char* s) { return strcmp(t->s, s) == 0; }

/* ===== scope: var name -> class name (or "Object") ===== */
typedef struct Sym { char* name; char* type; } Sym;
typedef struct Scope {
    Sym* v; int n; int cap;
    struct Scope* parent;
} Scope;

static Scope* scope_new(Scope* parent) {
    Scope* s = calloc(1, sizeof(Scope));
    s->parent = parent;
    return s;
}
static void scope_free(Scope* s) {
    if (!s) return;
    for (int i = 0; i < s->n; i++) { free(s->v[i].name); free(s->v[i].type); }
    free(s->v); free(s);
}
static void scope_put(Scope* s, const char* name, const char* type) {
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].name, name) == 0) {
            free(s->v[i].type);
            s->v[i].type = strdup(type);
            return;
        }
    }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, sizeof(Sym) * s->cap);
    }
    s->v[s->n].name = strdup(name);
    s->v[s->n].type = strdup(type);
    s->n++;
}
static const char* scope_lookup(Scope* s, const char* name) {
    for (; s; s = s->parent) {
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->v[i].name, name) == 0) return s->v[i].type;
    }
    return NULL;
}

/* ===== preprocessor line rewrite =====
 * #include "./X.cx"  ->  #include "X.h"
 * #include "X.cx"    ->  #include "X.h"
 * #include <X.cx>    ->  #include "X.h"
 *
 * Any directory prefix in the path is dropped (all generated .h files live in
 * the project's generated_c/ directory).
 */
static void emit_pp(Tok* tk, SB* out) {
    const char* s = tk->s;
    int L = tk->len;
    /* Locate either a "..." or <...> bracket on this line. */
    const char* q1 = NULL;
    char close = 0;
    for (const char* p = s; p < s + L; p++) {
        if (*p == '"') { q1 = p; close = '"'; break; }
        if (*p == '<') { q1 = p; close = '>'; break; }
    }
    if (!q1) { sb_putn(out, s, L); return; }
    const char* q2 = NULL;
    for (const char* p = q1 + 1; p < s + L; p++) {
        if (*p == close) { q2 = p; break; }
    }
    if (!q2) { sb_putn(out, s, L); return; }
    /* Path must end in ".cx". */
    if (q2 - q1 < 4 || strncmp(q2 - 3, ".cx", 3) != 0) {
        sb_putn(out, s, L); return;
    }
    /* Strip any directory prefix. */
    const char* path_start = q1 + 1;
    const char* slash = NULL;
    for (const char* p = path_start; p < q2; p++) if (*p == '/') slash = p;
    const char* name_start = slash ? slash + 1 : path_start;
    int name_len = (int)((q2 - 3) - name_start);

    sb_putn(out, s, (int)(q1 - s));
    sb_putc(out, '"');
    sb_putn(out, name_start, name_len);
    sb_puts(out, ".h");
    sb_putc(out, '"');
    sb_putn(out, q2 + 1, (int)(s + L - (q2 + 1)));
}

/* ===== forward decls ===== */
static void walk(Toks* ts, int start, int end, SB* out, Scope** sc_io, const char* cur_class);

/* Parse class body member-by-member and emit struct (into hdr) and impls (into src). */
static void parse_class_body(Toks* ts, int body_start, int body_end, const char* cname, SB* hdr, SB* src);

/* ===== walk: stream-transform tokens [start..end) into out, threading scope =====
 * sc_io: pointer to current scope; updated on { } nesting.
 * cur_class: when inside a class method body, the class name; else NULL.
 *
 * Recursive calls within a nested ( ... ) or sub-range use the same scope semantics:
 * they don't push a fresh scope (that happens on '{').
 */
static int find_matching(Toks* ts, int i, int end, const char* open, const char* close) {
    int depth = 1;
    int q = i + 1;
    while (q < end && depth > 0) {
        if (ts->v[q].t == T_PUNCT) {
            if (teq(&ts->v[q], open)) depth++;
            else if (teq(&ts->v[q], close)) { depth--; if (depth == 0) return q; }
        }
        q++;
    }
    return end;
}

static void walk(Toks* ts, int start, int end, SB* out, Scope** sc_io, const char* cur_class) {
    /* Pending narrowing: when we emit `if (var::typeof(T))`, on the next `{` we
     * push a scope and bind var -> T. */
    char* narrow_var = NULL;
    char* narrow_cls = NULL;

    for (int i = start; i < end; i++) {
        Tok* tk = &ts->v[i];

        if (tk->t == T_WS) { sb_putn(out, tk->s, tk->len); continue; }
        if (tk->t == T_COMMENT) { sb_putn(out, tk->s, tk->len); continue; }
        if (tk->t == T_PP) { emit_pp(tk, out); continue; }
        if (tk->t == T_STR || tk->t == T_CHR || tk->t == T_NUM) { sb_putn(out, tk->s, tk->len); continue; }
        if (tk->t == T_EOF) break;

        if (tk->t == T_PUNCT) {
            if (teq(tk, "{")) {
                *sc_io = scope_new(*sc_io);
                if (narrow_var) {
                    scope_put(*sc_io, narrow_var, narrow_cls);
                    free(narrow_var); free(narrow_cls);
                    narrow_var = NULL; narrow_cls = NULL;
                }
                sb_putc(out, '{');
                continue;
            }
            if (teq(tk, "}")) {
                Scope* ps = (*sc_io)->parent;
                scope_free(*sc_io);
                *sc_io = ps;
                sb_putc(out, '}');
                continue;
            }
            sb_putn(out, tk->s, tk->len);
            continue;
        }

        /* T_ID */

        /* `Object` keyword -> `void*` (legal anywhere a type appears; Object isn't a
         * valid C identifier in our project anyway). Also, if followed by an
         * identifier and then `,` or `)`, this is a parameter declaration: register
         * the param as Object-typed in the current scope. */
        if (teq(tk, "Object")) {
            sb_puts(out, "void*");
            int j = nxt(ts, i + 1);
            if (j < end && ts->v[j].t == T_ID) {
                /* Lookahead beyond walk-end is fine — ts->v[] always has T_EOF guard. */
                int k = nxt(ts, j + 1);
                if (k < ts->n && ts->v[k].t == T_PUNCT && (teq(&ts->v[k], ",") || teq(&ts->v[k], ")"))) {
                    scope_put(*sc_io, ts->v[j].s, "Object");
                }
            }
            continue;
        }

        /* `delete EXPR;` — if EXPR is a bare identifier whose class is statically
         * known, emit inline destructor + free. Otherwise emit `clax_delete(EXPR)`
         * which dispatches at runtime via the object header.
         *
         * We look ahead to the matching `;` (at the same paren depth) to delimit
         * the expression. */
        if (teq(tk, "delete")) {
            int j = nxt(ts, i + 1);
            /* Try bare-identifier fast path: `delete IDENT ;` */
            if (j < end && ts->v[j].t == T_ID) {
                int k = nxt(ts, j + 1);
                if (k < end && ts->v[k].t == T_PUNCT && teq(&ts->v[k], ";")) {
                    const char* v = ts->v[j].s;
                    const char* cls = scope_lookup(*sc_io, v);
                    if (cls && strcmp(cls, "Object") != 0) {
                        sb_printf(out, "do { if (%s) { %s__dtor(%s); free(%s); } } while (0)",
                                  v, cls, v, v);
                        i = j;
                        continue;
                    }
                }
            }
            /* General path: walk the expression until top-level `;`, wrap with clax_delete(). */
            int q = j;
            int depth_p = 0;
            int expr_end = end;
            while (q < end) {
                if (ts->v[q].t == T_PUNCT) {
                    if (teq(&ts->v[q], "(") || teq(&ts->v[q], "[")) depth_p++;
                    else if (teq(&ts->v[q], ")") || teq(&ts->v[q], "]")) depth_p--;
                    else if (teq(&ts->v[q], ";") && depth_p == 0) { expr_end = q; break; }
                }
                q++;
            }
            sb_puts(out, "clax_delete(");
            walk(ts, j, expr_end, out, sc_io, cur_class);
            sb_putc(out, ')');
            i = expr_end - 1; /* leave the ';' to be emitted next */
            continue;
        }

        /* `new` should only appear as part of a `Class var = new Class(...)` pattern,
         * which is matched below from the leading class-name identifier. A stray
         * `new` is an error. */
        if (teq(tk, "new")) {
            die("'new' is only allowed as `ClassName var = new ClassName(args);`");
        }

        /* `if` looking ahead for `(var::typeof(T))` -> set up narrowing. */
        if (teq(tk, "if")) {
            int j = nxt(ts, i + 1);
            if (j < end && teq(&ts->v[j], "(")) {
                int a = nxt(ts, j + 1);
                if (a < end && ts->v[a].t == T_ID) {
                    int b = nxt(ts, a + 1);
                    if (b < end && teq(&ts->v[b], "::")) {
                        int c = nxt(ts, b + 1);
                        if (c < end && teq(&ts->v[c], "typeof")) {
                            int d = nxt(ts, c + 1);
                            if (d < end && teq(&ts->v[d], "(")) {
                                int e = nxt(ts, d + 1);
                                if (e < end && ts->v[e].t == T_ID) {
                                    int f = nxt(ts, e + 1);
                                    if (f < end && teq(&ts->v[f], ")")) {
                                        int g = nxt(ts, f + 1);
                                        if (g < end && teq(&ts->v[g], ")")) {
                                            free(narrow_var); free(narrow_cls);
                                            narrow_var = strdup(ts->v[a].s);
                                            narrow_cls = strdup(ts->v[e].s);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            /* fall through and emit `if` normally */
        }

        /* Class name in identifier position:
         *   `Class var = new Class(args);`    declaration with `new`
         *   `Class var ,`  or  `Class var )`  parameter declaration
         *   else: just emit the class name (used as a type in cast, etc.)
         */
        if (sl_has(&g_classes, tk->s)) {
            const char* clsname = tk->s;
            int j = nxt(ts, i + 1);
            if (j < end && ts->v[j].t == T_ID) {
                const char* varname = ts->v[j].s;
                int k = nxt(ts, j + 1);
                /* declaration: Class var = new Class(args); */
                if (k < end && teq(&ts->v[k], "=")) {
                    int m = nxt(ts, k + 1);
                    if (m < end && teq(&ts->v[m], "new")) {
                        int nIdx = nxt(ts, m + 1);
                        if (nIdx >= end || ts->v[nIdx].t != T_ID)
                            die("expected class name after 'new'");
                        const char* newcls = ts->v[nIdx].s;
                        if (!sl_has(&g_classes, newcls))
                            die("unknown class in 'new': %s", newcls);
                        int popen = nxt(ts, nIdx + 1);
                        if (popen >= end || !teq(&ts->v[popen], "("))
                            die("expected '(' after class name in 'new'");
                        int pclose = find_matching(ts, popen, end, "(", ")");
                        if (pclose >= end) die("unterminated '(' in 'new'");
                        sb_printf(out, "%s* %s = malloc(sizeof(%s)); %s__ctor(%s",
                                  clsname, varname, newcls, newcls, varname);
                        /* args */
                        int arg_first = nxt(ts, popen + 1);
                        if (arg_first < pclose) {
                            sb_puts(out, ", ");
                            walk(ts, popen + 1, pclose, out, sc_io, cur_class);
                        }
                        sb_putc(out, ')');
                        scope_put(*sc_io, varname, clsname);
                        i = pclose;
                        continue;
                    }
                    /* maybe `Class var = otherExpr;` (e.g., Class var = obj::clone();) */
                    sb_printf(out, "%s* %s", clsname, varname);
                    scope_put(*sc_io, varname, clsname);
                    i = j;
                    continue;
                }
                /* parameter form: Class var , | Class var )  — lookahead may go
                 * past `end` when the param IDENT is the last token in the walked
                 * range; that's fine because ts->v always has a T_EOF guard. */
                if (k < ts->n && ts->v[k].t == T_PUNCT && (teq(&ts->v[k], ",") || teq(&ts->v[k], ")"))) {
                    sb_printf(out, "%s* %s", clsname, varname);
                    scope_put(*sc_io, varname, clsname);
                    i = j;
                    continue;
                }
            }
            /* otherwise: bare class name as a type token (e.g. cast) — emit as-is.
             * Casts to a class type usually want `Class*`, but the user code in
             * Clax shouldn't be casting to bare Class anyway. */
            sb_puts(out, tk->s);
            continue;
        }

        /* General identifier: look for `name :: member` or `this :: member` */
        {
            int j = nxt(ts, i + 1);
            if (j < end && ts->v[j].t == T_PUNCT && teq(&ts->v[j], "::")) {
                int k = nxt(ts, j + 1);
                if (k >= end || ts->v[k].t != T_ID)
                    die("expected identifier after '::'");
                const char* member = ts->v[k].s;
                const char* recv_class = NULL;
                bool is_this = strcmp(tk->s, "this") == 0;
                if (is_this) recv_class = cur_class;
                else recv_class = scope_lookup(*sc_io, tk->s);

                int after = nxt(ts, k + 1);
                bool is_call = (after < end && teq(&ts->v[after], "("));

                /* typeof */
                if (is_call && strcmp(member, "typeof") == 0) {
                    int t_idx = nxt(ts, after + 1);
                    if (t_idx >= end || ts->v[t_idx].t != T_ID)
                        die("typeof() expects a class name argument");
                    const char* T = ts->v[t_idx].s;
                    int close = nxt(ts, t_idx + 1);
                    if (close >= end || !teq(&ts->v[close], ")"))
                        die("typeof: expected ')'");
                    if (!sl_has(&g_classes, T))
                        die("typeof: unknown class %s", T);
                    const char* recv = is_this ? "this" : tk->s;
                    sb_printf(out,
                        "((%s) != NULL && ((ClaxHeader*)(%s))->__magic == CLAX_MAGIC && ((ClaxHeader*)(%s))->__type == CLAX_TYPE_%s)",
                        recv, recv, recv, T);
                    i = close;
                    continue;
                }

                /* clone */
                if (is_call && strcmp(member, "clone") == 0) {
                    int close = nxt(ts, after + 1);
                    if (close >= end || !teq(&ts->v[close], ")"))
                        die("clone: expected ')'");
                    const char* recv = is_this ? "this" : tk->s;
                    if (recv_class && strcmp(recv_class, "Object") != 0) {
                        sb_printf(out, "%s__clone(%s)", recv_class, recv);
                    } else {
                        sb_printf(out, "clax_clone(%s)", recv);
                    }
                    i = close;
                    continue;
                }

                /* method call: obj::method(args) -> Class__method(obj, args) */
                if (is_call) {
                    if (!recv_class)
                        die("'%s' is not a known class-typed variable", tk->s);
                    if (strcmp(recv_class, "Object") == 0)
                        die("method call on non-narrowed Object '%s'", tk->s);
                    const char* recv = is_this ? "this" : tk->s;
                    int close = find_matching(ts, after, end, "(", ")");
                    if (close >= end) die("unterminated '(' in method call");
                    sb_printf(out, "%s__%s(%s", recv_class, member, recv);
                    int arg_first = nxt(ts, after + 1);
                    if (arg_first < close) {
                        sb_puts(out, ", ");
                        walk(ts, after + 1, close, out, sc_io, cur_class);
                    }
                    sb_putc(out, ')');
                    i = close;
                    continue;
                }

                /* member access: obj::x -> obj->x (works for both this and obj) */
                const char* recv = is_this ? "this" : tk->s;
                sb_printf(out, "%s->%s", recv, member);
                i = k;
                continue;
            }
        }

        /* default: emit identifier as-is */
        sb_putn(out, tk->s, tk->len);
    }

    free(narrow_var); free(narrow_cls);
}

/* ===== class body parsing =====
 *
 * Inside `class Name { ... }` the body contains members which are either:
 *   - data:        TYPE-TOKENS  ID [ , ID ]* ;
 *   - constructor: ClassName    (  PARAMS ) { BODY }
 *   - destructor:  ~ClassName   (        ) { BODY }
 *   - method:      TYPE-TOKENS  ID ( PARAMS ) { BODY }
 *
 * Data members may use class types — we add `*` to the emitted struct field.
 * Method/ctor/dtor body is processed through `walk()` so all :: / new / delete
 * transformations apply.
 */
static void emit_param_list(Toks* ts, int popen, int pclose, SB* out, Scope* sc) {
    /* Emit params from tokens (popen+1, pclose), translating `ClassName id` and
     * `Object id`, registering each named param in `sc`. */
    Scope* dummy_parent = NULL;
    /* We pass &sc into walk(); but walk pushes scopes on `{` only. For params we
     * just want to register names. Reuse walk by giving it the param token range. */
    walk(ts, popen + 1, pclose, out, &sc, NULL);
    (void)dummy_parent;
}

/* Emit a class-typed data member: e.g. `MyClass item;` -> `MyClass* item;` */
static void emit_data_member(Toks* ts, int s, int e, SB* hdr) {
    /* The tokens [s..e] cover the declaration tokens (excluding ';').
     * Strategy: find the first TYPE word(s). Determine if the leading type is a
     * class name (then add `*`). Then emit. */
    /* Find the first non-WS, non-comment token */
    int i = nxt(ts, s);
    if (i > e) return;
    bool type_is_class = (ts->v[i].t == T_ID && sl_has(&g_classes, ts->v[i].s));
    sb_puts(hdr, "    ");
    if (type_is_class) {
        sb_puts(hdr, ts->v[i].s);
        sb_puts(hdr, "* ");
        /* emit remainder verbatim (the names) */
        int j = i + 1;
        for (; j <= e; j++) {
            Tok* tk = &ts->v[j];
            if (tk->t == T_WS && j == i + 1) continue; /* skip leading space after type */
            sb_putn(hdr, tk->s, tk->len);
        }
    } else {
        for (int j = s; j <= e; j++) {
            Tok* tk = &ts->v[j];
            sb_putn(hdr, tk->s, tk->len);
        }
    }
    sb_puts(hdr, ";\n");
}

/* Per-class collected info for clone/dtor generation and clax_runtime emission. */
typedef struct ClassMember {
    char* name;
    char* type;       /* class name, or "" for non-class */
} ClassMember;
typedef struct ClassInfo {
    char* name;
    ClassMember* members; int nmem; int cap;
    bool has_user_dtor;
} ClassInfo;

static ClassInfo* g_class_info = NULL;
static int g_class_info_n = 0;
static int g_class_info_cap = 0;

static ClassInfo* class_info_get(const char* name) {
    for (int i = 0; i < g_class_info_n; i++)
        if (strcmp(g_class_info[i].name, name) == 0) return &g_class_info[i];
    if (g_class_info_n == g_class_info_cap) {
        g_class_info_cap = g_class_info_cap ? g_class_info_cap * 2 : 8;
        g_class_info = realloc(g_class_info, sizeof(ClassInfo) * g_class_info_cap);
    }
    ClassInfo* ci = &g_class_info[g_class_info_n++];
    memset(ci, 0, sizeof(*ci));
    ci->name = strdup(name);
    return ci;
}
static void ci_add_member(ClassInfo* ci, const char* name, const char* type) {
    if (ci->nmem == ci->cap) {
        ci->cap = ci->cap ? ci->cap * 2 : 4;
        ci->members = realloc(ci->members, sizeof(ClassMember) * ci->cap);
    }
    ci->members[ci->nmem].name = strdup(name);
    ci->members[ci->nmem].type = strdup(type ? type : "");
    ci->nmem++;
}

/* Parse class body. Emits struct typedef + closing into hdr, function protos
 * after the typedef, and function bodies into src. */
static void parse_class_body(Toks* ts, int body_start, int body_end, const char* cname, SB* hdr, SB* src) {
    ClassInfo* ci = class_info_get(cname);

    /* Struct fields go directly into hdr (between the open and the closing brace
     * we'll emit later). Function prototypes are buffered in `protos` and emitted
     * after the struct typedef closes — otherwise gcc treats them as struct
     * field declarations. */
    SB protos; sb_init(&protos);

    sb_printf(hdr, "typedef struct %s {\n", cname);
    sb_puts(hdr,   "    unsigned int __magic;\n");
    sb_puts(hdr,   "    ClaxType     __type;\n");

    /* Walk top-level members in body. */
    int i = body_start;
    while (i < body_end) {
        /* skip ws/comments */
        if (ts->v[i].t == T_WS || ts->v[i].t == T_COMMENT) {
            /* don't emit into hdr (struct interior); keep struct tidy */
            i++; continue;
        }
        if (ts->v[i].t == T_PP) {
            /* unexpected inside class body */
            i++; continue;
        }
        /* destructor? `~ClassName ( ) { ... }` */
        if (ts->v[i].t == T_PUNCT && teq(&ts->v[i], "~")) {
            int q = nxt(ts, i + 1);
            if (q >= body_end || ts->v[q].t != T_ID || strcmp(ts->v[q].s, cname) != 0)
                die("destructor must be ~%s", cname);
            int popen = nxt(ts, q + 1);
            if (popen >= body_end || !teq(&ts->v[popen], "(")) die("dtor: expected '('");
            int pclose = find_matching(ts, popen, body_end, "(", ")");
            int bopen = nxt(ts, pclose + 1);
            if (bopen >= body_end || !teq(&ts->v[bopen], "{")) die("dtor: expected '{'");
            int bclose = find_matching(ts, bopen, body_end, "{", "}");
            i = bclose + 1;
            ci->has_user_dtor = true;
            continue;
        }
        /* From here we need to scan tokens to decide: data member or function.
         * Walk forward across tokens until we see ';' (data) or '(' (function). */
        int decl_start = i;
        int scan = i;
        int seen_paren = -1, seen_semi = -1;
        while (scan < body_end) {
            Tok* t = &ts->v[scan];
            if (t->t == T_PUNCT) {
                if (teq(t, "(")) { seen_paren = scan; break; }
                if (teq(t, ";")) { seen_semi = scan; break; }
                if (teq(t, "{")) break;
            }
            scan++;
        }
        if (seen_semi >= 0) {
            /* Data member. Tokens [decl_start..seen_semi-1] is the declaration. */
            /* Recognize class-typed member by checking if first non-ws ID is a class. */
            int first = nxt(ts, decl_start);
            int last = seen_semi - 1;
            /* trim trailing ws/comment */
            while (last >= first && (ts->v[last].t == T_WS || ts->v[last].t == T_COMMENT)) last--;
            emit_data_member(ts, decl_start, last, hdr);

            /* record member(s) for clone/dtor generation: parse names from the decl. */
            bool tcls = (ts->v[first].t == T_ID && sl_has(&g_classes, ts->v[first].s));
            const char* clsT = tcls ? ts->v[first].s : "";
            /* Collect names: each ID that immediately precedes `,` or `;` and is not
             * itself the type. We can extract IDs after the first ID. */
            for (int j = first + 1; j < seen_semi; j++) {
                if (ts->v[j].t != T_ID) continue;
                /* Skip identifiers that look like part of the type (e.g., `unsigned int`)
                 * — only the LAST identifier in a run before a punct counts as a name.
                 * Simpler: pick identifiers that immediately follow a non-ID type-modifier-free position. */
                /* Heuristic: an identifier is a name if the next non-ws token is `,` or `;` */
                int k = nxt(ts, j + 1);
                if (k < seen_semi + 1 && (teq(&ts->v[k], ",") || teq(&ts->v[k], ";"))) {
                    ci_add_member(ci, ts->v[j].s, clsT);
                }
            }
            i = seen_semi + 1;
            continue;
        }
        if (seen_paren >= 0) {
            /* function/ctor/method.
             * Decl head = tokens [decl_start..seen_paren-1].
             * Last ID in head is the function name; everything before is return type.
             * Constructor: name == cname and no return type (head has exactly one ID).
             */
            int popen = seen_paren;
            /* Find last non-ws/comment ID before popen */
            int name_idx = -1;
            for (int j = popen - 1; j >= decl_start; j--) {
                if (ts->v[j].t == T_WS || ts->v[j].t == T_COMMENT) continue;
                if (ts->v[j].t == T_ID) { name_idx = j; break; }
                break;
            }
            if (name_idx < 0) die("expected method name");
            const char* fname = ts->v[name_idx].s;
            /* Determine if it's a constructor: name matches class AND no other tokens before it (apart from ws). */
            int prev_idx = name_idx - 1;
            while (prev_idx >= decl_start && (ts->v[prev_idx].t == T_WS || ts->v[prev_idx].t == T_COMMENT)) prev_idx--;
            bool is_ctor = (strcmp(fname, cname) == 0) && (prev_idx < decl_start);
            int pclose = find_matching(ts, popen, body_end, "(", ")");
            int bopen = nxt(ts, pclose + 1);
            if (bopen >= body_end || !teq(&ts->v[bopen], "{")) die("method '%s': expected '{'", fname);
            int bclose = find_matching(ts, bopen, body_end, "{", "}");

            /* Emit prototype to hdr and definition to src. */
            /* Return type tokens: [decl_start..name_idx-1] */
            SB rt; sb_init(&rt);
            if (is_ctor) {
                sb_puts(&rt, "void");
            } else {
                /* Need to translate any class-name in return type to ClassName* and Object to void*.
                 * Simplest: feed through walk() with a throwaway scope. */
                Scope* sc = scope_new(NULL);
                int rt_end = name_idx - 1;
                while (rt_end >= decl_start && (ts->v[rt_end].t == T_WS || ts->v[rt_end].t == T_COMMENT)) rt_end--;
                /* Trim leading ws */
                int rt_start = decl_start;
                while (rt_start <= rt_end && (ts->v[rt_start].t == T_WS || ts->v[rt_start].t == T_COMMENT)) rt_start++;
                /* If the return type is exactly a class name (single ID), emit `Class*`. Else stream. */
                if (rt_start == rt_end && ts->v[rt_start].t == T_ID && sl_has(&g_classes, ts->v[rt_start].s)) {
                    sb_printf(&rt, "%s*", ts->v[rt_start].s);
                } else if (rt_start == rt_end && ts->v[rt_start].t == T_ID && strcmp(ts->v[rt_start].s, "Object") == 0) {
                    sb_puts(&rt, "void*");
                } else {
                    walk(ts, rt_start, rt_end + 1, &rt, &sc, NULL);
                }
                scope_free(sc);
            }

            /* Emit prototype */
            SB params; sb_init(&params);
            /* Build param string by walking tokens between (popen+1) and pclose.
             * Use a scope so we can pre-register names; but we need this scope to be
             * the function's body scope so the body sees them. */
            Scope* fn_scope = scope_new(NULL);
            scope_put(fn_scope, "this", cname);
            /* Translate params */
            Scope* tmp = fn_scope;
            walk(ts, popen + 1, pclose, &params, &tmp, cname);
            /* tmp == fn_scope (no { } inside params normally) */
            (void)tmp;
            /* Header proto: `RT Class__method(Class* this, params);` — buffered
             * in `protos` and flushed AFTER the struct typedef ends. */
            if (is_ctor) {
                sb_printf(&protos, "void %s__ctor(%s* this", cname, cname);
            } else {
                sb_printf(&protos, "%s %s__%s(%s* this", rt.s, cname, fname, cname);
            }
            bool any = false;
            for (int z = 0; z < params.n; z++) if (!isspace((unsigned char)params.s[z])) { any = true; break; }
            if (any) sb_printf(&protos, ", %s", params.s);
            sb_puts(&protos, ");\n");

            /* Definition */
            if (is_ctor) {
                sb_printf(src, "void %s__ctor(%s* this", cname, cname);
            } else {
                sb_printf(src, "%s %s__%s(%s* this", rt.s, cname, fname, cname);
            }
            if (any) sb_printf(src, ", %s", params.s);
            sb_puts(src, ") {\n");
            if (is_ctor) {
                sb_printf(src, "    this->__magic = CLAX_MAGIC;\n");
                sb_printf(src, "    this->__type  = CLAX_TYPE_%s;\n", cname);
                /* Zero class-typed members (NULL) — they're already zero from malloc?
                 * No: malloc doesn't zero. We could use calloc, but we use malloc.
                 * So initialize class-typed members to NULL here. */
                for (int m = 0; m < ci->nmem; m++) {
                    if (ci->members[m].type[0]) {
                        sb_printf(src, "    this->%s = NULL;\n", ci->members[m].name);
                    }
                }
            }
            /* Body content: tokens between bopen+1 and bclose */
            walk(ts, bopen + 1, bclose, src, &fn_scope, cname);
            sb_puts(src, "\n}\n");

            sb_free(&rt);
            sb_free(&params);
            scope_free(fn_scope);
            i = bclose + 1;
            continue;
        }
        /* Unknown; advance to avoid infinite loop */
        i++;
    }

    /* close struct */
    sb_puts(hdr, "} ");
    sb_puts(hdr, cname);
    sb_puts(hdr, ";\n");

    /* Flush buffered protos */
    sb_putn(hdr, protos.s ? protos.s : "", protos.n);

    /* dtor + clone */
    sb_printf(hdr, "void %s__dtor(%s* this);\n", cname, cname);
    sb_printf(hdr, "%s* %s__clone(%s* this);\n", cname, cname, cname);

    sb_free(&protos);
}

/* ===== file kind detection =====
 * A class file has a top-level `class IDENT {` token sequence; the IDENT must
 * match the file's basename (without .cx). Otherwise it's a program file.
 */
static int find_class_decl(Toks* ts, const char* cname_expected, int* out_brace_open, int* out_brace_close) {
    for (int i = 0; i < ts->n; i++) {
        if (ts->v[i].t == T_ID && teq(&ts->v[i], "class")) {
            int j = nxt(ts, i + 1);
            if (j < ts->n && ts->v[j].t == T_ID && teq(&ts->v[j], cname_expected)) {
                int k = nxt(ts, j + 1);
                if (k < ts->n && teq(&ts->v[k], "{")) {
                    int bclose = find_matching(ts, k, ts->n, "{", "}");
                    if (bclose >= ts->n) die("unterminated class body");
                    *out_brace_open = k;
                    *out_brace_close = bclose;
                    return i;
                }
            }
        }
    }
    return -1;
}

/* Look for any top-level `class IDENT` (used to detect that a file declares a class
 * whose name doesn't match its filename, which is an error). Also used in the first
 * pass to collect class names. */
static const char* find_any_class_name(Toks* ts) {
    for (int i = 0; i < ts->n; i++) {
        if (ts->v[i].t == T_ID && teq(&ts->v[i], "class")) {
            int j = nxt(ts, i + 1);
            if (j < ts->n && ts->v[j].t == T_ID) {
                return ts->v[j].s;
            }
        }
    }
    return NULL;
}

/* ===== process one file =====
 * If it's a class file (matching basename), emit basename.h and basename.c.
 * Else, emit basename.c.
 */
static char* basename_no_ext(const char* fname) {
    /* fname is just file name (no dir). Strip .cx */
    char* b = strdup(fname);
    char* dot = strrchr(b, '.');
    if (dot) *dot = 0;
    return b;
}

static void process_file(const char* dir, const char* fname) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char* src = read_file(path);
    if (!src) die("cannot read %s", path);
    g_current_file = fname;

    Toks ts = {0};
    tokenize(src, &ts);

    char* base = basename_no_ext(fname);
    int class_idx = -1, body_open = -1, body_close = -1;
    class_idx = find_class_decl(&ts, base, &body_open, &body_close);

    if (class_idx >= 0) {
        /* class file. */
        /* Emit header */
        SB hdr; sb_init(&hdr);
        SB src_out; sb_init(&src_out);

        sb_printf(&hdr, "#ifndef CLAX__%s_H\n#define CLAX__%s_H\n", base, base);
        sb_puts(&hdr, "#include \"clax_runtime.h\"\n");

        /* Top-level #include lines BEFORE the class block go into the header
         * (they may bring in types referenced by struct members). */
        for (int i = 0; i < class_idx; i++) {
            if (ts.v[i].t == T_PP) emit_pp(&ts.v[i], &hdr);
            if (ts.v[i].t == T_COMMENT) sb_putn(&hdr, ts.v[i].s, ts.v[i].len);
            if (ts.v[i].t == T_WS) sb_putn(&hdr, ts.v[i].s, ts.v[i].len);
        }

        /* Parse class body, emitting struct + protos into hdr, function bodies into src_out. */
        sb_printf(&src_out, "#include \"%s.h\"\n", base);
        sb_puts(&src_out, "#include <stdlib.h>\n");
        sb_puts(&src_out, "#include <string.h>\n");

        parse_class_body(&ts, body_open + 1, body_close, base, &hdr, &src_out);

        /* Trailing class block must be `};` — we accept either `}` then `;` or just `}`. */
        /* Anything AFTER the trailing `};` we emit into the .c (rare). */
        int after = body_close + 1;
        if (after < ts.n && teq(&ts.v[after], ";")) after++;
        Scope* tail_scope = scope_new(NULL);
        walk(&ts, after, ts.n, &src_out, &tail_scope, NULL);
        scope_free(tail_scope);

        /* Emit synthesized dtor wrapper (calls user body if any, then frees
         * class-typed members? No — per spec, the user dtor is responsible for
         * freeing owned class-typed members. We just give an empty default if the
         * user didn't write one. The user-supplied dtor was emitted as
         * Name__dtor by parse_class_body — wait, no: parse_class_body emitted
         * user dtor BODY but didn't emit a function definition because dtor was
         * skipped in the loop. We need to emit the dtor.
         *
         * Re-design: parse_class_body emits ctor + methods. Dtor we handle here.
         */
        /* We deferred dtor — re-scan for `~Name` body and emit it. */
        /* Find ~Name {body} inside class body and emit. */
        ClassInfo* ci = class_info_get(base);
        sb_printf(&src_out, "void %s__dtor(%s* this) {\n", base, base);
        /* find dtor body */
        for (int i = body_open + 1; i < body_close; i++) {
            if (ts.v[i].t == T_PUNCT && teq(&ts.v[i], "~")) {
                int q = nxt(&ts, i + 1);
                if (q < body_close && ts.v[q].t == T_ID && teq(&ts.v[q], base)) {
                    int popen = nxt(&ts, q + 1);
                    int pclose = find_matching(&ts, popen, body_close, "(", ")");
                    int bopen = nxt(&ts, pclose + 1);
                    int bclose = find_matching(&ts, bopen, body_close, "{", "}");
                    Scope* dsc = scope_new(NULL);
                    scope_put(dsc, "this", base);
                    walk(&ts, bopen + 1, bclose, &src_out, &dsc, base);
                    scope_free(dsc);
                    break;
                }
            }
        }
        sb_puts(&src_out, "\n}\n");

        /* Emit clone: deep copy header + C members; recursively clone class members. */
        sb_printf(&src_out, "%s* %s__clone(%s* this) {\n", base, base, base);
        sb_puts(&src_out,   "    if (this == NULL) return NULL;\n");
        sb_printf(&src_out, "    %s* c = malloc(sizeof(%s));\n", base, base);
        sb_puts(&src_out,   "    *c = *this;\n");
        for (int m = 0; m < ci->nmem; m++) {
            if (ci->members[m].type[0]) {
                sb_printf(&src_out, "    c->%s = %s__clone(this->%s);\n",
                          ci->members[m].name, ci->members[m].type, ci->members[m].name);
            }
        }
        sb_puts(&src_out, "    return c;\n}\n");

        sb_puts(&hdr, "#endif\n");

        char outh[1024], outc[1024];
        snprintf(outh, sizeof(outh), "%s/%s.h", g_output_dir, base);
        snprintf(outc, sizeof(outc), "%s/%s.c", g_output_dir, base);
        write_file(outh, hdr.s, hdr.n);
        write_file(outc, src_out.s, src_out.n);

        sb_free(&hdr);
        sb_free(&src_out);
    } else {
        /* program file */
        SB out; sb_init(&out);
        /* If the file declares a class whose name doesn't match the filename, error */
        const char* maybe_cls = find_any_class_name(&ts);
        if (maybe_cls) die("class '%s' declared in file '%s' — name must match", maybe_cls, fname);
        Scope* sc = scope_new(NULL);
        walk(&ts, 0, ts.n, &out, &sc, NULL);
        scope_free(sc);
        char outc[1024];
        snprintf(outc, sizeof(outc), "%s/%s.c", g_output_dir, base);
        write_file(outc, out.s, out.n);
        sb_free(&out);
    }

    free(base);
    free(src);
    for (int i = 0; i < ts.n; i++) free(ts.v[i].s);
    free(ts.v);
}

/* ===== first pass: collect class names by scanning each .cx file ===== */
static void scan_classes(const char* dir, const char* fname) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, fname);
    char* src = read_file(path);
    if (!src) die("cannot read %s", path);
    g_current_file = fname;
    Toks ts = {0};
    tokenize(src, &ts);
    char* base = basename_no_ext(fname);
    int co, cc;
    int idx = find_class_decl(&ts, base, &co, &cc);
    if (idx >= 0) {
        sl_add(&g_classes, base);
    } else {
        /* If there's a class declaration whose name doesn't match the file, error */
        const char* mc = find_any_class_name(&ts);
        if (mc) die("class name '%s' does not match file '%s'", mc, fname);
    }
    free(base);
    free(src);
    for (int i = 0; i < ts.n; i++) free(ts.v[i].s);
    free(ts.v);
}

/* ===== emit clax_runtime.h and clax_runtime.c ===== */
static void emit_runtime(const char* dir) {
    SB h; sb_init(&h);
    sb_puts(&h, "#ifndef CLAX_RUNTIME_H\n#define CLAX_RUNTIME_H\n");
    sb_puts(&h, "#include <stdlib.h>\n#include <stdbool.h>\n");
    /* Project-wide constants — every generated TU picks these up because every
     * generated header includes clax_runtime.h. */
    sb_puts(&h, "#include \"project.h\"\n");
    sb_puts(&h, "#define CLAX_MAGIC 0xC1A55EU\n");
    sb_puts(&h, "typedef enum {\n");
    for (int i = 0; i < g_classes.n; i++) {
        sb_printf(&h, "    CLAX_TYPE_%s,\n", g_classes.v[i]);
    }
    sb_puts(&h, "    CLAX_TYPE__count\n} ClaxType;\n");
    sb_puts(&h, "typedef struct ClaxHeader {\n    unsigned int __magic;\n    ClaxType     __type;\n} ClaxHeader;\n");
    /* Forward decls so the dispatch can use them */
    for (int i = 0; i < g_classes.n; i++) {
        sb_printf(&h, "struct %s; typedef struct %s %s;\n",
                  g_classes.v[i], g_classes.v[i], g_classes.v[i]);
        sb_printf(&h, "void %s__dtor(%s* this);\n", g_classes.v[i], g_classes.v[i]);
        sb_printf(&h, "%s* %s__clone(%s* this);\n",
                  g_classes.v[i], g_classes.v[i], g_classes.v[i]);
    }
    sb_puts(&h, "void clax_delete(void* o);\n");
    sb_puts(&h, "void* clax_clone(void* o);\n");
    sb_puts(&h, "void clax_runtime_error(const char* msg);\n");
    sb_puts(&h, "#endif\n");

    SB c; sb_init(&c);
    sb_puts(&c, "#include \"clax_runtime.h\"\n");
    sb_puts(&c, "#include <stdio.h>\n");
    sb_puts(&c, "#include <stdlib.h>\n");

    sb_puts(&c, "void clax_runtime_error(const char* msg) {\n");
    sb_puts(&c, "    fprintf(stderr, \"clax runtime error: %s\\n\", msg); abort();\n");
    sb_puts(&c, "}\n");

    sb_puts(&c, "static int clax_valid(void* o) {\n");
    sb_puts(&c, "    if (!o) return 0;\n");
    sb_puts(&c, "    ClaxHeader* h = (ClaxHeader*)o;\n");
    sb_puts(&c, "    if (h->__magic != CLAX_MAGIC) { clax_runtime_error(\"non-Clax pointer\"); return 0; }\n");
    sb_puts(&c, "    return 1;\n");
    sb_puts(&c, "}\n");

    sb_puts(&c, "void clax_delete(void* o) {\n");
    sb_puts(&c, "    if (!o) return;\n");
    sb_puts(&c, "    if (!clax_valid(o)) return;\n");
    sb_puts(&c, "    ClaxHeader* h = (ClaxHeader*)o;\n");
    sb_puts(&c, "    switch (h->__type) {\n");
    for (int i = 0; i < g_classes.n; i++) {
        sb_printf(&c, "        case CLAX_TYPE_%s: %s__dtor((%s*)o); free(o); break;\n",
                  g_classes.v[i], g_classes.v[i], g_classes.v[i]);
    }
    sb_puts(&c, "        default: clax_runtime_error(\"unknown type in delete\");\n");
    sb_puts(&c, "    }\n}\n");

    sb_puts(&c, "void* clax_clone(void* o) {\n");
    sb_puts(&c, "    if (!o) return NULL;\n");
    sb_puts(&c, "    if (!clax_valid(o)) return NULL;\n");
    sb_puts(&c, "    ClaxHeader* h = (ClaxHeader*)o;\n");
    sb_puts(&c, "    switch (h->__type) {\n");
    for (int i = 0; i < g_classes.n; i++) {
        sb_printf(&c, "        case CLAX_TYPE_%s: return %s__clone((%s*)o);\n",
                  g_classes.v[i], g_classes.v[i], g_classes.v[i]);
    }
    sb_puts(&c, "        default: clax_runtime_error(\"unknown type in clone\"); return NULL;\n");
    sb_puts(&c, "    }\n}\n");

    char ph[1024], pc[1024];
    snprintf(ph, sizeof(ph), "%s/clax_runtime.h", g_output_dir);
    snprintf(pc, sizeof(pc), "%s/clax_runtime.c", g_output_dir);
    (void)dir;
    write_file(ph, h.s, h.n);
    write_file(pc, c.s, c.n);
    sb_free(&h); sb_free(&c);
}

/* ===== driver ===== */
static int has_suffix(const char* s, const char* suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

/* Has an input file with this `base` already been queued? */
static bool inputs_has_base(const char* base) {
    for (int i = 0; i < g_inputs_n; i++)
        if (strcmp(g_inputs[i].base, base) == 0) return true;
    return false;
}

static void inputs_add(const char* dir, const char* fname) {
    char* base = basename_no_ext(fname);
    if (inputs_has_base(base)) { free(base); return; }
    if (g_inputs_n == g_inputs_cap) {
        g_inputs_cap = g_inputs_cap ? g_inputs_cap * 2 : 16;
        g_inputs = realloc(g_inputs, sizeof(InputFile) * g_inputs_cap);
    }
    g_inputs[g_inputs_n].dir   = strdup(dir);
    g_inputs[g_inputs_n].fname = strdup(fname);
    g_inputs[g_inputs_n].base  = base;
    g_inputs_n++;
}

static bool file_exists(const char* path) {
    struct stat st; return stat(path, &st) == 0;
}

/* Extract `path` and form (angle vs quote) from a `#include` line. Returns 1
 * on a `.cx` include, 0 otherwise. */
static int parse_include_pp(const char* pp, char* out_path, int out_sz, int* is_angle) {
    const char* s = pp;
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '#') return 0;
    s++;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "include", 7) != 0) return 0;
    s += 7;
    while (*s == ' ' || *s == '\t') s++;
    char close;
    if (*s == '"')      { close = '"'; *is_angle = 0; }
    else if (*s == '<') { close = '>'; *is_angle = 1; }
    else return 0;
    s++;
    const char* end = strchr(s, close);
    if (!end) return 0;
    int len = (int)(end - s);
    if (len < 4 || strncmp(end - 3, ".cx", 3) != 0) return 0;
    if (len >= out_sz) return 0;
    memcpy(out_path, s, len);
    out_path[len] = 0;
    return 1;
}

/* Returns the directory in which `path` (containing `name.cx`) was found, or
 * NULL if it could not be resolved. For angle-bracket includes, search the
 * --include directories. For quote includes, look relative to `src_dir`. */
static char* resolve_include(const char* src_dir, const char* path, int is_angle) {
    char buf[1024];
    if (is_angle) {
        for (int i = 0; i < g_include_dirs.n; i++) {
            snprintf(buf, sizeof(buf), "%s/%s", g_include_dirs.v[i], path);
            if (file_exists(buf)) return strdup(g_include_dirs.v[i]);
        }
        return NULL;
    }
    snprintf(buf, sizeof(buf), "%s/%s", src_dir, path);
    if (file_exists(buf)) return strdup(src_dir);
    return NULL;
}

/* For each input file, scan its PP tokens for `.cx` includes and add resolved
 * files to g_inputs. The for-loop intentionally re-evaluates g_inputs_n each
 * iteration so newly added entries are also scanned (transitive discovery). */
static void discover_inputs(void) {
    for (int i = 0; i < g_inputs_n; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", g_inputs[i].dir, g_inputs[i].fname);
        char* src = read_file(path);
        if (!src) { fprintf(stderr, "clax: cannot read %s\n", path); exit(1); }
        Toks ts = {0};
        tokenize(src, &ts);
        for (int k = 0; k < ts.n; k++) {
            if (ts.v[k].t != T_PP) continue;
            char inc[1024];
            int is_angle = 0;
            if (!parse_include_pp(ts.v[k].s, inc, sizeof(inc), &is_angle)) continue;
            /* Strip any directory in the path. */
            const char* sl = strrchr(inc, '/');
            const char* fname_only = sl ? sl + 1 : inc;
            /* basename (without .cx) for dedup */
            char base[256];
            int nl = (int)strlen(fname_only);
            if (nl < 4 || nl >= (int)sizeof(base)) continue;
            memcpy(base, fname_only, nl - 3);
            base[nl - 3] = 0;
            if (inputs_has_base(base)) continue;
            char* dir = resolve_include(g_inputs[i].dir, inc, is_angle);
            if (!dir) {
                fprintf(stderr, "clax: cannot resolve %s%s%s referenced from %s\n",
                        is_angle ? "<" : "\"", inc, is_angle ? ">" : "\"",
                        g_inputs[i].fname);
                exit(1);
            }
            inputs_add(dir, fname_only);
            free(dir);
        }
        free(src);
        for (int q = 0; q < ts.n; q++) free(ts.v[q].s);
        free(ts.v);
    }
}

/* ===== project scaffolding (used by --init-*-project) ===== */

/* Write `content` to `path`. With refuse_existing, error out if it already
 * exists. Returns 0 on success, 1 on failure (message already printed). */
static int scaffold_file(const char* path, const char* content, bool refuse_existing) {
    struct stat st;
    if (refuse_existing && stat(path, &st) == 0) {
        fprintf(stderr, "clax: %s already exists, refusing to overwrite\n", path);
        return 1;
    }
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "clax: cannot write %s\n", path); return 1; }
    fputs(content, f);
    fclose(f);
    return 0;
}

static const char* TPL_SDL2_MAIN_CX =
    "// main.cx — tiny SDL2 program with one clickable Button.\n"
    "#include \"./Button.cx\"\n"
    "#include <SDL2/SDL.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "int main(int argc, char** argv) {\n"
    "    (void)argc; (void)argv;\n"
    "\n"
    "    if (SDL_Init(SDL_INIT_VIDEO) != 0) {\n"
    "        fprintf(stderr, \"SDL_Init: %s\\n\", SDL_GetError());\n"
    "        return 1;\n"
    "    }\n"
    "\n"
    "    SDL_Window* win = SDL_CreateWindow(\n"
    "        \"clax + sdl2\",\n"
    "        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,\n"
    "        480, 320,\n"
    "        SDL_WINDOW_SHOWN);\n"
    "    if (!win) { fprintf(stderr, \"CreateWindow: %s\\n\", SDL_GetError()); SDL_Quit(); return 1; }\n"
    "\n"
    "    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);\n"
    "    if (!ren) { fprintf(stderr, \"CreateRenderer: %s\\n\", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 1; }\n"
    "\n"
    "    Button btn = new Button(140, 120, 200, 80);\n"
    "\n"
    "    bool running = true;\n"
    "    while (running) {\n"
    "        SDL_Event ev;\n"
    "        while (SDL_PollEvent(&ev)) {\n"
    "            if (ev.type == SDL_QUIT) {\n"
    "                running = false;\n"
    "            } else if (ev.type == SDL_MOUSEMOTION) {\n"
    "                btn::setHovered(btn::contains(ev.motion.x, ev.motion.y));\n"
    "            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {\n"
    "                if (btn::contains(ev.button.x, ev.button.y)) {\n"
    "                    btn::setPressed(true);\n"
    "                }\n"
    "            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {\n"
    "                bool was_in = btn::contains(ev.button.x, ev.button.y);\n"
    "                btn::setPressed(false);\n"
    "                if (was_in) btn::onClick();\n"
    "            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {\n"
    "                running = false;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        SDL_SetRenderDrawColor(ren, 30, 30, 35, 255);\n"
    "        SDL_RenderClear(ren);\n"
    "        btn::draw(ren);\n"
    "        SDL_RenderPresent(ren);\n"
    "    }\n"
    "\n"
    "    delete btn;\n"
    "    SDL_DestroyRenderer(ren);\n"
    "    SDL_DestroyWindow(win);\n"
    "    SDL_Quit();\n"
    "    return 0;\n"
    "}\n";

static const char* TPL_SDL2_BUTTON_CX =
    "// Button.cx — a tiny SDL2 GUI button.\n"
    "//\n"
    "// Hover / pressed state is updated by the event loop in main; on a click we\n"
    "// bump an internal counter. Rendering uses only SDL primitives (filled rect\n"
    "// + border) so this needs no SDL_ttf.\n"
    "#include <SDL2/SDL.h>\n"
    "#include <stdbool.h>\n"
    "\n"
    "class Button {\n"
    "    SDL_Rect rect;\n"
    "    int      clicks;\n"
    "    bool     pressed;\n"
    "    bool     hovered;\n"
    "\n"
    "    Button(int x, int y, int w, int h) {\n"
    "        this::rect.x = x;\n"
    "        this::rect.y = y;\n"
    "        this::rect.w = w;\n"
    "        this::rect.h = h;\n"
    "        this::clicks  = 0;\n"
    "        this::pressed = false;\n"
    "        this::hovered = false;\n"
    "    }\n"
    "\n"
    "    ~Button() { }\n"
    "\n"
    "    bool contains(int mx, int my) {\n"
    "        return mx >= this::rect.x\n"
    "            && my >= this::rect.y\n"
    "            && mx <  this::rect.x + this::rect.w\n"
    "            && my <  this::rect.y + this::rect.h;\n"
    "    }\n"
    "\n"
    "    void setHovered(bool h) { this::hovered = h; }\n"
    "    void setPressed(bool p) { this::pressed = p; }\n"
    "\n"
    "    void onClick() {\n"
    "        this::clicks += 1;\n"
    "        SDL_Log(\"button clicked! total = %d\", this::clicks);\n"
    "    }\n"
    "\n"
    "    int getClicks() { return this::clicks; }\n"
    "\n"
    "    void draw(SDL_Renderer* r) {\n"
    "        // Pick fill color from state.\n"
    "        Uint8 cr, cg, cb;\n"
    "        if (this::pressed)      { cr =  60; cg = 110; cb = 200; }\n"
    "        else if (this::hovered) { cr = 100; cg = 170; cb = 240; }\n"
    "        else                    { cr =  70; cg =  90; cb = 110; }\n"
    "\n"
    "        SDL_SetRenderDrawColor(r, cr, cg, cb, 255);\n"
    "        SDL_RenderFillRect(r, &this::rect);\n"
    "\n"
    "        // Border.\n"
    "        SDL_SetRenderDrawColor(r, 230, 230, 230, 255);\n"
    "        SDL_RenderDrawRect(r, &this::rect);\n"
    "\n"
    "        // A row of small white squares = click count (a poor-man's label).\n"
    "        int n = this::clicks;\n"
    "        if (n > 12) n = 12;\n"
    "        int sq = 10;\n"
    "        int gap = 4;\n"
    "        int total = n * sq + (n - 1) * gap;\n"
    "        int sx = this::rect.x + (this::rect.w - total) / 2;\n"
    "        int sy = this::rect.y + (this::rect.h - sq) / 2;\n"
    "        SDL_SetRenderDrawColor(r, 240, 240, 240, 255);\n"
    "        for (int i = 0; i < n; i++) {\n"
    "            SDL_Rect s;\n"
    "            s.x = sx + i * (sq + gap);\n"
    "            s.y = sy;\n"
    "            s.w = sq;\n"
    "            s.h = sq;\n"
    "            SDL_RenderFillRect(r, &s);\n"
    "        }\n"
    "    }\n"
    "};\n";

static const char* TPL_GLFW_MAIN_CX =
    "// main.cx — tiny GLFW program with one clickable Button.\n"
    "//\n"
    "// Uses legacy OpenGL with an orthographic 2D projection matching the window\n"
    "// size. The cursor's Y coordinate from GLFW is window-top-down, so the ortho\n"
    "// projection is set with top=0 / bottom=h to match.\n"
    "#include \"./Button.cx\"\n"
    "#include <GLFW/glfw3.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "int main(void) {\n"
    "    if (!glfwInit()) {\n"
    "        fprintf(stderr, \"glfwInit failed\\n\");\n"
    "        return 1;\n"
    "    }\n"
    "    GLFWwindow* win = glfwCreateWindow(480, 320, \"clax + glfw\", NULL, NULL);\n"
    "    if (!win) {\n"
    "        fprintf(stderr, \"glfwCreateWindow failed\\n\");\n"
    "        glfwTerminate();\n"
    "        return 1;\n"
    "    }\n"
    "    glfwMakeContextCurrent(win);\n"
    "    glfwSwapInterval(1);\n"
    "\n"
    "    Button btn = new Button(140, 120, 200, 80);\n"
    "\n"
    "    int prev_mb = GLFW_RELEASE;\n"
    "    while (!glfwWindowShouldClose(win)) {\n"
    "        glfwPollEvents();\n"
    "        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) {\n"
    "            glfwSetWindowShouldClose(win, 1);\n"
    "        }\n"
    "\n"
    "        double mx, my;\n"
    "        glfwGetCursorPos(win, &mx, &my);\n"
    "        btn::setHovered(btn::contains(mx, my));\n"
    "\n"
    "        int cur_mb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT);\n"
    "        if (cur_mb == GLFW_PRESS && prev_mb == GLFW_RELEASE) {\n"
    "            if (btn::contains(mx, my)) btn::setPressed(true);\n"
    "        } else if (cur_mb == GLFW_RELEASE && prev_mb == GLFW_PRESS) {\n"
    "            bool was_in = btn::contains(mx, my);\n"
    "            btn::setPressed(false);\n"
    "            if (was_in) btn::onClick();\n"
    "        }\n"
    "        prev_mb = cur_mb;\n"
    "\n"
    "        int fw, fh, ww, wh;\n"
    "        glfwGetFramebufferSize(win, &fw, &fh);\n"
    "        glfwGetWindowSize(win, &ww, &wh);\n"
    "\n"
    "        glViewport(0, 0, fw, fh);\n"
    "        glMatrixMode(GL_PROJECTION);\n"
    "        glLoadIdentity();\n"
    "        // top=0, bottom=wh so y matches GLFW cursor coords (top-down).\n"
    "        glOrtho(0.0, (double)ww, (double)wh, 0.0, -1.0, 1.0);\n"
    "        glMatrixMode(GL_MODELVIEW);\n"
    "        glLoadIdentity();\n"
    "\n"
    "        glClearColor(0.12f, 0.12f, 0.14f, 1.0f);\n"
    "        glClear(GL_COLOR_BUFFER_BIT);\n"
    "\n"
    "        btn::draw();\n"
    "\n"
    "        glfwSwapBuffers(win);\n"
    "    }\n"
    "\n"
    "    delete btn;\n"
    "    glfwDestroyWindow(win);\n"
    "    glfwTerminate();\n"
    "    return 0;\n"
    "}\n";

static const char* TPL_GLFW_BUTTON_CX =
    "// Button.cx — clickable button rendered with legacy OpenGL (no shaders).\n"
    "//\n"
    "// State (hover/pressed) is driven by the main event loop in main.cx via\n"
    "// setHovered / setPressed; onClick bumps an internal counter. Drawing uses\n"
    "// immediate-mode GL so the example needs no shader plumbing.\n"
    "#include <GLFW/glfw3.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "class Button {\n"
    "    int  x, y, w, h;\n"
    "    int  clicks;\n"
    "    bool pressed;\n"
    "    bool hovered;\n"
    "\n"
    "    Button(int x, int y, int w, int h) {\n"
    "        this::x = x; this::y = y; this::w = w; this::h = h;\n"
    "        this::clicks  = 0;\n"
    "        this::pressed = false;\n"
    "        this::hovered = false;\n"
    "    }\n"
    "\n"
    "    ~Button() { }\n"
    "\n"
    "    bool contains(double mx, double my) {\n"
    "        return mx >= this::x\n"
    "            && my >= this::y\n"
    "            && mx <  this::x + this::w\n"
    "            && my <  this::y + this::h;\n"
    "    }\n"
    "\n"
    "    void setHovered(bool h) { this::hovered = h; }\n"
    "    void setPressed(bool p) { this::pressed = p; }\n"
    "\n"
    "    void onClick() {\n"
    "        this::clicks += 1;\n"
    "        printf(\"button clicked! total = %d\\n\", this::clicks);\n"
    "    }\n"
    "\n"
    "    int getClicks() { return this::clicks; }\n"
    "\n"
    "    void drawQuad(int qx, int qy, int qw, int qh) {\n"
    "        glBegin(GL_QUADS);\n"
    "        glVertex2i(qx,      qy);\n"
    "        glVertex2i(qx + qw, qy);\n"
    "        glVertex2i(qx + qw, qy + qh);\n"
    "        glVertex2i(qx,      qy + qh);\n"
    "        glEnd();\n"
    "    }\n"
    "\n"
    "    void draw() {\n"
    "        float r, g, b;\n"
    "        if (this::pressed)      { r = 0.24f; g = 0.43f; b = 0.78f; }\n"
    "        else if (this::hovered) { r = 0.39f; g = 0.67f; b = 0.94f; }\n"
    "        else                    { r = 0.27f; g = 0.35f; b = 0.43f; }\n"
    "\n"
    "        // Filled rect.\n"
    "        glColor3f(r, g, b);\n"
    "        this::drawQuad(this::x, this::y, this::w, this::h);\n"
    "\n"
    "        // Border.\n"
    "        glColor3f(0.9f, 0.9f, 0.9f);\n"
    "        glBegin(GL_LINE_LOOP);\n"
    "        glVertex2i(this::x,             this::y);\n"
    "        glVertex2i(this::x + this::w,   this::y);\n"
    "        glVertex2i(this::x + this::w,   this::y + this::h);\n"
    "        glVertex2i(this::x,             this::y + this::h);\n"
    "        glEnd();\n"
    "\n"
    "        // Click-count indicator: row of small squares (capped at 12).\n"
    "        int n = this::clicks;\n"
    "        if (n > 12) n = 12;\n"
    "        int sq = 10;\n"
    "        int gap = 4;\n"
    "        int total = n * sq + (n - 1) * gap;\n"
    "        int sx = this::x + (this::w - total) / 2;\n"
    "        int sy = this::y + (this::h - sq) / 2;\n"
    "        glColor3f(0.94f, 0.94f, 0.94f);\n"
    "        for (int i = 0; i < n; i++) {\n"
    "            this::drawQuad(sx + i * (sq + gap), sy, sq, sq);\n"
    "        }\n"
    "    }\n"
    "};\n";

int main(int argc, char** argv) {
    enum { INIT_NONE = 0, INIT_PLAIN, INIT_SDL2, INIT_GLFW };
    const char* project_dir = NULL;
    int init_kind = INIT_NONE;
    /* argv[0] may end in "clax_lint" — default to lint mode in that case so
     * the wrapper script is a true alias rather than a flag-passing shim. */
    if (argc > 0) {
        const char* p0 = argv[0];
        const char* sl = strrchr(p0, '/');
        const char* base0 = sl ? sl + 1 : p0;
        if (strcmp(base0, "clax_lint") == 0) g_lint_only = true;
    }
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--include=", 10) == 0) {
            sl_add(&g_include_dirs, argv[i] + 10);
        } else if (strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
            sl_add(&g_include_dirs, argv[++i]);
        } else if (strcmp(argv[i], "--lint") == 0) {
            g_lint_only = true;
        } else if (strcmp(argv[i], "--init-project") == 0) {
            init_kind = INIT_PLAIN;
        } else if (strcmp(argv[i], "--init-sdl2-project") == 0) {
            init_kind = INIT_SDL2;
        } else if (strcmp(argv[i], "--init-glfw-project") == 0) {
            init_kind = INIT_GLFW;
        } else if (!project_dir) {
            project_dir = argv[i];
        } else {
            fprintf(stderr, "clax: unexpected argument: %s\n", argv[i]);
            return 1;
        }
    }
    if (!project_dir) {
        fprintf(stderr, "usage: clax [--lint] [--include=DIR ...] <project-dir>\n");
        fprintf(stderr, "   or: clax --init-project|--init-sdl2-project|--init-glfw-project <dir>\n");
        return 1;
    }

    if (init_kind != INIT_NONE) {
        /* Work out where the `system/` library lives so we can bake it into
         * the generated build.sh. Two sources, in priority order:
         *   1. a `system/` dir sibling to the binary's parent — i.e. running
         *      straight from the build tree (<root>/clax/clax -> <root>/system);
         *   2. the datadir baked in at configure time (installed binary).
         * realpath(argv[0]) only succeeds when argv[0] is a path; when clax is
         * found via PATH (argv[0] == "clax") it fails, which is fine — we just
         * skip step 1 and use the datadir. */
        char system_dir[4096];
        system_dir[0] = 0;
        char clax_real[4096];
        if (realpath(argv[0], clax_real)) {
            char clax_dir[4096];
            snprintf(clax_dir, sizeof(clax_dir), "%s", clax_real);
            char* sl = strrchr(clax_dir, '/'); if (sl) *sl = 0;
            char repo_root[4096];
            snprintf(repo_root, sizeof(repo_root), "%s", clax_dir);
            sl = strrchr(repo_root, '/'); if (sl) *sl = 0;
            char cand[4096];
            snprintf(cand, sizeof(cand), "%s/system", repo_root);
            struct stat sst;
            if (stat(cand, &sst) == 0)
                snprintf(system_dir, sizeof(system_dir), "%s", cand);
        }
#ifdef CLAX_DATADIR
        if (system_dir[0] == 0)
            snprintf(system_dir, sizeof(system_dir), "%s/system", CLAX_DATADIR);
#endif
        if (system_dir[0] == 0) {
            fprintf(stderr, "clax: cannot locate the system/ library "
                            "(not found relative to the binary; "
                            "no datadir was baked in at build time)\n");
            return 1;
        }

        /* Layout we scaffold:
         *
         *   <root>/
         *     build.sh         <- outside; runs clax over <root>/project/
         *     project/
         *       main.cx        <- hello-world entry point
         *       project.hx     <- project-wide constants
         *
         * `<root>` is the directory passed on the command line; `<root>/project`
         * is the actual Clax project directory consumed by the transpiler. */
        struct stat st;
        if (stat(project_dir, &st) != 0) {
            if (mkdir(project_dir, 0755) != 0) {
                fprintf(stderr, "clax: cannot create %s\n", project_dir); return 1;
            }
        }
        char proj_sub[4096];
        snprintf(proj_sub, sizeof(proj_sub), "%s/project", project_dir);
        if (stat(proj_sub, &st) != 0) {
            if (mkdir(proj_sub, 0755) != 0) {
                fprintf(stderr, "clax: cannot create %s\n", proj_sub); return 1;
            }
        }

        char path[4096];

        /* Per-kind scaffold content: the entry point, an optional Button class
         * for the GUI kinds, the pkg-config lines for build.sh, and the compile
         * command (GUI kinds add $PKG_CFLAGS/$PKG_LIBS). */
        const char* main_cx;
        const char* button_cx = NULL;
        const char* pkg_block;
        const char* compile_line;
        const char* kind_label;
        if (init_kind == INIT_SDL2) {
            main_cx      = TPL_SDL2_MAIN_CX;
            button_cx    = TPL_SDL2_BUTTON_CX;
            pkg_block    = "PKG_CFLAGS=\"$(pkg-config --cflags sdl2)\"\n"
                           "PKG_LIBS=\"$(pkg-config --libs sdl2)\"\n";
            compile_line = "$CC $CFLAGS $PKG_CFLAGS -o \"$here/program\" \"$PROJ\"/generated_c/*.c $PKG_LIBS $LDFLAGS";
            kind_label   = "Clax + SDL2";
        } else if (init_kind == INIT_GLFW) {
            main_cx      = TPL_GLFW_MAIN_CX;
            button_cx    = TPL_GLFW_BUTTON_CX;
            pkg_block    = "PKG_CFLAGS=\"$(pkg-config --cflags glfw3)\"\n"
                           "PKG_LIBS=\"$(pkg-config --libs glfw3 gl)\"\n";
            compile_line = "$CC $CFLAGS $PKG_CFLAGS -o \"$here/program\" \"$PROJ\"/generated_c/*.c $PKG_LIBS $LDFLAGS";
            kind_label   = "Clax + GLFW";
        } else {
            main_cx      = "#include <stdio.h>\n"
                           "\n"
                           "int main(void) {\n"
                           "    printf(\"hello world\\n\");\n"
                           "    return 0;\n"
                           "}\n";
            pkg_block    = "";
            compile_line = "$CC $CFLAGS -o \"$here/program\" \"$PROJ\"/generated_c/*.c $LDFLAGS";
            kind_label   = "Clax";
        }

        /* project/main.cx — never clobbered. */
        snprintf(path, sizeof(path), "%s/main.cx", proj_sub);
        if (scaffold_file(path, main_cx, true)) return 1;

        /* project/Button.cx — only for the GUI kinds. */
        if (button_cx) {
            snprintf(path, sizeof(path), "%s/Button.cx", proj_sub);
            if (scaffold_file(path, button_cx, true)) return 1;
        }

        /* project/project.hx — created only if absent. */
        const char* project_hx =
            "#ifndef PROJECT_HX\n"
            "#define PROJECT_HX\n"
            "/* project.hx — project-wide constants, macros, typedefs. */\n"
            "#endif\n";
        snprintf(path, sizeof(path), "%s/project.hx", proj_sub);
        if (stat(path, &st) != 0) {
            if (scaffold_file(path, project_hx, false)) return 1;
        }

        /* build.sh — never clobbered. */
        snprintf(path, sizeof(path), "%s/build.sh", project_dir);
        if (stat(path, &st) == 0) {
            fprintf(stderr, "clax: %s already exists, refusing to overwrite\n", path);
            return 1;
        }
        FILE* f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "clax: cannot write %s\n", path); return 1; }
        fprintf(f,
            "#!/usr/bin/env bash\n"
            "# Transpile and compile this %s project.\n"
            "# Overridable via CLAX, SYSTEM, CC, CFLAGS, LDFLAGS env vars.\n"
            "set -euo pipefail\n"
            "\n"
            "here=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"
            "PROJ=\"$here/project\"\n"
            "# `clax` is assumed installed on PATH. Override via $CLAX if not.\n"
            "CLAX=\"${CLAX:-clax}\"\n"
            "SYSTEM=\"${SYSTEM:-%s}\"\n"
            "CC=\"${CC:-gcc}\"\n"
            "CFLAGS=\"${CFLAGS:--O2 -Wall}\"\n"
            "LDFLAGS=\"${LDFLAGS:-}\"\n"
            "%s"
            "\n"
            "\"$CLAX\" --include=\"$SYSTEM\" \"$PROJ\"\n"
            "%s\n"
            "echo \"built: $here/program\"\n",
            kind_label, system_dir, pkg_block, compile_line);
        fclose(f);
        chmod(path, 0755);

        fprintf(stderr, "clax: initialized %s project at %s\n", kind_label, project_dir);
        fprintf(stderr, "  build.sh             — transpile + compile script\n");
        fprintf(stderr, "  project/main.cx      — entry point\n");
        if (button_cx)
            fprintf(stderr, "  project/Button.cx    — example clickable button\n");
        fprintf(stderr, "  project/project.hx   — project-wide constants\n");
        if (init_kind == INIT_SDL2)
            fprintf(stderr, "note: build needs the SDL2 dev package + pkg-config (e.g. libsdl2-dev).\n");
        else if (init_kind == INIT_GLFW)
            fprintf(stderr, "note: build needs GLFW3 + OpenGL dev packages + pkg-config (e.g. libglfw3-dev libgl-dev).\n");
        return 0;
    }

    /* Ensure <project-dir>/generated_c exists — but only if we'll write to it. */
    snprintf(g_output_dir, sizeof(g_output_dir), "%s/generated_c", project_dir);
    if (!g_lint_only) {
        struct stat st;
        if (stat(g_output_dir, &st) != 0) {
            if (mkdir(g_output_dir, 0755) != 0) {
                fprintf(stderr, "clax: cannot create %s\n", g_output_dir);
                return 1;
            }
        }
    }

    /* project.hx — the one user-authored header per project, holding
     * project-wide constants. The transpiler ensures it exists (creating an
     * empty stub if not), then copies it to generated_c/project.h so every
     * generated TU sees it via clax_runtime.h. Skipped in lint mode. */
    if (!g_lint_only) {
        char src_hx[1024], dst_h[1024];
        snprintf(src_hx, sizeof(src_hx), "%s/project.hx", project_dir);
        snprintf(dst_h,  sizeof(dst_h),  "%s/project.h",  g_output_dir);
        struct stat st;
        if (stat(src_hx, &st) != 0) {
            const char* stub =
                "#ifndef PROJECT_HX\n"
                "#define PROJECT_HX\n"
                "/* project.hx — project-wide constants, macros, typedefs.\n"
                " * This is the only hand-written header allowed in a Clax\n"
                " * project; everything else is a `.cx` class. The transpiler\n"
                " * copies this file to generated_c/project.h on each run, and\n"
                " * clax_runtime.h `#include`s it so every generated\n"
                " * translation unit sees its contents. */\n"
                "#endif\n";
            write_file(src_hx, stub, (int)strlen(stub));
        }
        char* hxbuf = read_file(src_hx);
        if (!hxbuf) { fprintf(stderr, "clax: cannot read %s\n", src_hx); return 1; }
        write_file(dst_h, hxbuf, (int)strlen(hxbuf));
        free(hxbuf);
    }

    /* Seed inputs with every .cx in the project dir (sorted). */
    DIR* d = opendir(project_dir);
    if (!d) { fprintf(stderr, "clax: cannot open dir %s\n", project_dir); return 1; }
    StrList files = {0};
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (has_suffix(ent->d_name, ".cx")) sl_add(&files, ent->d_name);
    }
    closedir(d);
    for (int i = 0; i < files.n; i++)
        for (int j = i + 1; j < files.n; j++)
            if (strcmp(files.v[i], files.v[j]) > 0) {
                char* t = files.v[i]; files.v[i] = files.v[j]; files.v[j] = t;
            }
    for (int i = 0; i < files.n; i++) inputs_add(project_dir, files.v[i]);

    /* Walk includes to pull in any .cx files from include dirs that the project
     * (or its dependencies) references via `#include <X.cx>` or `#include "X.cx"`. */
    discover_inputs();

    /* Pass 1: collect class names. */
    for (int i = 0; i < g_inputs_n; i++) scan_classes(g_inputs[i].dir, g_inputs[i].fname);

    /* Pass 2: transpile each input. */
    for (int i = 0; i < g_inputs_n; i++) process_file(g_inputs[i].dir, g_inputs[i].fname);

    /* Emit the shared runtime. */
    emit_runtime(project_dir);

    if (g_lint_only) {
        fprintf(stderr, "clax: lint OK — %d file(s), %d class(es)\n", g_inputs_n, g_classes.n);
    } else {
        fprintf(stderr, "clax: transpiled %d file(s), %d class(es)\n", g_inputs_n, g_classes.n);
    }
    return 0;
}
