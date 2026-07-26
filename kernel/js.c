#include "js.h"
#include "domrt.h"
#include "domparse.h"
#include "clock.h"

#define JS_STR_CAP   (320 * 1024)
static char g_str[JS_STR_CAP];
static int  g_strlen;

static int j_slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int j_streq(const char *a, const char *b)
{ int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; } return a[i] == b[i]; }
static char j_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int j_strieq(const char *a, const char *b)
{ int i = 0; while (a[i] && b[i]) { if (j_lc(a[i]) != j_lc(b[i])) return 0; i++; } return j_lc(a[i]) == j_lc(b[i]); }

static int str_intern(const char *s, int len)
{
    if (len < 0) len = j_slen(s);
    if (g_strlen + len + 1 > JS_STR_CAP) return 0;
    int off = g_strlen;
    for (int i = 0; i < len; i++) g_str[off + i] = s[i];
    g_str[off + len] = 0;
    g_strlen += len + 1;
    return off;
}
static const char *S(int off) { return (off >= 0 && off < g_strlen) ? g_str + off : ""; }

static int str_from_int(int v)
{
    char t[16]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; }
    unsigned uv = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    if (uv == 0) t[i++] = '0';
    while (uv) { t[i++] = (char)('0' + uv % 10); uv /= 10; }
    char buf[18]; int o = 0;
    if (neg) buf[o++] = '-';
    while (i) buf[o++] = t[--i];
    buf[o] = 0;
    return str_intern(buf, o);
}

static int parse_int_radix(const char *s, int radix)
{
    int i = 0, sign = 1, v = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
    if ((radix == 0 || radix == 16) && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) { radix = 16; i += 2; }
    if (radix == 0) radix = 10;
    for (;;) {
        int d = -1; char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'Z') d = 10 + c - 'A';
        if (d < 0 || d >= radix) break;
        v = v * radix + d; i++;
    }
    return sign * v;
}
static int parse_int_str(const char *s) { return parse_int_radix(s, 0); }

/* is the string entirely a decimal index? (for array element access) */
static int parse_index(const char *s, int *out)
{
    int i = 0, v = 0;
    if (!s[0]) return 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; if (v > 1000000) return 0; }
    if (s[i]) return 0;
    *out = v;
    return 1;
}

/* g_err latches on the first problem (syntax we don't handle, a blown limit,
 * whatever) and every later stage checks it and bails immediately - cheaper
 * than threading a return code through the whole parser/evaluator, and it
 * means a broken script just quietly does nothing instead of taking the
 * kernel down with it. STEP_MAX exists so `while(true){}` in a page's script
 * can't wedge the browser; DEPTH_MAX does the same job for recursion (both
 * eval() and exec() count against it); PDEPTH_MAX guards the recursive
 * descent parser itself, since deeply nested source would otherwise eat the
 * kernel stack before a single statement runs. */
static int g_err;
static long g_steps;
#define STEP_MAX   6000000L
#define DEPTH_MAX  80
#define PDEPTH_MAX 60
static int g_depth;
static int g_pdepth;
static int g_dirty;

static void fail(void) { g_err = 1; }
static int  budget(void) { if (++g_steps > STEP_MAX) g_err = 1; return g_err; }

enum { T_EOF, T_NUM, T_STR, T_IDENT, T_PUNCT };
typedef struct { int type; int num; int soff; char op[4]; } Tok;

#define MAX_TOKS 40000
static Tok g_toks[MAX_TOKS];
static int g_ntoks;

