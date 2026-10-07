/*
 * png2ico.c - build a multi-resolution Windows .ico file from one PNG.
 *
 * A Windows icon file holds several copies of the same picture at
 * different pixel sizes (16x16, 32x32, ... 256x256). Windows picks the
 * copy that best matches the size it needs to draw. This program reads a
 * single source PNG, produces a scaled copy for each requested size, and
 * packs them all into one .ico file.
 *
 * Dependencies: libpng 1.6.29 or later (libpng license) and zlib (zlib
 * license), which libpng itself requires. 1.6.29 is the first release with
 * png_image_write_to_memory(), used below.
 *
 * Build:
 *     cc -O2 -o png2ico png2ico.c $(pkg-config --cflags --libs libpng) -lm
 *
 * Usage:
 *     png2ico out.ico in.png [size ...]
 *
 *     Each size is a square edge length in pixels, from 1 to 256.
 *     If no sizes are given, the default is 16 24 32 48 64 128 256.
 *
 * How each size is produced:
 *   - The source keeps its aspect ratio. A non-square source is scaled to
 *     fit inside the square and centered, with transparent padding on the
 *     short sides (letterboxing).
 *   - Shrinking uses an area-averaging ("box") filter: each output pixel is
 *     the average of every source pixel it covers, weighted by how much of
 *     each source pixel falls inside it. This is the standard way to shrink
 *     an image without aliasing.
 *   - Enlarging (only when the source is smaller than a requested size)
 *     uses bilinear interpolation: each output pixel blends the four
 *     nearest source pixels. Box filtering would just repeat pixels and
 *     look blocky.
 *   - Both filters average colors weighted by alpha ("premultiplied
 *     alpha"). Without that, the invisible color stored in fully
 *     transparent pixels, often black, would bleed into the visible edge
 *     of the icon and leave a dark fringe.
 *
 * How the icons are stored:
 *   Sizes below 256 are stored as uncompressed bitmaps, the original icon
 *   format, which every version of Windows can read, including XP.
 *   The 256 size is stored as a complete PNG file instead. An
 *   uncompressed 256x256 bitmap would add about 270 KB, and only Windows
 *   Vista and later use 256-pixel icons; those versions also read PNG
 *   entries. Windows XP does not use the 256-pixel entry.
 */
#include <png.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print an error message and exit. Used for every unrecoverable error. */
static void die(const char *msg)
{
    fprintf(stderr, "png2ico: %s\n", msg);
    exit(1);
}

/*
 * Convert a computed color or alpha value to a byte: round it to the
 * nearest whole number and limit it to the range 0 to 255.
 */
static unsigned char clamp8(double v)
{
    v += 0.5;
    return v <= 0 ? 0 : v >= 255 ? 255 : (unsigned char)v;
}

/*
 * Write one finished output pixel to q (its 4 bytes: red, green, blue,
 * alpha), from the running totals a filter collected while visiting the
 * source pixels that contribute to it.
 *
 * For every contributing source pixel, the filter added:
 *   - to r, g and b: that pixel's red, green and blue values, each
 *     multiplied by the pixel's alpha and by its filter weight;
 *   - to a: the pixel's alpha multiplied by its filter weight;
 *   - to wsum: its filter weight alone.
 * (A filter weight is how much that source pixel counts toward this output
 * pixel.)
 *
 * From those totals:
 *   - r / a, g / a and b / a are the average colors, where more opaque
 *     source pixels count for more and invisible ones count for nothing;
 *   - a / wsum is the average alpha.
 * If a is zero, every contributing pixel was fully transparent, so there
 * is no color to average; the pixel is written as transparent black.
 */
static void emit(unsigned char *q, double r, double g, double b, double a,
                 double wsum)
{
    if (a > 0) {
        q[0] = clamp8(r / a);
        q[1] = clamp8(g / a);
        q[2] = clamp8(b / a);
    } else {
        q[0] = q[1] = q[2] = 0;
    }
    q[3] = wsum > 0 ? clamp8(a / wsum) : 0;
}

