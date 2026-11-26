#include "display.h"

#include <cairo.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace {

// Colours (macOS dark appearance).
constexpr double kGrey[3] = {0x98 / 255.0, 0x98 / 255.0, 0x9d / 255.0};
constexpr double kBlue[3] = {0x0a / 255.0, 0x84 / 255.0, 0xff / 255.0};
const char* kFont = "Adwaita Sans";

void rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

// Background: a soft radial glow behind the icon fading to near-black, computed per pixel in floating
// point and dithered (triangular noise of +-1 step) to 8 bits. A plain 8-bit dark gradient has only ~20
// grey levels over the screen height, which shows as wide bands, more so after the panel's upscaling.
void paint_background(cairo_surface_t* surf, int w, int h) {
  cairo_surface_flush(surf);
  uint8_t* px = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  const double cx = w * 0.5, cy = h * 0.40, radius = h * 1.05;
  // Centre and edge colours in linear 0..255 sRGB units (slightly cool, like macOS dark).
  const double c0[3] = {44, 45, 50}, c1[3] = {8, 8, 10};
  uint32_t seed = 0x9e3779b9u;
  auto rnd = [&seed]() {  // xorshift32 -> [0,1)
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return (seed >> 8) * (1.0 / 16777216.0);
  };
  for (int y = 0; y < h; y++) {
    uint32_t* row = (uint32_t*)(px + y * stride);
    double dy = (y - cy) / radius;
    for (int x = 0; x < w; x++) {
      double dx = (x - cx) / radius;
      double t = std::sqrt(dx * dx + dy * dy);
      t = t > 1 ? 1 : t;
      t = t * t * (3 - 2 * t);           // smoothstep: soft centre, gentle fall-off
      double n = rnd() + rnd() - 1.0;    // triangular dither, -1..1 steps, same for r, g, b
      uint32_t out = 0;
      for (int c = 0; c < 3; c++) {
        double v = c0[c] + (c1[c] - c0[c]) * t + n;
        int iv = int(std::lround(v));
        iv = iv < 0 ? 0 : iv > 255 ? 255 : iv;
        out |= uint32_t(iv) << (16 - 8 * c);
      }
      row[x] = out;
    }
  }
  cairo_surface_mark_dirty(surf);
}

void centered_text(cairo_t* cr, const std::string& t, double cx, double y, double size, cairo_font_weight_t wt) {
  cairo_select_font_face(cr, kFont, CAIRO_FONT_SLANT_NORMAL, wt);
  cairo_set_font_size(cr, size);
  cairo_text_extents_t e;
  cairo_text_extents(cr, t.c_str(), &e);
  cairo_move_to(cr, cx - e.width / 2 - e.x_bearing, y);
  cairo_show_text(cr, t.c_str());
}

// An original laptop symbol (thin-bezel screen on a flat base) with a lightning bolt on the screen.
void draw_laptop(cairo_t* cr, double cx, double cy, double s, bool connected) {
  double sw = 620 * s, sh = 400 * s, lw = 14 * s;
  double sx = cx - sw / 2, sy = cy - sh / 2;
  cairo_set_line_width(cr, lw);
  cairo_set_source_rgb(cr, 1, 1, 1);
  rounded_rect(cr, sx, sy, sw, sh, 30 * s);
  cairo_stroke(cr);

  // Base: a wide slab with rounded ends, slightly wider than the screen, with a thumb notch.
  double bw = 780 * s, bh = 30 * s, by = sy + sh + 4 * s;
  rounded_rect(cr, cx - bw / 2, by, bw, bh, bh / 2);
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);  // thumb notch: dark grey, close to the glow behind it
  rounded_rect(cr, cx - 70 * s, by - 1, 140 * s, 11 * s, 5 * s);
  cairo_fill(cr);

  // Lightning bolt (Thunderbolt), centred on the screen.
  static const double bolt[][2] = {{0.60, 0.00}, {0.12, 0.58}, {0.46, 0.58}, {0.36, 1.00},
                                   {0.88, 0.40}, {0.54, 0.40}, {0.66, 0.00}};
  double bhgt = 200 * s, bwid = 140 * s;
  double bx = cx - bwid / 2, byy = cy - bhgt / 2;
  cairo_new_path(cr);
  for (auto& p : bolt) cairo_line_to(cr, bx + p[0] * bwid, byy + p[1] * bhgt);
  cairo_close_path(cr);
  if (connected) cairo_set_source_rgb(cr, kBlue[0], kBlue[1], kBlue[2]);
  else cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_fill(cr);
}

