/*
 *   zsync - client side rsync over http
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
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include <arpa/inet.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "zlib/zlib.h"

#include "librcksum/rcksum.h"
#include "zsync.h"
#include "sha1.h"
#include "zmap.h"

struct zsync_state {
  struct rcksum_state* rs;
  off_t filelen;
  int blocks;
  long blocksize;

  char* checksum;
  const char* checksum_method;

  char** url;
  int nurl;

  struct zmap* zmap;
  char** zurl;
  int nzurl;

  char* cur_filename; /* If we have taken the filename from rcksum, it is here */
  char* filename;     /* This is just the Filename: header from the .zsync */
  char* zfilename;    /* ditto Z-Filename: */

  char* gzopts;
  char* gzhead;
};

static char** append_ptrlist(int *n, char** p, char* a) {
  if (!a) return p;
  p = realloc(p,(*n + 1) * sizeof *p);
  if (!p) { fprintf(stderr,"out of memory\n"); exit(1); }
  p[*n] = a;
  (*n)++;
  return p;
}

struct zsync_state* zsync_begin(FILE* f)
{
  struct zsync_state* zs = calloc(sizeof *zs,1);
  int checksum_bytes = 16,rsum_bytes = 4,seq_matches=1;
  char* safelines = NULL;

  if (!zs) return NULL;

