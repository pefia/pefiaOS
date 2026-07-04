#include "inflate.h"

/* Plain DEFLATE decoder, RFC 1951. Nothing fancy: bit-at-a-time reader,
 * canonical Huffman via the count/symbol table trick from the RFC's own
 * appendix, and the usual three block types. Window is just "the output
 * buffer so far" since we always decode into one flat destination rather
 * than a sliding buffer - fine for the sizes this kernel deals with. */

#define MAXBITS    15
#define MAXLCODES  288
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define MAXCLCODES 19

/* Bit-reader + output cursor, threaded through every helper below instead
 * of using globals (this thing might get called for several images while
 * decoding a page). */
typedef struct {
    const uint8_t *in;
    int inlen, inpos;
    int bitbuf, bitcnt;
    uint8_t *out;
    int outcap, outpos;
} Reader;

/* Canonical Huffman table: count[len] = how many codes of that bit length,
 * symbol[] = the symbols in canonical order. See build() below. */
typedef struct {
    short count[MAXBITS + 1];
    short symbol[MAXCODES];
} Huff;

static int next_bit(Reader *r)
{
    if (r->bitcnt == 0) {
        if (r->inpos >= r->inlen) return -1;
        r->bitbuf = r->in[r->inpos++];
        r->bitcnt = 8;
    }
    int bit = r->bitbuf & 1;
    r->bitbuf >>= 1;
    r->bitcnt--;
    return bit;
}

/* DEFLATE packs multi-bit fields LSB-first (unlike Huffman codes, which
 * are MSB-first - yes, both conventions show up in the same stream). */
static int next_bits(Reader *r, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        int bit = next_bit(r);
        if (bit < 0) return -1;
        v |= bit << i;
    }
    return v;
}

/* Turn a list of per-symbol code lengths into a canonical Huffman table.
 * Standard two-pass approach: count how many symbols land at each length,
 * then walk the symbols again and slot each one into its length's bucket
 * in ascending symbol order. */
