#include "display.h"

#include <cairo.h>
#include <dirent.h>
#include <fcntl.h>
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
#include <string>

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
  if (drmModeSetCrtc(fd_, crtc_id_, fb_id_, 0, 0, &conn_id_, 1, &mode) != 0) {
    fprintf(stderr, "display: setting mode %dx%d failed: %s\n", w_, h_, strerror(errno));
    close();
    return false;
  }
  // If we run on a VT (systemd TTYPath), switch it to graphics mode: the text console stops drawing.
  if (isatty(0) && ioctl(0, KDSETMODE, KD_GRAPHICS) == 0) tty_ = 0;
  base_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w_, h_);
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
  fprintf(stderr, "display: screen given back to the console\n");
}

void Display::render_base(const Status& st) {
  paint_background(base_, w_, h_);
  cairo_t* cr = cairo_create(base_);
  double s = h_ / 2160.0, cx = w_ / 2.0;

  double icon_cy = h_ * 0.40;
  draw_laptop(cr, cx, icon_cy, s, st.state == State::Connected);

  std::string title, sub;
  switch (st.state) {
    case State::NoCable:
      title = "Waiting for your Mac";
      sub = "Connect the Thunderbolt cable to your MacBook";
      break;
    case State::WaitingForMac:
      title = "Waiting for your Mac";
      sub = "Cable connected. Start iMac Display on your MacBook.";
      break;
    case State::Connected:
      title = st.mac_name.empty() ? "Connected" : "Connected to " + st.mac_name;
      sub = "Waiting for the first picture";
      break;
  }
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
  cairo_destroy(cr);
  cairo_surface_flush(base_);
}

void Display::copy_rect(int x, int y, int w, int h) {
  const uint8_t* src = cairo_image_surface_get_data(base_);
  int stride = cairo_image_surface_get_stride(base_);
  for (int r = y; r < y + h && r < h_; r++) memcpy(map_ + r * pitch_ + x * 4, src + r * stride + x * 4, w * 4);
}

void Display::show(const Status& s) {
  if (fd_ < 0) return;
  if (has_status_ && s.state == status_.state && s.mac_name == status_.mac_name && s.footer == status_.footer) return;
  status_ = s;
  has_status_ = true;
  render_base(s);
  copy_rect(0, 0, w_, h_);
  drmModeDirtyFB(fd_, fb_id_, nullptr, 0);
  animate();
}

void Display::animate() {
  if (fd_ < 0 || !has_status_) return;
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
