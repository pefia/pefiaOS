#include "interp.h"
#include "wm.h"
#include "heap.h"
#include "framebuffer.h"
#include "console.h"
#include "util.h"
#include "vfs.h"

#define ROWS      64
#define COLS      96
#define MAXGLOB   48
#define MAXLOCAL  24
#define MAXFUNC   24
#define MAXPARAM  6
#define PROG_MAX  512     /* total stored source lines per window */
#define PEND_CAP  1024
#define DEPTH_MAX 32      /* python-level call depth (bounds C recursion too) */
#define STEP_MAX  2000000L /* statement budget per run; stops while True: hangs */

enum { T_INT, T_STR, T_NONE };

typedef struct { int t; long i; const char *s; } Val;

static Val vint(long x)         { Val v; v.t = T_INT;  v.i = x; v.s = 0; return v; }
static Val vnone(void)          { Val v; v.t = T_NONE; v.i = 0; v.s = 0; return v; }
static Val vstr(const char *s)  { Val v; v.t = T_STR;  v.i = 0; v.s = s ? s : ""; return v; }

typedef struct { char name[16]; char params[MAXPARAM][16]; int nparams; int bs, bend, bind; } Func;

typedef struct {
    int  lang;
    char lines[ROWS][COLS];
    int  count;
    char input[128];
    int  inlen;

    char *pl[PROG_MAX];
    int   pind[PROG_MAX];         /* indent of each line, in columns */
    int   pn;

    char gnames[MAXGLOB][16];
    Val  gvals[MAXGLOB];
    int  nglob;

    Func funcs[MAXFUNC];
    int  nfunc;

    char pending[PEND_CAP];
    int  pendlen;
    int  in_block;
} Repl;

typedef struct { char names[MAXLOCAL][16]; Val vals[MAXLOCAL]; int n; } Frame;

/* Threaded through the whole execution of one run. */
typedef struct {
    Repl  *r;
    Frame *locals;      /* NULL at top level (assignments go to globals) */
    int    depth;
    long   steps;
    int    err;
    char   msg[40];
    Val    ret;
    int    interactive;
} Ex;

enum { S_OK, S_BREAK, S_CONT, S_RET, S_ERR };

static void push(Repl *r, const char *s)
{
    if (r->count >= ROWS) {
        for (int i = 0; i < ROWS - 1; i++) kmemmove(r->lines[i], r->lines[i + 1], COLS);
        r->count = ROWS - 1;
    }
    int i = 0;
    for (; s[i] && i < COLS - 1; i++) r->lines[r->count][i] = s[i];
    r->lines[r->count][i] = '\0';
    r->count++;
}

/* --- little string helpers (freestanding, so hand-rolled) --- */

static int str_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int scmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static char *dup_n(const char *s, int n)
{
    char *d = (char *)kmalloc((size_t)n + 1);
    if (!d) return 0;
    kmemmove(d, s, (size_t)n);
    d[n] = '\0';
    return d;
}

/* long -> decimal string (handles negatives; kutoa is unsigned-only). */
static void ltoa_s(long v, char *out)
{
    char tmp[24]; int n = 0, neg = 0;
    unsigned long u = (v < 0) ? (neg = 1, (unsigned long)(-v)) : (unsigned long)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (n) out[j++] = tmp[--n];
    out[j] = '\0';
}

static int is_alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static int is_digit(char ch) { return ch >= '0' && ch <= '9'; }
static int is_word(char ch)  { return is_alpha(ch) || is_digit(ch); }

