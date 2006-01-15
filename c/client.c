/*
 *   zsync - client side rsync over http
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
#include <ctype.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "config.h"

/* htons - where to get this? */
#ifdef HAVE_HTONS_IN_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_HTONS_IN_SYS_PARAM_H
#include <sys/param.h>
#endif

#include "zsync.h"
#include "http.h"
#include "url.h"
#include "fetch.h"

long known_blocks;

#ifdef HAVE_LIBCRYPTO

#include <openssl/sha.h>

#endif

void read_seed_file(struct zsync_state* z, const char* fname) {
  fprintf(stderr,"reading seed file %s: ",fname);
  {
    /* mmap failed, try streaming it */
    FILE* f = fopen(fname,"r");
    if (!f) {
      perror("open"); fprintf(stderr,"not using seed file %s\n",fname);
    } else {
      known_blocks += submit_source_file(z, f);
      if (fclose(f) != 0) {
	perror("close");
      }
    }
  }
  fputc('\n',stderr);
}

int blocksize;
long long http_down;

long long filelen;
char** url;
int nurl;
char** zurl;
int nzurl;
char* filename;
char* sha1sum;

static void** append_ptrlist(int *n, void** p, void* a) {
  if (!a) return p;
  p = realloc(p,(*n + 1) * sizeof *p);
  if (!p) { fprintf(stderr,"out of memory\n"); exit(1); }
  p[*n] = a;
  (*n)++;
  return p;
}

struct gzblock* zblock;
int nzblocks;

int read_zsync_control_stream(FILE* f, struct zsync_state** z, const char* source_name)
{
  struct zsync_state* zs = NULL;
  zs_blockid blocks;
  long long bitoffset;

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
	  exit(3);
	}
      } else if (!strcmp(buf, "Min-Version")) {
	if (strcmp(p,VERSION) > 0) {
	  fprintf(stderr,"control file indicates that zsync-%s or better is required\n",p);
	  exit(3);
	}
      } else if (!strcmp(buf, "Length")) {
	filelen = atol(p);
      } else if (!strcmp(buf, "Filename")) {
	if (!filename) {
	  if (strchr(buf,'/')) {
	    fprintf(stderr,"Rejected filename specified in %s, contained path component.\n",source_name);
	  } else {
	    char *s = strdup(source_name);
	    char *t = strrchr(s,'/');
	    char *u;
	    if (t) *t++ = 0;
	    else t = s;
	    u = t;
	    while (isalnum(*u)) { u++; }
	    *u = 0;
	    if (strlen(t) > 0)
	      if (!memcmp(p,t,strlen(t)))
		filename = strdup(p);
	    if (!filename) {
	      fprintf(stderr,"Rejected filename specified in %s - prefix %s differed from filename %s.\n",source_name, t, p);
	    }
	    free(s);
	  }
	}
      } else if (!strcmp(buf, "URL")) {
	char *u = make_url_absolute(source_name,p);
	if (!u) {
	  fprintf(stderr,"unable to determine full URL for %s\n",p);
	} else
	  url = (char**)append_ptrlist(&nurl, url, u);
      } else if (!strcmp(buf, "Z-URL")) {
	char *u = make_url_absolute(source_name,p);
	if (!u) {
	  fprintf(stderr,"unable to determine full URL for %s\n",p);
	} else
	  zurl = (char**)append_ptrlist(&nzurl, zurl, u);
      } else if (!strcmp(buf, "Blocksize")) {
	blocksize = atol(p);
	if (blocksize < 0 || (blocksize & (blocksize-1))) {
	  fprintf(stderr,"nonsensical blocksize %d\n",blocksize); return -1;
	}
      } else if (blocks && !strcmp(buf,"Z-Map")) {
	int i;
	nzblocks = atoi(p);
	if (nzblocks < 0) { fprintf(stderr,"bad Z-Map line\n"); return -1; }
	zblock = malloc(nzblocks * sizeof *zblock);
	for (i=0; i<nzblocks; i++) {
	  if (fread(&zblock[i],sizeof *zblock,1,f) < 1) { fprintf(stderr,"premature EOF after Z-Map\n"); return -1; }
	  zblock[i].inbitoffset = ntohl(zblock[i].inbitoffset);
	  zblock[i].outbyteoffset = ntohl(zblock[i].outbyteoffset);
	}
      } else if (!strcmp(buf,"SHA-1")) {
	sha1sum = strdup(p);
      } else {
	fprintf(stderr,"unrecognised tag %s - perhaps you need a newer version of zsync?\n",buf);
	return -1;
      }
      if (filelen && blocksize)
	blocks = (filelen + blocksize-1)/blocksize;
    } else {
      fprintf(stderr, "Bad line - not a zsync file? \"%s\"\n", buf);
      return -1;
    }
  }
  if (!filelen || !blocksize) {
    fprintf(stderr,"Not a zsync file (looked for Blocksize and Length lines)\n");
    return -1;
  }
  if (!(zs = zsync_init(blocks, blocksize))) {
    exit (1);
  }
  {
    zs_blockid id = 0;
    for (;id < blocks; id++) {
      struct {
	struct rsum r;
	unsigned char checksum[CHECKSUM_SIZE];
      } buf;

      if (fread((void*)&buf,sizeof(buf),1,f) < 1) {
	fprintf(stderr,"short read on control file; %s\n",strerror(ferror(f)));
	return -1;
      }
      buf.r.a = ntohs(buf.r.a); buf.r.b = ntohs(buf.r.b);
      add_target_block(zs, id, buf.r,buf.checksum);

    }
  }
  *z = zs;
  return 0;
}