  for (;;) {
    char buf[1024];
    char *p = NULL;
    int l;

    if (fgets(buf, sizeof(buf), f) != NULL) {
      if (buf[0] == '\n') break;
      l = strlen(buf) - 1;
      while (l >= 0 && (buf[l] == '\n' || buf[l] == '\r' || buf[l] == ' '))
	buf[l--] = 0;

      p = strchr(buf,':');
    }
    if (p && *(p+1) == ' ') {
      *p++ = 0;
      p++;
      if (!strcmp(buf, "zsync")) {
	if (!strcmp(p,"0.0.4")) {
	  fprintf(stderr,"This version of zsync is not compatible with zsync 0.0.4 streams.\n");
	  free(zs); return NULL;
	}
      } else if (!strcmp(buf, "Min-Version")) {
	if (strcmp(p,VERSION) > 0) {
	  fprintf(stderr,"control file indicates that zsync-%s or better is required\n",p);
	  free(zs); return NULL;
	}
      } else if (!strcmp(buf, "Length")) {
	zs->filelen = atol(p);
      } else if (!strcmp(buf, "Filename")) {
	zs->filename = strdup(p);
      } else if (!strcmp(buf, "Z-Filename")) {
	zs->zfilename = strdup(p);
      } else if (!strcmp(buf, "URL")) {
	zs->url = (char**)append_ptrlist(&(zs->nurl), zs->url, strdup(p));
      } else if (!strcmp(buf, "Z-URL")) {
	zs->zurl = (char**)append_ptrlist(&(zs->nzurl), zs->zurl, strdup(p));
      } else if (!strcmp(buf, "Blocksize")) {
	zs->blocksize = atol(p);
	if (zs->blocksize < 0 || (zs->blocksize & (zs->blocksize-1))) {
	  fprintf(stderr,"nonsensical blocksize %ld\n",zs->blocksize); 
	  free(zs); return NULL;
	}
      } else if (!strcmp(buf, "Hash-Lengths")) {
        if (sscanf(p,"%d,%d,%d",&seq_matches,&rsum_bytes,&checksum_bytes) != 3 || rsum_bytes < 1 || rsum_bytes > 4 || checksum_bytes < 3 || checksum_bytes > 16 || seq_matches > 2 || seq_matches < 1) {
	  fprintf(stderr,"nonsensical hash lengths line %s\n",p);
	  free(zs); return NULL;
	}
      } else if (zs->blocks && !strcmp(buf,"Z-Map2")) {
	int nzblocks;
	struct gzblock* zblock;

	nzblocks = atoi(p);
	if (nzblocks < 0) {
	  fprintf(stderr,"bad Z-Map line\n");
	  free(zs); return NULL;
	}

	zblock = malloc(nzblocks * sizeof *zblock);
	if (zblock) {
	  if (fread(zblock,sizeof *zblock,nzblocks,f) < nzblocks) {
	    fprintf(stderr,"premature EOF after Z-Map\n"); 
	    free(zs); return NULL;
	  }

	  zs->zmap = zmap_make(zblock,nzblocks);
	  free(zblock);
	}
      } else if (!strcmp(buf,"SHA-1")) {
	zs->checksum = strdup(p);
	zs->checksum_method = "SHA-1";
      } else if (!strcmp(buf,"Safe")) {
	safelines = strdup(p);
      } else if (!strcmp(buf,"Recompress")) {
	zs->gzhead = strdup(p);
	if (zs->gzhead) {
	  char *q = strchr(zs->gzhead,' ');
	  if (!q)
	    q = zs->gzhead + strlen(zs->gzhead);
	  {
	    if (*q) *q++ = 0;
	    /* Whitelist for safe options for gzip command line */
	    if (!strcmp(q,"--best") || !strcmp(q,"--rsync --best") || !strcmp(q,"--rsync") || !strcmp(q,""))
	      zs->gzopts = strdup(q);
	    else {
	      fprintf(stderr,"bad recompress options, rejected\n");
	      free(zs->gzhead);
	    }
	  }
	}
      } else if (!safelines || !strstr(safelines,buf)) {
	fprintf(stderr,"unrecognised tag %s - you need a newer version of zsync.\n",buf);
	free(zs); return NULL;
      }
      if (zs->filelen && zs->blocksize)
	zs->blocks = (zs->filelen + zs->blocksize-1)/zs->blocksize;
    } else {
      fprintf(stderr, "Bad line - not a zsync file? \"%s\"\n", buf);
      free(zs); return NULL;
    }
  }
  if (!zs->filelen || !zs->blocksize) {
    fprintf(stderr,"Not a zsync file (looked for Blocksize and Length lines)\n");
    free(zs); return NULL;
  }
  if (!(zs->rs = rcksum_init(zs->blocks, zs->blocksize, rsum_bytes, checksum_bytes, seq_matches))) {
    free(zs); return NULL;
  }
  {
    zs_blockid id = 0;
    for (;id < zs->blocks; id++) {
      struct rsum r = { 0,0 };
      unsigned char checksum[CHECKSUM_SIZE];

      if (fread(((char*)&r)+4-rsum_bytes,rsum_bytes,1,f) < 1 || fread((void*)&checksum,checksum_bytes,1,f) < 1) {
	fprintf(stderr,"short read on control file; %s\n",strerror(ferror(f)));
	rcksum_end(zs->rs);
	free(zs); return NULL;
      }
      r.a = ntohs(r.a); r.b = ntohs(r.b);
      rcksum_add_target_block(zs->rs, id, r, checksum);
    }
  }
  return zs;
}

int zsync_hint_decompress(const struct zsync_state* zs)
{
  return (zs->nzurl > 0 ? 1 : 0);
}

int zsync_blocksize(struct zsync_state* zs)
{ return zs->blocksize; }

char* zsync_filename(const struct zsync_state* zs)
{ return strdup(zs->gzhead && zs->zfilename ? zs->zfilename : zs->filename); }

int zsync_status(struct zsync_state* zs)
{
  int todo = rcksum_blocks_todo(zs->rs);

  if (todo == zs->blocks) return 0;
  if (todo > 0) return 1;
  return 2; /* TODO: more? */
}

void zsync_progress(const struct zsync_state* zs, long long* got, long long* total)
{

  if (got) {
    int todo = zs->blocks - rcksum_blocks_todo(zs->rs);
    *got = todo * zs->blocksize;
  }
  if (total) *total = zs->blocks * zs->blocksize;
}

const char * const * zsync_get_urls(struct zsync_state* zs, int* n, int* t)
{
  if (zs->zmap && zs->nzurl) {
    *n = zs->nzurl; *t = 1;
    return zs->zurl;
  } else {
    *n = zs->nurl; *t = 0;
    return zs->url;
  }
}

