
/*
 *   rcksum/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
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

/* This is the core of the rsync rolling checksum algorithm - this is what it's
 * all about. */

#include "zsglobal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "md4.h"
#include "rcksum.h"
#include "internal.h"
/* TODO: decide how to handle progress; this is now being used by the client
 * and by the library, which is ugly. */
#include "../progress.h"

#define UPDATE_RSUM(a, b, oldc, newc, bshift) do { (a) += ((unsigned char)(newc)) - ((unsigned char)(oldc)); (b) += (a) - ((oldc) << (bshift)); } while (0)

/* rcksum_calc_rsum_block(data, data_len)
 * Calculate the rsum for a single block of data. */
/* Note int len here, not size_t, because the compiler is stupid and expands
 * the 32bit size_t to 64bit inside the inner loop. */
struct rsum __attribute__((pure))
rcksum_calc_rsum_block(const unsigned char *data, size_t len) {
    register unsigned short a = 0;
    register unsigned short b = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = data[i];
        a += c;
        b += a;
    }
    {
        struct rsum r = { a, b };
        return r;
    }
}

/* rcksum_calc_checksum(checksum_buf, data, data_len)
 * Returns the MD4 checksum (in checksum_buf) of the given data block */
void rcksum_calc_checksum(unsigned char *c, const unsigned char *data,
                          size_t len) {
    MD4_CTX ctx;
    MD4Init(&ctx);
    MD4Update(&ctx, data, len);
    MD4Final(c, &ctx);
}

#ifndef HAVE_PWRITE
/* Fallback pwrite(2) implementation if needed (but not strictly complete, as
 * it moves the file pointer - we don't care). */
ssize_t pwrite(int d, const void *buf, size_t nbytes, off_t offset) {
    if (lseek(d, offset, SEEK_SET) == -1)
        return -1;
    return write(d, buf, nbytes);
}
#endif

/* write_blocks(rcksum_state, buf, startblock, endblock)
 * Writes the block range (inclusive) from the supplied buffer to our
 * under-construction output file */
static void write_blocks(struct rcksum_state *z, const unsigned char *data,
                         zs_blockid bfrom, zs_blockid bto) {
    off_t len = ((off_t) (bto - bfrom + 1)) << z->blockshift;
    off_t offset = ((off_t) bfrom) << z->blockshift;

    while (len) {
        size_t l = (size_t)len;
        ssize_t rc;

        /* On some platforms, the bytes-to-write could be more than pwrite(2)
         * will accept. Write in blocks of 2^31 bytes in that case. */
        if ((off_t) l < len)
            l = 0x8000000;

        /* Write */
        rc = pwrite(z->fd, data, l, offset);
        if (rc == -1) {
            fprintf(stderr, "IO error: %s\n", strerror(errno));
            exit(-1);
        }

        /* Keep track of any data still to do */
        len -= rc;
        if (len) {              /* More to write */
            data += rc;
            offset += rc;
        }
    }

    {   /* Having written those blocks, discard them from the rsum hashes (as
         * we don't need to identify data for those blocks again, and this may
         * speed up lookups (in particular if there are lots of identical
         * blocks), and add the written blocks to the record of blocks that we
         * have received and stored the data for */
        int id;
        for (id = bfrom; id <= bto; id++) {
            remove_block_from_hash(z, id);
            add_to_ranges(z, id);
        }
    }
}

/* rcksum_read_known_data(self, buf, offset, len)
 * Read back data from the working output - read len bytes from offset into
 * buf[] (which must be at least len bytes long) */
ssize_t rcksum_read_known_data(struct rcksum_state *z, unsigned char *buf,
                               off_t offset, size_t len) {
    ssize_t rc = pread(z->fd, buf, len, offset);
    return rc;
}

/* rcksum_submit_blocks(self, data, startblock, endblock)
 * The data in data[] (which should be (endblock - startblock + 1) * blocksize * bytes)
 * is tested block-by-block as valid data against the target checksums for
 * those blocks and, if valid, accepted and written to the working output.
 *
 * Use this when you have obtained data that you know corresponds to given
 * blocks in the output file (i.e. you've downloaded them from a real copy of
 * the target).
 */
