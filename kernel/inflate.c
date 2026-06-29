#include "inflate.h"

/* A from-scratch DEFLATE decoder (RFC1951) with canonical Huffman decoding. */

#define MAXBITS  15
#define MAXLCODES 288
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define MAXCLCODES 19

typedef struct {
    const uint8_t *in;
    int inlen, inpos;
    int bitbuf, bitcnt;
    uint8_t *out;
    int outcap, outpos;
} State;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[MAXCODES];
} Huff;

static int getbit(State *s)
{
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static int getbits(State *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = getbit(s);
        if (b < 0) return -1;
        v |= b << i;
    }
    return v;
}

static void build(Huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;

    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];

    for (int sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (short)sym;
}

/* Decode one symbol using canonical Huffman code (codes packed MSB-first). */
static int decode_sym(State *s, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = getbit(s);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_block_data(State *s, const Huff *lh, const Huff *dh)
{
    for (;;) {
        int sym = decode_sym(s, lh);
        if (sym < 0) return -1;
        if (sym == 256) return 0;            /* end of block */
        if (sym < 256) {
            if (s->outpos >= s->outcap) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = LEN_BASE[sym];
            int e = LEN_EXTRA[sym];
            if (e) { int x = getbits(s, e); if (x < 0) return -1; len += x; }

            int dsym = decode_sym(s, dh);
            if (dsym < 0 || dsym >= 30) return -1;
            int dist = DIST_BASE[dsym];
            e = DIST_EXTRA[dsym];
            if (e) { int x = getbits(s, e); if (x < 0) return -1; dist += x; }

            if (dist > s->outpos) return -1;
            if (s->outpos + len > s->outcap) return -1;
            int from = s->outpos - dist;
            for (int i = 0; i < len; i++) s->out[s->outpos + i] = s->out[from + i];
            s->outpos += len;
        }
    }
}

static int fixed_block(State *s)
{
    static Huff lh, dh;
    static int built = 0;
    if (!built) {
        uint8_t ll[288];
        for (int i = 0;   i < 144; i++) ll[i] = 8;
        for (int i = 144; i < 256; i++) ll[i] = 9;
        for (int i = 256; i < 280; i++) ll[i] = 7;
        for (int i = 280; i < 288; i++) ll[i] = 8;
        build(&lh, ll, 288);
        uint8_t dl[30];
        for (int i = 0; i < 30; i++) dl[i] = 5;
        build(&dh, dl, 30);
        built = 1;
    }
    return inflate_block_data(s, &lh, &dh);
}

static int dynamic_block(State *s)
{
    int hlit = getbits(s, 5); if (hlit < 0) return -1; hlit += 257;
    int hdist = getbits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int hclen = getbits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    static const uint8_t ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t cl_lengths[19];
    for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (int i = 0; i < hclen; i++) { int v = getbits(s, 3); if (v < 0) return -1; cl_lengths[ORDER[i]] = (uint8_t)v; }

    Huff clh;
    build(&clh, cl_lengths, 19);

    uint8_t lengths[MAXLCODES + MAXDCODES];
    int n = 0;
    while (n < hlit + hdist) {
        int sym = decode_sym(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int rep = getbits(s, 2); if (rep < 0) return -1; rep += 3;
            uint8_t prev = lengths[n - 1];
            while (rep-- && n < hlit + hdist) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep = getbits(s, 3); if (rep < 0) return -1; rep += 3;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        } else { /* sym == 18 */
            int rep = getbits(s, 7); if (rep < 0) return -1; rep += 11;
            while (rep-- && n < hlit + hdist) lengths[n++] = 0;
        }
    }
    if (n != hlit + hdist) return -1;

    Huff lh, dh;
    build(&lh, lengths, hlit);
    build(&dh, lengths + hlit, hdist);
    return inflate_block_data(s, &lh, &dh);
}

static int stored_block(State *s)
{
    s->bitcnt = 0;                            /* align to byte boundary */
    if (s->inpos + 4 > s->inlen) return -1;
    int len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    s->inpos += 4;                            /* skip LEN + NLEN */
    if (s->inpos + len > s->inlen) return -1;
    if (s->outpos + len > s->outcap) return -1;
    for (int i = 0; i < len; i++) s->out[s->outpos++] = s->in[s->inpos++];
    return 0;
}

int inflate_raw(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    State s;
    s.in = src; s.inlen = srclen; s.inpos = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = dst; s.outcap = dstcap; s.outpos = 0;

    int last;
    do {
        last = getbit(&s);
        if (last < 0) return -1;
        int type = getbits(&s, 2);
        if (type < 0) return -1;
        int r;
        if (type == 0) r = stored_block(&s);
        else if (type == 1) r = fixed_block(&s);
        else if (type == 2) r = dynamic_block(&s);
        else return -1;
        if (r < 0) return -1;
    } while (!last);

    return s.outpos;
}

int zlib_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen < 2) return -1;
    if ((src[0] & 0x0F) != 8) return -1;      /* CM must be deflate */
    if (((src[0] << 8) | src[1]) % 31 != 0) return -1;
    int hlen = 2;
    if (src[1] & 0x20) hlen += 4;             /* FDICT present */
    return inflate_raw(src + hlen, srclen - hlen, dst, dstcap);
}

int gzip_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen < 18) return -1;
    if (src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) return -1;
    int flg = src[3];
    int p = 10;
    if (flg & 0x04) {                          /* FEXTRA */
        if (p + 2 > srclen) return -1;
        int xlen = src[p] | (src[p + 1] << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < srclen && src[p]) p++; p++; }   /* FNAME */
    if (flg & 0x10) { while (p < srclen && src[p]) p++; p++; }   /* FCOMMENT */
    if (flg & 0x02) p += 2;                    /* FHCRC */
    if (p >= srclen) return -1;
    return inflate_raw(src + p, srclen - p, dst, dstcap);
}

int auto_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen >= 2 && src[0] == 0x1F && src[1] == 0x8B) return gzip_inflate(src, srclen, dst, dstcap);
    if (srclen >= 2 && (src[0] & 0x0F) == 8 && (((src[0] << 8) | src[1]) % 31 == 0)) return zlib_inflate(src, srclen, dst, dstcap);
    return inflate_raw(src, srclen, dst, dstcap);
}