static const char *skip_sp(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

/* If s starts with the whole word kw (word boundary after), return the
 * length consumed, else 0. */
static int word_at(const char *s, const char *kw)
{
    int i = 0;
    while (kw[i]) { if (s[i] != kw[i]) return 0; i++; }
    return is_word(s[i]) ? 0 : i;
}

static void fail(Ex *e, const char *msg)
{
    if (e->err) return;
    e->err = 1;
    int i = 0;
    while (msg[i] && i < (int)sizeof(e->msg) - 1) { e->msg[i] = msg[i]; i++; }
    e->msg[i] = '\0';
}

static Val *scope_find(char names[][16], Val *vals, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (str_eq(names[i], name)) return &vals[i];
    return 0;
}

static Val *find_var(Ex *e, const char *name)
{
    if (e->locals) {
        Val *v = scope_find(e->locals->names, e->locals->vals, e->locals->n, name);
        if (v) return v;
    }
    return scope_find(e->r->gnames, e->r->gvals, e->r->nglob, name);
}

/* Create-or-find in the current (innermost) scope, Python assignment rules. */
static Val *bind_var(Ex *e, const char *name)
{
    if (e->locals) {
        Val *v = scope_find(e->locals->names, e->locals->vals, e->locals->n, name);
        if (v) return v;
        if (e->locals->n >= MAXLOCAL) { fail(e, "too many locals"); return 0; }
        int idx = e->locals->n++;
        kstrcpy(e->locals->names[idx], name);
        e->locals->vals[idx] = vnone();
        return &e->locals->vals[idx];
    }
    Repl *r = e->r;
    Val *v = scope_find(r->gnames, r->gvals, r->nglob, name);
    if (v) return v;
    if (r->nglob >= MAXGLOB) { fail(e, "too many variables"); return 0; }
    int idx = r->nglob++;
    kstrcpy(r->gnames[idx], name);
    r->gvals[idx] = vnone();
    return &r->gvals[idx];
}

typedef struct {
    const char *p;
    Ex         *e;
    int         live;   /* 0 while parsing the dead arm of and/or: no calls, no errors */
} P;

static Val eval_or(P *c);
static int exec_range(Ex *e, int start, int end, int ind);

static void read_ident(P *c, char *out)
{
    int i = 0;
    while (is_word(*c->p) && i < 15) out[i++] = *c->p++;
    while (is_word(*c->p)) c->p++;   /* swallow overlong tails instead of mis-parsing */
    out[i] = '\0';
}

static int truthy(Val v)
{
    if (v.t == T_INT)  return v.i != 0;
    if (v.t == T_STR)  return v.s[0] != '\0';
    return 0;
}

/* to-string for print(); quote=1 gives the REPL echo form ('abc'). */
static void val_text(Val v, char *out, int cap, int quote)
{
    if (v.t == T_INT) { char n[24]; ltoa_s(v.i, n); int i = 0; while (n[i] && i < cap - 1) { out[i] = n[i]; i++; } out[i] = '\0'; return; }
    if (v.t == T_NONE) { const char *n = "None"; int i = 0; while (n[i] && i < cap - 1) { out[i] = n[i]; i++; } out[i] = '\0'; return; }
    int i = 0;
    if (quote && i < cap - 1) out[i++] = '\'';
    for (int k = 0; v.s[k] && i < cap - 2; k++) out[i++] = v.s[k];
    if (quote && i < cap - 1) out[i++] = '\'';
    out[i] = '\0';
}

static Val call_user(P *c, Func *f, Val *args, int nargs)
{
    Ex *e = c->e;
    if (nargs != f->nparams) { fail(e, "wrong argument count"); return vnone(); }
    if (e->depth >= DEPTH_MAX) { fail(e, "recursion too deep"); return vnone(); }

    Frame fr; fr.n = 0;
    for (int i = 0; i < nargs; i++) {
        kstrcpy(fr.names[i], f->params[i]);
        fr.vals[i] = args[i];
        fr.n++;
    }
    Frame *saved = e->locals;
    Val    saved_ret = e->ret;
    e->locals = &fr;
    e->depth++;
    e->ret = vnone();
    int sig = exec_range(e, f->bs, f->bend, f->bind);
    Val out = (sig == S_RET) ? e->ret : vnone();
    e->locals = saved;
    e->ret = saved_ret;
    e->depth--;
    if (sig == S_BREAK || sig == S_CONT) fail(e, "break outside loop");
    return e->err ? vnone() : out;
}

static Val run_file_builtin(P *c, Val name);

/* ident already consumed, '(' is next: parse args, dispatch builtin or user fn */
static Val eval_call(P *c, const char *name)
{
    Ex *e = c->e;
    Val args[MAXPARAM + 2];
    int nargs = 0;

    c->p++;
    c->p = skip_sp(c->p);
    if (*c->p != ')') {
        for (;;) {
            if (nargs >= MAXPARAM + 2) { fail(e, "too many arguments"); return vnone(); }
            args[nargs++] = eval_or(c);
            c->p = skip_sp(c->p);
            if (*c->p == ',') { c->p++; continue; }
            break;
        }
    }
    if (*c->p == ')') c->p++; else { if (c->live) fail(e, "missing )"); return vnone(); }
    if (e->err || !c->live) return vnone();

    if (str_eq(name, "print")) {
        char out[COLS + 64]; int p = 0;
        for (int i = 0; i < nargs; i++) {
            char part[COLS];
            val_text(args[i], part, sizeof(part), 0);
            if (i && p < (int)sizeof(out) - 1) out[p++] = ' ';
            for (int k = 0; part[k] && p < (int)sizeof(out) - 1; k++) out[p++] = part[k];
        }
        out[p] = '\0';
        push(e->r, out);
        return vnone();
    }
    if (str_eq(name, "len")) {
        if (nargs != 1 || args[0].t != T_STR) { fail(e, "len() wants a string"); return vnone(); }
        return vint((long)kstrlen(args[0].s));
    }
    if (str_eq(name, "str")) {
        if (nargs != 1) { fail(e, "str() wants 1 arg"); return vnone(); }
        char buf[COLS];
        val_text(args[0], buf, sizeof(buf), 0);
        char *d = dup_n(buf, (int)kstrlen(buf));
        if (!d) { fail(e, "out of memory"); return vnone(); }
        return vstr(d);
    }
    if (str_eq(name, "int")) {
        if (nargs != 1) { fail(e, "int() wants 1 arg"); return vnone(); }
        if (args[0].t == T_INT) return args[0];
        if (args[0].t == T_STR) {
            const char *s = skip_sp(args[0].s);
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            if (!is_digit(*s)) { fail(e, "int(): bad literal"); return vnone(); }
            long v = 0;
            while (is_digit(*s)) { v = v * 10 + (*s - '0'); s++; }
            return vint(neg ? -v : v);
        }
        fail(e, "int(): bad type"); return vnone();
    }
    if (str_eq(name, "abs")) {
        if (nargs != 1 || args[0].t != T_INT) { fail(e, "abs() wants an int"); return vnone(); }
        return vint(args[0].i < 0 ? -args[0].i : args[0].i);
    }
    if (str_eq(name, "min") || str_eq(name, "max")) {
        if (nargs < 2) { fail(e, "min/max want 2+ args"); return vnone(); }
        long best = 0;
        for (int i = 0; i < nargs; i++) {
            if (args[i].t != T_INT) { fail(e, "min/max want ints"); return vnone(); }
            if (i == 0 || (name[1] == 'i' ? args[i].i < best : args[i].i > best)) best = args[i].i;
        }
        return vint(best);
    }
    if (str_eq(name, "ord")) {
        if (nargs != 1 || args[0].t != T_STR || !args[0].s[0] || args[0].s[1]) { fail(e, "ord() wants 1 char"); return vnone(); }
        return vint((long)(unsigned char)args[0].s[0]);
    }
    if (str_eq(name, "chr")) {
        if (nargs != 1 || args[0].t != T_INT || args[0].i < 0 || args[0].i > 255) { fail(e, "chr(): bad code"); return vnone(); }
        char b[2] = { (char)args[0].i, 0 };
        char *d = dup_n(b, 1);
        if (!d) { fail(e, "out of memory"); return vnone(); }
        return vstr(d);
    }
    if (str_eq(name, "run")) {
        if (nargs != 1 || args[0].t != T_STR) { fail(e, "run() wants a filename"); return vnone(); }
        return run_file_builtin(c, args[0]);
    }

    for (int i = 0; i < e->r->nfunc; i++)
        if (str_eq(e->r->funcs[i].name, name))
            return call_user(c, &e->r->funcs[i], args, nargs);

    fail(e, "unknown function");
    return vnone();
}

static Val eval_primary(P *c)
{
    Ex *e = c->e;
    c->p = skip_sp(c->p);

    if (*c->p == '(') {
        c->p++;
        Val v = eval_or(c);
        c->p = skip_sp(c->p);
        if (*c->p == ')') c->p++; else if (c->live) fail(e, "missing )");
        return v;
    }
    if (is_digit(*c->p)) {
        long v = 0;
        while (is_digit(*c->p)) { v = v * 10 + (*c->p - '0'); c->p++; }
        return vint(v);
    }
    if (*c->p == '"' || *c->p == '\'') {
        char q = *c->p++;
        char buf[COLS + 32]; int n = 0;
        while (*c->p && *c->p != q) {
            char ch = *c->p++;
            if (ch == '\\' && *c->p) {
                char nx = *c->p++;
                ch = (nx == 'n') ? '\n' : (nx == 't') ? '\t' : nx;
            }
            if (n < (int)sizeof(buf) - 1) buf[n++] = ch;
        }
        if (*c->p == q) c->p++; else if (c->live) fail(e, "unterminated string");
        char *d = dup_n(buf, n);
        if (!d) { fail(e, "out of memory"); return vnone(); }
        return vstr(d);
    }
    if (is_alpha(*c->p)) {
        char name[16];
        read_ident(c, name);
        if (str_eq(name, "True"))  return vint(1);
        if (str_eq(name, "False")) return vint(0);
        if (str_eq(name, "None"))  return vnone();
        c->p = skip_sp(c->p);
        if (*c->p == '(') return eval_call(c, name);
        Val *slot = find_var(e, name);
        if (!slot) { if (c->live) fail(e, "undefined variable"); return vint(0); }
        return *slot;
    }
    if (c->live) fail(e, "bad expression");
    return vint(0);
}

/* postfix: string indexing s[i] (negative counts from the end) */
static Val eval_post(P *c)
{
    Val v = eval_primary(c);
    for (;;) {
        c->p = skip_sp(c->p);
        if (*c->p != '[') return v;
        c->p++;
        Val idx = eval_or(c);
        c->p = skip_sp(c->p);
        if (*c->p == ']') c->p++; else { if (c->live) fail(c->e, "missing ]"); return vnone(); }
        if (!c->live || c->e->err) { v = vnone(); continue; }
        if (v.t != T_STR || idx.t != T_INT) { fail(c->e, "bad index"); return vnone(); }
        long n = (long)kstrlen(v.s), i = idx.i;
        if (i < 0) i += n;
        if (i < 0 || i >= n) { fail(c->e, "index out of range"); return vnone(); }
        char b[2] = { v.s[i], 0 };
        char *d = dup_n(b, 1);
        if (!d) { fail(c->e, "out of memory"); return vnone(); }
        v = vstr(d);
    }
}

static Val eval_unary(P *c)
{
    c->p = skip_sp(c->p);
    if (*c->p == '-') {
        c->p++;
        Val v = eval_unary(c);
        if (v.t != T_INT) { if (c->live) fail(c->e, "bad operand"); return vnone(); }
        return vint(-v.i);
    }
    return eval_post(c);
}

static Val eval_mul(P *c)
{
    Val v = eval_unary(c);
    for (;;) {
        c->p = skip_sp(c->p);
        char op = *c->p;
        if (op != '*' && op != '/' && op != '%') return v;
        c->p++;
        if (op == '/' && *c->p == '/') c->p++;
        Val rhs = eval_unary(c);
        if (c->e->err) return vnone();
        if (v.t != T_INT || rhs.t != T_INT) { if (c->live) fail(c->e, "bad operands"); return vnone(); }
        if (op == '*') v = vint(v.i * rhs.i);
        else if (rhs.i == 0) { if (c->live) fail(c->e, "division by zero"); return vnone(); }
        else v = vint(op == '/' ? v.i / rhs.i : v.i % rhs.i);
    }
}

static Val eval_add(P *c)
{
    Val v = eval_mul(c);
    for (;;) {
        c->p = skip_sp(c->p);
        char op = *c->p;
        if (op != '+' && op != '-') return v;
        c->p++;
        Val rhs = eval_mul(c);
        if (c->e->err) return vnone();
        if (v.t == T_INT && rhs.t == T_INT) {
            v = vint(op == '+' ? v.i + rhs.i : v.i - rhs.i);
        } else if (op == '+' && v.t == T_STR && rhs.t == T_STR) {
            int la = (int)kstrlen(v.s), lb = (int)kstrlen(rhs.s);
            char *d = (char *)kmalloc((size_t)la + lb + 1);
            if (!d) { fail(c->e, "out of memory"); return vnone(); }
            kmemmove(d, v.s, (size_t)la);
            kmemmove(d + la, rhs.s, (size_t)lb + 1);
            v = vstr(d);
        } else {
            if (c->live) fail(c->e, "bad operands");
            return vnone();
        }
    }
}

static Val eval_cmp(P *c)
{
    Val v = eval_add(c);
    for (;;) {
        c->p = skip_sp(c->p);
        char a = c->p[0], b = c->p[1];
        int op = 0;   /* 1:< 2:> 3:<= 4:>= 5:== 6:!= */
        if      (a == '<' && b == '=') { op = 3; c->p += 2; }
        else if (a == '>' && b == '=') { op = 4; c->p += 2; }
        else if (a == '=' && b == '=') { op = 5; c->p += 2; }
        else if (a == '!' && b == '=') { op = 6; c->p += 2; }
        else if (a == '<')             { op = 1; c->p += 1; }
        else if (a == '>')             { op = 2; c->p += 1; }
        else return v;

        Val rhs = eval_add(c);
        if (c->e->err) return vnone();
        long d;
        if (v.t == T_INT && rhs.t == T_INT)      d = (v.i > rhs.i) - (v.i < rhs.i);
        else if (v.t == T_STR && rhs.t == T_STR) { int s = scmp(v.s, rhs.s); d = (s > 0) - (s < 0); }
        else if (op == 5) { v = vint(0); continue; }
        else if (op == 6) { v = vint(1); continue; }
        else { if (c->live) fail(c->e, "bad comparison"); return vnone(); }

        switch (op) {
        case 1: v = vint(d < 0);  break;
        case 2: v = vint(d > 0);  break;
        case 3: v = vint(d <= 0); break;
        case 4: v = vint(d >= 0); break;
        case 5: v = vint(d == 0); break;
        default: v = vint(d != 0); break;
        }
    }
}

static Val eval_not(P *c)
{
    c->p = skip_sp(c->p);
    int n = word_at(c->p, "not");
    if (n) { c->p += n; Val v = eval_not(c); return vint(!truthy(v)); }
    return eval_cmp(c);
}

static Val eval_and(P *c)
{
    Val v = eval_not(c);
    for (;;) {
        c->p = skip_sp(c->p);
        int n = word_at(c->p, "and");
        if (!n) return v;
        c->p += n;
        int was_live = c->live;
        if (!truthy(v)) c->live = 0;
        Val rhs = eval_not(c);
        c->live = was_live;
        v = vint(truthy(v) && truthy(rhs));
    }
}

static Val eval_or(P *c)
{
    Val v = eval_and(c);
    for (;;) {
        c->p = skip_sp(c->p);
        int n = word_at(c->p, "or");
        if (!n) return v;
        c->p += n;
        int was_live = c->live;
        if (truthy(v)) c->live = 0;
        Val rhs = eval_and(c);
        c->live = was_live;
        v = vint(truthy(v) || truthy(rhs));
    }
}

/* Evaluate a full expression from text; complains about trailing junk. */
static Val eval_text(Ex *e, const char *s)
{
    P c; c.p = s; c.e = e; c.live = 1;
    Val v = eval_or(&c);
    c.p = skip_sp(c.p);
    if (*c.p != '\0' && !e->err) fail(e, "bad expression");
    return v;
}

static int line_blank(Repl *r, int pc)
{
    const char *s = skip_sp(r->pl[pc]);
    return *s == '\0';
}

static int next_code_line(Repl *r, int pc, int end)
{
    while (pc < end && line_blank(r, pc)) pc++;
    return pc;
}

/* First line at or past `pc` whose indent is <= hdr_ind: the end of a body. */
static int skip_block(Repl *r, int pc, int hdr_ind, int end)
{
    while (pc < end) {
        if (!line_blank(r, pc) && r->pind[pc] <= hdr_ind) break;
        pc++;
    }
    return pc;
}

/* Append one raw source line to the store: strips the quote-aware # comment
 * and trailing whitespace, records the indent. Returns 0 when full. */
static int prog_append(Repl *r, const char *src, int len)
{
    if (r->pn >= PROG_MAX) return 0;

    int ind = 0, i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t')) { ind += (src[i] == '\t') ? 4 : 1; i++; }

    char buf[256]; int n = 0;
    char q = 0;
    for (int k = 0; k < len && n < (int)sizeof(buf) - 1; k++) {
        char ch = src[k];
        if (ch == '\r') continue;
        if (q) { if (ch == q) q = 0; }
        else if (ch == '"' || ch == '\'') q = ch;
        else if (ch == '#') break;
        buf[n++] = ch;
    }
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) n--;

    char *line = dup_n(buf, n);
    if (!line) return 0;
    r->pl[r->pn] = line;
    r->pind[r->pn] = (n == 0) ? 0 : ind;
    r->pn++;
    return 1;
}