static int is_id_start(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static int is_id_part(char c)  { return is_id_start(c) || (c>='0'&&c<='9'); }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

static const char *MULTI[] = {
    "===","!==",">>>","<<=",">>=","...","&&","||","==","!=","<=",">=",
    "+=","-=","*=","/=","%=","++","--","=>","**","??","?.", 0
};

static void push_punct(const char *s, int n)
{
    if (g_ntoks >= MAX_TOKS) { fail(); return; }
    Tok *t = &g_toks[g_ntoks++];
    t->type = T_PUNCT;
    int i = 0; for (; i < n && i < 3; i++) t->op[i] = s[i];
    t->op[i] = 0;
}

static void tokenize(const char *s, int n)
{
    g_ntoks = 0;
    int i = 0;
    while (i < n && !g_err) {
        char c = s[i];
        if (is_ws(c)) { i++; continue; }
        if (c == '/' && i + 1 < n && s[i+1] == '/') { i += 2; while (i < n && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && s[i+1] == '*') { i += 2; while (i + 1 < n && !(s[i]=='*'&&s[i+1]=='/')) i++; i += 2; continue; }

        if (g_ntoks >= MAX_TOKS) { fail(); break; }

        if ((c >= '0' && c <= '9') || (c == '.' && i+1 < n && s[i+1] >= '0' && s[i+1] <= '9')) {
            int j = i, val = 0;
            if (c == '0' && i+1 < n && (s[i+1]=='x'||s[i+1]=='X')) {
                j = i + 2;
                while (j < n) {
                    int d = -1;
                    if (s[j]>='0'&&s[j]<='9') d = s[j]-'0';
                    else if (s[j]>='a'&&s[j]<='f') d = 10+s[j]-'a';
                    else if (s[j]>='A'&&s[j]<='F') d = 10+s[j]-'A';
                    else break;
                    val = val*16 + d; j++;
                }
            } else {
                while (j < n && s[j] >= '0' && s[j] <= '9') { val = val*10 + (s[j]-'0'); j++; }
                /* numbers are ints here, so "3.14" scans the fraction and drops it */
                if (j < n && s[j] == '.') { j++; while (j < n && s[j] >= '0' && s[j] <= '9') j++; }
                if (j < n && (s[j]=='e'||s[j]=='E')) { j++; if (j<n&&(s[j]=='+'||s[j]=='-'))j++; while (j<n&&s[j]>='0'&&s[j]<='9')j++; }
            }
            Tok *t = &g_toks[g_ntoks++]; t->type = T_NUM; t->num = val;
            i = j; continue;
        }
        /* strings: single, double, and backtick all treated the same - no
         * template-literal interpolation, `${x}` just ends up as literal text */
        if (c == '"' || c == '\'' || c == '`') {
            char q = c; i++;
            char buf[2048]; int bl = 0;
            while (i < n && s[i] != q) {
                char ch = s[i];
                if (ch == '\\' && i+1 < n) {
                    i++; char e = s[i];
                    if (e=='n') ch='\n'; else if (e=='t') ch='\t'; else if (e=='r') ch='\r';
                    else if (e=='\\') ch='\\'; else if (e=='\'') ch='\''; else if (e=='"') ch='"';
                    else if (e=='`') ch='`'; else if (e=='0') ch=0; else ch=e;
                }
                if (bl < 2047) buf[bl++] = ch;
                i++;
            }
            if (i < n) i++;
            buf[bl] = 0;
            Tok *t = &g_toks[g_ntoks++]; t->type = T_STR; t->soff = str_intern(buf, bl);
            continue;
        }
        /* identifiers and keywords share a token type; the parser figures out
         * which keyword it's looking at by comparing the interned text */
        if (is_id_start(c)) {
            int j = i; while (j < n && is_id_part(s[j])) j++;
            char buf[128]; int bl = 0;
            for (int k = i; k < j && bl < 127; k++) buf[bl++] = s[k];
            buf[bl] = 0;
            Tok *t = &g_toks[g_ntoks++]; t->type = T_IDENT; t->soff = str_intern(buf, bl);
            i = j; continue;
        }
        /* punctuator: try the multi-char table first so "===" doesn't get
         * split into "==" + "=" */
        {
            int matched = 0;
            for (int m = 0; MULTI[m]; m++) {
                int ml = j_slen(MULTI[m]);
                if (i + ml <= n) {
                    int ok = 1;
                    for (int z = 0; z < ml; z++) if (s[i+z] != MULTI[m][z]) { ok = 0; break; }
                    if (ok) { push_punct(MULTI[m], ml); i += ml; matched = 1; break; }
                }
            }
            if (!matched) { char one[1] = { c }; push_punct(one, 1); i++; }
        }
    }
    if (g_ntoks < MAX_TOKS) { g_toks[g_ntoks].type = T_EOF; }
}

enum {
    A_NUM, A_STR, A_BOOL, A_NULL, A_UNDEF, A_IDENT,
    A_BINARY, A_LOGICAL, A_UNARY, A_ASSIGN, A_UPDATE,
    A_MEMBER, A_INDEX, A_CALL, A_COND,
    A_VARDECL, A_EXPRSTMT, A_BLOCK, A_IF, A_WHILE, A_FOR,
    A_RETURN, A_BREAK, A_CONTINUE, A_FUNC, A_EMPTY,
    A_ARRAYLIT, A_OBJLIT, A_PROP, A_DOWHILE, A_SWITCH, A_CASE,
    A_FORIN, A_TRY, A_THROW, A_NEW
};

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT, OP_NEG, OP_POS, OP_TYPEOF,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_ASSIGN, OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
    OP_PREINC, OP_PREDEC, OP_POSTINC, OP_POSTDEC,
    OP_NULLISH, OP_DELETE, OP_VOID
};

/* One node shape for every kind of statement/expression - a,b,c,d mean
 * different things depending on n->kind (e.g. a/b/c are cond/then/else for
 * A_IF but init/cond/update... plus d for A_FOR). Wasteful of a few bytes per
 * node, but it means the parser doesn't need a different struct per
 * production, and nodes come out of a flat array (g_ast) instead of malloc so
 * there's nothing to free. list/next chain siblings for anything with a
 * variable number of children: block statements, call args, function params.
 *
 * The pool resets per DOCUMENT, not per script run: click handlers and
 * timer callbacks registered by one <script> hold AstNode pointers that must
 * still be valid when a later script (or a click) runs. */
typedef struct AstNode {
    int kind, op, ival, soff;
    struct AstNode *a, *b, *c, *d;
    struct AstNode *list;
    struct AstNode *next;
} AstNode;

#define MAX_AST 40000
static AstNode g_ast[MAX_AST];
static int     g_nast;

static AstNode *node(int kind)
{
    if (g_nast >= MAX_AST) { fail(); return 0; }
    AstNode *n = &g_ast[g_nast++];
    n->kind = kind; n->op = 0; n->ival = 0; n->soff = 0;
    n->a = n->b = n->c = n->d = n->list = n->next = 0;
    return n;
}

/* appends `item` to the list hanging off container->list, tracked via an
 * external tail pointer so we don't have to walk the list to find the end
 * every time - this exact dance repeats in every place that builds a sibling
 * chain (block bodies, var decls, call args, top-level program) */
static void list_append(AstNode *container, AstNode **tail, AstNode *item)
{
    if (!container || !item) return;
    if (!container->list) container->list = item;
    else (*tail)->next = item;
    *tail = item;
}

/* --- parser ----------------------------------------------------------------
 * Recursive descent, with parse_bin doing precedence climbing for the binary
 * operators (see bin_op's table below) rather than a wall of parse_addsub /
 * parse_muldiv / ... functions. */

static int g_pos;

static Tok *cur(void) { return &g_toks[g_pos]; }
static Tok *peek(int k)
{
    int p = g_pos + k;
    if (p >= g_ntoks) p = g_ntoks;
    if (p >= MAX_TOKS) p = MAX_TOKS - 1;
    return &g_toks[p];
}
static int is_punct(const char *p) { Tok *t = cur(); return t->type == T_PUNCT && j_streq(t->op, p); }
static int is_kw(const char *k) { Tok *t = cur(); return t->type == T_IDENT && j_streq(S(t->soff), k); }
static void advance(void) { if (g_pos < g_ntoks) g_pos++; }
static int eat_punct(const char *p) { if (is_punct(p)) { advance(); return 1; } return 0; }

static AstNode *parse_expr(void);
static AstNode *parse_assign(void);
static AstNode *parse_stmt(void);

/* wrap an expression as { return expr; } - the body of an expression-bodied
 * arrow function */
static AstNode *wrap_return(AstNode *e)
{
    AstNode *blk = node(A_BLOCK);
    AstNode *ret = node(A_RETURN);
    if (ret) ret->a = e;
    if (blk) blk->list = ret;
    return blk;
}

/* parse "(a, b) => body" bodies + param lists shared by arrows/functions */
static void parse_params_into(AstNode *fn)
{
    AstNode *ptail = 0;
    while (!is_punct(")") && cur()->type != T_EOF && !g_err) {
        if (cur()->type == T_IDENT) {
            AstNode *p = node(A_IDENT); if (p) p->soff = cur()->soff; advance();
            list_append(fn, &ptail, p);
        } else advance();
        if (!eat_punct(",")) break;
    }
    eat_punct(")");
}

static AstNode *parse_arrow_after_params(AstNode *fn)
{

    advance();
    if (is_punct("{")) fn->a = parse_stmt();
    else fn->a = wrap_return(parse_assign());
    return fn;
}

/* Is the "(" at g_pos the start of an arrow function's parameter list?
 * Scan ahead for the matching ")" and check the next token for "=>". */
static int paren_is_arrow(void)
{
    int depth = 0;
    for (int k = 0; k < 4000; k++) {
        Tok *t = peek(k);
        if (t->type == T_EOF) return 0;
        if (t->type == T_PUNCT) {
            if (j_streq(t->op, "(")) depth++;
            else if (j_streq(t->op, ")")) {
                depth--;
                if (depth == 0) {
                    Tok *nx = peek(k + 1);
                    return nx->type == T_PUNCT && j_streq(nx->op, "=>");
                }
            }
        }
    }
    return 0;
}

static AstNode *parse_primary(void)
{
    if (g_err) return 0;
    if (++g_pdepth > PDEPTH_MAX) { g_pdepth--; fail(); return 0; }
    AstNode *result = 0;
    Tok *t = cur();
    if (t->type == T_NUM) { advance(); result = node(A_NUM); if (result) result->ival = t->num; g_pdepth--; return result; }
    if (t->type == T_STR) { advance(); result = node(A_STR); if (result) result->soff = t->soff; g_pdepth--; return result; }
    if (t->type == T_IDENT) {
        const char *s = S(t->soff);
        if (j_streq(s, "true"))  { advance(); result = node(A_BOOL); if (result) result->ival = 1; g_pdepth--; return result; }
        if (j_streq(s, "false")) { advance(); result = node(A_BOOL); if (result) result->ival = 0; g_pdepth--; return result; }
        if (j_streq(s, "null"))  { advance(); g_pdepth--; return node(A_NULL); }
        if (j_streq(s, "undefined")) { advance(); g_pdepth--; return node(A_UNDEF); }
        if (j_streq(s, "function")) {
            advance();
            AstNode *fn = node(A_FUNC);
            if (cur()->type == T_IDENT) { if (fn) fn->soff = cur()->soff; advance(); }
            if (!eat_punct("(")) { fail(); g_pdepth--; return fn; }
            if (fn) parse_params_into(fn); else { while (!is_punct(")") && cur()->type != T_EOF) advance(); eat_punct(")"); }
            if (fn) fn->a = parse_stmt();
            g_pdepth--;
            return fn;
        }

        if (peek(1)->type == T_PUNCT && j_streq(peek(1)->op, "=>")) {
            AstNode *fn = node(A_FUNC);
            AstNode *p = node(A_IDENT);
            if (p) p->soff = t->soff;
            if (fn) fn->list = p;
            advance();
            parse_arrow_after_params(fn);
            g_pdepth--;
            return fn;
        }
        advance();
        result = node(A_IDENT); if (result) result->soff = t->soff;
        g_pdepth--;
        return result;
    }
    if (is_punct("(")) {
        if (paren_is_arrow()) {
            AstNode *fn = node(A_FUNC);
            advance();
            if (fn) parse_params_into(fn); else { while (!is_punct(")") && cur()->type != T_EOF) advance(); eat_punct(")"); }
            if (fn && is_punct("=>")) parse_arrow_after_params(fn);
            else fail();
            g_pdepth--;
            return fn;
        }
        advance();
        AstNode *e = parse_expr();
        eat_punct(")");
        g_pdepth--;
        return e;
    }
    if (is_punct("[")) {
        advance();
        AstNode *arr = node(A_ARRAYLIT);
        AstNode *tail = 0;
        while (!is_punct("]") && cur()->type != T_EOF && !g_err) {
            AstNode *el = parse_assign();
            list_append(arr, &tail, el);
            if (!eat_punct(",")) break;
        }
        eat_punct("]");
        g_pdepth--;
        return arr;
    }
    if (is_punct("{")) {
        advance();
        AstNode *obj = node(A_OBJLIT);
        AstNode *tail = 0;
        while (!is_punct("}") && cur()->type != T_EOF && !g_err) {
            AstNode *p = node(A_PROP);
            Tok *kt = cur();
            if (kt->type == T_IDENT || kt->type == T_STR) { if (p) p->soff = kt->soff; advance(); }
            else if (kt->type == T_NUM) { if (p) p->soff = str_from_int(kt->num); advance(); }
            else { fail(); break; }
            if (eat_punct(":")) { if (p) p->a = parse_assign(); }
            else {
                AstNode *id = node(A_IDENT);
                if (id && p) { id->soff = p->soff; p->a = id; }
            }
            list_append(obj, &tail, p);
            if (!eat_punct(",")) break;
        }
        eat_punct("}");
        g_pdepth--;
        return obj;
    }
    /* whatever's left is stuff we don't parse - regex literals, spread,
     * template interpolation. Bail rather than pretend to understand it. */
    fail();
    g_pdepth--;
    return 0;
}

static AstNode *parse_args_call(AstNode *callee)
{
    AstNode *call = node(A_CALL);
    if (call) call->a = callee;
    AstNode *tail = 0;
    advance();
    while (!is_punct(")") && cur()->type != T_EOF && !g_err) {
        AstNode *arg = parse_assign();
        list_append(call, &tail, arg);
        if (!eat_punct(",")) break;
    }
    eat_punct(")");
    return call;
}

static AstNode *parse_postfix(void)
{
    AstNode *e = parse_primary();
    while (!g_err) {
        if (is_punct(".") || is_punct("?.")) {
            advance();
            AstNode *m = node(A_MEMBER);
            if (m) { m->a = e; m->soff = cur()->soff; }
            advance();
            e = m;
        } else if (is_punct("[")) {
            advance();
            AstNode *idx = node(A_INDEX);
            if (idx) { idx->a = e; idx->b = parse_expr(); }
            eat_punct("]");
            e = idx;
        } else if (is_punct("(")) {
            e = parse_args_call(e);
        } else if (is_punct("++") || is_punct("--")) {
            int dec = is_punct("--");
            advance();
            AstNode *u = node(A_UPDATE);
            if (u) { u->a = e; u->op = dec ? OP_POSTDEC : OP_POSTINC; }
            e = u;
        } else break;
    }
    return e;
}

static AstNode *parse_unary(void)
{
    if (is_punct("!") || is_punct("-") || is_punct("+")) {
        int op = is_punct("!") ? OP_NOT : is_punct("-") ? OP_NEG : OP_POS;
        advance();
        AstNode *u = node(A_UNARY); if (u) { u->op = op; u->a = parse_unary(); } return u;
    }
    if (is_kw("typeof")) { advance(); AstNode *u = node(A_UNARY); if (u) { u->op = OP_TYPEOF; u->a = parse_unary(); } return u; }
    if (is_kw("delete")) { advance(); AstNode *u = node(A_UNARY); if (u) { u->op = OP_DELETE; u->a = parse_unary(); } return u; }
    if (is_kw("void"))   { advance(); AstNode *u = node(A_UNARY); if (u) { u->op = OP_VOID; u->a = parse_unary(); } return u; }
    if (is_kw("new"))    { advance(); AstNode *u = node(A_NEW); if (u) u->a = parse_postfix(); return u; }
    if (is_punct("++") || is_punct("--")) {
        int dec = is_punct("--"); advance();
        AstNode *u = node(A_UPDATE); if (u) { u->op = dec ? OP_PREDEC : OP_PREINC; u->a = parse_unary(); } return u;
    }
    return parse_postfix();
}

/* table of binary operators the tokenizer might hand us next, with their
 * precedence (higher binds tighter); &&/||/?? are flagged as logical since
 * they need to short-circuit instead of eagerly evaluating both sides */
static int bin_op(int *prec, int *logical)
{
    *logical = 0;
    Tok *t = cur();
    if (t->type != T_PUNCT) {
        /* "in" and "instanceof" appear as identifiers; treat as loose
         * comparisons that mostly evaluate to something sane */
        if (t->type == T_IDENT && j_streq(S(t->soff), "instanceof")) { *prec = 9; return OP_EQ; }
        return -1;
    }
    const char *o = t->op;
    if (j_streq(o,"*")) { *prec=12; return OP_MUL; }
    if (j_streq(o,"/")) { *prec=12; return OP_DIV; }
    if (j_streq(o,"%")) { *prec=12; return OP_MOD; }
    if (j_streq(o,"+")) { *prec=11; return OP_ADD; }
    if (j_streq(o,"-")) { *prec=11; return OP_SUB; }
    if (j_streq(o,"<<")) { *prec=10; return OP_SHL; }
    if (j_streq(o,">>")) { *prec=10; return OP_SHR; }
    if (j_streq(o,">>>")) { *prec=10; return OP_SHR; }
    if (j_streq(o,"<")) { *prec=9; return OP_LT; }
    if (j_streq(o,">")) { *prec=9; return OP_GT; }
    if (j_streq(o,"<=")) { *prec=9; return OP_LE; }
    if (j_streq(o,">=")) { *prec=9; return OP_GE; }
    if (j_streq(o,"==")) { *prec=8; return OP_EQ; }
    if (j_streq(o,"!=")) { *prec=8; return OP_NE; }
    if (j_streq(o,"===")) { *prec=8; return OP_SEQ; }
    if (j_streq(o,"!==")) { *prec=8; return OP_SNE; }
    if (j_streq(o,"&")) { *prec=7; return OP_BAND; }
    if (j_streq(o,"^")) { *prec=6; return OP_BXOR; }
    if (j_streq(o,"|")) { *prec=5; return OP_BOR; }
    if (j_streq(o,"&&")) { *prec=4; *logical=1; return OP_AND; }
    if (j_streq(o,"||")) { *prec=3; *logical=1; return OP_OR; }
    if (j_streq(o,"??")) { *prec=3; *logical=1; return OP_NULLISH; }
    return -1;
}

static AstNode *parse_bin(int minprec)
{
    AstNode *left = parse_unary();
    while (!g_err) {
        int prec, logical, op = bin_op(&prec, &logical);
        if (op < 0 || prec < minprec) break;
        advance();
        AstNode *right = parse_bin(prec + 1);
        AstNode *b = node(logical ? A_LOGICAL : A_BINARY);
        if (b) { b->op = op; b->a = left; b->b = right; }
        left = b;
    }
    return left;
}

static AstNode *parse_cond(void)
{
    AstNode *c = parse_bin(0);
    if (is_punct("?")) {
        advance();
        AstNode *n = node(A_COND);
        if (n) { n->a = c; n->b = parse_assign(); }
        eat_punct(":");
        if (n) n->c = parse_assign();
        return n;
    }
    return c;
}

static AstNode *parse_assign(void)
{
    if (++g_pdepth > PDEPTH_MAX) { g_pdepth--; fail(); return 0; }
    AstNode *left = parse_cond();
    Tok *t = cur();
    if (t->type == T_PUNCT) {
        int op = -1;
        if (j_streq(t->op,"=")) op = OP_ASSIGN;
        else if (j_streq(t->op,"+=")) op = OP_ADDEQ;
        else if (j_streq(t->op,"-=")) op = OP_SUBEQ;
        else if (j_streq(t->op,"*=")) op = OP_MULEQ;
        else if (j_streq(t->op,"/=")) op = OP_DIVEQ;
        else if (j_streq(t->op,"%=")) op = OP_MODEQ;
        if (op >= 0) {
            advance();
            AstNode *n = node(A_ASSIGN);
            if (n) { n->op = op; n->a = left; n->b = parse_assign(); }
            g_pdepth--;
            return n;
        }
    }
    g_pdepth--;
    return left;
}

static AstNode *parse_expr(void)
{
    AstNode *e = parse_assign();
    while (is_punct(",") && !g_err) { advance(); e = parse_assign(); }  /* comma op: keep the last one */
    return e;
}

static AstNode *parse_block(void)
{
    AstNode *blk = node(A_BLOCK);
    AstNode *tail = 0;
    advance();
    while (!is_punct("}") && cur()->type != T_EOF && !g_err) {
        AstNode *s = parse_stmt();
        list_append(blk, &tail, s);
    }
    eat_punct("}");
    return blk;
}

/* "var a = 1, b, c = 2;" becomes an A_BLOCK of A_VARDECL nodes rather than
 * its own AST shape - one less node kind to handle everywhere else */
static AstNode *parse_var(void)
{
    advance(); /* var/let/const - we don't distinguish them */
    AstNode *blk = node(A_BLOCK);
    AstNode *tail = 0;
    while (!g_err) {
        if (cur()->type != T_IDENT) { fail(); break; }
        AstNode *decl = node(A_VARDECL);
        if (decl) decl->soff = cur()->soff;
        advance();
        if (eat_punct("=")) { if (decl) decl->a = parse_assign(); }
        list_append(blk, &tail, decl);
        if (!eat_punct(",")) break;
    }
    eat_punct(";");
    return blk;
}

static AstNode *parse_stmt(void)
{
    if (g_err) return 0;
    if (++g_pdepth > PDEPTH_MAX) { g_pdepth--; fail(); return 0; }
    AstNode *result = 0;
    if (is_punct("{")) { result = parse_block(); g_pdepth--; return result; }
    if (is_punct(";")) { advance(); g_pdepth--; return node(A_EMPTY); }
    if (is_kw("var") || is_kw("let") || is_kw("const")) { result = parse_var(); g_pdepth--; return result; }
    if (is_kw("function")) { result = parse_primary(); g_pdepth--; return result; }  /* decl and expr share the same parse */
    if (is_kw("if")) {
        advance();
        AstNode *n = node(A_IF);
        eat_punct("(");
        if (n) n->a = parse_expr();
        eat_punct(")");
        if (n) n->b = parse_stmt();
        if (is_kw("else")) { advance(); if (n) n->c = parse_stmt(); }
        g_pdepth--;
        return n;
    }
    if (is_kw("while")) {
        advance();
        AstNode *n = node(A_WHILE);
        eat_punct("(");
        if (n) n->a = parse_expr();
        eat_punct(")");
        if (n) n->b = parse_stmt();
        g_pdepth--;
        return n;
    }
    if (is_kw("do")) {
        advance();
        AstNode *n = node(A_DOWHILE);
        if (n) n->b = parse_stmt();
        if (is_kw("while")) {
            advance();
            eat_punct("(");
            if (n) n->a = parse_expr();
            eat_punct(")");
            eat_punct(";");
        } else fail();
        g_pdepth--;
        return n;
    }
    if (is_kw("for")) {
        advance();
        eat_punct("(");
        /* for-in / for-of: "[var] IDENT in|of expr" */
        {
            int save = g_pos;
            int decl = (is_kw("var") || is_kw("let") || is_kw("const"));
            if (decl) advance();
            if (cur()->type == T_IDENT && peek(1)->type == T_IDENT &&
                (j_streq(S(peek(1)->soff), "in") || j_streq(S(peek(1)->soff), "of"))) {
                AstNode *n = node(A_FORIN);
                if (n) n->soff = cur()->soff;
                advance();
                if (n) n->op = j_streq(S(cur()->soff), "of") ? 1 : 0;
                advance();
                if (n) n->a = parse_expr();
                eat_punct(")");
                if (n) n->b = parse_stmt();
                g_pdepth--;
                return n;
            }
            g_pos = save;
        }

        AstNode *n = node(A_FOR);
        if (is_punct(";")) { advance(); }
        else if (is_kw("var") || is_kw("let") || is_kw("const")) { if (n) n->a = parse_var(); }
        else { AstNode *e = parse_expr(); AstNode *es = node(A_EXPRSTMT); if (es) es->a = e; if (n) n->a = es; eat_punct(";"); }
        if (!is_punct(";")) { if (n) n->b = parse_expr(); }
        eat_punct(";");
        if (!is_punct(")")) { if (n) n->c = parse_expr(); }
        eat_punct(")");
        if (n) n->d = parse_stmt();
        g_pdepth--;
        return n;
    }
    if (is_kw("switch")) {
        advance();
        AstNode *n = node(A_SWITCH);
        eat_punct("(");
        if (n) n->a = parse_expr();
        eat_punct(")");
        eat_punct("{");
        AstNode *ctail = 0;
        while (!is_punct("}") && cur()->type != T_EOF && !g_err) {
            AstNode *cs = 0;
            if (is_kw("case")) {
                advance();
                cs = node(A_CASE);
                if (cs) cs->a = parse_expr();
                eat_punct(":");
            } else if (is_kw("default")) {
                advance();
                cs = node(A_CASE);
                eat_punct(":");
            } else { fail(); break; }
            AstNode *stail = 0;
            while (!is_kw("case") && !is_kw("default") && !is_punct("}") &&
                   cur()->type != T_EOF && !g_err) {
                AstNode *s = parse_stmt();
                list_append(cs, &stail, s);
            }
            list_append(n, &ctail, cs);
        }
        eat_punct("}");
        g_pdepth--;
        return n;
    }
    if (is_kw("try")) {
        advance();
        AstNode *n = node(A_TRY);
        if (n) n->a = parse_stmt();
        if (is_kw("catch")) {
            advance();
            if (eat_punct("(")) {
                if (cur()->type == T_IDENT) { if (n) n->soff = cur()->soff; advance(); }
                eat_punct(")");
            }
            if (n) n->b = parse_stmt(); else parse_stmt();
        }
        if (is_kw("finally")) { advance(); if (n) n->c = parse_stmt(); else parse_stmt(); }
        g_pdepth--;
        return n;
    }
    if (is_kw("throw")) {
        advance();
        AstNode *n = node(A_THROW);
        if (n) n->a = parse_expr();
        eat_punct(";");
        g_pdepth--;
        return n;
    }
    if (is_kw("return")) {
        advance();
        AstNode *n = node(A_RETURN);
        if (!is_punct(";") && !is_punct("}") && cur()->type != T_EOF) { if (n) n->a = parse_expr(); }
        eat_punct(";");
        g_pdepth--;
        return n;
    }
    if (is_kw("break")) { advance(); eat_punct(";"); g_pdepth--; return node(A_BREAK); }
    if (is_kw("continue")) { advance(); eat_punct(";"); g_pdepth--; return node(A_CONTINUE); }
    /* fell through everything above - must be a bare expression statement */
    {
        AstNode *e = parse_expr();
        AstNode *s = node(A_EXPRSTMT);
        if (s) s->a = e;
        eat_punct(";");
        g_pdepth--;
        return s;
    }
}

/* --- values -----------------------------------------------------------------
 * Value is a tagged union in struct clothing - `type` says which of
 * num/soff/objkind/obji/envi/node/fn actually means anything. O_* is the
 * flavor of V_OBJ: DOM elements, the singletons, script objects/arrays
 * (obji into g_objs), user functions (fn + captured env), and builtins. */

enum { V_UNDEF, V_NULL, V_NUM, V_BOOL, V_STR, V_OBJ };
enum { O_DOCUMENT, O_ELEMENT, O_STYLE, O_CONSOLE, O_WINDOW, O_MATH,
       O_FUNC, O_BUILTIN, O_LOCATION, O_ARRAY, O_PLAIN, O_CLASSLIST, O_JSON };
enum { B_PARSEINT, B_PARSEFLOAT, B_STRING, B_NUMBER, B_BOOLEAN, B_ISNAN, B_NOOP,
       B_ALERT, B_CONFIRM, B_PROMPT, B_SETTIMEOUT, B_SETINTERVAL, B_CLEARTMR,
       B_ENCURI, B_DECURI, B_DATE, B_DATENOW, B_OBJKEYS, B_OBJVALUES,
       B_ISARRAY, B_STRINGIFY, B_RAF, B_OBJECT, B_ARRAY };

typedef struct {
    int      type;
    int      num;
    int      soff;
    int      objkind;
    int      bid;
    int      obji;       /* index into g_objs (O_ARRAY/O_PLAIN), or -1 */
    int      envi;       /* captured env for O_FUNC, or -1 */
    DomNode *node;
    AstNode *fn;
} Value;

static Value v_undef(void){ Value v; v.type=V_UNDEF; v.num=0; v.soff=0; v.objkind=0; v.bid=0; v.obji=-1; v.envi=-1; v.node=0; v.fn=0; return v; }
static Value v_null(void){ Value v=v_undef(); v.type=V_NULL; return v; }
static Value v_num(int n){ Value v=v_undef(); v.type=V_NUM; v.num=n; return v; }
static Value v_bool(int b){ Value v=v_undef(); v.type=V_BOOL; v.num=b?1:0; return v; }
static Value v_str(int off){ Value v=v_undef(); v.type=V_STR; v.soff=off; return v; }
static Value v_obj(int kind){ Value v=v_undef(); v.type=V_OBJ; v.objkind=kind; return v; }
static Value v_elem(DomNode *n){ Value v=v_obj(O_ELEMENT); v.node=n; return v; }

#define OBJ_POOL  1200
#define PROP_POOL 10000
typedef struct { int key; Value val; int next; } Prop;
typedef struct { int phead; int alen; } JsObj;
static JsObj g_objs[OBJ_POOL];
static int   g_nobjs;
static Prop  g_props[PROP_POOL];
static int   g_nprops;

static int obj_new(void)
{
    if (g_nobjs >= OBJ_POOL) { fail(); return -1; }
    g_objs[g_nobjs].phead = -1;
    g_objs[g_nobjs].alen = 0;
    return g_nobjs++;
}

static Value v_new_array(void) { Value v = v_obj(O_ARRAY); v.obji = obj_new(); return v; }
static Value v_new_plain(void) { Value v = v_obj(O_PLAIN); v.obji = obj_new(); return v; }

static Value *prop_find(int obji, const char *name)
{
    if (obji < 0 || obji >= g_nobjs) return 0;
    for (int p = g_objs[obji].phead; p >= 0; p = g_props[p].next)
        if (j_streq(S(g_props[p].key), name)) return &g_props[p].val;
    return 0;
}

static void prop_set(int obji, const char *name, Value val)
{
    if (obji < 0 || obji >= g_nobjs) return;
    Value *slot = prop_find(obji, name);
    if (slot) { *slot = val; }
    else {
        if (g_nprops >= PROP_POOL) { fail(); return; }
        int p = g_nprops++;
        g_props[p].key = str_intern(name, -1);
        g_props[p].val = val;
        g_props[p].next = g_objs[obji].phead;
        g_objs[obji].phead = p;
    }
    int idx;
    if (parse_index(name, &idx) && idx >= g_objs[obji].alen)
        g_objs[obji].alen = idx + 1;
}

static Value arr_get(int obji, int idx)
{
    char k[16]; int kl = 0;
    if (idx < 0) return v_undef();
    { int v = idx; char t[16]; int i = 0; if (v == 0) t[i++]='0'; while (v) { t[i++]=(char)('0'+v%10); v/=10; } while (i) k[kl++]=t[--i]; k[kl]=0; }
    Value *p = prop_find(obji, k);
    return p ? *p : v_undef();
}

static void arr_push(int obji, Value v)
{
    if (obji < 0 || obji >= g_nobjs) return;
    char k[16]; int kl = 0;
    { int n = g_objs[obji].alen; char t[16]; int i = 0; if (n == 0) t[i++]='0'; while (n) { t[i++]=(char)('0'+n%10); n/=10; } while (i) k[kl++]=t[--i]; k[kl]=0; }
    prop_set(obji, k, v);
}

static int to_bool(Value v)
{
    switch (v.type) {
        case V_UNDEF: case V_NULL: return 0;
        case V_NUM: case V_BOOL: return v.num != 0;
        case V_STR: { const char *s = S(v.soff); return s[0] != 0; }
        default: return 1;
    }
}
static int to_num(Value v)
{
    switch (v.type) {
        case V_NUM: case V_BOOL: return v.num;
        case V_STR: return parse_int_str(S(v.soff));
        default: return 0;
    }
}

/* descendant text of an element, into the js arena; returns offset */
static int elem_text_collect(DomNode *n, char *buf, int *bl, int cap)
{
    if (!n) return 0;
    if (n->type == DOM_NODE_TEXT) {
        const char *t = dom_text(n);
        for (int i = 0; t[i] && *bl < cap - 1; i++) buf[(*bl)++] = t[i];
        return 0;
    }

    if (j_streq(n->tag, "script") || j_streq(n->tag, "style")) return 0;
    for (DomNode *c = n->first_child; c; c = c->next_sibling)
        elem_text_collect(c, buf, bl, cap);
    return 0;
}
static int elem_text(DomNode *n)
{
    static char buf[8192];
    int bl = 0;
    elem_text_collect(n, buf, &bl, sizeof(buf));
    buf[bl] = 0;
    return str_intern(buf, bl);
}

static int to_str_off(Value v);

static int array_join_off(int obji, const char *sep)
{
    char buf[2048]; int o = 0;
    int n = (obji >= 0 && obji < g_nobjs) ? g_objs[obji].alen : 0;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        if (i) for (int z = 0; sep[z] && o < 2046; z++) buf[o++] = sep[z];
        Value e = arr_get(obji, i);
        if (e.type == V_UNDEF || e.type == V_NULL) continue;
        const char *es = S(to_str_off(e));
        for (int z = 0; es[z] && o < 2046; z++) buf[o++] = es[z];
    }
    buf[o] = 0;
    return str_intern(buf, o);
}

