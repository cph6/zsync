/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005,2009 Colin Phipps <cph@moria.org.uk>
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

/* Command-line utility to create .zsync files */

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <math.h>
#include <time.h>

#include <arpa/inet.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "makegz.h"
#include "librcksum/rcksum.h"
#include "libzsync/zmap.h"
#include "libzsync/sha1.h"
#include "zlib/zlib.h"
#include "format_string.h"

/* We're only doing one file per run, so these are global state for the current
 * file being processed */
SHA1_CTX shactx;
size_t blocksize = 0;
off_t len = 0;

/* And settings from the command line */
int verbose = 0;
static int no_look_inside;

/* stream_error(function, stream) - Exit with IO-related error message */
void __attribute__ ((noreturn)) stream_error(const char *func, FILE * stream) {
    fprintf(stderr, "%s: %s\n", func, strerror(ferror(stream)));
    exit(2);
}

/* write_block_sums(buffer[], num_bytes, output_stream)
 * Given one block of data, calculate the checksums for this block and write
 * them (as raw bytes) to the given output stream */
static void write_block_sums(unsigned char *buf, size_t got, FILE * f) {
    struct rsum r;
    unsigned char checksum[CHECKSUM_SIZE];

    /* Pad for our checksum, if this is a short last block  */
    if (got < blocksize)
        memset(buf + got, 0, blocksize - got);

    /* Do rsum and checksum, and convert to network endian */
    r = rcksum_calc_rsum_block(buf, blocksize);
    rcksum_calc_checksum(&checksum[0], buf, blocksize);
    r.a = htons(r.a);
    r.b = htons(r.b);

    /* Write them raw to the stream */
    if (fwrite(&r, sizeof r, 1, f) != 1)
        stream_error("fwrite", f);
    if (fwrite(checksum, sizeof checksum, 1, f) != 1)
        stream_error("fwrite", f);
}

/* long long pos = in_position(z_stream*)
 * Returns the position (in bits) that zlib has used in the compressed data
 * stream so far */
static inline long long in_position(z_stream * pz) {
    return pz->total_in * (long long)8 - (63 & pz->data_type);
}

/* State for compressed file handling */
static FILE *zmap;
static int zmapentries;
static char *zhead;

/* write_zmap_delta(*prev_in, *prev_out, new_in, new_out, blockstart)
 * Given a position in the compressed and uncompressed streams, write a
 * checkpoint/map entry (to the stream held in the global variable zmap).
 * This is relative to the previous position supplied, and positions must be
 * supplied in order; caller provide two long long* as the first two parameters
 * for write_zmap_delta to use to keep state in.
 * blockstart is a boolean, is true if this is the start of a zlib block
 * (otherwise, this is a mid-block marker).
 */
static void write_zmap_delta(long long *prev_in, long long *prev_out,
                             long long new_in, long long new_out,
                             int blockstart) {
    struct gzblock g;
    {   /* Calculate number of bits that the input (compressed stream) pointer
         * has advanced from the previous entry. */
        uint16_t inbits = new_in - *prev_in;

        if (*prev_in + inbits != new_in) {
            fprintf(stderr,
                    "too long between blocks (try a smaller block size with -b)\n");
            exit(1);
        }

        /* Convert to network endian, save in zmap struct, update state */
        inbits = htons(inbits);
        g.inbitoffset = inbits;
        *prev_in = new_in;
    }
    {   /* Calculate number of bits that the output (uncompressed stream)
         * pointer has advanced from the previous entry. */
        uint16_t outbytes = new_out - *prev_out;

        outbytes &= ~GZB_NOTBLOCKSTART;
        if ((long long)outbytes + *prev_out != new_out) {
            fprintf(stderr, "too long output of block blocks?");
            exit(1);
        }
        /* Encode blockstart marker in this value */
        if (!blockstart)
            outbytes |= GZB_NOTBLOCKSTART;

        /* Convert to network endian, save in zmap struct, update state */
        outbytes = htons(outbytes);
        g.outbyteoffset = outbytes;
        *prev_out = new_out;
    }

    /* Write out the zmap delta struct */
    if (fwrite(&g, sizeof(g), 1, zmap) != 1) {
        perror("write");
        exit(1);
    }

    /* And keep state */
    zmapentries++;
}

