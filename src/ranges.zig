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

fn ResponseReder(buffer_size: usize) type {
    return struct {
        const Self = @This();

        reader: std.http.Client.Request.Reader,
        buffer: [buffer_size]u8 = undefined,
        bytes_in_buffer: usize = 0,

        fn init(reader: std.http.Client.Request.Reader) Self {
            return Self{
                .reader = reader,
            };
        }

        fn refill(self: *Self) !void {
            self.bytes_in_buffer += try self.reader.read(self.buffer[self.bytes_in_buffer..]);
        }

        fn skip(self: *Self, bytes_to_skip: usize) void {
            self.bytes_in_buffer -= bytes_to_skip;
            std.mem.copyForwards(u8, &self.buffer, self.buffer[bytes_to_skip..]);
        }

        fn get(self: *const Self) []const u8 {
            return self.buffer[0..self.bytes_in_buffer];
        }

        fn peek(self: *const Self, bytes: []const u8) bool {
            return std.mem.startsWith(u8, &self.buffer, bytes);
        }

        fn peekAndSkip(self: *Self, bytes: []const u8) !void {
            if (self.peek(bytes)) {
                self.skip(bytes.len);
            } else {
                return error.MalformedMultipartPayload;
            }
        }
    };
}

const RangeFetchResult = struct {
    start: usize,
    end: usize,
    data: std.ArrayList(u8),

    fn init(allocator: std.mem.Allocator, start: usize, end: usize) !RangeFetchResult {
        var result = RangeFetchResult{
            .start = start,
            .end = end,
            .data = std.ArrayList(u8).init(allocator),
        };
        try result.data.ensureTotalCapacity(start + end + 1);
        return result;
    }

    fn deinit(self: *RangeFetchResult) void {
        self.data.deinit();
    }
};

const range_fetch = anyopaque;