void  read_zsync_control_file(const char* p, struct zsync_state** pzs)
{
  FILE* f;
  char* lastpath = p;

  f = fopen(p,"r");
  if (!f) {
    if (memcmp(p,"http://",7)) {
      perror(p); exit(2);
    }
    f = http_open(p,NULL,200,&lastpath);
    if (!f) {
      fprintf(stderr,"could not read control file from URL %s\n",p);
      exit(3);
    }
    {
      char buf[512];
      do {
	fgets(buf,sizeof(buf),f);
	
	if (ferror(f)) {
	  perror("read"); exit(1);
	}
      } while (buf[0] != '\r' && !feof(f));
    }
    referer = lastpath;
  }
  if (read_zsync_control_stream(f, pzs, lastpath) != 0) { exit(1); }
  if (fclose(f) != 0) { perror("fclose"); exit(2); }
}

int fetch_remaining_blocks(struct zsync_state* zs)
{
  zs_blockid blrange[2];
  int first = 1;
  
  /* Use get_needed_block_ranges with a wide range and a dummy storage area. If we get at least once range, there is still data to transfer. */
  while (get_needed_block_ranges(zs, &blrange[0], 1, 0, 0x7fffffff) != 0) {
    char **ptryurl = NULL;
    int zfetch = 0;

    /* Pick a random URL from the list. Try compressed URLs first. */
    if (!first && zblock) {
      int i,c;

      zfetch = 1;
      for (i=0, c=0; i<nzurl; i++)
	if (zurl[i] != NULL) {
	  if (!c++) ptryurl = &zurl[i];
	  else if (!(rand() % c)) ptryurl = &zurl[i];
	}
    }
    if (!ptryurl) {
      int i,c;

      zfetch = 0;
      for (i=0, c=0; i<nurl; i++)
	if (url[i] != NULL) {
	  if (!c++) ptryurl = &url[i];
	  else if (!(rand() % c)) ptryurl = &url[i];
	}
    }
    if (!first && !ptryurl) return 1; /* All URLs eliminated. */

    if (ptryurl) {
      int rc;
      if (zfetch)
	rc = fetch_remaining_blocks_zlib_http(zs,*ptryurl,zblock,nzblocks);
      else
	rc = fetch_remaining_blocks_http(zs,*ptryurl,first && zblock ? 2 : 0);

      if (rc != 0) {
	fprintf(stderr,"%s removed from list\n",*ptryurl);
	free(*ptryurl);
	*ptryurl = NULL;
      }
    }
    first = 0;
  }
  return 0;
}

static int truncate_verify_close(int fh, long long filelen, const char* checksum, const char* checksum_method) {
  if (ftruncate(fh,filelen) != 0) { perror("ftruncate"); return -1; }
  if (lseek(fh,0,SEEK_SET) != 0) { perror("lseek"); return -1; }
#ifdef HAVE_LIBCRYPTO
  if (checksum && !strcmp(checksum_method,"SHA-1")) {
    SHA_CTX shactx;

    fprintf(stderr,"verifying download\n");
    if (strlen(checksum) != SHA_DIGEST_LENGTH*2) {
      fprintf(stderr,"SHA-1 digest from control file is wrong length.\n");
      return -1;
    }
    {
      char buf[4096];
      int rc;
      
      SHA1_Init(&shactx);
      while (0 < (rc = read(fh,buf,sizeof buf))) {
	SHA1_Update(&shactx,buf,rc);
      }
      if (rc < 0) { perror("read"); return -1; }
    }
    {
      unsigned char digest[SHA_DIGEST_LENGTH];
      int i;

      SHA1_Final(&digest[0], &shactx);

      for (i=0; i<SHA_DIGEST_LENGTH; i++) {
	int j;
	sscanf(&checksum[2*i],"%2x",&j);
	if (j != digest[i]) {
	  fprintf(stderr,"Checksum mismatch\n"); return 1;
	}
      }
    }
  }
  else
#endif
  {
    fprintf(stderr,"Unable to verify checksum (%s), but proceeding.\n", checksum_method ? checksum_method : "no recognised checksum provided");
  }
  return close (fh);
}