static int exec_stmt(Ex *e, int *ppc, int end);

static int exec_range(Ex *e, int start, int end, int ind)
{
    int pc = start;
    for (;;) {
        pc = next_code_line(e->r, pc, end);
        if (pc >= end) return S_OK;
        if (e->r->pind[pc] < ind) return S_OK;
        if (e->r->pind[pc] > ind) { fail(e, "unexpected indent"); return S_ERR; }
        int sig = exec_stmt(e, &pc, end);
        if (sig != S_OK) return sig;
    }
}

/* Body of the compound statement whose header is at `hdr`: locate its first
 * line and indent. Returns start pc, or -1 (empty body = error). */
static int body_of(Ex *e, int hdr, int end, int *bind, int *bend)
{
    *bend = skip_block(e->r, hdr + 1, e->r->pind[hdr], end);
    int bs = next_code_line(e->r, hdr + 1, *bend);
    if (bs >= *bend) { fail(e, "expected an indented block"); return -1; }
    *bind = e->r->pind[bs];
    return bs;
}

/* if/elif/else chain starting at *ppc; advances past the whole chain. */
static int exec_if(Ex *e, int *ppc, int end)
{
    Repl *r = e->r;
    int my = r->pind[*ppc];
    int taken = 0;
    int pc = *ppc;

    for (;;) {
        const char *s = skip_sp(r->pl[pc]);
        int is_else = 0, n;
        if      ((n = word_at(s, "if")))   s += n;
        else if ((n = word_at(s, "elif")))  s += n;
        else if ((n = word_at(s, "else")))  { s += n; is_else = 1; }
        else break;

        s = skip_sp(s);
        int cond = 0;
        if (is_else) {
            if (*s != ':') { fail(e, "expected :"); return S_ERR; }
            cond = !taken;
        } else {
            /* condition runs up to the trailing ':' - blank it out by copy */
            int L = (int)kstrlen(s);
            while (L > 0 && (s[L - 1] == ' ' || s[L - 1] == '\t')) L--;
            if (L == 0 || s[L - 1] != ':') { fail(e, "expected :"); return S_ERR; }
            char cbuf[256]; int i = 0;
            while (i < L - 1 && i < 255) { cbuf[i] = s[i]; i++; }
            cbuf[i] = '\0';
            if (!taken) {
                Val v = eval_text(e, cbuf);
                if (e->err) return S_ERR;
                cond = truthy(v);
            }
        }

        int bind, bend;
        int bs = body_of(e, pc, end, &bind, &bend);
        if (bs < 0) return S_ERR;
        if (cond) {
            taken = 1;
            int sig = exec_range(e, bs, bend, bind);
            if (sig != S_OK) { *ppc = pc; return sig; }
        }
        pc = bend;
        if (is_else) break;

        int nx = next_code_line(r, pc, end);
        if (nx >= end || r->pind[nx] != my) break;
        const char *t = skip_sp(r->pl[nx]);
        if (!word_at(t, "elif") && !word_at(t, "else")) break;
        pc = nx;
    }
    *ppc = pc;
    return S_OK;
}

