/* kernel/js.c
 *
 * Bounded tree-walking JS-subset interpreter over the DOM. No libc, so the
 * usual string zoo (strlen/strcmp/tolower) gets reinvented below in miniature.
 *
 * Strings never move once made: everything the interpreter produces (literals,
 * concatenation results, attribute reads, whatever) gets appended into one big
 * arena and referred to afterwards by its byte offset rather than a pointer.
 * That's what lets Value stay a flat, copyable struct with an `int soff`
 * instead of carrying real char* and having to worry about who owns it or
 * when it gets freed - nothing is ever freed until the next script starts and
 * the whole arena resets. The tradeoff is JS_STR_CAP: run something that
 * concatenates megabytes of text and str_intern starts returning 0 (the ""
 * sentinel) instead of growing.
 */
#include "js.h"
#include "domrt.h"
#include "domparse.h"

#define JS_STR_CAP   (192 * 1024)
static char g_str[JS_STR_CAP];
static int  g_strlen;

static int j_slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int j_streq(const char *a, const char *b)
{ int i = 0; while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; } return a[i] == b[i]; }
static char j_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int str_intern(const char *s, int len)
{
    if (len < 0) len = j_slen(s);
    if (g_strlen + len + 1 > JS_STR_CAP) return 0;   /* arena full: fall back to "" */
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

static int parse_int_str(const char *s)
{
    int i = 0, sign = 1, v = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
    if (s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'X')) {
        i += 2;
        while (s[i]) {
            int d = -1;
            if (s[i] >= '0' && s[i] <= '9') d = s[i] - '0';
            else if (s[i] >= 'a' && s[i] <= 'f') d = 10 + s[i] - 'a';
            else if (s[i] >= 'A' && s[i] <= 'F') d = 10 + s[i] - 'A';
            else break;
            v = v * 16 + d; i++;
        }
        return sign * v;
    }
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    return sign * v;
}

/* g_err latches on the first problem (syntax we don't handle, a blown limit,
 * whatever) and every later stage checks it and bails immediately - cheaper
 * than threading a return code through the whole parser/evaluator, and it
 * means a broken script just quietly does nothing instead of taking the
 * kernel down with it. STEP_MAX exists so `while(true){}` in a page's script
 * can't wedge the browser; DEPTH_MAX does the same job for recursion (both
 * eval() and exec() count against it, since a pathological script can nest
 * either one). g_dirty just tracks whether js_run should tell the caller to
 * re-layout the page. */
static int g_err;
static long g_steps;
#define STEP_MAX  6000000L
#define DEPTH_MAX 80
static int g_depth;
static int g_dirty;

static void fail(void) { g_err = 1; }
static int  budget(void) { if (++g_steps > STEP_MAX) g_err = 1; return g_err; }

/* --- tokenizer ------------------------------------------------------------ */

enum { T_EOF, T_NUM, T_STR, T_IDENT, T_PUNCT };
typedef struct { int type; int num; int soff; char op[4]; } Tok;

#define MAX_TOKS 40000
static Tok g_toks[MAX_TOKS];
static int g_ntoks;

static int is_id_start(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'; }
static int is_id_part(char c)  { return is_id_start(c) || (c>='0'&&c<='9'); }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }

