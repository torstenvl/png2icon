/*
 * png2ico.c - build a multi-resolution Windows .ico file from PNG images.
 *
 * A Windows icon file holds several copies of the same picture at
 * different pixel sizes (16x16, 32x32, ... 256x256). Windows picks the
 * copy that best matches the size it needs to draw. This program reads one
 * or more source PNGs, produces a copy of the picture at each requested
 * size, and packs them all into one .ico file.
 *
 * Dependencies: libpng 1.6.29 or later (libpng license) and zlib (zlib
 * license), which libpng itself requires. 1.6.29 is the first release with
 * png_image_write_to_memory(), used below.
 *
 * Build, with libpng and zlib linked statically (macOS and Linux):
 *     L=$(pkg-config --variable=libdir libpng)
 *     cc -O2 -o png2ico png2ico.c $(pkg-config --cflags libpng) \
 *         "$L/libpng16.a" "$L/libz.a" -lm
 *
 *   The archives are named by path because Apple's linker always prefers a
 *   .dylib over a .a in the same directory. This assumes zlib's archive is
 *   in the same directory as libpng's.
 *
 * Usage:
 *     png2ico -o OUTPUT [-favicon] INPUT...
 *
 *     -o OUTPUT    The .ico file to write.
 *     -favicon     Include only sizes 16, 32 and 48, for a website's
 *                  favicon.ico.
 *     INPUT        A source PNG file. At least one is required.
 *
 *     Options and inputs may appear in any order. Without -favicon, every
 *     size in standard_sizes[] below up to 256 is included: 16, 24, 32, 48,
 *     64, 128 and 256.
 *
 * Which input is used for each size:
 *   - An input can be assigned to one size in either of two ways:
 *       * by a size prefix: "16:small.png" is assigned to size 16;
 *       * by its name: if the file name, without its extension, ends in a
 *         possible size, it is assigned to that size. "icon16.png",
 *         "icon_16.png" and "icon_16x16.png" are all assigned to size 16.
 *     The size must be one of possible_sizes[] below. A prefix takes
 *     precedence over the name. Two inputs may not be assigned to the same
 *     size. If an assigned input is not exactly that size, it is scaled to
 *     fit and a warning is printed. An assigned size is added to the
 *     output if an .ico can hold it, with or without -favicon; this is the
 *     only way to produce a size that is not a standard size.
 *   - Every other size is made from the input whose picture is closest to
 *     that size without being smaller, so that it is scaled down as little
 *     as possible. If every input is smaller than that size, the largest
 *     input is scaled up. All inputs are candidates, including assigned
 *     ones. For example, given a 1024-pixel and a 65-pixel input, sizes 64
 *     and below are made from the 65-pixel input and larger sizes from the
 *     1024-pixel input.
 *   - An input's size, for this purpose, is its longer side.
 *
 * How each size is produced:
 *   - The source keeps its aspect ratio. A non-square source is scaled to
 *     fit inside the square and centered, with transparent padding on the
 *     short sides (letterboxing).
 *   - Shrinking uses an area-averaging ("box") filter: each output pixel is
 *     the average of every source pixel it covers, weighted by how much of
 *     each source pixel falls inside it.
 *   - Enlarging (only when no source is at least as large as the requested
 *     size) uses bilinear interpolation: each output pixel blends the four
 *     nearest source pixels.
 *   - Both filters average colors weighted by alpha ("premultiplied
 *     alpha"). Without that, the invisible color stored in fully
 *     transparent pixels, often black, would bleed into the visible edge
 *     of the icon and leave a dark fringe.
 *
 * How the icons are stored:
 *   Sizes below 256 are stored as uncompressed bitmaps, which Windows XP
 *   can read. The 256 size is stored as a complete PNG file, which
 *   requires Windows Vista or later.
 *
 * Exit status:
 *   0 on success. On failure, a message is printed and the program exits
 *   with the negative of the relevant errno value:
 *     -EINVAL  invalid command line, or an input that is not a valid PNG;
 *     -ENOMEM  out of memory, or an input too large to load;
 *     -errno   from a failed file operation, or -EIO if the operation
 *              failed without setting errno.
 *   The shell sees this value modulo 256; for example, -EINVAL (-22) is
 *   reported as 234.
 *
 * Output:
 *   The .ico is written to a temporary file in the same directory, named
 *   OUTPUT.PID.N, and then renamed to OUTPUT, so that OUTPUT is never left
 *   partly written. If anything fails, the temporary file is deleted and
 *   any existing OUTPUT is left unchanged.
 *
 * Structure:
 *   main() runs four steps in order, each in its own function:
 *     1. parse_command_line() fills in a struct options, which records only
 *        what the command line asked for.
 *     2. read_sources() fills in a struct image_set: it decodes the inputs,
 *        picks the input for each size, and scales it, producing a plain
 *        RGBA image for each size.
 *     3. write_ico() encodes those images and writes the .ico file.
 *     4. cleanup() frees the image set.
 *   The images from step 2 are not specific to the .ico format.
 *
 *   Errors are handled where they occur: the message is printed and the
 *   program exits immediately. Memory still allocated at that point is
 *   released by the operating system.
 */