/*
 * Shrink an image with an area-averaging (box) filter.
 *
 * src:  the source image, sw pixels wide and sh pixels tall. Each pixel is
 *       4 bytes (red, green, blue, alpha), and each row of pixels follows
 *       the previous row immediately in memory.
 * dst:  where to write the result, which will be dw pixels wide and dh
 *       pixels tall. dst may point into a larger image (the icon canvas),
 *       so the start of each output row is dstride pixels after the start
 *       of the previous one, where dstride is the larger image's width.
 *
 * Picture the output grid laid over the source. Each output pixel covers
 * a rectangle sx by sy source pixels in size, where sx = sw/dw and
 * sy = sh/dh. That rectangle usually does not line up with source pixel
 * boundaries, so source pixels along its edges are only partly inside it.
 * Each source pixel is weighted by the fraction of its area inside the
 * rectangle.
 */
static void scale_box(const unsigned char *src, int sw, int sh,
                      unsigned char *dst, int dstride, int dw, int dh)
{
    /*
     * How many source pixels fit across one output pixel (sx) and down one
     * output pixel (sy). For example, shrinking 512 pixels to 16 makes
     * each 32. These are fractional when the sizes don't divide evenly.
     */
    double sx = (double)sw / dw;
    double sy = (double)sh / dh;

    for (int oy = 0; oy < dh; oy++) {
        /*
         * Where this output row's top edge (y0) and bottom edge (y1) fall
         * in the source image, measured in source pixels. These can land
         * partway through a source row.
         */
        double y0 = oy * sy, y1 = y0 + sy;

        /*
         * The source rows that this output row touches, even partly: from
         * the row containing the top edge, iy0, up to but not including
         * row iy1. The limit keeps rounding from reaching past the bottom
         * of the source image.
         */
        int iy0 = (int)y0, iy1 = (int)ceil(y1);
        if (iy1 > sh)
            iy1 = sh;

        for (int ox = 0; ox < dw; ox++) {
            /*
             * The same for columns: this output pixel's left edge (x0) and
             * right edge (x1) in source pixels, and the range of source
             * columns it touches, ix0 up to but not including ix1.
             */
            double x0 = ox * sx, x1 = x0 + sx;
            int ix0 = (int)x0, ix1 = (int)ceil(x1);
            if (ix1 > sw)
                ix1 = sw;

            double r = 0, g = 0, b = 0, a = 0, wsum = 0;
            for (int y = iy0; y < iy1; y++) {
                /*
                 * How much of source row y's height lies between the
                 * output row's top and bottom edges: 1 if the row is fully
                 * inside, less for a row cut off at the top or bottom.
                 */
                double wy = fmin(y1, y + 1) - fmax(y0, y);
                const unsigned char *row = src + (size_t)y * sw * 4;

                for (int x = ix0; x < ix1; x++) {
                    /*
                     * This source pixel's weight: the fraction of its area
                     * that lies inside the output pixel, i.e. the covered
                     * fraction of its height times that of its width.
                     */
                    double w = wy * (fmin(x1, x + 1) - fmax(x0, x));
                    const unsigned char *p = row + (size_t)x * 4;

                    /*
                     * Add this pixel to the totals described in emit().
                     * Its colors are multiplied by its alpha as well as its
                     * weight, so transparent pixels add no color.
                     */
                    double pa = p[3] * w;
                    r += p[0] * pa;
                    g += p[1] * pa;
                    b += p[2] * pa;
                    a += pa;
                    wsum += w;
                }
            }
            emit(dst + ((size_t)oy * dstride + ox) * 4, r, g, b, a, wsum);
        }
    }
}

/*
 * Enlarge an image with bilinear interpolation.
 *
 * Arguments are the same as scale_box().
 *
 * Each output pixel's center is mapped back to a position in the source.
 * Pixel centers sit at +0.5, so output pixel ox maps to source position
 * (ox + 0.5) * sx - 0.5; this keeps the image centered rather than shifted
 * by half a pixel. That position generally falls between four source
 * pixels, and the output is a blend of those four, each weighted by how
 * close it is. Positions past the edge are clamped to the edge pixels.
 */
