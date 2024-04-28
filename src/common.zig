pub const c = @cImport({
    @cInclude("stdlib.h");
    @cInclude("stdio.h");
    @cInclude("string.h");
});

pub const ConstCString = [*c]const u8;
pub const CString = [*c]u8;
pub const off_t = c_long;