static int to_str_off(Value v)
{
    switch (v.type) {
        case V_STR:  return v.soff;
        case V_NUM:  return str_from_int(v.num);
        case V_BOOL: return str_intern(v.num ? "true" : "false", -1);
        case V_NULL: return str_intern("null", 4);
        case V_UNDEF:return str_intern("undefined", 9);
        case V_OBJ:
            if (v.objkind == O_ELEMENT) return elem_text(v.node);
            if (v.objkind == O_ARRAY)   return array_join_off(v.obji, ",");
            return str_intern("[object Object]", -1);
        default: return 0;
    }
}
static const char *to_cstr(Value v) { return S(to_str_off(v)); }

#define ENV_POOL 256
#define ENV_VARS 48
typedef struct Env {
    struct Env *parent;
    int   names[ENV_VARS];
    Value vals[ENV_VARS];
    int   count;
} Env;

static Env  g_envpool[ENV_POOL];
static int  g_nenv;
static int  g_globobj = -1;      /* globals live as props of this object, so
                                    the count is bounded by PROP_POOL, not
                                    by a tiny per-env array */

static Env *env_new(Env *parent)
{
    if (g_nenv >= ENV_POOL) { fail(); return 0; }
    Env *e = &g_envpool[g_nenv++];
    e->parent = parent; e->count = 0;
    return e;
}
static int env_index(Env *e) { return e ? (int)(e - g_envpool) : -1; }

static int name_eq(int a_off, int b_off)
{ return j_streq(S(a_off), S(b_off)); }

static Value *scope_find(Env *e, int name_off)
{
    for (Env *p = e; p; p = p->parent)
        for (int i = 0; i < p->count; i++)
            if (name_eq(p->names[i], name_off)) return &p->vals[i];
    return prop_find(g_globobj, S(name_off));
}
static void scope_define(Env *e, int name_off, Value v)
{
    if (!e) { prop_set(g_globobj, S(name_off), v); return; }
    for (int i = 0; i < e->count; i++)
        if (name_eq(e->names[i], name_off)) { e->vals[i] = v; return; }
    if (e->count >= ENV_VARS) { fail(); return; }
    e->names[e->count] = name_off; e->vals[e->count] = v; e->count++;
}
static void scope_assign(Env *e, int name_off, Value v)
{
    Value *slot = scope_find(e, name_off);
    if (slot) { *slot = v; return; }
    prop_set(g_globobj, S(name_off), v);
}

static int   g_signal;          /* 0 none, 1 return, 2 break, 3 continue */
static Value g_retval;
enum { SIG_NONE = 0, SIG_RETURN = 1, SIG_BREAK = 2, SIG_CONTINUE = 3 };

static char g_alert[200];
static int  g_alert_set;
static char g_nav[512];
static int  g_nav_set;
static char g_loc[512];

void js_set_location(const char *url)
{
    int i = 0;
    if (url) while (url[i] && i < (int)sizeof(g_loc) - 1) { g_loc[i] = url[i]; i++; }
    g_loc[i] = 0;
}

static void set_nav(const char *url)
{
    int i = 0;
    if (!url || !url[0]) return;
    while (url[i] && i < (int)sizeof(g_nav) - 1) { g_nav[i] = url[i]; i++; }
    g_nav[i] = 0;
    g_nav_set = 1;
}

static void set_alert(const char *msg)
{
    int i = 0;
    while (msg && msg[i] && i < (int)sizeof(g_alert) - 1) { g_alert[i] = msg[i]; i++; }
    g_alert[i] = 0;
    g_alert_set = 1;
}

void js_request_nav(const char *url) { set_nav(url); }

int js_take_nav(char *out, int cap)
{
    if (!g_nav_set || cap <= 1) return 0;
    int i = 0;
    while (g_nav[i] && i < cap - 1) { out[i] = g_nav[i]; i++; }
    out[i] = 0;
    g_nav_set = 0;
    return 1;
}

int js_take_alert(char *out, int cap)
{
    if (!g_alert_set || cap <= 1) return 0;
    int i = 0;
    while (g_alert[i] && i < cap - 1) { out[i] = g_alert[i]; i++; }
    out[i] = 0;
    g_alert_set = 0;
    return 1;
}

/* pieces of g_loc for the location object: proto includes "://", host stops
 * at '/', path runs to '?' or '#', search runs to '#' */
static int loc_part(const char *what)
{
    const char *u = g_loc;
    int scheme_end = 0;
    while (u[scheme_end] && u[scheme_end] != ':') scheme_end++;
    int has_scheme = u[scheme_end] == ':' && u[scheme_end+1] == '/' && u[scheme_end+2] == '/';
    int host_start = has_scheme ? scheme_end + 3 : 0;
    int host_end = host_start;
    while (u[host_end] && u[host_end] != '/' && u[host_end] != '?' && u[host_end] != '#') host_end++;
    int path_end = host_end;
    while (u[path_end] && u[path_end] != '?' && u[path_end] != '#') path_end++;
    int search_end = path_end;
    while (u[search_end] && u[search_end] != '#') search_end++;

    if (j_streq(what, "href"))     return str_intern(u, -1);
    if (j_streq(what, "protocol")) { char b[16]; int o=0; for (int i=0;i<scheme_end&&has_scheme&&o<14;i++) b[o++]=u[i]; if (has_scheme) b[o++]=':'; b[o]=0; return str_intern(b,o); }
    if (j_streq(what, "host") || j_streq(what, "hostname"))
        return str_intern(u + host_start, host_end - host_start);
    if (j_streq(what, "pathname")) {
        if (path_end == host_end) return str_intern("/", 1);
        return str_intern(u + host_end, path_end - host_end);
    }
    if (j_streq(what, "search")) return str_intern(u + path_end, search_end - path_end);
    if (j_streq(what, "hash"))   return str_intern(u + search_end, -1);
    if (j_streq(what, "origin")) return str_intern(u, host_end);
    return 0;
}

