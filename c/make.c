/*
 *   zsyncmake - client side rsync over http, metafile builder
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* htons - where to get this? */
#ifdef HAVE_HTONS_IN_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_HTONS_IN_SYS_PARAM_H
#include <sys/param.h>
#endif

#include "zsync.h"

#include "zlib/zlib.h"

#ifdef HAVE_LIBCRYPTO

#include <openssl/sha.h>

SHA_CTX shactx;

#else

#define SHA1_Init(a)
#define SHA1_Update(a,b,c)

#endif

size_t blocksize = 1024;
long long len = 0;

void stream_error(const char* func, FILE* stream)
{
  fprintf(stderr,"%s: %s\n",func,strerror(ferror(stream)));
  exit(2);
}

static void write_block_sums(char* buf, size_t got, FILE* f)
{
  struct rsum r;
  unsigned char checksum[CHECKSUM_SIZE];
  /* Now pad for our checksum */
  if (got < blocksize)
    memset(buf+got,0,blocksize-got);
  
  r = calc_rsum_block(buf, blocksize);
  calc_checksum(&checksum[0], buf, blocksize);
  r.a = htons(r.a); r.b = htons(r.b);
  
  if (fwrite(&r, sizeof r, 1, f) != 1) stream_error("fwrite",f);
  if (fwrite(checksum, sizeof checksum, 1, f) != 1) stream_error("fwrite",f);
  
}

/* gzip flag byte */
#define ASCII_FLAG   0x01 /* bit 0 set: file probably ascii text */
#define HEAD_CRC     0x02 /* bit 1 set: header CRC present */
#define EXTRA_FIELD  0x04 /* bit 2 set: extra field present */
#define ORIG_NAME    0x08 /* bit 3 set: original file name present */
#define COMMENT      0x10 /* bit 4 set: file comment present */
#define RESERVED     0xE0 /* bits 5..7: reserved */

static inline long long in_position(z_stream* pz)
{ return pz->total_in * 8 - ( 63 & pz->data_type); }

static FILE* zmap;
static int zmapentries;
static long long last_delta_in;

static void write_zmap_delta(long long *prev_in, long long *prev_out, long long new_in, long long new_out, int blockstart)
{
  {
    long inbits = new_in - *prev_in;

    inbits &= 0x7fffffff;
    if (*prev_in + inbits != new_in) { fprintf(stderr,"too long between blocks?"); exit(1); }
    if (!blockstart) inbits |= 0x80000000;

    inbits = htonl(inbits);
    fwrite(&inbits,sizeof(inbits),1,zmap);
    *prev_in = new_in;
  }
  {
    long outbytes = new_out - *prev_out;
    if ((long long)outbytes + *prev_out != new_out) { fprintf(stderr,"too long output of block blocks?"); exit(1); }
    outbytes = htonl(outbytes);
    fwrite(&outbytes,sizeof(outbytes),1,zmap);
    *prev_out = new_out;
  }
  zmapentries++;
  last_delta_in = new_in;
}

