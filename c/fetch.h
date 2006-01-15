extern long long http_down;
extern int blocksize;

struct gzblock {
  long inbitoffset;
  long outbyteoffset;
};

int fetch_remaining_blocks_zlib_http(struct zsync_state* z, const char* url, struct gzblock* zblock, int nzblocks);
int fetch_remaining_blocks_http(struct zsync_state* z, const char* url, int maxblocks);