/* do_zstream(data_stream, zsync_stream, buffer, buffer_len)
 * Constructs the zmap for a compressed data stream, in a temporary file.
 * The compressed data is from data_stream, except that some bytes have already
 * been read from it - those are supplied in buffer (buffer_len of them).
 * The zsync block checksums are written to zsync_stream, and the zmap is
 * written to a temp file and the handle returned in the global var zmap.
 */
void do_zstream(FILE * fin, FILE * fout, const char *bufsofar, size_t got) {
    z_stream zs;
    Bytef *inbuf = malloc(blocksize);
    const size_t inbufsz = blocksize;
    Bytef *outbuf = malloc(blocksize);
    int eoz = 0;
    int header_bits;
    long long prev_in = 0;
    long long prev_out = 0;
    long long midblock_in = 0;
    long long midblock_out = 0;
    int want_zdelta = 0;

    if (!inbuf || !outbuf) {
        fprintf(stderr, "memory allocation failure\n");
        exit(1);
    }

    /* Initialize decompressor */
    zs.zalloc = Z_NULL;
    zs.zfree = Z_NULL;
    zs.opaque = NULL;
    zs.next_in = inbuf;
    zs.avail_in = 0;
    zs.total_in = 0;
    zs.next_out = outbuf;
    zs.avail_out = 0;
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        exit(-1);

    {   /* Skip gzip header and do initial buffer fill */
        const char *p = skip_zhead(bufsofar);

        {   /* Store hex version of gzip header in zhead */
            int header_bytes = p - bufsofar;
            int i;

            header_bits = 8 * header_bytes;
            got -= header_bytes;

            zhead = malloc(1 + 2 * header_bytes);
            for (i = 0; i < header_bytes; i++)
                sprintf(zhead + 2 * i, "%02x", (unsigned char)bufsofar[i]);
        }
        if (got > inbufsz) {
            fprintf(stderr,
                    "internal failure, " SIZE_T_PF " > " SIZE_T_PF
                    " input buffer available\n", got, inbufsz);
            exit(2);
        }

        /* Copy any remaining already-read data from the buffer to the
         * decompressor input buffer */
        memcpy(inbuf, p, got);
        zs.avail_in = got;

        /* Fill the buffer up to offset inbufsz of the input file - we want to
         * try and keep the input blocks aligned with block boundaries in the
         * underlying filesystem and physical storage */
        if (inbufsz > got + (header_bits / 8))
            zs.avail_in +=
                fread(inbuf + got, 1, inbufsz - got - (header_bits / 8), fin);
    }

    /* Start the zmap. We write into a temp file, which the caller then copies into the zsync file later. */
    zmap = tmpfile();
    if (!zmap) {
        perror("tmpfile");
        exit(2);
    }

    /* We are past the header, so we are now at the start of the first block */
    write_zmap_delta(&prev_in, &prev_out, header_bits, zs.total_out, 1);
    zs.avail_out = blocksize;

    /* keep going until the end of the compressed stream */
    while (!eoz) {
        /* refill input buffer if empty */
        if (zs.avail_in == 0) {
            int rc = fread(inbuf, 1, inbufsz, fin);
            if (rc < 0) {
                perror("read");
                exit(2);
            }

            /* Still expecting data (!eoz and avail_in == 0) but none there. */
            if (rc == 0) {
                fprintf(stderr, "Premature end of compressed data.\n");
                exit(1);
            }

            zs.next_in = inbuf;
            zs.avail_in = rc;
        }
        {
            int rc;

            /* Okay, decompress more data from inbuf to outbuf.
             * Z_BLOCK means that decompression will halt if we reach the end of a
             *  compressed block in the input file.
             * And decompression will also stop if outbuf is filled (at which point
             *  we have a whole block of uncompressed data and so should write its
             *  checksums)
             *
             * Terminology note:
             * Compressed block   = zlib block (stream of bytes compressed with
             *                      common huffman table)
             * Uncompressed block = Block of blocksize bytes (starting at an
             *                      offset that is a whole number of blocksize
             *                      bytes blocks from the start of the
             *                      (uncompressed) data. I.e. a zsync block.
             */
            rc = inflate(&zs, Z_BLOCK);
            switch (rc) {
            case Z_STREAM_END:
                eoz = 1;
            case Z_BUF_ERROR:  /* Not really an error, just means we provided stingy buffers */
            case Z_OK:
                break;
            default:
                fprintf(stderr, "zlib error %s\n", zs.msg);
                exit(1);
            }

            /* If the output buffer is filled, i.e. we've now got a whole block of uncompressed data. */
            if (zs.avail_out == 0 || rc == Z_STREAM_END) {
                /* Add to the running SHA1 of the entire file. */
                SHA1Update(&shactx, outbuf, blocksize - zs.avail_out);

                /* Completed a block; write out its checksums */
                write_block_sums(outbuf, blocksize - zs.avail_out, fout);

                /* Clear the decompressed data buffer, ready for the next block of uncompressed data. */
                zs.next_out = outbuf;
                zs.avail_out = blocksize;

                /* Having passed a block boundary in the uncompressed data */
                want_zdelta = 1;
            }

            /* If we have reached a block boundary in the compressed data */
            if (zs.data_type & 128 || rc == Z_STREAM_END) {
                /* write out info on this block */
                write_zmap_delta(&prev_in, &prev_out,
                                 header_bits + in_position(&zs), zs.total_out,
                                 1);

                midblock_in = midblock_out = 0;
                want_zdelta = 0;
            }

            /* If we passed a block boundary in the uncompressed data, record the
             * next available point at which we could stop or start decompression.
             * Write a zmap delta with the 1st when we see the 2nd, etc */
            if (want_zdelta && inflateSafePoint(&zs)) {
                long long cur_in = header_bits + in_position(&zs);
                if (midblock_in) {
                    write_zmap_delta(&prev_in, &prev_out, midblock_in,
                                     midblock_out, 0);
                }
                midblock_in = cur_in;
                midblock_out = zs.total_out;
                want_zdelta = 0;
            }
        }
    }

    /* Record uncompressed length */
    len += zs.total_out;
    fputc('\n', fout);
    /* Move back to the start of the zmap constructed, ready for the caller to read it back in */
    rewind(zmap);

    /* Clean up */
    inflateEnd(&zs);
    free(inbuf);
    free(outbuf);
}