enum { EV_CLICK = 0, EV_LOAD = 1, EV_INPUT = 2, EV_CHANGE = 3, EV_SUBMIT = 4 };

#define HND_MAX 160
typedef struct { DomNode *node; int evt; Value fn; int used; } Handler;
static Handler g_handlers[HND_MAX];
static int     g_nhandlers;

static int evt_from_name(const char *n)
{
    if (j_strieq(n, "click")) return EV_CLICK;
    if (j_strieq(n, "load") || j_strieq(n, "DOMContentLoaded")) return EV_LOAD;
    if (j_strieq(n, "input")) return EV_INPUT;
    if (j_strieq(n, "change")) return EV_CHANGE;
    if (j_strieq(n, "submit")) return EV_SUBMIT;
    return -1;
}

/* replace=1 mirrors `elem.onclick = fn` (one slot per node+event);
 * replace=0 mirrors addEventListener (multiple listeners allowed) */
static void handler_add(DomNode *node, int evt, Value fn, int replace)
{
    if (evt < 0) return;
    if (fn.type != V_OBJ || (fn.objkind != O_FUNC && fn.objkind != O_BUILTIN)) return;
    if (replace) {
        for (int i = 0; i < g_nhandlers; i++)
            if (g_handlers[i].used && g_handlers[i].node == node && g_handlers[i].evt == evt) {
                g_handlers[i].fn = fn;
                return;
            }
    }
    if (g_nhandlers >= HND_MAX) return;
    g_handlers[g_nhandlers].node = node;
    g_handlers[g_nhandlers].evt = evt;
    g_handlers[g_nhandlers].fn = fn;
    g_handlers[g_nhandlers].used = 1;
    g_nhandlers++;
}

#define TMR_MAX 24
typedef struct { int active; unsigned due; int interval; Value fn; } Timer;
static Timer g_timers[TMR_MAX];

static int timer_add(Value fn, int ms, int interval)
{
    if (fn.type != V_OBJ && fn.type != V_STR) return 0;
    if (ms < 0) ms = 0;
    if (interval && ms < 50) ms = 50;   /* don't let setInterval(f,0) melt the frame loop */
    for (int i = 0; i < TMR_MAX; i++) {
        if (g_timers[i].active) continue;
        g_timers[i].active = 1;
        g_timers[i].due = clock_ms() + (unsigned)ms;
        g_timers[i].interval = interval ? ms : 0;
        g_timers[i].fn = fn;
        return i + 1;
    }
    return 0;
}

static void timer_clear(int id)
{
    if (id >= 1 && id <= TMR_MAX) g_timers[id - 1].active = 0;
}

static Value eval(AstNode *n, Env *e);
static void  exec(AstNode *n, Env *e);
static Value call_value(Value fv, Value *argv, int argc);

/* CSS-property camelCase -> kebab-case for element.style.X = v */
static void camel_to_kebab(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 2; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') { out[o++] = '-'; out[o++] = (char)(c + 32); }
        else out[o++] = c;
    }
    out[o] = 0;
}

static void style_set(DomNode *n, const char *prop_camel, const char *val)
{
    if (!n) return;
    char kebab[64]; camel_to_kebab(prop_camel, kebab, sizeof(kebab));
    const char *old = dom_get_attr(n, "style");
    char buf[1024]; int o = 0;
    for (int i = 0; old[i] && o < 900; i++) buf[o++] = old[i];
    if (o > 0 && buf[o-1] != ';') buf[o++] = ';';
    for (int i = 0; kebab[i] && o < 990; i++) buf[o++] = kebab[i];
    buf[o++] = ':';
    for (int i = 0; val[i] && o < 1020; i++) buf[o++] = val[i];
    buf[o++] = ';'; buf[o] = 0;
    dom_set_attr(n, "style", buf);
    g_dirty = 1;
}

/* --- classList add/remove/toggle/contains over the class attribute --- */

static int class_present(const char *cl, const char *want)
{
    int wl = j_slen(want), p = 0;
    while (cl[p]) {
        while (cl[p] == ' ') p++;
        int q = p; while (cl[q] && cl[q] != ' ') q++;
        if (q - p == wl) {
            int m = 1;
            for (int z = 0; z < wl; z++) if (cl[p+z] != want[z]) { m = 0; break; }
            if (m) return 1;
        }
        p = q;
    }
    return 0;
}

static void class_change(DomNode *n, const char *name, int add)
{
    const char *cl = dom_get_attr(n, "class");
    char buf[256]; int o = 0;
    int wl = j_slen(name), p = 0;
    while (cl[p]) {
        while (cl[p] == ' ') p++;
        int q = p; while (cl[q] && cl[q] != ' ') q++;
        int same = (q - p == wl);
        for (int z = 0; same && z < wl; z++) if (cl[p+z] != name[z]) same = 0;
        if (!same && q > p) {
            if (o > 0 && o < 254) buf[o++] = ' ';
            for (int z = p; z < q && o < 254; z++) buf[o++] = cl[z];
        }
        p = q;
    }
    if (add) {
        if (o > 0 && o < 254) buf[o++] = ' ';
        for (int z = 0; name[z] && o < 254; z++) buf[o++] = name[z];
    }
    buf[o] = 0;
    dom_set_attr(n, "class", buf);
    g_dirty = 1;
}

/* selector matcher shared by querySelector(All): "tag", ".class", "#id",
 * or "tag.class". Anything fancier matches nothing (bounded honesty). */
static int simple_sel_match(DomNode *n, const char *q)
{
    if (n->type != DOM_NODE_ELEMENT) return 0;
    if (q[0] == '#') return j_streq(dom_get_attr(n, "id"), q + 1);
    if (q[0] == '.') return class_present(dom_get_attr(n, "class"), q + 1);
    char tag[16]; int tl = 0;
    int i = 0;
    while (q[i] && q[i] != '.' && q[i] != '#' && tl < 15) tag[tl++] = j_lc(q[i++]);
    tag[tl] = 0;
    if (tl && !j_streq(n->tag, tag)) return 0;
    if (q[i] == '.') return class_present(dom_get_attr(n, "class"), q + i + 1);
    if (q[i] == '#') return j_streq(dom_get_attr(n, "id"), q + i + 1);
    return tl > 0;
}

/* DFS from root collecting matches; mode 0 = tag name ("*" = all elements),
 * 1 = class name, 2 = simple selector. Returns first match if first=1. */
static Value collect_elements(DomNode *root, const char *q, int mode, int first)
{
    Value arr = first ? v_undef() : v_new_array();
    if (!root) return first ? v_null() : arr;
    DomNode *st[256]; int sp = 0;
    st[sp++] = root;
    int found = 0;
    while (sp > 0 && found < 300) {
        DomNode *n = st[--sp];
        if (n->type == DOM_NODE_ELEMENT && n != root) {
            int m = 0;
            if (mode == 0) m = j_streq(q, "*") || j_strieq(n->tag, q);
            else if (mode == 1) m = class_present(dom_get_attr(n, "class"), q);
            else m = simple_sel_match(n, q);
            if (m) {
                if (first) return v_elem(n);
                arr_push(arr.obji, v_elem(n));
                found++;
            }
        }
        /* push children in reverse document order so pops come out in order */
        int nc = 0; DomNode *kids[64];
        for (DomNode *c = n->first_child; c && nc < 64; c = c->next_sibling) kids[nc++] = c;
        for (int i = nc - 1; i >= 0 && sp < 256; i--) st[sp++] = kids[i];
    }
    return first ? v_null() : arr;
}

static DomNode *find_title(void)
{
    DomNode *st[64]; int sp = 0;
    st[sp++] = dom_root();
    while (sp > 0) {
        DomNode *cn = st[--sp];
        if (cn->type == DOM_NODE_ELEMENT && j_streq(cn->tag, "title")) return cn;
        for (DomNode *c = cn->first_child; c && sp < 64; c = c->next_sibling) st[sp++] = c;
    }
    return 0;
}

static Value host_get(Value obj, const char *name)
{
    if (obj.type == V_STR) {
        if (j_streq(name, "length")) return v_num(j_slen(S(obj.soff)));
        int idx;
        if (parse_index(name, &idx)) {
            const char *s = S(obj.soff);
            if (idx < j_slen(s)) { char b[2] = { s[idx], 0 }; return v_str(str_intern(b, 1)); }
            return v_undef();
        }
        return v_undef();
    }
    if (obj.type != V_OBJ) return v_undef();

    /* script objects and arrays: plain property lookup */
    if (obj.objkind == O_ARRAY || obj.objkind == O_PLAIN) {
        if (obj.objkind == O_ARRAY && j_streq(name, "length"))
            return v_num(obj.obji >= 0 && obj.obji < g_nobjs ? g_objs[obj.obji].alen : 0);
        Value *p = prop_find(obj.obji, name);
        return p ? *p : v_undef();
    }

    if (obj.objkind == O_ELEMENT) {
        DomNode *n = obj.node;
        if (j_streq(name,"innerHTML") || j_streq(name,"textContent") || j_streq(name,"innerText"))
            return v_str(elem_text(n));
        if (j_streq(name,"id"))        return v_str(str_intern(dom_get_attr(n,"id"), -1));
        if (j_streq(name,"className")) return v_str(str_intern(dom_get_attr(n,"class"), -1));
        if (j_streq(name,"value"))     return v_str(str_intern(dom_get_attr(n,"value"), -1));
        if (j_streq(name,"href"))      return v_str(str_intern(dom_get_attr(n,"href"), -1));
        if (j_streq(name,"src"))       return v_str(str_intern(dom_get_attr(n,"src"), -1));
        if (j_streq(name,"alt"))       return v_str(str_intern(dom_get_attr(n,"alt"), -1));
        if (j_streq(name,"title"))     return v_str(str_intern(dom_get_attr(n,"title"), -1));
        if (j_streq(name,"name"))      return v_str(str_intern(dom_get_attr(n,"name"), -1));
        if (j_streq(name,"type"))      return v_str(str_intern(dom_get_attr(n,"type"), -1));
        if (j_streq(name,"checked"))   return v_bool(dom_has_attr(n,"checked"));
        if (j_streq(name,"disabled"))  return v_bool(dom_has_attr(n,"disabled"));
        if (j_streq(name,"tagName") || j_streq(name,"nodeName")) {
            char up[16]; int i = 0; const char *t = n->tag;
            for (; t[i] && i < 15; i++) up[i] = (t[i] >= 'a' && t[i] <= 'z') ? (char)(t[i]-32) : t[i];
            up[i] = 0; return v_str(str_intern(up, i));
        }
        if (j_streq(name,"nodeType"))  return v_num(n->type == DOM_NODE_ELEMENT ? 1 : 3);
        if (j_streq(name,"style"))     { Value v = v_obj(O_STYLE); v.node = n; return v; }
        if (j_streq(name,"classList")) { Value v = v_obj(O_CLASSLIST); v.node = n; return v; }
        if (j_streq(name,"parentNode") || j_streq(name,"parentElement"))
            return n->parent ? v_elem(n->parent) : v_null();
        if (j_streq(name,"firstChild"))
            return n->first_child ? v_elem(n->first_child) : v_null();
        if (j_streq(name,"firstElementChild")) {
            for (DomNode *c = n->first_child; c; c = c->next_sibling)
                if (c->type == DOM_NODE_ELEMENT) return v_elem(c);
            return v_null();
        }
        if (j_streq(name,"nextSibling"))
            return n->next_sibling ? v_elem(n->next_sibling) : v_null();
        if (j_streq(name,"nextElementSibling")) {
            for (DomNode *c = n->next_sibling; c; c = c->next_sibling)
                if (c->type == DOM_NODE_ELEMENT) return v_elem(c);
            return v_null();
        }
        if (j_streq(name,"children")) {
            Value arr = v_new_array();
            for (DomNode *c = n->first_child; c; c = c->next_sibling)
                if (c->type == DOM_NODE_ELEMENT) arr_push(arr.obji, v_elem(c));
            return arr;
        }
        if (j_streq(name,"childNodes")) {
            Value arr = v_new_array();
            for (DomNode *c = n->first_child; c; c = c->next_sibling)
                arr_push(arr.obji, v_elem(c));
            return arr;
        }
        if (j_streq(name,"offsetWidth") || j_streq(name,"offsetHeight") ||
            j_streq(name,"clientWidth") || j_streq(name,"clientHeight") ||
            j_streq(name,"scrollTop")   || j_streq(name,"scrollHeight")) return v_num(0);
        return v_undef();
    }
    if (obj.objkind == O_CLASSLIST) {
        if (j_streq(name,"length")) {
            const char *cl = dom_get_attr(obj.node, "class");
            int count = 0, p = 0;
            while (cl[p]) { while (cl[p]==' ') p++; if (cl[p]) count++; while (cl[p] && cl[p]!=' ') p++; }
            return v_num(count);
        }
        return v_undef();
    }
    if (obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"body")) return v_elem(dom_body());
        if (j_streq(name,"documentElement")) return v_elem(dom_root());
        if (j_streq(name,"location")) return v_obj(O_LOCATION);
        if (j_streq(name,"title")) {
            DomNode *t = find_title();
            return v_str(t ? elem_text(t) : 0);
        }
        if (j_streq(name,"cookie")) return v_str(0);
        if (j_streq(name,"readyState")) return v_str(str_intern("complete", -1));
        return v_undef();
    }
    if (obj.objkind == O_WINDOW) {
        if (j_streq(name,"document")) return v_obj(O_DOCUMENT);
        if (j_streq(name,"console"))  return v_obj(O_CONSOLE);
        if (j_streq(name,"Math"))     return v_obj(O_MATH);
        if (j_streq(name,"location")) return v_obj(O_LOCATION);
        if (j_streq(name,"innerWidth"))  return v_num(1280);
        if (j_streq(name,"innerHeight")) return v_num(720);
        if (j_streq(name,"navigator") || j_streq(name,"screen")) return v_new_plain();
        return v_undef();
    }
    if (obj.objkind == O_LOCATION) {
        int off = loc_part(name);
        if (off || j_streq(name,"href") || j_streq(name,"search") || j_streq(name,"hash"))
            return v_str(off);
        return v_undef();
    }
    if (obj.objkind == O_MATH) {
        if (j_streq(name,"PI")) return v_num(3);
        if (j_streq(name,"E"))  return v_num(2);
        return v_undef();
    }
    if (obj.objkind == O_BUILTIN) {
        if (obj.bid == B_DATE && j_streq(name,"now")) { Value v = v_obj(O_BUILTIN); v.bid = B_DATENOW; return v; }
        if (obj.bid == B_OBJECT && j_streq(name,"keys"))   { Value v = v_obj(O_BUILTIN); v.bid = B_OBJKEYS; return v; }
        if (obj.bid == B_OBJECT && j_streq(name,"values")) { Value v = v_obj(O_BUILTIN); v.bid = B_OBJVALUES; return v; }
        if (obj.bid == B_ARRAY && j_streq(name,"isArray")) { Value v = v_obj(O_BUILTIN); v.bid = B_ISARRAY; return v; }
        return v_undef();
    }
    if (obj.objkind == O_JSON) {
        if (j_streq(name,"stringify")) { Value v = v_obj(O_BUILTIN); v.bid = B_STRINGIFY; return v; }
        if (j_streq(name,"parse"))     { Value v = v_obj(O_BUILTIN); v.bid = B_NOOP; return v; }
        return v_undef();
    }
    if (obj.objkind == O_STYLE) return v_str(0);
    return v_undef();
}

