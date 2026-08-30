// serialize.c from Zig, through @cImport. No wrapper, no binding layer:
// translate-c consumes serialize.h directly, static inline functions and all.
const std = @import("std");
const c = @cImport({
    @cInclude("serialize.h");
});

pub fn main() !void {
    // Writer wants a multiple of 8 bytes; the reader's allocation must
    // extend at least 8 bytes past the data (see README, buffer contract).
    var buffer: [64 + 8]u8 = undefined;

    // ---- write ----
    var w: c.serialize_write_stream_t = undefined;
    c.serialize_write_stream_init(&w, &buffer, 64);
    _ = c.serialize_write_int(&w, 42, 0, 1000); // 10 bits, not 32
    _ = c.serialize_write_bool(&w, 1); // 1 bit
    _ = c.serialize_write_float(&w, 3.25);
    c.serialize_write_flush(&w);
    const bytes = c.serialize_write_bytes_processed(&w);

    // ---- read back ----
    var r: c.serialize_read_stream_t = undefined;
    c.serialize_read_stream_init(&r, &buffer, bytes);
    var health: i32 = 0;
    var at_rest: c_int = 0;
    var x: f32 = 0;
    _ = c.serialize_read_int(&r, &health, 0, 1000);
    _ = c.serialize_read_bool(&r, &at_rest);
    _ = c.serialize_read_float(&r, &x);
    if (c.serialize_read_error(&r) != 0) return error.ReadFailed;
    if (health != 42 or at_rest != 1 or x != 3.25) return error.Mismatch;
    std.debug.print("round trip: {} bytes, health={} at_rest={} x={}\n", .{ bytes, health, at_rest, x });

    // ---- refusal: a truncated buffer must fail the read, not fake a value ----
    var t: c.serialize_read_stream_t = undefined;
    c.serialize_read_stream_init(&t, &buffer, bytes - 4);
    _ = c.serialize_read_int(&t, &health, 0, 1000);
    _ = c.serialize_read_bool(&t, &at_rest);
    if (c.serialize_read_float(&t, &x) != 0 or c.serialize_read_error(&t) == 0)
        return error.TruncationNotRefused;
    std.debug.print("refusal: truncated read failed cleanly\n", .{});
}
