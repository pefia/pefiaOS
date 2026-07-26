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
 * 87a/89a with animation: each image descriptor is composited onto a
 * persistent logical-screen canvas and snapshotted into its own Bitmap.
 * Handles GIF interlacing since that one's cheap: four passes over a
 * flat index buffer instead of a real streaming decode.
 */

/* Decodes one image's LZW sub-blocks (starting at the min-code-size byte
 * at *pp) into a fresh iw*ih index buffer in top-to-bottom row order,
 * interlacing undone. Advances *pp past the block terminator. Returns
 * the kmalloc'd buffer, or 0 on failure. */
static uint8_t *gif_lzw_frame(const uint8_t *data, int len, int *pp,
                              int iw, int ih, int interlaced)
{
    int p = *pp;
    int min_code_size = (p < len) ? data[p++] : 8;
    if (min_code_size < 2 || min_code_size > 8) return 0;

    /* Sub-blocks of LZW-compressed indices - reassemble into one
     * contiguous buffer before decoding. */
    int comp_cap = len;
    uint8_t *comp = (uint8_t *)kmalloc(comp_cap);
    if (!comp) return 0;
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
    if (!indices) { kfree(comp); return 0; }
    for (int i = 0; i < iw * ih; i++) indices[i] = 0; /* truncated data reads as index 0 */
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

    /* indices[] holds rows in storage order; for interlaced GIFs that's
     * four passes at increasing density - permute into plain row order. */
    if (interlaced) {
        uint8_t *ordered = (uint8_t *)kmalloc(iw * ih);
        if (!ordered) { kfree(indices); return 0; }
        static const int PASS_START[4] = {0, 4, 2, 1};
        static const int PASS_STEP[4]  = {8, 8, 4, 2};
        int src_row = 0;
        for (int pass = 0; pass < 4; pass++) {
            for (int row = PASS_START[pass]; row < ih; row += PASS_STEP[pass]) {
                for (int col = 0; col < iw; col++)
                    ordered[row * iw + col] = indices[src_row * iw + col];
                src_row++;
            }
        }
        kfree(indices);
        indices = ordered;
    }

    *pp = p;
    return indices;
}

/* Walks the whole GIF, compositing each frame onto a logical-screen
 * canvas and snapshotting up to max_frames of them. Anything that goes
 * wrong mid-stream (truncation, junk block, allocation failure) keeps
 * whatever frames were already banked; only a total failure returns -1. */
static int gif_decode_frames(const uint8_t *data, int len, AnimBitmap *out, int max_frames)
{
    if (len < 13) return -1;
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') return -1;

    int width = data[6] | (data[7] << 8);
    int height = data[8] | (data[9] << 8);
    int flags = data[10];
    int has_gct = (flags & 0x80) != 0;
    int gct_size = 2 << (flags & 7);
    if (width <= 0 || height <= 0 || (long)width * height > MAX_PIXELS) return -1;

    /* A big animation multiplies its canvas by the frame count; past 4MB
     * per frame quietly fall back to first-frame-only so one hostile GIF
     * can't eat 16x the memory. */
    if ((long)width * height * 4 > 4 * 1024 * 1024) max_frames = 1;

    uint8_t gct[256][3];
    for (int i = 0; i < 256; i++) { gct[i][0] = gct[i][1] = gct[i][2] = 0; }
    int p = 13;
    if (has_gct) {
        for (int i = 0; i < gct_size && p + 3 <= len; i++) {
            gct[i][0] = data[p]; gct[i][1] = data[p+1]; gct[i][2] = data[p+2];
            p += 3;
        }
    }

    uint32_t *canvas = (uint32_t *)kmalloc((uint32_t)(width * height * 4));
    if (!canvas) return -1;
    for (int i = 0; i < width * height; i++) canvas[i] = 0;

    int transparent_idx = -1;
    int disposal = 0;
    int delay_units = 0;
    out->count = 0;

    while (p < len && out->count < max_frames) {
        uint8_t block = data[p++];
        if (block == 0x3B) break;

        if (block == 0x21) {
            uint8_t label = (p < len) ? data[p++] : 0;
            if (label == 0xF9) {
                int sz = (p < len) ? data[p++] : 0;
                if (sz >= 4 && p + 4 <= len) {
                    if (data[p] & 1) transparent_idx = data[p + 3];
                    disposal = (data[p] >> 2) & 7;
                    delay_units = data[p + 1] | (data[p + 2] << 8);
                }
                p += sz;
            }
            while (p < len) { int sz = data[p++]; if (!sz) break; p += sz; }
            continue;
        }

        if (block != 0x2C) break; /* some block type we don't understand - give up */

        if (p + 9 > len) break;
        int left = data[p]   | (data[p+1] << 8);
        int top  = data[p+2] | (data[p+3] << 8);
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
        if (iw <= 0 || ih <= 0 || (long)iw * ih > MAX_PIXELS) break;

        uint8_t *indices = gif_lzw_frame(data, len, &p, iw, ih, interlaced);
        if (!indices) break;

        /* Composite the sub-image onto the canvas, clipped to the logical
         * screen; transparent-index pixels leave the canvas untouched. */
        for (int row = 0; row < ih && top + row < height; row++) {
            for (int col = 0; col < iw && left + col < width; col++) {
                int ci = indices[row * iw + col];
                if (ci == transparent_idx) continue;
                canvas[(top + row) * width + (left + col)] =
                    pack_argb(255, active_pal[ci][0], active_pal[ci][1], active_pal[ci][2]);
            }
        }
        kfree(indices);

        Bitmap *fr = &out->frames[out->count];
        if (alloc_bitmap(fr, width, height) != 0) break;
        for (int i = 0; i < width * height; i++) fr->pixels[i] = canvas[i];
        int delay_ms = delay_units * 10;
        if (delay_ms < 20) delay_ms = 100;
        out->delays_ms[out->count] = delay_ms;
        out->count++;

        /* Disposal 2 = restore to background; 3 wants "restore previous"
         * but we keep no canvas history, so treat it the same - clearing
         * the sub-rect back to transparent is the closest we get. */
        if (disposal == 2 || disposal == 3) {
            for (int row = 0; row < ih && top + row < height; row++)
                for (int col = 0; col < iw && left + col < width; col++)
                    canvas[(top + row) * width + (left + col)] = 0;
        }

        /* a graphic control extension only governs the one image after it */
        transparent_idx = -1;
        disposal = 0;
        delay_units = 0;
    }

    kfree(canvas);
    if (out->count < 1) return -1;
    if (out->count == 1) out->delays_ms[0] = 0; /* a still image has no delay */
    return 0;
}

static int gif_decode(const uint8_t *data, int len, Bitmap *out)
{
    AnimBitmap anim;
    if (gif_decode_frames(data, len, &anim, 1) != 0) return -1;
    *out = anim.frames[0];
    return 0;
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

int img_decode_anim(const uint8_t *data, int len, AnimBitmap *out)
{
    if (!data || len < 4 || !out) return -1;
    out->count = 0;

    if (data[0]=='G' && data[1]=='I' && data[2]=='F')
        return gif_decode_frames(data, len, out, ANIM_MAX_FRAMES);

    if (img_decode(data, len, &out->frames[0]) != 0) return -1;
    out->delays_ms[0] = 0;
    out->count = 1;
    return 0;
}

void anim_free(AnimBitmap *a)
{
    if (!a) return;
    for (int i = 0; i < a->count; i++) bmp_free(&a->frames[i]);
    a->count = 0;
}