static void host_set(Value obj, const char *name, Value val)
{
    if (obj.type != V_OBJ) return;

    if (obj.objkind == O_ARRAY || obj.objkind == O_PLAIN) {
        if (obj.objkind == O_ARRAY && j_streq(name, "length")) {
            if (obj.obji >= 0 && obj.obji < g_nobjs) g_objs[obj.obji].alen = to_num(val);
            return;
        }
        prop_set(obj.obji, name, val);
        return;
    }

    if (obj.objkind == O_ELEMENT) {
        DomNode *n = obj.node;
        if (j_streq(name,"innerHTML")) {
            const char *h = to_cstr(val);
            dom_parse_fragment(n, h, j_slen(h));
            g_dirty = 1; return;
        }
        if (j_streq(name,"textContent") || j_streq(name,"innerText")) {
            dom_remove_children(n);
            const char *t = to_cstr(val);
            DomNode *tn = dom_create_text(t, j_slen(t));
            if (tn) dom_append_child(n, tn);
            g_dirty = 1; return;
        }
        if (j_streq(name,"id"))        { dom_set_attr(n,"id", to_cstr(val)); return; }
        if (j_streq(name,"className")) { dom_set_attr(n,"class", to_cstr(val)); g_dirty = 1; return; }
        if (j_streq(name,"value"))     { dom_set_attr(n,"value", to_cstr(val)); g_dirty = 1; return; }
        if (j_streq(name,"href"))      { dom_set_attr(n,"href", to_cstr(val)); return; }
        if (j_streq(name,"src"))       { dom_set_attr(n,"src", to_cstr(val)); g_dirty = 1; return; }
        if (j_streq(name,"checked"))   { if (to_bool(val)) dom_set_attr(n,"checked",""); else dom_set_attr(n,"checked-off",""); g_dirty = 1; return; }
        if (j_streq(name,"onclick"))   { handler_add(n, EV_CLICK, val, 1); return; }
        if (j_streq(name,"onchange"))  { handler_add(n, EV_CHANGE, val, 1); return; }
        if (j_streq(name,"oninput"))   { handler_add(n, EV_INPUT, val, 1); return; }
        if (j_streq(name,"onload"))    { handler_add(n, EV_LOAD, val, 1); return; }
        return;
    }
    if (obj.objkind == O_STYLE) {
        style_set(obj.node, name, to_cstr(val));
        return;
    }
    if (obj.objkind == O_LOCATION) {
        if (j_streq(name,"href")) set_nav(to_cstr(val));
        return;
    }
    if (obj.objkind == O_WINDOW || obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"onload")) { handler_add(0, EV_LOAD, val, 1); return; }
        if (j_streq(name,"onclick")) { handler_add(0, EV_CLICK, val, 1); return; }
        if (obj.objkind == O_DOCUMENT && j_streq(name,"title")) {
            DomNode *t = find_title();
            if (t) {
                dom_remove_children(t);
                const char *s = to_cstr(val);
                DomNode *tn = dom_create_text(s, j_slen(s));
                if (tn) dom_append_child(t, tn);
            }
            return;
        }
        if (obj.objkind == O_DOCUMENT && j_streq(name,"cookie")) return;
        return;
    }
}

static int isqrt_i(int v) { if (v <= 0) return 0; int r = 0; while ((r+1)*(r+1) <= v) r++; return r; }
static int ipow_i(int b, int e) { int r = 1; while (e-- > 0) r *= b; return r; }

static int json_null(char *buf, int o, int cap)
{
    const char *s = "null";
    while (*s && o < cap - 1) buf[o++] = *s++;
    return o;
}

/* JSON.stringify, bounded and shallow (depth 3) */
static int json_out(Value v, char *buf, int o, int cap, int depth)
{
    if (o >= cap - 8) return o;
    switch (v.type) {
        case V_NUM: case V_BOOL: case V_NULL: {
            const char *s = v.type == V_NULL ? "null" : (v.type == V_BOOL ? (v.num ? "true" : "false") : S(str_from_int(v.num)));
            while (*s && o < cap - 1) buf[o++] = *s++;
            return o;
        }
        case V_STR: {
            const char *s = S(v.soff);
            buf[o++] = '"';
            for (int i = 0; s[i] && o < cap - 3; i++) {
                if (s[i] == '"' || s[i] == '\\') buf[o++] = '\\';
                buf[o++] = s[i];
            }
            buf[o++] = '"';
            return o;
        }
        case V_OBJ:
            if (depth <= 0) { const char *s = "null"; while (*s && o < cap-1) buf[o++] = *s++; return o; }
            if (v.objkind == O_ARRAY) {
                buf[o++] = '[';
                int n = (v.obji >= 0 && v.obji < g_nobjs) ? g_objs[v.obji].alen : 0;
                if (n > 64) n = 64;
                for (int i = 0; i < n; i++) {
                    if (i) buf[o++] = ',';
                    o = json_out(arr_get(v.obji, i), buf, o, cap, depth - 1);
                    if (o >= cap - 8) break;
                }
                if (o < cap - 1) buf[o++] = ']';
                return o;
            }
            if (v.objkind == O_PLAIN) {
                buf[o++] = '{';
                int first = 1;
                for (int p = (v.obji >= 0 && v.obji < g_nobjs) ? g_objs[v.obji].phead : -1;
                     p >= 0 && o < cap - 8; p = g_props[p].next) {
                    if (!first) buf[o++] = ',';
                    first = 0;
                    o = json_out(v_str(g_props[p].key), buf, o, cap, 1);
                    if (o < cap - 1) buf[o++] = ':';
                    o = json_out(g_props[p].val, buf, o, cap, depth - 1);
                }
                if (o < cap - 1) buf[o++] = '}';
                return o;
            }
            return json_null(buf, o, cap);
        default:
            return json_null(buf, o, cap);
    }
}

