# Aurora accounts on the console

The console links itself to an Aurora account. The account server is the
`aurora-site/Account-API` repository: a Cloudflare Worker that serves
`account.aurora3ds.xyz` (the pages for people), `api.aurora3ds.xyz` (for PC and
mobile apps) and `3ds.aurora3ds.xyz` (for the console). Its `API.md` is the
contract this side follows. Accounts are created and managed only on the
website. The console does not handle passwords: it shows a code, the person
approves it on the website, and the console gets a token. This is the OAuth
device flow (RFC 8628).

Status: **works on hardware** (New 3DS, 2026-10-04). The console links to an
account through `3ds.aurora3ds.xyz`. Before that it was tested on the PC
against the server running under `wrangler dev` (see *What was checked*).

## Using it

**Settings > Aurora Account** (the second row; it shows the linked username, or
"Not linked"). The first-run wizard has the same thing as its **Account**
step, after Details: *Link or create* opens this screen, and *Skip* moves on.

Not linked, the screen offers:

- **Link an account**: for someone who already has an account. The top screen
  says "On a phone or computer, go to account.aurora3ds.xyz/link and enter
  this code", with the code (for example `K7QM-4XPD`) in a box. The bottom
  screen shows a QR code of `https://account.aurora3ds.xyz/link?code=K7QM-4XPD`,
  so a phone camera opens the page with the code filled in.
- **Create an account**: the same flow, but it sends the person to sign up
  first. The QR code opens
  `https://account.aurora3ds.xyz/signup?next=%2Flink%3Fcode%3DK7QM-4XPD`.
  After signing up, the site goes straight to `/link` with the code filled in.
  Typing the address works too: sign up at `account.aurora3ds.xyz/signup`, then
  choose *Link a console* on the settings page and enter the code.

Either way, the website then asks "Link this console?" and shows the console's
name. The console polls in the background and shows "Waiting for you to approve
it..." and a countdown ("Code expires in 9:41"). When the person presses
*Approve*, the console fetches the username, saves the token and says "This
console is now linked. Signed in as Nate_721". *Deny* on the website ends it
with "The link was declined". B cancels while waiting. If a code expires (10
minutes), a new one is fetched by itself, up to six codes (an hour).

Linked, the screen offers:

- **Check account**: asks the server who the token belongs to
  (`GET /v1/me`). That refreshes the username, or finds that the console was
  unlinked on the website or not used for 180 days. In that case the token is
  deleted and the screen says "This console was unlinked. Link it again to
  sign in."
- **Unlink this console**: after a Yes/No dialog, deletes the token on the
  console. The server has no endpoint for a console to revoke its own token,
  so the screen says to remove it at `account.aurora3ds.xyz` too
  (*Settings > Revoke* on the website).

If the console is not on a network, the screen first joins the network saved
in Settings > Wi-Fi ("Joining your Wi-Fi network..."), the same way the
terminal's `ping` does. Without a saved network it says why and stops. The
wizard's Account step says "Set up Wi-Fi first" when the network step saved
nothing.

## The token file

`SD:/Aurora/account.txt`, written when linking succeeds:

```
token=0u4aO2kMi37H5qP_GlHcU-wmnttkbr5oFVDgDHkRQYA
username=Nate_721
```

The token is 43 base64url characters. A file whose token is not exactly that
is treated as no account. The username is cached so Settings can show it
without the network. Unlink (or a revoked token found by Check account)
deletes the file.

The token sits on the SD card in plain text, as `API.md` suggests. It crosses
the network in plain HTTP too, until the console has TLS. That is why the
server gives console tokens only the `profile:read` scope. Anyone with the
card or on the same Wi-Fi could read the profile until the token is revoked on
the website.

## How linking works

