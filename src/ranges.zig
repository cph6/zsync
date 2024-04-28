const std = @import("std");
const common = @import("common.zig");

var gpa = std.heap.GeneralPurposeAllocator(.{}){};

const Range = struct {
    start: usize,
    end: usize,

    fn size(self: *const Range) usize {
        return self.end - self.start;
    }
};

// struct range_fetch;
const RangeFetch = struct {
    const Self = @This();
    allocator: std.mem.Allocator,

    uri_backing_storage: []u8,
    uri: std.Uri,

    client: std.http.Client,

    // char *boundary; /* If we're in the middle of reading a mime/multipart response, this is the boundary string.

    // /* State for block currently being read */
    // size_t block_left;  /* non-zero if we're in the middle of reading a block */
    // off_t offset;       /* and this is the offset of the start of the block we are reading */

    // /* Buffering of data from the remote server */
    // char buf[4096];
    // int buf_start, buf_end; /* Bytes buf_start .. buf_end-1 in buf[] are valid */

    // /* Keep count of total bytes retrieved */
    // off_t bytes_down;

    // int server_close; /* 0: can send more, 1: cannot send more (but one set of headers still to read), 2: cannot send more and all existing headers read */

    // /* Byte ranges to fetch */
    ranges_todo: std.ArrayList(Range),
    current_range: usize = 0,

    buffer: std.ArrayList(u8),
    buffer_offset: usize = 0,
    offset_in_buffer: usize = 0,

    // int rangessent;     /* We've requested the first rangessent ranges from the remote */
    // int rangesdone;     /* and received this many */

    fn init(allocator: std.mem.Allocator, url: []const u8) !Self {
        const url_dup = try allocator.dupe(u8, url);
        const uri = try std.Uri.parse(url_dup);

        return Self{
            .allocator = allocator,
            .uri_backing_storage = url_dup,
            .uri = uri,
            .client = std.http.Client{
                .allocator = allocator,
            },
            .ranges_todo = std.ArrayList(Range).init(allocator),
            .buffer = std.ArrayList(u8).init(allocator),
        };

        // /* If going through a proxy, we can immediately set up the host and port to
        // * connect to */
        // if (proxy) {
        //     rf->cport = strdup(pport);
        //     rf->chost = strdup(proxy);
        // }
    }

    fn deinit(self: *Self) void {
        self.client.deinit();
        self.ranges_todo.deinit();
        self.buffer.deinit();
    }

    fn fetchNextRange(self: *Self) void {
        self.offset_in_buffer = 0;

        const current_range = &self.ranges_todo.items[self.current_range];
        self.buffer_offset = current_range.start;
        self.buffer.clearRetainingCapacity();

        var range_buffer: [128]u8 = undefined;
        const range_value = std.fmt.bufPrint(&range_buffer, "{}-{}", .{ current_range.start, current_range.end }) catch unreachable;

        _ = self.client.fetch(.{
            .extra_headers = &[_]std.http.Header{
                .{
                    .name = "Range",
                    .value = range_value,
                },
            },
            .location = .{ .uri = self.uri },
            .redirect_behavior = .not_allowed,
            .method = .GET,
            .response_storage = .{ .dynamic = &self.buffer },
            .max_append_size = 1 * 1024 * 1024 * 1024,
        }) catch unreachable;

        self.current_range += 1;
    }

    fn getDataFromBuffer(self: *Self, output_buffer: []u8) struct { usize, usize } {
        const data_remaining_in_buffer = self.buffer.items.len - self.offset_in_buffer;
        const data_to_read = @min(data_remaining_in_buffer, output_buffer.len);

        @memcpy(output_buffer[0..data_to_read], self.buffer.items[self.offset_in_buffer .. self.offset_in_buffer + data_to_read]);

        const offset = self.buffer_offset + self.offset_in_buffer;
        self.offset_in_buffer += data_to_read;

        return .{ offset, data_to_read };
    }
};

const range_fetch = anyopaque;

// struct range_fetch* range_fetch_start(const char* orig_url);
pub export fn range_fetch_start(orig_url: common.ConstCString) ?*range_fetch {
    var allocator = gpa.allocator();

    const result = allocator.create(RangeFetch) catch return null;
    errdefer {
        allocator.destroy(result);
    }

    result.* = RangeFetch.init(allocator, std.mem.span(orig_url)) catch return null;

    return result;
}

// void range_fetch_addranges(struct range_fetch* rf, off_t* ranges, int nranges);
pub export fn range_fetch_addranges(rf: ?*range_fetch, ranges: [*c]common.off_t, nranges: c_int) void {
    if (rf == null) @panic("range_fetch_addranges: rf == null");
    const rf_impl: *RangeFetch = @alignCast(@ptrCast(rf));

    const ranges_count: usize = @intCast(nranges);
    const input_ranges = ranges[0 .. 2 * ranges_count];

    for (0..ranges_count) |i| {
        rf_impl.ranges_todo.append(Range{
            .start = @intCast(input_ranges[2 * i]),
            .end = @intCast(input_ranges[2 * i + 1]),
        }) catch unreachable;
    }
}

// int get_range_block(struct range_fetch* rf, off_t* offset, unsigned char* data, size_t dlen, const char *referer);
pub export fn get_range_block(rf: ?*range_fetch, offset: [*c]common.off_t, data: [*c]u8, dlen: usize, referer: common.ConstCString) c_int {
    if (rf == null) @panic("range_fetch_addranges: rf == null");
    const rf_impl: *RangeFetch = @alignCast(@ptrCast(rf));

    const data_slice = data[0..dlen];

    if (rf_impl.*.offset_in_buffer < rf_impl.*.buffer.items.len) {
        // some data in buffer, return it
        const bytes_offset, const bytes_read = rf_impl.*.getDataFromBuffer(data_slice);
        offset.* = @intCast(bytes_offset);
        return @intCast(bytes_read);
    } else if (rf_impl.*.current_range < rf_impl.*.ranges_todo.items.len) {
        // buffer empty, but more ranges to get
        rf_impl.*.fetchNextRange();
        const bytes_offset, const bytes_read = rf_impl.*.getDataFromBuffer(data_slice);
        offset.* = @intCast(bytes_offset);
        return @intCast(bytes_read);
    } else {
        return 0;
    }
    const current_range = &rf_impl.*.ranges_todo.items[rf_impl.*.current_range];
    offset.* = @intCast(current_range.*.start);

    _ = referer;
    @panic("Not implemented!");
}

// off_t range_fetch_bytes_down(const struct range_fetch* rf);
pub export fn range_fetch_bytes_down(rf: *const range_fetch) common.off_t {
    _ = rf;
    return 0;
}

// void range_fetch_end(struct range_fetch* rf);
pub export fn range_fetch_end(rf: ?*range_fetch) void {
    if (rf == null) @panic("range_fetch_addranges: rf == null");
    const rf_impl: *RangeFetch = @alignCast(@ptrCast(rf));

    rf_impl.*.deinit();
    gpa.allocator().destroy(rf_impl);
}
