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

#include <string.h>
#include <stdlib.h>

/* Very crude URL parsing */

#include "url.h"

char* __attribute__((pure)) get_host_port(const char* url, char* hostn, int hnlen, int* port)
{
  char *p;
  char *q = strstr(url,"://");
  /* Must parse the url to get the hostname */
  if (!q) return NULL;
  q+=3;
  
  p = strchr(q,':');
  if (!p) { *port = 80; p = strchr(q,'/'); }
  else { *port = atoi(p+1); }
  
  if (!p) return NULL;
  
  if (p-q < hnlen-1) {
    memcpy(hostn,q,p-q);
    hostn[p-q] = 0;
  }
  
  if (*p == ':') p = strchr(p,'/');
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
