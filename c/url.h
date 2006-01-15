
char* __attribute__((pure)) get_host_port(const char* url, char* hostn, int hnlen, int* port);

char* __attribute__((pure)) make_url_absolute(const char* base, const char* url);
