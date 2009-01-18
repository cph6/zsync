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
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#ifdef WITH_DMALLOC
# include <dmalloc.h>
#endif

#include "libzsync/zsync.h"

#include "http.h"
#include "url.h"
#include "progress.h"

void read_seed_file(struct zsync_state* z, const char* fname) {
  if (zsync_hint_decompress(z) && strlen(fname) > 3 && !strcmp(fname + strlen(fname) - 3,".gz")) {
    FILE* f;
    {
      char* cmd = malloc(6 + strlen(fname)*2);

      if (!cmd) return;

      strcpy(cmd,"zcat ");
      {
	int i,j;
	for (i=0,j=5; fname[i]; i++) {
	  if (!isalnum(fname[i])) cmd[j++] = '\\';
	  cmd[j++] = fname[i];
	}
	cmd[j] = 0;
      }

      if (!no_progress) fprintf(stderr,"reading seed %s: ",cmd);
      f = popen(cmd,"r");
      free(cmd);
    } 
    if (!f) {
      perror("popen"); fprintf(stderr,"not using seed file %s\n",fname);
    } else {
      zsync_submit_source_file(z, f, !no_progress);
      if (pclose(f) != 0) {
	perror("close");
      }
    }
  } else {
    FILE* f = fopen(fname,"r");
    if (!no_progress) fprintf(stderr,"reading seed file %s: ",fname);
    if (!f) {
      perror("open"); fprintf(stderr,"not using seed file %s\n",fname);
    } else {
      zsync_submit_source_file(z, f, !no_progress);
      if (fclose(f) != 0) {
	perror("close");
      }
    }
  }
  {
    long long done,total;
    zsync_progress(z, &done, &total);
    if (!no_progress) fprintf(stderr,"\rRead %s. Target %02.1f%% complete.      \n",fname,(100.0f * done)/total);
  }
}

long long http_down;

static void** append_ptrlist(int *n, void** p, void* a) {
  if (!a) return p;
  p = realloc(p,(*n + 1) * sizeof *p);
  if (!p) { fprintf(stderr,"out of memory\n"); exit(1); }
  p[*n] = a;
  (*n)++;
  return p;
}

struct zsync_state* read_zsync_control_file(const char* p, const char* fn)
{
  FILE* f;
  struct zsync_state* zs;
  char* lastpath = NULL;

  f = fopen(p,"r");
  if (!f) {
    if (!is_url_absolute(p)) {
      perror(p); exit(2);
    }
    f = http_get(p,&lastpath,fn);
    if (!f) {
      fprintf(stderr,"could not read control file from URL %s\n",p);
      exit(3);
    }
    referer = lastpath;
  }
  if ((zs = zsync_begin(f)) == NULL) { exit(1); }
  if (fclose(f) != 0) { perror("fclose"); exit(2); }
  return zs;
}

static char* get_filename_prefix(const char* p) {
  char* s = strdup(p);
  char *t = strrchr(s,'/');
  char *u;
  if (t) *t++ = 0;
  else t = s;
  u = t;
  while (isalnum(*u)) { u++; }
  *u = 0;
  if (*t > 0)
    t = strdup(t);
  else
    t = NULL;
  free(s);
  return t;
}

char* get_filename(const struct zsync_state* zs, const char* source_name)
{
  char* p = zsync_filename(zs);
  char* filename = NULL;

  if (p) {
    if (strchr(p,'/')) {
      fprintf(stderr,"Rejected filename specified in %s, contained path component.\n",source_name);
      free(p);
    } else {
      char *t = get_filename_prefix(source_name);

      if (t && !memcmp(p,t,strlen(t)))
	filename = p;
      else
	free(p);

      if (t && !filename) {
	fprintf(stderr,"Rejected filename specified in %s - prefix %s differed from filename %s.\n",source_name, t, p);
      }
      free(t);
    }
  }
  if (!filename) {
    filename = get_filename_prefix(source_name);
    if (!filename) filename = strdup("zsync-download");
  }
  return filename;
}

static float calc_zsync_progress(const struct zsync_state* zs)
{
  long long zgot, ztot;

  zsync_progress(zs, &zgot, &ztot);
  return (100.0f*zgot / ztot);
}

#define BUFFERSIZE 8192

int fetch_remaining_blocks_http(struct zsync_state* z, const char* url, int type)
{
  int ret = 0;
  struct range_fetch* rf;
  unsigned char* buf;
  struct zsync_receiver* zr;
  char *u = make_url_absolute(referer, url);
  
  if (!u) {
    fprintf(stderr,"URL '%s' from the .zsync file is relative, but I don't know the referer URL (you probably downloaded the .zsync separately and gave it to me as a file). I need to know the referring URL (the URL of the .zsync) in order to locate the download. You can specify this with -u (or edit the URL line(s) in the .zsync file you have).\n",url);
    return -1;
  }

  rf = range_fetch_start(u);
  if (!rf) { free(u); return -1; }

  zr = zsync_begin_receive(z, type);
  if (!zr) { range_fetch_end(rf); free(u); return -1; }
  
  if (!no_progress) fprintf(stderr,"downloading from %s:",u);
  
  buf = malloc(BUFFERSIZE);
  if (!buf) { zsync_end_receive(zr); range_fetch_end(rf); free(u); return -1; }

  {
    int nrange;
    off_t *zbyterange;

    zbyterange = zsync_needed_byte_ranges(z, &nrange, type);
    if (!zbyterange) return 1;
    if (nrange == 0) return 0;

    range_fetch_addranges(rf, zbyterange, nrange);

    free(zbyterange);
  }

  {
    int len;
    off_t zoffset;
    struct progress p = {0,0,0,0};

    if (!no_progress) fputc('\n',stderr);
    if (!no_progress)
      do_progress(&p,calc_zsync_progress(z),range_fetch_bytes_down(rf));

    while (!ret && (len = get_range_block(rf, &zoffset, buf, BUFFERSIZE)) > 0) {
      if (zsync_receive_data(zr, buf, zoffset, len) != 0)
	ret = 1;
      
      if (!no_progress)
	do_progress(&p,calc_zsync_progress(z),range_fetch_bytes_down(rf));

      zoffset += len; // Needed in case next call returns len=0 and we need to signal where the EOF was.
    }

    if (len < 0) ret = -1;
    else
      zsync_receive_data(zr, NULL, zoffset, 0);

    if (!no_progress) end_progress(&p,zsync_status(z) >= 2 ? 2 : len == 0 ? 1 : 0);
  }

  free(buf);
  http_down += range_fetch_bytes_down(rf);
  zsync_end_receive(zr);
  range_fetch_end(rf);
  free(u);
  return ret;
}

