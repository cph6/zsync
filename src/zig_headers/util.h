#ifndef ZIG_UTIL_H
#define ZIG_UTIL_H

char* base64(const char*);
int set_proxy_from_string(const char* s);
void add_auth(char* host, char* user, char* pass);

#endif