static const char *MULTI[] = {
    "===","!==",">>>","<<=",">>=","&&","||","==","!=","<=",">=",
    "+=","-=","*=","/=","%=","++","--","=>","**","&&","??", 0
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
            int j = i, val = 0, ishex = 0;
            if (c == '0' && i+1 < n && (s[i+1]=='x'||s[i+1]=='X')) {
                ishex = 1; j = i + 2;
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
            (void)ishex;
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
            if (i < n) i++; /* eat closing quote */
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

/* --- AST ------------------------------------------------------------------- */

enum {
    A_NUM, A_STR, A_BOOL, A_NULL, A_UNDEF, A_IDENT,
    A_BINARY, A_LOGICAL, A_UNARY, A_ASSIGN, A_UPDATE,
    A_MEMBER, A_INDEX, A_CALL, A_COND,
    A_VARDECL, A_EXPRSTMT, A_BLOCK, A_IF, A_WHILE, A_FOR,
    A_RETURN, A_BREAK, A_CONTINUE, A_FUNC, A_EMPTY
};

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT, OP_NEG, OP_POS, OP_TYPEOF,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_ASSIGN, OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ,
    OP_PREINC, OP_PREDEC, OP_POSTINC, OP_POSTDEC
};

/* One node shape for every kind of statement/expression - a,b,c,d mean
 * different things depending on n->kind (e.g. a/b/c are cond/then/else for
 * A_IF but init/cond/update... plus d for A_FOR). Wasteful of a few bytes per
 * node, but it means the parser doesn't need a different struct per
 * production, and nodes come out of a flat array (g_ast) instead of malloc so
 * there's nothing to free. list/next chain siblings for anything with a
 * variable number of children: block statements, call args, function params. */
typedef struct AstNode {
    int kind, op, ival, soff;
    struct AstNode *a, *b, *c, *d;
    struct AstNode *list;
    struct AstNode *next;
} AstNode;

#define MAX_AST 30000
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
static int is_punct(const char *p) { Tok *t = cur(); return t->type == T_PUNCT && j_streq(t->op, p); }
static int is_kw(const char *k) { Tok *t = cur(); return t->type == T_IDENT && j_streq(S(t->soff), k); }
static void advance(void) { if (g_pos < g_ntoks) g_pos++; }
static int eat_punct(const char *p) { if (is_punct(p)) { advance(); return 1; } return 0; }

static AstNode *parse_expr(void);
static AstNode *parse_assign(void);
static AstNode *parse_stmt(void);

static AstNode *parse_primary(void)
{
    if (g_err) return 0;
    Tok *t = cur();
    if (t->type == T_NUM) { advance(); AstNode *n = node(A_NUM); if (n) n->ival = t->num; return n; }
    if (t->type == T_STR) { advance(); AstNode *n = node(A_STR); if (n) n->soff = t->soff; return n; }
    if (t->type == T_IDENT) {
        const char *s = S(t->soff);
        if (j_streq(s, "true"))  { advance(); AstNode *n = node(A_BOOL); if (n) n->ival = 1; return n; }
        if (j_streq(s, "false")) { advance(); AstNode *n = node(A_BOOL); if (n) n->ival = 0; return n; }
        if (j_streq(s, "null"))  { advance(); return node(A_NULL); }
        if (j_streq(s, "undefined")) { advance(); return node(A_UNDEF); }
        if (j_streq(s, "function")) {
            advance();
            AstNode *fn = node(A_FUNC);
            if (cur()->type == T_IDENT) { if (fn) fn->soff = cur()->soff; advance(); }
            if (!eat_punct("(")) { fail(); return fn; }
            AstNode *ptail = 0;
            while (!is_punct(")") && cur()->type != T_EOF && !g_err) {
                if (cur()->type == T_IDENT) {
                    AstNode *p = node(A_IDENT); if (p) p->soff = cur()->soff; advance();
                    list_append(fn, &ptail, p);
                } else advance();
                if (!eat_punct(",")) break;
            }
            eat_punct(")");
            fn->a = parse_stmt();    /* body block */
            return fn;
        }
        advance();
        AstNode *n = node(A_IDENT); if (n) n->soff = t->soff; return n;
    }
    if (is_punct("(")) {
        advance();
        AstNode *e = parse_expr();
        eat_punct(")");
        return e;
    }
    /* whatever's left is stuff we don't parse - array/object literals, regex,
     * template interpolation. Bail rather than pretend to understand it. */
    fail();
    return 0;
}

static AstNode *parse_args_call(AstNode *callee)
{
    AstNode *call = node(A_CALL);
    if (call) call->a = callee;
    AstNode *tail = 0;
    advance(); /* past ( */
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
        if (is_punct(".")) {
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
    if (is_punct("++") || is_punct("--")) {
        int dec = is_punct("--"); advance();
        AstNode *u = node(A_UPDATE); if (u) { u->op = dec ? OP_PREDEC : OP_PREINC; u->a = parse_unary(); } return u;
    }
    return parse_postfix();
}

/* table of binary operators the tokenizer might hand us next, with their
 * precedence (higher binds tighter); &&/|| are flagged as logical since they
 * need to short-circuit instead of eagerly evaluating both sides */
static int bin_op(int *prec, int *logical)
{
    *logical = 0;
    Tok *t = cur();
    if (t->type != T_PUNCT) return -1;
    const char *o = t->op;
    if (j_streq(o,"*")) { *prec=12; return OP_MUL; }
    if (j_streq(o,"/")) { *prec=12; return OP_DIV; }
    if (j_streq(o,"%")) { *prec=12; return OP_MOD; }
    if (j_streq(o,"+")) { *prec=11; return OP_ADD; }
    if (j_streq(o,"-")) { *prec=11; return OP_SUB; }
    if (j_streq(o,"<<")) { *prec=10; return OP_SHL; }
    if (j_streq(o,">>")) { *prec=10; return OP_SHR; }
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
            return n;
        }
    }
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
    advance(); /* past { */
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
    if (is_punct("{")) return parse_block();
    if (is_punct(";")) { advance(); return node(A_EMPTY); }
    if (is_kw("var") || is_kw("let") || is_kw("const")) return parse_var();
    if (is_kw("function")) return parse_primary();  /* decl and expr share the same parse */
    if (is_kw("if")) {
        advance();
        AstNode *n = node(A_IF);
        eat_punct("(");
        if (n) n->a = parse_expr();
        eat_punct(")");
        if (n) n->b = parse_stmt();
        if (is_kw("else")) { advance(); if (n) n->c = parse_stmt(); }
        return n;
    }
    if (is_kw("while")) {
        advance();
        AstNode *n = node(A_WHILE);
        eat_punct("(");
        if (n) n->a = parse_expr();
        eat_punct(")");
        if (n) n->b = parse_stmt();
        return n;
    }
    if (is_kw("for")) {
        advance();
        AstNode *n = node(A_FOR);
        eat_punct("(");
        /* only the classic three-clause for(;;) is supported - no for-in or
         * for-of. Not worth the object-iteration machinery for a subset that
         * has no real objects/arrays to iterate over anyway. */
        if (is_punct(";")) { advance(); }
        else if (is_kw("var") || is_kw("let") || is_kw("const")) { if (n) n->a = parse_var(); }
        else { AstNode *e = parse_expr(); AstNode *es = node(A_EXPRSTMT); if (es) es->a = e; if (n) n->a = es; eat_punct(";"); }
        if (!is_punct(";")) { if (n) n->b = parse_expr(); }
        eat_punct(";");
        if (!is_punct(")")) { if (n) n->c = parse_expr(); }
        eat_punct(")");
        if (n) n->d = parse_stmt();
        return n;
    }
    if (is_kw("return")) {
        advance();
        AstNode *n = node(A_RETURN);
        if (!is_punct(";") && !is_punct("}") && cur()->type != T_EOF) { if (n) n->a = parse_expr(); }
        eat_punct(";");
        return n;
    }
    if (is_kw("break")) { advance(); eat_punct(";"); return node(A_BREAK); }
    if (is_kw("continue")) { advance(); eat_punct(";"); return node(A_CONTINUE); }
    /* fell through everything above - must be a bare expression statement */
    {
        AstNode *e = parse_expr();
        AstNode *s = node(A_EXPRSTMT);
        if (s) s->a = e;
        eat_punct(";");
        return s;
    }
}