static Value host_method(Value obj, const char *name, Value *argv, int argc)
{

    if (obj.type == V_STR) {
        const char *s = S(obj.soff); int sl = j_slen(s);
        if (j_streq(name,"toUpperCase")) { char b[1024]; int i=0; for(;s[i]&&i<1023;i++) b[i]=(s[i]>='a'&&s[i]<='z')?(char)(s[i]-32):s[i]; b[i]=0; return v_str(str_intern(b,i)); }
        if (j_streq(name,"toLowerCase")) { char b[1024]; int i=0; for(;s[i]&&i<1023;i++) b[i]=j_lc(s[i]); b[i]=0; return v_str(str_intern(b,i)); }
        if (j_streq(name,"charAt"))      { int idx = argc>0?to_num(argv[0]):0; if (idx<0||idx>=sl) return v_str(0); char b[2]={s[idx],0}; return v_str(str_intern(b,1)); }
        if (j_streq(name,"charCodeAt") || j_streq(name,"codePointAt"))
            { int idx = argc>0?to_num(argv[0]):0; if (idx<0||idx>=sl) return v_num(0); return v_num((unsigned char)s[idx]); }
        if (j_streq(name,"indexOf"))     { const char *q = argc>0?to_cstr(argv[0]):""; int ql=j_slen(q); for(int i=0;i+ql<=sl;i++){int m=1;for(int z=0;z<ql;z++)if(s[i+z]!=q[z]){m=0;break;}if(m)return v_num(i);} return v_num(-1); }
        if (j_streq(name,"lastIndexOf")) { const char *q = argc>0?to_cstr(argv[0]):""; int ql=j_slen(q); for(int i=sl-ql;i>=0;i--){int m=1;for(int z=0;z<ql;z++)if(s[i+z]!=q[z]){m=0;break;}if(m)return v_num(i);} return v_num(-1); }
        if (j_streq(name,"includes"))    { const char *q = argc>0?to_cstr(argv[0]):""; int ql=j_slen(q); for(int i=0;i+ql<=sl;i++){int m=1;for(int z=0;z<ql;z++)if(s[i+z]!=q[z]){m=0;break;}if(m)return v_bool(1);} return v_bool(ql==0); }
        if (j_streq(name,"startsWith"))  { const char *q = argc>0?to_cstr(argv[0]):""; int i=0; while(q[i]){ if(s[i]!=q[i]) return v_bool(0); i++; } return v_bool(1); }
        if (j_streq(name,"endsWith"))    { const char *q = argc>0?to_cstr(argv[0]):""; int ql=j_slen(q); if(ql>sl) return v_bool(0); for(int z=0;z<ql;z++) if(s[sl-ql+z]!=q[z]) return v_bool(0); return v_bool(1); }
        if (j_streq(name,"substring") || j_streq(name,"substr") || j_streq(name,"slice")) {
            int a = argc>0?to_num(argv[0]):0; int b = argc>1?to_num(argv[1]):sl;
            if (j_streq(name,"substr")) { if (a < 0) a = sl + a; b = a + (argc>1?to_num(argv[1]):sl); }
            else if (j_streq(name,"slice")) { if (a < 0) a = sl + a; if (argc>1 && b < 0) b = sl + b; }
            if (a < 0) a = 0;
            if (b > sl) b = sl;
            if (b < a) b = a;
            char buf[1024]; int o=0; for(int i=a;i<b&&o<1023;i++) buf[o++]=s[i]; buf[o]=0; return v_str(str_intern(buf,o));
        }
        if (j_streq(name,"trim")) { int a=0,b=sl; while(a<b&&(s[a]==' '||s[a]=='\t'||s[a]=='\n'||s[a]=='\r'))a++; while(b>a&&(s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'||s[b-1]=='\r'))b--; char buf[1024]; int o=0; for(int i=a;i<b&&o<1023;i++)buf[o++]=s[i]; buf[o]=0; return v_str(str_intern(buf,o)); }
        if (j_streq(name,"split")) {
            Value arr = v_new_array();
            const char *sep = argc>0 ? to_cstr(argv[0]) : 0;
            if (!sep || !argc) { arr_push(arr.obji, obj); return arr; }
            int spl = j_slen(sep);
            if (spl == 0) {
                for (int i = 0; i < sl && i < 64; i++) { char b[2]={s[i],0}; arr_push(arr.obji, v_str(str_intern(b,1))); }
                return arr;
            }
            int start = 0, parts = 0;
            for (int i = 0; i + spl <= sl && parts < 63; i++) {
                int m = 1;
                for (int z = 0; z < spl; z++) if (s[i+z] != sep[z]) { m = 0; break; }
                if (m) { arr_push(arr.obji, v_str(str_intern(s + start, i - start))); parts++; start = i + spl; i += spl - 1; }
            }
            arr_push(arr.obji, v_str(str_intern(s + start, sl - start)));
            return arr;
        }
        if (j_streq(name,"replace") || j_streq(name,"replaceAll")) {
            const char *q = argc>0?to_cstr(argv[0]):"";
            const char *r = argc>1?to_cstr(argv[1]):"";
            int ql = j_slen(q);
            if (!ql) return obj;
            char buf[2048]; int o = 0, i = 0, all = j_streq(name,"replaceAll"), done = 0;
            while (i < sl && o < 2046) {
                int m = (!done || all) && i + ql <= sl;
                for (int z = 0; m && z < ql; z++) if (s[i+z] != q[z]) m = 0;
                if (m) { for (int z = 0; r[z] && o < 2046; z++) buf[o++] = r[z]; i += ql; done = 1; }
                else buf[o++] = s[i++];
            }
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        if (j_streq(name,"concat")) {
            char buf[2048]; int o = 0;
            for (int i = 0; s[i] && o < 2046; i++) buf[o++] = s[i];
            for (int a2 = 0; a2 < argc; a2++) {
                const char *x = to_cstr(argv[a2]);
                for (int i = 0; x[i] && o < 2046; i++) buf[o++] = x[i];
            }
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        if (j_streq(name,"repeat")) {
            int cnt = argc>0?to_num(argv[0]):0; if (cnt < 0) cnt = 0; if (cnt > 64) cnt = 64;
            char buf[2048]; int o = 0;
            for (int c2 = 0; c2 < cnt; c2++) for (int i = 0; s[i] && o < 2046; i++) buf[o++] = s[i];
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        if (j_streq(name,"localeCompare")) {
            const char *b = argc>0?to_cstr(argv[0]):"";
            int i = 0; while (s[i] && b[i] && s[i]==b[i]) i++;
            int d = (unsigned char)s[i] - (unsigned char)b[i];
            return v_num(d < 0 ? -1 : d > 0 ? 1 : 0);
        }
        if (j_streq(name,"toString")) return obj;
        return v_undef();
    }
    if (obj.type == V_NUM || obj.type == V_BOOL) {
        if (j_streq(name,"toString") || j_streq(name,"toFixed")) return v_str(str_from_int(obj.num));
        return v_undef();
    }
    if (obj.type != V_OBJ) return v_undef();

    if (obj.objkind == O_ARRAY) {
        int oi = obj.obji;
        int len = (oi >= 0 && oi < g_nobjs) ? g_objs[oi].alen : 0;
        if (j_streq(name,"push")) { for (int i = 0; i < argc; i++) arr_push(oi, argv[i]); return v_num(len + argc); }
        if (j_streq(name,"pop")) {
            if (len <= 0) return v_undef();
            Value last = arr_get(oi, len - 1);
            g_objs[oi].alen = len - 1;
            return last;
        }
        if (j_streq(name,"shift")) {
            if (len <= 0) return v_undef();
            Value first = arr_get(oi, 0);
            for (int i = 1; i < len && i < 4096; i++) {
                char k[16]; int kl = 0; { int v2=i-1; char t[16]; int z=0; if(!v2)t[z++]='0'; while(v2){t[z++]=(char)('0'+v2%10);v2/=10;} while(z)k[kl++]=t[--z]; k[kl]=0; }
                prop_set(oi, k, arr_get(oi, i));
            }
            g_objs[oi].alen = len - 1;
            return first;
        }
        if (j_streq(name,"join")) return v_str(array_join_off(oi, argc>0 ? to_cstr(argv[0]) : ","));
        if (j_streq(name,"indexOf")) {
            for (int i = 0; i < len && i < 4096; i++) {
                Value e = arr_get(oi, i);
                if (e.type == argv[0].type && to_num(e) == to_num(argv[0]) &&
                    (e.type != V_STR || j_streq(S(e.soff), S(argv[0].soff)))) return v_num(i);
            }
            return v_num(-1);
        }
        if (j_streq(name,"includes")) {
            Value r = host_method(obj, "indexOf", argv, argc);
            return v_bool(r.num >= 0);
        }
        if (j_streq(name,"slice")) {
            int a = argc>0?to_num(argv[0]):0, b = argc>1?to_num(argv[1]):len;
            if (a < 0) a = len + a;
            if (b < 0) b = len + b;
            if (a < 0) a = 0;
            if (b > len) b = len;
            Value out = v_new_array();
            for (int i = a; i < b && i < 4096; i++) arr_push(out.obji, arr_get(oi, i));
            return out;
        }
        if (j_streq(name,"concat")) {
            Value out = v_new_array();
            for (int i = 0; i < len && i < 4096; i++) arr_push(out.obji, arr_get(oi, i));
            for (int a2 = 0; a2 < argc; a2++) {
                if (argv[a2].type == V_OBJ && argv[a2].objkind == O_ARRAY) {
                    int l2 = g_objs[argv[a2].obji].alen;
                    for (int i = 0; i < l2 && i < 4096; i++) arr_push(out.obji, arr_get(argv[a2].obji, i));
                } else arr_push(out.obji, argv[a2]);
            }
            return out;
        }
        if (j_streq(name,"reverse")) {
            for (int i = 0; i < len / 2; i++) {
                Value a = arr_get(oi, i), b = arr_get(oi, len - 1 - i);
                char k[16]; int kl;
                kl = 0; { int v2=i; char t[16]; int z=0; if(!v2)t[z++]='0'; while(v2){t[z++]=(char)('0'+v2%10);v2/=10;} while(z)k[kl++]=t[--z]; k[kl]=0; }
                prop_set(oi, k, b);
                kl = 0; { int v2=len-1-i; char t[16]; int z=0; if(!v2)t[z++]='0'; while(v2){t[z++]=(char)('0'+v2%10);v2/=10;} while(z)k[kl++]=t[--z]; k[kl]=0; }
                prop_set(oi, k, a);
            }
            return obj;
        }
        if (j_streq(name,"forEach") || j_streq(name,"map") || j_streq(name,"filter") || j_streq(name,"find")) {
            if (argc < 1) return v_undef();
            Value out = v_undef();
            int want_map = j_streq(name,"map"), want_filter = j_streq(name,"filter"), want_find = j_streq(name,"find");
            if (want_map || want_filter) out = v_new_array();
            for (int i = 0; i < len && i < 4096 && !g_err; i++) {
                if (budget()) break;
                Value args[2] = { arr_get(oi, i), v_num(i) };
                Value r = call_value(argv[0], args, 2);
                if (want_map) arr_push(out.obji, r);
                else if (want_filter) { if (to_bool(r)) arr_push(out.obji, args[0]); }
                else if (want_find) { if (to_bool(r)) return args[0]; }
            }
            return out;
        }
        return v_undef();
    }
    if (obj.objkind == O_PLAIN) {
        if (j_streq(name,"getTime") || j_streq(name,"valueOf")) {
            Value *ms = prop_find(obj.obji, "__ms");
            if (ms) return *ms;
        }
        if (j_streq(name,"hasOwnProperty"))
            return v_bool(argc > 0 && prop_find(obj.obji, to_cstr(argv[0])) != 0);
        if (j_streq(name,"toString")) return v_str(to_str_off(obj));
        Value *m = prop_find(obj.obji, name);
        if (m) return call_value(*m, argv, argc);
        return v_undef();
    }

    if (obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"getElementById")) {
            DomNode *n = dom_get_element_by_id(argc>0?to_cstr(argv[0]):"");
            return n ? v_elem(n) : v_null();
        }
        if (j_streq(name,"querySelector"))
            return collect_elements(dom_root(), argc>0?to_cstr(argv[0]):"", 2, 1);
        if (j_streq(name,"querySelectorAll"))
            return collect_elements(dom_root(), argc>0?to_cstr(argv[0]):"", 2, 0);
        if (j_streq(name,"getElementsByTagName"))
            return collect_elements(dom_root(), argc>0?to_cstr(argv[0]):"*", 0, 0);
        if (j_streq(name,"getElementsByClassName"))
            return collect_elements(dom_root(), argc>0?to_cstr(argv[0]):"", 1, 0);
        if (j_streq(name,"write") || j_streq(name,"writeln")) {
            for (int i = 0; i < argc; i++) { const char *h = to_cstr(argv[i]); dom_parse_append(dom_body(), h, j_slen(h)); }
            if (j_streq(name,"writeln")) dom_parse_append(dom_body(), "<br>", 4);
            g_dirty = 1; return v_undef();
        }
        if (j_streq(name,"createElement")) {
            DomNode *n = dom_create_element(argc>0?to_cstr(argv[0]):"div");
            return n ? v_elem(n) : v_null();
        }
        if (j_streq(name,"createTextNode")) {
            const char *t = argc>0?to_cstr(argv[0]):"";
            DomNode *n = dom_create_text(t, j_slen(t));
            return n ? v_elem(n) : v_null();
        }
        if (j_streq(name,"createDocumentFragment")) {
            DomNode *n = dom_create_element("div");
            return n ? v_elem(n) : v_null();
        }
        if (j_streq(name,"addEventListener")) {
            if (argc >= 2) handler_add(0, evt_from_name(to_cstr(argv[0])), argv[1], 0);
            return v_undef();
        }
        return v_undef();
    }
    if (obj.objkind == O_ELEMENT) {
        DomNode *n = obj.node;
        if (j_streq(name,"setAttribute")) { if (argc>=2) dom_set_attr(n, to_cstr(argv[0]), to_cstr(argv[1])); g_dirty=1; return v_undef(); }
        if (j_streq(name,"getAttribute")) { return v_str(str_intern(dom_get_attr(n, argc>0?to_cstr(argv[0]):""), -1)); }
        if (j_streq(name,"hasAttribute")) { return v_bool(dom_has_attr(n, argc>0?to_cstr(argv[0]):"")); }
        if (j_streq(name,"removeAttribute")) { if (argc>0) dom_set_attr(n, to_cstr(argv[0]), ""); g_dirty=1; return v_undef(); }
        if (j_streq(name,"appendChild") || j_streq(name,"append")) {
            if (argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ELEMENT) { dom_append_child(n, argv[0].node); g_dirty=1; return argv[0]; }
            if (argc>0 && argv[0].type==V_STR) { DomNode *t = dom_create_text(S(argv[0].soff), -1); if (t) { dom_append_child(n, t); g_dirty=1; } }
            return v_undef();
        }
        if (j_streq(name,"insertBefore")) {
            if (argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ELEMENT) { dom_append_child(n, argv[0].node); g_dirty=1; return argv[0]; }
            return v_undef();
        }
        if (j_streq(name,"removeChild")) {
            if (argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ELEMENT) {
                DomNode *kid = argv[0].node, *prev = 0;
                for (DomNode *c = n->first_child; c; prev = c, c = c->next_sibling) {
                    if (c != kid) continue;
                    if (prev) prev->next_sibling = c->next_sibling;
                    else n->first_child = c->next_sibling;
                    if (n->last_child == c) n->last_child = prev;
                    c->parent = 0; c->next_sibling = 0;
                    g_dirty = 1;
                    break;
                }
                return argv[0];
            }
            return v_undef();
        }
        if (j_streq(name,"remove")) {
            if (n && n->parent) {
                Value parent = v_elem(n->parent), self = obj;
                host_method(parent, "removeChild", &self, 1);
            }
            return v_undef();
        }
        if (j_streq(name,"querySelector"))
            return collect_elements(n, argc>0?to_cstr(argv[0]):"", 2, 1);
        if (j_streq(name,"querySelectorAll"))
            return collect_elements(n, argc>0?to_cstr(argv[0]):"", 2, 0);
        if (j_streq(name,"getElementsByTagName"))
            return collect_elements(n, argc>0?to_cstr(argv[0]):"*", 0, 0);
        if (j_streq(name,"getElementsByClassName"))
            return collect_elements(n, argc>0?to_cstr(argv[0]):"", 1, 0);
        if (j_streq(name,"addEventListener")) {
            if (argc >= 2) handler_add(n, evt_from_name(to_cstr(argv[0])), argv[1], 0);
            return v_undef();
        }
        if (j_streq(name,"removeEventListener")) {
            if (argc >= 1) {
                int evt = evt_from_name(to_cstr(argv[0]));
                for (int i = 0; i < g_nhandlers; i++)
                    if (g_handlers[i].node == n && g_handlers[i].evt == evt) g_handlers[i].used = 0;
            }
            return v_undef();
        }
        if (j_streq(name,"click")) { js_click(n); return v_undef(); }
        if (j_streq(name,"focus") || j_streq(name,"blur") || j_streq(name,"scrollIntoView")) return v_undef();
        return v_undef();
    }
    if (obj.objkind == O_CLASSLIST) {
        DomNode *n = obj.node;
        const char *cn = argc>0 ? to_cstr(argv[0]) : "";
        if (j_streq(name,"add"))      { if (!class_present(dom_get_attr(n,"class"), cn)) class_change(n, cn, 1); return v_undef(); }
        if (j_streq(name,"remove"))   { class_change(n, cn, 0); return v_undef(); }
        if (j_streq(name,"toggle"))   { int has = class_present(dom_get_attr(n,"class"), cn); class_change(n, cn, !has); return v_bool(!has); }
        if (j_streq(name,"contains")) { return v_bool(class_present(dom_get_attr(n,"class"), cn)); }
        return v_undef();
    }
    if (obj.objkind == O_CONSOLE) {
        /* evaluate-only; no visible output to avoid polluting pages */
        return v_undef();
    }
    if (obj.objkind == O_MATH) {
        int a = argc>0?to_num(argv[0]):0, b = argc>1?to_num(argv[1]):0;
        if (j_streq(name,"floor")||j_streq(name,"round")||j_streq(name,"ceil")||j_streq(name,"trunc")) return v_num(a);
        if (j_streq(name,"abs")) return v_num(a<0?-a:a);
        if (j_streq(name,"max")) { int m=a; for(int i=1;i<argc;i++){int x=to_num(argv[i]); if(x>m)m=x;} return v_num(argc?m:0); }
        if (j_streq(name,"min")) { int m=a; for(int i=1;i<argc;i++){int x=to_num(argv[i]); if(x<m)m=x;} return v_num(argc?m:0); }
        if (j_streq(name,"pow")) return v_num(ipow_i(a,b));
        if (j_streq(name,"sqrt")) return v_num(isqrt_i(a));
        if (j_streq(name,"random")) return v_num(0);
        return v_num(0);
    }
    if (obj.objkind == O_LOCATION) {
        if (j_streq(name,"assign") || j_streq(name,"replace")) { if (argc>0) set_nav(to_cstr(argv[0])); return v_undef(); }
        if (j_streq(name,"reload")) return v_undef();
        if (j_streq(name,"toString")) return v_str(loc_part("href"));
        return v_undef();
    }
    if (obj.objkind == O_WINDOW) {
        if (j_streq(name,"alert"))   { set_alert(argc>0?to_cstr(argv[0]):""); return v_undef(); }
        if (j_streq(name,"confirm")) { return v_bool(1); }
        if (j_streq(name,"prompt"))  { return v_str(0); }
        if (j_streq(name,"open"))    { if (argc>0) set_nav(to_cstr(argv[0])); return v_undef(); }
        if (j_streq(name,"setTimeout"))  return v_num(timer_add(argc>0?argv[0]:v_undef(), argc>1?to_num(argv[1]):0, 0));
        if (j_streq(name,"setInterval")) return v_num(timer_add(argc>0?argv[0]:v_undef(), argc>1?to_num(argv[1]):0, 1));
        if (j_streq(name,"clearTimeout") || j_streq(name,"clearInterval")) { if (argc>0) timer_clear(to_num(argv[0])); return v_undef(); }
        if (j_streq(name,"requestAnimationFrame")) return v_num(timer_add(argc>0?argv[0]:v_undef(), 16, 0));
        if (j_streq(name,"addEventListener")) {
            if (argc >= 2) handler_add(0, evt_from_name(to_cstr(argv[0])), argv[1], 0);
            return v_undef();
        }
        if (j_streq(name,"getComputedStyle")) {
            if (argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ELEMENT) { Value v = v_obj(O_STYLE); v.node = argv[0].node; return v; }
            return v_obj(O_STYLE);
        }
        if (j_streq(name,"scrollTo") || j_streq(name,"scrollBy") || j_streq(name,"focus")) return v_undef();
        return v_undef();
    }
    return v_undef();
}

static Value call_builtin(int bid, Value *argv, int argc)
{
    switch (bid) {
        case B_PARSEINT: {
            if (argc == 0) return v_num(0);
            int radix = argc > 1 ? to_num(argv[1]) : 0;
            if (argv[0].type == V_STR) return v_num(parse_int_radix(S(argv[0].soff), radix));
            return v_num(to_num(argv[0]));
        }
        case B_PARSEFLOAT: return v_num(argc>0?to_num(argv[0]):0);
        case B_STRING:   return v_str(argc>0?to_str_off(argv[0]):0);
        case B_NUMBER:   return v_num(argc>0?to_num(argv[0]):0);
        case B_BOOLEAN:  return v_bool(argc>0?to_bool(argv[0]):0);
        case B_ISNAN: {
            if (argc == 0) return v_bool(1);
            if (argv[0].type == V_NUM || argv[0].type == V_BOOL) return v_bool(0);
            if (argv[0].type == V_STR) {
                const char *s = S(argv[0].soff);
                int i = 0; while (s[i]==' ') i++;
                if (s[i]=='-'||s[i]=='+') i++;
                int digits = 0; while (s[i]>='0'&&s[i]<='9') { i++; digits++; }
                while (s[i]==' ') i++;
                return v_bool(!digits || s[i] != 0);
            }
            return v_bool(1);
        }
        case B_ALERT:   set_alert(argc>0?to_cstr(argv[0]):""); return v_undef();
        case B_CONFIRM: return v_bool(1);
        case B_PROMPT:  return v_str(0);
        case B_SETTIMEOUT:  return v_num(timer_add(argc>0?argv[0]:v_undef(), argc>1?to_num(argv[1]):0, 0));
        case B_SETINTERVAL: return v_num(timer_add(argc>0?argv[0]:v_undef(), argc>1?to_num(argv[1]):0, 1));
        case B_CLEARTMR: if (argc>0) timer_clear(to_num(argv[0])); return v_undef();
        case B_RAF: return v_num(timer_add(argc>0?argv[0]:v_undef(), 16, 0));
        case B_ENCURI: {
            const char *s = argc>0?to_cstr(argv[0]):"";
            const char *hex = "0123456789ABCDEF";
            char buf[1024]; int o = 0;
            for (int i = 0; s[i] && o < 1020; i++) {
                char c = s[i];
                int keep = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
                if (keep) buf[o++] = c;
                else { buf[o++]='%'; buf[o++]=hex[(c>>4)&0xF]; buf[o++]=hex[c&0xF]; }
            }
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        case B_DECURI: {
            const char *s = argc>0?to_cstr(argv[0]):"";
            char buf[1024]; int o = 0;
            for (int i = 0; s[i] && o < 1022; i++) {
                if (s[i]=='%' && s[i+1] && s[i+2]) {
                    int hi = s[i+1]>='a'?s[i+1]-'a'+10:s[i+1]>='A'?s[i+1]-'A'+10:s[i+1]-'0';
                    int lo = s[i+2]>='a'?s[i+2]-'a'+10:s[i+2]>='A'?s[i+2]-'A'+10:s[i+2]-'0';
                    buf[o++] = (char)((hi<<4)|lo); i += 2;
                } else if (s[i]=='+') buf[o++] = ' ';
                else buf[o++] = s[i];
            }
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        case B_DATE: {
            Value d = v_new_plain();
            prop_set(d.obji, "__ms", v_num((int)clock_ms()));
            return d;
        }
        case B_DATENOW: return v_num((int)clock_ms());
        case B_OBJKEYS: case B_OBJVALUES: {
            Value out = v_new_array();
            if (argc>0 && argv[0].type==V_OBJ && argv[0].obji >= 0) {
                for (int p = g_objs[argv[0].obji].phead; p >= 0; p = g_props[p].next)
                    arr_push(out.obji, bid == B_OBJKEYS ? v_str(g_props[p].key) : g_props[p].val);
            }
            return out;
        }
        case B_ISARRAY: return v_bool(argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ARRAY);
        case B_STRINGIFY: {
            if (argc == 0) return v_str(0);
            char buf[2048];
            int o = json_out(argv[0], buf, 0, sizeof(buf), 3);
            buf[o] = 0;
            return v_str(str_intern(buf, o));
        }
        case B_OBJECT: return v_new_plain();
        case B_ARRAY: {
            Value arr = v_new_array();
            for (int i = 0; i < argc; i++) arr_push(arr.obji, argv[i]);
            return arr;
        }
        case B_NOOP:     return v_undef();
    }
    return v_undef();
}

static Value call_user(Value fv, Value *argv, int argc)
{
    if (!fv.fn || g_depth >= DEPTH_MAX) { fail(); return v_undef(); }
    g_depth++;
    Env *parent = (fv.envi >= 0 && fv.envi < g_nenv) ? &g_envpool[fv.envi] : 0;
    Env *e = env_new(parent);
    if (!e) { g_depth--; return v_undef(); }
    int i = 0;
    for (AstNode *p = fv.fn->list; p; p = p->next, i++)
        scope_define(e, p->soff, i < argc ? argv[i] : v_undef());
    int saved_signal = g_signal;
    g_signal = SIG_NONE;
    exec(fv.fn->a, e);
    Value rv = (g_signal == SIG_RETURN) ? g_retval : v_undef();
    g_signal = saved_signal == SIG_RETURN ? SIG_NONE : saved_signal;
    if (g_signal == SIG_BREAK || g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
    g_depth--;
    return rv;
}

static Value call_value(Value fv, Value *argv, int argc)
{
    if (fv.type == V_OBJ && fv.objkind == O_FUNC) return call_user(fv, argv, argc);
    if (fv.type == V_OBJ && fv.objkind == O_BUILTIN) return call_builtin(fv.bid, argv, argc);
    return v_undef();
}

/* resolve a bare identifier to host globals / builtins when not a variable */
static int global_value(const char *nm, Value *out)
{
    if (j_streq(nm,"document")) { *out = v_obj(O_DOCUMENT); return 1; }
    if (j_streq(nm,"console"))  { *out = v_obj(O_CONSOLE); return 1; }
    if (j_streq(nm,"window") || j_streq(nm,"self") || j_streq(nm,"globalThis") || j_streq(nm,"top") || j_streq(nm,"this")) { *out = v_obj(O_WINDOW); return 1; }
    if (j_streq(nm,"Math"))     { *out = v_obj(O_MATH); return 1; }
    if (j_streq(nm,"JSON"))     { *out = v_obj(O_JSON); return 1; }
    if (j_streq(nm,"location")) { *out = v_obj(O_LOCATION); return 1; }
    if (j_streq(nm,"undefined")){ *out = v_undef(); return 1; }
    if (j_streq(nm,"NaN"))      { *out = v_num(0); return 1; }
    if (j_streq(nm,"Infinity")) { *out = v_num(2147483647); return 1; }
    if (j_streq(nm,"parseInt"))  { Value v=v_obj(O_BUILTIN); v.bid=B_PARSEINT; *out=v; return 1; }
    if (j_streq(nm,"parseFloat")){ Value v=v_obj(O_BUILTIN); v.bid=B_PARSEFLOAT; *out=v; return 1; }
    if (j_streq(nm,"String"))    { Value v=v_obj(O_BUILTIN); v.bid=B_STRING; *out=v; return 1; }
    if (j_streq(nm,"Number"))    { Value v=v_obj(O_BUILTIN); v.bid=B_NUMBER; *out=v; return 1; }
    if (j_streq(nm,"Boolean"))   { Value v=v_obj(O_BUILTIN); v.bid=B_BOOLEAN; *out=v; return 1; }
    if (j_streq(nm,"isNaN"))     { Value v=v_obj(O_BUILTIN); v.bid=B_ISNAN; *out=v; return 1; }
    if (j_streq(nm,"alert"))     { Value v=v_obj(O_BUILTIN); v.bid=B_ALERT; *out=v; return 1; }
    if (j_streq(nm,"confirm"))   { Value v=v_obj(O_BUILTIN); v.bid=B_CONFIRM; *out=v; return 1; }
    if (j_streq(nm,"prompt"))    { Value v=v_obj(O_BUILTIN); v.bid=B_PROMPT; *out=v; return 1; }
    if (j_streq(nm,"setTimeout")) { Value v=v_obj(O_BUILTIN); v.bid=B_SETTIMEOUT; *out=v; return 1; }
    if (j_streq(nm,"setInterval")){ Value v=v_obj(O_BUILTIN); v.bid=B_SETINTERVAL; *out=v; return 1; }
    if (j_streq(nm,"clearTimeout") || j_streq(nm,"clearInterval")) { Value v=v_obj(O_BUILTIN); v.bid=B_CLEARTMR; *out=v; return 1; }
    if (j_streq(nm,"requestAnimationFrame")) { Value v=v_obj(O_BUILTIN); v.bid=B_RAF; *out=v; return 1; }
    if (j_streq(nm,"encodeURIComponent") || j_streq(nm,"encodeURI")) { Value v=v_obj(O_BUILTIN); v.bid=B_ENCURI; *out=v; return 1; }
    if (j_streq(nm,"decodeURIComponent") || j_streq(nm,"decodeURI")) { Value v=v_obj(O_BUILTIN); v.bid=B_DECURI; *out=v; return 1; }
    if (j_streq(nm,"Date"))      { Value v=v_obj(O_BUILTIN); v.bid=B_DATE; *out=v; return 1; }
    if (j_streq(nm,"Object"))    { Value v=v_obj(O_BUILTIN); v.bid=B_OBJECT; *out=v; return 1; }
    if (j_streq(nm,"Array"))     { Value v=v_obj(O_BUILTIN); v.bid=B_ARRAY; *out=v; return 1; }
    return 0;
}

static int streq2(const char *a, const char *b) { return j_streq(a,b); }

static int compare_op(int op, Value l, Value r)
{
    /* string compare if both strings; else numeric */
    if (l.type == V_STR && r.type == V_STR) {
        const char *a = S(l.soff), *b = S(r.soff);
        int i = 0; while (a[i] && b[i] && a[i]==b[i]) i++;
        int d = (unsigned char)a[i] - (unsigned char)b[i];
        switch (op) { case OP_LT: return d<0; case OP_GT: return d>0; case OP_LE: return d<=0; case OP_GE: return d>=0; }
    }
    int a = to_num(l), b = to_num(r);
    switch (op) { case OP_LT: return a<b; case OP_GT: return a>b; case OP_LE: return a<=b; case OP_GE: return a>=b; }
    return 0;
}

static int loose_eq(Value l, Value r)
{
    if (l.type == V_STR || r.type == V_STR) {
        if ((l.type==V_NULL||l.type==V_UNDEF) || (r.type==V_NULL||r.type==V_UNDEF))
            return (l.type==V_NULL||l.type==V_UNDEF) && (r.type==V_NULL||r.type==V_UNDEF);
        return streq2(to_cstr(l), to_cstr(r));
    }
    if ((l.type==V_NULL||l.type==V_UNDEF) || (r.type==V_NULL||r.type==V_UNDEF))
        return (l.type==V_NULL||l.type==V_UNDEF) && (r.type==V_NULL||r.type==V_UNDEF);
    if (l.type==V_OBJ || r.type==V_OBJ)
        return l.type==V_OBJ && r.type==V_OBJ && l.node==r.node && l.objkind==r.objkind && l.obji==r.obji;
    return to_num(l) == to_num(r);
}
static int strict_eq(Value l, Value r)
{
    if (l.type != r.type) return 0;
    switch (l.type) {
        case V_STR: return streq2(S(l.soff), S(r.soff));
        case V_NUM: case V_BOOL: return l.num == r.num;
        case V_NULL: case V_UNDEF: return 1;
        case V_OBJ: return l.node==r.node && l.objkind==r.objkind && l.obji==r.obji;
    }
    return 0;
}

static Value binop(int op, Value l, Value r)
{
    switch (op) {
        case OP_ADD:
            if (l.type==V_STR || r.type==V_STR || l.type==V_OBJ || r.type==V_OBJ) {
                const char *a = to_cstr(l); int al = j_slen(a);
                const char *b = to_cstr(r); int bl = j_slen(b);
                char buf[4096]; int o=0;
                for (int i=0;i<al&&o<4095;i++) buf[o++]=a[i];
                for (int i=0;i<bl&&o<4095;i++) buf[o++]=b[i];
                buf[o]=0; return v_str(str_intern(buf,o));
            }
            return v_num(to_num(l)+to_num(r));
        case OP_SUB: return v_num(to_num(l)-to_num(r));
        case OP_MUL: return v_num(to_num(l)*to_num(r));
        case OP_DIV: { int d=to_num(r); return v_num(d?to_num(l)/d:0); }
        case OP_MOD: { int d=to_num(r); return v_num(d?to_num(l)%d:0); }
        case OP_BAND: return v_num(to_num(l)&to_num(r));
        case OP_BOR:  return v_num(to_num(l)|to_num(r));
        case OP_BXOR: return v_num(to_num(l)^to_num(r));
        case OP_SHL:  return v_num(to_num(l)<<(to_num(r)&31));
        case OP_SHR:  return v_num(to_num(l)>>(to_num(r)&31));
        case OP_EQ:  return v_bool(loose_eq(l,r));
        case OP_NE:  return v_bool(!loose_eq(l,r));
        case OP_SEQ: return v_bool(strict_eq(l,r));
        case OP_SNE: return v_bool(!strict_eq(l,r));
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: return v_bool(compare_op(op,l,r));
    }
    return v_undef();
}

static void assign_to(AstNode *target, Value val, Env *e)
{
    if (!target) return;
    if (target->kind == A_IDENT) { scope_assign(e, target->soff, val); return; }
    if (target->kind == A_MEMBER) {
        Value obj = eval(target->a, e);
        host_set(obj, S(target->soff), val);
        return;
    }
    if (target->kind == A_INDEX) {
        Value obj = eval(target->a, e);
        Value k = eval(target->b, e);
        host_set(obj, to_cstr(k), val);
        return;
    }
}
static Value read_target(AstNode *target, Env *e)
{
    if (!target) return v_undef();
    if (target->kind == A_IDENT) {
        Value *slot = scope_find(e, target->soff);
        if (slot) return *slot;
        Value g; if (global_value(S(target->soff), &g)) return g;
        return v_undef();
    }
    return eval(target, e);
}

static Value eval(AstNode *n, Env *e)
{
    if (!n || g_err) return v_undef();
    if (++g_depth > DEPTH_MAX) { g_depth--; fail(); return v_undef(); }
    Value result = v_undef();
    switch (n->kind) {
        case A_NUM:  result = v_num(n->ival); break;
        case A_STR:  result = v_str(n->soff); break;
        case A_BOOL: result = v_bool(n->ival); break;
        case A_NULL: result = v_null(); break;
        case A_UNDEF:result = v_undef(); break;
        case A_IDENT: {
            Value *slot = scope_find(e, n->soff);
            if (slot) { result = *slot; break; }
            Value g; if (global_value(S(n->soff), &g)) { result = g; break; }
            result = v_undef();
            break;
        }
        case A_FUNC: { Value v = v_obj(O_FUNC); v.fn = n; v.envi = env_index(e); result = v; break; }
        case A_ARRAYLIT: {
            Value arr = v_new_array();
            for (AstNode *el = n->list; el && !g_err; el = el->next)
                arr_push(arr.obji, eval(el, e));
            result = arr;
            break;
        }
        case A_OBJLIT: {
            Value o = v_new_plain();
            for (AstNode *p = n->list; p && !g_err; p = p->next)
                prop_set(o.obji, S(p->soff), eval(p->a, e));
            result = o;
            break;
        }
        case A_NEW: {
            Value v = eval(n->a, e);
            if (v.type == V_OBJ) result = v;
            else result = v_new_plain();
            break;
        }
        case A_BINARY: {
            Value l = eval(n->a, e);
            Value r = eval(n->b, e);
            result = binop(n->op, l, r);
            break;
        }
        case A_LOGICAL: {
            Value l = eval(n->a, e);
            if (n->op == OP_AND)          result = to_bool(l) ? eval(n->b, e) : l;
            else if (n->op == OP_NULLISH) result = (l.type == V_NULL || l.type == V_UNDEF) ? eval(n->b, e) : l;
            else                          result = to_bool(l) ? l : eval(n->b, e);
            break;
        }
        case A_UNARY: {
            if (n->op == OP_TYPEOF) {
                Value v = eval(n->a, e);
                const char *t = "undefined";
                switch (v.type) {
                    case V_NUM: t="number"; break; case V_BOOL: t="boolean"; break;
                    case V_STR: t="string"; break; case V_NULL: t="object"; break;
                    case V_OBJ: t=(v.objkind==O_FUNC||v.objkind==O_BUILTIN)?"function":"object"; break;
                    default: t="undefined";
                }
                result = v_str(str_intern(t,-1));
                break;
            }
            if (n->op == OP_DELETE) { eval(n->a, e); result = v_bool(1); break; }
            if (n->op == OP_VOID)   { eval(n->a, e); result = v_undef(); break; }
            Value v = eval(n->a, e);
            if (n->op == OP_NOT) result = v_bool(!to_bool(v));
            else if (n->op == OP_NEG) result = v_num(-to_num(v));
            else result = v_num(to_num(v));
            break;
        }
        case A_UPDATE: {
            Value old = read_target(n->a, e);
            int ov = to_num(old);
            int nv = (n->op == OP_PREINC || n->op == OP_POSTINC) ? ov + 1 : ov - 1;
            assign_to(n->a, v_num(nv), e);
            result = (n->op == OP_PREINC || n->op == OP_PREDEC) ? v_num(nv) : v_num(ov);
            break;
        }
        case A_ASSIGN: {
            Value rhs = eval(n->b, e);
            if (n->op != OP_ASSIGN) {
                Value cur_v = read_target(n->a, e);
                int bop = n->op==OP_ADDEQ?OP_ADD:n->op==OP_SUBEQ?OP_SUB:n->op==OP_MULEQ?OP_MUL:n->op==OP_DIVEQ?OP_DIV:OP_MOD;
                rhs = binop(bop, cur_v, rhs);
            }
            assign_to(n->a, rhs, e);
            result = rhs;
            break;
        }
        case A_COND: {
            result = to_bool(eval(n->a, e)) ? eval(n->b, e) : eval(n->c, e);
            break;
        }
        case A_MEMBER: {
            Value obj = eval(n->a, e);
            result = host_get(obj, S(n->soff));
            break;
        }
        case A_INDEX: {
            Value obj = eval(n->a, e);
            Value k = eval(n->b, e);
            result = host_get(obj, to_cstr(k));
            break;
        }
        case A_CALL: {
            AstNode *callee = n->a;
            Value argv[16]; int argc = 0;
            for (AstNode *arg = n->list; arg && argc < 16; arg = arg->next) argv[argc++] = eval(arg, e);
            if (g_err) break;
            if (callee && callee->kind == A_MEMBER) {
                Value obj = eval(callee->a, e);
                if (obj.type == V_OBJ && (obj.objkind == O_PLAIN || obj.objkind == O_ARRAY)) {
                    /* user function stored as a property? call it; else host */
                    Value *m = prop_find(obj.obji, S(callee->soff));
                    if (m && m->type == V_OBJ && (m->objkind == O_FUNC || m->objkind == O_BUILTIN)) {
                        result = call_value(*m, argv, argc);
                        break;
                    }
                }
                result = host_method(obj, S(callee->soff), argv, argc);
            } else if (callee && callee->kind == A_INDEX) {
                Value obj = eval(callee->a, e);
                Value k = eval(callee->b, e);
                result = host_method(obj, to_cstr(k), argv, argc);
            } else {
                Value fv = eval(callee, e);
                result = call_value(fv, argv, argc);
            }
            break;
        }
        default: result = v_undef(); break;
    }
    g_depth--;
    return result;
}

static void exec(AstNode *n, Env *e)
{
    if (!n || g_err || g_signal != SIG_NONE) return;
    if (budget()) return;
    if (++g_depth > DEPTH_MAX) { g_depth--; fail(); return; }
    switch (n->kind) {
        case A_BLOCK:
            for (AstNode *s = n->list; s && !g_err && g_signal == SIG_NONE; s = s->next) exec(s, e);
            break;
        case A_VARDECL: {
            Value v = n->a ? eval(n->a, e) : v_undef();
            scope_define(e, n->soff, v);
            break;
        }
        case A_EXPRSTMT:
            eval(n->a, e);
            break;
        case A_IF:
            if (to_bool(eval(n->a, e))) exec(n->b, e);
            else if (n->c) exec(n->c, e);
            break;
        case A_WHILE:
            while (!g_err && to_bool(eval(n->a, e))) {
                if (budget()) break;
                exec(n->b, e);
                if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                if (g_signal == SIG_CONTINUE) { g_signal = SIG_NONE; continue; }
                if (g_signal == SIG_RETURN) break;
            }
            break;
        case A_DOWHILE:
            do {
                if (budget()) break;
                exec(n->b, e);
                if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                if (g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
                if (g_signal == SIG_RETURN) break;
            } while (!g_err && to_bool(eval(n->a, e)));
            break;
        case A_FOR: {
            if (n->a) exec(n->a, e);
            while (!g_err && (!n->b || to_bool(eval(n->b, e)))) {
                if (budget()) break;
                exec(n->d, e);
                if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                if (g_signal == SIG_RETURN) break;
                if (g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
                if (n->c) eval(n->c, e);
            }
            break;
        }
        case A_FORIN: {
            Value coll = eval(n->a, e);
            if (coll.type == V_OBJ && (coll.objkind == O_ARRAY || coll.objkind == O_PLAIN)) {
                if (n->op == 1 && coll.objkind == O_ARRAY) {
                    int len = g_objs[coll.obji].alen;
                    for (int i = 0; i < len && i < 4096 && !g_err; i++) {
                        if (budget()) break;
                        scope_assign(e, n->soff, arr_get(coll.obji, i));
                        exec(n->b, e);
                        if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                        if (g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
                        if (g_signal == SIG_RETURN) break;
                    }
                } else {
                    for (int p = g_objs[coll.obji].phead; p >= 0 && !g_err; p = g_props[p].next) {
                        if (budget()) break;
                        scope_assign(e, n->soff, n->op == 1 ? g_props[p].val : v_str(g_props[p].key));
                        exec(n->b, e);
                        if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                        if (g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
                        if (g_signal == SIG_RETURN) break;
                    }
                }
            } else if (coll.type == V_STR && n->op == 1) {
                const char *s = S(coll.soff);
                for (int i = 0; s[i] && !g_err; i++) {
                    if (budget()) break;
                    char b[2] = { s[i], 0 };
                    scope_assign(e, n->soff, v_str(str_intern(b, 1)));
                    exec(n->b, e);
                    if (g_signal == SIG_BREAK) { g_signal = SIG_NONE; break; }
                    if (g_signal == SIG_CONTINUE) g_signal = SIG_NONE;
                    if (g_signal == SIG_RETURN) break;
                }
            }
            break;
        }
        case A_SWITCH: {
            Value disc = eval(n->a, e);
            AstNode *start = 0, *dflt = 0;
            for (AstNode *cs = n->list; cs; cs = cs->next) {
                if (!cs->a) { if (!dflt) dflt = cs; continue; }
                if (strict_eq(eval(cs->a, e), disc)) { start = cs; break; }
            }
            if (!start) start = dflt;
            for (AstNode *cs = start; cs && !g_err && g_signal == SIG_NONE; cs = cs->next)
                for (AstNode *s = cs->list; s && !g_err && g_signal == SIG_NONE; s = s->next)
                    exec(s, e);
            if (g_signal == SIG_BREAK) g_signal = SIG_NONE;
            break;
        }
        case A_TRY: {
            exec(n->a, e);
            if (g_err) {
                g_err = 0;
                if (g_signal != SIG_RETURN) g_signal = SIG_NONE;
                if (n->b) {
                    if (n->soff) scope_define(e, n->soff, v_str(str_intern("error", 5)));
                    exec(n->b, e);
                }
            }
            if (n->c) exec(n->c, e);
            break;
        }
        case A_THROW:
            eval(n->a, e);
            g_err = 1;
            break;
        case A_RETURN:
            g_retval = n->a ? eval(n->a, e) : v_undef();
            g_signal = SIG_RETURN;
            break;
        case A_BREAK:    g_signal = SIG_BREAK; break;
        case A_CONTINUE: g_signal = SIG_CONTINUE; break;
        case A_FUNC:
            if (n->soff) { Value v = v_obj(O_FUNC); v.fn = n; v.envi = env_index(e); scope_define(e ? e : 0, n->soff, v); }
            break;
        case A_EMPTY: break;
        default: eval(n, e); break;
    }
    g_depth--;
}

void js_engine_reset(void)
{
    g_strlen = 1; g_str[0] = 0;
    g_nenv = 0;
    g_nast = 0;                      /* AST persists per document, resets here */
    g_nobjs = 0;
    g_nprops = 0;
    g_nhandlers = 0;
    for (int i = 0; i < TMR_MAX; i++) g_timers[i].active = 0;
    g_alert_set = 0;
    g_nav_set = 0;
    g_globobj = obj_new();
}

/* fresh error/step/signal state for one entry into the engine (a script,
 * a handler, a timer callback) - g_dirty accumulates across an entry */
static void begin_invoke(void)
{
    g_err = 0; g_steps = 0; g_depth = 0; g_pdepth = 0;
    g_signal = SIG_NONE; g_dirty = 0;
}

/* parse + execute one source string against the current document state */
static void run_source(const char *src, int len)
{
    if (!src || len <= 0) return;
    tokenize(src, len);
    if (g_err) return;

    g_pos = 0;
    AstNode *prog = node(A_BLOCK);
    AstNode *tail = 0;
    while (cur()->type != T_EOF && !g_err) {
        AstNode *s = parse_stmt();
        if (prog && s) { if (!prog->list) prog->list = s; else tail->next = s; tail = s; }
        if (!s) break;
    }
    if (g_err || !prog) return;

    /* hoist top-level function declarations so calls can precede them */
    for (AstNode *s = prog->list; s; s = s->next)
        if (s->kind == A_FUNC && s->soff) { Value v = v_obj(O_FUNC); v.fn = s; v.envi = -1; prop_set(g_globobj, S(s->soff), v); }

    exec(prog, 0);
}

int js_run(const char *src, int len)
{
    if (g_globobj < 0) js_engine_reset();
    begin_invoke();
    run_source(src, len);
    return g_dirty;
}

/* run one handler value (function or attribute source string) */
static void run_handler_value(Value fn)
{
    if (fn.type == V_STR) { run_source(S(fn.soff), j_slen(S(fn.soff))); return; }
    Value ev = v_new_plain();
    call_value(fn, &ev, 1);
}

int js_fire_load(void)
{
    if (g_globobj < 0) return 0;
    begin_invoke();
    for (int i = 0; i < g_nhandlers && !g_err; i++)
        if (g_handlers[i].used && g_handlers[i].evt == EV_LOAD)
            run_handler_value(g_handlers[i].fn);

    DomNode *b = dom_body();
    const char *attr = b ? dom_get_attr(b, "onload") : "";
    if (attr[0]) run_source(attr, j_slen(attr));
    return g_dirty;
}

int js_click(DomNode *n)
{
    if (g_globobj < 0) return 0;
    begin_invoke();
    for (DomNode *p = n; p; p = p->parent) {
        for (int i = 0; i < g_nhandlers; i++)
            if (g_handlers[i].used && g_handlers[i].evt == EV_CLICK && g_handlers[i].node == p)
                run_handler_value(g_handlers[i].fn);
        const char *attr = dom_get_attr(p, "onclick");
        if (attr[0]) run_source(attr, j_slen(attr));
        if (g_err) { g_err = 0; }      /* one broken handler shouldn't block the rest */
    }
    /* document/window-level click handlers (node == 0) */
    for (int i = 0; i < g_nhandlers; i++)
        if (g_handlers[i].used && g_handlers[i].evt == EV_CLICK && g_handlers[i].node == 0)
            run_handler_value(g_handlers[i].fn);
    return g_dirty;
}

int js_has_click_handler(const DomNode *n)
{
    if (g_globobj < 0) return 0;
    for (const DomNode *p = n; p; p = p->parent) {
        for (int i = 0; i < g_nhandlers; i++)
            if (g_handlers[i].used && g_handlers[i].evt == EV_CLICK &&
                (const DomNode *)g_handlers[i].node == p) return 1;
        if (dom_get_attr(p, "onclick")[0]) return 1;
    }
    return 0;
}

int js_pump(void)
{
    if (g_globobj < 0) return 0;
    unsigned now = clock_ms();
    int total_dirty = 0, ran = 0;
    for (int i = 0; i < TMR_MAX && ran < 8; i++) {
        if (!g_timers[i].active) continue;
        if ((int)(now - g_timers[i].due) < 0) continue;
        Value fn = g_timers[i].fn;
        if (g_timers[i].interval > 0) g_timers[i].due = now + (unsigned)g_timers[i].interval;
        else g_timers[i].active = 0;
        begin_invoke();
        run_handler_value(fn);
        total_dirty |= g_dirty;
        ran++;
    }
    g_dirty = total_dirty;
    return total_dirty;
}