int rcksum_submit_blocks(struct rcksum_state *const z, const unsigned char *data,
                         zs_blockid bfrom, zs_blockid bto) {
    zs_blockid x;
    unsigned char md4sum[CHECKSUM_SIZE];

    /* Build checksum hash tables if we don't have them yet */
    if (!z->rsum_hash)
        if (!build_hash(z))
            return -1;

    /* Check each block */
    for (x = bfrom; x <= bto; x++) {
        rcksum_calc_checksum(&md4sum[0], data + ((x - bfrom) << z->blockshift),
                             z->blocksize);
        if (memcmp(&md4sum, &(z->blockhashes[x].checksum[0]), z->checksum_bytes)) {
            if (x > bfrom)      /* Write any good blocks we did get */
                write_blocks(z, data, bfrom, x - 1);
            return -1;
        }
    }

    /* All blocks are valid; write them and update our state */
    write_blocks(z, data, bfrom, bto);
    return 0;
}

/* check_checksums_on_hash_chain(self, &hash_entry, data[], onlyone)
 * Given a hash table entry, check the data in this block against every entry
 * in the linked list for this hash entry, checking the checksums for this
 * block against those recorded in the hash entries.
 *
 * If we get a hit (checksums match a desired block), write the data to that
 * block in the target file and update our state accordingly to indicate that
 * we have got that block successfully.
 *
 * Return the number of blocks successfully obtained.
 */
static int check_checksums_on_hash_chain(struct rcksum_state *const z,
                                         const struct hash_entry *e,
                                         const unsigned char *data,
                                         int onlyone) {
    unsigned char md4sum[2][CHECKSUM_SIZE];
    signed int done_md4 = -1;
    int got_blocks = 0;
    register struct rsum r = z->r[0];

    /* This is a hint to the caller that they should try matching the next
     * block against a particular hash entry (because at least z->seq_matches
     * prior blocks to it matched in sequence). Clear it here and set it below
     * if and when we get such a set of matches. */
    z->next_match = NULL;

    /* This is essentially a for (;e;e=e->next), but we want to remove links from
     * the list as we find matches, without keeping too many temp variables.
     */
    z->rover = e;
    while (z->rover) {
        zs_blockid id;

        e = z->rover;
        z->rover = onlyone ? NULL : e->next;

        /* Check weak checksum first */

        z->stats.hashhit++;
        if (e->r.a != (r.a & z->rsum_a_mask) || e->r.b != r.b) {
            continue;
        }

        id = get_HE_blockid(z, e);

        if (!onlyone && z->seq_matches > 1
            && (e[1].r.a != (z->r[1].a & z->rsum_a_mask)
                || e[1].r.b != z->r[1].b)) {
            continue;
        }

        z->stats.weakhit++;

        {
            int ok = 1;
            signed int check_md4 = 0;

            /* This block at least must match; we must match at least
             * z->seq_matches-1 others, which could either be trailing stuff,
             * or these could be preceding blocks that we have verified
             * already. */
            do {
                /* We only calculate the MD4 once we need it; but need not do so twice */
                if (check_md4 > done_md4) {
                    rcksum_calc_checksum(&md4sum[check_md4][0],
                                         data + z->blocksize * check_md4,
                                         z->blocksize);
                    done_md4 = check_md4;
                    z->stats.checksummed++;
                }

                /* Now check the strong checksum for this block */
                if (memcmp(&md4sum[check_md4],
                     e[check_md4].checksum,
                     z->checksum_bytes))
                    ok = 0;

                check_md4++;
            } while (ok && !onlyone && check_md4 < z->seq_matches);

            if (ok) {
                int num_write_blocks;

                /* Find the next block that we already have data for. If this
                 * is part of a run of matches then we have this stored already
                 * as ->next_known. */
                zs_blockid next_known = onlyone ? z->next_known : next_known_block(z, id);

                z->stats.stronghit += check_md4;

                if (next_known > id + check_md4) {
                    num_write_blocks = check_md4;

                    /* Save state for this run of matches */
                    z->next_match = &(z->blockhashes[id + check_md4]);
                    if (!onlyone) z->next_known = next_known;
                }
                else {
                    /* We've reached the EOF, or data we already know. Just
                     * write out the blocks we don't know, and that's the end
                     * of this run of matches. */
                    num_write_blocks = next_known - id;
                }

                /* Write out the matched blocks that we don't yet know */
                write_blocks(z, data, id, id + num_write_blocks - 1);
                got_blocks += num_write_blocks;
            }
        }
    }
    return got_blocks;
}