/* --- values -----------------------------------------------------------------
 * Value is a tagged union in struct clothing (no real unions here, just every
 * field laid out side by side) - `type` says which of num/soff/objkind/node/fn
 * actually means anything. O_* distinguishes the flavors of V_OBJ: DOM
 * elements, the style/console/window/Math singletons, user functions, and
 * builtins. B_* is which builtin, for the handful that don't need their own
 * O_ kind. */

enum { V_UNDEF, V_NULL, V_NUM, V_BOOL, V_STR, V_OBJ };
enum { O_DOCUMENT, O_ELEMENT, O_STYLE, O_CONSOLE, O_WINDOW, O_MATH,
       O_FUNC, O_BUILTIN, O_LOCATION };
enum { B_PARSEINT, B_PARSEFLOAT, B_STRING, B_NUMBER, B_BOOLEAN, B_ISNAN, B_NOOP };

typedef struct {
    int      type;
    int      num;        /* V_NUM / V_BOOL */
    int      soff;       /* V_STR */
    int      objkind;    /* V_OBJ */
    int      bid;        /* builtin id */
    DomNode *node;       /* element / style target */
    AstNode *fn;         /* user function */
} Value;

static Value v_undef(void){ Value v; v.type=V_UNDEF; v.num=0; v.soff=0; v.objkind=0; v.bid=0; v.node=0; v.fn=0; return v; }
static Value v_null(void){ Value v=v_undef(); v.type=V_NULL; return v; }
static Value v_num(int n){ Value v=v_undef(); v.type=V_NUM; v.num=n; return v; }
static Value v_bool(int b){ Value v=v_undef(); v.type=V_BOOL; v.num=b?1:0; return v; }
static Value v_str(int off){ Value v=v_undef(); v.type=V_STR; v.soff=off; return v; }
static Value v_obj(int kind){ Value v=v_undef(); v.type=V_OBJ; v.objkind=kind; return v; }
static Value v_elem(DomNode *n){ Value v=v_obj(O_ELEMENT); v.node=n; return v; }

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
    /* skip script/style content */
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
            return str_intern("[object Object]", -1);
        default: return 0;
    }
}
static const char *to_cstr(Value v) { return S(to_str_off(v)); }

