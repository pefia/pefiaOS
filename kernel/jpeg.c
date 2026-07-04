/* kernel/jpeg.c - baseline sequential JPEG (SOF0) decoder.
 * Handles 8-bit, 1 or 3 component images at 4:4:4/4:2:2/4:2:0 subsampling
 * with restart markers. No progressive JPEG (SOF2) support, and the IDCT
 * below is fixed-point since there's no FPU to spend on this. Numeric tables
 * (the IDCT cosine matrix, zigzag order) are exact per the JPEG spec - don't
 * touch them without checking output against a reference decoder. */
#include "image.h"
#include "heap.h"

static const int IC[8][8] = {
  {  724,  724,  724,  724,  724,  724,  724,  724},
  { 1004,  851,  569,  200, -200, -569, -851,-1004},
  {  946,  392, -392, -946, -946, -392,  392,  946},
  {  851, -200,-1004, -569,  569, 1004,  200, -851},
  {  724, -724, -724,  724,  724, -724, -724,  724},
  {  569,-1004,  200,  851, -851, -200, 1004, -569},
  {  392, -946,  946, -392, -392,  946, -946,  392},
  {  200, -569,  851,-1004, 1004, -851,  569, -200},
};

static const uint8_t ZZ[64] = {
   0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
  12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
  35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
  58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

typedef struct {
    int count[17];
    uint8_t huffval[256];
    int mincode[17], maxcode[17], valptr[17];
    int present;
} HTbl;

typedef struct { int id, h, v, tq, td, ta, pred; } Comp;

typedef struct {
    const uint8_t *d; int len, pos;
    int bitbuf, bitcnt, hit_marker;
} BR;

static int fill_byte(BR *b)
{
    if (b->pos >= b->len) return -1;
    int c = b->d[b->pos++];
    if (c == 0xFF) {
        int c2 = (b->pos < b->len) ? b->d[b->pos] : 0xD9;
        if (c2 == 0) { b->pos++; return 0xFF; }    /* stuffed byte */
        b->pos--;                                  /* leave marker for caller */
        b->hit_marker = 1;
        return -1;
    }
    return c;
}

static int getbit(BR *b)
{
    if (b->bitcnt == 0) {
        int c = fill_byte(b);
        if (c < 0) return 0;                        /* pad past marker */
        b->bitbuf = c; b->bitcnt = 8;
    }
    int bit = (b->bitbuf >> 7) & 1;
    b->bitbuf = (b->bitbuf << 1) & 0xFF;
    b->bitcnt--;
    return bit;
}

static int receive(BR *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | getbit(b);
    return v;
}

static int extend(int v, int n)
{
    if (n == 0) return 0;
    if (v < (1 << (n - 1))) v += (-(1 << n)) + 1;
    return v;
}

static void build_huff(HTbl *t, const uint8_t *counts, const uint8_t *vals)
{
    int huffsize[257]; int huffcode[257];
    int p = 0;
    for (int L = 1; L <= 16; L++) for (int i = 0; i < counts[L]; i++) huffsize[p++] = L;
    huffsize[p] = 0;
    int lastp = p;

    int code = 0, si = huffsize[0]; p = 0;
    while (huffsize[p]) {
        while (huffsize[p] == si) { huffcode[p++] = code++; }
        code <<= 1; si++;
    }

    for (int i = 0; i < lastp; i++) t->huffval[i] = vals[i];

    p = 0;
    for (int L = 1; L <= 16; L++) {
        if (counts[L]) { t->valptr[L] = p; t->mincode[L] = huffcode[p]; p += counts[L]; t->maxcode[L] = huffcode[p - 1]; }
        else t->maxcode[L] = -1;
        t->count[L] = counts[L];
    }
    t->present = 1;
}

static int huff_decode(BR *b, const HTbl *t)
{
    int code = 0;
    for (int L = 1; L <= 16; L++) {
        code = (code << 1) | getbit(b);
        if (t->maxcode[L] >= 0 && code <= t->maxcode[L])
            return t->huffval[t->valptr[L] + code - t->mincode[L]];
    }
    return 0;
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* 8x8 integer IDCT: in = dequantized natural-order coeffs, out = spatial+128. */
static void idct8x8(const int *in, uint8_t *out, int out_stride)
{
    long long tmp[64];
    for (int v = 0; v < 8; v++)
        for (int x = 0; x < 8; x++) {
            long long s = 0;
            for (int u = 0; u < 8; u++) s += (long long)IC[u][x] * in[v * 8 + u];
            tmp[v * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            long long s = 0;
            for (int v = 0; v < 8; v++) s += (long long)IC[v][y] * tmp[v * 8 + x];
            int val = (int)(s / (4 * 1024 * 1024)) + 128;
            out[y * out_stride + x] = (uint8_t)clamp8(val);
        }
}

#define MAX_PIXELS (3000 * 3000)

/* Bilinear-sample a (possibly subsampled) component plane at full-res (x,y).
 * sh/sv are the upsample factors (Hmax/comp.h etc); chroma sample centers are
 * offset by (s-1)/2 luma pixels, matching libjpeg's smooth upsampling. */
static int sample_comp(const uint8_t *plane, int pw, int ph, int sh, int sv, int x, int y)
{
    int fx = (2 * x - (sh - 1)) * 128 / sh;     /* chroma x * 256 */
    int fy = (2 * y - (sv - 1)) * 128 / sv;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    int x0 = fx >> 8, xf = fx & 255;
    int y0 = fy >> 8, yf = fy & 255;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 >= pw) x0 = pw - 1;
    if (x1 >= pw) x1 = pw - 1;
    if (y0 >= ph) y0 = ph - 1;
    if (y1 >= ph) y1 = ph - 1;
    int v00 = plane[y0 * pw + x0], v01 = plane[y0 * pw + x1];
    int v10 = plane[y1 * pw + x0], v11 = plane[y1 * pw + x1];
    int top = v00 + (((v01 - v00) * xf) >> 8);
    int bot = v10 + (((v11 - v10) * xf) >> 8);
    return top + (((bot - top) * yf) >> 8);
}

int jpeg_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return -1;

    uint16_t qt[4][64];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 64; j++) qt[i][j] = 0;
    HTbl dc[4], ac[4];
    for (int i = 0; i < 4; i++) { dc[i].present = 0; ac[i].present = 0; }
    Comp comp[3]; int ncomp = 0;
    int width = 0, height = 0;
    int restart = 0;
    int Hmax = 1, Vmax = 1;

    int p = 2;
    /* parse headers up to SOS */
    while (p + 4 <= len) {
        if (data[p] != 0xFF) { p++; continue; }
        int marker = data[p + 1];
        p += 2;
        if (marker == 0xD9) return -1;                 /* EOI before SOS */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (p + 2 > len) return -1;
        int seg = (data[p] << 8) | data[p + 1];
        const uint8_t *sd = data + p + 2;
        int slen = seg - 2;
        if (p + seg > len) return -1;

        if (marker == 0xDB) {                          /* DQT */
            int q = 0;
            while (q < slen) {
                int pq = sd[q] >> 4, tq = sd[q] & 15; q++;
                if (tq > 3) return -1;
                for (int i = 0; i < 64; i++) {
                    if (pq) { qt[tq][i] = (sd[q] << 8) | sd[q + 1]; q += 2; }
                    else qt[tq][i] = sd[q++];
                }
            }
        } else if (marker == 0xC0) {                   /* SOF0 baseline */
            int prec = sd[0]; (void)prec;
            height = (sd[1] << 8) | sd[2];
            width = (sd[3] << 8) | sd[4];
            ncomp = sd[5];
            if (ncomp != 1 && ncomp != 3) return -1;
            for (int c = 0; c < ncomp; c++) {
                comp[c].id = sd[6 + c * 3];
                comp[c].h = sd[7 + c * 3] >> 4;
                comp[c].v = sd[7 + c * 3] & 15;
                comp[c].tq = sd[8 + c * 3];
                comp[c].pred = 0;
                if (comp[c].h > Hmax) Hmax = comp[c].h;
                if (comp[c].v > Vmax) Vmax = comp[c].v;
            }
        } else if (marker == 0xC2) {                   /* progressive: unsupported */
            return -1;
        } else if (marker == 0xC4) {                   /* DHT */
            int q = 0;
            while (q < slen) {
                int tc = sd[q] >> 4, th = sd[q] & 15; q++;
                uint8_t counts[17]; int total = 0;
                counts[0] = 0;
                for (int i = 1; i <= 16; i++) { counts[i] = sd[q++]; total += counts[i]; }
                const uint8_t *vals = sd + q; q += total;
                if (th > 3) return -1;
                if (tc) build_huff(&ac[th], counts, vals);
                else build_huff(&dc[th], counts, vals);
            }
        } else if (marker == 0xDD) {                   /* DRI */
            restart = (sd[0] << 8) | sd[1];
        } else if (marker == 0xDA) {                   /* SOS */
            int ns = sd[0];
            for (int i = 0; i < ns; i++) {
                int cid = sd[1 + i * 2];
                int t = sd[2 + i * 2];
                for (int c = 0; c < ncomp; c++) if (comp[c].id == cid) { comp[c].td = t >> 4; comp[c].ta = t & 15; }
            }
            p += seg;                                  /* entropy data starts here */
            goto decode;
        }
        p += seg;
    }
    return -1;

decode:
    if (width <= 0 || height <= 0 || (long)width * height > MAX_PIXELS) return -1;

    int mcux = (width + Hmax * 8 - 1) / (Hmax * 8);
    int mcuy = (height + Vmax * 8 - 1) / (Vmax * 8);

    uint8_t *plane[3]; int pw[3], ph[3];
    for (int c = 0; c < ncomp; c++) {
        pw[c] = mcux * comp[c].h * 8;
        ph[c] = mcuy * comp[c].v * 8;
        plane[c] = (uint8_t *)kmalloc(pw[c] * ph[c]);
        if (!plane[c]) { for (int k = 0; k < c; k++) kfree(plane[k]); return -1; }
    }

    BR br; br.d = data + p; br.len = len - p; br.pos = 0; br.bitbuf = 0; br.bitcnt = 0; br.hit_marker = 0;

    int mcu_count = 0;
    for (int my = 0; my < mcuy; my++) {
        for (int mx = 0; mx < mcux; mx++) {
            if (restart && mcu_count > 0 && (mcu_count % restart) == 0) {
                /* align + consume RSTn */
                br.bitcnt = 0;
                while (br.pos + 1 < br.len) {
                    if (br.d[br.pos] == 0xFF && br.d[br.pos + 1] >= 0xD0 && br.d[br.pos + 1] <= 0xD7) { br.pos += 2; break; }
                    br.pos++;
                }
                br.hit_marker = 0;
                for (int c = 0; c < ncomp; c++) comp[c].pred = 0;
            }

            for (int c = 0; c < ncomp; c++) {
                for (int by = 0; by < comp[c].v; by++) {
                    for (int bx = 0; bx < comp[c].h; bx++) {
                        int coef[64];
                        for (int i = 0; i < 64; i++) coef[i] = 0;

                        int t = huff_decode(&br, &dc[comp[c].td]);
                        int diff = t ? extend(receive(&br, t), t) : 0;
                        comp[c].pred += diff;
                        coef[0] = comp[c].pred * qt[comp[c].tq][0];

                        int k = 1;
                        while (k < 64) {
                            int rs = huff_decode(&br, &ac[comp[c].ta]);
                            int r = rs >> 4, s = rs & 15;
                            if (s == 0) { if (r == 15) { k += 16; continue; } else break; }
                            k += r;
                            if (k > 63) break;
                            int val = extend(receive(&br, s), s);
                            coef[ZZ[k]] = val * qt[comp[c].tq][k];
                            k++;
                        }

                        int px = (mx * comp[c].h + bx) * 8;
                        int py = (my * comp[c].v + by) * 8;
                        idct8x8(coef, plane[c] + py * pw[c] + px, pw[c]);
                    }
                }
            }
            mcu_count++;
        }
    }

    /* assemble RGBA */
    out->pixels = (uint32_t *)kmalloc(width * height * 4);
    if (!out->pixels) { for (int c = 0; c < ncomp; c++) kfree(plane[c]); return -1; }
    out->width = width; out->height = height; out->stride = width; out->has_alpha = 0;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int Y, Cb = 128, Cr = 128;
            Y  = sample_comp(plane[0], pw[0], ph[0], Hmax / comp[0].h, Vmax / comp[0].v, x, y);
            if (ncomp == 3) {
                Cb = sample_comp(plane[1], pw[1], ph[1], Hmax / comp[1].h, Vmax / comp[1].v, x, y);
                Cr = sample_comp(plane[2], pw[2], ph[2], Hmax / comp[2].h, Vmax / comp[2].v, x, y);
            }
            int r, g, b;
            if (ncomp == 1) { r = g = b = Y; }
            else {
                int cb = Cb - 128, cr = Cr - 128;
                r = Y + ((91881 * cr) >> 16);
                g = Y - ((22554 * cb + 46802 * cr) >> 16);
                b = Y + ((116130 * cb) >> 16);
            }
            out->pixels[y * width + x] = ((uint32_t)255 << 24) |
                ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | (uint32_t)clamp8(b);
        }
    }

    for (int c = 0; c < ncomp; c++) kfree(plane[c]);
    return 0;
}
