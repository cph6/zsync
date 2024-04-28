#ifndef ZIG_RANGES_H
#define ZIG_RANGES_H
#include <sys/types.h>


struct range_fetch;

struct range_fetch* range_fetch_start(const char* orig_url);
void range_fetch_addranges(struct range_fetch* rf, off_t* ranges, int nranges);
int get_range_block(struct range_fetch* rf, off_t* offset, unsigned char* data, size_t dlen, const char *referer);
off_t range_fetch_bytes_down(const struct range_fetch* rf);
void range_fetch_end(struct range_fetch* rf);


#endif