uint64_t mono_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// Restart symbol: an open circle (gap at the top right) ending in an arrowhead.
void draw_restart_icon(cairo_t* cr, double cx, double cy, double r, double lw) {
  cairo_set_line_width(cr, lw);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  const double a0 = -M_PI / 2 + 0.55, a1 = -M_PI / 2 + 2 * M_PI - 0.35;
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, a0, a1);
  cairo_stroke(cr);
  // Arrowhead at the end of the arc (a1), pointing along the direction of travel (clockwise).
  double ex = cx + r * std::cos(a1), ey = cy + r * std::sin(a1);
  double tx = -std::sin(a1), ty = std::cos(a1);  // tangent, clockwise
  double nx = std::cos(a1), ny = std::sin(a1);   // outward normal
  double L = lw * 2.2, W = lw * 1.9;
  cairo_move_to(cr, ex + tx * L, ey + ty * L);
  cairo_line_to(cr, ex + nx * W - tx * L * 0.2, ey + ny * W - ty * L * 0.2);
  cairo_line_to(cr, ex - nx * W - tx * L * 0.2, ey - ny * W - ty * L * 0.2);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// Power symbol: a circle open at the top with a vertical bar through the gap.
void draw_power_icon(cairo_t* cr, double cx, double cy, double r, double lw) {
  cairo_set_line_width(cr, lw);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, -M_PI / 2 + 0.75, -M_PI / 2 + 2 * M_PI - 0.75);
  cairo_stroke(cr);
  cairo_move_to(cr, cx, cy - r * 1.15);
  cairo_line_to(cr, cx, cy - r * 0.15);
  cairo_stroke(cr);
}

}  // namespace

Display::~Display() { close(); }

