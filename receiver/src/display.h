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
    bool menu = false;     // Restart / Shut Down buttons and the link switch (only while no Mac is connected)
    bool wifi = false;     // the link in use: Wi-Fi (testing) instead of the Thunderbolt cable
  };
  void show(const Status& s);  // full redraw when something changed
  void animate();              // next frame of the "waiting" dots; call ~15 times a second
  bool save_png(const std::string& path) const;  // the current picture, for checking without looking
  void set_snapshot_dir(const std::string& d) { snap_dir_ = d; }  // testing: every redraw saved as NNN.png

  // Video borrows the panel: pause() stops drawing, target() describes the panel, resume() puts the
  // waiting screen back up (called by the video presenter before it frees its buffers).
  ScreenTarget target() const;
  void pause();
  bool paused() const { return paused_; }
  void resume();
  void blank();  // black panel (between two video streams), stays paused

  // The iMac's own mouse and keyboard on the waiting screen (outside a session): a pointer, the
  // Restart / Shut Down buttons and their confirmation dialog. press() returns the confirmed action.
  enum class Action { None, Restart, ShutDown, UseThunderbolt, UseWifi };
  void pointer_motion(int dx, int dy);
  Action press(uint16_t code, int value);  // Linux key or button code, value 1 down / 0 up
  void idle_tick();                        // once a second: hides the pointer when unused, closes a stale dialog
  void show_busy(const std::string& text); // "Restarting…": replaces the dialog, buttons inactive

 private:
  enum class Hit { None, Restart, ShutDown, Cancel, Confirm, LinkTb, LinkWifi };
  enum class Dialog { None, Restart, ShutDown, Busy };
  struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool in(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  void render_base(const Status& s);
  void redraw();  // the current status again, after a hover or dialog change
  Hit hit_test() const;
  void update_hover();
  void cursor(bool on);
  bool make_cursor();
  void copy_rect(int x, int y, int w, int h);  // base surface -> scanout buffer

  int fd_ = -1;
  uint32_t conn_id_ = 0, crtc_id_ = 0, fb_id_ = 0, handle_ = 0, pitch_ = 0;
  uint64_t size_ = 0;
  uint8_t* map_ = nullptr;
  int w_ = 0, h_ = 0;
  void* saved_crtc_ = nullptr;  // drmModeCrtc* to restore the console's mode on close
  int tty_ = -1;                // our VT, switched to graphics mode so the text console stays quiet
  cairo_surface_t* base_ = nullptr;  // static picture in system memory
  cairo_surface_t* bg_ = nullptr;    // the dithered background alone (slow to compute: done once)
  bool has_status_ = false;
  Status status_;
  int dots_x_ = 0, dots_y_ = 0, dots_w_ = 0, dots_h_ = 0, dot_r_ = 0;
  unsigned frame_ = 0;
  bool paused_ = false;
  void* mode_ = nullptr;  // drmModeModeInfo

  Rect btn_restart_, btn_shutdown_, dlg_cancel_, dlg_confirm_, seg_tb_, seg_wifi_;
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