static void scale_bilinear(const unsigned char *src, int sw, int sh,
                           unsigned char *dst, int dstride, int dw, int dh)
{
    double sx = (double)sw / dw, sy = (double)sh / dh;

    for (int oy = 0; oy < dh; oy++) {
        /*
         * Where the center of this output row falls in the source image,
         * measured in source pixels (see the explanation above). Near the
         * top and bottom edges this can fall outside the source image, so
         * it is limited to the first and last source rows.
         */
        double fy = (oy + 0.5) * sy - 0.5;
        if (fy < 0)
            fy = 0;
        if (fy > sh - 1)
            fy = sh - 1;

        /*
         * y0 and y1 are the source rows just above and just below that
         * position. On the last row there is no row below, so both are the
         * last row. ty says how far the position is from y0 toward y1:
         * 0 means exactly on y0, 0.5 means halfway between them.
         */
        int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        double ty = fy - y0;

        for (int ox = 0; ox < dw; ox++) {
            /*
             * The same for columns: where this output pixel's center falls
             * horizontally, the source columns just left (x0) and right
             * (x1) of it, and how far it is from x0 toward x1 (tx).
             */
            double fx = (ox + 0.5) * sx - 0.5;
            if (fx < 0)
                fx = 0;
            if (fx > sw - 1)
                fx = sw - 1;
            int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            double tx = fx - x0;

            /*
             * Blend the four source pixels around that position. Each
             * pixel's weight is larger the closer the position is to it,
             * and the four weights add up to 1, so the totals are already
             * averages (hence the 1.0 passed to emit()).
             */
            const int xs[2] = { x0, x1 }, ys[2] = { y0, y1 };
            const double wx[2] = { 1 - tx, tx }, wy[2] = { 1 - ty, ty };
            double r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const unsigned char *p =
                        src + ((size_t)ys[j] * sw + xs[i]) * 4;
                    double pa = p[3] * wx[i] * wy[j];
                    r += p[0] * pa;
                    g += p[1] * pa;
                    b += p[2] * pa;
                    a += pa;
                }
            emit(dst + ((size_t)oy * dstride + ox) * 4, r, g, b, a, 1.0);
        }
    }
}

/*
 * Produce one icon image: an s x s RGBA buffer containing the source
 * scaled to fit, centered, with transparent padding around it. The caller
 * frees the result.
 */
static unsigned char *render(const unsigned char *src, int sw, int sh, int s)
{
    /* calloc zero-fills, so the padding starts out fully transparent. */
    unsigned char *canvas = calloc((size_t)s * s, 4);
    if (!canvas)
        die("out of memory");

    /*
     * Work out how big the picture should be inside the icon, without
     * distorting it. The icon is s pixels on each side. The source's
     * longer side is scaled to exactly s pixels so it fills the icon edge
     * to edge, and the shorter side is scaled by the same factor. For
     * example, a 600x300 source in a 256-pixel icon becomes 256x128. The
     * resulting width and height are dw and dh, never less than 1 pixel.
     */
    int m = sw > sh ? sw : sh;
    int dw = (int)((double)s * sw / m + 0.5);
    int dh = (int)((double)s * sh / m + 0.5);
    if (dw < 1)
        dw = 1;
    if (dh < 1)
        dh = 1;

    /*
     * Center the picture in the icon by leaving equal transparent margins
     * on opposite sides. In the example above, the margins are 64 pixels
     * at the top and 64 at the bottom. dst is the location in the canvas
     * where the picture's top-left pixel goes.
     */
    unsigned char *dst = canvas + ((size_t)((s - dh) / 2) * s + (s - dw) / 2) * 4;

    /*
     * Pick the filter: bilinear if the picture is being made larger than
     * the source, the box filter if it is being made smaller (or kept the
     * same size). Width and height are scaled by the same factor, so they
     * always grow or shrink together.
     */
    if (dw > sw || dh > sh)
        scale_bilinear(src, sw, sh, dst, s, dw, dh);
    else
        scale_box(src, sw, sh, dst, s, dw, dh);
    return canvas;
}

/*
 * Compress a w x h RGBA image into a PNG file held in memory. Returns a
 * malloc'd buffer and stores its length in *len. The caller frees it.
 *
 * libpng's write-to-memory call is made twice: the first call, with a
 * NULL buffer, only reports how many bytes the PNG needs; the second
 * call writes it.
 */
static unsigned char *encode_png(const unsigned char *rgba, int w, int h,
                                 png_alloc_size_t *len)
{
    png_image img;
    memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = (png_uint_32)w;
    img.height = (png_uint_32)h;
    img.format = PNG_FORMAT_RGBA;

    if (!png_image_write_to_memory(&img, NULL, len, 0, rgba, 0, NULL))
        die(img.message);
    unsigned char *buf = malloc(*len);
    if (!buf)
        die("out of memory");
    if (!png_image_write_to_memory(&img, buf, len, 0, rgba, 0, NULL))
        die(img.message);
    return buf;
}

/*
 * The .ico format stores all multi-byte numbers little-endian (least
 * significant byte first). These write a 16- or 32-bit value that way,
 * regardless of the byte order of the machine running this program.
 */