bool Display::open() {
  if (fd_ >= 0) return true;
  DIR* d = opendir("/dev/dri");
  if (!d) return false;
  std::string path;
  int fd = -1;
  drmModeConnector* conn = nullptr;
  drmModeRes* res = nullptr;
  while (dirent* e = readdir(d)) {
    if (strncmp(e->d_name, "card", 4)) continue;
    path = std::string("/dev/dri/") + e->d_name;
    fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    // Skip the firmware framebuffer (simpledrm): it exists for a few seconds at boot until amdgpu
    // replaces it, and a screen taken from it would go dead. Wait for the real driver instead.
    if (drmVersionPtr v = drmGetVersion(fd)) {
      bool firmware_fb = !strcmp(v->name, "simpledrm");
      drmFreeVersion(v);
      if (firmware_fb) { ::close(fd); fd = -1; continue; }
    }
    res = drmModeGetResources(fd);
    for (int i = 0; res && i < res->count_connectors && !conn; i++) {
      drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
      if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) conn = c;
      else drmModeFreeConnector(c);
    }
    if (conn) break;
    if (res) drmModeFreeResources(res);
    res = nullptr;
    ::close(fd);
    fd = -1;
  }
  closedir(d);
  if (!conn) return false;
  if (drmSetMaster(fd) != 0) {  // someone else (a test viewer) owns the screen
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    ::close(fd);
    return false;
  }

  drmModeModeInfo mode = conn->modes[0];
  for (int i = 0; i < conn->count_modes; i++)
    if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = conn->modes[i]; break; }

  // A CRTC that can drive this connector: the current one if any, else the first possible.
  uint32_t crtc = 0;
  if (conn->encoder_id) {
    if (drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id)) { crtc = enc->crtc_id; drmModeFreeEncoder(enc); }
  }
  for (int i = 0; !crtc && i < conn->count_encoders; i++) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[i]);
    if (!enc) continue;
    for (int j = 0; j < res->count_crtcs; j++)
      if (enc->possible_crtcs & (1u << j)) { crtc = res->crtcs[j]; break; }
    drmModeFreeEncoder(enc);
  }
  conn_id_ = conn->connector_id;
  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  if (!crtc) { drmDropMaster(fd); ::close(fd); return false; }
  crtc_id_ = crtc;
  w_ = mode.hdisplay;
  h_ = mode.vdisplay;

  drm_mode_create_dumb cd{};
  cd.width = w_; cd.height = h_; cd.bpp = 32;
  if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) { drmDropMaster(fd); ::close(fd); return false; }
  handle_ = cd.handle; pitch_ = cd.pitch; size_ = cd.size;
  uint32_t handles[4] = {handle_}, pitches[4] = {pitch_}, offsets[4] = {0};
  drm_mode_map_dumb md{};
  md.handle = handle_;
  if (drmModeAddFB2(fd, w_, h_, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb_id_, 0) != 0 ||
      drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0 ||
      (map_ = (uint8_t*)mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset)) == MAP_FAILED) {
    map_ = nullptr;
    fd_ = fd;
    close();
    return false;
  }
  fd_ = fd;
  memset(map_, 0, size_);
  saved_crtc_ = drmModeGetCrtc(fd_, crtc_id_);
  mode_ = new drmModeModeInfo(mode);
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, &mode) != 0) {
    fprintf(stderr, "display: setting mode %dx%d failed: %s\n", w_, h_, strerror(errno));
    close();
    return false;
  }
  // If we run on a VT (systemd TTYPath), switch it to graphics mode: the text console stops drawing.
  if (isatty(0) && ioctl(0, KDSETMODE, KD_GRAPHICS) == 0) tty_ = 0;
  base_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
  bg_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
  paint_background(bg_, w_, h_);
  px_ = w_ / 2.0;
  py_ = h_ * 0.62;
  dialog_ = Dialog::None;
  if (!make_cursor()) fprintf(stderr, "display: no pointer (hardware cursor not available)\n");
  fprintf(stderr, "display: %s, %dx%d@%d, showing the waiting screen\n", path.c_str(), w_, h_, mode.vrefresh);
  has_status_ = false;
  return true;
}

void Display::close() {
  if (fd_ < 0) return;
  if (saved_crtc_) {
    auto* c = (drmModeCrtc*)saved_crtc_;
    if (c->buffer_id) drmModeSetCrtc(fd_, c->crtc_id, c->buffer_id, c->x, c->y, &conn_id_, 1, &c->mode);
    drmModeFreeCrtc(c);
    saved_crtc_ = nullptr;
  }
  cursor(false);
  if (cursor_handle_) {
    drm_mode_destroy_dumb dd{};
    dd.handle = cursor_handle_;
    drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    cursor_handle_ = 0;
  }
  if (tty_ >= 0) { ioctl(tty_, KDSETMODE, KD_TEXT); tty_ = -1; }
  if (map_) munmap(map_, size_);
  map_ = nullptr;
  if (fb_id_) drmModeRmFB(fd_, fb_id_);
  fb_id_ = 0;
  if (handle_) {
    drm_mode_destroy_dumb dd{};
    dd.handle = handle_;
    drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    handle_ = 0;
  }
  drmDropMaster(fd_);
  ::close(fd_);
  fd_ = -1;
  if (base_) cairo_surface_destroy(base_);
  base_ = nullptr;
  if (bg_) cairo_surface_destroy(bg_);
  bg_ = nullptr;
  delete (drmModeModeInfo*)mode_;
  mode_ = nullptr;
  paused_ = false;
  fprintf(stderr, "display: screen given back to the console\n");
}