void do_zstream(FILE *fin, FILE* fout, const char* bufsofar, size_t got)
{
  z_stream zs;
  Bytef inbuf[4096];
  Bytef *outbuf = malloc(blocksize);
  int eoz = 0;
  int header_bits;
  long long prev_in = 0;
  long long prev_out = 0;
  long long midblock_in = 0;
  long long midblock_out = 0;
  
  zs.zalloc = Z_NULL;
  zs.zfree = Z_NULL;
  zs.opaque = NULL;
  zs.next_in = inbuf;
  zs.avail_in = 0;
  zs.total_in = 0;
  zs.next_out = outbuf;
  zs.avail_out = 0;

  if (inflateInit2(&zs,-MAX_WBITS) != Z_OK) exit(-1);

  { /* Skip gzip header and do iniital buffer fill */
    int flags = bufsofar[3];
    const char *p = bufsofar + 10;
    if (flags & ORIG_NAME)
      while (*p++ != 0) ;
    header_bits = 8*(p - bufsofar);
    got -= (p-bufsofar);
    if (got > sizeof(inbuf)) { fprintf(stderr,"internal failure, %d > %d input buffer available\n",got,sizeof(inbuf)); exit(2); }
    memcpy(inbuf,p,got);
    /* Fill the buffer up to offset sizeof(inbuf) of the input file - we want to try and keep the input blocks aligned with block boundaries in the underlying filesystem and physical storage */
    if (sizeof(inbuf) > got +(header_bits/8))
      zs.avail_in = got + fread(inbuf+got,1,sizeof(inbuf)-got-(header_bits/8),fin);
  }
  /* Start the zmap. We write into a temp file, which the caller then copies into the zsync file later. */
  zmap = tmpfile();
  if (!zmap) { perror("tmpfile"); exit(2); }

  /* We are past the header, so we are now at the start of the first block */
  write_zmap_delta(&prev_in,&prev_out,header_bits, zs.total_out, 1);
  zs.avail_out = blocksize;
 
  while (!eoz) {
    if (zs.avail_in == 0) {
      int rc = fread(inbuf,1,sizeof(inbuf),fin);
      zs.next_in = inbuf;
      if (rc < 0) { perror("read"); exit(2); }
      zs.avail_in = rc;
    }
    {
      int rc;

      rc = inflate(&zs,Z_BLOCK);
      switch (rc) {
      case Z_STREAM_END:
	eoz = 1;
      case Z_BUF_ERROR: /* Not really an error, just means we provided stingy buffers */
      case Z_OK:
	break;
      default:
	fprintf(stderr,"zlib error %s\n",zs.msg);
	exit(1);
      }
      if (zs.data_type & 128 || rc == Z_STREAM_END) {
	write_zmap_delta(&prev_in,&prev_out,header_bits + in_position(&zs),zs.total_out,1);

	midblock_in = midblock_out = 0;
      }
      if (zs.avail_out == 0 || rc == Z_STREAM_END) {
	SHA1_Update(&shactx, outbuf, blocksize-zs.avail_out);
	/* Completed a block */
	write_block_sums(outbuf,blocksize-zs.avail_out,fout);
	zs.next_out = outbuf; zs.avail_out = blocksize;
      } else if (inflateSafePoint(&zs)) {
	long long cur_in = header_bits + in_position(&zs);
	//	fprintf(stderr,"here %lld %lld %lld!\n",cur_in,midblock_in,last_delta_in);
	if (cur_in > (midblock_in ? midblock_in : last_delta_in) + 16*blocksize) {
	  if (midblock_in) {
	    write_zmap_delta(&prev_in,&prev_out,midblock_in,midblock_out,0);
	  }
	  midblock_in = cur_in; midblock_out = zs.total_out;
	}
      }
    }
  }
  len += zs.total_out;
  inflateEnd(&zs);
  fputc('\n',fout);
  /* Move back to the start of the zmap constructed, ready for the caller to read it back in */
  rewind(zmap);
}

static int no_look_inside;

void read_stream_write_blocksums(FILE* fin, FILE* fout) 
{
  unsigned char *buf = malloc(blocksize);
 
  if (!buf) {
    fprintf(stderr,"out of memory\n"); exit(1);
  }
  
  while (!feof(fin)) {
    int got = fread(buf, 1, blocksize, fin);

    if (got > 0) {
      if (!no_look_inside && len == 0 && buf[0] == 0x1f && buf[1] == 0x8b) {
	do_zstream(fin,fout,buf,got);
	break;
      }

      /* The SHA-1 sum, unlike our internal block-based sums, is on the whole file and nothing else - no padding */
      SHA1_Update(&shactx, buf, got);

      write_block_sums(buf,got,fout);
      len += got;
    } else {
      if (ferror(fin))
	stream_error("fread",fin);
    }
  }
}

void fcopy(FILE* fin, FILE* fout)
{
  unsigned char buf[4096];
  size_t len;

  while ((len = fread(buf,1,sizeof(buf),fin)) > 0) {
    if (fwrite(buf,1,len,fout) < len)
      break;
  }
  if (ferror(fin)) {
    stream_error("fread",fin);
  }
  if (ferror(fout)) {
    stream_error("fwrite",fout);
  }
}

