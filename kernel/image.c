#include "image.h"
#include "inflate.h"
#include "heap.h"

/* Format sniffing + decoders for the image types we bother supporting.
 * PNG and GIF are handled right here; JPEG lives in jpeg.c because it's
 * a whole different beast (DCT, quant tables, Huffman-coded residuals)
 * and BMP is simple enough that bitmap.c owns it directly. */

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* A 3000x3000 image is already a lot for this kernel's heap; treat
 * anything bigger as hostile/corrupt rather than trying to honor it. */
#define MAX_PIXELS (3000 * 3000)

static int alloc_bitmap(Bitmap *out, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    if ((long)w * h > MAX_PIXELS) return -1;
    out->pixels = (uint32_t *)kmalloc((uint32_t)(w * h * 4));
    if (!out->pixels) return -1;
    out->width = w;
    out->height = h;
    out->stride = w;
    out->has_alpha = 1;
    return 0;
}

static uint32_t pack_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* --- PNG -------------------------------------------------------------
 * Enough of the spec to cover what browsers actually emit: all five
 * color types, bit depths 1/2/4/8/16, tRNS transparency, no interlacing
 * (Adam7 support would roughly double this function for a feature we've
 * never actually hit in the wild).
 */

static int paeth_predict(int a, int b, int c)
{
    int guess = a + b - c;
    int da = iabs(guess - a), db = iabs(guess - b), dc = iabs(guess - c);
    if (da <= db && da <= dc) return a;
    if (db <= dc) return b;
    return c;
}

