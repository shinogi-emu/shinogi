/*
 * shinogi-shot - crop and magnify a captured screen.
 *
 *   shinogi-shot <in.ppm> <out.png> [--crop X,Y,W,H] [--scale N]
 *
 * The emulator can write a PNG of the whole screen and nothing else, and
 * the whole screen is the wrong picture for most questions asked of it.
 * A GEM dialog is a few hundred pixels wide inside 1280x720; looking at
 * it whole means either squinting at it or downscaling it, and
 * downscaled screenshots have produced two false rendering findings in
 * this project already. So: capture the screen as PPM, cut out the part
 * that matters, and magnify it by a whole number of pixels -- nearest
 * neighbour, so a magnified pixel is a block of identical pixels and no
 * colour appears that was not in the original.
 *
 * The PNG is written with stored (uncompressed) deflate blocks. That
 * makes a larger file than real compression would, and it means this
 * program needs no zlib -- which matters, because it is built static so
 * that it runs on a machine where nothing has been installed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long crc_table[256];

static void crc_init(void)
{
    unsigned long c;
    int n, k;

    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc_update(unsigned long crc, const unsigned char *buf, size_t len)
{
    size_t n;

    for (n = 0; n < len; n++)
        crc = crc_table[(crc ^ buf[n]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void write_chunk(FILE *f, const char *type, const unsigned char *data, size_t len)
{
    unsigned char hdr[8];
    unsigned char crcbuf[4];
    unsigned long crc;

    put_be32(hdr, (unsigned long)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len)
        fwrite(data, 1, len, f);

    crc = crc_update(0xffffffffUL, hdr + 4, 4);
    if (len)
        crc = crc_update(crc, data, len);
    put_be32(crcbuf, crc ^ 0xffffffffUL);
    fwrite(crcbuf, 1, 4, f);
}

/* Skip whitespace and #-comments between PPM header fields. */
static int ppm_int(FILE *f)
{
    int c, v = 0, got = 0;

    for (;;) {
        c = fgetc(f);
        if (c == EOF)
            return -1;
        if (c == '#') {
            while (c != '\n' && c != EOF)
                c = fgetc(f);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (got)
                return v;
            continue;
        }
        if (c < '0' || c > '9')
            return -1;
        v = v * 10 + (c - '0');
        got = 1;
    }
}

int main(int argc, char **argv)
{
    const char *in_path, *out_path;
    int cx = 0, cy = 0, cw = 0, ch = 0, scale = 1;
    int w, h, maxval, i, y, x, s;
    unsigned char *pix, *row;
    unsigned char ihdr[13];
    unsigned char *idat;
    size_t raw_stride, raw_len, idat_len, pos;
    unsigned long a = 1, b = 0;
    FILE *f;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.ppm> <out.png> [--crop X,Y,W,H] [--scale N]\n",
                argv[0]);
        return 2;
    }
    in_path = argv[1];
    out_path = argv[2];
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--crop") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d", &cx, &cy, &cw, &ch) != 4) {
                fprintf(stderr, "--crop wants X,Y,W,H\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1 || scale > 16) {
                fprintf(stderr, "--scale wants 1..16\n");
                return 2;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    f = fopen(in_path, "rb");
    if (!f) {
        perror(in_path);
        return 1;
    }
    if (fgetc(f) != 'P' || fgetc(f) != '6') {
        fprintf(stderr, "%s is not a binary PPM\n", in_path);
        return 1;
    }
    w = ppm_int(f);
    h = ppm_int(f);
    maxval = ppm_int(f);
    if (w <= 0 || h <= 0 || maxval != 255) {
        fprintf(stderr, "%s: unsupported PPM (%dx%d maxval %d)\n",
                in_path, w, h, maxval);
        return 1;
    }
    pix = malloc((size_t)w * (size_t)h * 3);
    if (!pix || fread(pix, 1, (size_t)w * (size_t)h * 3, f) != (size_t)w * (size_t)h * 3) {
        fprintf(stderr, "%s: short read\n", in_path);
        return 1;
    }
    fclose(f);

    if (cw == 0 && ch == 0) {
        cw = w;
        ch = h;
    }
    /* Clamp rather than refuse: a caller asking for a region that runs
     * off the edge wants what is there, and an error here would only be
     * answered by doing this arithmetic again outside. */
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= w) cx = w - 1;
    if (cy >= h) cy = h - 1;
    if (cx + cw > w) cw = w - cx;
    if (cy + ch > h) ch = h - cy;
    if (cw <= 0 || ch <= 0) {
        fprintf(stderr, "empty crop\n");
        return 1;
    }

    raw_stride = 1 + (size_t)cw * 3 * (size_t)scale;
    raw_len = raw_stride * (size_t)ch * (size_t)scale;

    /* Stored deflate: 2 zlib header bytes, a 5-byte header per 65535
     * bytes of payload, then 4 bytes of adler32. */
    idat_len = 2 + raw_len + 5 * (raw_len / 65535 + 1) + 4;
    idat = malloc(idat_len);
    if (!idat) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* Build the raw stream first: filter byte 0 then the pixels, each
     * source pixel repeated scale times across and each row repeated
     * scale times down. */
    row = malloc(raw_len);
    if (!row) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    pos = 0;
    for (y = 0; y < ch * scale; y++) {
        int sy = cy + y / scale;
        row[pos++] = 0;
        for (x = 0; x < cw; x++) {
            const unsigned char *p = pix + ((size_t)sy * w + (size_t)(cx + x)) * 3;
            for (s = 0; s < scale; s++) {
                row[pos++] = p[0];
                row[pos++] = p[1];
                row[pos++] = p[2];
            }
        }
    }

    for (pos = 0; pos < raw_len; pos++) {
        a = (a + row[pos]) % 65521;
        b = (b + a) % 65521;
    }

    /* Wrap it in a zlib stream of stored blocks. */
    pos = 0;
    idat[pos++] = 0x78;
    idat[pos++] = 0x01;
    {
        size_t left = raw_len, off = 0;
        do {
            size_t n = left > 65535 ? 65535 : left;
            idat[pos++] = (left == n) ? 1 : 0;      /* BFINAL on the last */
            idat[pos++] = (unsigned char)(n & 0xff);
            idat[pos++] = (unsigned char)(n >> 8);
            idat[pos++] = (unsigned char)(~n & 0xff);
            idat[pos++] = (unsigned char)((~n >> 8) & 0xff);
            memcpy(idat + pos, row + off, n);
            pos += n;
            off += n;
            left -= n;
        } while (left);
    }
    put_be32(idat + pos, (b << 16) | a);
    pos += 4;

    crc_init();
    f = fopen(out_path, "wb");
    if (!f) {
        perror(out_path);
        return 1;
    }
    fwrite("\211PNG\r\n\032\n", 1, 8, f);
    put_be32(ihdr, (unsigned long)(cw * scale));
    put_be32(ihdr + 4, (unsigned long)(ch * scale));
    ihdr[8] = 8;        /* bit depth */
    ihdr[9] = 2;        /* truecolour */
    ihdr[10] = 0;       /* deflate */
    ihdr[11] = 0;       /* adaptive filtering */
    ihdr[12] = 0;       /* no interlace */
    write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    write_chunk(f, "IDAT", idat, pos);
    write_chunk(f, "IEND", NULL, 0);
    fclose(f);

    printf("%dx%d\n", cw * scale, ch * scale);
    return 0;
}
