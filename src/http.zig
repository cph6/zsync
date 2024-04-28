const std = @import("std");
const common = @import("common.zig");

// FILE* http_get(const char* orig_url, char** track_referer, const char* tfname);
pub export fn http_get(orig_url: common.ConstCString, track_referer: [*c]common.CString, tfname: common.ConstCString) [*c]common.c.FILE {
    // TODO: Allow using local .zsync file
    // TODO: Add proxy handling
    // TODO: Add auth handling
    _ = tfname;

    const raw_url = std.mem.span(orig_url);

    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = gpa.allocator();
    defer _ = gpa.deinit();

    var client = std.http.Client{
        .allocator = allocator,
    };
    defer client.deinit();

    var result_buffer = std.ArrayList(u8).init(allocator);
    defer result_buffer.deinit();

    const result = client.fetch(
        .{
            .location = .{ .url = raw_url },
            .method = .GET,
            .response_storage = .{ .dynamic = &result_buffer },
            // TODO: Allow redirects, remember to set track_referer to new host
            .redirect_behavior = .not_allowed,
        },
    ) catch |err| {
        std.debug.print("{}\n", .{err});
        return null;
    };

    if (track_referer != null) {
        track_referer.* = common.c.strdup(orig_url);
    }

    _ = result;

    const output_file = common.c.tmpfile();
    _ = common.c.fwrite(result_buffer.items.ptr, 1, result_buffer.items.len, output_file);
    _ = common.c.rewind(output_file);

    return output_file;
}
