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
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

#ifndef HAVE_GETADDRINFO
#include "getaddrinfo.h"
#endif

#include "http.h"
#include "url.h"
#include "progress.h"
#include "format_string.h"

int connect_to(const char* node, const char* service)
{
  struct addrinfo hint;
  struct addrinfo *ai;
  int rc;

  memset(&hint,0,sizeof hint);
  hint.ai_family = AF_UNSPEC;
  hint.ai_socktype = SOCK_STREAM;

  if ((rc = getaddrinfo(node, service, &hint, &ai)) != 0) {
    perror(node);
    return -1;
  } else {
    struct addrinfo *p;
    int sd = -1;

    for (p = ai; sd == -1 && p != NULL; p = p->ai_next) {
      if ((sd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
	perror("socket");
      } else   
      if (connect(sd, p->ai_addr, p->ai_addrlen) < 0) {
	perror(node); close(sd); sd = -1;
      }
    }
    freeaddrinfo(ai);
    return sd;
  }
}

FILE* http_get_stream(int fd, int* code)
{
  FILE* f = fdopen(fd, "r");
  char buf[256];
  char *p;
  
  if (fgets(buf,sizeof(buf),f) == NULL || memcmp(buf, "HTTP/1", 6) != 0 || (p = strchr(buf, ' ')) == NULL) {
    *code = 0; fclose(f); return NULL;
  }
  
  *code = atoi(++p);
  
  return f;
}

/* Extract the Location URL, and make it absolute using the current URL 
 * (it ought to be absolute anyway, by the RFC, but many servers send 
 * relative URIs). */
char* get_location_url(FILE* f, const char* cur_url) {
  char buf[1024];

  while (fgets(buf,sizeof(buf),f)) {
    char *p;
    if (buf[0] == '\r' || buf[0] == '\n') return NULL;

    p = strchr(buf,':');
    if (!p) return NULL;
    *p++ = 0;
    if (strcasecmp(buf,"Location")) continue;

    while (*p == ' ') p++;

    { /* Remove trailing whitespace */
      char *q = p;
      while (*q != '\r' && *q != '\n' && *q != ' ' && *q) q++;

      *q = 0;
    }
    if (!*p) return NULL;
    return make_url_absolute(cur_url,p);
  }
  return NULL; // TODO
}

char *proxy;
char *pport;
static char **auth_details;
static int num_auth_details;

char *referer;

int set_proxy_from_string(const char* s)
{
  if (!memcmp(s,"http://",7)) {
    proxy = malloc(256);
    if (!proxy) return 0;
    if (!get_host_port(s,proxy,256,&pport))
      return 0;
    if (!pport) { pport = strdup("webcache"); }
    return 1;
  } else {
    char *p;
    proxy = strdup(s);
    p = strchr(proxy,':');
    if (!p) { pport = strdup("webcache"); return 1; }
    *p++ = 0;
    pport = strdup(p);
    return 1;
  }
}

void add_auth(char* host, char* user, char* pass)
{
  auth_details = realloc(auth_details, (num_auth_details + 1) * sizeof *auth_details);
  auth_details[num_auth_details*3  ] = host;
  auth_details[num_auth_details*3+1] = user;
  auth_details[num_auth_details*3+2] = pass;
  num_auth_details++;
}

static char* get_auth_hdr(const char* hn)
{
  int i;
  for (i=0; i < num_auth_details*3; i+=3) {
    if (!strcasecmp(auth_details[i], hn)) {
      char *u = auth_details[i+1];
      char *p = auth_details[i+2];
      size_t l = strlen(u)+strlen(p)+2;
      char *w = malloc(l);
      char *b;
      char *h;
      const char* t = "Authorization: Basic %s\r\n";

      // Make and encode the authorization string
      snprintf(w,l,"%s:%s", u, p);
      b = base64(w);

      // Make the header itself
      l = strlen(b)+strlen(t)+1;
      h = malloc(l);
      snprintf(h, l, t, b);
      free(w); free(b);
      return h;
    }
  }
  return NULL;
}

static char* http_date_string(time_t t, char* const buf, const int blen)
{
  struct tm d;

  if (gmtime_r(&t,&d) != NULL) {
    if (strftime(buf, blen, "%a, %d %h %Y %T GMT", &d) > 0) {
      return buf;
    }
  }
  return NULL;
}

FILE* http_get(const char* orig_url, char** track_referer, const char* tfname)
{
  int allow_redirects = 5;
  char* url;
  FILE* f = NULL;
  FILE* g = NULL;
  char* fname = NULL;
  char ifrange[200] = { "" };
  char *authhdr = NULL;
  int code;

  if (tfname) {
    struct stat st;
    
    fname = malloc(strlen(tfname) + 6);
    strcpy(fname,tfname); strcat(fname,".part");

    if (stat(fname,&st) == 0) {
      char buf[50];

      if (http_date_string(st.st_mtime,buf,sizeof(buf)) != NULL)
        snprintf(ifrange,sizeof(ifrange),"If-Unmodified-Since: %s\r\nRange: bytes=" OFF_T_PF "-\r\n",buf,st.st_size);

    } else if (errno == ENOENT && stat(tfname,&st) == 0) {
      char buf[50];

      if (http_date_string(st.st_mtime,buf,sizeof(buf)) != NULL)
        snprintf(ifrange,sizeof(ifrange),"If-Modified-Since: %s\r\n",buf);
    }
  }
      
  url = strdup(orig_url);
  if (!url) { free(fname); return NULL; }

  for (;allow_redirects-- && url && !f;) {
    char hostn[256];
    const char* connecthost;
    char* connectport;
    char *p;
    char *port;

    if ( (p = get_host_port(url,hostn,sizeof(hostn),&port)) == NULL) break;
    if (!proxy) {
      connecthost = hostn;
      connectport = strdup(port);
    } else {
      connecthost = proxy;
      connectport = strdup(pport);
    }
    {
      int sfd = connect_to(connecthost, connectport);

      free(connectport);

      if (sfd == -1) break;

      {
        char buf[1024];
        snprintf(buf, sizeof(buf), "GET %s HTTP/1.0\r\nHost: %s%s%s\r\nUser-Agent: zsync/%s\r\n%s%s\r\n",
                proxy ? url : p,
                hostn, !strcmp(port,"http") ? "" : ":", !strcmp(port,"http") ? "" : port,
                VERSION,
                ifrange[0] ? ifrange : "",
                authhdr ? authhdr : ""
                );
        if (send(sfd,buf,strlen(buf),0) == -1) {
          perror("sendmsg"); close(sfd); break;
        }
      }
      f = http_get_stream(sfd, &code);

      if (!f) break;
      if (code == 301 || code == 302 || code == 307) {
        char *oldurl = url;
        url = get_location_url(f, oldurl);
        free(oldurl);
        fclose(f); f = NULL;
      } else if (code == 401) { // Authorization required
        authhdr = get_auth_hdr(hostn);
        if (authhdr) {
          fclose(f); f = NULL;
          // And go round again
        } else {
          fclose(f); f = NULL; break;
        }
      } else if (code == 412) { // Precondition (i.e. if-unmodified-since) failed
        ifrange[0] = 0;
        fclose(f); f = NULL; // and go round again without the conditional Range:
      } else if (code == 200) { // Downloading whole file
        g = fname ? fopen(fname,"w+") : tmpfile();
      } else if (code == 206 && fname) { // Had partial content and server confirms not modified
        g = fopen(fname,"a+");
      } else if (code == 304) { // Unchanged (if-modified-since was false)
        g = fopen(tfname,"r");
      } else {
        fclose(f); f = NULL; break;
      }
    }
  }

  if (track_referer)
    *track_referer = url;
  else free(url);

  if (code == 304) {
    fclose(f);
    free(fname);
    return g;
  }

  if (!f) {
    fprintf(stderr,"failed on url %s\n",url ? url : "(missing redirect)");
    return NULL;
  }

  if (!g) { fclose(f); perror("fopen"); return NULL; }

  {
    size_t len = 0;
    { /* Skip headers. TODO support content-encodings, Content-Location etc */
      char buf[512];
      do {
	fgets(buf,sizeof(buf),f);
	
	sscanf(buf,"Content-Length: " SIZE_T_PF,&len);
	if (ferror(f)) {
	  perror("read"); exit(1);
	}
      } while (buf[0] != '\r' && !feof(f));
    }
    {
      size_t got = 0;
      struct progress p = {0,0,0,0};
      int r;

      if (!no_progress)
        do_progress(&p,0,got);

      while (!feof(f)) {
        char buf[1024];
        r = fread(buf, 1, sizeof(buf), f);
	
        if (r > 0)
          if (r > fwrite(buf, 1, r, g)) {
            fprintf(stderr,"short write on %s\n",fname);
            break;
          }
        if (r < 0) { perror("read"); break; }

        if (r>0) {
          got += r;
          if (!no_progress)
            do_progress(&p, len ? (100.0*got / len) : 0, got);
        }
      }
      if (!no_progress) end_progress(&p,feof(f) ? 2 : 0);
    }
    fclose(f);
  }
  rewind(g);

  if (fname) {
    rename(fname,tfname);
    free(fname);
  }

  return g;
}

/* HTTP Range: / 206 response interface 
 * 
 * If we are reading a multipart/byteranges, boundary is set.
 * If we are in the middle of an actual block, block_left is non-zero and offset gives the remembered offset.
 */

struct range_fetch {
  char* boundary;
  char* url;
  char hosth[256];

  char* authh;
  char* chost;
  char* cport;

  size_t block_left;
  off_t offset;
  int sd;
  char buf[4096];
  int buf_start, buf_end;
  off_t bytes_down;
  int server_close; /* 0: can send more, 1: cannot send more (but one set of headers still to read), 2: cannot send more and all existing headers read */

  off_t* ranges_todo;
  int nranges;
  int rangesdone;
  int rangessent;
};

static int get_more_data(struct range_fetch* rf) 
{
  if (rf->buf_start) {
    memmove(rf->buf, &(rf->buf[rf->buf_start]), rf->buf_end - rf->buf_start);
    rf->buf_end -= rf->buf_start; rf->buf_start = 0;
  }
  {
    int n;

    do {
      n = read(rf->sd, &(rf->buf[rf->buf_end]), sizeof(rf->buf) - rf->buf_end);
    } while (n == -1 && errno == EINTR);

    if (n < 0) {
      perror("read");
    } else {
      rf->bytes_down += n;
      rf->buf_end += n;
    }
    return n;
  }
}

static char* rfgets(char* buf, size_t len, struct range_fetch* rf) 
{
  char *p;

  while (1) {
    p = memchr(rf->buf + rf->buf_start, '\n', rf->buf_end - rf->buf_start);

    if (!p) {
      int n = get_more_data(rf);
      if (n <= 0) { /* If cut off, return the rest of the buffer */
	p = &(rf->buf[rf->buf_end]);
      }
    } else p++; /* Step past \n */
    
    if (p) {
      register char *bufstart = &(rf->buf[rf->buf_start]);
      len--; /* allow for trailing \0 */
      if (len > p-bufstart) len = p-bufstart;
      memcpy(buf, bufstart, len);
      buf[len] = 0;
      rf->buf_start += len;
      return buf;
    }
  }
}

struct range_fetch* range_fetch_start(const char* orig_url)
{
  struct range_fetch* rf = malloc(sizeof(struct range_fetch));
  char *p;
  char hostn[sizeof(rf->hosth)];

  if (!rf) return NULL;
  p = get_host_port(orig_url, hostn, sizeof(hostn), &(rf->cport));
  if (!p) { free(rf); return NULL; }
  
  if (strcmp(rf->cport,"http") != 0)
    snprintf(rf->hosth,sizeof(rf->hosth),"%s:%s",hostn,rf->cport);
  else
    snprintf(rf->hosth,sizeof(rf->hosth),"%s",hostn);

  if (proxy) {
    // URL must be absolute; throw away cport and get port for proxy
    rf->url = strdup(orig_url);
    free(rf->cport);
    rf->cport = strdup(pport);
    rf->chost = strdup(proxy);
  } else {
    // cport already set; set url to relative part and chost to the target
    rf->url = strdup(p);
    rf->chost = strdup(hostn);
  }

  rf->authh = get_auth_hdr(hostn);

  rf->block_left = 0;
  rf->bytes_down = 0;
  rf->boundary = NULL;

  rf->buf_start = rf->buf_end = 0;

  rf->sd = -1;

  rf->ranges_todo = NULL; rf->nranges = rf->rangesdone = 0;

  return rf;
}

void range_fetch_addranges(struct range_fetch* rf, off_t* ranges, int nranges)
{
  int existing_ranges = rf->nranges - rf->rangesdone;
  off_t* nr = malloc(2*sizeof(*ranges)*(nranges + existing_ranges));
  
  if (!nr) return;
  /* Copy existing queue over */
  memcpy(nr,&(rf->ranges_todo[2*rf->rangesdone]),2*sizeof(*ranges)*existing_ranges);
  /* And append the new stuff */
  memcpy(&nr[2*existing_ranges], ranges, 2*sizeof(*ranges)*nranges);

  /* Move back rangessent and rangesdone to the new locations, and update the count. */
  rf->rangessent -= rf->rangesdone;
  rf->rangesdone = 0;
  rf->nranges = existing_ranges + nranges;

  free(rf->ranges_todo);
  rf->ranges_todo = nr;
}

static void range_fetch_connect(struct range_fetch* rf)
{
  rf->sd = connect_to(rf->chost, rf->cport);
  rf->server_close = 0;
  rf->rangessent = rf->rangesdone;
}

static void range_fetch_getmore(struct range_fetch* rf)
{
  char request[2048];
  int l;
  int max_range_per_request = 20;

  /* Only if there's stuff queued to get */
  if (rf->rangessent == rf->nranges) return;

  snprintf(request,sizeof(request), 
	   "GET %s HTTP/1.1\r\n"
	   "User-Agent: zsync/" VERSION "\r\n"
	   "Host: %s"
	   "%s%s\r\n"
	   "%s"
	   "Range: bytes=",
	   rf->url, rf->hosth,
	   referer ? "\r\nReferer: " : "", referer ? referer : "",
	   rf->authh ? rf->authh : ""
	   );
  
  /* The for loop here is just a sanity check, lastrange is the real loop control */
  for (; rf->rangessent < rf->nranges; ) {
    int i = rf->rangessent;
    int lastrange = 0;

    l = strlen(request);
    if (l > 1200 || !(--max_range_per_request) || i == rf->nranges-1) lastrange = 1;
    
    snprintf(request + l, sizeof(request)-l, OFF_T_PF "-" OFF_T_PF "%s", rf->ranges_todo[2*i], rf->ranges_todo[2*i+1], lastrange ? "" : ",");

    rf->rangessent++;
    if (lastrange) break;
  }
  l = strlen(request);
  /* Possibly close the connection (and record the fact, so we definitely don't send more stuff) if this is the last */
  snprintf(request + l, sizeof(request)-l, "\r\n%s\r\n", rf->rangessent == rf->nranges ? (rf->server_close = 1, "Connection: close\r\n") : "");
  
  {
    size_t len = strlen(request);
    char *p = request;
    int r = 0;
    
    while (len > 0 && ((r = send(rf->sd,p,len,0)) != -1 || errno == EINTR)) {
      if (r >= 0) { p += r; len -= r; }
    }
    if (r == -1) {
      perror("send");
    }
  }
}

static void buflwr(char* s)
{
  char c;
  while((c = *s) != 0) {
    if (c >= 'A' && c <= 'Z')
      *s = c - 'A' + 'a';
    s++;
  }
}

/* This has 3 cases - EOF returns 0, good returns >0, error returns <0 */
int range_fetch_read_http_headers(struct range_fetch* rf)
{
  char buf[512];

  { /* read status line */
    char *p;
    int c;

    if (rfgets(buf,sizeof(buf),rf) == NULL)
      return -1;
    if (buf[0] == 0) return 0; /* EOF, caller decides if that's an error */
    if (memcmp(buf, "HTTP/1", 6) != 0 || (p = strchr(buf, ' ')) == NULL) {
      return -1;
    }
    c = atoi(p+1);
    if (c != 206) {
      fprintf(stderr,"bad status code %d\n",c);
      return -1;
    }
    if (*(p-1) == '0') { /* HTTP/1.0 server? */
      rf->server_close = 2;
    }
  }

  while (1) {
    char *p;
    
    if (rfgets(buf,sizeof(buf),rf) == NULL) return -1;
    if (buf[0] == '\r' || buf[0] == '\0') {
      /* End of headers. We are happy provided we got the block boundary */
      if ((rf->boundary || rf->block_left) && !(rf->boundary && rf->block_left)) return 1;
      break;
    }
    p = strstr(buf,": ");
    if (!p) break;
    *p = 0; p+=2;
    buflwr(buf);
      /* buf is the header name (lower-cased), p the value */
    if (!strcmp(buf,"content-range")) {
      off_t from,to;
      sscanf(p,"bytes " OFF_T_PF "-" OFF_T_PF "/",&from,&to);
      if (from <= to) {
	rf->block_left = to + 1 - from;
	rf->offset = from;
      }
      /* Can only have got one range. */
      rf->rangesdone++;
      rf->rangessent = rf->rangesdone;
    }
    if (!strcmp(buf,"connection") && !strcmp(p,"close")) {
      rf->server_close = 2;
    }
    if (!strcasecmp(buf,"content-type") && !strncasecmp(p,"multipart/byteranges",20)) {
      char *q = strstr(p,"boundary=");
      if (!q) break;
      q += 9;
      if (*q == '"') {
	rf->boundary = strdup(q+1);
	q = strchr(rf->boundary,'"');
	if (q) *q = 0;
      } else {
	rf->boundary = strdup(q);
	q = rf->boundary + strlen(rf->boundary)-1;
      
	while (*q == '\r' || *q == ' ' || *q == '\n') *q-- = '\0';
      }
    }
  }
  return -1;
}

int get_range_block(struct range_fetch* rf, off_t* offset, unsigned char* data, size_t dlen)
{
  size_t bytes_to_caller = 0;

  if (!rf->block_left) {
check_boundary:
    if (!rf->boundary) {
      int newconn = 0;
      int header_result;

      if (rf->sd != -1 && rf->server_close == 2) {
	close(rf->sd); rf->sd = -1;
      }
      if (rf->sd == -1) {
        if (rf->rangesdone == rf->nranges) return 0;
	range_fetch_connect(rf);
	if (rf->sd == -1) return -1;
	newconn = 1;
	range_fetch_getmore(rf);
      }
      header_result = range_fetch_read_http_headers(rf);

      /* Might be the last */
      if (rf->server_close == 1) rf->server_close = 2;

      /* EOF on first connect is fatal */
      if (newconn && header_result == 0) {
        fprintf(stderr,"EOF from %s\n",rf->url);
        return -1;
      }

      /* Return EOF or error to caller */
      if (header_result <= 0) return header_result ? -1 : 0;
      
      /* HTTP Pipelining - send next request before reading current response */
      if (!rf->server_close) range_fetch_getmore(rf);
    }
    if (rf->boundary) {
      char buf[512];
      int gotr = 0;

      if (!rfgets(buf,sizeof(buf),rf)) return 0;
      /* Get, hopefully, boundary marker */
      if (!rfgets(buf,sizeof(buf),rf)) return 0;
      if (buf[0] != '-' || buf[1] != '-') return 0;

      if (memcmp(&buf[2],rf->boundary,strlen(rf->boundary))) {
        fprintf(stderr,"got bad block boundary: %s != %s",rf->boundary, buf);
        return -1; /* This is an error now */
      }
      /* Look for last record marker */
      if (buf[2+strlen(rf->boundary)] == '-') { free(rf->boundary); rf->boundary = NULL; goto check_boundary; }
      
      for(;buf[0] != '\r' && buf[0] != '\n' && buf[0] != '\0';) {
        off_t from, to;
        if (!rfgets(buf,sizeof(buf),rf)) return 0;

        /* HTTP headers are case insensitive */
        buflwr(buf);

        if (2 == sscanf(buf,"content-range: bytes " OFF_T_PF "-" OFF_T_PF "/",&from,&to)) {
          rf->offset = from; rf->block_left = to - from + 1; gotr = 1;
        }
      }
      if (!gotr) {
        fprintf(stderr,"got multipart/byteranges but no Content-Range?");
        return -1;
      }
      rf->rangesdone++;
    }
  }
  /* Now the easy bit - we are reading a block */
  if (!rf->block_left) return 0;
  *offset = rf->offset;

  for (;;) {
    size_t rl = rf->block_left;
    /* Note that we do not use n to test EOF - that is implicit in setting rl
     * to min(rl,buf_end-buf_start), as buf_end-buf_start == 0 iff EOF */

    /* We want to send rf->block_left to the caller, but we may have less in the buffer, and they may have less buffer space, so reduce appropriately */
    if (rl > dlen) rl = dlen;
    if (rf->buf_end - rf->buf_start < rl) {
      rl = rf->buf_end - rf->buf_start;

      /* If we have exhausted the buffer, get more data.
       * If we don't get data, drop through and return what we have got.
       * If we do, back to top of loop and recalculate how much to return to caller.
       */
      if (!rl && get_more_data(rf) > 0) continue;
    }

    if (!rl)
      return bytes_to_caller;

    /* Copy as much as we can to their buffer, freeing space in rf->buf */
    memcpy(data, &(rf->buf[rf->buf_start]), rl);
    rf->buf_start += rl; /* Track pos in our buffer... */
    data += rl; dlen -= rl; /* ...and caller's */
    bytes_to_caller += rl; /* ...and the return value */

    /* Keep track of our location in the stream */
    rf->block_left -= rl;
    rf->offset += rl;

  }
}

off_t range_fetch_bytes_down(const struct range_fetch* rf)
{ return rf->bytes_down; }

void range_fetch_end(struct range_fetch* rf) {
  if (rf->sd != -1) close(rf->sd);
  free(rf->ranges_todo);
  free(rf->boundary);
  free(rf->url);
  free(rf->cport);
  free(rf->chost);
  free(rf);
}