#define _POSIX_C_SOURCE 200809L     /* open(), getpid(), fdopen() */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <png.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Every size, in pixels, that can be produced or assigned to an input,
 * smallest first: the sizes Windows 11 draws icons at, plus those of a
 * macOS .icns. Sizes above 256 are not used in .ico files but are
 * accepted as input sizes and kept for other icon formats.
 *
 * Many arrays below have one element per entry in this table, called a
 * "slot". Slot i holds information about size possible_sizes[i].
 */
static const int possible_sizes[] = {
    16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256, 512, 1024
};
enum { NUM_SIZES = sizeof possible_sizes / sizeof *possible_sizes };

/*
 * The sizes produced automatically, when -favicon is not given: Windows'
 * minimum set (16, 24, 32, 48, 256) plus a typical macOS .icns (16, 32,
 * 64, 128, 256, 512, 1024). An .ico gets those up to 256. Other possible
 * sizes are produced only when an input is assigned to them.
 */
static const int standard_sizes[] = { 16, 24, 32, 48, 64, 128, 256, 512, 1024 };

/* The sizes produced when -favicon is given. */
static const int favicon_sizes[] = { 16, 32, 48 };

/* The largest size an .ico file can hold. */
enum { ICO_MAX_SIZE = 256 };

/* One input file named on the command line. */
struct input {
    const char *path;       /* file name, without any "SIZE:" prefix */
    int slot;               /* slot it is assigned to, or -1 if none */
};

/* What the command line asked for. Filled in by parse_command_line(). */
struct options {
    const char *output;             /* the file to write */
    struct input *inputs;           /* every input, in command-line order */
    int num_inputs;
    bool target[NUM_SIZES];         /* true for each size to produce */
};

/* An RGBA image: 4 bytes per pixel (red, green, blue, alpha), rows packed
 * with no gaps between them. */
struct image {
    int width;
    int height;
    unsigned char *rgba;
};

/*
 * The pixels the program works with. Filled in by read_sources().
 *
 * All the icons share one allocation, icon_block. Each target slot's icon
 * is a square of possible_sizes[slot] pixels on a side, and icon[slot] points to
 * where it starts inside the block. Slots that are not targets have no
 * icon, and their icon[] entry is NULL.
 */
struct image_set {
    struct image *input;            /* decoded inputs, in the same order
                                       as options.inputs */
    int source[NUM_SIZES];          /* for each target slot, the index of
                                       the input its icon is made from */
    unsigned char *icon_block;
    unsigned char *icon[NUM_SIZES];
};

/*
 * Return the slot for a size in pixels, or -1 if it is not a possible
 * size.
 */
static int slot_for_size(long size)
{
    for (int i = 0; i < NUM_SIZES; i++)
        if (possible_sizes[i] == size)
            return i;
    return -1;
}

/*
 * Return the slot a file name assigns its file to, or -1 if none.
 *
 * Takes the last component of the path (after any '/' or '\'), removes the
 * extension (from the last '.'), and looks at the digits at the end of
 * what remains. If there are 1 to 4 of them and they form a possible size,
 * that is the size. "art/icon_16x16.png" gives 16; "logo2023.png" gives
 * none, since 2023 is not a possible size.
 */
static int slot_from_name(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;

    const char *end = strrchr(base, '.');
    if (!end)
        end = base + strlen(base);

    const char *digits = end;
    while (digits > base && digits[-1] >= '0' && digits[-1] <= '9')
        digits--;
    if (end - digits < 1 || end - digits > 4)
        return -1;

    int size = 0;
    for (const char *p = digits; p < end; p++)
        size = size * 10 + (*p - '0');
    return slot_for_size(size);
}

