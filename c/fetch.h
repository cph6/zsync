extern long long http_down;
extern int blocksize;

int fetch_remaining_blocks_zlib_http(struct zsync_state* z, const char* url, const struct zmap* zm);
int fetch_remaining_blocks_http(struct zsync_state* z, const char* url, int maxblocks);