// struct range_fetch;
const RangeFetch = struct {
    const Self = @This();
    allocator: std.mem.Allocator,

    uri_backing_storage: []u8,
    uri: std.Uri,

    client: std.http.Client,

    // /* Byte ranges to fetch */
    ranges_todo: std.ArrayList(Range),
    current_range: usize = 0,

    buffer: std.ArrayList(RangeFetchResult),
    offset_in_buffer: usize = 0,
    downloaded_bytes: usize = 0,

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
            .buffer = std.ArrayList(RangeFetchResult).init(allocator),
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
        // TODO remove from buffer if exists
        self.allocator.free(self.uri_backing_storage);
    }

    fn buildRangeHeader(ranges: []const Range, buffer: []u8) ![]const u8 {
        var range_header_value = try std.fmt.bufPrint(buffer, "bytes={}-{}", .{ ranges[0].start, ranges[0].end });
        var current_offset = range_header_value.len;

        if (ranges.len > 1) {
            for (ranges[1..ranges.len]) |range| {
                range_header_value = try std.fmt.bufPrint(buffer[current_offset..buffer.len], ", {}-{}", .{ range.start, range.end });
                current_offset += range_header_value.len;
            }
        }

        return buffer[0..current_offset];
    }

    fn fetch(self: *Self, ranges: []const Range) !usize {
        var range_header_buffer: [1024]u8 = undefined;
        const range_header_value = try RangeFetch.buildRangeHeader(ranges, &range_header_buffer);

        var response_header_buffer: [16 * 1024]u8 = undefined;

        var request = try self.client.open(
            .GET,
            self.uri,
            .{
                .server_header_buffer = &response_header_buffer,
                .redirect_behavior = .not_allowed,
                .headers = .{},
                .extra_headers = &[_]std.http.Header{
                    .{
                        .name = "Range",
                        .value = range_header_value,
                    },
                },
                .keep_alive = true,
            },
        );

        defer request.deinit();
        try request.send();

        try request.finish();
        try request.wait();

        const response = &request.response;
        if (response.status != std.http.Status.partial_content) {
            std.debug.panic("Unexpected result code: {}", .{response.status});
        }

        if (std.mem.startsWith(u8, response.content_type orelse "", "multipart/byteranges")) {
            return try self.handleMultipartResponse(response.content_type.?, request.reader());
        } else {
            if (ranges.len > 1) {
                @panic("Received single part response for multipart request");
            }
            // TODO: Comapre content-length and expected range length
            try self.buffer.append(try RangeFetchResult.init(self.allocator, ranges.ptr[0].start, 0));
            try request.reader().readAllArrayList(&self.buffer.items[self.buffer.items.len - 1].data, 1024 * 1024 * 1024);
            return response.content_length.?;
        }
    }

    fn handleMultipartResponse(self: *Self, content_type: []const u8, reader: std.http.Client.Request.Reader) !usize {
        var downloaded_bytes: usize = 0;

        const boundary_marker = "boundary=";
        const boundary_pos = std.mem.indexOf(u8, content_type, boundary_marker) orelse return error.MalformedContentType;
        const boundary = content_type[boundary_pos + boundary_marker.len ..];

        std.log.debug("Detected boundary: {s}\r\n", .{boundary});

        var response_reader = ResponseReder(256).init(reader);

        while (true) {
            try response_reader.refill();

            try response_reader.peekAndSkip("--");
            try response_reader.peekAndSkip(boundary);

            if (response_reader.peek("--\r\n")) {
                response_reader.skip(4);
                break;
            } else try response_reader.peekAndSkip("\r\n");

            try response_reader.refill();

            var part_header_buffer: [16 * 1024]u8 = undefined;
            var parser = std.http.protocol.HeadersParser.init(&part_header_buffer);
            _ = try parser.checkCompleteHead("\r\n");
            while (true) {
                const consumed_header_bytes = try parser.checkCompleteHead(response_reader.get());

                response_reader.skip(consumed_header_bytes);
                try response_reader.refill();

                if (consumed_header_bytes < response_reader.buffer.len) {
                    break;
                }
            }

            var headers_it = std.http.HeaderIterator.init(part_header_buffer[0 .. parser.header_bytes_len + 2]);
            const range_start, const range_end = try RangeFetch.parseHeaders(&headers_it);

            // both ends inclusive => +1
            const range_size = range_end - range_start + 1;
            var bytes_to_read = range_size;

            try self.buffer.append(try RangeFetchResult.init(self.allocator, range_start, range_end));
            const result = &self.buffer.items[self.buffer.items.len - 1];

            while (bytes_to_read > 0) {
                const read_size = @min(response_reader.get().len, bytes_to_read);
                bytes_to_read -= read_size;
                try result.data.appendSlice(response_reader.get()[0..read_size]);
                response_reader.skip(read_size);
                try response_reader.refill();
            }
            downloaded_bytes += range_size;
            try response_reader.peekAndSkip("\r\n");
        }

        if (response_reader.bytes_in_buffer != 0) return error.LeftoverBytesInBuffer;
        return downloaded_bytes;
    }

    fn parseHeaders(it: *std.http.HeaderIterator) !struct { usize, usize } {
        var range_start: ?usize = null;
        var range_end: ?usize = null;

        while (it.next()) |header| {
            std.log.debug("Header: {s} Value: {s}\r\n", .{ header.name, header.value });
            if (std.mem.eql(u8, header.name, "Content-Range")) {
                if (!std.mem.startsWith(u8, header.value, "bytes ")) @panic("Malformed Content-Range header");

                const dash_pos = std.mem.indexOfScalar(u8, header.value[6..], '-') orelse @panic("Malformed Content-Range header");
                const slash_pos = std.mem.indexOfScalar(u8, header.value[6..], '/') orelse @panic("Malformed Content-Range header");

                range_start = try std.fmt.parseInt(usize, header.value[6 .. 6 + dash_pos], 10);
                range_end = try std.fmt.parseInt(usize, header.value[6 + dash_pos + 1 .. 6 + slash_pos], 10);
            }
        }

        std.log.debug("Range start: {d}", .{range_start.?});
        std.log.debug("Range end: {d}", .{range_end.?});

        return .{ range_start.?, range_end.? };
    }

    fn fetchMoreRanges(self: *Self) void {
        const ranges_left_to_fetch = self.ranges_todo.items.len - self.current_range;
        const ranges_to_fetch = @min(10, ranges_left_to_fetch);

        const fetched_bytes = self.fetch(self.ranges_todo.items[self.current_range .. self.current_range + ranges_to_fetch]) catch unreachable;
        std.log.debug("Fetched {} bytes \r\n", .{fetched_bytes});
        self.downloaded_bytes += fetched_bytes;
        self.current_range += ranges_to_fetch;
    }

    fn getDataFromBuffer(self: *Self, output_buffer: []u8) struct { usize, usize } {
        if (self.buffer.items.len == 0) {
            if (self.current_range < self.ranges_todo.items.len) {
                self.fetchMoreRanges();
            } else {
                return .{ 0, 0 };
            }
        }

        const current_buffer = &self.buffer.items[0];

        const data_remaining_in_buffer = current_buffer.data.items.len - self.offset_in_buffer;
        const data_to_read = @min(data_remaining_in_buffer, output_buffer.len);

        @memcpy(output_buffer[0..data_to_read], current_buffer.data.items[self.offset_in_buffer .. self.offset_in_buffer + data_to_read]);

        const read_offset = current_buffer.start + self.offset_in_buffer;
        self.offset_in_buffer += data_to_read;

        if (self.offset_in_buffer == current_buffer.data.items.len) {
            current_buffer.deinit();
            _ = self.buffer.orderedRemove(0);
            self.offset_in_buffer = 0;
        }
        return .{ read_offset, data_to_read };
    }

    fn addRanges(self: *Self, ranges: []c_long) !void {
        for (0..(ranges.len / 2)) |i| {
            try self.ranges_todo.append(Range{
                .start = @intCast(ranges[2 * i]),
                .end = @intCast(ranges[2 * i + 1]),
            });
        }
    }

    fn castFromOpaque(rf: ?*range_fetch) !*Self {
        if (rf == null) return error.Nullptr;
        return @alignCast(@ptrCast(rf));
    }
};