void Display::render_base(const Status& st) {
  cairo_t* cr = cairo_create(base_);
  cairo_set_source_surface(cr, bg_, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  double s = h_ / 2160.0, cx = w_ / 2.0;

  double icon_cy = h_ * 0.40;
  draw_laptop(cr, cx, icon_cy, s, st.state == State::Connected);

  std::string title, sub;
  switch (st.state) {
    case State::NoCable:
      title = "Waiting for your Mac";
      sub = st.wifi ? "Wi-Fi is not connected" : "Connect the Thunderbolt cable to your MacBook";
      break;
    case State::WaitingForMac:
      title = "Waiting for your Mac";
      sub = st.wifi ? "On Wi-Fi. Start iMac Display on your MacBook." : "Cable connected. Start iMac Display on your MacBook.";
      break;
    case State::Connected:
      title = st.mac_name.empty() ? "Connected" : "Connected to " + st.mac_name;
      sub = st.detail.empty() ? "Waiting for the first picture" : st.detail;
      break;
  }
  fprintf(stderr, "display: waiting screen \"%s\" / \"%s\"\n", title.c_str(), sub.c_str());
  double ty = icon_cy + 360 * s;
  cairo_set_source_rgb(cr, 1, 1, 1);
  centered_text(cr, title, cx, ty, 72 * s, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_source_rgb(cr, kGrey[0], kGrey[1], kGrey[2]);
  centered_text(cr, sub, cx, ty + 80 * s, 40 * s, CAIRO_FONT_WEIGHT_NORMAL);

  dot_r_ = int(9 * s);
  dots_w_ = int(120 * s);
  dots_h_ = dot_r_ * 2 + 4;
  dots_x_ = int(cx - dots_w_ / 2.0);
  dots_y_ = int(ty + 150 * s);

  if (!st.footer.empty()) {
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.37);
    centered_text(cr, st.footer, cx, h_ - 60 * s, 26 * s, CAIRO_FONT_WEIGHT_NORMAL);
  }

  // Restart and Shut Down, like the buttons under the macOS login window: round, translucent, label below.
  btn_restart_ = btn_shutdown_ = dlg_cancel_ = dlg_confirm_ = seg_tb_ = seg_wifi_ = Rect{};
  if (st.menu) {
    // Link switch: a two-part control, the one in use filled.
    const double sw = 280 * s, sh = 68 * s, sy = h_ - 560 * s;
    seg_tb_ = Rect{cx - sw, sy, sw, sh};
    seg_wifi_ = Rect{cx, sy, sw, sh};
    rounded_rect(cr, cx - sw, sy, 2 * sw, sh, sh / 2);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_fill(cr);
    struct S { Rect* rc; const char* label; bool on; Hit hit; };
    S segs[2] = {{&seg_tb_, "Thunderbolt", !st.wifi, Hit::LinkTb}, {&seg_wifi_, "Wi-Fi", st.wifi, Hit::LinkWifi}};
    for (auto& g : segs) {
      bool hot = dialog_ == Dialog::None && hover_ == g.hit && !g.on;
      if (g.on || hot) {
        rounded_rect(cr, g.rc->x + 4 * s, g.rc->y + 4 * s, g.rc->w - 8 * s, g.rc->h - 8 * s, (sh - 8 * s) / 2);
        if (g.on) cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], 0.9);
        else cairo_set_source_rgba(cr, 1, 1, 1, 0.14);
        cairo_fill(cr);
      }
      cairo_set_source_rgba(cr, 1, 1, 1, g.on ? 1.0 : 0.7);
      centered_text(cr, g.label, g.rc->x + g.rc->w / 2, g.rc->y + g.rc->h / 2 + 10 * s, 28 * s,
                    g.on ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    }
    if (st.wifi) {
      cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
      centered_text(cr, "Wi-Fi is for testing: slower, and the iMac's own macOS drive is not shared", cx,
                    sy + sh + 46 * s, 24 * s, CAIRO_FONT_WEIGHT_NORMAL);
    }

    const double r = 56 * s, gap = 300 * s, by = h_ - 330 * s;
    struct B { Rect* rect; double x; const char* label; Hit hit; bool restart; };
    B bs[2] = {{&btn_restart_, cx - gap / 2, "Restart", Hit::Restart, true},
               {&btn_shutdown_, cx + gap / 2, "Shut Down", Hit::ShutDown, false}};
    for (auto& b : bs) {
      bool active = dialog_ == Dialog::None;
      bool hot = active && hover_ == b.hit;
      cairo_set_source_rgba(cr, 1, 1, 1, hot ? (pressed_ == b.hit ? 0.40 : 0.28) : 0.14);
      cairo_arc(cr, b.x, by, r, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.95 : 0.5);
      if (b.restart) draw_restart_icon(cr, b.x, by, r * 0.42, 6 * s);
      else draw_power_icon(cr, b.x, by, r * 0.42, 6 * s);
      cairo_set_source_rgba(cr, 1, 1, 1, hot ? 1.0 : 0.75);
      centered_text(cr, b.label, b.x, by + r + 50 * s, 28 * s, CAIRO_FONT_WEIGHT_NORMAL);
      *b.rect = Rect{b.x - r * 1.4, by - r, r * 2.8, r * 2 + 70 * s};  // circle and label
    }
  }

  // Confirmation dialog (or "Restarting…"), centred over everything.
  if (dialog_ != Dialog::None) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);  // dim the screen behind it
    cairo_paint(cr);
    const double dw = 1240 * s, dh = dialog_ == Dialog::Busy ? 220 * s : 400 * s;
    const double dx = cx - dw / 2, dy = h_ / 2.0 - dh / 2;
    rounded_rect(cr, dx, dy, dw, dh, 28 * s);
    cairo_set_source_rgb(cr, 0.17, 0.17, 0.19);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_set_line_width(cr, 2 * s);
    cairo_stroke(cr);
    if (dialog_ == Dialog::Busy) {
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, busy_text_, cx, dy + dh / 2 + 18 * s, 48 * s, CAIRO_FONT_WEIGHT_BOLD);
    } else {
      bool restart = dialog_ == Dialog::Restart;
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, restart ? "Restart the iMac now?" : "Shut down the iMac now?", cx, dy + 110 * s, 46 * s,
                    CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_source_rgb(cr, kGrey[0], kGrey[1], kGrey[2]);
      centered_text(cr, restart ? "The iMac starts macOS. To come back to this screen, hold the Option key"
                                : "To use it as a display again, hold the Option key while switching it on",
                    cx, dy + 180 * s, 30 * s, CAIRO_FONT_WEIGHT_NORMAL);
      centered_text(cr, restart ? "while it starts and choose \u201cEFI Boot\u201d."
                                : "and choose \u201cEFI Boot\u201d.",
                    cx, dy + 222 * s, 30 * s, CAIRO_FONT_WEIGHT_NORMAL);
      const double bw = 300 * s, bh = 76 * s, byy = dy + dh - bh - 44 * s;
      dlg_cancel_ = Rect{cx - bw - 16 * s, byy, bw, bh};
      dlg_confirm_ = Rect{cx + 16 * s, byy, bw, bh};
      struct DB { Rect* rc; const char* label; Hit hit; bool primary; };
      DB dbs[2] = {{&dlg_cancel_, "Cancel", Hit::Cancel, false},
                   {&dlg_confirm_, restart ? "Restart" : "Shut Down", Hit::Confirm, true}};
      for (auto& b : dbs) {
        bool hot = hover_ == b.hit;
        rounded_rect(cr, b.rc->x, b.rc->y, b.rc->w, b.rc->h, 16 * s);
        if (b.primary) cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], hot ? 1.0 : 0.85);
        else cairo_set_source_rgba(cr, 1, 1, 1, hot ? 0.28 : 0.16);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        centered_text(cr, b.label, b.rc->x + b.rc->w / 2, b.rc->y + b.rc->h / 2 + 12 * s, 32 * s,
                      CAIRO_FONT_WEIGHT_BOLD);
      }
    }
  }
  cairo_destroy(cr);
  cairo_surface_flush(base_);
}