static int exec_stmt(Ex *e, int *ppc, int end)
{
    Repl *r = e->r;
    int pc = *ppc;
    const char *s = skip_sp(r->pl[pc]);
    int n;

    if (++e->steps > STEP_MAX) { fail(e, "too many steps (infinite loop?)"); return S_ERR; }

    if ((n = word_at(s, "pass")))     { (void)n; *ppc = pc + 1; return S_OK; }
    if ((n = word_at(s, "break")))    { *ppc = pc + 1; return S_BREAK; }
    if ((n = word_at(s, "continue"))) { *ppc = pc + 1; return S_CONT; }

    if ((n = word_at(s, "return"))) {
        const char *rest = skip_sp(s + n);
        e->ret = (*rest == '\0') ? vnone() : eval_text(e, rest);
        *ppc = pc + 1;
        return e->err ? S_ERR : S_RET;
    }

    if (word_at(s, "if")) return exec_if(e, ppc, end);
    if (word_at(s, "elif") || word_at(s, "else")) { fail(e, "elif/else without if"); return S_ERR; }

    if ((n = word_at(s, "while"))) {
        const char *cond_src = skip_sp(s + n);
        int L = (int)kstrlen(cond_src);
        if (L == 0 || cond_src[L - 1] != ':') { fail(e, "expected :"); return S_ERR; }
        char cbuf[256]; int i = 0;
        while (i < L - 1 && i < 255) { cbuf[i] = cond_src[i]; i++; }
        cbuf[i] = '\0';

        int bind, bend;
        int bs = body_of(e, pc, end, &bind, &bend);
        if (bs < 0) return S_ERR;

        for (;;) {
            if (++e->steps > STEP_MAX) { fail(e, "too many steps (infinite loop?)"); return S_ERR; }
            Val v = eval_text(e, cbuf);
            if (e->err) return S_ERR;
            if (!truthy(v)) break;
            int sig = exec_range(e, bs, bend, bind);
            if (sig == S_BREAK) break;
            if (sig == S_CONT || sig == S_OK) continue;
            return sig;
        }
        *ppc = bend;
        return S_OK;
    }

    if ((n = word_at(s, "for"))) {
        P c; c.p = s + n; c.e = e; c.live = 1;
        c.p = skip_sp(c.p);
        if (!is_alpha(*c.p)) { fail(e, "expected a variable after for"); return S_ERR; }
        char var[16];
        read_ident(&c, var);
        c.p = skip_sp(c.p);
        int k = word_at(c.p, "in");
        if (!k) { fail(e, "expected in"); return S_ERR; }
        c.p = skip_sp(c.p + k);
        k = word_at(c.p, "range");
        if (!k) { fail(e, "only for..in range() is supported"); return S_ERR; }
        c.p = skip_sp(c.p + k);
        if (*c.p != '(') { fail(e, "expected ("); return S_ERR; }
        c.p++;

        long a[3]; int na = 0;
        for (;;) {
            if (na >= 3) { fail(e, "range() wants 1-3 args"); return S_ERR; }
            Val v = eval_or(&c);
            if (e->err) return S_ERR;
            if (v.t != T_INT) { fail(e, "range() wants ints"); return S_ERR; }
            a[na++] = v.i;
            c.p = skip_sp(c.p);
            if (*c.p == ',') { c.p++; continue; }
            break;
        }
        if (*c.p != ')') { fail(e, "missing )"); return S_ERR; }
        c.p = skip_sp(c.p + 1);
        if (*c.p != ':') { fail(e, "expected :"); return S_ERR; }

        long lo = (na >= 2) ? a[0] : 0;
        long hi = (na >= 2) ? a[1] : a[0];
        long st = (na == 3) ? a[2] : 1;
        if (st == 0) { fail(e, "range() step is zero"); return S_ERR; }

        int bind, bend;
        int bs = body_of(e, pc, end, &bind, &bend);
        if (bs < 0) return S_ERR;

        for (long i = lo; (st > 0) ? (i < hi) : (i > hi); i += st) {
            if (++e->steps > STEP_MAX) { fail(e, "too many steps (infinite loop?)"); return S_ERR; }
            Val *slot = bind_var(e, var);
            if (!slot) return S_ERR;
            *slot = vint(i);
            int sig = exec_range(e, bs, bend, bind);
            if (sig == S_BREAK) break;
            if (sig == S_CONT || sig == S_OK) continue;
            return sig;
        }
        *ppc = bend;
        return S_OK;
    }

    if ((n = word_at(s, "def"))) {
        P c; c.p = skip_sp(s + n); c.e = e; c.live = 1;
        if (!is_alpha(*c.p)) { fail(e, "expected a function name"); return S_ERR; }
        char name[16];
        read_ident(&c, name);
        c.p = skip_sp(c.p);
        if (*c.p != '(') { fail(e, "expected ("); return S_ERR; }
        c.p = skip_sp(c.p + 1);

        char params[MAXPARAM][16]; int np = 0;
        if (*c.p != ')') {
            for (;;) {
                if (np >= MAXPARAM || !is_alpha(*c.p)) { fail(e, "bad parameter list"); return S_ERR; }
                read_ident(&c, params[np++]);
                c.p = skip_sp(c.p);
                if (*c.p == ',') { c.p = skip_sp(c.p + 1); continue; }
                break;
            }
        }
        if (*c.p != ')') { fail(e, "missing )"); return S_ERR; }
        c.p = skip_sp(c.p + 1);
        if (*c.p != ':') { fail(e, "expected :"); return S_ERR; }

        int bind, bend;
        int bs = body_of(e, pc, end, &bind, &bend);
        if (bs < 0) return S_ERR;

        Func *f = 0;
        for (int i = 0; i < r->nfunc; i++)
            if (str_eq(r->funcs[i].name, name)) { f = &r->funcs[i]; break; }
        if (!f) {
            if (r->nfunc >= MAXFUNC) { fail(e, "too many functions"); return S_ERR; }
            f = &r->funcs[r->nfunc++];
        }
        kstrcpy(f->name, name);
        f->nparams = np;
        for (int i = 0; i < np; i++) kstrcpy(f->params[i], params[i]);
        f->bs = bs; f->bend = bend; f->bind = bind;
        *ppc = bend;
        return S_OK;
    }

    /* assignment: ident = expr, or augmented (+= -= *= /= %=), but not == */
    if (is_alpha(*s)) {
        P look; look.p = s; look.e = e; look.live = 1;
        char name[16];
        read_ident(&look, name);
        const char *after = skip_sp(look.p);
        char op = 0;
        const char *rhs = 0;
        if ((after[0] == '+' || after[0] == '-' || after[0] == '*' ||
             after[0] == '/' || after[0] == '%') && after[1] == '=') {
            op = after[0]; rhs = after + 2;
        } else if (after[0] == '=' && after[1] != '=') {
            rhs = after + 1;
        }
        if (rhs) {
            Val v = eval_text(e, rhs);
            if (e->err) return S_ERR;
            Val *slot;
            if (op) {
                slot = find_var(e, name);
                if (!slot) { fail(e, "undefined variable"); return S_ERR; }
                if (op == '+' && slot->t == T_STR && v.t == T_STR) {
                    int la = (int)kstrlen(slot->s), lb = (int)kstrlen(v.s);
                    char *d = (char *)kmalloc((size_t)la + lb + 1);
                    if (!d) { fail(e, "out of memory"); return S_ERR; }
                    kmemmove(d, slot->s, (size_t)la);
                    kmemmove(d + la, v.s, (size_t)lb + 1);
                    *slot = vstr(d);
                } else if (slot->t == T_INT && v.t == T_INT) {
                    long x = slot->i, y = v.i;
                    if ((op == '/' || op == '%') && y == 0) { fail(e, "division by zero"); return S_ERR; }
                    switch (op) {
                    case '+': x += y; break;  case '-': x -= y; break;
                    case '*': x *= y; break;  case '/': x /= y; break;
                    default:  x %= y; break;
                    }
                    *slot = vint(x);
                } else { fail(e, "bad operands"); return S_ERR; }
            } else {
                slot = bind_var(e, name);
                if (!slot) return S_ERR;
                *slot = v;
            }
            *ppc = pc + 1;
            return S_OK;
        }
    }

    /* expression statement; the REPL echoes top-level results */
    Val v = eval_text(e, s);
    if (e->err) return S_ERR;
    if (e->interactive && !e->locals && v.t != T_NONE) {
        char out[COLS];
        val_text(v, out, sizeof(out), v.t == T_STR);
        push(r, out);
    }
    *ppc = pc + 1;
    return S_OK;
}

