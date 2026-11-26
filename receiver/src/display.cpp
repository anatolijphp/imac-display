#include "display.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cairo.h>
#include <gbm.h>
#include <poll.h>
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
#include <cstdlib>
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

// Background colours: a soft radial glow behind the icon fading to near-black (slightly cool, like macOS
// dark), in 0..255 sRGB units. Dithered to 8 bits (a plain 8-bit dark gradient shows as wide bands).
constexpr double kGlow[3] = {44, 45, 50}, kEdge[3] = {8, 8, 10};

// Soft shadows drifting over the glow: each a large Gaussian blob on a slow Lissajous path.
// Positions and amplitudes in screen heights, periods in seconds, depth = darkening at the centre.
struct Shadow { double x, y, ax, ay, px, py, phase, radius, depth; };
constexpr Shadow kShadows[] = {
    {0.55, 0.30, 0.45, 0.20, 33, 24, 0.0, 0.34, 0.45},
    {1.30, 0.75, 0.40, 0.25, 39, 28, 1.7, 0.40, 0.40},
    {0.20, 0.85, 0.35, 0.18, 27, 36, 3.1, 0.30, 0.38},
    {1.10, 0.10, 0.50, 0.15, 45, 21, 4.4, 0.36, 0.35},
};

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

// Wi-Fi symbol: three arcs over a dot, fanning upwards. bars < 0: all full; else 1..4 shown full, rest faint.
void draw_wifi_icon(cairo_t* cr, double cx, double cy, double r, double lw, int bars = -1, double alpha = 1.0) {
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, lw);
  // The dot sits low, the arcs fan out above it. Placed so the whole symbol (top of the outer arc to
  // the bottom of the dot) is centred on cy.
  double base = cy + 0.63 * r - 0.2 * lw;
  for (int i = 0; i < 4; i++) {
    bool on = bars < 0 || i < bars;
    cairo_set_source_rgba(cr, 1, 1, 1, alpha * (on ? 1.0 : 0.25));
    if (i == 0) {
      cairo_arc(cr, cx, base, lw * 0.9, 0, 2 * M_PI);
      cairo_fill(cr);
      continue;
    }
    double rr = r * 0.42 * i;
    cairo_new_path(cr);
    cairo_arc(cr, cx, base, rr, -M_PI / 2 - 0.78, -M_PI / 2 + 0.78);
    cairo_stroke(cr);
  }
}

void draw_lock(cairo_t* cr, double x, double y, double s) {  // x, y: top left; ~22 x 28 units
  cairo_set_line_width(cr, 3 * s);
  cairo_new_path(cr);
  cairo_arc(cr, x + 11 * s, y + 11 * s, 7 * s, M_PI, 2 * M_PI);
  cairo_stroke(cr);
  rounded_rect(cr, x, y + 11 * s, 22 * s, 17 * s, 3 * s);
  cairo_fill(cr);
}

void left_text(cairo_t* cr, const std::string& t, double x, double y, double size, cairo_font_weight_t wt) {
  cairo_select_font_face(cr, kFont, CAIRO_FONT_SLANT_NORMAL, wt);
  cairo_set_font_size(cr, size);
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, t.c_str());
}

// Text typed on a keyboard with the US layout (the password field). 0: not a character.
char key_char(uint16_t code, bool shift, bool caps) {
  static const char* lower[] = {"", "", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "", "",
                                "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "[", "]", "", "", "a", "s",
                                "d", "f", "g", "h", "j", "k", "l", ";", "'", "`", "", "\\", "z", "x", "c", "v",
                                "b", "n", "m", ",", ".", "/"};
  static const char* upper[] = {"", "", "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "_", "+", "", "",
                                "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "{", "}", "", "", "A", "S",
                                "D", "F", "G", "H", "J", "K", "L", ":", "\"", "~", "", "|", "Z", "X", "C", "V",
                                "B", "N", "M", "<", ">", "?"};
  if (code == KEY_SPACE) return ' ';
  if (code >= sizeof lower / sizeof lower[0] || !lower[code][0]) return 0;
  char c = lower[code][0];
  bool letter = c >= 'a' && c <= 'z';
  bool up = letter ? (shift != caps) : shift;
  return up ? upper[code][0] : c;
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
  base_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w_, h_);
  fw_ = (w_ + kCell - 1) / kCell;
  fh_ = (h_ + kCell - 1) / kCell;
  for (auto& f : field_) f.assign(size_t(fw_ + 2) * (fh_ + 2), 0);
  noise_.resize(128 * 128);
  uint32_t seed = 0x9e3779b9u;
  auto rnd = [&seed]() {  // xorshift32 -> 0..255
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return int(seed >> 24);
  };
  for (auto& n : noise_) n = int16_t(rnd() + rnd() - 255);  // triangular, about -1..1 step
  spans_.assign(h_, {0, 0});
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  start_us_ = uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
  px_ = w_ / 2.0;
  py_ = h_ * 0.62;
  dialog_ = Dialog::None;
  if (!make_cursor()) fprintf(stderr, "display: no pointer (hardware cursor not available)\n");
  gl_ = gl_init();
  fprintf(stderr, "display: waiting screen drawn %s\n", gl_ ? "on the GPU, 30 frames a second" : "by the CPU (GPU setup failed)");
  fprintf(stderr, "display: %s, %dx%d@%d, showing the waiting screen\n", path.c_str(), w_, h_, mode.vrefresh);
  has_status_ = false;
  return true;
}

void Display::close() {
  if (fd_ < 0) return;
  gl_close();
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
  if (dots_layer_) cairo_surface_destroy(dots_layer_);
  dots_layer_ = nullptr;

  delete (drmModeModeInfo*)mode_;
  mode_ = nullptr;
  paused_ = false;
  fprintf(stderr, "display: screen given back to the console\n");
}