static void put16(unsigned char *p, unsigned v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put32(unsigned char *p, unsigned long v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, (v >> 16) & 0xFFFF);
}

/*
 * Store a w x h RGBA image as an icon bitmap, the uncompressed format
 * that every version of Windows can read. Returns a malloc'd buffer and
 * stores its length in *len. The caller frees it.
 *
 * An icon bitmap has three parts, one after another:
 *   1. A 40-byte header (BITMAPINFOHEADER) giving the image dimensions and
 *      pixel format.
 *   2. The color pixels, 4 bytes each in the order blue, green, red,
 *      alpha (the reverse of PNG's red, green, blue). Rows are stored
 *      from the bottom row of the picture up to the top row.
 *   3. A transparency mask: 1 bit per pixel, a 1 marking a transparent
 *      pixel, also bottom row first. Windows XP and later use the alpha
 *      bytes instead, but the mask must still be present, and older
 *      versions of Windows that don't understand alpha rely on it. Each
 *      mask row is padded with unused bits to a multiple of 4 bytes.
 */
static unsigned char *encode_bmp(const unsigned char *rgba, int w, int h,
                                 png_alloc_size_t *len)
{
    /* Bytes per mask row: w bits, rounded up to a multiple of 32 bits. */
    size_t mask_row = ((size_t)w + 31) / 32 * 4;
    size_t color_size = (size_t)w * h * 4;
    size_t mask_size = mask_row * h;
    size_t total = 40 + color_size + mask_size;

    /* calloc zero-fills, so header fields not set below are 0, and the
     * mask starts with every pixel marked opaque. */
    unsigned char *buf = calloc(total, 1);
    if (!buf)
        die("out of memory");

    /*
     * Header. The height is written as twice the image height: in icon
     * files it counts the color pixels and the mask together. The fields
     * left at 0 are compression (0 = none), print resolution, and palette
     * sizes, none of which apply here.
     */
    put32(buf, 40);                                /* header size */
    put32(buf + 4, (unsigned long)w);
    put32(buf + 8, 2UL * (unsigned long)h);
    put16(buf + 12, 1);                            /* color planes */
    put16(buf + 14, 32);                           /* bits per pixel */
    put32(buf + 20, (unsigned long)(color_size + mask_size));

    unsigned char *color = buf + 40;
    unsigned char *mask = color + color_size;

    /* Output row y holds picture row h - 1 - y, so the bottom row is first. */
    for (int y = 0; y < h; y++) {
        const unsigned char *src = rgba + (size_t)(h - 1 - y) * w * 4;
        unsigned char *dst = color + (size_t)y * w * 4;
        unsigned char *mrow = mask + (size_t)y * mask_row;

        for (int x = 0; x < w; x++, src += 4, dst += 4) {
            dst[0] = src[2];    /* blue */
            dst[1] = src[1];    /* green */
            dst[2] = src[0];    /* red */
            dst[3] = src[3];    /* alpha */

            /* Mark fully transparent pixels in the mask. Bits are filled
             * from the most significant bit of each byte down. */
            if (src[3] == 0)
                mrow[x / 8] |= (unsigned char)(0x80 >> (x % 8));
        }
    }

    *len = total;
    return buf;
}

