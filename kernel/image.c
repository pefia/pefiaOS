#include "image.h"
#include "inflate.h"
#include "heap.h"

/* ========================================================================== */
/* helpers                                                                    */
/* ========================================================================== */

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int iabs(int v) { return v < 0 ? -v : v; }

#define MAX_PIXELS (3000 * 3000)

static int alloc_image(Bitmap *out, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    if ((long)w * h > MAX_PIXELS) return -1;
    out->pixels = (uint32_t *)kmalloc((uint32_t)(w * h * 4));
    if (!out->pixels) return -1;
    out->width = w; out->height = h; out->stride = w; out->has_alpha = 1;
    return 0;
}

static uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{ return ((uint32_t)a<<24)|((uint32_t)r<<16)|((uint32_t)g<<8)|b; }

/* ========================================================================== */
/* PNG                                                                        */
/* ========================================================================== */

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static int png_decode(const uint8_t *data, int len, Bitmap *out)
{
    static const uint8_t SIG[8] = {137,80,78,71,13,10,26,10};
    if (len < 33) return -1;
    for (int i = 0; i < 8; i++) if (data[i] != SIG[i]) return -1;

    int width = 0, height = 0, bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t palette[256][3]; int pal_n = 0;
    uint8_t trns[256]; int trns_n = 0;
    uint16_t trns_gray = 0xFFFF, trns_r = 0xFFFF, trns_g = 0xFFFF, trns_b = 0xFFFF;
    int have_trns_color = 0;

    /* gather IDAT into one buffer */
    int idat_cap = len;                       /* upper bound */
    uint8_t *idat = (uint8_t *)kmalloc(idat_cap);
    if (!idat) return -1;
    int idat_len = 0;

    int p = 8;
    int ok_ihdr = 0;
    while (p + 8 <= len) {
        uint32_t clen = be32(data + p);
        const uint8_t *type = data + p + 4;
        const uint8_t *cdat = data + p + 8;
        if (p + 12 + (int)clen > len) break;

        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            width = (int)be32(cdat); height = (int)be32(cdat + 4);
            bitdepth = cdat[8]; colortype = cdat[9]; interlace = cdat[12];
            ok_ihdr = 1;
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            pal_n = clen / 3; if (pal_n > 256) pal_n = 256;
            for (int i = 0; i < pal_n; i++) { palette[i][0]=cdat[i*3]; palette[i][1]=cdat[i*3+1]; palette[i][2]=cdat[i*3+2]; }
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            if (colortype == 3) { trns_n = clen > 256 ? 256 : clen; for (int i=0;i<trns_n;i++) trns[i]=cdat[i]; }
            else if (colortype == 0 && clen >= 2) { trns_gray = (cdat[0]<<8)|cdat[1]; have_trns_color = 1; }
            else if (colortype == 2 && clen >= 6) { trns_r=(cdat[0]<<8)|cdat[1]; trns_g=(cdat[2]<<8)|cdat[3]; trns_b=(cdat[4]<<8)|cdat[5]; have_trns_color = 1; }
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            if (idat_len + (int)clen <= idat_cap) { for (uint32_t i=0;i<clen;i++) idat[idat_len++]=cdat[i]; }
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            break;
        }
        p += 12 + clen;                        /* length + type + data + crc */
    }

    if (!ok_ihdr || interlace != 0 || width <= 0 || height <= 0) { kfree(idat); return -1; }
    if (!(bitdepth==1||bitdepth==2||bitdepth==4||bitdepth==8||bitdepth==16)) { kfree(idat); return -1; }
    if ((long)width * height > MAX_PIXELS) { kfree(idat); return -1; }

    int channels = (colortype==2)?3:(colortype==6)?4:(colortype==4)?2:1;
    int bppByte = (bitdepth * channels + 7) / 8; if (bppByte < 1) bppByte = 1;
    int stride = (width * channels * bitdepth + 7) / 8;
    int rawsize = height * (stride + 1);

    uint8_t *raw = (uint8_t *)kmalloc(rawsize);
    if (!raw) { kfree(idat); return -1; }
    int got = zlib_inflate(idat, idat_len, raw, rawsize);
    kfree(idat);
    if (got < rawsize) { kfree(raw); return -1; }

    /* unfilter, in place: produce contiguous scanlines (drop filter byte) */
    uint8_t *lines = (uint8_t *)kmalloc(height * stride);
    if (!lines) { kfree(raw); return -1; }
    for (int y = 0; y < height; y++) {
        uint8_t filt = raw[y * (stride + 1)];
        const uint8_t *src = raw + y * (stride + 1) + 1;
        uint8_t *cur = lines + y * stride;
        uint8_t *prev = (y > 0) ? lines + (y - 1) * stride : 0;
        for (int x = 0; x < stride; x++) {
            int a = (x >= bppByte) ? cur[x - bppByte] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bppByte) ? prev[x - bppByte] : 0;
            int v = src[x];
            switch (filt) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            cur[x] = (uint8_t)v;
        }
    }
    kfree(raw);

    if (alloc_image(out, width, height) != 0) { kfree(lines); return -1; }

    int maxv = (1 << bitdepth) - 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = lines + y * stride;
        for (int x = 0; x < width; x++) {
            uint8_t r=0,g=0,b=0,a=255;
            if (bitdepth == 8) {
                const uint8_t *px = row + x * channels;
                if (colortype == 0) { r=g=b=px[0]; if (have_trns_color && px[0]==(uint8_t)trns_gray) a=0; }
                else if (colortype == 2) { r=px[0]; g=px[1]; b=px[2]; if (have_trns_color && px[0]==trns_r&&px[1]==trns_g&&px[2]==trns_b) a=0; }
                else if (colortype == 3) { int idx=px[0]; if(idx<pal_n){r=palette[idx][0];g=palette[idx][1];b=palette[idx][2];} a=(idx<trns_n)?trns[idx]:255; }
                else if (colortype == 4) { r=g=b=px[0]; a=px[1]; }
                else { r=px[0]; g=px[1]; b=px[2]; a=px[3]; }
            } else if (bitdepth == 16) {
                const uint8_t *px = row + x * channels * 2;
                if (colortype == 0) { r=g=b=px[0]; }
                else if (colortype == 2) { r=px[0]; g=px[2]; b=px[4]; }
                else if (colortype == 4) { r=g=b=px[0]; a=px[2]; }
                else { r=px[0]; g=px[2]; b=px[4]; a=px[6]; }
            } else {
                /* sub-byte: grayscale or palette index */
                int bitpos = x * bitdepth;
                int byte = row[bitpos >> 3];
                int shift = 8 - bitdepth - (bitpos & 7);
                int val = (byte >> shift) & maxv;
                if (colortype == 3) { if(val<pal_n){r=palette[val][0];g=palette[val][1];b=palette[val][2];} a=(val<trns_n)?trns[val]:255; }
                else { int s = val * 255 / maxv; r=g=b=(uint8_t)s; if (have_trns_color && val==(int)trns_gray) a=0; }
            }
            out->pixels[y * width + x] = argb(a, r, g, b);
        }
    }
    kfree(lines);
    return 0;
}