void Display::render_base(const Status& st) {
  cairo_t* cr = cairo_create(base_);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);  // transparent: the moving background shows through
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
  if (!dots_layer_ || cairo_image_surface_get_width(dots_layer_) != dots_w_ ||
      cairo_image_surface_get_height(dots_layer_) != dots_h_) {
    if (dots_layer_) cairo_surface_destroy(dots_layer_);
    dots_layer_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dots_w_, dots_h_);
  }
  dots_bg_.assign(size_t(dots_w_) * dots_h_, 0);
  draw_dots();

  if (!st.footer.empty()) {
    cairo_set_source_rgb(cr, 0.35, 0.35, 0.37);
    centered_text(cr, st.footer, cx, h_ - 60 * s, 26 * s, CAIRO_FONT_WEIGHT_NORMAL);
  }

  // Restart and Shut Down, like the buttons under the macOS login window: round, translucent, label below.
  btn_restart_ = btn_shutdown_ = btn_wifi_ = dlg_cancel_ = dlg_confirm_ = seg_tb_ = seg_wifi_ = dlg_done_ =
      show_pass_ = Rect{};
  row_rects_.clear();
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
      bool wifi_half = g.hit == Hit::LinkWifi;
      // The Wi-Fi half also holds a small icon button at the toggle's right end.
      const double d = 42 * s, gap = 0;
      cairo_select_font_face(cr, kFont, CAIRO_FONT_SLANT_NORMAL, g.on ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 28 * s);
      cairo_text_extents_t te;
      cairo_text_extents(cr, g.label, &te);
      // Wi-Fi's label moves a little to the left, so it has some room from the icon at the right end.
      double label_cx = g.rc->x + g.rc->w / 2 - (wifi_half ? d * 0.45 : 0);
      (void)gap;
      cairo_set_source_rgba(cr, 1, 1, 1, g.on ? 1.0 : 0.7);
      centered_text(cr, g.label, label_cx, g.rc->y + g.rc->h / 2 + 10 * s, 28 * s,
                    g.on ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
      if (wifi_half) {
        // Wi-Fi status icon button: full when connected, faint and struck through when not.
        // Always a button: clicking it opens the Wi-Fi window.
        bool connected = false;
        for (auto& r : wifi_rows_) connected = connected || r.connected;
        // At the toggle's right end, concentric with its rounded edge.
        double icx = g.rc->x + g.rc->w - sh / 2, icy = g.rc->y + sh / 2;
        btn_wifi_ = Rect{icx - d / 2, icy - d / 2, d, d};
        bool hot = dialog_ == Dialog::None && hover_ == Hit::WifiButton;
        cairo_arc(cr, icx, icy, d / 2, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, hot ? 0.32 : 0.16);
        cairo_fill(cr);
        draw_wifi_icon(cr, icx, icy, 13 * s, 3 * s, connected ? -1 : 0, connected ? 1.0 : 0.9);
        if (!connected) {
          cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
          cairo_set_line_width(cr, 3 * s);
          cairo_move_to(cr, icx - 10 * s, icy + 10 * s);
          cairo_line_to(cr, icx + 10 * s, icy - 10 * s);
          cairo_stroke(cr);
        }
      }
    }
    if (st.wifi) {
      cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
      centered_text(cr, "Wi-Fi is for testing: slower, and the iMac's own macOS drive is not shared", cx,
                    sy + sh + 46 * s, 24 * s, CAIRO_FONT_WEIGHT_NORMAL);
    }

    const double r = 56 * s, gap = 300 * s, by = h_ - 330 * s;
    struct B { Rect* rect; double x; const char* label; Hit hit; int icon; };
    B bs[2] = {{&btn_restart_, cx - gap / 2, "Restart", Hit::Restart, 0},
               {&btn_shutdown_, cx + gap / 2, "Shut Down", Hit::ShutDown, 1}};
    for (auto& b : bs) {
      bool active = dialog_ == Dialog::None;
      bool hot = active && hover_ == b.hit;
      cairo_set_source_rgba(cr, 1, 1, 1, hot ? (pressed_ == b.hit ? 0.40 : 0.28) : 0.14);
      cairo_arc(cr, b.x, by, r, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.95 : 0.5);
      if (b.icon == 0) draw_restart_icon(cr, b.x, by, r * 0.42, 6 * s);
      else if (b.icon == 1) draw_power_icon(cr, b.x, by, r * 0.42, 6 * s);
      else draw_wifi_icon(cr, b.x, by, r * 0.55, 6 * s, -1, active ? 0.95 : 0.5);
      cairo_set_source_rgba(cr, 1, 1, 1, hot ? 1.0 : 0.75);
      centered_text(cr, b.label, b.x, by + r + 50 * s, 28 * s, CAIRO_FONT_WEIGHT_NORMAL);
      *b.rect = Rect{b.x - r * 1.4, by - r, r * 2.8, r * 2 + 70 * s};  // circle and label
    }
  }

  // Confirmation dialog (or "Restarting…"), centred over everything.
  if (dialog_ != Dialog::None) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);  // dim the screen behind it
    cairo_paint(cr);
    const size_t nrows = std::min<size_t>(wifi_rows_.size(), 8);
    const double rh = 88 * s;
    const double dw = 1240 * s,
                 dh = dialog_ == Dialog::Busy           ? 220 * s
                      : dialog_ == Dialog::Wifi         ? 330 * s + 8 * rh  // fixed: rows coming in do not move it
                      : dialog_ == Dialog::WifiPassword ? 520 * s
                                                        : 400 * s;
    const double dx = cx - dw / 2, dy = std::min(h_ / 2.0 - dh / 2, h_ - 600 * s - dh);  // clear of the switch
    rounded_rect(cr, dx, dy, dw, dh, 28 * s);
    cairo_set_source_rgb(cr, 0.17, 0.17, 0.19);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_set_line_width(cr, 2 * s);
    cairo_stroke(cr);
    if (dialog_ == Dialog::Busy) {
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, busy_text_, cx, dy + dh / 2 + 18 * s, 48 * s, CAIRO_FONT_WEIGHT_BOLD);
    } else if (dialog_ == Dialog::Wifi) {
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, "Wi-Fi", cx, dy + 90 * s, 46 * s, CAIRO_FONT_WEIGHT_BOLD);
      std::string line = wifi_status_;
      if (line.empty()) {
        std::string cur;
        for (auto& r : wifi_rows_) if (r.connected) cur = r.name;
        line = !cur.empty() ? "Connected to \u201c" + cur + "\u201d" : wifi_scanning_ ? "Looking for networks\u2026" : "Not connected";
      }
      cairo_set_source_rgb(cr, kGrey[0], kGrey[1], kGrey[2]);
      centered_text(cr, line, cx, dy + 145 * s, 30 * s, CAIRO_FONT_WEIGHT_NORMAL);
      double y0 = dy + 190 * s;
      if (nrows == 0) {
        centered_text(cr, wifi_scanning_ ? "Looking for networks\u2026" : "No networks found", cx, y0 + rh / 2 + 10 * s,
                      32 * s, CAIRO_FONT_WEIGHT_NORMAL);
      }
      for (size_t i = 0; i < nrows; i++) {
        const WifiRow& w = wifi_rows_[i];
        Rect rc{dx + 40 * s, y0 + i * rh, dw - 80 * s, rh - 8 * s};
        row_rects_.push_back(rc);
        if (hover_row_ == int(i) && !w.connected) {
          rounded_rect(cr, rc.x, rc.y, rc.w, rc.h, 14 * s);
          cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
          cairo_fill(cr);
        }
        double ty2 = rc.y + rc.h / 2 + 12 * s;
        if (w.connected) {  // check mark
          cairo_set_source_rgb(cr, kBlue[0], kBlue[1], kBlue[2]);
          cairo_set_line_width(cr, 5 * s);
          cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
          cairo_move_to(cr, rc.x + 26 * s, ty2 - 12 * s);
          cairo_line_to(cr, rc.x + 38 * s, ty2);
          cairo_line_to(cr, rc.x + 60 * s, ty2 - 26 * s);
          cairo_stroke(cr);
        }
        cairo_set_source_rgb(cr, 1, 1, 1);
        left_text(cr, w.name, rc.x + 84 * s, ty2, 34 * s, w.connected ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        double rx = rc.x + rc.w - 70 * s;
        draw_wifi_icon(cr, rx, rc.y + rc.h / 2 - 6 * s, 34 * s, 4.5 * s, w.bars, 0.9);
        if (w.secured) {
          cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
          draw_lock(cr, rx - 80 * s, rc.y + rc.h / 2 - 16 * s, 1.1 * s);
        }
      }
      dlg_done_ = Rect{dx + dw - 340 * s, dy + dh - 116 * s, 300 * s, 76 * s};
      rounded_rect(cr, dlg_done_.x, dlg_done_.y, dlg_done_.w, dlg_done_.h, 16 * s);
      cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], hover_ == Hit::Done ? 1.0 : 0.85);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, "Done", dlg_done_.x + dlg_done_.w / 2, dlg_done_.y + dlg_done_.h / 2 + 12 * s, 32 * s,
                    CAIRO_FONT_WEIGHT_BOLD);
    } else if (dialog_ == Dialog::WifiPassword) {
      cairo_set_source_rgb(cr, 1, 1, 1);
      centered_text(cr, "Enter the password for \u201c" + wifi_pick_ + "\u201d", cx, dy + 100 * s, 40 * s,
                    CAIRO_FONT_WEIGHT_BOLD);
      Rect f{dx + 120 * s, dy + 160 * s, dw - 240 * s, 90 * s};
      rounded_rect(cr, f.x, f.y, f.w, f.h, 14 * s);
      cairo_set_source_rgb(cr, 0.09, 0.09, 0.10);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], 0.9);
      cairo_set_line_width(cr, 3 * s);
      cairo_stroke(cr);
      std::string shown;
      if (wifi_show_pass_) shown = wifi_pass_;
      else for (size_t i = 0; i < wifi_pass_.size(); i++) shown += "\u2022";
      cairo_set_source_rgb(cr, 1, 1, 1);
      left_text(cr, shown + "|", f.x + 30 * s, f.y + f.h / 2 + 13 * s, 36 * s, CAIRO_FONT_WEIGHT_NORMAL);
      if (!wifi_pass_error_.empty()) {
        cairo_set_source_rgb(cr, 1.0, 0.45, 0.42);
        centered_text(cr, wifi_pass_error_, cx, dy + 300 * s, 30 * s, CAIRO_FONT_WEIGHT_NORMAL);
      }
      // "Show password" check box (also useful to see what a non-US keyboard typed).
      show_pass_ = Rect{f.x, dy + 330 * s, 420 * s, 50 * s};
      rounded_rect(cr, f.x, dy + 338 * s, 34 * s, 34 * s, 7 * s);
      if (wifi_show_pass_) {
        cairo_set_source_rgb(cr, kBlue[0], kBlue[1], kBlue[2]);
        cairo_fill(cr);
      } else {
        cairo_set_source_rgba(cr, 1, 1, 1, hover_ == Hit::ShowPass ? 0.6 : 0.4);
        cairo_set_line_width(cr, 3 * s);
        cairo_stroke(cr);
      }
      cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
      left_text(cr, "Show password", f.x + 52 * s, dy + 366 * s, 30 * s, CAIRO_FONT_WEIGHT_NORMAL);
      const double bw = 300 * s, bh = 76 * s, byy = dy + dh - bh - 44 * s;
      dlg_cancel_ = Rect{cx - bw - 16 * s, byy, bw, bh};
      dlg_confirm_ = Rect{cx + 16 * s, byy, bw, bh};
      for (int k = 0; k < 2; k++) {
        Rect& b = k ? dlg_confirm_ : dlg_cancel_;
        bool hot = hover_ == (k ? Hit::Confirm : Hit::Cancel);
        rounded_rect(cr, b.x, b.y, b.w, b.h, 16 * s);
        if (k) cairo_set_source_rgba(cr, kBlue[0], kBlue[1], kBlue[2], wifi_pass_.size() >= 8 ? (hot ? 1.0 : 0.85) : 0.35);
        else cairo_set_source_rgba(cr, 1, 1, 1, hot ? 0.28 : 0.16);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        centered_text(cr, k ? "Join" : "Cancel", b.x + b.w / 2, b.y + b.h / 2 + 12 * s, 32 * s, CAIRO_FONT_WEIGHT_BOLD);
      }
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
  content_spans();
  force_frame_ = true;
  content_dirty_ = true;
}