/* read_stream_write_blocksums(data_stream, zsync_stream)
 * Reads the data stream and writes to the zsync stream the blocksums for the
 * given data. No compression handling.
 */
void read_stream_write_blocksums(FILE * fin, FILE * fout) {
    unsigned char *buf = malloc(blocksize);

    if (!buf) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    while (!feof(fin)) {
        int got = fread(buf, 1, blocksize, fin);

        if (got > 0) {
            if (!no_look_inside && len == 0 && buf[0] == 0x1f && buf[1] == 0x8b) {
                do_zstream(fin, fout, (char *)buf, got);
                break;
            }

            /* The SHA-1 sum, unlike our internal block-based sums, is on the whole file and nothing else - no padding */
            SHA1Update(&shactx, buf, got);

            write_block_sums(buf, got, fout);
            len += got;
        }
        else {
            if (ferror(fin))
                stream_error("fread", fin);
        }
    }
    free(buf);
}

/* fcopy(instream, outstream)
 * Copies data from one stream to the other until EOF on the input.
 */
void fcopy(FILE * fin, FILE * fout) {
    unsigned char buf[4096];
    size_t len;

    while ((len = fread(buf, 1, sizeof(buf), fin)) > 0) {
        if (fwrite(buf, 1, len, fout) < len)
            break;
    }
    if (ferror(fin)) {
        stream_error("fread", fin);
    }
    if (ferror(fout)) {
        stream_error("fwrite", fout);
    }
}

