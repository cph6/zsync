/*
 *   rcksum/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004,2005,2007 Colin Phipps <cph@moria.org.uk>
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "md4.h"
#include "rcksum.h"
#include "internal.h"

#define UPDATE_RSUM(a, b, oldc, newc, bshift) do { (a) += ((unsigned char)(newc)) - ((unsigned char)(oldc)); (b) += (a) - ((oldc) << (bshift)); } while (0)

struct rsum  __attribute__((pure)) rcksum_calc_rsum_block(const unsigned char* data, size_t len)
{
  register unsigned short a = 0;
  register unsigned short b = 0;

  {
    while (len) {
      unsigned char c = *data++;
      a += c; b += len*c;
      len--;
    }
  }
  {
    struct rsum r = { a, b };
    return r;
  }
}

void rcksum_calc_checksum(unsigned char *c, const unsigned char* data, size_t len)
{
  MD4_CTX ctx;
  MD4Init(&ctx);
  MD4Update(&ctx,data,len);
  MD4Final(c,&ctx);
}

static void unlink_block(struct rcksum_state* z, zs_blockid id)
{
  struct hash_entry* t = &(z->blockhashes[id]);

  struct hash_entry** p = &(z->rsum_hash[calc_rhash(z,t) & z->hashmask]);

  while (*p != NULL) {
    if (*p == t) {
      if (t == z->rover) { z->rover = t->next; }
      *p = (*p)->next;
      return;
    } else {
      p = &((*p)->next);
    }
  }
}

#ifndef HAVE_PWRITE
ssize_t pwrite(int d, const void* buf, size_t nbytes, off_t offset)
{
  if (lseek(d, offset, SEEK_SET) == -1) return -1;
  return write(d, buf, nbytes);
}
#endif

static void write_blocks(struct rcksum_state* z, const unsigned char* data, zs_blockid bfrom, zs_blockid bto)
{
  off_t len = ((off_t)(bto - bfrom + 1)) << z->blockshift;
  off_t offset = ((off_t)bfrom) << z->blockshift;

  while (len) {
    size_t l = len;
    int rc;

    if ((off_t)l < len) l = 0x8000000;

    rc = pwrite(z->fd,data,l,offset);
    
    if (rc == -1) {
      fprintf(stderr, "IO error: %s\n", strerror(errno));
      exit(-1);
    }
    len -= rc;
    if (len) { /* Incomplete, must advance */
      data += rc;
      offset += rc;
    }
  }
  {
    int id;
    for (id = bfrom; id <= bto; id++) {
      unlink_block(z, id);
      add_to_ranges(z, id);
    }
  }
}

int rcksum_read_known_data(struct rcksum_state* z, unsigned char* buf, off_t offset, size_t len)
{
  int rc = pread(z->fd,buf,len,offset);
  return rc;
}

int rcksum_submit_blocks(struct rcksum_state* const z, unsigned char* data, zs_blockid bfrom, zs_blockid bto)
{
  zs_blockid x;
  unsigned char md4sum[CHECKSUM_SIZE];

  if (!z->rsum_hash)
    if (!build_hash(z))
      return -1;

  for (x = bfrom; x <= bto; x++) {
    rcksum_calc_checksum(&md4sum[0], data + ((x-bfrom) << z->blockshift), z->blocksize);
    if (memcmp(&md4sum, &(z->blockhashes[x].checksum[0]), z->checksum_bytes)) {
      if (x > bfrom) /* Write any good blocks we did get */
	write_blocks(z,data,bfrom,x-1);
      return -1;
    }
  }
  write_blocks(z,data,bfrom,bto);
  return 0;
}

static int check_checksums_on_hash_chain(struct rcksum_state* const z, const struct hash_entry* e, const unsigned char* data, int onlyone)
{
  unsigned char md4sum[2][CHECKSUM_SIZE];
  signed int done_md4 = -1;
  int got_blocks = 0;
  register struct rsum r = z->r[0];

  z->rover = e;
  
  /* This is essentially a for (;e;e=e->next), but we want to remove links from
   * the list as we find matches, without keeping too many temp variables.
   */
  while (z->rover) {
    zs_blockid id;

    e = z->rover; z->rover = onlyone ? NULL : e->next;

    /* Check weak checksum first */
    
    z->stats.hashhit++;
    if (e->r.a != (r.a & z->rsum_a_mask) || e->r.b != r.b) {
      continue;
    }

    id = get_HE_blockid(z,e);

    if (!onlyone && z->seq_matches > 1 && (z->blockhashes[id+1].r.a != (z->r[1].a & z->rsum_a_mask) || z->blockhashes[id+1].r.b != z->r[1].b))
      continue;

    z->stats.weakhit++;

    {
      int ok = 1;
      signed int check_md4 = 0;

      /* This block at least must match; we must match at least z->seq_matches-1 others, which could either be trailing stuff, or these could be preceding blocks that we have verified already. */
      do {
	/* We only calculate the MD4 once we need it; but need not do so twice */
	if (check_md4 > done_md4) {
	  rcksum_calc_checksum(&md4sum[check_md4][0], data + z->blocksize*check_md4, z->blocksize);
	  done_md4 = check_md4;
	  z->stats.checksummed++; 
	}

	/* Now check the strong checksum for this block */
	if (memcmp(&md4sum[check_md4], z->blockhashes[id+check_md4].checksum, z->checksum_bytes))
	  ok = 0;

	check_md4++;
      } while (ok && !onlyone && check_md4 < z->seq_matches);

      if (ok) {
	write_blocks(z, data, id, id+check_md4-1);
	got_blocks+=check_md4;
	z->stats.stronghit+=check_md4;
	z->next_match = z->blockhashes + id+check_md4;
      }
    }
  }
  return got_blocks;
}