void Display::content_spans() {
  const uint8_t* src = cairo_image_surface_get_data(base_);
  int stride = cairo_image_surface_get_stride(base_);
  for (int y = 0; y < h_; y++) {
    const uint32_t* row = (const uint32_t*)(src + size_t(y) * stride);
    int x0 = 0, x1 = w_;
    while (x0 < w_ && !(row[x0] >> 24)) x0++;
    while (x1 > x0 && !(row[x1 - 1] >> 24)) x1--;
    spans_[y] = {x0, x1};
  }
}

void Display::update_field(double t) {
  const double cx = w_ * 0.5, cy = h_ * 0.40, radius = h_ * 1.05;
  double sx[4], sy[4];
  for (int k = 0; k < 4; k++) {
    const Shadow& d = kShadows[k];
    sx[k] = h_ * (d.x + d.ax * std::sin(2 * M_PI * t / d.px + d.phase));
    sy[k] = h_ * (d.y + d.ay * std::sin(2 * M_PI * t / d.py + d.phase * 1.3));
  }
  for (int gy = 0; gy <= fh_; gy++) {
    double py = gy * kCell, dy = (py - cy) / radius;
    for (int gx = 0; gx <= fw_; gx++) {
      double px = gx * kCell, dx = (px - cx) / radius;
      double r = std::min(1.0, std::sqrt(dx * dx + dy * dy));
      r = r * r * (3 - 2 * r);  // smoothstep: soft centre, gentle fall-off
      double shade = 1.0;
      for (int k = 0; k < 4; k++) {
        double rr = kShadows[k].radius * h_;
        double d2 = ((px - sx[k]) * (px - sx[k]) + (py - sy[k]) * (py - sy[k])) / (rr * rr);
        shade *= 1.0 - kShadows[k].depth * std::exp(-d2);
      }
      size_t i = size_t(gy) * (fw_ + 2) + gx;
      for (int c = 0; c < 3; c++) field_[c][i] = int32_t((kGlow[c] + (kEdge[c] - kGlow[c]) * r) * shade * 256.0);
    }
  }
}