/* Print a short description of how to run the program. */
static void print_usage(void)
{
    fputs("Usage: png2ico -o OUTPUT [-favicon] INPUT...\n"
          "\n"
          "  -o OUTPUT  The .ico file to write.\n"
          "  -favicon   Include only sizes 16, 32 and 48, for a website favicon.\n"
          "  INPUT      A source PNG. Prefix it with a size (16:small.png), or end\n"
          "             its name with one (icon16.png), to use it for that size.\n",
          stderr);
}

/*
 * Step 1: read the command line into opt. See the top of this file for
 * the command-line syntax.
 *
 * The arguments are read by hand rather than with getopt(), because
 * getopt() only supports single-letter options and would read -favicon as
 * the seven options -f, -a, -v, -i, -c, -o and -n.
 */
static void parse_command_line(struct options *opt, int argc, char **argv)
{
    bool favicon = false;

    memset(opt, 0, sizeof *opt);

    /*
     * Make room for the inputs. There can be no more of them than there
     * are arguments, so allocate that many and count the real ones below.
     */
    opt->inputs = calloc((size_t)argc, sizeof *opt->inputs);
    if (!opt->inputs) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* Any argument beginning with '-' is an option. */
        if (arg[0] == '-') {
            if (strcmp(arg, "-o") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "png2ico: The -o option needs a file name.\n");
                    print_usage();
                    exit(-EINVAL);
                }
                if (opt->output) {
                    fprintf(stderr, "png2ico: The -o option was given more than once.\n");
                    exit(-EINVAL);
                }
                opt->output = argv[++i];
            } else if (strcmp(arg, "-favicon") == 0) {
                favicon = true;
            } else {
                fprintf(stderr, "png2ico: Unknown option \"%s\".\n", arg);
                print_usage();
                exit(-EINVAL);
            }
            continue;
        }

        /* Anything else is an input file. */
        struct input *in = &opt->inputs[opt->num_inputs++];

        /*
         * A size prefix is one or more digits followed by ':', as in
         * "16:small.png". Without one, the file name may still assign the
         * input to a size.
         */
        size_t ndigits = strspn(arg, "0123456789");
        if (ndigits > 0 && arg[ndigits] == ':') {
            in->slot = ndigits <= 4 ? slot_for_size(strtol(arg, NULL, 10)) : -1;
            in->path = arg + ndigits + 1;
            if (in->slot < 0) {
                fprintf(stderr, "png2ico: In \"%s\", %.*s is not a possible "
                        "icon size. Valid sizes are", arg, (int)ndigits, arg);
                for (int j = 0; j < NUM_SIZES; j++)
                    fprintf(stderr, "%s %d", j ? "," : "", possible_sizes[j]);
                fputs(".\n", stderr);
                exit(-EINVAL);
            }
            if (*in->path == '\0') {
                fprintf(stderr, "png2ico: \"%s\" has a size but no file name.\n",
                        arg);
                exit(-EINVAL);
            }
        } else {
            in->path = arg;
            in->slot = slot_from_name(arg);
        }

        /* No two inputs may be assigned to the same size. */
        if (in->slot >= 0) {
            for (int j = 0; j < opt->num_inputs - 1; j++) {
                if (opt->inputs[j].slot == in->slot) {
                    fprintf(stderr, "png2ico: \"%s\" and \"%s\" are both "
                            "assigned to size %d.\n", opt->inputs[j].path,
                            in->path, possible_sizes[in->slot]);
                    exit(-EINVAL);
                }
            }
        }
    }

    if (!opt->output) {
        fprintf(stderr, "png2ico: No output file was given. Use -o FILE.\n");
        print_usage();
        exit(-EINVAL);
    }
    if (opt->num_inputs == 0) {
        fprintf(stderr, "png2ico: No input files were given.\n");
        print_usage();
        exit(-EINVAL);
    }

    /*
     * Choose the sizes to produce: the favicon sizes or the standard sizes
     * an .ico can hold, plus every size an input is assigned to, if an .ico
     * can hold it.
     */
    if (favicon) {
        for (size_t i = 0; i < sizeof favicon_sizes / sizeof *favicon_sizes; i++)
            opt->target[slot_for_size(favicon_sizes[i])] = true;
    } else {
        for (size_t i = 0; i < sizeof standard_sizes / sizeof *standard_sizes; i++)
            if (standard_sizes[i] <= ICO_MAX_SIZE)
                opt->target[slot_for_size(standard_sizes[i])] = true;
    }
    for (int i = 0; i < opt->num_inputs; i++) {
        int slot = opt->inputs[i].slot;
        if (slot >= 0 && possible_sizes[slot] <= ICO_MAX_SIZE)
            opt->target[slot] = true;
    }
}

