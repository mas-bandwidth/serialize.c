# Security Policy

serialize.c is a bitpacking serialization library. Its read path consumes buffers that
in practice arrive from the network, so a malformed or hostile stream must not be able to
read out of bounds, allocate without bound, or continue after a failure it
should have latched.

## Reporting a vulnerability

**Please do not report security issues in public GitHub issues or pull requests.**

Report privately through either channel:

- **GitHub private vulnerability reporting** (preferred): on this repository, go to the
  **Security** tab → **Report a vulnerability**. This opens a private advisory visible
  only to the maintainers.
- **Email**: glenn@mas-bandwidth.com.

Please include enough detail to reproduce: the affected version or commit, a description
of the flaw, and — where possible — a proof-of-concept buffer or a small patch. Fuzzing
crash artifacts are ideal.

We will acknowledge your report, keep you updated on our assessment, and coordinate
disclosure timing with you. We prefer coordinated disclosure and will credit reporters
who wish to be named.

## Scope

In scope — bugs in the library itself (`serialize.c` and `serialize.h`).

Especially of interest, in the read path reachable from a hostile buffer:

- **Out-of-bounds reads** past the end of a stream.
- **Integer overflow** in bit or byte counts, particularly where a count derived from
  wire data feeds an index or a length calculation.
- **Unbounded allocation** — any way for a serialized length or array count to drive an
  allocation without first being bounds-checked against what remains in the buffer. In C
  the caller owns every buffer, so the relevant form here is a length or count from the
  wire driving an index or a copy without being checked against what remains.
- **Any read that touches memory outside the reader's allocation contract.** The reader
  loads whole 8-byte windows unconditionally, the same read path as the C++ library, so
  the allocation backing the buffer must extend at least 8 bytes past the end of the
  data. Near the end of a stream the final window begins inside the last bytes and
  reaches past them. Those bytes are loaded and never interpreted: poison there changes
  no decoded value and no refusal. A read that touches memory beyond that slack is a bug
  in the library. Handing the reader an allocation without the slack is caller error,
  and a payload received into an exactly sized allocation is read through
  `serialize_read_stream_init_padded`, which copies it into a caller-supplied
  destination and zeroes the slack — and which refuses, in every build, a destination
  too small to hold both, copying nothing and handing back a failed stream rather than
  writing past the end of it.
- **A failure that is not sticky.** Once a stream fails, every subsequent operation must
  fail without touching the buffer. A path that resumes writing or reading after an
  error can produce a partially-decoded structure that the caller believes is whole.
- **Divergence from the C++, C#, Go or Rust ports** on the same input. The four are meant to
  be bit-identical; if this one accepts a stream another refuses, that is a security bug
  in a deployment that mixes languages across client and server, which is the normal
  case.

## Out of scope

- **Transport-level concerns.** This is a wire-format library, not a transport. Replay,
  spoofing, amplification, rate limiting and authentication belong to the layer above.
- **Confidentiality.** The library performs no encryption and no authentication. It is
  normally used underneath a layer that authenticates. That does not put the items above
  out of scope — a stream is only trustworthy if the layer above actually verified it,
  and we would rather serialize.c be safe on its own.
- **Write-side misuse.** Writing a value outside its declared range is a caller bug.

## Supported versions

The latest tagged release is supported. There are no long-term support branches; a fix
lands on `main` and in the next tag.
