// serialize.c from Odin, through foreign import. The binding block below is
// the whole binding: declare the signatures you use, nothing else. Streams
// are opaque storage here -- every access goes through the C functions, and
// the size guards in main() keep the storage honest.
package main

import "core:fmt"
import "core:os"

foreign import serialize "shim.o"

// Opaque stream storage. Sizes are asserted against the C truth at startup.
Write_Stream :: struct #align (8) {
	_opaque: [48]u8,
}
Read_Stream :: struct #align (8) {
	_opaque: [40]u8,
}

foreign serialize {
	serialize_write_stream_init :: proc(stream: ^Write_Stream, buffer: rawptr, bytes: i32) ---
	serialize_write_int :: proc(stream: ^Write_Stream, value: i32, min: i32, max: i32) -> i32 ---
	serialize_write_bool :: proc(stream: ^Write_Stream, value: i32) -> i32 ---
	serialize_write_float :: proc(stream: ^Write_Stream, value: f32) -> i32 ---
	serialize_write_flush :: proc(stream: ^Write_Stream) ---
	serialize_write_bytes_processed :: proc(stream: ^Write_Stream) -> i32 ---

	serialize_read_stream_init :: proc(stream: ^Read_Stream, buffer: rawptr, bytes: i32) ---
	serialize_read_int :: proc(stream: ^Read_Stream, value: ^i32, min: i32, max: i32) -> i32 ---
	serialize_read_bool :: proc(stream: ^Read_Stream, value: ^i32) -> i32 ---
	serialize_read_float :: proc(stream: ^Read_Stream, value: ^f32) -> i32 ---
	serialize_read_error :: proc(stream: ^Read_Stream) -> i32 ---

	serialize_shim_sizeof_write_stream :: proc() -> i32 ---
	serialize_shim_sizeof_read_stream :: proc() -> i32 ---
}

main :: proc() {
	assert(i32(size_of(Write_Stream)) == serialize_shim_sizeof_write_stream())
	assert(i32(size_of(Read_Stream)) == serialize_shim_sizeof_read_stream())

	// Writer wants a multiple of 8 bytes; the reader's allocation must
	// extend at least 8 bytes past the data (see README, buffer contract).
	buffer: [64 + 8]u8

	// ---- write ----
	w: Write_Stream
	serialize_write_stream_init(&w, &buffer, 64)
	serialize_write_int(&w, 42, 0, 1000) // 10 bits, not 32
	serialize_write_bool(&w, 1) // 1 bit
	serialize_write_float(&w, 3.25)
	serialize_write_flush(&w)
	bytes := serialize_write_bytes_processed(&w)

	// ---- read back ----
	r: Read_Stream
	serialize_read_stream_init(&r, &buffer, bytes)
	health, at_rest: i32
	x: f32
	serialize_read_int(&r, &health, 0, 1000)
	serialize_read_bool(&r, &at_rest)
	serialize_read_float(&r, &x)
	if serialize_read_error(&r) != 0 {
		fmt.eprintln("FAILED: read error on a valid stream")
		os.exit(1)
	}
	if health != 42 || at_rest != 1 || x != 3.25 {
		fmt.eprintln("FAILED: round trip mismatch")
		os.exit(1)
	}
	fmt.printfln("round trip: %v bytes, health=%v at_rest=%v x=%v", bytes, health, at_rest, x)

	// ---- refusal: a truncated buffer must fail the read, not fake a value ----
	t: Read_Stream
	serialize_read_stream_init(&t, &buffer, bytes - 4)
	serialize_read_int(&t, &health, 0, 1000)
	serialize_read_bool(&t, &at_rest)
	if serialize_read_float(&t, &x) != 0 || serialize_read_error(&t) == 0 {
		fmt.eprintln("FAILED: truncated read was not refused")
		os.exit(1)
	}
	fmt.println("refusal: truncated read failed cleanly")
}