/* Append `src` to the program store and execute the new lines. */
static void run_source(Repl *r, const char *src, int interactive)
{
    int start = r->pn;
    const char *p = src;
    while (*p) {
        const char *nl = p;
        while (*nl && *nl != '\n') nl++;
        if (!prog_append(r, p, (int)(nl - p))) { push(r, "error: program store full"); return; }
        p = (*nl == '\n') ? nl + 1 : nl;
    }

    Ex e;
    e.r = r; e.locals = 0; e.depth = 0; e.steps = 0;
    e.err = 0; e.msg[0] = '\0'; e.ret = vnone();
    e.interactive = interactive;

    int first = next_code_line(r, start, r->pn);
    if (first >= r->pn) return;
    if (r->pind[first] != 0) { push(r, "error: unexpected indent"); return; }

    int sig = exec_range(&e, start, r->pn, 0);
    if (e.err) {
        char out[COLS];
        kstrcpy(out, "error: ");
        kstrcat(out, e.msg);
        push(r, out);
    } else if (sig == S_RET) {
        push(r, "error: return outside function");
    }
}

static Val run_file_builtin(P *c, Val name)
{
    Ex *e = c->e;
    if (e->depth >= DEPTH_MAX) { fail(e, "recursion too deep"); return vnone(); }
    /* search the whole VFS by name - no path syntax to speak of */
    for (int i = 0; i < vfs_count(); i++) {
        const VNode *v = vfs_node(i);
        if (v && !v->is_dir && str_eq(v->name, name.s)) {
            e->depth++;
            run_source(e->r, v->content ? v->content : "", 0);
            e->depth--;
            return vnone();
        }
    }
    fail(e, "run(): file not found");
    return vnone();
}