/* ========================================================================== */
/* environments                                                               */
/* ========================================================================== */

#define ENV_POOL 64
#define ENV_VARS 64
typedef struct Env {
    struct Env *parent;
    int   names[ENV_VARS];
    Value vals[ENV_VARS];
    int   count;
} Env;

static Env  g_envpool[ENV_POOL];
static int  g_nenv;
static Env *g_global;

static Env *env_new(Env *parent)
{
    if (g_nenv >= ENV_POOL) { fail(); return g_global; }
    Env *e = &g_envpool[g_nenv++];
    e->parent = parent; e->count = 0;
    return e;
}

static int name_eq(int a_off, int b_off)
{ return j_streq(S(a_off), S(b_off)); }

static Value *env_find(Env *e, int name_off)
{
    for (Env *p = e; p; p = p->parent)
        for (int i = 0; i < p->count; i++)
            if (name_eq(p->names[i], name_off)) return &p->vals[i];
    return 0;
}
static void env_define(Env *e, int name_off, Value v)
{
    for (int i = 0; i < e->count; i++)
        if (name_eq(e->names[i], name_off)) { e->vals[i] = v; return; }
    if (e->count >= ENV_VARS) { fail(); return; }
    e->names[e->count] = name_off; e->vals[e->count] = v; e->count++;
}
static void env_assign(Env *e, int name_off, Value v)
{
    Value *slot = env_find(e, name_off);
    if (slot) { *slot = v; return; }
    env_define(g_global, name_off, v);   /* implicit global */
}

/* ========================================================================== */
/* control-flow signals                                                       */
/* ========================================================================== */

static int   g_signal;          /* 0 none, 1 return, 2 break, 3 continue */
static Value g_retval;
enum { SIG_NONE = 0, SIG_RETURN = 1, SIG_BREAK = 2, SIG_CONTINUE = 3 };

/* ========================================================================== */
/* evaluator                                                                  */
/* ========================================================================== */

static Value eval(AstNode *n, Env *e);
static void  exec(AstNode *n, Env *e);

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