int main(int argc, char **argv)
{
    static const int defaults[] = { 16, 24, 32, 48, 64, 128, 256 };

    if (argc < 3) {
        fprintf(stderr, "usage: %s out.ico in.png [size ...]\n", argv[0]);
        return 2;
    }

    /*
     * Collect the requested sizes, or use the defaults. The .ico header
     * stores the image count in 16 bits, hence the 65535 limit. Sizes are
     * capped at 256 because each directory entry stores width and height
     * in a single byte, with 0 meaning 256.
     */
    int n = argc > 3 ? argc - 3 : (int)(sizeof defaults / sizeof *defaults);
    if (n > 0xFFFF)
        die("too many sizes");
    int *sizes = malloc(sizeof *sizes * (size_t)n);
    if (!sizes)
        die("out of memory");
    for (int i = 0; i < n; i++) {
        if (argc > 3) {
            char *end;
            long v = strtol(argv[3 + i], &end, 10);
            if (*end || v < 1 || v > 256)
                die("sizes must be integers in 1..256");
            sizes[i] = (int)v;
        } else {
            sizes[i] = defaults[i];
        }
    }

    /*
     * Read the source PNG using libpng's "simplified" API, which handles
     * file I/O and pixel-format conversion internally.
     *
     * png_image_begin_read_from_file() opens the file with fopen() and
     * reads the header, which fills in the image width and height. We
     * then ask for 8-bit RGBA output, whatever the file's actual format
     * (palette, grayscale, 16-bit, etc.), and png_image_finish_read()
     * decodes the pixels into our buffer and closes the file.
     *
     * If either call fails, libpng closes the file and frees its own
     * state before returning, so the error paths need no cleanup. The one
     * exception is our own malloc failing between the two calls, where we
     * must call png_image_free() ourselves to close the file.
     */
    png_image src;
    memset(&src, 0, sizeof src);
    src.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&src, argv[2]))
        die(src.message);
    src.format = PNG_FORMAT_RGBA;
    unsigned char *pixels = malloc(PNG_IMAGE_SIZE(src));
    if (!pixels) {
        png_image_free(&src);
        die("out of memory");
    }
    if (!png_image_finish_read(&src, NULL, pixels, 0, NULL))
        die(src.message);

    /*
     * Build every icon image up front. Each one is scaled, then stored as
     * a bitmap, or as a PNG for the 256 size (see the top of this file).
     * The stored lengths must be known before anything is written,
     * because the file's directory records where each image begins.
     */
    unsigned char **blobs = malloc(sizeof *blobs * (size_t)n);
    png_alloc_size_t *lens = malloc(sizeof *lens * (size_t)n);
    if (!blobs || !lens)
        die("out of memory");
    for (int i = 0; i < n; i++) {
        int s = sizes[i];
        unsigned char *px = render(pixels, (int)src.width,
                                   (int)src.height, s);
        blobs[i] = s == 256 ? encode_png(px, s, s, &lens[i])
                            : encode_bmp(px, s, s, &lens[i]);
        free(px);
    }
    free(pixels);

    /*
     * Write the .ico file. Its layout is:
     *
     *   1. A 6-byte header ("ICONDIR").
     *   2. One 16-byte directory entry ("ICONDIRENTRY") per image, giving
     *      that image's size, its length in bytes, and its byte offset from
     *      the start of the file.
     *   3. The image data itself: each image's bitmap or PNG, back to back.
     */
    FILE *f = fopen(argv[1], "wb");
    if (!f)
        die("cannot open output file");

    /* Header: reserved (must be 0), file type (1 = icon, 2 = cursor), and
     * number of images. */
    unsigned char hdr[6];
    put16(hdr, 0);
    put16(hdr + 2, 1);
    put16(hdr + 4, (unsigned)n);
    fwrite(hdr, 1, sizeof hdr, f);

    /* The first image starts right after the header and all directory
     * entries. Each later image starts where the previous one ended. */
    unsigned long off = 6 + 16UL * (unsigned long)n;
    for (int i = 0; i < n; i++) {
        unsigned char e[16];

        /* Width and height in one byte each. 256 doesn't fit, so the
         * format uses 0 to mean 256; masking with 0xFF produces that. */
        e[0] = (unsigned char)(sizes[i] & 0xFF);
        e[1] = (unsigned char)(sizes[i] & 0xFF);

        /* Number of palette colors: 0, since these are not palette images. */
        e[2] = 0;

        /* Reserved; must be 0. */
        e[3] = 0;

        /*
         * Color planes: always 1. Bits per pixel: 32, meaning 8 bits each
         * for red, green, blue and alpha.
         */
        put16(e + 4, 1);
        put16(e + 6, 32);

        /* Length of this image's data, then where in the file it starts. */
        put32(e + 8, (unsigned long)lens[i]);
        put32(e + 12, off);

        fwrite(e, 1, sizeof e, f);
        off += (unsigned long)lens[i];
    }

    /* The image data, in the same order as the directory entries. */
    for (int i = 0; i < n; i++) {
        fwrite(blobs[i], 1, lens[i], f);
        free(blobs[i]);
    }
    free(blobs);
    free(lens);
    free(sizes);

    /*
     * fwrite() is buffered, so a write error may only show up later.
     * ferror() reports any error from the writes so far, and fclose()
     * flushes the remaining buffered data and reports any error from that.
     * Both are checked; the bitwise | makes sure fclose() is always
     * called, even if ferror() already reported a failure.
     */
    if (ferror(f) | fclose(f))
        die("write failed");
    return 0;
}