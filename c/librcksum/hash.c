
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

/* Functions to manage the rsum and checksum values per block and set up the
 * hash tables of the rsum values. */

#include "zsglobal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "rcksum.h"
#include "internal.h"

/* rcksum_add_target_block(self, blockid, rsum, checksum)
 * Sets the stored hash values for the given blockid to the given values.
 */
void rcksum_add_target_block(struct rcksum_state *z, zs_blockid b,
                             struct rsum r, void *checksum) {
    if (b < z->blocks) {
        /* Get hash entry with checksums for this block */
        struct hash_entry *e = &(z->blockhashes[b]);

        /* Enter checksums */
        memcpy(e->checksum, checksum, z->checksum_bytes);
        e->r.a = r.a & z->rsum_a_mask;
        e->r.b = r.b;

        /* New checksums invalidate any existing checksum hash tables */
        if (z->rsum_hash) {
            free(z->rsum_hash);
            z->rsum_hash = NULL;
            free(z->bithash);
            z->bithash = NULL;
        }
    }
}

static void print_hashstats(const struct rcksum_state* z) {
#ifdef DEBUG
    int i;
    {
        int num_bits_set = 0;
        for (i = 0; i < z->bithashmask + 1; i++) {
            unsigned char c;
            for (c = z->bithash[i]; c; c &= c - 1)
                num_bits_set++;
        }
            
        fprintf(stderr, "bithash density %.1f%%\n",
                100.0 * num_bits_set / (z->bithashmask + 1));
    }
    {
        int hash_entries_used = 0;
        int max_depth = 0;
        for (i = 0; i < z->hashmask + 1; i++) {
            struct hash_entry* p = z->rsum_hash[i];
            if (!p) continue;
            hash_entries_used++;
            int depth;
            for (depth = 0; p; p = p->next)
                depth++;
            if (depth > max_depth) max_depth = depth;
        }
        fprintf(stderr,
                "rsum hash density: %.1f%% (depth avg: %.1f, max: %d)\n",
                100.0 * hash_entries_used / (z->hashmask + 1),
                z->blocks / (float)hash_entries_used, max_depth);
    }
#endif
}

/* build_hash(self)
 * Build hash tables to quickly lookup a block based on its rsum value.
 * Returns non-zero if successful.
 */
int build_hash(struct rcksum_state *z) {
    zs_blockid id;
    int i = 16;

    /* Try hash size of 2^i; step down the value of i until we find a good size
     */
    while ((2 << (i - 1)) > z->blocks && i > 4)
        i--;

    /* Allocate hash based on rsum */
    z->hashmask = (2 << i) - 1;
    z->rsum_hash = calloc(z->hashmask + 1, sizeof *(z->rsum_hash));
    if (!z->rsum_hash)
        return 0;

    /* Allocate bit-table based on rsum */
    z->bithashmask = (2 << (i + BITHASHBITS)) - 1;
    z->bithash = calloc(z->bithashmask + 1, 1);
    if (!z->bithash) {
        free(z->rsum_hash);
        z->rsum_hash = NULL;
        return 0;
    }

    /* Now fill in the hash tables.
     * Minor point: We do this in reverse order, because we're adding entries
     * to the hash chains by prepending, so if we iterate over the data in
     * reverse then the resulting hash chains have the blocks in normal order.
     * That's improves our pattern of I/O when writing out identical blocks
     * once we are processing data; we will write them in order. */
    for (id = z->blocks; id > 0;) {
        /* Decrement the loop variable here, and get the hash entry. */
        struct hash_entry *e = z->blockhashes + (--id);

        /* Prepend to linked list for this hash entry */
        unsigned h = calc_rhash(z, e);
        e->next = z->rsum_hash[h & z->hashmask];
        z->rsum_hash[h & z->hashmask] = e;

        /* And set relevant bit in the bithash to 1 */
        z->bithash[(h & z->bithashmask) >> 3] |= 1 << (h & 7);
    }

    print_hashstats(z);
    return 1;
}

/* remove_block_from_hash(self, block_id)
 * Remove the given data block from the rsum hash table, so it won't be
 * returned in a hash lookup again (e.g. because we now have the data)
 */

void remove_block_from_hash(struct rcksum_state *z, zs_blockid id) {
    struct hash_entry *t = &(z->blockhashes[id]);

    struct hash_entry **p = &(z->rsum_hash[calc_rhash(z, t) & z->hashmask]);

    while (*p != NULL) {
        if (*p == t) {
            if (t == z->rover) {
                z->rover = t->next;
            }
            *p = (*p)->next;
            return;
        }
        else {
            p = &((*p)->next);
        }
    }
}

