
extern char *proxy;
extern unsigned short pport;

FILE* http_open(const char* orig_url, const char* extraheader, int require_code);

struct range_fetch;

struct range_fetch* range_fetch_start(const char* orig_url);
void range_fetch_addranges(struct range_fetch* rf, long long* ranges, int nranges);
int get_range_block(struct range_fetch* rf, long long* offset, unsigned char* data, size_t dlen);
long long range_fetch_bytes_down(const struct range_fetch* rf);
void range_fetch_end(struct range_fetch* rf);