void Display::copy_rect(int x, int y, int w, int h) {
  const uint8_t* src = cairo_image_surface_get_data(base_);
  int stride = cairo_image_surface_get_stride(base_);
  for (int r = y; r < y + h && r < h_; r++) memcpy(map_ + r * pitch_ + x * 4, src + r * stride + x * 4, w * 4);
}

ScreenTarget Display::target() const {
  ScreenTarget t;
  t.fd = fd_;
  t.crtc_id = crtc_id_;
  t.conn_id = conn_id_;
  t.mode = mode_;
  t.w = w_;
  t.h = h_;
  return t;
}

void Display::resume() {
  if (fd_ < 0) return;
  paused_ = false;
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_) != 0)
    fprintf(stderr, "display: taking the screen back failed: %s\n", strerror(errno));
  has_status_ = false;  // force a full redraw on the next show()
  if (base_) { copy_rect(0, 0, w_, h_); drmModeDirtyFB(fd_, fb_id_, nullptr, 0); }
}

void Display::pause() {
  paused_ = true;
  cursor(false);
  if (dialog_ != Dialog::Busy) dialog_ = Dialog::None;
  hover_ = pressed_ = Hit::None;
}

void Display::blank() {
  if (fd_ < 0) return;
  pause();
  memset(map_, 0, size_);
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_) != 0)
    fprintf(stderr, "display: blank failed: %s\n", strerror(errno));
  has_status_ = false;  // the waiting screen is redrawn in full when it comes back
}