/* ========================================================================== */
/* GIF (87a/89a, first frame, LZW)                                            */
/* ========================================================================== */

static int gif_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (len < 13) return -1;
    if (data[0]!='G'||data[1]!='I'||data[2]!='F') return -1;

    int width = data[6] | (data[7]<<8);
    int height = data[8] | (data[9]<<8);
    int flags = data[10];
    int gct = (flags & 0x80) != 0;
    int gct_size = 2 << (flags & 7);
    if (width<=0||height<=0||(long)width*height>MAX_PIXELS) return -1;

    uint8_t gpal[256][3];
    for (int i = 0; i < 256; i++) { gpal[i][0]=gpal[i][1]=gpal[i][2]=0; }
    int p = 13;
    if (gct) { for (int i=0;i<gct_size && p+3<=len;i++){gpal[i][0]=data[p];gpal[i][1]=data[p+1];gpal[i][2]=data[p+2];p+=3;} }

    int transparent = -1;

    /* walk blocks until image descriptor */
    while (p < len) {
        uint8_t b = data[p++];
        if (b == 0x3B) return -1;             /* trailer, no image */
        if (b == 0x21) {                       /* extension */
            uint8_t label = (p<len)?data[p++]:0;
            if (label == 0xF9) {               /* graphic control */
                int bsz = (p<len)?data[p++]:0;
                if (bsz >= 4 && p+4<=len) { if (data[p] & 1) transparent = data[p+3]; }
                p += bsz;
            }
            while (p < len) { int sz = data[p++]; if (!sz) break; p += sz; }
            continue;
        }
        if (b == 0x2C) {                       /* image descriptor */
            if (p + 9 > len) return -1;
            int iw = data[p+4] | (data[p+5]<<8);
            int ih = data[p+6] | (data[p+7]<<8);
            int lflags = data[p+8];
            int interlace = (lflags & 0x40) != 0;
            p += 9;
            uint8_t lpal[256][3]; for (int i=0;i<256;i++){lpal[i][0]=lpal[i][1]=lpal[i][2]=0;}
            int lct = (lflags & 0x80)!=0; int lct_size = 2 << (lflags & 7);
            uint8_t (*pal)[3] = gpal;
            if (lct) { for (int i=0;i<lct_size && p+3<=len;i++){lpal[i][0]=data[p];lpal[i][1]=data[p+1];lpal[i][2]=data[p+2];p+=3;} pal = lpal; }
            if (iw<=0||ih<=0||(long)iw*ih>MAX_PIXELS) return -1;

            int min_code = (p<len)?data[p++]:8;
            if (min_code < 2 || min_code > 8) return -1;

            /* collect sub-blocks of LZW data */
            int comp_cap = len; uint8_t *comp = (uint8_t*)kmalloc(comp_cap); if(!comp) return -1;
            int comp_len = 0;
            while (p < len) { int sz = data[p++]; if (!sz) break; for(int i=0;i<sz&&p<len;i++) comp[comp_len++]=data[p++]; }

            /* LZW decode */
            int clear = 1 << min_code;
            int eoi = clear + 1;
            int code_size = min_code + 1;
            int next = eoi + 1;
            static int pref[4096]; static uint8_t suf[4096]; static uint8_t first[4096];
            uint8_t *idx = (uint8_t*)kmalloc(iw * ih); if(!idx){kfree(comp);return -1;}
            int outn = 0;
            int bitpos = 0;
            int prev_code = -1;
            uint8_t stack[4096]; int sp = 0;

            for (int i = 0; i < clear; i++) { pref[i] = -1; suf[i] = (uint8_t)i; first[i] = (uint8_t)i; }

            while (bitpos + code_size <= comp_len * 8) {
                int code = 0;
                for (int i = 0; i < code_size; i++) {
                    int bytep = (bitpos + i) >> 3, bit = (bitpos + i) & 7;
                    if (bytep < comp_len && (comp[bytep] >> bit) & 1) code |= (1 << i);
                }
                bitpos += code_size;

                if (code == clear) { code_size = min_code + 1; next = eoi + 1; prev_code = -1; continue; }
                if (code == eoi) break;

                if (prev_code == -1) { if (code < next) { if (outn < iw*ih) idx[outn++] = first[code]; prev_code = code; } continue; }

                int in_code = code;
                sp = 0;
                if (code >= next) { stack[sp++] = first[prev_code]; code = prev_code; }
                while (code >= clear) { if(sp>=4096||code<0||code>=4096) break; stack[sp++] = suf[code]; code = pref[code]; }
                if (code >= 0 && code < 4096) stack[sp++] = first[code];

                int base = (code>=0&&code<4096)?first[code]:0;
                while (sp > 0 && outn < iw*ih) idx[outn++] = stack[--sp];

                if (next < 4096) {
                    pref[next] = prev_code; suf[next] = (uint8_t)base; first[next] = first[prev_code];
                    next++;
                    if (next == (1 << code_size) && code_size < 12) code_size++;
                }
                prev_code = in_code;
            }
            kfree(comp);

            if (alloc_image(out, iw, ih) != 0) { kfree(idx); return -1; }
            /* idx holds samples in storage order; de-interlace into image rows. */
            static const int IL_START[4] = {0, 4, 2, 1};
            static const int IL_STEP[4]  = {8, 8, 4, 2};
            int srow = 0;
            for (int pass = 0; pass < (interlace ? 4 : 1); pass++) {
                int start = interlace ? IL_START[pass] : 0;
                int step  = interlace ? IL_STEP[pass]  : 1;
                for (int r = start; r < ih; r += step) {
                    for (int c = 0; c < iw; c++) {
                        int ci = idx[srow * iw + c];
                        uint8_t rr=pal[ci][0], gg=pal[ci][1], bb=pal[ci][2];
                        uint8_t a = (ci == transparent) ? 0 : 255;
                        out->pixels[r * iw + c] = argb(a, rr, gg, bb);
                    }
                    srow++;
                }
            }
            kfree(idx);
            return 0;
        }
        /* unknown block: bail */
        return -1;
    }
    return -1;
}

/* ========================================================================== */
/* dispatcher                                                                 */
/* ========================================================================== */

int jpeg_decode(const uint8_t *data, int len, Bitmap *out);   /* in jpeg.c */

int img_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (!data || len < 4 || !out) return -1;
    out->pixels = 0; out->width = out->height = 0;

    if (data[0]==137 && data[1]=='P' && data[2]=='N' && data[3]=='G') return png_decode(data, len, out);
    if (data[0]=='G' && data[1]=='I' && data[2]=='F') return gif_decode(data, len, out);
    if (data[0]==0xFF && data[1]==0xD8) return jpeg_decode(data, len, out);
    if (data[0]=='B' && data[1]=='M') return bmp_decode(data, len, out);
    return -1;
}
