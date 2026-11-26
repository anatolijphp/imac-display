// Video: one CPU decoder thread per tile (libavcodec), one presenter thread on the GPU (OpenGL ES 3 on
// GBM/EGL). Each decoder thread copies its decoded tile straight into GPU-visible memory (a persistently
// mapped pixel buffer, one per tile per picture slot), all tiles in parallel. When the last tile of a
// picture is in, the presenter starts the GPU transfer, converts YUV to RGB, scales the picture to the
// panel and page-flips at the next refresh. Pictures are never queued: a newer complete picture replaces
// an older one that has not been shown yet (counted as dropped).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct AVFrame;

struct ScreenTarget {  // the panel, borrowed from Display while video runs
  int fd = -1;
  uint32_t crtc_id = 0, conn_id = 0;
  const void* mode = nullptr;  // drmModeModeInfo*
  int w = 0, h = 0;
};

struct VideoConfig {
  int w = 0, h = 0;  // full picture
  struct Tile { int id, x, y, w, h; };
  std::vector<Tile> tiles;
};

struct VideoStats {
  unsigned decoded = 0, dropped = 0, shown = 0;
  double decode_ms = 0, present_ms = 0;
  // Breakdown (averages): copy of a decoded tile into GPU memory; picture complete -> presenter starts;
  // upload + draw submitted; swap; page flip done.
  double copy_ms = 0, wait_ms = 0, upload_ms = 0, swap_ms = 0, flip_ms = 0;
};

class Video {
 public:
  // request_keyframe is called from any thread; it must be cheap and thread-safe.
  // release_screen is called on the presenter thread just before its GPU buffers are freed (while the
  // owner is blocked in ~Video), so the owner can put its own picture on the panel first.
  // screen.fd < 0: decode only, nothing shown (testing).
  Video(ScreenTarget screen, VideoConfig cfg, std::function<void()> request_keyframe,
        std::function<void()> release_screen);
  ~Video();

  void push(uint16_t tile, uint8_t flags, uint64_t pts_us, const uint8_t* data, size_t len);
  VideoStats take_stats();  // counts since the last call, averages
  bool showing() const { return shown_any_; }

 private:
  struct Packet { std::vector<uint8_t> data; uint8_t flags; uint64_t pts, recv_us; };
  struct Tile;
  struct TileMeta { int w = 0, h = 0, colorspace = 0, range = 0, format = 0; };
  struct Picture {
    uint64_t pts = 0, last_recv_us = 0, complete_us = 0, first_seen_us = 0;
    int slot = -1;  // -1: no free slot, the picture is dropped
    uint32_t sent = 0, have = 0;  // tile masks: packets received for this picture, tiles decoded and copied
    bool last_seen = false;       // the packet flagged "last tile sent for this picture" has arrived
    int writers = 0;
    bool abandoned = false;
    std::vector<TileMeta> meta;
  };
  struct Slot {
    std::vector<uint8_t*> ptr;  // per tile: Y, then U, then V, tightly packed
    bool free = true;
  };
  void decode_loop(Tile* t);
  void on_decoded(int index, uint64_t pts, uint64_t recv_us, const AVFrame* f, double ms);
  void present_loop();
  int take_slot();                   // pic_mu_ held
  void forget_old(uint64_t now);     // pic_mu_ held: pictures that will never complete
  void release(Picture& p);          // pic_mu_ held
  size_t tile_bytes(int i) const;

  ScreenTarget screen_;
  VideoConfig cfg_;
  std::function<void()> request_keyframe_;
  std::function<void()> release_screen_;
  std::vector<std::unique_ptr<Tile>> tiles_;
  std::map<uint16_t, int> tile_index_;  // tile id -> index

  std::mutex pic_mu_;
  std::condition_variable pic_cv_;
  std::map<uint64_t, Picture> assembling_;  // by pts
  Picture ready_;                           // newest complete picture not shown yet
  bool has_ready_ = false;
  std::vector<Slot> slots_;
  bool gpu_ready_ = false, gpu_failed_ = false;
  std::vector<uint8_t> headless_buf_;       // decode-only mode: copies go here

  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> last_key_request_us_{0};
  std::atomic<bool> shown_any_{false};
  std::thread presenter_;

  std::mutex stats_mu_;
  VideoStats stats_;
  double decode_sum_ = 0, present_sum_ = 0, copy_sum_ = 0, wait_sum_ = 0, upload_sum_ = 0, swap_sum_ = 0,
         flip_sum_ = 0;
  unsigned decode_n_ = 0, present_n_ = 0;
};