/*
 * Read a PNG file into img as 8-bit RGBA.
 *
 * Uses libpng's "simplified" API. png_image_begin_read_from_stdio() reads
 * the header, which gives the width and height. We then ask for 8-bit RGBA
 * output, whatever the file's actual format (palette, grayscale, 16-bit,
 * etc.), and png_image_finish_read() decodes the pixels into our buffer,
 * converting them as needed.
 */
static void read_png(struct image *img, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        int err = errno ? errno : EIO;
        fprintf(stderr, "png2ico: Cannot open \"%s\": %s.\n", path, strerror(err));
        exit(-err);
    }

    png_image png;
    memset(&png, 0, sizeof png);
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_stdio(&png, f)) {
        fprintf(stderr, "png2ico: Cannot read \"%s\": %s.\n", path, png.message);
        exit(-EINVAL);
    }

    /*
     * The buffer needs width * height * 4 bytes. Refuse an image for which
     * that product would not fit in a size_t (possible on 32-bit systems)
     * or whose dimensions would not fit in an int, rather than allocate a
     * buffer that is too small.
     */
    if (png.width > INT32_MAX || png.height > INT32_MAX ||
        png.width > SIZE_MAX / 4 / png.height) {
        fprintf(stderr, "png2ico: \"%s\" is too large to load (%lux%lu pixels).\n",
                path, (unsigned long)png.width, (unsigned long)png.height);
        exit(-ENOMEM);
    }

    png.format = PNG_FORMAT_RGBA;
    img->rgba = malloc((size_t)png.width * png.height * 4);
    if (!img->rgba) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }
    if (!png_image_finish_read(&png, NULL, img->rgba, 0, NULL)) {
        fprintf(stderr, "png2ico: Cannot read \"%s\": %s.\n", path, png.message);
        exit(-EINVAL);
    }
    fclose(f);

    img->width = (int)png.width;
    img->height = (int)png.height;
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
 * Draw src into an s x s RGBA icon at dst, scaled to fit and centered.
 * dst must already be zero-filled (fully transparent), since only the area
 * the picture covers is written; the rest is left as transparent padding.
 */
static void render(unsigned char *dst, int s, const struct image *src)
{
    int sw = src->width, sh = src->height;

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
     * at the top and 64 at the bottom. corner is the location in the icon
     * where the picture's top-left pixel goes.
     */
    unsigned char *corner = dst + ((size_t)((s - dh) / 2) * s + (s - dw) / 2) * 4;

    /*
     * Pick the filter: bilinear if the picture is being made larger than
     * the source, the box filter if it is being made smaller (or kept the
     * same size). Width and height are scaled by the same factor, so they
     * always grow or shrink together.
     */
    if (dw > sw || dh > sh)
        scale_bilinear(src->rgba, sw, sh, corner, s, dw, dh);
    else
        scale_box(src->rgba, sw, sh, corner, s, dw, dh);
}

/* An image's size for choosing between inputs: its longer side. */
static int longer_side(const struct image *img)
{
    return img->width > img->height ? img->width : img->height;
}

/*
 * Choose the input each target size is made from, and record its index in
 * set->source[]. See the top of this file for the rules.
 */