int rcksum_submit_source_data(struct rcksum_state* const z, unsigned char* data, size_t len, off_t offset)
{
  int x = 0;
  register int bs = z->blocksize;
  int got_blocks = 0;

  if (offset) {
    x = z->skip;
  } else {
    z->next_match = NULL;
  }

  if (x || !offset) {
    z->r[0] = rcksum_calc_rsum_block(data+x, bs);
    if (z->seq_matches > 1)
      z->r[1] = rcksum_calc_rsum_block(data+x+bs, bs);
  }
  z->skip = 0;

  for (;;) {
    if (x+z->context == len) {
      return got_blocks;
    }
    
#if 0
    {
      int k = 0;
      struct rsum c = rcksum_calc_rsum_block(data+x+bs*k, bs);
      if (c.a != z->r[k].a || c.b != z->r[k].b) {
	fprintf(stderr,"rsum miscalc (%d) at %lld\n",k,offset+x);
	exit(3);
      }
    }
#endif

    {
      int thismatch = 0;
      int blocks_matched = 0;

      if (z->next_match && z->seq_matches > 1) {
	if (0 != (thismatch = check_checksums_on_hash_chain(z, z->next_match, data+x, 1))) {
	  blocks_matched = 1;
	} else
	  z->next_match = NULL;
      }
      if (!blocks_matched) {
	const struct hash_entry* e;
	
	unsigned hash = z->r[0].b;
	hash ^= ((z->seq_matches > 1) ? z->r[1].b : z->r[0].a) << BITHASHBITS;
	e = z->rsum_hash[hash & z->hashmask];
	
	if ((z->bithash[(hash & z->bithashmask) >> 3] & (1 << (hash & 7))) != 0
	    &&(e = z->rsum_hash[hash & z->hashmask]) != NULL) {
	  thismatch = check_checksums_on_hash_chain(z, e, data+x, 0);
	  if (thismatch)
	    blocks_matched = z->seq_matches;
	}
      }
      got_blocks += thismatch;
	
      if (blocks_matched) {
	x += bs + (blocks_matched > 1 ? bs : 0);

	if (x + z->context > len) {
	  /* can't calculate rsum for block after this one, because it's not in the buffer. So leave a hint for next time so we know we need to recalculate */
	  z->skip = x + z->context - len;
	  return got_blocks;
	}
	
	  /* If we are moving forward just 1 block, we already have the following block rsum. If we are skipping both, then recalculate both */
	if (z->seq_matches > 1 && blocks_matched == 1)
	  z->r[0] = z->r[1];
	else 
	  z->r[0] = rcksum_calc_rsum_block(data+x, bs);
	
	if (z->seq_matches > 1)
	  z->r[1] = rcksum_calc_rsum_block(data+x+bs, bs);
	continue;
      }
    }

    {
      unsigned char Nc = data[x+bs*2];
      unsigned char nc = data[x+bs];
      unsigned char oc = data[x];
      UPDATE_RSUM(z->r[0].a,z->r[0].b,oc,nc,z->blockshift);
      if (z->seq_matches > 1)
	UPDATE_RSUM(z->r[1].a,z->r[1].b,nc,Nc,z->blockshift);
    }
    x++;
  }
}

int rcksum_submit_source_file(struct rcksum_state* z, FILE* f, int progress)
{
  register int bufsize = z->blocksize*16;
  unsigned char *buf = malloc(bufsize + z->context);
  int got_blocks = 0;
  off_t in = 0;
  int in_mb = 0;

  if (!buf) return 0;

  if (!z->rsum_hash)
    if (!build_hash(z))
      return 0;

  while (!feof(f)) {
    size_t len;
    off_t start_in = in;

    if (!in) {
      len = fread(buf,1,bufsize,f);
      if (len < z->context) return 0;
      in += len;
    } else {
      memcpy(buf, buf + (bufsize - z->context), z->context);
      in += bufsize - z->context;
      len = z->context + fread(buf + z->context,1,bufsize - z->context,f);
    }
    if (ferror(f)) {
      perror("fread"); free(buf);
      return got_blocks;
    }
    if (feof(f)) { /* 0 pad to complete a block */
      memset(buf+len,0,z->context); len += z->context;
    }
    got_blocks += rcksum_submit_source_data(z,buf,len,start_in);
    if (progress && in_mb != in / 1000000) { in_mb = in / 1000000; fputc('*',stderr); }
  }
  free(buf);
  return got_blocks;
}





