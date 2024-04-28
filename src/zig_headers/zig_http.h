#ifndef ZIG_HTTP_H
#define ZIG_HTTP_H
#include <stdio.h>


FILE* http_get(const char* orig_url, char** track_referer, const char* tfname);

#endif