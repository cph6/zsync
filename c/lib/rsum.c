/*
 *   zsync/lib - library for using the rsync algorithm to determine
 *               which parts of a file you have and which you need.
 *   Copyright (C) 2004 Colin Phipps <cph@moria.org.uk>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.

 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* For pread/pwrite */
#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <openssl/md4.h>

#include "zsync.h"
#include "internal.h"

#define UPDATE_RSUM(a, b, oldc, newc, bshift) do { (a) += ((unsigned char)(newc)) - ((unsigned char)(oldc)); (b) += (a) - ((oc) << (bshift)); } while (0)

struct rsum  __attribute__((pure)) calc_rsum_block(const unsigned char* data, size_t len)
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

void calc_checksum(unsigned char *c, const unsigned char* data, size_t len)
{
  MD4(data,len,c);
}

static void write_blocks(struct zsync_state* z, unsigned char* data, zs_blockid bfrom, zs_blockid bto)
{
  long long len = ((long long)(bto - bfrom + 1)) << z->blockshift;
  off_t offset = ((long long)bfrom) << z->blockshift;

  while (len) {
    size_t l = len;
    int rc;

    if ((long long)l < len) l = 0x8000000;

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
    for (id = bfrom; id <= bto; id++)
      add_to_ranges(z, id);
  }
}

int read_known_data(struct zsync_state* z, unsigned char* buf, long long offset, size_t len)
{
  int rc = pread(z->fd,buf,len,offset);
  return rc;
}

int submit_blocks(struct zsync_state* z, unsigned char* data, zs_blockid bfrom, zs_blockid bto)
{
  zs_blockid x;
  unsigned char md4sum[CHECKSUM_SIZE];

  for (x = bfrom; x <= bto; x++) {
    calc_checksum(&md4sum[0], data + ((x-bfrom) << z->blockshift), z->blocksize);
    if (memcmp(&md4sum, &(z->blockhashes[x].checksum[0]), CHECKSUM_SIZE)) {
      if (x > bfrom) /* Write any good blocks we did get */
	write_blocks(z,data,bfrom,x-1);
      return -1;
    }
  }
  write_blocks(z,data,bfrom,bto);
  return 0;
}

static int check_checksums_on_hash_chain(struct zsync_state* z, struct hash_entry* e, const char* data, struct rsum r)
{
  unsigned char md4sum[CHECKSUM_SIZE];
  int done_md4 = 0;
  int got_blocks = 0;
  struct hash_entry* pprev = NULL;
  
  /* This is essentially a for (;e;e=e->next), but we want to remove links from
   * the list as we find matches, without kepeing too many temp variables. So
   * every code path in the loop has to update e to e->next, but in its own 
   * way. */
  while (e) {
    /* Check weak checksum first */
    
    if (e->r.a != r.a || e->r.b != r.b) {
      pprev = e; e = e->next;
      continue;
    }
    
    /* We only calculate the MD4 once we need it; but need not do so twice */
    if (!done_md4) {
      calc_checksum(&md4sum[0], data, z->blocksize);
      done_md4 = 1;
    }
    
    /* Now check the strong checksum for this block */
    if (!memcmp(&md4sum, e->checksum, sizeof e->checksum)) {
      zs_blockid id = get_HE_blockid(z,e);
    
      write_blocks(z, data, id, id);
      got_blocks++;

      /* Delink this entry.
       * pprev need not advance (prev entry remains the same).
       * If pprev, we have an entry before us, so just change its next ptr.
       * else, we are first link in the chain, so update the hash table itself
       */
      if (pprev) {
	pprev->next = e = e->next;
      } else {
	z->rsum_hash[calc_rhash(z,r)] = e = e->next;
      }
    } else {
      pprev = e; e = e->next;
      continue;      
    }
  }
  return got_blocks;
}

int submit_source_data(struct zsync_state* z, unsigned char* data, size_t len, long long offset)
{
  int x = 0;
  struct rsum r;
  register int bs = z->blocksize;
  int got_blocks = 0;

  if (offset) {
    r = z->current_rsum;
    x = z->skip;
  }
  if (x || !offset) {
    r = calc_rsum_block(data+x, bs);
  }
  z->skip = 0;

  for (;;) {
#if 0
    {
      struct rsum c = calc_rsum_block(data+x, bs);
      if (c.a != r.a || c.b != r.b) {
	fprintf(stderr,"rsum miscalc at %lld\n",offset+x);
	exit(3);
      }
    }
#endif
    {
      const struct hash_entry* e = get_first_hash_entry(z, r);
      if (e) {
	int thismatch = check_checksums_on_hash_chain(z, e, data+x, r);
	got_blocks += thismatch;
	if (thismatch) {
	  x += bs;
	  if (x+bs > len) {
	    /* can't calculate rsum for block after this one, because it's not in the buffer. So leave a hint for next time so we kow we need to recalculate */
	    z->skip = x+bs-len;
	    return got_blocks;
	  }
	  r = calc_rsum_block(data+x, bs);
	  continue;
	}
      }
    }

    if (x+bs == len) {
      z->current_rsum = r;
      return got_blocks;
    }
    
    {
      unsigned char nc = data[x+bs];
      unsigned char oc = data[x];
      UPDATE_RSUM(r.a,r.b,oc,nc,z->blockshift);
    }
    x++;
  }
}

int submit_source_file(struct zsync_state* z, FILE* f)
{
  register int bufsize = z->blocksize*16;
  char *buf = malloc(bufsize + z->blocksize);
  int got_blocks = 0;
  long long in = 0;
  int in_mb = 0;

  if (!buf) return 0;

  while (!feof(f)) {
    size_t len;
    long long start_in = in;

    if (!in) {
      len = fread(buf,1,bufsize,f);
      if (len < z->blocksize) return 0;
      in += len;
    } else {
      memcpy(buf, buf + (bufsize - z->blocksize), z->blocksize);
      in += bufsize - z->blocksize;
      len = z->blocksize + fread(buf + z->blocksize,1,bufsize - z->blocksize,f);
    }
    if (ferror(f)) {
      perror("fread"); free(buf);
      return got_blocks;
    }
    if (feof(f)) { /* 0 pad to complete a block */
      memset(buf+len,0,z->blocksize); len += z->blocksize;
    }
    got_blocks += submit_source_data(z,buf,len,start_in);
    if (in_mb != in / 1000000) { in_mb = in / 1000000; fputc('*',stderr); }
  }
  free(buf);
  return got_blocks;
}