static int png_decode(const uint8_t *data, int len, Bitmap *out)
{
    static const uint8_t PNG_SIG[8] = {137,80,78,71,13,10,26,10};
    if (len < 33) return -1;
    for (int i = 0; i < 8; i++) if (data[i] != PNG_SIG[i]) return -1;

    int width = 0, height = 0, bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t palette[256][3];
    int pal_n = 0;
    uint8_t trns[256];
    int trns_n = 0;
    uint16_t trns_gray = 0xFFFF, trns_r = 0xFFFF, trns_g = 0xFFFF, trns_b = 0xFFFF;
    int have_color_key = 0;
    int saw_ihdr = 0;

    /* IDAT chunks can be split arbitrarily by the encoder; stitch them
     * back into one buffer before handing it to zlib_inflate. */
    int idat_cap = len; /* can't possibly need more than the whole file */
    uint8_t *idat = (uint8_t *)kmalloc(idat_cap);
    if (!idat) return -1;
    int idat_len = 0;

    int p = 8;
    while (p + 8 <= len) {
        uint32_t clen = read_be32(data + p);
        const uint8_t *tag = data + p + 4;
        const uint8_t *body = data + p + 8;

        /* clen comes straight from the file - do the bounds check in a
         * way that can't be fooled by clen wrapping when truncated to
         * int (a 4GB "chunk length" in a corrupt file used to sail
         * right past a signed-overflow version of this check). */
        int room = len - p - 12;
        if (room < 0 || clen > (uint32_t)room) break;

        if (tag[0]=='I'&&tag[1]=='H'&&tag[2]=='D'&&tag[3]=='R') {
            width = (int)read_be32(body);
            height = (int)read_be32(body + 4);
            bitdepth = body[8];
            colortype = body[9];
            interlace = body[12];
            saw_ihdr = 1;
        } else if (tag[0]=='P'&&tag[1]=='L'&&tag[2]=='T'&&tag[3]=='E') {
            pal_n = clen / 3;
            if (pal_n > 256) pal_n = 256;
            for (int i = 0; i < pal_n; i++) {
                palette[i][0] = body[i*3]; palette[i][1] = body[i*3+1]; palette[i][2] = body[i*3+2];
            }
        } else if (tag[0]=='t'&&tag[1]=='R'&&tag[2]=='N'&&tag[3]=='S') {
            if (colortype == 3) {
                trns_n = clen > 256 ? 256 : (int)clen;
                for (int i = 0; i < trns_n; i++) trns[i] = body[i];
            } else if (colortype == 0 && clen >= 2) {
                trns_gray = (body[0] << 8) | body[1];
                have_color_key = 1;
            } else if (colortype == 2 && clen >= 6) {
                trns_r = (body[0] << 8) | body[1];
                trns_g = (body[2] << 8) | body[3];
                trns_b = (body[4] << 8) | body[5];
                have_color_key = 1;
            }
        } else if (tag[0]=='I'&&tag[1]=='D'&&tag[2]=='A'&&tag[3]=='T') {
            if (idat_len + (int)clen <= idat_cap)
                for (uint32_t i = 0; i < clen; i++) idat[idat_len++] = body[i];
        } else if (tag[0]=='I'&&tag[1]=='E'&&tag[2]=='N'&&tag[3]=='D') {
            break;
        }
        p += 12 + (int)clen;
    }

    if (!saw_ihdr || interlace != 0 || width <= 0 || height <= 0) { kfree(idat); return -1; }
    if (!(bitdepth==1||bitdepth==2||bitdepth==4||bitdepth==8||bitdepth==16)) { kfree(idat); return -1; }
    if ((long)width * height > MAX_PIXELS) { kfree(idat); return -1; }

    int channels = (colortype==2) ? 3 : (colortype==6) ? 4 : (colortype==4) ? 2 : 1;
    int bpp_bytes = (bitdepth * channels + 7) / 8;
    if (bpp_bytes < 1) bpp_bytes = 1;
    int stride = (width * channels * bitdepth + 7) / 8;
    int rawsize = height * (stride + 1); /* +1 per row for the filter-type byte */

    uint8_t *raw = (uint8_t *)kmalloc(rawsize);
    if (!raw) { kfree(idat); return -1; }
    int got = zlib_inflate(idat, idat_len, raw, rawsize);
    kfree(idat);
    if (got < rawsize) { kfree(raw); return -1; }

    /* Undo the per-scanline filters in place, dropping the filter-type
     * byte so what's left is contiguous, unfiltered scanlines. */
    uint8_t *lines = (uint8_t *)kmalloc(height * stride);
    if (!lines) { kfree(raw); return -1; }
    for (int y = 0; y < height; y++) {
        uint8_t filter = raw[y * (stride + 1)];
        const uint8_t *src = raw + y * (stride + 1) + 1;
        uint8_t *cur = lines + y * stride;
        uint8_t *prev = (y > 0) ? lines + (y - 1) * stride : 0;
        for (int x = 0; x < stride; x++) {
            int a = (x >= bpp_bytes) ? cur[x - bpp_bytes] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp_bytes) ? prev[x - bpp_bytes] : 0;
            int v = src[x];
            switch (filter) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth_predict(a, b, c); break;
                default: break; /* 0 = none, anything unrecognized: leave as-is */
            }
            cur[x] = (uint8_t)v;
        }
    }
    kfree(raw);

    if (alloc_bitmap(out, width, height) != 0) { kfree(lines); return -1; }

    int maxval = (1 << bitdepth) - 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = lines + y * stride;
        for (int x = 0; x < width; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bitdepth == 8) {
                const uint8_t *px = row + x * channels;
                if (colortype == 0) {
                    r = g = b = px[0];
                    if (have_color_key && px[0] == (uint8_t)trns_gray) a = 0;
                } else if (colortype == 2) {
                    r = px[0]; g = px[1]; b = px[2];
                    if (have_color_key && px[0]==trns_r && px[1]==trns_g && px[2]==trns_b) a = 0;
                } else if (colortype == 3) {
                    int idx = px[0];
                    if (idx < pal_n) { r = palette[idx][0]; g = palette[idx][1]; b = palette[idx][2]; }
                    a = (idx < trns_n) ? trns[idx] : 255;
                } else if (colortype == 4) {
                    r = g = b = px[0]; a = px[1];
                } else {
                    r = px[0]; g = px[1]; b = px[2]; a = px[3];
                }
            } else if (bitdepth == 16) {
                /* we only keep the high byte of each 16-bit sample */
                const uint8_t *px = row + x * channels * 2;
                if (colortype == 0) { r = g = b = px[0]; }
                else if (colortype == 2) { r = px[0]; g = px[2]; b = px[4]; }
                else if (colortype == 4) { r = g = b = px[0]; a = px[2]; }
                else { r = px[0]; g = px[2]; b = px[4]; a = px[6]; }
            } else {
                /* sub-byte depths only ever mean grayscale or a palette index */
                int bitpos = x * bitdepth;
                int byte = row[bitpos >> 3];
                int shift = 8 - bitdepth - (bitpos & 7);
                int val = (byte >> shift) & maxval;
                if (colortype == 3) {
                    if (val < pal_n) { r = palette[val][0]; g = palette[val][1]; b = palette[val][2]; }
                    a = (val < trns_n) ? trns[val] : 255;
                } else {
                    int scaled = val * 255 / maxval;
                    r = g = b = (uint8_t)scaled;
                    if (have_color_key && val == (int)trns_gray) a = 0;
                }
            }
            out->pixels[y * width + x] = pack_argb(a, r, g, b);
        }
    }
    kfree(lines);
    return 0;
}

