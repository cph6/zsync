struct gzblock {
  long inbitoffset;
  long outbyteoffset;
};

struct zmap;
struct z_stream_s;

struct zmap* make_zmap(const struct gzblock* zb, int n);
int map_to_compressed_ranges(const struct zmap* zm, long long* zbyterange, int maxout, long long* byterange, int nrange);
void configure_zstream_for_zdata(const struct zmap* zm, struct z_stream_s* zs, long zoffset, long long* poutoffset);