off_t* zsync_needed_byte_ranges(struct zsync_state* zs, int* num, int type)
{
  int nrange;
  zs_blockid* blrange;
  off_t* byterange;
  int i;
  
  blrange = rcksum_needed_block_ranges(zs->rs, &nrange, 0, 0x7fffffff);
  if (!blrange) return NULL;

  byterange = malloc(2 * nrange * sizeof *byterange);
  if (!byterange) { free(blrange); return NULL; }

  for (i=0; i<nrange; i++) {
    byterange[2*i] = blrange[2*i] * zs->blocksize;
    byterange[2*i+1] = blrange[2*i+1] * zs->blocksize-1;
  }
  free(blrange);

  switch (type) {
  case 0:
    *num = nrange;
    return byterange;
  case 1:
    {
      off_t* zbyterange = zmap_to_compressed_ranges(zs->zmap, byterange, nrange, &nrange);
      if (zbyterange) {
	*num = nrange;
      }
      free(byterange);
      return zbyterange;
    }
  default:
    free(byterange); return NULL;
  }
}

int zsync_submit_source_file(struct zsync_state* zs, FILE* f, int progress)
{
  return rcksum_submit_source_file(zs->rs, f, progress);
}

char* zsync_cur_filename(struct zsync_state* zs)
{
  if (!zs->cur_filename)
    zs->cur_filename = rcksum_filename(zs->rs);

  return zs->cur_filename;
}

int zsync_rename_file(struct zsync_state* zs, const char* f)
{
  char* rf = zsync_cur_filename(zs);

  int x = rename(rf, f);

  if (!x) { free(rf); zs->cur_filename = strdup(f); }
  else perror("rename");

  return x;
}

static int hexdigit(char c) {
  return (isdigit(c) ? (c-'0') : isupper(c) ? (0xa + (c-'A')) : islower(c) ? (0xa + (c-'a')) : 0);
}

int zsync_complete(struct zsync_state* zs)
{
  int fh = rcksum_filehandle(zs->rs);
  int rc;

  zsync_cur_filename(zs);
  rcksum_end(zs->rs); zs->rs = NULL;
    
  if (ftruncate(fh,zs->filelen) != 0) { perror("ftruncate"); return -1; }
  if (lseek(fh,0,SEEK_SET) != 0) { perror("lseek"); return -1; }
  if (zs->checksum && !strcmp(zs->checksum_method,"SHA-1")) {
    SHA1_CTX shactx;

    if (strlen(zs->checksum) != SHA1_DIGEST_LENGTH*2) {
      fprintf(stderr,"SHA-1 digest from control file is wrong length.\n");
      return -1;
    }
    {
      char buf[4096];
      int rc;
      
      SHA1Init(&shactx);
      while (0 < (rc = read(fh,buf,sizeof buf))) {
	SHA1Update(&shactx,buf,rc);
      }
      if (rc < 0) { perror("read"); close(fh); return -1; }
    }
    close(fh);
    {
      unsigned char digest[SHA1_DIGEST_LENGTH];
      int i;

      SHA1Final(digest, &shactx);

      for (i=0; i<SHA1_DIGEST_LENGTH; i++) {
	int j;
	sscanf(&(zs->checksum[2*i]),"%2x",&j);
	if (j != digest[i]) {
	  return -1;
	}
      }
    }
    rc = 1;
  }
  else
  {
    rc = 0;
  }
  /* Recompression. This is a fugly mess, calling gzip on the temporary file with options
   *  read out of the .zsync, reading its output and replacing the gzip header. Ugh. */
  if (zs->gzhead && zs->gzopts) {
    FILE* g;
    FILE* zout;
    
    char cmd[1024];
    snprintf(cmd,sizeof(cmd),"gzip -n %s < ",zs->gzopts);
    
    {
      int i=0;
      int j = strlen(cmd);
      char c;
      
      while ((c = zs->cur_filename[i++]) != 0 && j < sizeof(cmd) - 2) {
	if (!isalnum(c)) cmd[j++] = '\\';
	cmd[j++] = c;
      }
      cmd[j] = 0;
    }
    
    g = popen(cmd,"r");
    if (g) {
      char zoname[1024];
      
      snprintf(zoname,sizeof(zoname),"%s.gz",zs->cur_filename);
      zout=fopen(zoname,"w");
      
      if (zout) {
	char *p = zs->gzhead;
	int skip = 1;
	
	while (p[0] && p[1]) {
	  if (fputc((hexdigit(p[0]) << 4)+hexdigit(p[1]), zout) == EOF) {
	    perror("putc"); rc = -1;
	  }
	  p+= 2;
	}
	while (!feof(g)) {
	  char buf[1024];
	  int r;
	  char *p = buf;
	  
	  if ((r = fread(buf,1,sizeof(buf),g)) < 0) {
	    perror("fread"); rc = -1; goto leave_it;
	  }
	  if (skip) { p = skip_zhead(buf); skip = 0; }
	  if (fwrite(p,1,r - (p-buf),zout) != r - (p-buf)) {
	    perror("fwrite"); rc = -1; goto leave_it;
	  }
	}
	
      leave_it:
	if (fclose(zout) != 0) { perror("close"); rc = -1; }
      }
      if (fclose(g) != 0) { perror("close"); rc = -1; }

      unlink(zs->cur_filename);
      free(zs->cur_filename);
      zs->cur_filename = strdup(zoname);
    } else {
      fprintf(stderr,"problem with gzip, unable to compress.\n");
    }
  }
  return rc;
}