/* --- GIF ---------------------------------------------------------------
 * 87a/89a, first frame only (no animation support - we're not trying to
 * be a GIF player, just render whatever's embedded in a page). Handles
 * Adam7-style GIF interlacing since that one's cheap: four passes over
 * a flat index buffer instead of a real streaming decode.
 */

static int gif_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (len < 13) return -1;
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return -1;

    int width = data[6] | (data[7] << 8);
    int height = data[8] | (data[9] << 8);
    int flags = data[10];
    int has_gct = (flags & 0x80) != 0;
    int gct_size = 2 << (flags & 7);
    if (width <= 0 || height <= 0 || (long)width * height > MAX_PIXELS) return -1;

    uint8_t gct[256][3];
    for (int i = 0; i < 256; i++) { gct[i][0] = gct[i][1] = gct[i][2] = 0; }
    int p = 13;
    if (has_gct) {
        for (int i = 0; i < gct_size && p + 3 <= len; i++) {
            gct[i][0] = data[p]; gct[i][1] = data[p+1]; gct[i][2] = data[p+2];
            p += 3;
        }
    }

    int transparent_idx = -1;

    /* Walk top-level blocks (extensions, comments, etc.) until we find
     * the first image descriptor - that's the frame we render. */
    while (p < len) {
        uint8_t block = data[p++];
        if (block == 0x3B) return -1; /* trailer reached, no image in file */

        if (block == 0x21) { /* extension block */
            uint8_t label = (p < len) ? data[p++] : 0;
            if (label == 0xF9) { /* graphic control extension */
                int sz = (p < len) ? data[p++] : 0;
                if (sz >= 4 && p + 4 <= len) {
                    if (data[p] & 1) transparent_idx = data[p + 3];
                }
                p += sz;
            }
            while (p < len) { int sz = data[p++]; if (!sz) break; p += sz; }
            continue;
        }

        if (block == 0x2C) { /* image descriptor */
            if (p + 9 > len) return -1;
            int iw = data[p+4] | (data[p+5] << 8);
            int ih = data[p+6] | (data[p+7] << 8);
            int local_flags = data[p+8];
            int interlaced = (local_flags & 0x40) != 0;
            p += 9;

            uint8_t lct[256][3];
            for (int i = 0; i < 256; i++) { lct[i][0] = lct[i][1] = lct[i][2] = 0; }
            int has_lct = (local_flags & 0x80) != 0;
            int lct_size = 2 << (local_flags & 7);
            uint8_t (*active_pal)[3] = gct;
            if (has_lct) {
                for (int i = 0; i < lct_size && p + 3 <= len; i++) {
                    lct[i][0] = data[p]; lct[i][1] = data[p+1]; lct[i][2] = data[p+2];
                    p += 3;
                }
                active_pal = lct;
            }
            if (iw <= 0 || ih <= 0 || (long)iw * ih > MAX_PIXELS) return -1;

            int min_code_size = (p < len) ? data[p++] : 8;
            if (min_code_size < 2 || min_code_size > 8) return -1;

            /* Sub-blocks of LZW-compressed indices - reassemble into one
             * contiguous buffer before decoding. */
            int comp_cap = len;
            uint8_t *comp = (uint8_t *)kmalloc(comp_cap);
            if (!comp) return -1;
            int comp_len = 0;
            while (p < len) {
                int sz = data[p++];
                if (!sz) break;
                for (int i = 0; i < sz && p < len; i++) comp[comp_len++] = data[p++];
            }

            int clear_code = 1 << min_code_size;
            int eoi_code = clear_code + 1;
            int code_bits = min_code_size + 1;
            int next_code = eoi_code + 1;
            static int lzw_prefix[4096];
            static uint8_t lzw_suffix[4096], lzw_first[4096];
            uint8_t *indices = (uint8_t *)kmalloc(iw * ih);
            if (!indices) { kfree(comp); return -1; }
            int written = 0;
            int bitpos = 0;
            int prev_code = -1;
            uint8_t stack[4096];
            int sp = 0;

            for (int i = 0; i < clear_code; i++) {
                lzw_prefix[i] = -1; lzw_suffix[i] = (uint8_t)i; lzw_first[i] = (uint8_t)i;
            }

            while (bitpos + code_bits <= comp_len * 8) {
                int code = 0;
                for (int i = 0; i < code_bits; i++) {
                    int byte_idx = (bitpos + i) >> 3, bit_idx = (bitpos + i) & 7;
                    if (byte_idx < comp_len && (comp[byte_idx] >> bit_idx) & 1) code |= (1 << i);
                }
                bitpos += code_bits;

                if (code == clear_code) {
                    code_bits = min_code_size + 1;
                    next_code = eoi_code + 1;
                    prev_code = -1;
                    continue;
                }
                if (code == eoi_code) break;

                if (prev_code == -1) {
                    if (code < next_code) {
                        if (written < iw * ih) indices[written++] = lzw_first[code];
                        prev_code = code;
                    }
                    continue;
                }

                int incoming = code;
                sp = 0;
                if (code >= next_code) { stack[sp++] = lzw_first[prev_code]; code = prev_code; }
                while (code >= clear_code) {
                    if (sp >= 4096 || code < 0 || code >= 4096) break;
                    stack[sp++] = lzw_suffix[code];
                    code = lzw_prefix[code];
                }
                if (code >= 0 && code < 4096) stack[sp++] = lzw_first[code];

                int leader = (code >= 0 && code < 4096) ? lzw_first[code] : 0;
                while (sp > 0 && written < iw * ih) indices[written++] = stack[--sp];

                if (next_code < 4096) {
                    lzw_prefix[next_code] = prev_code;
                    lzw_suffix[next_code] = (uint8_t)leader;
                    lzw_first[next_code] = lzw_first[prev_code];
                    next_code++;
                    if (next_code == (1 << code_bits) && code_bits < 12) code_bits++;
                }
                prev_code = incoming;
            }
            kfree(comp);

            if (alloc_bitmap(out, iw, ih) != 0) { kfree(indices); return -1; }

            /* indices[] holds rows in storage order; for interlaced GIFs
             * that's four passes at increasing density, not top-to-bottom. */
            static const int PASS_START[4] = {0, 4, 2, 1};
            static const int PASS_STEP[4]  = {8, 8, 4, 2};
            int src_row = 0;
            for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
                int start = interlaced ? PASS_START[pass] : 0;
                int step  = interlaced ? PASS_STEP[pass]  : 1;
                for (int row = start; row < ih; row += step) {
                    for (int col = 0; col < iw; col++) {
                        int ci = indices[src_row * iw + col];
                        uint8_t rr = active_pal[ci][0], gg = active_pal[ci][1], bb = active_pal[ci][2];
                        uint8_t a = (ci == transparent_idx) ? 0 : 255;
                        out->pixels[row * iw + col] = pack_argb(a, rr, gg, bb);
                    }
                    src_row++;
                }
            }
            kfree(indices);
            return 0;
        }

        return -1; /* some block type we don't understand - give up */
    }
    return -1;
}

/* jpeg_decode lives in jpeg.c; no shared header exists for it (it's only
 * ever called from here), so just forward-declare it like the original
 * code did. */
int jpeg_decode(const uint8_t *data, int len, Bitmap *out);

int img_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (!data || len < 4 || !out) return -1;
    out->pixels = 0;
    out->width = out->height = 0;

    if (data[0]==137 && data[1]=='P' && data[2]=='N' && data[3]=='G') return png_decode(data, len, out);
    if (data[0]=='G' && data[1]=='I' && data[2]=='F') return gif_decode(data, len, out);
    if (data[0]==0xFF && data[1]==0xD8) return jpeg_decode(data, len, out);
    if (data[0]=='B' && data[1]=='M') return bmp_decode(data, len, out);
    return -1;
}