long interp_eval(const char *s, int *ok)
{
    static Repl scratch;
    scratch.nglob = 0; scratch.nfunc = 0; scratch.pn = 0; scratch.count = 0;
    Ex e;
    e.r = &scratch; e.locals = 0; e.depth = 0; e.steps = 0;
    e.err = 0; e.msg[0] = '\0'; e.ret = vnone(); e.interactive = 0;
    Val v = eval_text(&e, s);
    if (ok) *ok = (!e.err && v.t == T_INT);
    return (v.t == T_INT) ? v.i : 0;
}

void interp_run_source(void *state, const char *src)
{
    Repl *r = (Repl *)state;
    if (r) run_source(r, src, 0);
}

void *interp_new(int lang)
{
    Repl *r = (Repl *)kmalloc(sizeof(Repl));
    if (!r) return 0;
    kmemset(r, 0, sizeof(Repl));
    r->lang = lang;
    if (lang == INTERP_PY) {
        push(r, "pefiaOS Python - ints, strings, if/while/for, def, print()");
        push(r, "block statements: end the line with ':', finish with a blank line");
        push(r, "run a file:  run(\"script.py\")");
    } else {
        push(r, "pefiaOS C interpreter (Python-flavoured statements)");
        push(r, "try:  x = 6 * 7;  then   print(x)");
    }
    return r;
}

