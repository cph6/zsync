
/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2007,2009 Colin Phipps <cph@moria.org.uk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the Artistic License v2 (see the accompanying
 *   file COPYING for the full license terms), or, at your option, any later
 *   version of the same license.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   COPYING file for details.
 */

#include "zsglobal.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

#include "zlib/zlib.h"

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

/* fputlong(filehandle, long)
 * Writes a 32bit int as raw bytes in little-endian to the given filehandle.
 * Returns 0 if successful; otherwise ferror(filehandle) to see the error */
static int fputlong(FILE * f, unsigned long x) {
    int n;
    for (n = 0; n < 4; n++) {
        if (fputc((int)(x & 0xff), f) == EOF)
            return -1;
        x >>= 8;
    }
    return 0;
}

/* time = get_mtime(filehandle)
 * Get the mtime of a file from an open filehandle; or 0 if unavailable */
time_t get_mtime(FILE * f) {
    struct stat s;

    if (fstat(fileno(f), &s) == 0)
        return s.st_mtime;
    else
        return 0;
}

/* filehandle = optimal_gzip(filehandle, out_filename_str, blocksize)
 * Constructs a compressed version of the data in the file referenced by the
 * supplied filehandle; this is saved in a file with the filename supplied as
 * the second parameter. The compressed version is optimised for the zsync
 * algorithm with the supplied blocksize. The function returns a handle to the
 * compressed file (opened for r+w but rewound to the start of the file,
 * ready for reading.
 */

/* Algorithm: simple really. We construct a gzip (have to write header with
 * mtime, footer with length and crc; we do a standard zlib compress on the
 * content _except_ that we supply one block (of the size that zsync will be
 * using) and a Z_PARTIAL_FLUSH at the end of each one, so zlib breaks the
 * compression runs at exactly the places zsync will start/stop retrieving data.
 */
FILE *optimal_gzip(FILE * ffin, const char *fout, size_t blocksize) {
    time_t mtime = get_mtime(ffin);

    /* Open output file (for writing, but also reading so we can return the
     * handle for reading by the caller. */
    FILE *ffout = fopen(fout, "wb+");
    if (!ffout) {
        perror("open");
        return NULL;
    }

    /* Write gzip header */
    if (fwrite("\x1f\x8b\x08\x00", 4, 1, ffout) != 1) {
        perror("write");
        return NULL;
    }
    if (fputlong(ffout, mtime) == -1) {
        perror("write");
        return NULL;
    }
    if (fwrite("\x00\x03", 2, 1, ffout) != 1) {
        perror("write");
        return NULL;
    }

    {   /* Now write compressed content */
        z_stream zs;
        unsigned char *inbuf = malloc(blocksize);
        unsigned char *outbuf = malloc(blocksize + 500);
        int err, r;
        unsigned long crc = crc32(0L, Z_NULL, 0);

        /* Set up zlib object */
        zs.zalloc = Z_NULL;
        zs.zfree = Z_NULL;
        zs.opaque = NULL;
        zs.total_in = 0;
        zs.total_out = 0;

        /* windowBits is passed < 0 to suppress zlib header */
        err = deflateInit2(&zs, 9,
                           Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);

        /* Until end of file or error */
        for (r = 1; r > 0;) {
            r = fread(inbuf, 1, blocksize, ffin);
            if (r < 0)
                break;

            /* Maintain running crc (for footer) */
            crc = crc32(crc, inbuf, r);

            /* Set up compressor for this block */
            zs.next_in = inbuf;
            zs.avail_in = r;
            zs.next_out = outbuf;
            zs.avail_out = blocksize + 500;

            /* Compress with partial flush at the end of this block 
             * unless EOF, in which case finish */
            err = deflate(&zs, r ? Z_PARTIAL_FLUSH : Z_FINISH);
            switch (err) {
            case Z_STREAM_END:
            case Z_OK:
                {
                    size_t w = zs.next_out - outbuf;

                    if (w != fwrite(outbuf, 1, w, ffout)) {
                        perror("write");
                        r = -1;
                    }
                }
                break;
            default:
                fprintf(stderr, "zlib error: %s (%d)\n", zs.msg, err);
                r = -1;
            }
        }

        /* Write gzip footer */
        if (fputlong(ffout, crc) == -1) {
            perror("write");
            return NULL;
        }
        if (fputlong(ffout, zs.total_in) == -1) {
            perror("write");
            return NULL;
        }

        /* Clean up */
        fflush(ffout);
        free(outbuf);
        free(inbuf);
        if (fclose(ffin) != 0 || r != 0) {
            fclose(ffout);
            return NULL;
        }
    }

    /* Return rewound handle on the compressed data to the caller */
    rewind(ffout);
    return ffout;
}
