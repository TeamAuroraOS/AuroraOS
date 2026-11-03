# HTTPS and TLS

Since 2026-10-28 AuroraOS speaks HTTPS: TLS 1.2 on the ARM9, with
[BearSSL](https://bearssl.org/), over a TCP connection the ARM11 core keeps
open (from core 121). Checked on a PC against real servers, through the same
code the console runs, then reported working on the maintainer's console
(2026-11-01: `curl`, aShop and the account).

Who uses it:

- **Aurora accounts and aShop** (`src/os/Http.c`): every request to
  `3ds.aurora3ds.xyz` goes over HTTPS on port 443, so the console's token no
  longer crosses the Wi-Fi in clear text ([`account.md`](account.md),
  [`store.md`](store.md)). There is no fallback to plain HTTP.
- **Apps**: `https://` addresses in the C SDK's `http_get()`, `http_post()`,
  `http_request()` and `http_download()` (SDK 1.4) and in Auric's network
  built-ins (Auric 0.6), redirects to and from `https://` included. Apps
  built before have no TLS and must be rebuilt.
- **The terminal**: [`curl`](terminal.md#curl) fetches or saves `http://`
  and `https://` addresses.

## How it fits together

| Layer | Where | Runs on |
|-------|-------|---------|
| HTTP: the request, the reply's headers, redirects, downloads | `src/os/Http.c` (OS), `src/os/AppNet.c` (apps, `curl`) | ARM9 |
| TLS: handshake, certificates, records | `src/os/Tls.c` over BearSSL (`lib/bearssl`) | ARM9 |
| The byte rings | `src/os/WiFiTcp.c` | ARM9 |
| TCP, IP, the Wi-Fi chip | `WIFI_NETOP_TCP` in `src/os/WiFi11.c`, `src/os/Net11.c` | ARM11 core |

The crypto runs on the ARM9 because the ARM9 runs with its caches on and the
core does not: code that executes from uncached FCRAM is many times slower.
The core only moves bytes. `WIFI_NETOP_TCP` holds the connection for as long
as the ARM9 wants it, and the two sides exchange bytes through two rings in
shared FCRAM (docs/wifi.md, *A connection the ARM9 drives*). While it runs,
the core takes no other command, as with every Wi-Fi command, so the OS draws
straight into the framebuffers (`wifi_direct_on`) and apps post no presents
or sounds.

One request is one connection: `tls_exchange()` sets up BearSSL, posts the
operation, runs the handshake, sends the request (HTTP/1.0 with
`Connection: close`, at most 2 KB), and decrypts the reply into
`WIFI_HTTP_RESP` (at most 1020 KB, headers included), where the HTTP code
reads it as it read the core's plain replies. The reply is complete when the
headers and as much body as their `Content-Length` say are in (none for a
`HEAD`, a 204 or a 304); then the console sends TLS's `close_notify` and a
FIN. Without a `Content-Length` it reads until the server closes; a close
without `close_notify` is taken as the end of such a reply, as HTTP/1.0
allows. Each wait for the server is bounded (15 s for the account, aShop and
apps). Larger files come in Range pieces, each its own connection
(`appnet_download`, aShop's downloads).

Between the steps the ARM9 calls the caller's tick about ten times a second
(the OS's spinners, an app's `net_on_wait()` handler), bumps `alive` so the
core knows it is still there, and, when nothing moved, waits for the next
millisecond on the timer rather than polling FCRAM, which the core runs its
code from.

## What it speaks

- **TLS 1.2 only.** Servers that accept only TLS 1.3 fail with "the server
  wants a newer TLS than 1.2"; TLS 1.0 and 1.1 are not offered.
- **Cipher suites**, in this order: ECDHE with ECDSA or RSA and
  ChaCha20-Poly1305 (fast in software), then AES-128-GCM, AES-256-GCM, then
  AES-CBC with SHA-256, SHA-384 or SHA-1, then plain RSA key exchange with
  AES-GCM and AES-CBC for old servers.
- **Key exchange curves**: X25519, P-256, P-384, P-521.
- **Signatures**: RSA (PKCS#1 v1.5) and ECDSA, with SHA-1 to SHA-512.
- **SNI**: the host name goes in the handshake, as Cloudflare and most hosts
  need.
- **Sessions**: the last four hosts' sessions are kept in memory and offered
  again, which saves the certificate checks and the key exchange when the
  server still has them (session IDs; BearSSL has no tickets). Cloudflare
  remembers them only some of the time.
- No client certificates, no renegotiation, no compression.

Which certificate a server sends is its choice: Cloudflare (the Aurora
server) always sends its ECDSA P-256 one when the client accepts ECDSA, which
is the more costly kind to verify on a slow CPU.

## Certificates

A server's certificate must:

- chain to a trusted root (below), each signature checked;
- be for the host name asked for (subject alternative names, wildcards as
  usual);
- be valid on the console's date (see *The clock*);
- use keys and algorithms BearSSL takes (RSA keys too small and MD5
  signatures are refused).

Revocation (OCSP, CRLs) is not checked, as in most small clients.

### The built-in roots

`src/os/TlsRoots.c` holds 46 root authorities, made by `tools/mkroots.py`
(`make roots`) from `assets/certs/roots.pem`, which has the certificates
exactly as in the Mozilla CA store that curl publishes
(<https://curl.se/docs/caextract.html>; data as of 2026-10-20). They cover
most of the web:

| Authority | Roots |
|-----------|-------|
| Let's Encrypt | ISRG Root X1, X2 |
| Google Trust Services (Cloudflare, Google) | GTS Root R1, R3, R4 |
| GlobalSign | Root CA - R3, R6; ECC Root CA - R4, R5; Root R46, E46 |
| DigiCert | Global Root G2, G3; Trusted Root G4; TLS RSA4096 Root G5; TLS ECC P384 Root G5; QuoVadis Root CA 2 G3 |
| Amazon | Amazon Root CA 1 to 4; Starfield Services Root Certificate Authority - G2 |
| GoDaddy | Go Daddy Root Certificate Authority - G2; Starfield Root Certificate Authority - G2 |
| Sectigo | USERTrust RSA and ECC Certification Authority; COMODO RSA and ECC Certification Authority; Public Server Authentication Root R46, E46 |
| Microsoft | Microsoft RSA and ECC Root Certificate Authority 2017 |
| SSL.com | Root Certification Authority RSA, ECC; TLS RSA and ECC Root CA 2022 |
| Others | Certum Trusted Network CA, Certum Trusted Root CA, IdenTrust Commercial Root CA 1, Actalis Authentication Root CA, Buypass Class 2 Root CA, HARICA TLS RSA and ECC Root CA 2021, T-TeleSec GlobalRoot Class 2, Certainly Root R1, E1 |

Each anchor is the root's name and public key, as BearSSL's `brssl ta`
writes them; together about 20 KB. To change the list, edit `roots.pem`
(any PEM certificates, each optionally with a name line and `====` above it,
as in curl's file), run `make roots`, and rebuild the OS and the apps.

### Roots on the SD card

Certificates in `SD:/Aurora/certs` are trusted too: files ending `.pem`,
`.crt`, `.cer` or `.der`, PEM with any number of certificates (other objects
are skipped) or one DER certificate. curl's whole `cacert.pem` works there.

- A CA certificate adds an authority.
- Any other certificate trusts its key as a server's own: a self-signed
  server, or one pinned this way, is accepted with that key, the name still
  checked. A key shared by several certificates (a test site's, say) is
  trusted in all of them.

They are read once per run, the first time HTTPS is used after the OS starts
(a HOME return restarts it) or in an app. The built-in and SD roots together
are at most 256, with 128 KB for the card's names and keys; what does not fit
is skipped. The card is used as the caller has it mounted, or mounted briefly.

## The clock

Certificates are valid between two dates, so the console's date matters.
AuroraOS reads the RTC plus its own offset (Settings > Clock; the RTC is
never written, see [`power.md`](power.md)). Many consoles' RTCs read years
behind, because Horizon keeps its own offset where AuroraOS cannot read it.
So the date used is the clock's, but never earlier than
`AURORA_BUILD_DATE` (`include/aurora.h`, the day this version was made,
moved with the version): a clock that reads 2011 checks against the build
date, which still rejects certificates that expired before the build. A
clock set far ahead makes valid certificates look expired. A date failure
says "the certificate has expired or is not valid yet (check Settings >
Clock)" in apps and the terminal, and "Secure connection failed: check the
clock." in Settings and aShop.

## Randomness

A TLS handshake needs secret random numbers; a guessable one would let an
eavesdropper decrypt the session. The ARM9 has no documented random number
generator, so `Tls.c` mixes several sources through SHA-256 into an
HMAC-DRBG (SHA-256) and gives BearSSL 32 fresh bytes per handshake:

- the eight words of the ARM9's PRNG block at `0x10011000`, which Boot9 and
  Process9 use to seed their generators (3dbrew, *IO Registers*);
- the ARM9 timers and the running tick count;
- the RTC;
- the whole `WifiShared` block (frame counters, timings, the scan's signal
  readings), which changes with the network;
- `SD:/Aurora/random.bin`, 32 bytes carried from the last run: read at the
  first HTTPS use, then replaced with new output;
- after each exchange, its timing, so the next handshake has more.

Whether the PRNG block reads differently each time on the ARM9 has not been
checked on a console; the design does not rely on any one source.

## Errors

`tls_error_text()` turns BearSSL's error into a line; apps and the terminal
show it after the host's name (`net_error()`, `curl: ...`). The OS's screens
use "The secure connection failed." (`STR_AC_E_TLS`), or the clock message
above (`STR_AC_E_CLOCK`).

| Line | Usually |
|------|---------|
| the certificate has expired or is not valid yet (check Settings > Clock) | the clock, or an expired site |
| the certificate is for another host name | a wrong address, or a captive portal |
| the certificate is not from an authority AuroraOS trusts | a self-signed server, an authority not built in (add its root to `SD:/Aurora/certs`), or something intercepting the connection |
| the server wants a newer TLS than 1.2 | a TLS 1.3-only server |
| the server refused the handshake: no cipher or curve in common | an old or unusual server |
| the server closed the connection during the handshake | a server that does not speak TLS on that port, or a filter |
| the reply is larger than 1020 KB | use a download (`http_download`, `curl -o`) |

## Memory

| What | Where | Size |
|------|-------|------|
| BearSSL's record buffers and contexts | ARM9 `.bss` of the OS or the app | about 41 KB |
| The anchors and the card's names and keys | ARM9 `.bss` | about 135 KB |
| `WifiTcp`: the core's line, the ARM9's line, `tx`, `rx` | `0x233F0000` | 64 B + 16 KB + 32 KB |
| The decrypted reply | `WIFI_HTTP_RESP`, `0x23200000` | up to 1020 KB |

Code: the OS grew from 541,840 to 672,032 bytes (BearSSL's client about
82 KB, the roots 20 KB, `AppNet.c` and `curl` the rest), under the 1 MB its
HOME snapshot allows, which `os.ld` now checks. An app grows only if it uses
the network (the linker drops the rest): Auric's `net` example went from
48 KB to 163 KB and the SDK's from 138 KB to 250 KB, while the SDK's `hello`
stayed at 90,380 bytes.

## Speed

Not measured on a console yet. A request costs two round trips more than
plain HTTP, plus the ARM9's handshake work, mostly the signature checks
(ECDSA ones cost more than RSA ones) and the key exchange; a resumed session
skips both. After the handshake the ARM9 decrypts far faster than the
network delivers (about 160 KB/s on a New 3DS), so downloads should keep
their speed, less one handshake per piece.

## What was checked

On a PC (2026-10-28, msys gcc), in a harness that runs the real `AppNet.c`,
`Tls.c`, `TlsRoots.c`, `WiFiTcp.c` and all of `lib/bearssl` against a
simulated core: a thread that answers the AudioCtrl commands (`STATUS`,
`DNS`, `HTTP`, `TCP`) with real sockets and moves bytes through the real
rings in ragged pieces, so they wrap at every offset. Against live servers,
all as expected:

- trusted: `3ds.aurora3ds.xyz` (the 401 without a token), example.com,
  google.com, letsencrypt.org, github.com, api.github.com, wikipedia.org,
  cloudflare.com, microsoft.com, aws.amazon.com, iana.org, and badssl.com's
  ECC P-256 and P-384 and RSA 2048 and 4096 hosts: Let's Encrypt, Google
  Trust Services, DigiCert, Sectigo, Amazon and Microsoft chains, ECDSA and
  RSA;
- redirects from `http://github.com/` and `http://www.wikipedia.org/` to
  `https://`;
- refused with the right reason: expired.badssl.com, sha384 and sha512
  (their certificates expired in 2022), wrong.host.badssl.com,
  untrusted-root.badssl.com, self-signed.badssl.com; a reply of 2 MB
  (too large); a name that does not exist;
- `HEAD` (done at the headers), `POST` to httpbin.org, a 900 KB reply
  whole, `appnet_download` of RFC 8446 byte for byte as curl got it;
- the clock: set to 2011, iana.org still checks out (the build-date floor);
  set to 2045, python.org's certificate is refused as expired;
- a certificate on the SD card: with self-signed.badssl.com's certificate in
  `SD:/Aurora/certs`, that site is accepted; without it, refused; the seed
  file is written.

The core side was checked in its own simulation (docs/wifi.md, *A connection
the ARM9 drives*): 11 cases on a lossy, reordering link, all bytes checked.
The firm is byte-identical to before; the OS, the core, the SDK (9 tests) and
Auric (117 tests) build without warnings.

On 2026-11-01 the real `Tls.c` and `AppNet.c` ran on a PC again, after these
fixes: www.cloudflare.com (twice, the second a resumption), `3ds.aurora3ds.xyz`,
example.com and github.com were accepted, and expired, wrong.host, self-signed
and untrusted-root.badssl.com refused for the right reasons.

- **A reply cut short.** A connection that ends without a close_notify used to
  count as the end of the reply. Now, if the reply's own Content-Length says more
  was coming, it is reported as `WIFI_NETS_TIMEOUT` and so tried again, since an
  attacker on the network can end a connection as easily as a dropped link can.
  Without a Content-Length nothing can tell, and the reply is taken as it came.
- **An interim reply.** A `100 Continue` used to stop the check at its own blank
  line, so the request waited for the timeout. It is skipped now.
- **Keys on a redirect.** A redirect to another host, another port or from
  `https://` to `http://` no longer carries the caller's `Authorization` and
  `Cookie` lines (`build()` in `AppNet.c`); other lines still go, as curl does.
- **A download cut short.** `appnet_download()` compared nothing on a `200`, so a
  short body was saved as the whole file. Against its Content-Length, it now
  retries and then fails, and leaves nothing behind.
- **The last bytes.** If the core finished just after a pass read the ring, that
  pass could end the exchange with its last bytes still unread. It now takes one
  more look before ending.

## Checking it on a console

1. Copy the new `AURORAOS.BIN` (its core replaces the running one by
   itself). This work leaves `Aurora.firm` unchanged.
2. Settings > Aurora Account > Check account: "Account checked just now"
   means HTTPS to the Aurora server works.
3. Open aShop and download something: watch the speed on the download
   screen.
4. In the terminal: `curl -I https://example.com/` should print the headers;
   `curl https://expired.badssl.com/` should fail with the expired line.
5. If something fails with the clock message, set Settings > Clock.

## Files

| File | What |
|------|------|
| `src/os/Tls.c`, `include/tls.h` | `tls_exchange()`, the client's setup, roots from the card, the clock, randomness, sessions, error lines |
| `src/os/TlsRoots.c` | the built-in roots (generated, do not edit) |
| `tools/mkroots.py`, `assets/certs/roots.pem` | how the roots are made, and from what |
| `src/os/WiFiTcp.c`, `include/wifi.h` | the ARM9 side of the rings (`wifi_tcp_*`), `WifiTcp` |
| `src/os/WiFi9.c` | `wifi_https()`, the OS's way in |
| `src/os/AppNet.c`, `include/appnet.h` | `https://` for apps and `curl` |
| `src/os/WiFi11.c`, `src/os/Net11.c` | `WIFI_NETOP_TCP` on the core |
| `lib/bearssl/` | BearSSL |

## BearSSL in this tree

`lib/bearssl` holds BearSSL's headers (`inc/`), `src/inner.h`,
`src/config.h` and the 72 source files the client uses, from its git master
of 2026-04-06 (commit 7bea48e, after 0.6), with two changes: a block at the
end of `config.h` that turns off its platform detection (no OS clock or
random source, the 32-bit code everywhere), and one comment's "Damgard"
spelt in ASCII. The Makefile compiles every `.c` under `lib/bearssl/src`,
each function in a section of its own so the linker keeps only what is used;
`sdk/aurcc.py` and Auric's `aurc` compile it into their build folders once.
To take a newer BearSSL, copy the same files from it, keep the `config.h`
block, and check that the OS links (a missing symbol names a file to add).

## License and credits

BearSSL is by Thomas Pornin, under the MIT license
(`lib/bearssl/LICENSE.txt`), which GPL-2.0 and GPL-3.0 code may use. The
root certificates come from the Mozilla CA store, distributed under the
MPL-2.0. `Tls.c` is part of AuroraOS (GPL-3.0); `WiFiTcp.c` and the core's
TCP operation are part of the Wi-Fi driver (GPL-2.0).