void Display::show(const Status& s) {
  if (fd_ < 0 || paused_) return;
  if (has_status_ && s.state == status_.state && s.mac_name == status_.mac_name && s.footer == status_.footer &&
      s.detail == status_.detail && s.menu == status_.menu)
    return;
  if (!s.menu && dialog_ != Dialog::Busy) {  // a Mac connected: the buttons and their dialog go away
    dialog_ = Dialog::None;
    hover_ = pressed_ = Hit::None;
    cursor(false);
  }
  status_ = s;
  has_status_ = true;
  render_base(s);
  copy_rect(0, 0, w_, h_);
  drmModeDirtyFB(fd_, fb_id_, nullptr, 0);
  animate();
}

void Display::animate() {
  if (fd_ < 0 || !has_status_ || paused_) return;
  frame_++;
  // Draw the dots on a copy of the base picture in that small rectangle, then copy it out.
  cairo_surface_t* tmp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dots_w_, dots_h_);
  cairo_t* cr = cairo_create(tmp);
  cairo_set_source_surface(cr, base_, -dots_x_, -dots_y_);
  cairo_paint(cr);
  {
    double t = frame_ / 15.0;  // seconds at 15 fps
    for (int i = 0; i < 3; i++) {
      double phase = t * 2 * M_PI / 1.4 - i * 0.9;  // one wave every 1.4 s, left to right
      double a = 0.25 + 0.75 * (0.5 + 0.5 * std::sin(phase));
      cairo_set_source_rgba(cr, 1, 1, 1, a);
      cairo_arc(cr, dots_w_ / 2.0 + (i - 1) * dot_r_ * 4, dots_h_ / 2.0, dot_r_, 0, 2 * M_PI);
      cairo_fill(cr);
    }
  }
  cairo_destroy(cr);
  cairo_surface_flush(tmp);
  const uint8_t* src = cairo_image_surface_get_data(tmp);
  int stride = cairo_image_surface_get_stride(tmp);
  for (int r = 0; r < dots_h_; r++) memcpy(map_ + (dots_y_ + r) * pitch_ + dots_x_ * 4, src + r * stride, dots_w_ * 4);
  cairo_surface_destroy(tmp);
  drmModeDirtyFB(fd_, fb_id_, nullptr, 0);
}