void interp_key(Window *w, char ch)
{
    Repl *r = (Repl *)w->state;
    if (!r) return;
    if (ch == '\n') {
        /* strip a trailing ';' so C-style lines work too */
        int L = r->inlen;
        while (L > 0 && (r->input[L - 1] == ';' || r->input[L - 1] == ' ')) r->input[--L] = '\0';

        char echo[160];
        const char *tag = r->in_block ? "... " : (r->lang == INTERP_PY ? ">>> " : "c> ");
        int i = 0;
        for (const char *s = tag; *s && i < 159; s++) echo[i++] = *s;
        for (int k = 0; r->input[k] && i < 159; k++) echo[i++] = r->input[k];
        echo[i] = '\0';
        push(r, echo);

        if (r->in_block) {
            if (L == 0) {
                r->in_block = 0;
                r->pending[r->pendlen] = '\0';
                run_source(r, r->pending, 1);
                r->pendlen = 0;
            } else {
                for (int k = 0; r->input[k] && r->pendlen < PEND_CAP - 2; k++)
                    r->pending[r->pendlen++] = r->input[k];
                r->pending[r->pendlen++] = '\n';
            }
        } else if (L > 0 && r->input[L - 1] == ':') {
            r->in_block = 1;
            r->pendlen = 0;
            for (int k = 0; r->input[k] && r->pendlen < PEND_CAP - 2; k++)
                r->pending[r->pendlen++] = r->input[k];
            r->pending[r->pendlen++] = '\n';
        } else if (L > 0) {
            run_source(r, r->input, 1);
        }
        r->inlen = 0; r->input[0] = '\0';
    } else if (ch == '\b') {
        if (r->inlen > 0) r->input[--r->inlen] = '\0';
    } else if (ch >= 32 && ch < 127 && r->inlen < (int)sizeof(r->input) - 1) {
        r->input[r->inlen++] = ch;
        r->input[r->inlen] = '\0';
    }
}

