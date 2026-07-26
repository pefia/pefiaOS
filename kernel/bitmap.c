#include "bitmap.h"
#include "heap.h"

/* BMP fields are little-endian regardless of host, so we read them by hand
 * instead of trusting struct layout/alignment across compilers. */
static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void clear(Bitmap *b)
{
    b->width = 0;
    b->height = 0;
    b->stride = 0;
    b->has_alpha = 0;
    b->pixels = 0;
}

/* Decodes uncompressed BI_RGB BMPs at 24 or 32 bpp only - which covers
 * everything we bundle. Anything else (RLE, paletted, 16-bit) is rejected
 * rather than half-supported. Returns 0 on success, negative error codes
 * otherwise (see the checks below for what each one means). */
int bmp_decode(const uint8_t *data, int len, Bitmap *out)
{
    if (!out) return -1;
    clear(out);
    if (!data || len < 54) return -1;

    if (data[0] != 'B' || data[1] != 'M') return -2;

    uint32_t pixel_offset = read_le32(data + 10);
    uint32_t dib_size = read_le32(data + 14);
    if (dib_size < 40) return -3;

    int width = (int)read_le32(data + 18);
    int height = (int)read_le32(data + 22);
    uint16_t planes = read_le16(data + 26);
    uint16_t bpp = read_le16(data + 28);
    uint32_t compression = read_le32(data + 30);

    if (planes != 1) return -4;
    if (!(bpp == 24 || bpp == 32)) return -5;
    if (compression != 0) return -6;

    /* Negative height means the rows are stored top-down; positive means
     * bottom-up, which is BMP's usual (annoying) convention. */
    int top_down = 0;
    if (height < 0) {
        top_down = 1;
        height = -height;
    }
    if (width <= 0 || height <= 0) return -7;

    int row_stride = ((width * bpp + 31) / 32) * 4;
    if ((int)pixel_offset + row_stride * height > len) return -8;

    uint32_t *pixels = (uint32_t *)kmalloc((uint32_t)(width * height * 4));
    if (!pixels) return -9;

    for (int y = 0; y < height; y++) {
        int src_row = top_down ? y : (height - 1 - y);
        const uint8_t *row = data + pixel_offset + src_row * row_stride;
        for (int x = 0; x < width; x++) {
            uint8_t b = row[x * (bpp / 8) + 0];
            uint8_t g = row[x * (bpp / 8) + 1];
            uint8_t r = row[x * (bpp / 8) + 2];
            uint8_t a = (bpp == 32) ? row[x * 4 + 3] : 255;
            pixels[y * width + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    out->width = width;
    out->height = height;
    out->stride = width;
    out->has_alpha = (bpp == 32);
    out->pixels = pixels;
    return 0;
}

void bmp_free(Bitmap *bmp)
{
    if (!bmp) return;
    if (bmp->pixels) kfree(bmp->pixels);
    clear(bmp);
}

/* Clips to (max_w, max_h) rather than the destination surface itself -
 * callers are expected to pass in whatever's left of the target area. */
void bmp_blit(const Bitmap *bmp, int dst_x, int dst_y, int max_w, int max_h)
{
    if (!bmp || !bmp->pixels) return;
    int w = bmp->width;
    int h = bmp->height;
    if (w > max_w) w = max_w;
    if (h > max_h) h = max_h;
    if (w <= 0 || h <= 0) return;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t px = bmp->pixels[y * bmp->stride + x];
            uint8_t a = (uint8_t)(px >> 24);
            if (a < 16) continue;
            uint8_t r = (uint8_t)(px >> 16);
            uint8_t g = (uint8_t)(px >> 8);
            uint8_t b = (uint8_t)(px);
            fb_put_pixel(dst_x + x, dst_y + y, fb_rgb(r, g, b));
        }
    }
}