All requests go to `http://3ds.aurora3ds.xyz` (port 80). The host name is
looked up with DNS once per session, and again after a connection fails
(Cloudflare's addresses can change).

1. `POST /v1/device/code` with `client_id=aurora-3ds` and `name`, the
   console's name as it will appear on the website: the owner's name from
   USER.dat plus the model, for example `Nate's New 3DS`. It is cut to fit 24
   printable ASCII characters, with accents dropped (`Renee's New 3DS`). With no
   owner name it is `AuroraOS New 3DS`. The reply gives `device_code` (kept on
   the console), `user_code` (shown), `verification_uri`, `interval` (5 s) and
   `expires_in` (600 s).
2. Every `interval` seconds, counted from the end of the last poll:
   `POST /v1/device/token` with
   `grant_type=urn:ietf:params:oauth:grant-type:device_code` (percent-encoded),
   `client_id` and `device_code`:
   - `authorization_pending`: poll again.
   - `slow_down`: add 5 s to the interval.
   - `access_denied`: stop ("declined").
   - `expired_token`, or the countdown reaching zero: get a new code.
   - Any other 4xx (`invalid_grant`, `invalid_client`, ...): stop, and show the
     status and error code on the third line, for example
     `HTTP 400: invalid_grant`.
   - No reply, or a 5xx: show "Connection problem. Trying again..." and keep
     polling. If the Wi-Fi session dropped, join again first.
   - 200: `access_token`.
3. `GET /v1/me` with `Authorization: Bearer <token>` for the username, then
   the file is written.

Every request is HTTP/1.0 with `Host: 3ds.aurora3ds.xyz`,
`User-Agent: AuroraOS/0.1.4` (the digits of `AURORA_VERSION`),
`Accept: application/json` and `Connection: close`, and a form body with its
`Content-Length`. The reply is read until the server closes or until the
`Content-Length` after the headers is in. The status code is the three digits
after the first space of the status line (`HTTP/1.1 200 OK` from Cloudflare).
Headers are skipped except `Content-Length` (any case). The body is parsed with
`src/os/Json.c`. Rate limiting (`429 rate_limited`) shows "Too many tries. Wait
a while."

## The network underneath: HTTP over TCP on the ARM11

The Wi-Fi driver runs on the ARM11 core and only reads the chip while a
command runs (docs/wifi.md, *Staying joined*). So a TCP connection lives inside
one command: `AUDIO_CMD_WIFI_NET` with `WIFI_NETOP_HTTP` (core 114) connects,
sends the request, collects the reply and closes, all in one go.

| Where | What |
|---|---|
| `WifiNetIo` (`0x233B8100`) | `ip`, `port`, `req_len`, `timeout_ms` in; `status`, `resp_len`, `stage` (`WIFI_HTTP_CONNECT` / `SEND` / `WAIT` / `DONE`), `resent` out |
| `WIFI_HTTP_REQ` (`0x233B9000`, 2 KB) | the request bytes, written by the ARM9 |
| `WIFI_HTTP_RESP` (`0x233B9800`, 8 KB) | the reply bytes, headers included |

`wifi_http()` in `src/os/WiFi9.c` copies the request in, posts the command,
waits with the screens drawn directly (like `wifi_net()`), and copies the reply
out. The whole exchange is bounded by `timeout_ms` (at most 20 s; the account
code asks for 15 s).

Results: `WIFI_NETS_OK` (the reply is in), `TIMEOUT`, `NOLINK` (the link
dropped), `NOHOST` (no router MAC), `REFUSED` (the server sent RST), `TOOBIG`
(request over 2 KB or reply over 8 KB), `NOSEND`.

The TCP client in `src/os/Net11.c` is just enough for this:

- One connection at a time, from a random port in 49152-65535 with a random
  initial sequence number (the core's timer mixed with the MAC).
- The SYN announces an MSS of 1200, so a full segment still fits one HTC frame
  (1664 bytes). There are no other options (no window scaling, SACK or
  timestamps), so the server sends none either. The window we announce is the
  room left in the 8 KB reply buffer.
- Data goes out in segments of at most 512 bytes, within the server's MSS and
  window. A request is one or two segments.
- Received data is kept only when it continues the stream. A segment that
  skips ahead is dropped and answered with a duplicate ACK, so the server
  resends. Retransmitted data that overlaps what we have is trimmed. Every
  segment with data or a FIN is ACKed at once.
- Retransmission: nothing acknowledged for 1 s, then 2 s, then 4 s (and 4 s
  after that) sends everything from the first unacknowledged byte again. A
  lost SYN is resent the same way.
- When the reply is complete, the console sends FIN and waits up to 0.5 s for
  the server's FIN and the ACK of its own. On an error it sends RST. There is
  no TIME_WAIT: the next connection uses a new port.
- IP fragments are not reassembled (Cloudflare sets DF and stays under the
  MSS).

Each received frame is read from the chip a byte at a time (about 0.3 s for a
full-size frame, docs/wifi.md *Path forward*). A typical Cloudflare reply of
about 1 KB therefore takes a fraction of a second on top of the round trips.

## The QR code

`src/os/Qr.c` encodes QR codes (ISO/IEC 18004) in byte mode at error
correction level M, versions 1 to 10 (up to 213 bytes). It picks the smallest
version and the mask with the lowest penalty score. The link address is
version 4 (33 modules) and the sign-up address version 5 (37). The account
screen draws it on a white card with the four-module quiet zone, at the
largest whole number of pixels per module that fits 196 pixels: 4 for both,
so 164 and 180 pixels square.

## Memory

| Address | Size | Use |
|---|---|---|
| `0x233B8100` | 316 B | `WifiNetIo`, now with the HTTP fields |
| `0x233B9000` | 2 KB | `WIFI_HTTP_REQ` |
| `0x233B9800` | 8 KB | `WIFI_HTTP_RESP` (to `0x233BB800`, below `GpuShared` at `0x233C0000`) |

`Account.c` keeps its own 2 KB request and 4 KB reply buffers in `.bss`, and
`Qr.c` two 57x57 grids.

## What was checked

On the PC, with the msys gcc:

- **TCP.** `Net11.c` and the real `wifi_op_http()` from `WiFi11.c` (cut out of
  the file unchanged) ran against a simulated server and link. All passed: a
  clean exchange; FIN on the last data segment; a lost SYN, SYN-ACK, ACK of
  the SYN-ACK, request segment and first reply segment; a reordered reply; a
  duplicated reply; a server MSS of 100 and a window of 200; a server that
  never closes (finished by Content-Length); RST to the SYN (`REFUSED`); a
  silent server (`TIMEOUT` after 1+2+4 s of SYNs); no HTC credit at first.
  Every frame's IP and TCP checksums were checked by separate code, with the
  server's sequence numbers wrapping past 2^32 during the exchange.
- **The account flow.** `Account.c`, `Json.c` and `Qr.c` ran against the real
  server under `wrangler dev` (local database only). The console's requests
  went over a real socket with only the `Host` line changed to the local name.
  A small Node script played the person on the website: sign up, enter the
  code, Approve or Deny, revoke. All passed: link and approve; check account;
  revoke on the website, then check (the token is deleted); create and deny;
  cancel with B; link while not joined (joins first); unlink. Each request was
  also checked against the rules in `API.md`.
- **The screens.** The real `screen.c`, `ui.c`, `assets.c` and `StatusBar.c`
  rendered every account screen in English, Spanish and French: the main
  screen linked and not, the busy screens, both code screens and the results.
  Nothing is cut off. The QR modules in the rendered frames match the
  encoder's output exactly, and OpenCV decodes both (the sign-up one from a
  crop of the card, or with its second detector on the whole screen).
- **The QR encoder.** OpenCV decodes its codes at versions 1, 4, 5, 6, 8 and
  10 (213 bytes), and with each of the eight masks forced. Two of those 16
  forced-mask codes needed OpenCV's second detector (`QRCodeDetectorAruco`)
  or another scale; the encoder's own choice of mask always decoded at once.

**On hardware** (2026-10-04): linking works on a New 3DS against the live
server. That also shows the TCP client works with Cloudflare's real TCP stack
over Wi-Fi. Not yet tried: an Old 3DS.

## Checking it on a console

1. Settings > Wi-Fi: have a network saved and joined.
2. Settings > Aurora Account > Link an account. Within a few seconds a code
   and a QR code should appear. If it says "Could not reach Aurora", the third
   line has the reason (for example "The server did not answer in time"); see
   *Results* above.
3. Scan the QR code with a phone (or type the address), sign in, Approve. The
   console should say "This console is now linked" within one poll (5 s).
4. Check account: "Account checked just now."
5. On the website, revoke the console, then Check account on the console: "This
   console was unlinked."

## Code map

| File | What |
|---|---|
| `src/os/Account.c` | the token file, HTTP requests and replies, the device flow, Settings > Aurora Account |
| `include/account.h` | `account_load`, `account_linked`, `account_name`, `account_screen` |
| `src/os/Qr.c`, `include/qr.h` | the QR encoder |
| `src/os/Http.c`, `include/http.h` | `http_call()`: the request, the host look-up, the reply's status and headers (shared with aShop, [`store.md`](store.md)) |
| `src/os/WiFi9.c` | `wifi_http()` |
| `src/os/WiFi11.c` | `wifi_op_http()`: the connection, retransmission, close |
| `src/os/Net11.c` | `net_tcp_open`, `net_tcp_seg`, the TCP receive path in `net_rx`, `net_http_done` |
| `src/os/os_setup.c` | the wizard's Account step, and the `STR_AC_*` strings |
| `src/os/os_main.c` | the Settings row (`SET_ACCOUNT`) |

## Not done

- TLS: `https://3ds.aurora3ds.xyz` already works on the server, but the
  console has no TLS.
- Revoking the token from the console needs an endpoint the server does not
  have.
- Deleting an account has no page on the website yet (see the server's
  README).