int fetch_remaining_blocks(struct zsync_state* zs)
{
  int n, utype;
  const char * const * url = zsync_get_urls(zs, &n, &utype);
  int *status;
  int ok_urls = n;


  if (!url) {
    fprintf(stderr,"no URLs available from zsync?");
    return 1;
  }
  status = calloc(n, sizeof *status);

  while (zsync_status(zs) < 2 && ok_urls) {
    /* Still need data */
    int try = rand() % n;

    if (!status[try]) {
      const char* tryurl = url[try];

      int rc = fetch_remaining_blocks_http(zs,tryurl, utype);

      if (rc != 0) {
	fprintf(stderr,"failed to retrieve from %s\n",tryurl);
	status[try] = 1; ok_urls--;
      }
    }
  }
  free(status);
  return 0;
}

int main(int argc, char** argv) {
  struct zsync_state* zs;
  char *temp_file = NULL;
  char **seedfiles = NULL;
  int nseedfiles = 0;
  char* filename = NULL;
  long long local_used;
  char* zfname = NULL;

  srand(getpid());
  {
    int opt;
    while ((opt = getopt(argc,argv,"A:k:o:i:Vsu:")) != -1) {
      switch (opt) {
      case 'A':
	{ /* Scan string as hostname=username:password */
	  char* p = strdup(optarg);
	  char* q = strchr(p, '=');
	  char* r = q ? strchr(q, ':') : NULL;
	  if (!q || !r) {
	    fprintf(stderr, "-A takes hostname=username:password\n");
	    exit(1);
	  } else {
	    *q++ = *r++ = 0;
	    add_auth(p, q, r);
	  }
	}
	break;
      case 'k':
	free(zfname); zfname = strdup(optarg);
	break;
      case 'o':
	free(filename); filename = strdup(optarg);
	break;
      case 'i':
	seedfiles = append_ptrlist(&nseedfiles,seedfiles,optarg);
	break;
      case 'V':
	printf(PACKAGE " v" VERSION " (compiled " __DATE__ " " __TIME__ ")\n"
	       "By Colin Phipps <cph@moria.org.uk>\n"
	       "Published under the Artistic License v2, see the COPYING file for details.\n");
	exit(0);
      case 's':
	no_progress = 1;
	break;
      case 'u':
	referer = strdup(optarg);
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
  if (!isatty(0)) no_progress = 1;
  {
    char *pr = getenv("http_proxy");
    if (pr != NULL) set_proxy_from_string(pr);
  }
  if ((zs = read_zsync_control_file(argv[optind],zfname)) == NULL)
    exit(1);

  if (!filename) filename = get_filename(zs, argv[optind]);

  temp_file = malloc(strlen(filename)+6);
  strcpy(temp_file,filename);
  strcat(temp_file,".part");

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
    zsync_progress(zs, &local_used, NULL);
    if (!local_used) {
      fputs("No relevent local data found - I will be downloading the whole file. If that's not what you want, CTRL-C out. You should specify the local file is the old version of the file to download with -i (you might have to decompress it with gzip -d first). Or perhaps you just have no data that helps download the file\n",stderr);
    }
  }

  if (zsync_rename_file(zs, temp_file) != 0) {
    perror("rename"); exit(1);
  }

  if (fetch_remaining_blocks(zs) != 0) {
    fprintf(stderr,"failed to retrieve all remaining blocks - no valid download URLs remain. Incomplete transfer left in %s.\n(If this is the download filename with .part appended, zsync will automatically pick this up and reuse the data it has already done if you retry in this dir.)\n",temp_file);
    exit(3);
  }

  {
    int r;

    if (!no_progress)
      printf("verifying download...");
    r = zsync_complete(zs);
    switch (r) {
    case -1:
      fprintf(stderr,"Aborting, download available in %s\n",temp_file);
      exit(2);
    case 0:
      if (!no_progress)
        printf("no recognised checksum found\n");
      break;
    case 1:
      if (!no_progress)
        printf("checksum matches OK\n");
      break;
    }
  }

  free(temp_file);
  temp_file = zsync_end(zs);

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
    free(filename);
  } else {
    printf("No filename specified for download - completed download left in %s\n",temp_file);
  }

  if (!no_progress)
    printf("used %lld local, fetched %lld\n", local_used, http_down);
  free(referer);
  free(temp_file);
  return 0;
}