/* rcksum_submit_source_data(self, data, datalen, offset)
 * Reads the supplied data (length datalen) and identifies any contained blocks
 * of data that can be used to make up the target file.
 *
 * offset should be 0 for a new data stream (or if our position in the data
 * stream has been changed and does not match the last call) or should be the
 * offset in the whole source stream otherwise.
 *
 * Returns the number of blocks in the target file that we obtained as a result
 * of reading this buffer. 
 *
 * IMPLEMENTATION:
 * We maintain the following state:
 * skip - the number of bytes to skip next time we enter rcksum_submit_source_data
 *        e.g. because we've just matched a block and the forward jump takes 
 *        us past the end of the buffer
 * r[0] - rolling checksum of the first blocksize bytes of the buffer
 * r[1] - rolling checksum of the next blocksize bytes of the buffer (if seq_matches > 1)
 */
int rcksum_submit_source_data(struct rcksum_state *const z, unsigned char *data,
                              size_t len, off_t offset) {
    /* The window in data[] currently being considered is [x, x+bs) */
    int x = 0;
    int got_blocks = 0;  /* Count the number of useful data blocks found. */

    /* z->context doesn't vary during an invocation; help the compiler by
     * putting it into a local variable here. */
    register const int x_limit = len - z->context;

    if (offset) {
        x = z->skip;
    }
    else {
        z->next_match = NULL;
    }

    if (x || !offset) {
        z->r[0] = rcksum_calc_rsum_block(data + x, z->blocksize);
        if (z->seq_matches > 1)
            z->r[1] = rcksum_calc_rsum_block(data + x + z->blocksize, z->blocksize);
    }
    z->skip = 0;

    /* Work through the block until the current z->context bytes being
     * considered, starting at x, is at or past the end of the buffer */

    /* The loop is split into an outer and an inner loop here. Both are looping
     * over the data in the buffer; the inner loop is not strictly necessary but
     * makes it clearer what is the critical code path i.e. stepping through the
     * buffer one byte at a time. When we find a matching block, we skip forward
     * by a whole block - the outer loop handles this case. */
    while (x < x_limit /* which is len - z->context */) {
        /* # of blocks to advance if thismatch > 0. Can be less than
         * thismatch as thismatch could be N*blocks_matched, if a block was
         * duplicated to multiple locations in the output file. */
        int blocks_matched = 0; 

        /* Pull some invariants into locals, because the compiler doesn't
         * know they are invariants. */
        register const int seq_matches = z->seq_matches;
        register const size_t bs = z->blocksize;

        /* If the previous block was a match, but we're looking for
         * sequential matches, then test this block against the block in
         * the target immediately after our previous hit. */
        if (z->next_match && z->seq_matches > 1) {
            int thismatch;
            if (0 != (thismatch = check_checksums_on_hash_chain(z, z->next_match, data + x, 1))) {
                blocks_matched = 1;
                got_blocks += thismatch;
            }
        }

        /* If we already matched this block, we don't look it up in the hash
         * table at all.
         * Advance one byte at a time through the input stream, looking up the
         * rolling checksum in the rsum hash table. */
        while (0 == blocks_matched && x < x_limit) {
            /* # of blocks of the output file we got from this data */
            int thismatch = 0;

#if 0
            {   /* Catch rolling checksum failure */
                int k = 0;
                struct rsum c = rcksum_calc_rsum_block(data + x + bs * k, bs);
                if (c.a != z->r[k].a || c.b != z->r[k].b) {
                    fprintf(stderr, "rsum miscalc (%d) at %lld\n", k, offset + x);
                    exit(3);
                }
            }
#endif

            {
                const struct hash_entry *e;

                /* Do a hash table lookup - first in the bithash (fast negative
                 * check) and then in the rsum hash */
                unsigned hash = z->r[0].b;
                hash ^= ((seq_matches > 1) ? z->r[1].b
                        : z->r[0].a & z->rsum_a_mask) << z->hash_func_shift;

                if ((z->bithash[(hash & z->bithashmask) >> 3] & (1 << (hash & 7))) != 0
                    && (e = z->rsum_hash[hash & z->hashmask]) != NULL) {

                    /* Okay, we have a hash hit. Follow the hash chain and
                     * check our block against all the entries. */
                    thismatch = check_checksums_on_hash_chain(z, e, data + x, 0);
                    if (thismatch)
                        blocks_matched = seq_matches;
                }
            }
            got_blocks += thismatch;

            /* (If we didn't match any data) advance the window by 1 byte -
             * update the rolling checksum and our offset in the buffer */
            if (!blocks_matched) {
                unsigned char Nc = data[x + bs * 2];
                unsigned char nc = data[x + bs];
                unsigned char oc = data[x];
                UPDATE_RSUM(z->r[0].a, z->r[0].b, oc, nc, z->blockshift);
                if (seq_matches > 1)
                    UPDATE_RSUM(z->r[1].a, z->r[1].b, nc, Nc, z->blockshift);
                x++;
            }
        }

        /* If we got a hit, skip forward (if a block in the target matches
         * at x, it's highly unlikely to get a hit at x+1 as all the
         * target's blocks are multiples of the blocksize apart. */
        if (blocks_matched) {
            x += z->blocksize + (blocks_matched > 1 ? z->blocksize : 0);

            if (x > x_limit) {
                /* can't calculate rsum for block after this one, because
                 * it's not in the buffer. We will drop out of the loop and
                 * return. */
            } else {
                /* If we are moving forward just 1 block, we already have the
                 * following block rsum. If we are skipping both, then
                 * recalculate both */
                if (z->seq_matches > 1 && blocks_matched == 1)
                    z->r[0] = z->r[1];
                else
                    z->r[0] = rcksum_calc_rsum_block(data + x, z->blocksize);
                if (z->seq_matches > 1)
                    z->r[1] = rcksum_calc_rsum_block(data + x + z->blocksize, z->blocksize);
            }
        }
    }
    /* If we jumped to a point in the stream not yet in the buffer (x > x_limit)
     * then we need to save that state so that the next call knows where to
     * resume - and also so that the next call knows that it must calculate the
     * checksum of the first block because we do not have enough data to do so
     * right now. */
    z->skip = x - x_limit;

    /* Keep caller informed about how much useful data we are getting. */
    return got_blocks;
}