/* fcopy_hashes(hash_stream, zsync_stream, rsum_bytes, hash_bytes)
 * Copy the full block checksums from their temporary store file to the .zsync,
 * stripping the hashes down to the desired lengths specified by the last 2
 * parameters.
 */
void fcopy_hashes(FILE * fin, FILE * fout, size_t rsum_bytes, size_t hash_bytes) {
    unsigned char buf[20];
    size_t len;

    while ((len = fread(buf, 1, sizeof(buf), fin)) > 0) {
        /* write trailing rsum_bytes of the rsum (trailing because the second part of the rsum is more useful in practice for hashing), and leading checksum_bytes of the checksum */
        if (fwrite(buf + 4 - rsum_bytes, 1, rsum_bytes, fout) < rsum_bytes)
            break;
        if (fwrite(buf + 4, 1, hash_bytes, fout) < hash_bytes)
            break;
    }
    if (ferror(fin)) {
        stream_error("fread", fin);
    }
    if (ferror(fout)) {
        stream_error("fwrite", fout);
    }
}

/* read_sample_and_close(stream, len, buf)
 * Reads len bytes from stream into buffer */
static int read_sample_and_close(FILE * f, size_t l, void *buf) {
    int rc = 0;
    if (fread(buf, 1, l, f) == l)
        rc = 1;
    else if (errno != EBADF)
        perror("read");
    fclose(f);
    return rc;
}

/* str = encode_filename(filename_str)
 * Returns shell-escaped version of a given (filename) string */
static char *encode_filename(const char *fname) {
    char *cmd = malloc(2 + strlen(fname) * 2);
    if (!cmd)
        return NULL;

    {   /* pass through string character by character */
        int i, j;
        for (i = j = 0; fname[i]; i++) {
            if (!isalnum(fname[i]))
                cmd[j++] = '\\';
            cmd[j++] = fname[i];
        }
        cmd[j] = 0;
    }
    return cmd;
}

/* opt_str = guess_gzip_options(filename_str)
 * For the given (gzip) file, try to guess the options that were used with gzip
 * to create it.
 * Returns a malloced string containing the options for gzip, or NULL */
static const char *const try_opts[] =
    { "--best", "", "--rsync", "--rsync --best", NULL };
#define SAMPLE 1024

char *guess_gzip_options(const char *f) {
    char orig[SAMPLE];
    {   /* Read sample of the header of the compressed file */
        FILE *s = fopen(f, "r");
        if (!s) {
            perror("open");
            return NULL;
        }
        if (!read_sample_and_close(s, SAMPLE, orig))
            return NULL;
    }
    {
        int i;
        const char *o;
        char *enc_f = encode_filename(f);
        int has_mtime_fname;

        {
            int has_mtime = zhead_has_mtime(orig);
            int has_fname = zhead_has_fname(orig);

            if (has_mtime && !has_fname) {
                fprintf(stderr, "can't recompress, stream has mtime but no fname\n");
                return NULL;
            }
            else if (has_fname && !has_mtime) {
                fprintf(stderr, "can't recompress, stream has fname but no mtime\n");
                return NULL;
            }
            else {
                has_mtime_fname = has_fname; /* which = has_mtime */
            }
        }

        /* For each likely set of options, try recompressing the content with
         * those options */
        for (i = 0; (o = try_opts[i]) != NULL; i++) {
            FILE *p;
            {   /* Compose command line */
                char cmd[1024];
                snprintf(cmd, sizeof(cmd), "zcat %s | gzip -n %s 2> /dev/null",
                        enc_f, o);

                /* And run it */
                if (verbose)
                    fprintf(stderr, "running %s to determine gzip options\n",
                            cmd);
                p = popen(cmd, "r");
                if (!p) {
                    perror(cmd);
                }
            }

            if (p) {   /* Read the recompressed content */
                char samp[SAMPLE];
                if (!read_sample_and_close(p, SAMPLE, samp)) {
                    ;       /* Read error - just fail this one and let the loop
                             * try another */
                }
                else {
                    /* We have the compressed version with these options.
                     * Compare with the original */
                    const char *a = skip_zhead(orig);
                    const char *b = skip_zhead(samp);
                    if (!memcmp(a, b, 900))
                        break;
                }
            }
        }
        free(enc_f);

        if (!o) {
            return NULL;
        }
        else if (has_mtime_fname) {
            return strdup(o);
        }
        else {  /* Add --no-name to options to return */
            static const char noname[] = { "--no-name" };
            char* opts = malloc(strlen(o)+strlen(noname)+2);
            if (o[0]) {
                strcpy(opts, o);
                strcat(opts, " ");
            }
            else { opts[0] = 0; }
            strcat(opts, noname);
            return opts;
        }
    }
}