/* obj.prop read */
static Value host_get(Value obj, const char *name)
{
    if (obj.type == V_STR) {
        if (j_streq(name, "length")) return v_num(j_slen(S(obj.soff)));
        return v_undef();
    }
    if (obj.type != V_OBJ) return v_undef();

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
        if (j_streq(name,"tagName") || j_streq(name,"nodeName")) {
            char up[16]; int i = 0; const char *t = n->tag;
            for (; t[i] && i < 15; i++) up[i] = (t[i] >= 'a' && t[i] <= 'z') ? (char)(t[i]-32) : t[i];
            up[i] = 0; return v_str(str_intern(up, i));
        }
        if (j_streq(name,"nodeType"))  return v_num(n->type == DOM_NODE_ELEMENT ? 1 : 3);
        if (j_streq(name,"style"))     { Value v = v_obj(O_STYLE); v.node = n; return v; }
        if (j_streq(name,"parentNode") || j_streq(name,"parentElement"))
            return n->parent ? v_elem(n->parent) : v_null();
        if (j_streq(name,"firstChild") || j_streq(name,"firstElementChild"))
            return n->first_child ? v_elem(n->first_child) : v_null();
        if (j_streq(name,"nextSibling") || j_streq(name,"nextElementSibling"))
            return n->next_sibling ? v_elem(n->next_sibling) : v_null();
        return v_undef();
    }
    if (obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"body")) return v_elem(dom_body());
        if (j_streq(name,"documentElement")) return v_elem(dom_root());
        if (j_streq(name,"title")) {
            for (int x = 0; x < 1; x++) { (void)x; }
            DomNode *r = dom_root();
            /* find <title> */
            /* simple DFS */
            DomNode *st[64]; int sp = 0; st[sp++] = r;
            while (sp > 0) {
                DomNode *cn = st[--sp];
                if (cn->type == DOM_NODE_ELEMENT && j_streq(cn->tag,"title"))
                    return v_str(elem_text(cn));
                for (DomNode *c = cn->first_child; c && sp < 64; c = c->next_sibling) st[sp++] = c;
            }
            return v_str(0);
        }
        if (j_streq(name,"cookie")) return v_str(0);
        return v_undef();
    }
    if (obj.objkind == O_WINDOW) {
        if (j_streq(name,"document")) return v_obj(O_DOCUMENT);
        if (j_streq(name,"console"))  return v_obj(O_CONSOLE);
        if (j_streq(name,"Math"))     return v_obj(O_MATH);
        return v_undef();
    }
    if (obj.objkind == O_MATH) {
        if (j_streq(name,"PI")) return v_num(3);
        if (j_streq(name,"E"))  return v_num(2);
        return v_undef();
    }
    if (obj.objkind == O_STYLE) return v_str(0);
    return v_undef();
}

/* obj.prop = value */
static void host_set(Value obj, const char *name, Value val)
{
    if (obj.type != V_OBJ) return;
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
        if (j_streq(name,"value"))     { dom_set_attr(n,"value", to_cstr(val)); return; }
        if (j_streq(name,"href"))      { dom_set_attr(n,"href", to_cstr(val)); return; }
        if (j_streq(name,"src"))       { dom_set_attr(n,"src", to_cstr(val)); g_dirty = 1; return; }
        return;
    }
    if (obj.objkind == O_STYLE) {
        style_set(obj.node, name, to_cstr(val));
        return;
    }
    if (obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"title")) {
            DomNode *r = dom_root();
            DomNode *st[64]; int sp = 0; st[sp++] = r;
            while (sp > 0) {
                DomNode *cn = st[--sp];
                if (cn->type == DOM_NODE_ELEMENT && j_streq(cn->tag,"title")) {
                    dom_remove_children(cn);
                    const char *t = to_cstr(val);
                    DomNode *tn = dom_create_text(t, j_slen(t));
                    if (tn) dom_append_child(cn, tn);
                    return;
                }
                for (DomNode *c = cn->first_child; c && sp < 64; c = c->next_sibling) st[sp++] = c;
            }
        }
        return;
    }
}

static int isqrt_i(int v) { if (v <= 0) return 0; int r = 0; while ((r+1)*(r+1) <= v) r++; return r; }
static int ipow_i(int b, int e) { int r = 1; while (e-- > 0) r *= b; return r; }