// Bilinear from the field (fixed point), plus the dither tile, then the content blended on top
// (premultiplied ARGB: out = content + background * (1 - alpha)). Each row is built in memory and
// written to the panel in one go (the scanout buffer is slow to read and best written in sequence).
void Display::compose_rows(uint8_t* dst, int pitch, int y0, int y1) {
  const uint8_t* src = cairo_image_surface_get_data(base_);
  const int stride = cairo_image_surface_get_stride(base_);
  std::vector<uint32_t> out(w_);
  std::vector<int32_t> col[3];
  for (auto& c : col) c.resize(fw_ + 2);
  const int fstride = fw_ + 2;
  for (int y = y0; y < y1; y++) {
    int gy = y / kCell, fy = y % kCell;
    for (int c = 0; c < 3; c++) {  // vertical interpolation once per field column
      const int32_t* a = &field_[c][size_t(gy) * fstride];
      const int32_t* b = a + fstride;
      for (int gx = 0; gx <= fw_; gx++) col[c][gx] = a[gx] * (kCell - fy) + b[gx] * fy;
    }
    const int16_t* nrow = &noise_[(y & 127) * 128];
    // Across each field cell the value changes linearly: start and step per channel, no multiplies
    // per pixel. Field values stay well inside 0..255 (8..50 plus a dither step), so no clamping.
    for (int gx = 0, x = 0; x < w_; gx++) {
      int32_t v0 = col[0][gx] * kCell, v1 = col[1][gx] * kCell, v2 = col[2][gx] * kCell;  // 8.8 * 256
      const int32_t d0 = col[0][gx + 1] - col[0][gx], d1 = col[1][gx + 1] - col[1][gx], d2 = col[2][gx + 1] - col[2][gx];
      const int xe = std::min(x + kCell, w_);
      for (; x < xe; x++) {
        const int n = nrow[x & 127] + 128;
        out[x] = uint32_t(((v0 >> 8) + n) >> 8) << 16 | uint32_t(((v1 >> 8) + n) >> 8) << 8 | uint32_t(((v2 >> 8) + n) >> 8);
        v0 += d0; v1 += d1; v2 += d2;
      }
    }
    auto [x0, x1] = spans_[y];
    const uint32_t* crow = (const uint32_t*)(src + size_t(y) * stride);
    for (int x = x0; x < x1; x++) {
      uint32_t c = crow[x], a = c >> 24;
      if (!a) continue;
      if (a == 255) { out[x] = c & 0xFFFFFF; continue; }
      uint32_t bgp = out[x], inv = 255 - a, r = 0;
      for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t v = ((c >> sh) & 0xFF) + (((bgp >> sh) & 0xFF) * inv + 127) / 255;
        r |= std::min<uint32_t>(v, 255) << sh;
      }
      out[x] = r;
    }
    // The dots go into the row before it is written, so they are never missing from the panel.
    if (dots_layer_ && y >= dots_y_ && y < dots_y_ + dots_h_) {
      int r = y - dots_y_;
      memcpy(&dots_bg_[size_t(r) * dots_w_], &out[dots_x_], size_t(dots_w_) * 4);
      blend_dots_row(r, &out[dots_x_]);
    }
    memcpy(dst + size_t(y) * pitch, out.data(), size_t(w_) * 4);
  }
}