char* zsync_end(struct zsync_state* zs)
{
  char *f = zsync_cur_filename(zs);

  if (zs->rs) rcksum_end(zs->rs);
  if (zs->zmap) zmap_free(zs->zmap);
  {
    int i;
    for (i=0; i<zs->nurl; i++) free(zs->url[i]);
    for (i=0; i<zs->nzurl; i++) free(zs->zurl[i]);
  }
  free(zs->url); free(zs->zurl); free(zs->checksum);
  free(zs->filename); free(zs->zfilename);
  free(zs);
  return f;
}

/* Functions for receiving data from supplied URLs below */

void zsync_configure_zstream_for_zdata(const struct zsync_state* zs, struct z_stream_s* zstrm, long zoffset, long long* poutoffset)
{
  configure_zstream_for_zdata(zs->zmap, zstrm, zoffset, poutoffset);
  { /* Load in prev 32k sliding window for backreferences */
    long long pos = *poutoffset;
    int lookback = (pos > 32768) ? 32768 : pos;
    char wbuf[32768];
    
    rcksum_read_known_data(zs->rs, wbuf, pos-lookback,lookback);
    /* Fake an output buffer of 32k filled with data to zlib */
    zstrm->next_out = wbuf+lookback; zstrm->avail_out = 0;
    updatewindow(zstrm,lookback);
  }
}

struct zsync_receiver {
  struct zsync_state* zs;
  struct z_stream_s strm;
  int url_type;
  char* outbuf;
  off_t outoffset;
};

static int zsync_submit_data(struct zsync_state* zs, unsigned char* buf, off_t offset, int blocks)
{
  zs_blockid blstart = offset / zs->blocksize;
  zs_blockid blend = blstart + blocks - 1;

  return rcksum_submit_blocks(zs->rs, buf, blstart, blend);
}

struct zsync_receiver* zsync_begin_receive(struct zsync_state*zs, int url_type)
{
  struct zsync_receiver* zr = malloc(sizeof(struct zsync_receiver));

  if (!zr) return NULL;
  zr->zs = zs;

  zr->outbuf = malloc(zs->blocksize);
  if (!zr->outbuf) { free(zr); return NULL; }

  /* Set up new inflate object */
  zr->strm.zalloc = Z_NULL; zr->strm.zfree = Z_NULL; zr->strm.opaque = NULL;
  zr->strm.total_in = 0;

  zr->url_type = url_type;
  zr->outoffset = 0;