int main(int argc, char** argv) {
  struct zsync_state* zs;
  char *temp_file = NULL;
  char **seedfiles = NULL;
  int nseedfiles = 0;

  {
    int opt;
    while ((opt = getopt(argc,argv,"o:i:")) != -1) {
      switch (opt) {
      case 'o':
	filename = strdup(optarg);
	break;
      case 'i':
	seedfiles = append_ptrlist(&nseedfiles,seedfiles,optarg);
	break;
      }
    }
  }
  if (optind == argc) {
    fprintf(stderr,"No .zsync file specified.\nUsage: zsync http://example.com/some/filename.zsync\n");
    exit(3);
  } else if (optind < argc-1) {
    fprintf(stderr,"Usage: zsync http://example.com/some/filename.zsync\n");
    exit(3);
  }
  read_zsync_control_file(argv[optind], &zs);
  if (filename) {
    temp_file = malloc(strlen(filename)+6);
    strcpy(temp_file,filename);
    strcat(temp_file,".part");
  }

  {
    int i;

    for (i=0; i<nseedfiles; i++) {
      read_seed_file(zs, seedfiles[i]);
    }
    if (!access(filename,R_OK)) {
      read_seed_file(zs, filename);
    }
    if (!access(temp_file,R_OK)) {
      read_seed_file(zs, temp_file);
    }
    if (!known_blocks) {
      fputs("No relevent local data found - I would have to download the whole file. You should specify the local file is the old version of the file to download with -i (you might have to decompress it with gzip -d first). Or you just really have no data to help download the file - in which case use wget :-)",stderr);
      exit(3);
    } else {
      zs_blockid blocks;
      blocks = (filelen + blocksize - 1)/blocksize;
      fprintf(stderr,"%d known/%d total\n",known_blocks,blocks);
    }
  }
  { /* Get the working file from libzsync */
    char* cur_temp_file = zsync_filename(zs);

    if (temp_file) {
      rename(cur_temp_file,temp_file);
    } else temp_file = cur_temp_file;
  }
  if (fetch_remaining_blocks(zs) != 0) {
    fprintf(stderr,"failed to retrieve all remaining blocks - no valid download URLs remain. Incomplete transfer left in %s.\n(If this is the download filename with .part appended, zsync will automatically pick this up and reuse the data it has already done if you retry in this dir.)\n",temp_file);
    exit(3);
  }

  {
    int fh = zsync_filehandle(zs);

    zsync_end(zs);
    
    if (truncate_verify_close(fh,filelen,sha1sum,"SHA-1") != 0) {
      fprintf(stderr,"Aborting, download available in %s\n",temp_file);
      exit(2);
    }
  }

  if (filename) {
    char* oldfile_backup = malloc(strlen(filename)+8);
    int ok = 1;

    strcpy(oldfile_backup,filename);
    strcat(oldfile_backup,".zs-old");

    if (!access(filename,F_OK)) {
      /* backup of old file */
      unlink(oldfile_backup); /* Don't care if this fails - the link below will catch any failure */
      if (link(filename,oldfile_backup) != 0) {
	perror("link");
	fprintf(stderr,"Unable to back up old file %s - completed download left in %s\n",filename,temp_file);
	ok = 0; /* Prevent overwrite of old file below */
      }
    }
    if (ok)
      if (rename(temp_file,filename) != 0) {
	perror("rename");
	fprintf(stderr,"Unable to back up old file %s - completed download left in %s\n",filename,temp_file);
      }
    free(oldfile_backup);
  } else {
    printf("No filename specified for download - completed download left in %s\n",temp_file);
  }

  fprintf(stderr,"used %lld local, fetched %lld\n",((long long)known_blocks)*blocksize,http_down);
  return 0;
}