static void build_table(Huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;

    short bucket_start[MAXBITS + 1];
    bucket_start[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        bucket_start[len + 1] = bucket_start[len] + h->count[len];

    for (int sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbol[bucket_start[lengths[sym]]++] = (short)sym;
}

/* Pull one Huffman symbol off the bitstream. Codes are MSB-first and of
 * varying length, so we grow the candidate code bit by bit and check it
 * against the range of codes assigned to that length. */
static int read_symbol(Reader *r, const Huff *h)
{
    int code = 0, first_code = 0, table_index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = next_bit(r);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - first_code < count) return h->symbol[table_index + (code - first_code)];
        table_index += count;
        first_code = (first_code + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* Length/distance tables from RFC 1951 section 3.2.5 - do not touch, these
 * are the standard's numbers, not something to "clean up". */
static const short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* Decode literal/length/distance symbols until we hit the end-of-block
 * marker (256), copying matched runs straight out of what we've already
 * written to out - that's the whole "LZ77" part of DEFLATE. */
static int run_block(Reader *r, const Huff *litlen, const Huff *dist)
{
    for (;;) {
        int sym = read_symbol(r, litlen);
        if (sym < 0) return -1;
        if (sym == 256) return 0;
        if (sym < 256) {
            if (r->outpos >= r->outcap) return -1;
            r->out[r->outpos++] = (uint8_t)sym;
            continue;
        }

        sym -= 257;
        if (sym >= 29) return -1;
        int len = LEN_BASE[sym];
        if (LEN_EXTRA[sym]) {
            int extra = next_bits(r, LEN_EXTRA[sym]);
            if (extra < 0) return -1;
            len += extra;
        }

        int dsym = read_symbol(r, dist);
        if (dsym < 0 || dsym >= 30) return -1;
        int back = DIST_BASE[dsym];
        if (DIST_EXTRA[dsym]) {
            int extra = next_bits(r, DIST_EXTRA[dsym]);
            if (extra < 0) return -1;
            back += extra;
        }

        /* back-reference can't point before the start of the output,
         * and the copy has to fit in what's left of the buffer */
        if (back > r->outpos) return -1;
        if (r->outpos + len > r->outcap) return -1;
        int src = r->outpos - back;
        for (int i = 0; i < len; i++) r->out[r->outpos + i] = r->out[src + i];
        r->outpos += len;
    }
}

/* Block type 1: literal/length and distance codes are fixed by the spec
 * rather than transmitted, so build them once and reuse across calls. */
static int fixed_huffman_block(Reader *r)
{
    static Huff litlen, dist;
    static int ready = 0;
    if (!ready) {
        uint8_t lens[288];
        for (int i = 0;   i < 144; i++) lens[i] = 8;
        for (int i = 144; i < 256; i++) lens[i] = 9;
        for (int i = 256; i < 280; i++) lens[i] = 7;
        for (int i = 280; i < 288; i++) lens[i] = 8;
        build_table(&litlen, lens, 288);

        uint8_t dlens[30];
        for (int i = 0; i < 30; i++) dlens[i] = 5;
        build_table(&dist, dlens, 30);
        ready = 1;
    }
    return run_block(r, &litlen, &dist);
}

/* Both the "repeat previous length" and "repeat zero" code-length symbols
 * (16/17/18) boil down to "write the same value N more times, but stop
 * early if we hit the declared total" - shared here instead of copy-pasted
 * three times. */
static void fill_run(uint8_t *lengths, int *n, int total, int count, uint8_t value)
{
    while (count-- && *n < total) lengths[(*n)++] = value;
}

/* Block type 2: literal/length and distance code lengths are themselves
 * Huffman-coded using a third table (the "code length" alphabet), which
 * is what HCLEN/the ORDER permutation below are about. */
static int dynamic_huffman_block(Reader *r)
{
    int hlit = next_bits(r, 5);   if (hlit  < 0) return -1; hlit  += 257;
    int hdist = next_bits(r, 5);  if (hdist < 0) return -1; hdist += 1;
    int hclen = next_bits(r, 4);  if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    static const uint8_t CL_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t cl_lengths[19];
    for (int i = 0; i < 19; i++) cl_lengths[i] = 0;
    for (int i = 0; i < hclen; i++) {
        int v = next_bits(r, 3);
        if (v < 0) return -1;
        cl_lengths[CL_ORDER[i]] = (uint8_t)v;
    }

    Huff cl_table;
    build_table(&cl_table, cl_lengths, 19);

    uint8_t lengths[MAXLCODES + MAXDCODES];
    int total = hlit + hdist;
    int n = 0;
    while (n < total) {
        int sym = read_symbol(r, &cl_table);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;             /* nothing previous to repeat */
            int rep = next_bits(r, 2);
            if (rep < 0) return -1;
            fill_run(lengths, &n, total, rep + 3, lengths[n - 1]);
        } else if (sym == 17) {
            int rep = next_bits(r, 3);
            if (rep < 0) return -1;
            fill_run(lengths, &n, total, rep + 3, 0);
        } else {
            int rep = next_bits(r, 7);
            if (rep < 0) return -1;
            fill_run(lengths, &n, total, rep + 11, 0);
        }
    }
    if (n != total) return -1;

    Huff litlen, dist;
    build_table(&litlen, lengths, hlit);
    build_table(&dist, lengths + hlit, hdist);
    return run_block(r, &litlen, &dist);
}

/* Block type 0: no compression, just LEN bytes copied verbatim after
 * padding out to the next byte boundary and skipping LEN's redundant
 * complement (NLEN). */
static int stored_block(Reader *r)
{
    r->bitcnt = 0;
    if (r->inpos + 4 > r->inlen) return -1;
    int len = r->in[r->inpos] | (r->in[r->inpos + 1] << 8);
    r->inpos += 4;
    if (r->inpos + len > r->inlen) return -1;
    if (r->outpos + len > r->outcap) return -1;
    for (int i = 0; i < len; i++) r->out[r->outpos++] = r->in[r->inpos++];
    return 0;
}

int inflate_raw(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    Reader r;
    r.in = src; r.inlen = srclen; r.inpos = 0;
    r.bitbuf = 0; r.bitcnt = 0;
    r.out = dst; r.outcap = dstcap; r.outpos = 0;

    int final_block;
    do {
        final_block = next_bit(&r);
        if (final_block < 0) return -1;
        int type = next_bits(&r, 2);

        int status;
        switch (type) {
            case 0:  status = stored_block(&r); break;
            case 1:  status = fixed_huffman_block(&r); break;
            case 2:  status = dynamic_huffman_block(&r); break;
            default: return -1;                /* type 3 is reserved/invalid */
        }
        if (status < 0) return -1;
    } while (!final_block);

    return r.outpos;
}

int zlib_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen < 2) return -1;
    if ((src[0] & 0x0F) != 8) return -1;               /* CM: must be deflate */
    if (((src[0] << 8) | src[1]) % 31 != 0) return -1; /* header checksum */

    int header_len = 2;
    if (src[1] & 0x20) header_len += 4;                /* preset dictionary present */
    return inflate_raw(src + header_len, srclen - header_len, dst, dstcap);
}

int gzip_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen < 18) return -1;
    if (src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) return -1;

    int flags = src[3];
    int pos = 10;
    if (flags & 0x04) {                                /* FEXTRA */
        if (pos + 2 > srclen) return -1;
        int xlen = src[pos] | (src[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { while (pos < srclen && src[pos]) pos++; pos++; }  /* FNAME */
    if (flags & 0x10) { while (pos < srclen && src[pos]) pos++; pos++; }  /* FCOMMENT */
    if (flags & 0x02) pos += 2;                        /* FHCRC */
    if (pos >= srclen) return -1;
    return inflate_raw(src + pos, srclen - pos, dst, dstcap);
}

int auto_inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap)
{
    if (srclen >= 2 && src[0] == 0x1F && src[1] == 0x8B)
        return gzip_inflate(src, srclen, dst, dstcap);
    if (srclen >= 2 && (src[0] & 0x0F) == 8 && ((src[0] << 8) | src[1]) % 31 == 0)
        return zlib_inflate(src, srclen, dst, dstcap);
    return inflate_raw(src, srclen, dst, dstcap);
}