static void draw_clipped(int x, int y, const char *s, int max_cols, color_t fg, color_t bg)
{
    char clip[COLS + 4];
    if (max_cols <= 0) return;
    if (max_cols > COLS + 3) max_cols = COLS + 3;
    int i = 0; while (s[i] && i < max_cols) { clip[i] = s[i]; i++; } clip[i] = '\0';
    gfx_text(x, y, clip, fg, bg);
}

void interp_paint(Window *w, int cx, int cy, int cw, int ch)
{
    Repl *r = (Repl *)w->state;
    color_t bg = fb_rgb(18, 20, 30), fg = fb_rgb(210, 220, 235);
    color_t pr = (r && r->lang == INTERP_PY) ? fb_rgb(120, 200, 120) : fb_rgb(120, 180, 240);

    fb_fill_rect(cx, cy, cw, ch, bg);
    if (!r) return;

    int cols = (cw - 8) / 8;
    int rows = (ch - 6) / 16;
    if (rows < 1) return;

    int visible = rows - 1;
    int first = (r->count > visible) ? r->count - visible : 0;
    int y = cy + 3, row = 0;
    for (int i = first; i < r->count; i++)
        draw_clipped(cx + 4, y + (row++) * 16, r->lines[i], cols, fg, bg);

    char prompt[160];
    const char *tag = r->in_block ? "... " : (r->lang == INTERP_PY ? ">>> " : "c> ");
    int i = 0; for (const char *s = tag; *s && i < 159; s++) prompt[i++] = *s;
    for (int k = 0; r->input[k] && i < 159; k++) prompt[i++] = r->input[k];
    if (i < 159) prompt[i++] = '_';
    prompt[i] = '\0';
    draw_clipped(cx + 4, y + row * 16, prompt, cols, pr, bg);
}