bool Display::save_png(const std::string& path) const {
  return base_ && cairo_surface_write_to_png(base_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

// --- pointer, buttons, dialog -----------------------------------------------------------------

void Display::redraw() {
  if (fd_ < 0 || paused_ || !has_status_) return;
  render_base(status_);
  copy_rect(0, 0, w_, h_);
  drmModeDirtyFB(fd_, fb_id_, nullptr, 0);
  animate();
  if (!snap_dir_.empty()) {
    char name[32];
    snprintf(name, sizeof name, "/%03u.png", snap_n_++);
    // PNG compression of a 4K picture takes about a second: on a copy, in the background, so the
    // pointer keeps up while testing.
    cairo_surface_t* copy = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    cairo_t* cr = cairo_create(copy);
    cairo_set_source_surface(cr, base_, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    std::thread([copy, path = snap_dir_ + name] {
      cairo_surface_write_to_png(copy, path.c_str());
      cairo_surface_destroy(copy);
    }).detach();
  }
}

bool Display::make_cursor() {
  uint64_t cw = 64, ch = 64;
  drmGetCap(fd_, DRM_CAP_CURSOR_WIDTH, &cw);
  drmGetCap(fd_, DRM_CAP_CURSOR_HEIGHT, &ch);
  cursor_w_ = uint32_t(std::min<uint64_t>(cw, 128));
  cursor_h_ = uint32_t(std::min<uint64_t>(ch, 128));
  drm_mode_create_dumb cd{};
  cd.width = cursor_w_; cd.height = cursor_h_; cd.bpp = 32;
  if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) return false;
  cursor_handle_ = cd.handle;
  drm_mode_map_dumb md{};
  md.handle = cd.handle;
  if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) return false;
  void* m = mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, md.offset);
  if (m == MAP_FAILED) return false;
  memset(m, 0, cd.size);
  // The classic arrow: black with a white outline, premultiplied ARGB like the cursor plane expects.
  cairo_surface_t* cs = cairo_image_surface_create_for_data((unsigned char*)m, CAIRO_FORMAT_ARGB32, cursor_w_,
                                                            cursor_h_, int(cd.pitch));
  cairo_t* cr = cairo_create(cs);
  const double k = std::min(cursor_h_, 64u) / 64.0 * 2.9 * std::max(1.0, h_ / 2160.0);  // ~50 px tall at 4K
  const double pts[][2] = {{0, 0}, {0, 16}, {3.8, 12.4}, {6.4, 18.2}, {8.6, 17.2}, {6.1, 11.6}, {11.2, 11.6}};
  cursor_hot_ = 3;
  cairo_new_path(cr);
  for (auto& p : pts) cairo_line_to(cr, cursor_hot_ + p[0] * k, cursor_hot_ + p[1] * k);
  cairo_close_path(cr);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_fill_preserve(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_set_line_width(cr, 1.3 * k / 2.9 * 2);
  cairo_stroke(cr);
  cairo_destroy(cr);
  cairo_surface_flush(cs);
  cairo_surface_destroy(cs);
  munmap(m, cd.size);
  return true;
}

void Display::cursor(bool on) {
  if (fd_ < 0 || !cursor_handle_ || on == cursor_on_) return;
  cursor_on_ = on;
  if (on) {
    drmModeSetCursor(fd_, crtc_id_, cursor_handle_, cursor_w_, cursor_h_);
    drmModeMoveCursor(fd_, crtc_id_, int(px_) - cursor_hot_, int(py_) - cursor_hot_);
  } else {
    drmModeSetCursor(fd_, crtc_id_, 0, 0, 0);
  }
}

Display::Hit Display::hit_test() const {
  if (dialog_ == Dialog::Restart || dialog_ == Dialog::ShutDown) {
    if (dlg_cancel_.in(px_, py_)) return Hit::Cancel;
    if (dlg_confirm_.in(px_, py_)) return Hit::Confirm;
    return Hit::None;
  }
  if (dialog_ == Dialog::None) {
    if (btn_restart_.in(px_, py_)) return Hit::Restart;
    if (btn_shutdown_.in(px_, py_)) return Hit::ShutDown;
    if (seg_tb_.in(px_, py_)) return Hit::LinkTb;
    if (seg_wifi_.in(px_, py_)) return Hit::LinkWifi;
  }
  return Hit::None;
}

void Display::update_hover() {
  Hit h = hit_test();
  if (h != hover_) {
    hover_ = h;
    redraw();
  }
}

void Display::pointer_motion(int dx, int dy) {
  if (fd_ < 0 || paused_ || !has_status_ || !status_.menu) return;
  // Simple acceleration: slow movements stay precise, fast ones cross the 4K panel quickly.
  double v = std::sqrt(double(dx) * dx + double(dy) * dy);
  double f = (v < 3 ? 1.5 : v < 10 ? 2.5 : 4.0) * std::max(1.0, h_ / 2160.0);
  px_ = std::min(std::max(px_ + dx * f, 0.0), double(w_ - 1));
  py_ = std::min(std::max(py_ + dy * f, 0.0), double(h_ - 1));
  last_motion_us_ = mono_us();
  if (!cursor_on_) cursor(true);
  else drmModeMoveCursor(fd_, crtc_id_, int(px_) - cursor_hot_, int(py_) - cursor_hot_);
  update_hover();
}

Display::Action Display::press(uint16_t code, int value) {
  if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) ctrl_down_ = value;
  if (code == KEY_LEFTALT || code == KEY_RIGHTALT) alt_down_ = value;
  if (fd_ < 0 || paused_ || !has_status_ || !status_.menu || dialog_ == Dialog::Busy) return Action::None;
  bool asking = dialog_ == Dialog::Restart || dialog_ == Dialog::ShutDown;
  auto open_dialog = [&](Dialog d) {
    dialog_ = d;
    dialog_us_ = mono_us();
    hover_ = Hit::None;
    redraw();
    hover_ = hit_test();
    if (hover_ != Hit::None) redraw();
    fprintf(stderr, "display: asking to %s\n", d == Dialog::Restart ? "restart" : "shut down");
  };
  auto confirm = [&]() {
    Action a = dialog_ == Dialog::Restart ? Action::Restart : Action::ShutDown;
    dialog_ = Dialog::None;
    return a;
  };
  auto cancel = [&]() {
    dialog_ = Dialog::None;
    hover_ = Hit::None;
    redraw();
    update_hover();
    fprintf(stderr, "display: cancelled\n");
  };

  if (code == BTN_LEFT) {
    if (value) {
      pressed_ = hover_;
      if (pressed_ == Hit::Restart || pressed_ == Hit::ShutDown) redraw();
      return Action::None;
    }
    Hit was = pressed_;
    pressed_ = Hit::None;
    if (was == Hit::None || was != hit_test()) { if (was != Hit::None) redraw(); return Action::None; }
    switch (was) {
      case Hit::Restart: open_dialog(Dialog::Restart); break;
      case Hit::ShutDown: open_dialog(Dialog::ShutDown); break;
      case Hit::Cancel: cancel(); break;
      case Hit::Confirm: return confirm();
      case Hit::LinkTb: if (status_.wifi) return Action::UseThunderbolt; break;
      case Hit::LinkWifi: if (!status_.wifi) return Action::UseWifi; break;
      case Hit::None: break;
    }
    return Action::None;
  }
  if (!value) return Action::None;
  if (asking && code == KEY_ESC) cancel();
  else if (asking && (code == KEY_ENTER || code == KEY_KPENTER)) return confirm();
  else if (!asking && code == KEY_DELETE && ctrl_down_ && alt_down_) open_dialog(Dialog::Restart);  // keyboard only
  return Action::None;
}

void Display::idle_tick() {
  uint64_t now = mono_us();
  if (cursor_on_ && now - last_motion_us_ > 10000000 && dialog_ == Dialog::None) {
    cursor(false);  // like macOS: the pointer goes away when nobody uses it
    if (hover_ != Hit::None) { hover_ = Hit::None; redraw(); }
  }
  if ((dialog_ == Dialog::Restart || dialog_ == Dialog::ShutDown) && now - dialog_us_ > 60000000) {
    dialog_ = Dialog::None;  // nobody answered: nothing happens
    hover_ = Hit::None;
    redraw();
    fprintf(stderr, "display: dialog closed after 60 s without an answer\n");
  }
}

void Display::show_busy(const std::string& text) {
  busy_text_ = text;
  dialog_ = Dialog::Busy;
  hover_ = pressed_ = Hit::None;
  cursor(false);
  redraw();
}
