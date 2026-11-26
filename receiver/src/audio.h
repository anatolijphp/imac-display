// Sound (PROTOCOL.md, Audio): AUDIO_OUT from the MacBook played on the iMac's speakers / headphone jack,
// the iMac's microphone captured and sent back as AUDIO_IN. ALSA on the built-in codec (CS4206).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioFormat {
  int rate = 48000, channels = 2;  // s16le only (the protocol's format)
};

struct AudioStats {
  unsigned received = 0;     // AUDIO_OUT messages
  unsigned underruns = 0;    // times the jitter buffer ran dry (silence played)
  unsigned trimmed = 0;      // times 10 ms were dropped because the buffer grew too large
  double buffer_ms = 0;      // jitter buffer level, average
  unsigned mic_sent = 0;     // AUDIO_IN messages
};

class Audio {
 public:
  // send_in(payload) is called from the capture thread with one 10 ms AUDIO_IN payload.
  Audio(AudioFormat out, bool in_enabled, AudioFormat in, std::function<void(std::vector<uint8_t>)> send_in);
  ~Audio();

  void play(const uint8_t* pcm, size_t len);  // one AUDIO_OUT payload
  static void set_volume(double level, bool mute);  // AUDIO_VOLUME: the codec's Master control
  AudioStats take_stats();

 private:
  void playback_loop();
  void capture_loop();

  AudioFormat out_, in_;
  bool in_enabled_;
  std::function<void(std::vector<uint8_t>)> send_in_;
  std::atomic<bool> stop_{false};

  std::mutex mu_;
  std::deque<int16_t> ring_;  // interleaved samples waiting to be played
  bool primed_ = false;       // playback starts once the buffer reaches its target
  AudioStats stats_;
  double level_sum_ = 0;
  unsigned level_n_ = 0;

  std::thread play_thread_, cap_thread_;
};
