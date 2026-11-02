# BearSSL in AuroraOS

[BearSSL](https://bearssl.org/) by Thomas Pornin, MIT license
(`LICENSE.txt`), the TLS behind AuroraOS's HTTPS (`src/os/Tls.c`,
`docs/tls.md`).

- From BearSSL's git master of 2026-04-06, commit 7bea48e (after 0.6).
- `inc/` is BearSSL's whole public header set; `src/` has `inner.h`,
  `config.h` and only the 72 source files the client uses.
- Local changes: the block at the end of `src/config.h` (platform detection
  off: no OS clock or random source, the portable 32-bit code everywhere),
  and "Damgard" spelt in ASCII in a comment of `inc/bearssl_hash.h`.
- Line endings are LF, as upstream.

The Makefile, `sdk/aurcc.py` and Auric's `aurc` compile every `.c` under
`src/`. To update, copy the same files from a newer BearSSL, keep the
`config.h` block, and link the OS: a missing symbol names a file to add.