static void choose_sources(const struct options *opt, struct image_set *set)
{
    bool chosen[NUM_SIZES] = { false };

    /* First, every input assigned to a size is used for that size. */
    for (int i = 0; i < opt->num_inputs; i++) {
        int slot = opt->inputs[i].slot;
        if (slot < 0 || !opt->target[slot])
            continue;

        const struct image *img = &set->input[i];
        int s = possible_sizes[slot];
        if (img->width != s || img->height != s)
            fprintf(stderr, "png2ico: Warning: \"%s\" is %dx%d, not %dx%d. "
                    "It will be scaled to fit.\n", opt->inputs[i].path,
                    img->width, img->height, s, s);
        set->source[slot] = i;
        chosen[slot] = true;
    }

    /*
     * Every other size uses the smallest input that is at least that
     * size, or, if there is none, the largest input. When two inputs are
     * the same size, the one given first on the command line is used.
     */
    for (int slot = 0; slot < NUM_SIZES; slot++) {
        if (!opt->target[slot] || chosen[slot])
            continue;

        int s = possible_sizes[slot];
        int smallest_fit = -1;      /* index of the smallest input >= s */
        int largest = 0;            /* index of the largest input of all */

        for (int i = 0; i < opt->num_inputs; i++) {
            int size = longer_side(&set->input[i]);

            if (size >= s && (smallest_fit < 0 ||
                              size < longer_side(&set->input[smallest_fit])))
                smallest_fit = i;
            if (size > longer_side(&set->input[largest]))
                largest = i;
        }
        set->source[slot] = smallest_fit >= 0 ? smallest_fit : largest;
    }
}

/*
 * Step 2: decode every input, choose the input for each size to be
 * produced, and scale it to that size, filling in set.
 */
static void read_sources(const struct options *opt, struct image_set *set)
{
    memset(set, 0, sizeof *set);

    set->input = calloc((size_t)opt->num_inputs, sizeof *set->input);
    if (!set->input) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }
    for (int i = 0; i < opt->num_inputs; i++)
        read_png(&set->input[i], opt->inputs[i].path);

    choose_sources(opt, set);

    /*
     * Allocate one zero-filled block for all the icons, then lay them out
     * in it one after another, smallest first. Zero-filled means fully
     * transparent, which render() relies on for the padding.
     */
    size_t block_size = 0;
    for (int slot = 0; slot < NUM_SIZES; slot++)
        if (opt->target[slot])
            block_size += (size_t)possible_sizes[slot] * possible_sizes[slot] * 4;

    set->icon_block = calloc(block_size, 1);
    if (!set->icon_block) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }

    unsigned char *next = set->icon_block;
    for (int slot = 0; slot < NUM_SIZES; slot++) {
        if (!opt->target[slot])
            continue;
        int s = possible_sizes[slot];
        set->icon[slot] = next;
        render(next, s, &set->input[set->source[slot]]);
        next += (size_t)s * s * 4;
    }
}

/*
 * The .ico format stores all multi-byte numbers little-endian (least
 * significant byte first). These write a 16- or 32-bit value that way,
 * regardless of the byte order of the machine running this program.
 */
static void put16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, (uint16_t)(v & 0xFFFF));
    put16(p + 2, (uint16_t)(v >> 16));
}

/* Sizes of the fixed-length structures in an .ico file, in bytes. */
enum {
    ICONDIR_SIZE = 6,           /* the file header */
    ICONDIRENTRY_SIZE = 16,     /* one directory entry per image */
    BMP_HEADER_SIZE = 40        /* BITMAPINFOHEADER, at the start of each
                                   bitmap image */
};

/* Bytes in one row of a bitmap's transparency mask: 1 bit per pixel,
 * rounded up to a multiple of 4 bytes. */
static size_t bmp_mask_row(int s)
{
    return ((size_t)s + 31) / 32 * 4;
}

/* Bytes needed to store an s x s icon as a bitmap. */
static size_t bmp_size(int s)
{
    return BMP_HEADER_SIZE + (size_t)s * s * 4 + bmp_mask_row(s) * s;
}

/*
 * Store an s x s RGBA icon as a bitmap at out, which must have room for
 * bmp_size(s) bytes and must already be zero-filled.
 *
 * A bitmap has three parts, one after another:
 *   1. A 40-byte header (BITMAPINFOHEADER) giving the image dimensions and
 *      pixel format.
 *   2. The color pixels, 4 bytes each in the order blue, green, red,
 *      alpha (the reverse of PNG's red, green, blue). Rows are stored
 *      from the bottom row of the picture up to the top row.
 *   3. A transparency mask: 1 bit per pixel, a 1 marking a transparent
 *      pixel, also bottom row first. The format requires it even when the
 *      pixels have alpha. Each mask row is padded with unused bits to a
 *      multiple of 4 bytes.
 */