#include <libgen.h>

int main(int argc, char** argv) {
  FILE* tf = tmpfile();
  FILE* instream;
  char * fname = NULL;
  char ** url = NULL;
  int nurls = 0;
  char ** Uurl = NULL;
  int nUurls = 0;
  char * outfname = NULL;
  FILE* fout;
  char *infname = NULL;

  {
    int opt;
    while ((opt = getopt(argc,argv,"o:f:b:u:U:Z")) != -1) {
      switch (opt) {
      case 'o':
	outfname = strdup(optarg);
	break;
      case 'f':
	fname = strdup(optarg);
	break;
      case 'b':
	blocksize = atoi(optarg);
	if ((blocksize & (blocksize-1)) != 0) { fprintf(stderr,"blocksize must be a power of 2 (512, 1024, 2048, ...)\n"); exit(2); }
	break;
      case 'u':
	url = realloc(url,(nurls+1)*sizeof *url);
	url[nurls++] = optarg;
	break;
      case 'U':
	Uurl = realloc(Uurl,(nUurls+1)*sizeof *Uurl);
	Uurl[nUurls++] = optarg;
	break;
      case 'Z':
	no_look_inside = 1;
	break;
      }
    }
    if (optind == argc-1) {
      instream = fopen(argv[optind],"rb");
      if (!instream) { perror("open"); exit(2); }
      infname = strdup(argv[optind]);
      if (!fname) fname = basename(argv[optind]);
    }
    else {
      instream = stdin;
    }
  }

  SHA1_Init(&shactx);

  read_stream_write_blocksums(instream,tf);

  if (fname && zmapentries) {
    /* Remove any trailing .gz, as it is the uncompressed file being transferred */
    char *p = strrchr(fname,'.');
    if (p) {
      if (!strcmp(p,".gz")) *p = 0;
      if (!strcmp(p,".tgz")) strcpy(p,".tar");
    }
  }
  if (!outfname && fname) {
    outfname = malloc(strlen(fname) + 10);
    sprintf(outfname,"%s.zsync",fname);
  }
  if (outfname) {
    fout = fopen(outfname,"wb");
    if (!fout) { perror("open"); exit(2); }
    free(outfname);
  } else {
    fout = stdout;
  }

  /* Okay, start writing the zsync file */
  fprintf(fout,"zsync: " VERSION "\nMin-Version: 0.0.5\n");
  if (fname) fprintf(fout,"Filename: %s\n",fname);
  fprintf(fout,"Blocksize: %d\n",blocksize);
  fprintf(fout,"Length: %lld\n",len);
  { /* Write URLs */
    int i;
    for (i = 0; i < nurls; i++)
      fprintf(fout,"%s: %s\n",zmapentries ? "Z-URL" : "URL", url[i]);
    for (i = 0; i < nUurls; i++)
      fprintf(fout,"URL: %s\n", Uurl[i]);
  }
  if (nurls == 0 && infname) {
    /* Assume that we are in the public dir, and use relative paths.
     * Look for an uncompressed version and add a URL for that to if appropriate. */
    fprintf(fout,"%s: %s\n",zmapentries ? "Z-URL" : "URL", infname);
    if (zmapentries && fname && !access(fname,R_OK)) {
      fprintf(fout,"URL: %s\n",fname);
    }
    fprintf(stderr,"Relative URL included in .zsync file - you must keep the file being served and the .zsync in the same public directory\n");
  }
#ifdef HAVE_LIBCRYPTO
  fputs("SHA-1: ",fout);
  {
    unsigned char digest[SHA_DIGEST_LENGTH];
    int i;


    SHA1_Final(&digest[0], &shactx);

    for (i = 0; i < sizeof digest; i++)
      fprintf(fout,"%02x",digest[i]);
  }
  fputc('\n',fout);
#endif
  if (zmapentries) {
    fprintf(fout,"Z-Map: %d\n",zmapentries);
    fcopy(zmap,fout);
    fclose(zmap);
  }

  fputc('\n',fout);
  rewind(tf);
  fcopy(tf,fout);
  fclose(tf);
  fclose(fout);

  return 0;
}
