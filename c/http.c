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

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

int connect_to(const char* node, const char* service)
{
  struct addrinfo hint;
  struct addrinfo *ai;
  int rc;

  bzero(&hint,sizeof hint);
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

FILE* http_get_stream(int fd, const char* url, const char* hostname, const char* extraheader, int* code)
{
  {
    FILE* f = fdopen(fd, "r+");

    fprintf(f, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: zsync %s\r\n%s%s\r\n",
	    url, hostname, VERSION,
	    extraheader ? extraheader : "",
	    extraheader ? "\r\n" : ""
	    );
    fflush(f);

    {
      char buf[256];
      char *p;

      if (fgets(buf,sizeof(buf),f) == NULL || memcmp(buf, "HTTP/1", 6) != 0 || (p = strchr(buf, ' ')) == NULL) {
	*code = 0; fclose(f); return NULL;
      }

      *code = atoi(++p);

      return f;
    }
  }
}

char* get_location_url(FILE* f) {
  return NULL; // TODO
}

char *proxy = NULL;
unsigned short pport;

FILE* http_open(const char* orig_url, const char* extraheader, int require_code)
{
  int allow_redirects = 5;
  char* url = strdup(orig_url);
  FILE* f = NULL;
      
  for (;allow_redirects-- && url && !f;) {
    char hostn[256];
    const char* connecthost;
    char *p;
    int port;

    if (!proxy) {
      /* Must parse the url to get the hostname */
      if (memcmp(url,"http://",7)) break;
      
      p = strchr(url+7,':');
      if (!p) { port = 80; p = strchr(url+7,'/'); }
      else { port = atoi(p+1); }

      if (!p) break;

      memcpy(hostn,url+7,p-&url[7]);
      hostn[p-&url[7]] = 0;
      connecthost = hostn;

      if (*p == ':') p = strchr(p,'/');
      if (!p) break;
    } else {
      connecthost = proxy;
      port = pport;
    }
    {
      int sfd = connect_to(connecthost, proxy ? "webcache" : "http");
      int code;

      if (sfd == -1) break;

      f = http_get_stream(sfd, proxy ? url : p, hostn, extraheader, &code);

      if (!f) break;
      if (code == 301 || code == 302) {
	free(url);
	url = get_location_url(f);
	fclose(f); f = NULL;
      } else if (code != require_code) {
	fclose(f); f = NULL;
      }
    }
  }

  if (!f)
    fprintf(stderr,"failed on url %s\n",url ? url : "(missing redirect)");
  free(url);
  return f;
}

/* HTTP Range: / 206 response interface 
 * 
 * If we are reading a multipart/byteranges, boundary is set.
 * If we are in the middle of an actual block, block_left is non-zero and offset gives the remembered offset.
 */

struct range_fetch {
  char* boundary;
  size_t block_left;
  long long offset;
  FILE* stream;
  long long bytes_down;
};

static char* rfgets(char* buf, size_t len, struct range_fetch* rf) 
{
  char *p = fgets(buf,len,rf->stream);
  if (p) rf->bytes_down += strlen(p);
  return p;
}

struct range_fetch* range_fetch_start(const char* orig_url, long long* ranges, int nranges)
{
  struct range_fetch* rf = malloc(sizeof(struct range_fetch));

  if (!rf) return NULL;

  {
    char rangestring[512] = { "Accept-Ranges: bytes\r\nRange: bytes=" };
    int i;

    for (i=0; i<nranges; i++) {
      int l = strlen(rangestring);
      snprintf(rangestring + l, sizeof(rangestring)-l, "%lld-%lld%s", ranges[2*i], ranges[2*i+1], (i < nranges-1) ? "," : "");
    }

    rf->stream = http_open(orig_url, rangestring, 206);
    if (!rf->stream) {
      free(rf); return NULL;
    }
  }
  rf->block_left = 0;
  rf->bytes_down = 0;
  rf->boundary = NULL;
  /* From here, rf is valid and must be freed accordingly */
  {
    while (!feof(rf->stream)) {
      char buf[512];
      char *p;

      if (rfgets(buf,sizeof(buf),rf) == NULL) break;
      if (buf[0] == '\r' || buf[0] == '\0') {
	/* End of headers. We are happy provided we got the block boundary */
	if ((rf->boundary || rf->block_left) && !(rf->boundary && rf->block_left)) return rf;
	break;
      }
      p = strstr(buf,": ");
      if (!p) break;
      *p = 0; p+=2;
      /* buf is the header name, p the value */
      if (!strcmp(buf,"Content-Range")) {
	unsigned long long from,to;
	sscanf(p,"bytes %llu-%llu/",&from,&to);
	if (from <= to) {
	  rf->block_left = to + 1 - from;
	  rf->offset = from;
	}
      }
      if (!strcasecmp(buf,"Content-Type") && !strncasecmp(p,"multipart/byteranges",20)) {
	char *q = strstr(p,"boundary=");
	if (!q) break;
	rf->boundary = strdup(q+9); /* length of the above */
	q = rf->boundary + strlen(rf->boundary)-1;

	while (*q == '\r' || *q == ' ') *q-- = '\0';
      }
    }
    fclose(rf->stream);
    free(rf);
  }
  return NULL;
}

int get_range_block(struct range_fetch* rf, long long* offset, unsigned char* data, size_t dlen) {
  {
    char buf[512];
    if (!rf->block_left && rf->boundary) {
      if (!rfgets(buf,sizeof(buf),rf)) return 0;
      /* Get, hopefully, boundary marker */
      if (!rfgets(buf,sizeof(buf),rf)) return 0;
      if (buf[0] != '-' || buf[1] != '-') return 0;
      //      fprintf(stderr,"boundary %s comparing to %s\n",rf->boundary,buf);
      if (memcmp(&buf[2],rf->boundary,strlen(rf->boundary))) return 0;
      /* Look for last record marker */
      if (buf[2+strlen(rf->boundary)] == '-') return 0;
      
      for(;buf[0] != '\r' && buf[0] != '\n' && buf[0] != '\0';) {
	int from, to;
	if (!rfgets(buf,sizeof(buf),rf)) return 0;
	if (2 == sscanf(buf,"Content-range: bytes %d-%d/",&from,&to)) {
	  rf->offset = from; rf->block_left = to - from + 1;
	}
      }
    }
  }
  /* Now the easy bit - we are reading a block */
  if (!rf->block_left) return 0;
  {
    size_t rl = rf->block_left;
    if (rl > dlen) rl = dlen;
    rl = fread(data, 1, rl, rf->stream);
    if (rl <= 0) return 0;
    rf->bytes_down += rl;

    /* Successful read. Update the state fields, and return the offset and length to the caller. */
    if (offset) *offset = rf->offset;
    rf->block_left -= rl;
    rf->offset += rl;
    return rl;
  }
}

long long range_fetch_bytes_down(const struct range_fetch* rf)
{ return rf->bytes_down; }

void range_fetch_end(struct range_fetch* rf) {
  if (rf->stream) fclose(rf->stream);
  free(rf->boundary);
  free(rf);
}
