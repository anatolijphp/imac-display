// The bare screen: DRM/KMS dumb buffer on the connected panel, plus the waiting screen drawn with cairo.
// Video (stage 3) will replace the dumb buffer with GPU-imported pictures; the waiting screen stays.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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
    bool menu = false;     // Restart / Shut Down buttons and the link switch (only while no Mac is connected)
    bool wifi = false;     // the link in use: Wi-Fi (testing) instead of the Thunderbolt cable
  };
  void show(const Status& s);  // full redraw when something changed
  void animate();              // next frame (drifting background, dots); call 30 times a second
  bool save_png(const std::string& path) const;  // the current picture, for checking without looking
  // The boot splash (a Plymouth "script" theme) in the waiting screen's look: background and icon as
  // splash.png, dot.png, and the script that animates the dots. No screen needed.
  bool render_splash(const std::string& dir, int w, int h);
  void set_snapshot_dir(const std::string& d) { snap_dir_ = d; }  // testing: every redraw saved as NNN.png

  // Video borrows the panel: pause() stops drawing, target() describes the panel, resume() puts the
  // waiting screen back up (called by the video presenter before it frees its buffers).
  ScreenTarget target() const;
  void pause();
  bool paused() const { return paused_; }
  void resume();
  void blank();  // black panel (between two video streams), stays paused
  // Called by the video presenter (its thread) just before its first picture takes the panel: stops
  // the waiting screen and waits for its last page flip, so the two never share flip events.
  void hand_over();
  // The waiting screen's page flips: poll flip_fd() (-1 when none is pending), then on_drm_event().
  int flip_fd() const { return flip_pending_ ? fd_ : -1; }
  void on_drm_event();

  // The iMac's own mouse and keyboard on the waiting screen (outside a session): a pointer, the
  // Restart / Shut Down buttons and their confirmation dialog. press() returns the confirmed action.
  enum class Action { None, Restart, ShutDown, UseThunderbolt, UseWifi, WifiOpen, WifiJoin, WifiPassword };
  void pointer_motion(int dx, int dy);
  Action press(uint16_t code, int value);  // Linux key or button code, value 1 down / 0 up
  void idle_tick();                        // once a second: hides the pointer when unused, closes a stale dialog
  void show_busy(const std::string& text); // "Restarting…": replaces the dialog, buttons inactive

  // Wi-Fi panel. The owner fills in the networks; WifiJoin means "join wifi_pick()", WifiPassword
  // means "join wifi_pick() with take_wifi_password()".
  struct WifiRow { std::string name; int bars = 0; bool secured = false, connected = false; };
  void set_wifi(std::vector<WifiRow> rows, bool scanning, const std::string& status);
  void ask_wifi_password(const std::string& name, const std::string& error);
  bool wifi_open() const { return dialog_ == Dialog::Wifi || dialog_ == Dialog::WifiPassword; }
  std::string wifi_pick() const { return wifi_pick_; }
  std::string take_wifi_password();

 private:
  enum class Hit { None, Restart, ShutDown, Cancel, Confirm, LinkTb, LinkWifi, WifiButton, Row, Done, ShowPass };
  enum class Dialog { None, Restart, ShutDown, Busy, Wifi, WifiPassword };
  struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool in(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  void init_canvas();  // base_, the background field and dither tile, for w_ x h_
  void render_base(const Status& s);
  void redraw();  // the current status again, after a hover or dialog change
  Hit hit_test() const;
  void update_hover();
  void cursor(bool on);
  bool make_cursor();
  // Background with slowly drifting soft shadows: a low-resolution field updated every frame, scaled up
  // and dithered per pixel, with the content (base_) blended on top. frame() writes the whole screen.
  void update_field(double t);
  void compose(uint8_t* dst, int pitch);  // field + content, all rows (in parallel)
  void compose_rows(uint8_t* dst, int pitch, int y0, int y1);
  void content_spans();                   // per row: where base_ has anything (blending is skipped elsewhere)
  void frame();                           // a new background frame on the panel, with the dots

  int fd_ = -1;
  uint32_t conn_id_ = 0, crtc_id_ = 0, fb_id_ = 0, handle_ = 0, pitch_ = 0;
  uint64_t size_ = 0;
  uint8_t* map_ = nullptr;
  int w_ = 0, h_ = 0;
  void* saved_crtc_ = nullptr;  // drmModeCrtc* to restore the console's mode on close
  int tty_ = -1;                // our VT, switched to graphics mode so the text console stays quiet
  cairo_surface_t* base_ = nullptr;  // everything but the background (ARGB, transparent elsewhere)
  static constexpr int kCell = 16;   // background field: one value per 16 x 16 pixels
  int fw_ = 0, fh_ = 0;
  std::vector<int32_t> field_[3];    // per channel, 8.8 fixed point, (fw_ + 1) x (fh_ + 1)
  std::vector<int16_t> noise_;       // dither tile, 128 x 128, -256..256 (= -1..1 step)
  std::vector<std::pair<int, int>> spans_;  // per row [x0, x1) where base_ is not transparent
  uint64_t start_us_ = 0;
  std::vector<uint32_t> dots_bg_;    // the dots' rectangle as last composed, without the dots
  cairo_surface_t* dots_layer_ = nullptr;  // the dots alone (ARGB), drawn every frame
  void draw_dots();
  void blend_dots_row(int r, uint32_t* px);  // dots row r over px (dots_w_ pixels)

  // The waiting screen on the GPU: background, shadows, dither and dots computed per pixel in a
  // shader, the content (base_) as a texture uploaded when it changes. 30 frames a second at almost
  // no CPU. The CPU path above stays for snapshots and as a fallback (gl_ false).
  bool gl_init();
  void gl_close();
  void gl_frame();
  void gl_release_shown();  // drm_mu_ held: our buffer is off the panel (video, blank, resume)
  static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void* self);
  bool gl_ = false;
  void* gbm_ = nullptr;   // gbm_device*
  void* gs_ = nullptr;    // gbm_surface*
  void* edpy_ = nullptr;  // EGLDisplay
  void* ectx_ = nullptr;  // EGLContext
  void* esurf_ = nullptr; // EGLSurface
  unsigned prog_ = 0, tex_ = 0, vao_ = 0;
  int u_size_ = -1, u_sh_ = -1, u_dots_ = -1, u_dot_a_ = -1, u_show_dots_ = -1;
  bool content_dirty_ = true;
  void* shown_bo_ = nullptr;    // gbm_bo* on the panel
  void* pending_bo_ = nullptr;  // gbm_bo* waiting for its flip
  std::atomic<bool> flip_pending_{false};
  std::string gl_snap_path_;  // testing: read the next GPU frame back into this PNG
  std::mutex drm_mu_;
  bool force_frame_ = true;          // content changed: compose the whole screen at the next animate()
  bool has_status_ = false;
  Status status_;
  int dots_x_ = 0, dots_y_ = 0, dots_w_ = 0, dots_h_ = 0, dot_r_ = 0;
  unsigned frame_ = 0;
  std::atomic<bool> paused_{false};
  void* mode_ = nullptr;  // drmModeModeInfo

  Rect btn_restart_, btn_shutdown_, btn_wifi_, dlg_cancel_, dlg_confirm_, seg_tb_, seg_wifi_, dlg_done_, show_pass_;
  std::vector<Rect> row_rects_;
  int hover_row_ = -1;
  std::vector<WifiRow> wifi_rows_;
  bool wifi_scanning_ = false;
  std::string wifi_status_, wifi_pick_, wifi_pass_, wifi_pass_error_;
  bool wifi_show_pass_ = false, shift_down_ = false, caps_ = false;
  Dialog dialog_ = Dialog::None;
  std::string busy_text_;
  uint64_t dialog_us_ = 0;
  Hit hover_ = Hit::None, pressed_ = Hit::None;
  bool ctrl_down_ = false, alt_down_ = false;
  double px_ = 0, py_ = 0;           // pointer position on the panel
  bool cursor_on_ = false;
  uint64_t last_motion_us_ = 0;
  uint32_t cursor_handle_ = 0, cursor_w_ = 64, cursor_h_ = 64;
  int cursor_hot_ = 0;
  std::string snap_dir_;
  unsigned snap_n_ = 0;
};
