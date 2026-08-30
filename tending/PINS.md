# Toolchain pins

Pins for the [bindings/](../bindings) examples only — the library itself
needs nothing beyond a C compiler.

- Zig 0.16.0 (stable), macos aarch64
  - url: https://ziglang.org/download/0.16.0/zig-aarch64-macos-0.16.0.tar.xz
  - sha256: b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489 (matches the shasum in ziglang.org/download/index.json)
  - unpacked at: dist/zig-aarch64-macos-0.16.0/ (gitignored; re-fetch by url+sha)
  - invoke: dist/zig-aarch64-macos-0.16.0/zig

- Odin dev-2026-08 (latest release tag), macos arm64
  - url: https://github.com/odin-lang/Odin/releases/download/dev-2026-08/odin-macos-arm64-dev-2026-08.tar.gz
  - sha256: 941b3bf1d930908a63214df7b3ebeff955ef33c20d3c123b417526fa2caf3205 (matches the release asset's published digest)
  - unpacked at: dist/odin-macos-arm64-nightly+2026-08-06/ (the tarball's own root name; gitignored; re-fetch by url+sha)
  - invoke: dist/odin-macos-arm64-nightly+2026-08-06/odin (reports dev-2026-08-nightly:902106f)
