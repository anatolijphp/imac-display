#include "audio.h"

#include <alsa/asoundlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// The built-in codec by name, not "default" (which could be the GPU's HDMI audio or a sound server).
const char* kDevice = "plughw:PCH,0";
const char* kCard = "hw:PCH";

// Jitter buffer, in milliseconds of audio. PROTOCOL.md asks for 20 to 40 ms.
constexpr int kTargetMs = 30;  // fill level we start at, and come back to after an underrun
constexpr int kMaxMs = 60;     // above this, 10 ms are dropped (the Mac's clock runs a little fast)
constexpr int kPeriodMs = 5;   // ALSA period: what the playback thread writes per step

snd_pcm_t* open_pcm(snd_pcm_stream_t dir, const AudioFormat& f, int period_ms, int periods) {
  snd_pcm_t* pcm = nullptr;
  if (snd_pcm_open(&pcm, kDevice, dir, 0) < 0) return nullptr;
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(pcm, hw);
  snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(pcm, hw, f.channels);
  unsigned rate = f.rate;
  snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
  snd_pcm_uframes_t period = f.rate * period_ms / 1000;
  snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
  snd_pcm_uframes_t buffer = period * periods;
  snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
  if (snd_pcm_hw_params(pcm, hw) < 0) {
    snd_pcm_close(pcm);
    return nullptr;
  }
  fprintf(stderr, "audio: %s %s %u Hz, %d ch, period %lu frames, buffer %lu frames\n", kDevice,
          dir == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture", rate, f.channels, period, buffer);
  return pcm;
}

}  // namespace

Audio::Audio(AudioFormat out, bool in_enabled, AudioFormat in, std::function<void(std::vector<uint8_t>)> send_in)
    : out_(out), in_(in), in_enabled_(in_enabled), send_in_(std::move(send_in)) {
  play_thread_ = std::thread(&Audio::playback_loop, this);
  if (in_enabled_) cap_thread_ = std::thread(&Audio::capture_loop, this);
}

Audio::~Audio() {
  stop_ = true;
  play_thread_.join();
  if (cap_thread_.joinable()) cap_thread_.join();
}

void Audio::play(const uint8_t* pcm, size_t len) {
  size_t n = len / 2;
  const int max_samples = out_.rate * out_.channels * kMaxMs / 1000;
  std::lock_guard<std::mutex> l(mu_);
  stats_.received++;
  for (size_t i = 0; i < n; i++) ring_.push_back(int16_t(pcm[2 * i] | (pcm[2 * i + 1] << 8)));
  if (int(ring_.size()) > max_samples) {
    // Too much buffered (sender's clock faster than the codec's, or a burst after a network hiccup):
    // drop 10 ms from the front to come back near the target.
    size_t drop = out_.rate * out_.channels / 100;
    ring_.erase(ring_.begin(), ring_.begin() + std::min(drop, ring_.size()));
    stats_.trimmed++;
  }
}

void Audio::playback_loop() {
  const int period_frames = out_.rate * kPeriodMs / 1000;
  // A small device buffer (4 periods = 20 ms) keeps the added delay low; the jitter buffer is ours.
  snd_pcm_t* pcm = open_pcm(SND_PCM_STREAM_PLAYBACK, out_, kPeriodMs, 4);
  if (!pcm) {
    fprintf(stderr, "audio: cannot open %s for playback\n", kDevice);
    return;
  }
  std::vector<int16_t> buf(period_frames * out_.channels);
  const size_t target = size_t(out_.rate) * out_.channels * kTargetMs / 1000;
  while (!stop_) {
    {
      std::lock_guard<std::mutex> l(mu_);
      level_sum_ += double(ring_.size()) / out_.channels / out_.rate * 1000.0;
      level_n_++;
      if (!primed_ && ring_.size() >= target) primed_ = true;
      if (primed_ && ring_.size() >= buf.size()) {
        std::copy(ring_.begin(), ring_.begin() + buf.size(), buf.begin());
        ring_.erase(ring_.begin(), ring_.begin() + buf.size());
      } else {
        // Nothing (enough) to play: silence keeps the device running, then wait for the buffer to fill
        // up to the target again before playing, so one late packet does not cause a run of clicks.
        if (primed_) stats_.underruns++;
        primed_ = false;
        std::fill(buf.begin(), buf.end(), 0);
      }
    }
    snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf.data(), period_frames);  // blocks: paces this loop
    if (w < 0) snd_pcm_recover(pcm, int(w), 1);
  }
  snd_pcm_drop(pcm);
  snd_pcm_close(pcm);
}

void Audio::capture_loop() {
  snd_pcm_t* pcm = open_pcm(SND_PCM_STREAM_CAPTURE, in_, 10, 4);
  if (!pcm) {
    fprintf(stderr, "audio: cannot open %s for capture\n", kDevice);
    return;
  }
  const int frames = in_.rate / 100;  // 10 ms per AUDIO_IN message
  std::vector<int16_t> buf(frames * in_.channels);
  while (!stop_) {
    snd_pcm_sframes_t r = snd_pcm_readi(pcm, buf.data(), frames);
    if (r < 0) {
      snd_pcm_recover(pcm, int(r), 1);
      continue;
    }
    if (r != frames) continue;
    std::vector<uint8_t> out(buf.size() * 2);
    for (size_t i = 0; i < buf.size(); i++) {
      out[2 * i] = uint8_t(buf[i] & 0xFF);
      out[2 * i + 1] = uint8_t((uint16_t(buf[i]) >> 8) & 0xFF);
    }
    send_in_(std::move(out));
    std::lock_guard<std::mutex> l(mu_);
    stats_.mic_sent++;
  }
  snd_pcm_drop(pcm);
  snd_pcm_close(pcm);
}

AudioStats Audio::take_stats() {
  std::lock_guard<std::mutex> l(mu_);
  AudioStats s = stats_;
  s.buffer_ms = level_n_ ? level_sum_ / level_n_ : 0;
  stats_ = AudioStats{};
  level_sum_ = 0;
  level_n_ = 0;
  return s;
}

void Audio::set_volume(double level, bool mute) {
  snd_mixer_t* mx = nullptr;
  if (snd_mixer_open(&mx, 0) < 0) return;
  if (snd_mixer_attach(mx, kCard) < 0 || snd_mixer_selem_register(mx, nullptr, nullptr) < 0 ||
      snd_mixer_load(mx) < 0) {
    snd_mixer_close(mx);
    return;
  }
  snd_mixer_selem_id_t* id;
  snd_mixer_selem_id_alloca(&id);
  snd_mixer_selem_id_set_name(id, "Master");
  if (snd_mixer_elem_t* e = snd_mixer_find_selem(mx, id)) {
    // Perceptual curve (amplitude ~ level^3), like a volume slider: 1 -> 0 dB, 0.5 -> -18 dB,
    // 0.25 -> -36 dB, 0 -> muted. In hundredths of a dB.
    level = level < 0 ? 0 : level > 1 ? 1 : level;
    long db100 = level <= 0 ? -6000 : long(std::lround(6000.0 * std::log10(level)));
    if (db100 < -6000) db100 = -6000;
    snd_mixer_selem_set_playback_dB_all(e, db100, 1);
    if (snd_mixer_selem_has_playback_switch(e)) snd_mixer_selem_set_playback_switch_all(e, mute || level <= 0 ? 0 : 1);
    fprintf(stderr, "audio: volume %.2f (%.1f dB)%s\n", level, db100 / 100.0, mute ? ", muted" : "");
  }
  snd_mixer_close(mx);
}