void Display::blend_dots_row(int r, uint32_t* px) {
  if (dialog_ != Dialog::None) return;  // no dots under a dialog
  const uint8_t* src = cairo_image_surface_get_data(dots_layer_);
  const uint32_t* drow = (const uint32_t*)(src + size_t(r) * cairo_image_surface_get_stride(dots_layer_));
  for (int x = 0; x < dots_w_; x++) {
    uint32_t c = drow[x], a = c >> 24;
    if (!a) continue;
    uint32_t b = px[x], inv = 255 - a, o = 0;
    for (int sh = 0; sh <= 16; sh += 8)
      o |= std::min<uint32_t>(((c >> sh) & 0xFF) + (((b >> sh) & 0xFF) * inv + 127) / 255, 255) << sh;
    px[x] = o;
  }
}

void Display::draw_dots() {
  if (!dots_layer_) return;
  cairo_t* cr = cairo_create(dots_layer_);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  double t = (mono_us() - start_us_) / 1e6;
  for (int i = 0; i < 3; i++) {
    double phase = t * 2 * M_PI / 1.4 - i * 0.9;  // one wave every 1.4 s, left to right
    double a = 0.25 + 0.75 * (0.5 + 0.5 * std::sin(phase));
    cairo_set_source_rgba(cr, 1, 1, 1, a);
    cairo_arc(cr, dots_w_ / 2.0 + (i - 1) * dot_r_ * 4, dots_h_ / 2.0, dot_r_, 0, 2 * M_PI);
    cairo_fill(cr);
  }
  cairo_destroy(cr);
  cairo_surface_flush(dots_layer_);
}

void Display::compose(uint8_t* dst, int pitch) {
  const int n = 6;  // rows in parallel: ~25 ms of work at 3840x2160 spread over the CPU's threads
  std::vector<std::thread> th;
  for (int i = 0; i < n; i++)
    th.emplace_back([this, dst, pitch, i] { compose_rows(dst, pitch, h_ * i / n, h_ * (i + 1) / n); });
  for (auto& t : th) t.join();
}

void Display::frame() {
  if (fd_ < 0 || !has_status_ || paused_) return;
  update_field((mono_us() - start_us_) / 1e6);
  compose(map_, int(pitch_));
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
  std::lock_guard<std::mutex> l(drm_mu_);
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_) != 0)
    fprintf(stderr, "display: taking the screen back failed: %s\n", strerror(errno));
  gl_release_shown();
  paused_ = false;
  has_status_ = false;  // force a full redraw on the next show()
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
  std::lock_guard<std::mutex> l(drm_mu_);
  memset(map_, 0, size_);
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_) != 0)
    fprintf(stderr, "display: blank failed: %s\n", strerror(errno));
  gl_release_shown();
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
  animate();
}

void Display::animate() {
  if (fd_ < 0 || !has_status_ || paused_) return;
  frame_++;
  if (gl_) { gl_frame(); return; }
  draw_dots();
  // The whole screen (drifting background, content, dots) 3 times a second: the shadows move a
  // fraction of a grey level per step. In between only the dots' rectangle, from the background saved
  // at the last full frame: written in one pass, never a frame without the dots.
  if (force_frame_ || frame_ % 10 == 0) {
    force_frame_ = false;
    frame();
  } else if (dialog_ == Dialog::None) {
    std::vector<uint32_t> row(dots_w_);
    for (int r = 0; r < dots_h_; r++) {
      memcpy(row.data(), &dots_bg_[size_t(r) * dots_w_], size_t(dots_w_) * 4);
      blend_dots_row(r, row.data());
      memcpy(map_ + size_t(dots_y_ + r) * pitch_ + size_t(dots_x_) * 4, row.data(), size_t(dots_w_) * 4);
    }
  }
  drmModeDirtyFB(fd_, fb_id_, nullptr, 0);
}

bool Display::save_png(const std::string& path) const {
  if (!base_ || fd_ < 0) return false;
  cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
  cairo_surface_flush(img);
  const_cast<Display*>(this)->compose(cairo_image_surface_get_data(img), cairo_image_surface_get_stride(img));
  cairo_surface_mark_dirty(img);
  bool ok = cairo_surface_write_to_png(img, path.c_str()) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(img);
  return ok;
}

// --- pointer, buttons, dialog -----------------------------------------------------------------