/* obj.method(args) */
static Value host_method(Value obj, const char *name, Value *argv, int argc)
{
    /* string methods */
    if (obj.type == V_STR) {
        const char *s = S(obj.soff); int sl = j_slen(s);
        if (j_streq(name,"toUpperCase")) { char b[1024]; int i=0; for(;s[i]&&i<1023;i++) b[i]=(s[i]>='a'&&s[i]<='z')?(char)(s[i]-32):s[i]; b[i]=0; return v_str(str_intern(b,i)); }
        if (j_streq(name,"toLowerCase")) { char b[1024]; int i=0; for(;s[i]&&i<1023;i++) b[i]=j_lc(s[i]); b[i]=0; return v_str(str_intern(b,i)); }
        if (j_streq(name,"charAt"))      { int idx = argc>0?to_num(argv[0]):0; if (idx<0||idx>=sl) return v_str(0); char b[2]={s[idx],0}; return v_str(str_intern(b,1)); }
        if (j_streq(name,"charCodeAt"))  { int idx = argc>0?to_num(argv[0]):0; if (idx<0||idx>=sl) return v_num(0); return v_num((unsigned char)s[idx]); }
        if (j_streq(name,"indexOf"))     { const char *q = argc>0?to_cstr(argv[0]):""; int ql=j_slen(q); for(int i=0;i+ql<=sl;i++){int m=1;for(int z=0;z<ql;z++)if(s[i+z]!=q[z]){m=0;break;}if(m)return v_num(i);} return v_num(-1); }
        if (j_streq(name,"substring") || j_streq(name,"substr") || j_streq(name,"slice")) {
            int a = argc>0?to_num(argv[0]):0; int b = argc>1?to_num(argv[1]):sl;
            if (j_streq(name,"substr")) b = a + (argc>1?to_num(argv[1]):sl);
            if (a < 0) a = 0;
            if (b > sl) b = sl;
            if (b < a) b = a;
            char buf[1024]; int o=0; for(int i=a;i<b&&o<1023;i++) buf[o++]=s[i]; buf[o]=0; return v_str(str_intern(buf,o));
        }
        if (j_streq(name,"trim")) { int a=0,b=sl; while(a<b&&(s[a]==' '||s[a]=='\t'||s[a]=='\n'))a++; while(b>a&&(s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\n'))b--; char buf[1024]; int o=0; for(int i=a;i<b&&o<1023;i++)buf[o++]=s[i]; buf[o]=0; return v_str(str_intern(buf,o)); }
        if (j_streq(name,"toString")) return obj;
        return v_undef();
    }
    if (obj.type == V_NUM || obj.type == V_BOOL) {
        if (j_streq(name,"toString")) return v_str(str_from_int(obj.num));
        return v_undef();
    }
    if (obj.type != V_OBJ) return v_undef();

    if (obj.objkind == O_DOCUMENT) {
        if (j_streq(name,"getElementById")) {
            DomNode *n = dom_get_element_by_id(argc>0?to_cstr(argv[0]):"");
            return n ? v_elem(n) : v_null();
        }
        if (j_streq(name,"querySelector")) {
            const char *q = argc>0?to_cstr(argv[0]):"";
            if (q[0] == '#') { DomNode *n = dom_get_element_by_id(q+1); return n?v_elem(n):v_null(); }
            /* tag or .class: first match via DFS */
            DomNode *st[128]; int sp=0; st[sp++]=dom_root();
            while (sp>0) { DomNode *cn=st[--sp];
                if (cn->type==DOM_NODE_ELEMENT) {
                    if (q[0]=='.') { const char *cl=dom_get_attr(cn,"class"); int ql=j_slen(q+1),p=0; while(cl[p]){while(cl[p]==' ')p++;int e2=p;while(cl[e2]&&cl[e2]!=' ')e2++;if(e2-p==ql){int m=1;for(int z=0;z<ql;z++)if(cl[p+z]!=q[1+z]){m=0;break;}if(m)return v_elem(cn);}p=e2;} }
                    else if (j_streq(cn->tag,q)) return v_elem(cn);
                }
                for (DomNode *c=cn->first_child;c&&sp<128;c=c->next_sibling) st[sp++]=c;
            }
            return v_null();
        }
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
        if (j_streq(name,"getElementsByTagName") || j_streq(name,"getElementsByClassName")
            || j_streq(name,"addEventListener")) return v_undef();
        return v_undef();
    }
    if (obj.objkind == O_ELEMENT) {
        DomNode *n = obj.node;
        if (j_streq(name,"setAttribute")) { if (argc>=2) dom_set_attr(n, to_cstr(argv[0]), to_cstr(argv[1])); g_dirty=1; return v_undef(); }
        if (j_streq(name,"getAttribute")) { return v_str(str_intern(dom_get_attr(n, argc>0?to_cstr(argv[0]):""), -1)); }
        if (j_streq(name,"hasAttribute")) { return v_bool(dom_has_attr(n, argc>0?to_cstr(argv[0]):"")); }
        if (j_streq(name,"removeAttribute")) { if (argc>0) dom_set_attr(n, to_cstr(argv[0]), ""); g_dirty=1; return v_undef(); }
        if (j_streq(name,"appendChild")) { if (argc>0 && argv[0].type==V_OBJ && argv[0].objkind==O_ELEMENT) { dom_append_child(n, argv[0].node); g_dirty=1; return argv[0]; } return v_undef(); }
        if (j_streq(name,"querySelector")) { Value d = v_obj(O_DOCUMENT); return host_method(d, "querySelector", argv, argc); }
        if (j_streq(name,"addEventListener") || j_streq(name,"removeEventListener") ||
            j_streq(name,"focus") || j_streq(name,"blur") || j_streq(name,"click") ||
            j_streq(name,"remove") || j_streq(name,"scrollIntoView")) return v_undef();
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
    if (obj.objkind == O_WINDOW) {
        if (j_streq(name,"alert")||j_streq(name,"setTimeout")||j_streq(name,"setInterval")||
            j_streq(name,"addEventListener")||j_streq(name,"scrollTo")) return v_undef();
        if (j_streq(name,"parseInt")) return v_num(argc>0?to_num(argv[0]):0);
        return v_undef();
    }
    return v_undef();
}

static Value call_builtin(int bid, Value *argv, int argc)
{
    switch (bid) {
        case B_PARSEINT: case B_PARSEFLOAT: return v_num(argc>0?to_num(argv[0]):0);
        case B_STRING:   return v_str(argc>0?to_str_off(argv[0]):0);
        case B_NUMBER:   return v_num(argc>0?to_num(argv[0]):0);
        case B_BOOLEAN:  return v_bool(argc>0?to_bool(argv[0]):0);
        case B_ISNAN:    return v_bool(0);
        case B_NOOP:     return v_undef();
    }
    return v_undef();
}

static Value call_user(AstNode *fn, Value *argv, int argc)
{
    if (!fn || g_depth >= DEPTH_MAX) { fail(); return v_undef(); }
    g_depth++;
    Env *e = env_new(g_global);
    int i = 0;
    for (AstNode *p = fn->list; p; p = p->next, i++)
        env_define(e, p->soff, i < argc ? argv[i] : v_undef());
    g_signal = SIG_NONE;
    exec(fn->a, e);
    Value rv = (g_signal == SIG_RETURN) ? g_retval : v_undef();
    g_signal = SIG_NONE;
    g_depth--;
    return rv;
}

/* resolve a bare identifier to host globals / builtins when not a variable */
static int global_value(const char *nm, Value *out)
{
    if (j_streq(nm,"document")) { *out = v_obj(O_DOCUMENT); return 1; }
    if (j_streq(nm,"console"))  { *out = v_obj(O_CONSOLE); return 1; }
    if (j_streq(nm,"window") || j_streq(nm,"self") || j_streq(nm,"globalThis") || j_streq(nm,"top")) { *out = v_obj(O_WINDOW); return 1; }
    if (j_streq(nm,"Math"))     { *out = v_obj(O_MATH); return 1; }
    if (j_streq(nm,"undefined")){ *out = v_undef(); return 1; }
    if (j_streq(nm,"NaN"))      { *out = v_num(0); return 1; }
    if (j_streq(nm,"parseInt"))  { Value v=v_obj(O_BUILTIN); v.bid=B_PARSEINT; *out=v; return 1; }
    if (j_streq(nm,"parseFloat")){ Value v=v_obj(O_BUILTIN); v.bid=B_PARSEFLOAT; *out=v; return 1; }
    if (j_streq(nm,"String"))    { Value v=v_obj(O_BUILTIN); v.bid=B_STRING; *out=v; return 1; }
    if (j_streq(nm,"Number"))    { Value v=v_obj(O_BUILTIN); v.bid=B_NUMBER; *out=v; return 1; }
    if (j_streq(nm,"Boolean"))   { Value v=v_obj(O_BUILTIN); v.bid=B_BOOLEAN; *out=v; return 1; }
    if (j_streq(nm,"isNaN"))     { Value v=v_obj(O_BUILTIN); v.bid=B_ISNAN; *out=v; return 1; }
    if (j_streq(nm,"alert") || j_streq(nm,"setTimeout") || j_streq(nm,"setInterval")) { Value v=v_obj(O_BUILTIN); v.bid=B_NOOP; *out=v; return 1; }
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
    if (l.type==V_OBJ || r.type==V_OBJ) return l.node==r.node && l.objkind==r.objkind;
    return to_num(l) == to_num(r);
}
static int strict_eq(Value l, Value r)
{
    if (l.type != r.type) return 0;
    switch (l.type) {
        case V_STR: return streq2(S(l.soff), S(r.soff));
        case V_NUM: case V_BOOL: return l.num == r.num;
        case V_NULL: case V_UNDEF: return 1;
        case V_OBJ: return l.node==r.node && l.objkind==r.objkind;
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

/* assignment target helpers */
static void assign_to(AstNode *target, Value val, Env *e)
{
    if (!target) return;
    if (target->kind == A_IDENT) { env_assign(e, target->soff, val); return; }
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
        Value *slot = env_find(e, target->soff);
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
            Value *slot = env_find(e, n->soff);
            if (slot) { result = *slot; break; }
            Value g; if (global_value(S(n->soff), &g)) { result = g; break; }
            result = v_undef();
            break;
        }
        case A_FUNC: { Value v = v_obj(O_FUNC); v.fn = n; result = v; break; }
        case A_BINARY: {
            Value l = eval(n->a, e);
            Value r = eval(n->b, e);
            result = binop(n->op, l, r);
            break;
        }
        case A_LOGICAL: {
            Value l = eval(n->a, e);
            if (n->op == OP_AND) result = to_bool(l) ? eval(n->b, e) : l;
            else                 result = to_bool(l) ? l : eval(n->b, e);
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
                result = host_method(obj, S(callee->soff), argv, argc);
            } else if (callee && callee->kind == A_INDEX) {
                Value obj = eval(callee->a, e);
                Value k = eval(callee->b, e);
                result = host_method(obj, to_cstr(k), argv, argc);
            } else {
                Value fv = eval(callee, e);
                if (fv.type == V_OBJ && fv.objkind == O_FUNC) result = call_user(fv.fn, argv, argc);
                else if (fv.type == V_OBJ && fv.objkind == O_BUILTIN) result = call_builtin(fv.bid, argv, argc);
                else result = v_undef();
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
    switch (n->kind) {
        case A_BLOCK:
            for (AstNode *s = n->list; s && !g_err && g_signal == SIG_NONE; s = s->next) exec(s, e);
            break;
        case A_VARDECL: {
            Value v = n->a ? eval(n->a, e) : v_undef();
            env_define(e, n->soff, v);
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
        case A_RETURN:
            g_retval = n->a ? eval(n->a, e) : v_undef();
            g_signal = SIG_RETURN;
            break;
        case A_BREAK:    g_signal = SIG_BREAK; break;
        case A_CONTINUE: g_signal = SIG_CONTINUE; break;
        case A_FUNC:
            if (n->soff) { Value v = v_obj(O_FUNC); v.fn = n; env_define(g_global, n->soff, v); }
            break;
        case A_EMPTY: break;
        default: eval(n, e); break;
    }
}

/* ========================================================================== */
/* public entry points                                                        */
/* ========================================================================== */

void js_engine_reset(void)
{
    g_strlen = 1; g_str[0] = 0;     /* offset 0 == "" */
    g_nenv = 0;
    g_global = env_new(0);
}

int js_run(const char *src, int len)
{
    if (!src || len <= 0) return 0;
    if (!g_global) js_engine_reset();

    g_err = 0; g_steps = 0; g_depth = 0; g_dirty = 0;
    g_signal = SIG_NONE;
    g_nast = 0;

    tokenize(src, len);
    if (g_err) return g_dirty;

    g_pos = 0;
    /* parse the whole program into a top-level block */
    AstNode *prog = node(A_BLOCK);
    AstNode *tail = 0;
    while (cur()->type != T_EOF && !g_err) {
        AstNode *s = parse_stmt();
        if (prog && s) { if (!prog->list) prog->list = s; else tail->next = s; tail = s; }
        if (!s) break;
    }
    if (g_err) return g_dirty;

    /* hoist top-level function declarations so calls can precede them */
    for (AstNode *s = prog->list; s; s = s->next)
        if (s->kind == A_FUNC && s->soff) { Value v = v_obj(O_FUNC); v.fn = s; env_define(g_global, s->soff, v); }

    exec(prog, g_global);
    return g_dirty;
}