/* off_t get_file_size(FILE*)
 * Returns the size of the given file, if available. 0 otherwise.
 */
static off_t get_file_size(FILE* f) {
    struct stat st;
    int fd = fileno(f);
    if (fd == -1) return 0;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        return 0;
    }
    return st.st_size;
}

/* rcksum_submit_source_file(self, stream, progress)
 * Read the given stream, applying the rsync rolling checksum algorithm to
 * identify any blocks of data in common with the target file. Blocks found are
 * written to our working target output. Progress reports if progress != 0
 */
int rcksum_submit_source_file(struct rcksum_state *z, FILE * f, int progress) {
    /* Track progress */
    int got_blocks = 0;
    off_t in = 0;
    int in_mb = 0;
    off_t size = get_file_size(f);
    struct progress *p;

    /* Allocate buffer of 16 blocks */
    register size_t bufsize = z->blocksize * 16;
    unsigned char *buf = malloc(bufsize + z->context);
    if (!buf)
        return 0;

    /* Build checksum hash tables ready to analyse the blocks we find */
    if (!z->rsum_hash)
        if (!build_hash(z)) {
            free(buf);
            return 0;
        }

    if (progress) {
        p = start_progress();
        do_progress(p, 0, in);
    }

    while (!feof(f)) {
        size_t len;
        off_t start_in = in;

        /* If this is the start, fill the buffer for the first time */
        if (!in) {
            len = fread(buf, 1, bufsize, f);
            in += len;
        }

        /* Else, move the last context bytes from the end of the buffer to the
         * start, and refill the rest of the buffer from the stream. */
        else {
            memcpy(buf, buf + (bufsize - z->context), z->context);
            in += bufsize - z->context;
            len = z->context + fread(buf + z->context, 1, bufsize - z->context, f);
        }

        /* If either fread above failed, or EOFed */
        if (ferror(f)) {
            perror("fread");
            free(buf);
            if (progress)
                end_progress(p, 0);
            return got_blocks;
        }
        if (feof(f)) {          /* 0 pad to complete a block */
            memset(buf + len, 0, z->context);
            len += z->context;
        }

        /* Process the data in the buffer, and report progress */
        got_blocks += rcksum_submit_source_data(z, buf, len, start_in);
        if (progress && in_mb != in / 1000000) {
            do_progress(p, 100.0 * in / size, in);
            in_mb = in / 1000000;
        }
    }
    free(buf);
    if (progress) {
        end_progress(p, 2);
    }
    return got_blocks;
}