static void write_bmp(unsigned char *out, const unsigned char *rgba, int s)
{
    size_t mask_row = bmp_mask_row(s);
    size_t color_size = (size_t)s * s * 4;

    /*
     * Header. The height is written as twice the image height: in icon
     * files it counts the color pixels and the mask together. The fields
     * not written here are left at 0: compression (0 = none), print
     * resolution, and palette sizes, none of which apply.
     */
    put32(out, BMP_HEADER_SIZE);
    put32(out + 4, (uint32_t)s);                    /* width */
    put32(out + 8, 2 * (uint32_t)s);                /* height, doubled */
    put16(out + 12, 1);                             /* color planes */
    put16(out + 14, 32);                            /* bits per pixel */
    put32(out + 20, (uint32_t)(bmp_size(s) - BMP_HEADER_SIZE));

    unsigned char *color = out + BMP_HEADER_SIZE;
    unsigned char *mask = color + color_size;

    /* Output row y holds picture row s - 1 - y, so the bottom row is first. */
    for (int y = 0; y < s; y++) {
        const unsigned char *src = rgba + (size_t)(s - 1 - y) * s * 4;
        unsigned char *dst = color + (size_t)y * s * 4;
        unsigned char *mrow = mask + (size_t)y * mask_row;

        for (int x = 0; x < s; x++, src += 4, dst += 4) {
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
}

/*
 * PNG encoding uses libpng's png_image_write_to_memory(). Called with a
 * NULL buffer, it compresses the image only to report how many bytes the
 * PNG needs; called again with a buffer that large, it writes the PNG.
 * png_size() makes the first call and write_png() the second. Failure
 * is reported as -ENOMEM.
 */
static size_t png_size(const unsigned char *rgba, int s)
{
    png_image png;
    memset(&png, 0, sizeof png);
    png.version = PNG_IMAGE_VERSION;
    png.width = (png_uint_32)s;
    png.height = (png_uint_32)s;
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t len = 0;
    if (!png_image_write_to_memory(&png, NULL, &len, 0, rgba, 0, NULL)) {
        fprintf(stderr, "png2ico: Cannot encode the %dx%d icon: %s.\n",
                s, s, png.message);
        exit(-ENOMEM);
    }
    return len;
}

static void write_png(unsigned char *out, size_t len,
                      const unsigned char *rgba, int s)
{
    png_image png;
    memset(&png, 0, sizeof png);
    png.version = PNG_IMAGE_VERSION;
    png.width = (png_uint_32)s;
    png.height = (png_uint_32)s;
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t written = len;
    if (!png_image_write_to_memory(&png, out, &written, 0, rgba, 0, NULL)) {
        fprintf(stderr, "png2ico: Cannot encode the %dx%d icon: %s.\n",
                s, s, png.message);
        exit(-ENOMEM);
    }
}

/*
 * Step 3: encode each icon in set and write the .ico file.
 *
 * The file's layout is:
 *   1. A 6-byte header ("ICONDIR").
 *   2. One 16-byte directory entry ("ICONDIRENTRY") per image, giving that
 *      image's size, its length in bytes, and its byte offset from the
 *      start of the file.
 *   3. The image data itself: each image's bitmap or PNG, back to back.
 *
 * The whole file is assembled in one buffer and written with one call.
 * Sizes below 256 are stored as bitmaps and 256 as PNG; see the top of
 * this file.
 */
static void write_ico(const struct options *opt, const struct image_set *set)
{
    /*
     * First work out each image's length, and from those the length of
     * the whole file, so that it can be allocated at once. A bitmap's
     * length follows from its size; a PNG's is only known by compressing
     * it.
     */
    size_t len[NUM_SIZES] = { 0 };
    size_t file_size = ICONDIR_SIZE;
    unsigned count = 0;

    for (int slot = 0; slot < NUM_SIZES; slot++) {
        if (!opt->target[slot])
            continue;
        int s = possible_sizes[slot];
        len[slot] = s == 256 ? png_size(set->icon[slot], s) : bmp_size(s);
        file_size += ICONDIRENTRY_SIZE + len[slot];
        count++;
    }

    /* Zero-filled, as write_bmp() requires. */
    unsigned char *file = calloc(file_size, 1);
    if (!file) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }

    /* Header: reserved (must be 0), file type (1 = icon, 2 = cursor), and
     * number of images. */
    put16(file, 0);
    put16(file + 2, 1);
    put16(file + 4, (uint16_t)count);

    /* The first image starts right after the header and all directory
     * entries. Each later image starts where the previous one ended. */
    unsigned char *entry = file + ICONDIR_SIZE;
    size_t offset = ICONDIR_SIZE + (size_t)count * ICONDIRENTRY_SIZE;

    for (int slot = 0; slot < NUM_SIZES; slot++) {
        if (!opt->target[slot])
            continue;
        int s = possible_sizes[slot];

        /* Width and height in one byte each. 256 doesn't fit in a byte,
         * so the format uses 0 to mean 256. */
        entry[0] = s == 256 ? 0 : (unsigned char)s;
        entry[1] = s == 256 ? 0 : (unsigned char)s;

        /* entry[2], the number of palette colors, and entry[3], reserved,
         * stay 0. */

        /*
         * Color planes: always 1. Bits per pixel: 32, meaning 8 bits each
         * for red, green, blue and alpha.
         */
        put16(entry + 4, 1);
        put16(entry + 6, 32);

        /* Length of this image's data, then where in the file it starts. */
        put32(entry + 8, (uint32_t)len[slot]);
        put32(entry + 12, (uint32_t)offset);

        if (s == 256)
            write_png(file + offset, len[slot], set->icon[slot], s);
        else
            write_bmp(file + offset, set->icon[slot], s);

        offset += len[slot];
        entry += ICONDIRENTRY_SIZE;
    }

    /*
     * Write to a temporary file in the same directory, then rename it to
     * OUTPUT. On any failure, delete the temporary file. Each errno is
     * saved before calling remove(), which may change it; EIO stands in if
     * a call failed without setting errno.
     */
    /*
     * The temporary file's name is OUTPUT.PID.N. The process ID keeps it
     * distinct from any other run's; N counts up from 0 if a file of that
     * name already exists, such as one left behind by a crashed run.
     * O_EXCL makes open() fail with EEXIST rather than reuse an existing
     * file. The kernel applies the umask to the 0666 mode, giving the same
     * permissions fopen() would.
     */
    size_t tmp_size = strlen(opt->output) + 48;    /* room for ".PID.N" */
    char *tmp = malloc(tmp_size);
    if (!tmp) {
        fprintf(stderr, "png2ico: Out of memory.\n");
        exit(-ENOMEM);
    }

    int fd;
    for (unsigned attempt = 0; ; attempt++) {
        snprintf(tmp, tmp_size, "%s.%ld.%u", opt->output, (long)getpid(), attempt);
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0)
            break;
        if (errno != EEXIST || attempt == 99) {
            int err = errno ? errno : EIO;
            fprintf(stderr, "png2ico: Cannot create \"%s\": %s.\n",
                    tmp, strerror(err));
            exit(-err);
        }
    }

    FILE *f = fdopen(fd, "wb");
    if (!f) {
        int err = errno ? errno : EIO;
        close(fd);
        remove(tmp);
        fprintf(stderr, "png2ico: Cannot write \"%s\": %s.\n",
                tmp, strerror(err));
        exit(-err);
    }

    if (fwrite(file, 1, file_size, f) != file_size) {
        int err = errno ? errno : EIO;
        fclose(f);
        remove(tmp);
        fprintf(stderr, "png2ico: Cannot write \"%s\": %s.\n",
                tmp, strerror(err));
        exit(-err);
    }

    /* fwrite() is buffered; fclose() writes the rest and reports whether
     * that succeeded. */
    if (fclose(f) != 0) {
        int err = errno ? errno : EIO;
        remove(tmp);
        fprintf(stderr, "png2ico: Cannot write \"%s\": %s.\n",
                tmp, strerror(err));
        exit(-err);
    }

    if (rename(tmp, opt->output) != 0) {
        int err = errno ? errno : EIO;
        remove(tmp);
        fprintf(stderr, "png2ico: Cannot rename \"%s\" to \"%s\": %s.\n",
                tmp, opt->output, strerror(err));
        exit(-err);
    }

    free(tmp);
    free(file);
}

/* Step 4: free everything allocated by the earlier steps. */
static void cleanup(struct options *opt, struct image_set *set)
{
    for (int i = 0; i < opt->num_inputs; i++)
        free(set->input[i].rgba);
    free(set->input);
    free(set->icon_block);
    free(opt->inputs);
}

int main(int argc, char **argv)
{
    struct options opt;
    struct image_set set;

    parse_command_line(&opt, argc, argv);
    read_sources(&opt, &set);
    write_ico(&opt, &set);
    cleanup(&opt, &set);
    return EXIT_SUCCESS;
}
