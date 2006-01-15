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

#include "zsync.h"
#include "internal.h"
#include "mdfour.h"

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
  struct mdfour MD;
  
  mdfour_begin(&MD);
  mdfour_update(&MD, data, len);
  mdfour_result(&MD, c);
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

int submit_source_data(struct zsync_state* z, unsigned char* data, size_t len, int current_rsum_valid) {
  int x = 0;
  struct rsum r;
  register int bs = z->blocksize;
  int got_blocks = 0;

  if (current_rsum_valid) {
    r = z->current_rsum;
  } else {
    r = calc_rsum_block(data, bs);
  }

  while (1) {
    {
      const struct hash_entry* e = get_first_hash_entry(z, r);
      if (e) {
	unsigned char md4sum[CHECKSUM_SIZE];
	int done_md4 = 0;
	
	for (;e;e = e->next) {
	  zs_blockid id;
	  /* Begin checksum and offer block */
	  /* Should probably be a separate function, but it's too fiddly to transfer all this state about which parts of which buffers we refer to to another place */

	  if (e->r.a != r.a || e->r.b != r.b) continue;

	  id = get_HE_blockid(z,e);

	  if (already_got_block(z, id)) continue;

	  /* We only calculate the MD4 once we need it; but need not do so twice */
	  if (!done_md4) {
	    calc_checksum(&md4sum[0], &data[x], bs);
	    done_md4 = 1;
	  }

	  /* Now check the strong checksum for this block */
	  if (!memcmp(&md4sum, e->checksum, sizeof e->checksum)) {
	    write_blocks(z,&data[x],id,id);
	    got_blocks++;
	  }

	  /* End checksum and offer block */
	}
      }
    }

    if (x == len-bs) {
      z->current_rsum = r;
      return got_blocks;
    }
    
    {
      unsigned char oc = data[x];
      unsigned char nc = data[x+bs];
      UPDATE_RSUM(r.a,r.b,oc,nc,z->blockshift);
    }
    x++;
  }
}

int submit_source_file(struct zsync_state* z, FILE* f)
{
  register int bufsize = z->blocksize*16;
  char *buf = malloc(bufsize);
  int first = 1;
  int got_blocks = 0;

  if (!buf) return 0;

  while (!feof(f)) {
    size_t len;
    if (first) {
      len = fread(buf,1,bufsize,f);
      first = 0;
      if (len < z->blocksize) return 0;
    } else {
      memcpy(buf, buf + (bufsize - z->blocksize), z->blocksize);
      len = z->blocksize + fread(buf,1,bufsize - z->blocksize,f);
    }
    if (ferror(f)) {
      perror("fread");
      return got_blocks;
    }
    got_blocks += submit_source_data(z,buf,len,!first);
  }
  return got_blocks;
}





