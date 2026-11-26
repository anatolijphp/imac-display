// The bare screen: DRM/KMS dumb buffer on the connected panel, plus the waiting screen drawn with cairo.
// Video (stage 3) will replace the dumb buffer with GPU-imported pictures; the waiting screen stays.
#pragma once
#include <cstdint>
#include <string>

#include "video.h"

typedef struct _cairo_surface cairo_surface_t;

class Display {
 public:
  ~Display();
  // Takes the screen: first connected connector, its preferred mode. Returns false if the screen is not
  // available right now (no DRM device, or another program is DRM master); the caller retries later.
  bool open();
  void close();  // gives the screen back to the text console
  bool is_open() const { return fd_ >= 0; }
  int width() const { return w_; }
  int height() const { return h_; }

  enum class State { NoCable, WaitingForMac, Connected };
  struct Status {
    State state = State::NoCable;
    std::string mac_name;  // when connected
    std::string footer;    // small text at the bottom: interface, addresses
    std::string detail;    // connected: line under the title (default "Waiting for the first picture")
  };
  void show(const Status& s);  // full redraw when something changed
  void animate();              // next frame of the "waiting" dots; call ~15 times a second
  bool save_png(const std::string& path) const;  // the current picture, for checking without looking

  // Video borrows the panel: pause() stops drawing, target() describes the panel, resume() puts the
  // waiting screen back up (called by the video presenter before it frees its buffers).
  ScreenTarget target() const;
  void pause() { paused_ = true; }
  bool paused() const { return paused_; }
  void resume();
  void blank();  // black panel (between two video streams), stays paused

 private:
  void render_base(const Status& s);
  void copy_rect(int x, int y, int w, int h);  // base surface -> scanout buffer

  int fd_ = -1;
  uint32_t conn_id_ = 0, crtc_id_ = 0, fb_id_ = 0, handle_ = 0, pitch_ = 0;
  uint64_t size_ = 0;
  uint8_t* map_ = nullptr;
  int w_ = 0, h_ = 0;
  void* saved_crtc_ = nullptr;  // drmModeCrtc* to restore the console's mode on close
  int tty_ = -1;                // our VT, switched to graphics mode so the text console stays quiet
  cairo_surface_t* base_ = nullptr;  // static picture in system memory
  bool has_status_ = false;
  Status status_;
  int dots_x_ = 0, dots_y_ = 0, dots_w_ = 0, dots_h_ = 0, dot_r_ = 0;
  unsigned frame_ = 0;
  bool paused_ = false;
  void* mode_ = nullptr;  // drmModeModeInfo
};