void Display::redraw() {
  if (fd_ < 0 || paused_ || !has_status_) return;
  render_base(status_);
  animate();
  if (!snap_dir_.empty()) {
    char name[32];
    snprintf(name, sizeof name, "/%03u.png", snap_n_++);
    // PNG compression of a 4K picture takes about a second: on a copy, in the background, so the
    // pointer keeps up while testing.
    if (gl_) { gl_snap_path_ = snap_dir_ + name; return; }  // what the GPU draws, read back (gl_frame)
    cairo_surface_t* copy = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    cairo_surface_flush(copy);
    compose(cairo_image_surface_get_data(copy), cairo_image_surface_get_stride(copy));
    cairo_surface_mark_dirty(copy);
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
  if (dialog_ == Dialog::Restart || dialog_ == Dialog::ShutDown || dialog_ == Dialog::WifiPassword) {
    if (dlg_cancel_.in(px_, py_)) return Hit::Cancel;
    if (dlg_confirm_.in(px_, py_)) return Hit::Confirm;
    if (dialog_ == Dialog::WifiPassword && show_pass_.in(px_, py_)) return Hit::ShowPass;
    return Hit::None;
  }
  if (dialog_ == Dialog::Wifi) {
    if (dlg_done_.in(px_, py_)) return Hit::Done;
    for (auto& r : row_rects_)
      if (r.in(px_, py_)) return Hit::Row;
    return Hit::None;
  }
  if (dialog_ == Dialog::None) {
    if (btn_restart_.in(px_, py_)) return Hit::Restart;
    if (btn_shutdown_.in(px_, py_)) return Hit::ShutDown;
    if (btn_wifi_.in(px_, py_)) return Hit::WifiButton;  // inside the Wi-Fi half: checked first
    if (seg_tb_.in(px_, py_)) return Hit::LinkTb;
    if (seg_wifi_.in(px_, py_)) return Hit::LinkWifi;
  }
  return Hit::None;
}

void Display::update_hover() {
  Hit h = hit_test();
  int row = -1;
  if (h == Hit::Row)
    for (size_t i = 0; i < row_rects_.size(); i++)
      if (row_rects_[i].in(px_, py_)) row = int(i);
  if (h != hover_ || row != hover_row_) {
    hover_ = h;
    hover_row_ = row;
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
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) shift_down_ = value;
  if (code == KEY_CAPSLOCK && value) caps_ = !caps_;
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

  // Wi-Fi panel and its password box.
  if (dialog_ == Dialog::Wifi || dialog_ == Dialog::WifiPassword) {
    bool pw = dialog_ == Dialog::WifiPassword;
    auto back_to_list = [&]() {
      dialog_ = Dialog::Wifi;
      wifi_pass_.clear();
      wifi_pass_error_.clear();
      hover_ = Hit::None;
      redraw();
      update_hover();
    };
    auto close_panel = [&]() {
      dialog_ = Dialog::None;
      wifi_pass_.clear();
      hover_ = Hit::None;
      redraw();
      update_hover();
    };
    auto join_with_password = [&]() {
      if (wifi_pass_.size() < 8) return Action::None;  // WPA passwords are 8 to 63 characters
      dialog_ = Dialog::Wifi;
      wifi_pass_error_.clear();
      hover_ = Hit::None;
      redraw();
      return Action::WifiPassword;
    };
    if (code == BTN_LEFT) {
      if (value) { pressed_ = hover_; return Action::None; }
      Hit was = pressed_;
      pressed_ = Hit::None;
      if (was == Hit::None || was != hit_test()) return Action::None;
      if (!pw && was == Hit::Done) close_panel();
      else if (!pw && was == Hit::Row && hover_row_ >= 0 && hover_row_ < int(wifi_rows_.size())) {
        const WifiRow& r = wifi_rows_[hover_row_];
        if (r.connected) return Action::None;
        wifi_pick_ = r.name;
        return Action::WifiJoin;
      } else if (pw && was == Hit::Cancel) back_to_list();
      else if (pw && was == Hit::Confirm) return join_with_password();
      else if (pw && was == Hit::ShowPass) { wifi_show_pass_ = !wifi_show_pass_; redraw(); }
      return Action::None;
    }
    if (!value) return Action::None;
    if (code == KEY_ESC) { if (pw) back_to_list(); else close_panel(); return Action::None; }
    if (!pw) return Action::None;
    if (code == KEY_ENTER || code == KEY_KPENTER) return join_with_password();
    if (code == KEY_BACKSPACE) { if (!wifi_pass_.empty()) { wifi_pass_.pop_back(); redraw(); } return Action::None; }
    if (char c = key_char(code, shift_down_, caps_); c && wifi_pass_.size() < 63) { wifi_pass_ += c; redraw(); }
    return Action::None;
  }

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
      case Hit::WifiButton:
        dialog_ = Dialog::Wifi;
        wifi_status_.clear();
        hover_ = Hit::None;
        redraw();
        update_hover();
        return Action::WifiOpen;
      case Hit::LinkTb: if (status_.wifi) return Action::UseThunderbolt; break;
      case Hit::LinkWifi: if (!status_.wifi) return Action::UseWifi; break;
      case Hit::None: case Hit::Row: case Hit::Done: case Hit::ShowPass: break;
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

void Display::set_wifi(std::vector<WifiRow> rows, bool scanning, const std::string& status) {
  auto current = [](const std::vector<WifiRow>& v) {
    for (auto& r : v) if (r.connected) return r.name;
    return std::string();
  };
  bool label_changed = current(rows) != current(wifi_rows_);
  wifi_rows_ = std::move(rows);
  if (label_changed && dialog_ == Dialog::None) redraw();  // the network name above the switch
  wifi_scanning_ = scanning;
  wifi_status_ = status;
  if (dialog_ == Dialog::Wifi) {
    redraw();
    update_hover();
  }
}

void Display::ask_wifi_password(const std::string& name, const std::string& error) {
  if (!wifi_open()) return;
  fprintf(stderr, "display: asking for the Wi-Fi password of \"%s\"%s\n", name.c_str(), error.empty() ? "" : " again");
  wifi_pick_ = name;
  wifi_pass_.clear();
  wifi_pass_error_ = error;
  dialog_ = Dialog::WifiPassword;
  hover_ = Hit::None;
  redraw();
  update_hover();
}

std::string Display::take_wifi_password() {
  std::string p;
  p.swap(wifi_pass_);
  return p;
}

// --- the waiting screen on the GPU ------------------------------------------------------------

namespace {

const char* kBgVertex = R"(#version 300 es
void main() {  // one triangle covering the screen
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

// Same picture as the CPU path: radial glow, four drifting Gaussian shadows, a static triangular
// dither of one grey step, the content texture (premultiplied, cairo's byte order) over it, the dots.
const char* kBgFragment = R"(#version 300 es
precision highp float;
uniform vec2 size;
uniform vec4 sh[4];      // centre x, y, radius, depth (pixels from the top left)
uniform vec4 dots;       // centre x, y, spacing, radius
uniform vec3 dot_a;
uniform int show_dots;
uniform sampler2D content;
out vec4 color;
float hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}
void main() {
  vec2 px = vec2(gl_FragCoord.x, size.y - gl_FragCoord.y);
  vec2 c = vec2(size.x * 0.5, size.y * 0.40);
  float r = min(1.0, length(px - c) / (size.y * 1.05));
  r = r * r * (3.0 - 2.0 * r);
  vec3 col = mix(vec3(44.0, 45.0, 50.0), vec3(8.0, 8.0, 10.0), r);
  float shade = 1.0;
  for (int i = 0; i < 4; i++) {
    vec2 d = px - sh[i].xy;
    shade *= 1.0 - sh[i].w * exp(-dot(d, d) / (sh[i].z * sh[i].z));
  }
  col *= shade;
  float n = hash(floor(px)) + hash(floor(px) + 17.31) - 1.0;
  col = floor(col + n + 0.5) / 255.0;
  vec4 ct = texelFetch(content, ivec2(px), 0).bgra;
  col = ct.rgb + col * (1.0 - ct.a);
  if (show_dots == 1) {
    for (int i = 0; i < 3; i++) {
      vec2 dc = dots.xy + vec2(float(i - 1) * dots.z, 0.0);
      float a = dot_a[i] * clamp(dots.w + 0.5 - length(px + 0.5 - dc), 0.0, 1.0);
      col = mix(col, vec3(1.0), a);
    }
  }
  color = vec4(col, 1.0);
}
)";

struct GlFb { int fd; uint32_t fb; };
void destroy_gl_fb(gbm_bo*, void* data) {
  auto* c = (GlFb*)data;
  if (c->fb) drmModeRmFB(c->fd, c->fb);
  delete c;
}
uint32_t gl_fb_for(int fd, gbm_bo* bo) {
  if (auto* c = (GlFb*)gbm_bo_get_user_data(bo)) return c->fb;
  uint32_t handles[4] = {gbm_bo_get_handle(bo).u32}, strides[4] = {gbm_bo_get_stride(bo)}, offsets[4] = {0};
  auto* c = new GlFb{fd, 0};
  if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, strides, offsets,
                    &c->fb, 0) != 0)
    fprintf(stderr, "display: drmModeAddFB2 failed: %s\n", strerror(errno));
  gbm_bo_set_user_data(bo, c, destroy_gl_fb);
  return c->fb;
}

