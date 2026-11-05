# Video

Apps can play H.264 video with AAC sound: fragmented MP4, with video and sound in
separate files, the form DASH uses. It takes an app built with
`py sdk/aurcc.py build ... --video`, which adds about 2 MB, and a core from 125 on.

**Status (2026-11-02): none of this has played on a console yet.** The demuxer is
checked on a PC against `ffprobe`; the decoders and everything on the ARM11 are only
built. `VideoTest` (`sdk/examples/videotest`) is what finds out. It plays from the
card, so `video_open_urls` has not run at all, on a console or anywhere else.

## Who does what

| Where | Runs | Does |
|-------|------|------|
| ARM9 | `sdk/runtime/app_video.c`, `app_fmp4.c`, the app | fetches from the card or the network (TLS included), demuxes, fills the rings, draws the bottom screen |
| ARM11 core 1 | `sdk/video11/engine.c` | feeds the decoders, AAC, converts frames, keeps time |
| ARM11 cores 2 and 3 | FFmpeg's frame threads (`threads11.c`) | H.264; New 3DS at full power only |
| ARM11 core 0 | `src/os/Video11.c` with `Gpu11.c`, `Audio11.c`, `WiFi11.c` | shows frames, runs the sound's channels, keeps the Wi-Fi going |

Core 0 alone touches the GPU and CSND, as it always has. An Old 3DS has core 1 and
no more, so everything on that row and the next shares it. Settings > Full power off
treats a New 3DS the same way: at 268 MHz the extra cores are the least tried case
and the least worth having.

`video_height_limit()` is 240 there and 480 on a New 3DS at full power.

## Memory

`include/video.h` has the addresses. The rings, the sound and the finished frames
are in `0x26100000..0x26A40000`, which the engine maps uncached and the ARM9 cleans
and invalidates. The engine's code, stacks and heap start at `0x27100000`, cached and
coherent between the cores, and run to `0x28000000` on an Old 3DS (11 MB of heap) or
`0x2C000000` on a New one. All of it is free while an app runs; what the OS keeps
there it loads again at its next start. HOME's snapshot at `0x26000000` is outside
it. Image decoding (`image_load`) uses some of the same memory, so it
must not run during playback.

## The rings

One writer and one reader each. A packet is a `VideoPkt` (size, time in ms, key
frame), its bytes, and padding to a word. A marker, or too little room for one, sends
the reader back to the start. The ARM9 writes a whole packet before moving
`vring_write`, so a half-written packet is never seen. Each side writes only its own
cache lines of `VideoShared`.

## Time

The sound is the clock. CSND's position cannot be read, so the engine works it out
from core 0's clock, which `VideoMail.now_us` publishes on every pass, and the rate
CSND really runs at for that divider. The two rings loop forever once started; the
engine writes ahead of the playing position and silences what has been played, so a
stall replays nothing old. That also makes pausing and seeking simple: the rings keep
turning with silence in them. A pause takes the sound written ahead out of them at
once, up to 1.5 s of it, and puts it back when it ends, so nothing plays over the
frozen picture and nothing is skipped.

Out of sound the clock holds, which is what a stall wants. Once the stream has ended
it goes on, so that a video longer than its sound still reaches its last frame, and
a video only counts as over when both have finished.

One engine can play one video after another. A new configuration puts the rings, the
clock and the counters back to the start, and the sound starts again at the new
video's rate.

A frame is shown when the clock reaches it, and dropped if it is more than 60 ms
late. Showing is a mail to core 0, not a command, so it never waits for, or
overwrites, whatever the ARM9 has in the one command slot. Core 0 copies the frame
into a hidden framebuffer, goes on with its loop, and switches at the next blank.

## Limits

- 8-bit 4:2:0, limited range only.
- Over the network the server must answer `Range:` requests with 206; a 200 is an
  error, since that is the file from its first byte. The app says how long each
  stream is and where its index ends (`VideoTrackUrl`): nothing here reads a DASH
  manifest or any other list of streams. A redirect's address is kept for the rest of
  the video, and if it left the first host, its `Authorization` and `Cookie` lines
  are dropped for good. A failed request is asked again four times, over 5 s.
- The first version scales by taking the nearest pixel and converts on the CPU.
- A fault in the engine hangs its core, and the app sees `heartbeat` stop.
- Do not draw on the top screen while a video plays. Present the bottom one only a
  few times a second: that present waits for a blank on core 0, holding up the frame
  and the Wi-Fi meanwhile.
- `AUDIO_VOICE_ANY` can pick voices 6 and 7, which the sound uses.
- FFmpeg's license: [`lib/ffmpeg/README.md`](../lib/ffmpeg/README.md).