/* len = get_len(stream)
 * Returns the length of the file underlying this stream */
off_t get_len(FILE * f) {
    struct stat s;

    if (fstat(fileno(f), &s) == -1)
        return 0;
    return s.st_size;
}

/****************************************************************************
 *
 * Main program
 */
int main(int argc, char **argv) {
    FILE *instream;
    char *fname = NULL, *zfname = NULL;
    char **url = NULL;
    int nurls = 0;
    char **Uurl = NULL;
    int nUurls = 0;
    char *outfname = NULL;
    FILE *fout;
    char *infname = NULL;
    int rsum_len, checksum_len, seq_matches;
    int do_compress = 0;
    int do_recompress = -1;     // -1 means we decide for ourselves
    int do_exact = 0;
    char *gzopts = NULL;
    time_t mtime = -1;

    /* Open temporary file */
    FILE *tf = tmpfile();

    {   /* Options parsing */
        int opt;
        while ((opt = getopt(argc, argv, "b:Ceo:f:u:U:vVzZ")) != -1) {
            switch (opt) {
            case 'e':
                do_exact = 1;
                break;
            case 'C':
                do_recompress = 0;
                break;
            case 'o':
                if (outfname) {
                    fprintf(stderr, "specify -o only once\n");
                    exit(2);
                }
                outfname = strdup(optarg);
                break;
            case 'f':
                if (fname) {
                    fprintf(stderr, "specify -f only once\n");
                    exit(2);
                }
                fname = strdup(optarg);
                break;
            case 'b':
                blocksize = atoi(optarg);
                if ((blocksize & (blocksize - 1)) != 0) {
                    fprintf(stderr,
                            "blocksize must be a power of 2 (512, 1024, 2048, ...)\n");
                    exit(2);
                }
                break;
            case 'u':
                url = realloc(url, (nurls + 1) * sizeof *url);
                url[nurls++] = optarg;
                break;
            case 'U':
                Uurl = realloc(Uurl, (nUurls + 1) * sizeof *Uurl);
                Uurl[nUurls++] = optarg;
                break;
            case 'v':
                verbose++;
                break;
            case 'V':
                printf(PACKAGE " v" VERSION " (zsyncmake compiled " __DATE__ " "
                       __TIME__ ")\n" "By Colin Phipps <cph@moria.org.uk>\n"
                       "Published under the Artistic License v2, see the COPYING file for details.\n");
                exit(0);
            case 'z':
                do_compress = 1;
                break;
            case 'Z':
                no_look_inside = 1;
                break;
            }
        }

        /* Open data to create .zsync for - either it's a supplied filename, or stdin */
        if (optind == argc - 1) {
            infname = strdup(argv[optind]);
            instream = fopen(infname, "rb");
            if (!instream) {
                perror("open");
                exit(2);
            }

            {   /* Get mtime if available */
                struct stat st;
                if (fstat(fileno(instream), &st) == 0) {
                    mtime = st.st_mtime;
                }
            }

            /* Use supplied filename as the target filename */
            if (!fname)
                fname = basename(argv[optind]);
        }
        else {
            instream = stdin;
        }
    }

    /* If not user-specified, choose a blocksize based on size of the input file */
    if (!blocksize) {
        blocksize = (get_len(instream) < 100000000) ? 2048 : 4096;
    }

    /* If we've been asked to compress this file, do so and substitute the
     * compressed version for the original */
    if (do_compress) {
        char *newfname = NULL;

        {   /* Try adding .gz to the input filename */
            char *tryfname = infname;
            if (!tryfname) {
                tryfname = fname;
            }
            if (tryfname) {
                newfname = malloc(strlen(tryfname) + 4);
                if (!newfname)
                    exit(1);
                strcpy(newfname, tryfname);
                strcat(newfname, ".gz");
            }
        }

        /* If we still don't know what to call it, default name */
        if (!newfname) {
            newfname = strdup("zsync-target.gz");
            if (!newfname)
                exit(1);
        }

        /* Create optimal compressed version */
        instream = optimal_gzip(instream, newfname, blocksize);
        if (!instream) {
            fprintf(stderr, "failed to compress\n");
            exit(-1);
        }

        /* This replaces the original input stream for creating the .zsync */
        if (infname) {
            free(infname);
            infname = newfname;
        }
        else
            free(newfname);
    }

    /* Read the input file and construct the checksum of the whole file, and
     * the per-block checksums */
    SHA1Init(&shactx);
    read_stream_write_blocksums(instream, tf);

    {   /* Decide how long a rsum hash and checksum hash per block we need for this file */
        seq_matches = len > blocksize ? 2 : 1;
        rsum_len = ceil(((log(len) + log(blocksize)) / log(2) - 8.6) / seq_matches / 8);

        /* min and max lengths of rsums to store */
        if (rsum_len > 4) rsum_len = 4;
        if (rsum_len < 2) rsum_len = 2;

        /* Now the checksum length; min of two calculations */
        checksum_len = ceil(
                (20 + (log(len) + log(1 + len / blocksize)) / log(2))
                / seq_matches / 8);
        {
            int checksum_len2 =
                (7.9 + (20 + log(1 + len / blocksize) / log(2))) / 8;
            if (checksum_len < checksum_len2)
                checksum_len = checksum_len2;
        }
    }

    /* Recompression:
     * Where we were given a compressed file (not an uncompressed file that we
     * then compressed), but we nonetheless looked inside and made a .zsync for
     * the uncompressed data, the user may want to actually have the client
     * have the compressed version once the whole operation is done. 
     * If so, if possible we want the compressed version that the client gets
     * to exactly match the original; but as the client will have to compress
     * it after completion of zsyncing, it might not be possible to achieve
     * that.
     * So a load of code here to work out whether (the client should)
     * recompress, what options it should use to do so, and to inform the
     * creator of the zsync if we don't think the recompression will work. 
     */

    /* The only danger of the client not getting the original file is if we have compressed;
     * in that case we want to recompress iff the compressed version was supplied
     * (i.e. we weren't told to generate it ourselves with -z). */
    if (do_exact) {
        int old_do_recompress = do_recompress;
        do_recompress = (zmapentries && !do_compress) ? 2 : 0;
        if (old_do_recompress != -1 && (!old_do_recompress) != (!do_recompress)) {
            fprintf(stderr,
                    "conflicting request for compression and exactness\n");
            exit(2);
        }
    }

    /* We recompress if we were told to, OR if
     *  we were left to make our own decision about recompression
     *  the original was compressed & the zsync is of the uncompressed (i.e. there is a zmap)
     *  AND this compressed original isn't one we made ourselves just for transmission
     */
    if ((do_recompress > 0)
        || (do_recompress == -1 && zmapentries && !do_compress))
        gzopts = guess_gzip_options(infname);
    /* We now know whether to recompress - if the above and guess_gzip_options worked */
    if (do_recompress == -1)
        do_recompress = (gzopts != NULL) ? 1 : 0;
    if (do_recompress > 1 && gzopts == NULL) {
        fprintf(stderr, "recompression required, but %s\n",
                zmap ?
                "could not determine gzip options to reproduce this archive" :
                "we are not looking into a compressed stream");
        exit(2);
    }

    /* Work out filename for the .zsync */
    if (fname && zmapentries) {
        /* Remove any trailing .gz, as it is the uncompressed file being transferred */
        char *p = strrchr(fname, '.');
        if (p) {
            zfname = strdup(fname);
            if (!strcmp(p, ".gz"))
                *p = 0;
            if (!strcmp(p, ".tgz"))
                strcpy(p, ".tar");
        }
    }
    if (!outfname && fname) {
        outfname = malloc(strlen(fname) + 10);
        sprintf(outfname, "%s.zsync", fname);
    }

    /* Open output file */
    if (outfname) {
        fout = fopen(outfname, "wb");
        if (!fout) {
            perror("open");
            exit(2);
        }
        free(outfname);
    }
    else {
        fout = stdout;
    }

    /* Okay, start writing the zsync file */
    fprintf(fout, "zsync: " VERSION "\n");

    /* Lines we might include but which older clients can ignore */
    if (do_recompress) {
        if (zfname)
            fprintf(fout, "Safe: Z-Filename Recompress MTime\nZ-Filename: %s\n",
                    zfname);
        else
            fprintf(fout, "Safe: Recompress MTime:\n");
    }

    if (fname) {
        fprintf(fout, "Filename: %s\n", fname);
        if (mtime != -1) {
            char buf[32];
            struct tm mtime_tm;

            if (gmtime_r(&mtime, &mtime_tm) != NULL) {
                if (strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %z", &mtime_tm) > 0)
                    fprintf(fout, "MTime: %s\n", buf);
            }
            else {
                fprintf(stderr, "error converting %d to struct tm\n", mtime);
            }
        }
    }
    fprintf(fout, "Blocksize: " SIZE_T_PF "\n", blocksize);
    fprintf(fout, "Length: " OFF_T_PF "\n", len);
    fprintf(fout, "Hash-Lengths: %d,%d,%d\n", seq_matches, rsum_len,
            checksum_len);
    {                           /* Write URLs */
        int i;
        for (i = 0; i < nurls; i++)
            fprintf(fout, "%s: %s\n", zmapentries ? "Z-URL" : "URL", url[i]);
        for (i = 0; i < nUurls; i++)
            fprintf(fout, "URL: %s\n", Uurl[i]);
    }
    if (nurls == 0 && infname) {
        /* Assume that we are in the public dir, and use relative paths.
         * Look for an uncompressed version and add a URL for that to if appropriate. */
        fprintf(fout, "%s: %s\n", zmapentries ? "Z-URL" : "URL", infname);
        if (zmapentries && fname && !access(fname, R_OK)) {
            fprintf(fout, "URL: %s\n", fname);
        }
        fprintf(stderr,
                "No URL given, so I am including a relative URL in the .zsync file - you must keep the file being served and the .zsync in the same public directory. Use -u %s to get this same result without this warning.\n",
                infname);
    }

    {   /* Write out SHA1 checksum of the entire file */
        unsigned char digest[SHA1_DIGEST_LENGTH];
        unsigned int i;

        fputs("SHA-1: ", fout);

        SHA1Final(digest, &shactx);

        for (i = 0; i < sizeof digest; i++)
            fprintf(fout, "%02x", digest[i]);
        fputc('\n', fout);
    }

    if (do_recompress)      /* Write Recompress header if wanted */
        fprintf(fout, "Recompress: %s %s\n", zhead, gzopts);
    if (gzopts)
        free(gzopts);

    /* If we have a zmap, write it, header first and then the map itself */
    if (zmapentries) {
        fprintf(fout, "Z-Map2: %d\n", zmapentries);
        fcopy(zmap, fout);
        fclose(zmap);
    }

    /* End of headers */
    fputc('\n', fout);

    /* Now copy the actual block hashes to the .zsync */
    rewind(tf);
    fcopy_hashes(tf, fout, rsum_len, checksum_len);

    /* And cleanup */
    fclose(tf);
    fclose(fout);

    return 0;
}