GLuint gl_compile(GLenum type, const char* src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(sh, sizeof log, nullptr, log);
    fprintf(stderr, "display: shader: %s\n", log);
  }
  return sh;
}

}  // namespace

bool Display::gl_init() {
  auto* gbm = gbm_create_device(fd_);
  if (!gbm) return false;
  gbm_ = gbm;
  auto* gs = gbm_surface_create(gbm, w_, h_, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gs) { gl_close(); return false; }
  gs_ = gs;
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) { gl_close(); return false; }
  edpy_ = dpy;
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint cattr[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
  EGLConfig configs[64];
  EGLint n = 0;
  eglChooseConfig(dpy, cattr, configs, 64, &n);
  EGLConfig conf = nullptr;
  for (int i = 0; i < n; i++) {
    EGLint id;
    eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == GBM_FORMAT_XRGB8888) { conf = configs[i]; break; }
  }
  EGLint ctxattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLContext ctx = conf ? eglCreateContext(dpy, conf, EGL_NO_CONTEXT, ctxattr) : EGL_NO_CONTEXT;
  if (ctx == EGL_NO_CONTEXT) { gl_close(); return false; }
  ectx_ = ctx;
  EGLSurface surf = eglCreateWindowSurface(dpy, conf, (EGLNativeWindowType)gs, nullptr);
  if (surf == EGL_NO_SURFACE) { gl_close(); return false; }
  esurf_ = surf;
  if (!eglMakeCurrent(dpy, surf, surf, ctx)) { gl_close(); return false; }
  eglSwapInterval(dpy, 0);

  prog_ = glCreateProgram();
  glAttachShader(prog_, gl_compile(GL_VERTEX_SHADER, kBgVertex));
  glAttachShader(prog_, gl_compile(GL_FRAGMENT_SHADER, kBgFragment));
  glLinkProgram(prog_);
  GLint ok = 0;
  glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
  if (!ok) { gl_close(); return false; }
  glUseProgram(prog_);
  u_size_ = glGetUniformLocation(prog_, "size");
  u_sh_ = glGetUniformLocation(prog_, "sh");
  u_dots_ = glGetUniformLocation(prog_, "dots");
  u_dot_a_ = glGetUniformLocation(prog_, "dot_a");
  u_show_dots_ = glGetUniformLocation(prog_, "show_dots");
  glUniform1i(glGetUniformLocation(prog_, "content"), 0);
  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);
  glGenTextures(1, &tex_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w_, h_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  content_dirty_ = true;
  return glGetError() == GL_NO_ERROR;
}

