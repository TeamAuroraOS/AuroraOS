#include "wav.h"
#include "audio.h"

const char *wav_error(WavResult r) {
  switch (r) {
    case WAV_OK:              return "";
    case WAV_ERR_OPEN:        return "Could not read the file";
    case WAV_ERR_FORMAT:      return "Not a WAV file";
    case WAV_ERR_UNSUPPORTED: return "Only uncompressed 8/16-bit PCM";
    default:                  return "File is damaged";
  }
}

WavResult wav_play(const char *path, u32 *osamples, u32 *orate, u32 *odepth) {
  WavInfo info;
  WavResult r = wav_load(path, (u8 *)AUDIO_PCM_ADDR, AUDIO_PCM_MAX, 0, &info);

  if (r != WAV_OK)
    return r;
  audio_play_pcm(info.samples, info.rate, info.depth);
  if (osamples) *osamples = info.samples;
  if (orate)    *orate = info.rate;
  if (odepth)   *odepth = info.depth;
  return WAV_OK;
}
