/*
 *   zsync - client side rsync over http
 *   Copyright (C) 2004,2005 Colin Phipps <cph@moria.org.uk>
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

#include <string.h>
#include <stdlib.h>

#include "config.h"

/* Very crude URL parsing */

#include "url.h"

char* get_host_port(const char* url, char* hostn, int hnlen, char** port)
{
  char *p;
  char *q = strstr(url,"://");
  /* Must parse the url to get the hostname */
  if (!q) return NULL;
  q+=3;
  
  p = strchr(q,':');
  if (p) { /* if : is after teh first /, we have looked too far ahead */
    char *r = strchr(q,'/');
    if (r && r < p) p = NULL;
  }
  if (!p) { *port = strdup("http"); p = strchr(q,'/'); }
  
  if (!p) return NULL;
  
  if (p-q < hnlen-1) {
    memcpy(hostn,q,p-q);
    hostn[p-q] = 0;
  }
  
  if (*p == ':') {
    int l;
    q = p;
    p = strchr(p,'/');
    l = p ? p-q-1 : strlen(q)-1;
    *port = malloc(l+1);
    if (!*port) return NULL;
    memcpy(*port,q+1,l);
    (*port)[l] = 0;
    if (!p) p = strdup("/");
  }
  return p;
}

static const char special[] = { ":/?" };

char* __attribute__((pure)) make_url_absolute(const char* base, const char* url) {
  int n = strcspn(url,special);

  if (n == 0 && *url == '/' && base) {
    /* Full path specified */
    char *p = strchr(base,':');
    if (!p) return NULL;

    if (p[1] != '/' || p[2] != '/') return NULL;
    p = strchr(p+3,'/');
    if (p) {
      int l = p-base;
      char *newurl = malloc(l + strlen(url) + 1);

      /* assert */
      if (base[l] != '/') return NULL;

      memcpy(newurl,base,l);
      strcpy(newurl+l,url);
      return newurl;
    }
  }

  /* aaaa: - has scheme, probably absolute. More work here? */
  if (n > 0 && url[n] == ':') { return strdup(url); }

  if (n > 0 && base) { /* No leading / or scheme - relative */
    char *q;
    char *p = strchr(base,'?');
    if (!p) p = strchr(base,'#');
    if (!p) p = base+strlen(base);

    for (q = p; q > base && *q != '/'; q--) ;

    if (*q == '/') {
      int l = q-base+1;
      char *newurl = malloc(l + strlen(url) + 1);

      /* assert */
      if (base[l-1] != '/') return NULL;

      memcpy(newurl,base,l);
      strcpy(newurl+l,url);
      return newurl;
    }
  }
  return NULL;
}