void Display::gl_close() {
  if (!gbm_) return;
  {
    std::lock_guard<std::mutex> l(drm_mu_);
    for (int i = 0; flip_pending_ && i < 5; i++) {  // our last flip, if any
      pollfd pf{fd_, POLLIN, 0};
      if (poll(&pf, 1, 100) > 0) {
        drmEventContext ev{};
        ev.version = 2;
        ev.page_flip_handler = on_flip;
        drmHandleEvent(fd_, &ev);
      }
    }
  }
  if (edpy_) {
    if (prog_) glDeleteProgram(prog_);
    if (tex_) glDeleteTextures(1, &tex_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    eglMakeCurrent((EGLDisplay)edpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (esurf_) eglDestroySurface((EGLDisplay)edpy_, (EGLSurface)esurf_);
    if (ectx_) eglDestroyContext((EGLDisplay)edpy_, (EGLContext)ectx_);
    eglTerminate((EGLDisplay)edpy_);
  }
  prog_ = tex_ = vao_ = 0;
  edpy_ = ectx_ = esurf_ = nullptr;
  // The dumb buffer goes back on the panel before our buffers are freed (freeing a scanned-out
  // framebuffer turns the panel off).
  if (shown_bo_ && fb_id_) drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_);
  if (gs_ && shown_bo_) gbm_surface_release_buffer((gbm_surface*)gs_, (gbm_bo*)shown_bo_);
  if (gs_ && pending_bo_) gbm_surface_release_buffer((gbm_surface*)gs_, (gbm_bo*)pending_bo_);
  shown_bo_ = pending_bo_ = nullptr;
  flip_pending_ = false;
  if (gs_) gbm_surface_destroy((gbm_surface*)gs_);
  gs_ = nullptr;
  gbm_device_destroy((gbm_device*)gbm_);
  gbm_ = nullptr;
  gl_ = false;
}

void Display::gl_release_shown() {
  if (!gl_ || !gs_) return;
  if (shown_bo_) gbm_surface_release_buffer((gbm_surface*)gs_, (gbm_bo*)shown_bo_);
  shown_bo_ = nullptr;
}

void Display::on_flip(int, unsigned, unsigned, unsigned, void* self_) {
  auto* self = (Display*)self_;
  if (self->shown_bo_ && self->gs_) gbm_surface_release_buffer((gbm_surface*)self->gs_, (gbm_bo*)self->shown_bo_);
  self->shown_bo_ = self->pending_bo_;
  self->pending_bo_ = nullptr;
  self->flip_pending_ = false;
}

void Display::on_drm_event() {
  std::lock_guard<std::mutex> l(drm_mu_);
  if (!flip_pending_) return;  // not ours: the video presenter reads its own events
  pollfd pf{fd_, POLLIN, 0};
  if (poll(&pf, 1, 0) <= 0) return;
  drmEventContext ev{};
  ev.version = 2;
  ev.page_flip_handler = on_flip;
  drmHandleEvent(fd_, &ev);
}

void Display::hand_over() {
  std::lock_guard<std::mutex> l(drm_mu_);
  paused_ = true;
  for (int i = 0; flip_pending_ && i < 5; i++) {
    pollfd pf{fd_, POLLIN, 0};
    if (poll(&pf, 1, 100) > 0) {
      drmEventContext ev{};
      ev.version = 2;
      ev.page_flip_handler = on_flip;
      drmHandleEvent(fd_, &ev);
    }
  }
}

void Display::gl_frame() {
  std::lock_guard<std::mutex> l(drm_mu_);
  if (paused_ || flip_pending_ || !has_status_) return;  // one flip at a time; the next tick draws again
  if (content_dirty_) {
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride(base_) / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, cairo_image_surface_get_data(base_));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    content_dirty_ = false;
  }
  double t = (mono_us() - start_us_) / 1e6;
  float sh[16];
  for (int k = 0; k < 4; k++) {
    const Shadow& d = kShadows[k];
    sh[k * 4 + 0] = float(h_ * (d.x + d.ax * std::sin(2 * M_PI * t / d.px + d.phase)));
    sh[k * 4 + 1] = float(h_ * (d.y + d.ay * std::sin(2 * M_PI * t / d.py + d.phase * 1.3)));
    sh[k * 4 + 2] = float(d.radius * h_);
    sh[k * 4 + 3] = float(d.depth);
  }
  float a[3];
  for (int i = 0; i < 3; i++) a[i] = float(0.25 + 0.75 * (0.5 + 0.5 * std::sin(t * 2 * M_PI / 1.4 - i * 0.9)));
  glViewport(0, 0, w_, h_);
  glUniform2f(u_size_, float(w_), float(h_));
  glUniform4fv(u_sh_, 4, sh);
  glUniform4f(u_dots_, dots_x_ + dots_w_ / 2.0f, dots_y_ + dots_h_ / 2.0f, dot_r_ * 4.0f, float(dot_r_));
  glUniform3fv(u_dot_a_, 1, a);
  glUniform1i(u_show_dots_, dialog_ == Dialog::None ? 1 : 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glFinish();  // done before the flip is asked for (see video.cpp: otherwise it lands a refresh late)
  if (!gl_snap_path_.empty()) {  // testing: this frame as a PNG (bottom-up rows, RGBA)
    std::vector<uint8_t> px(size_t(w_) * h_ * 4);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    cairo_surface_t* img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
    uint8_t* d = cairo_image_surface_get_data(img);
    int ds = cairo_image_surface_get_stride(img);
    for (int y = 0; y < h_; y++)
      for (int x = 0; x < w_; x++) {
        const uint8_t* p = &px[(size_t(h_ - 1 - y) * w_ + x) * 4];
        ((uint32_t*)(d + size_t(y) * ds))[x] = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
      }
    cairo_surface_mark_dirty(img);
    std::thread([img, path = gl_snap_path_] {
      cairo_surface_write_to_png(img, path.c_str());
      cairo_surface_destroy(img);
    }).detach();
    gl_snap_path_.clear();
  }
  eglSwapBuffers((EGLDisplay)edpy_, (EGLSurface)esurf_);
  gbm_bo* bo = gbm_surface_lock_front_buffer((gbm_surface*)gs_);
  if (!bo) return;
  uint32_t fb = gl_fb_for(fd_, bo);
  if (drmModePageFlip(fd_, crtc_id_, fb, DRM_MODE_PAGE_FLIP_EVENT, this) == 0) {
    pending_bo_ = bo;
    flip_pending_ = true;
  } else {
    // No flip possible (e.g. the CRTC was just set by someone else): set it directly.
    if (drmModeSetCrtc(fd_, crtc_id_, fb, 0, 0, &conn_id_, 1, (drmModeModeInfo*)mode_) == 0) {
      gl_release_shown();
      shown_bo_ = bo;
    } else {
      gbm_surface_release_buffer((gbm_surface*)gs_, bo);
    }
  }
}