// struct range_fetch* range_fetch_start(const char* orig_url);
pub export fn range_fetch_start(orig_url: common.ConstCString) ?*range_fetch {
    const result = gpa.allocator().create(RangeFetch) catch return null;
    errdefer {
        gpa.allocator().destroy(result);
    }

    result.* = RangeFetch.init(gpa.allocator(), std.mem.span(orig_url)) catch return null;

    return result;
}

// void range_fetch_addranges(struct range_fetch* rf, off_t* ranges, int nranges);
pub export fn range_fetch_addranges(rf: ?*range_fetch, ranges: [*c]common.off_t, nranges: c_int) void {
    const rf_impl = RangeFetch.castFromOpaque(rf) catch unreachable;

    const ranges_count: usize = @intCast(nranges);
    const input_ranges = ranges[0 .. 2 * ranges_count];

    rf_impl.*.addRanges(input_ranges) catch unreachable;
}

// int get_range_block(struct range_fetch* rf, off_t* offset, unsigned char* data, size_t dlen, const char *referer);
pub export fn get_range_block(rf: ?*range_fetch, offset: [*c]common.off_t, data: [*c]u8, dlen: usize, referer: common.ConstCString) c_int {
    const rf_impl = RangeFetch.castFromOpaque(rf) catch unreachable;

    _ = referer;

    const data_slice = data[0..dlen];
    const bytes_offset, const bytes_read = rf_impl.*.getDataFromBuffer(data_slice);

    offset.* = @intCast(bytes_offset);
    return @intCast(bytes_read);
}

// off_t range_fetch_bytes_down(const struct range_fetch* rf);
pub export fn range_fetch_bytes_down(rf: ?*const range_fetch) common.off_t {
    const rf_impl = RangeFetch.castFromOpaque(@constCast(rf)) catch unreachable;

    return @intCast(rf_impl.*.downloaded_bytes);
}

// void range_fetch_end(struct range_fetch* rf);
pub export fn range_fetch_end(rf: ?*range_fetch) void {
    const rf_impl = RangeFetch.castFromOpaque(rf) catch unreachable;

    rf_impl.*.deinit();
    gpa.allocator().destroy(rf_impl);
    // _ = gpa.deinit();
}

test "Test RangeFetch.buildRangeHeader single range" {
    var test_buffer: [1024]u8 = undefined;

    const single_range = try RangeFetch.buildRangeHeader(
        &[_]Range{.{ .start = 100, .end = 200 }},
        &test_buffer,
    );

    try std.testing.expectEqualStrings("bytes=100-200", single_range);
}

test "Test RangeFetch.buildRangeHeader multiple ranges" {
    var test_buffer: [1024]u8 = undefined;

    const single_range = try RangeFetch.buildRangeHeader(
        &[_]Range{
            .{ .start = 100, .end = 200 },
            .{ .start = 300, .end = 400 },
            .{ .start = 500, .end = 600 },
        },
        &test_buffer,
    );

    try std.testing.expectEqualStrings("bytes=100-200, 300-400, 500-600", single_range);
}