  return zr;
}

int zsync_receive_data(struct zsync_receiver* zr, unsigned char* buf, off_t offset, size_t len)
{
  int blocksize = zr->zs->blocksize;

  if (zr->url_type == 1) { 
    int ret=0;
    int eoz=0;

    if (!len) return 0;

    /* Now set up for the downloaded block */
    zr->strm.next_in = buf; zr->strm.avail_in = len;
    
    if (zr->strm.total_in == 0 || offset != zr->strm.total_in) {
      zsync_configure_zstream_for_zdata(zr->zs, &(zr->strm), offset, &(zr->outoffset));
      
      /* On first iteration, we might be reading an incomplete block from zsync's point of view. Limit avail_out so we can stop after doing that and realign with the buffer. */
      zr->strm.avail_out = blocksize - (zr->outoffset % blocksize);
      zr->strm.next_out = zr->outbuf;
    } else {
      if (zr->outoffset == -1) { fprintf(stderr,"data didn't align with block boundary in compressed stream\n"); return 1; }
      zr->strm.next_in = buf; zr->strm.avail_in = len;
    }
    
    while (zr->strm.avail_in && !eoz) {
      int rc;
      
      /* Read in up to the next block (in the libzsync sense on the output stream) boundary */

      rc = inflate(&(zr->strm),Z_SYNC_FLUSH);
      switch (rc) {
      case Z_STREAM_END: eoz = 1;
      case Z_BUF_ERROR:
      case Z_OK:
	if (zr->strm.avail_out == 0 || eoz) {
	  /* If this was at the start of a block, try submitting it */
	  if (!(zr->outoffset % blocksize)) {
	    int rc;
	    
	    if (zr->strm.avail_out) memset(zr->strm.next_out,0,zr->strm.avail_out);
	    rc = zsync_submit_data(zr->zs, zr->outbuf, zr->outoffset, 1);
	    if (!zr->strm.avail_out) ret |= rc;
	    zr->outoffset += blocksize;
	  } else {
	    /* We were reading a block fragment; update outoffset, and we are nwo block-aligned. */
	    zr->outoffset += (((char*)(zr->strm.next_out)) - (zr->outbuf));
	  }
	  zr->strm.avail_out = blocksize; zr->strm.next_out = zr->outbuf;
	}
	break;
      default:
	fprintf(stderr,"zlib error: %s (%d)\n",zr->strm.msg, rc);
	eoz=1; ret = -1; break;
      }
    }
    return ret;
  } else {
    int ret = 0;

    if (0 != (offset % blocksize)) {
      size_t x = len;

      if (x > blocksize - (offset % blocksize)) x = blocksize - (offset % blocksize);

      if (zr->outoffset == offset) {
	/* Half-way through a block, so let's try and complete it */
	if (len)
	  memcpy(zr->outbuf + offset % blocksize, buf, x);
	else {
	  // Pad with 0s to length.
	  memset(zr->outbuf + offset % blocksize, 0, len = x = blocksize - (offset % blocksize));
	}

	if ( (x + offset) % blocksize == 0)
	  if (zsync_submit_data(zr->zs, zr->outbuf, zr->outoffset + x - blocksize, blocksize))
	    ret = 1;
      }
      buf += x; len -= x; offset += x;
      if (!len) return 0;
    }

    /* Now we are block-aligned */
    if (len >= blocksize) {
      int w = len / blocksize;

      if (zsync_submit_data(zr->zs, buf, offset, w))
	ret = 1;

      w *= blocksize;
      buf += w; len -= w; offset += w;
      
    }
    /* Store incomplete block */
    if (len) {
      memcpy(zr->outbuf, buf, len);
      offset += len; /* not needed: buf += len; len -= len; */
    }

    zr->outoffset = offset;
    return ret;
  }  
}

void zsync_end_receive(struct zsync_receiver* zr)
{
  if (zr->strm.total_in > 0) { inflateEnd(&(zr->strm)); }
  free(zr->outbuf);
  free(zr);
}

