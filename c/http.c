/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004 Colin Phipps <cph@moria.org.uk>
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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "http.h"
#include "url.h"

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

FILE* http_open(const char* orig_url, const char* extraheader, int require_code, char** track_referer)
{
  int allow_redirects = 5;
  char* url = strdup(orig_url);
  FILE* f = NULL;
      
  for (;allow_redirects-- && url && !f;) {
    char hostn[256];
    const char* connecthost;
    char *p;
    char *port;

    if ( (p = get_host_port(url,hostn,sizeof(hostn),&port)) == NULL) break;
    if (!proxy) {
      connecthost = hostn;
    } else {
      connecthost = proxy;
      port = pport;
    }
    {
      int sfd = connect_to(connecthost, port);
      int code;

      if (sfd == -1) break;

      {
	char buf[1024];
	snprintf(buf, sizeof(buf), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: zsync %s\r\n%s%s\r\n",
		 proxy ? url : p, hostn, VERSION,
		 extraheader ? extraheader : "",
		 extraheader ? "\r\n" : ""
		 );
	if (send(sfd,buf,strlen(buf),0) == -1) {
	  perror("sendmsg"); close(sfd); break;
	}
      }
      f = http_get_stream(sfd, &code);

      if (!f) break;
      if (code == 301 || code == 302) {
	char *oldurl = url;
	url = get_location_url(f, oldurl);
	free(oldurl);
	fclose(f); f = NULL;
      } else if (code != require_code) {
	fclose(f); f = NULL;
      }
    }
  }

  if (!f)
    fprintf(stderr,"failed on url %s\n",url ? url : "(missing redirect)");
  if (track_referer)
    *track_referer = url;
  return f;
}

/* HTTP Range: / 206 response interface 
 * 
 * If we are reading a multipart/byteranges, boundary is set.
 * If we are in the middle of an actual block, block_left is non-zero and offset gives the remembered offset.
 */

struct range_fetch {
  char* boundary;
  char* url;
  char hostn[256];

  char* chost;
  char* cport;

  size_t block_left;
  long long offset;
  int sd;
  char buf[4096];
  int buf_start, buf_end;
  long long bytes_down;
  int server_close;

  long long* ranges_todo;
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

  if (!rf) return NULL;
  p = get_host_port(orig_url, rf->hostn, sizeof(rf->hostn), &(rf->cport));
  if (!p) { free(rf); return NULL; }
  
  if (proxy) {
    // URL must be absolute; throw away cport and get port for proxy
    rf->url = strdup(orig_url);
    rf->cport = pport;
    rf->chost = proxy;
  } else {
    // cport already set; set url to relative part and chost to the target
    rf->url = strdup(p);
    rf->chost = rf->hostn;
  }

  rf->block_left = 0;
  rf->bytes_down = 0;
  rf->boundary = NULL;

  rf->buf_start = rf->buf_end = 0;

  rf->sd = -1;

  rf->ranges_todo = NULL; rf->nranges = rf->rangesdone = 0;

  return rf;
}

void range_fetch_addranges(struct range_fetch* rf, long long* ranges, int nranges)
{
  int existing_ranges = rf->nranges - rf->rangesdone;
  long long* nr = malloc(2*sizeof(*ranges)*(nranges + existing_ranges));
  
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
  if (rf->sd == -1) perror("connect");
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
	   "Accept-Ranges: bytes\r\nRange: bytes=",
	   rf->url, rf->hostn,
	   referer ? "\r\nReferer: " : "", referer ? referer : ""
	   );
  
  /* The for loop here is just a sanity check, lastrange is the real loop control */
  for (; rf->rangessent < rf->nranges; ) {
    int i = rf->rangessent;
    int lastrange = 0;

    l = strlen(request);
    if (l > 1200 || !(--max_range_per_request) || i == rf->nranges-1) lastrange = 1;
    
    snprintf(request + l, sizeof(request)-l, "%lld-%lld%s", rf->ranges_todo[2*i], rf->ranges_todo[2*i+1], lastrange ? "" : ",");

    rf->rangessent++;
    if (lastrange) break;
  }
  l = strlen(request);
  snprintf(request + l, sizeof(request)-l, "\r\n%s\r\n", rf->rangessent == rf->nranges ? "Connection: close\r\n" : "");
  
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
  while(c = *s) {
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
    if ((c = atoi(p+1)) != 206) {
      fprintf(stderr,"bad status code %d\n",c);
      return -1;
    }
    if (*(p-1) == '0') { /* HTTP/1.0 server? */
      rf->server_close = 1;
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
      unsigned long long from,to;
      sscanf(p,"bytes %llu-%llu/",&from,&to);
      if (from <= to) {
	rf->block_left = to + 1 - from;
	rf->offset = from;
      }
      /* Can only have got one range. */
      rf->rangesdone++;
      rf->rangessent = rf->rangesdone;
    }
    if (!strcmp(buf,"connection") && !strcmp(p,"close")) {
      rf->server_close = 1;
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

int get_range_block(struct range_fetch* rf, long long* offset, unsigned char* data, size_t dlen)
{
  size_t bytes_to_caller = 0;

  if (!rf->block_left) {
check_boundary:
    if (!rf->boundary) {
      int newconn = 0;
      int header_result;

      if (rf->sd != -1 && rf->server_close) {
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
      //      fprintf(stderr,"boundary %s comparing to %s\n",rf->boundary,buf);
      if (memcmp(&buf[2],rf->boundary,strlen(rf->boundary))) {
	fprintf(stderr,"got bad block boundary: %s != %s",rf->boundary, buf);
	return -1; /* This is an error now */
      }
      /* Look for last record marker */
      if (buf[2+strlen(rf->boundary)] == '-') { free(rf->boundary); rf->boundary = NULL; goto check_boundary; }
      
      for(;buf[0] != '\r' && buf[0] != '\n' && buf[0] != '\0';) {
	int from, to;
	if (!rfgets(buf,sizeof(buf),rf)) return 0;
	buflwr(buf);
	if (2 == sscanf(buf,"content-range: bytes %d-%d/",&from,&to)) {
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

long long range_fetch_bytes_down(const struct range_fetch* rf)
{ return rf->bytes_down; }

void range_fetch_end(struct range_fetch* rf) {
  if (rf->sd != -1) close(rf->sd);
  free(rf->ranges_todo);
  free(rf->boundary);
  free(rf);
}
