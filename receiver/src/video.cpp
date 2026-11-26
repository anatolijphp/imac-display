#include "video.h"

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <poll.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "net.h"  // now_us()

namespace {

// A decoder that is this far behind (age of the oldest waiting packet) cannot catch up: its backlog is
// dropped and a keyframe requested. Generous on purpose: a keyframe is large and takes several frame
// times to decode, and the small frames after it are caught up quickly.
constexpr uint64_t kMaxBacklogUs = 500000;
constexpr uint64_t kRepeatKeyRequestUs = 1000000;  // while a decoder waits for a keyframe, ask again this often
constexpr uint64_t kForgetUs = 1000000;            // incomplete pictures older than this are given up
// Back in step with the panel: when pictures keep finishing well before the refresh that shows the
// previous one, one picture is skipped (see present_loop).
constexpr int kBehindToSkip = 30;              // pictures in a row that waited for the previous flip
constexpr uint64_t kBehindMarginUs = 1500;     // ...by longer than the GPU work plus this
constexpr uint64_t kSkipBackoffUs = 2000000;   // first pause between skips; doubles when a skip did not help
constexpr uint64_t kSkipBackoffMaxUs = 60000000;
constexpr int kSlots = 5;        // pictures in flight: shown/flipping, ready, up to 3 being decoded

double ms_since(uint64_t t0) { return (now_us() - t0) / 1000.0; }

const char* kVertex = R"(#version 300 es
in vec2 pos;
in vec2 uv;
out vec2 v_uv;
void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";

// YUV (8-bit planar) -> RGB. m and off are chosen per tile for BT.601/709 and limited/full range.
const char* kFragment = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D ty, tu, tv;
uniform mat3 m;
uniform vec3 off;
out vec4 color;
void main() {
  vec3 yuv = vec3(texture(ty, v_uv).r, texture(tu, v_uv).r, texture(tv, v_uv).r) - off;
  color = vec4(clamp(m * yuv, 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    fprintf(stderr, "video: shader: %s\n", log);
  }
  return s;
}

// Column-major mat3 for GLSL: columns are the contributions of Y, U, V.
void yuv_matrix(int colorspace, int range, int format, int height, float m[9], float off[3]) {
  bool full = range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P;
  bool bt601 = colorspace == AVCOL_SPC_BT470BG || colorspace == AVCOL_SPC_SMPTE170M ||
               (colorspace == AVCOL_SPC_UNSPECIFIED && height < 720);
  // R = Y + rv V;  G = Y + gu U + gv V;  B = Y + bu U
  float rv = bt601 ? 1.402f : 1.5748f, gu = bt601 ? -0.344136f : -0.187324f,
        gv = bt601 ? -0.714136f : -0.468124f, bu = bt601 ? 1.772f : 1.8556f;
  float ys = full ? 1.0f : 255.0f / 219.0f, cs = full ? 1.0f : 255.0f / 224.0f;
  off[0] = full ? 0.0f : 16.0f / 255.0f;
  off[1] = off[2] = 128.0f / 255.0f;
  float mm[9] = {ys, ys, ys, 0, gu * cs, bu * cs, rv * cs, gv * cs, 0};
  memcpy(m, mm, sizeof mm);
}

struct FbCache { int fd; uint32_t fb; };
void destroy_fb(gbm_bo*, void* data) {
  auto* c = (FbCache*)data;
  if (c->fb) drmModeRmFB(c->fd, c->fb);
  delete c;
}
uint32_t fb_for(int fd, gbm_bo* bo) {
  if (auto* c = (FbCache*)gbm_bo_get_user_data(bo)) return c->fb;
  uint32_t handles[4] = {gbm_bo_get_handle(bo).u32}, strides[4] = {gbm_bo_get_stride(bo)}, offsets[4] = {0};
  auto* c = new FbCache{fd, 0};
  if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, strides,
                    offsets, &c->fb, 0) != 0)
    fprintf(stderr, "video: drmModeAddFB2 failed: %s\n", strerror(errno));
  gbm_bo_set_user_data(bo, c, destroy_fb);
  return c->fb;
}

// Copies one plane row by row into a tightly packed destination.
void copy_plane(uint8_t* dst, const uint8_t* src, int src_stride, int w, int h) {
  if (src_stride == w) { memcpy(dst, src, size_t(w) * h); return; }
  for (int y = 0; y < h; y++) memcpy(dst + size_t(y) * w, src + size_t(y) * src_stride, w);
}

}  // namespace

struct Video::Tile {
  int index = 0;
  VideoConfig::Tile cfg{};
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Packet> queue;
  bool waiting_for_key = true;  // decoding starts at a keyframe
  std::thread thread;
};

size_t Video::tile_bytes(int i) const {
  size_t w = cfg_.tiles[i].w, h = cfg_.tiles[i].h;
  return w * h + 2 * ((w + 1) / 2) * ((h + 1) / 2);
}

Video::Video(ScreenTarget screen, VideoConfig cfg, std::function<void()> request_keyframe,
             std::function<void()> release_screen,
             std::function<void()> take_screen)
    : screen_(screen), cfg_(std::move(cfg)), request_keyframe_(std::move(request_keyframe)),
      release_screen_(std::move(release_screen)),
      take_screen_(std::move(take_screen)) {
  for (size_t i = 0; i < cfg_.tiles.size(); i++) {
    auto t = std::make_unique<Tile>();
    t->index = int(i);
    t->cfg = cfg_.tiles[i];
    tile_index_[uint16_t(t->cfg.id)] = int(i);
    tiles_.push_back(std::move(t));
  }
  for (auto& t : tiles_) t->thread = std::thread(&Video::decode_loop, this, t.get());
  presenter_ = std::thread(&Video::present_loop, this);
  fprintf(stderr, "video: started, %dx%d in %zu tile(s)\n", cfg_.w, cfg_.h, tiles_.size());
}

Video::~Video() {
  stop_ = true;
  for (auto& t : tiles_) {
    { std::lock_guard<std::mutex> l(t->mu); }
    t->cv.notify_all();
  }
  { std::lock_guard<std::mutex> l(pic_mu_); }
  pic_cv_.notify_all();
  for (auto& t : tiles_) t->thread.join();
  presenter_.join();
  fprintf(stderr, "video: stopped\n");
}

int Video::take_slot() {
  for (size_t i = 0; i < slots_.size(); i++)
    if (slots_[i].free) { slots_[i].free = false; return int(i); }
  // None free: the oldest incomplete picture will most likely never complete (a tile still waits for its
  // keyframe, or lost a frame). Take its slot; newer pictures are worth more.
  for (auto it = assembling_.begin(); it != assembling_.end(); ++it) {
    Picture& o = it->second;
    if (o.slot < 0 || o.writers > 0) continue;
    int s = o.slot;
    o.slot = -1;
    o.abandoned = true;
    std::lock_guard<std::mutex> l(stats_mu_);
    stats_.dropped++;
    return s;  // stays taken, now for the new picture
  }
  return -1;
}

void Video::release(Picture& p) {
  if (p.slot >= 0) slots_[p.slot].free = true;
  p.slot = -1;
}

void Video::push(uint16_t tile, uint8_t flags, uint64_t pts_us, const uint8_t* data, size_t len) {
  auto it = tile_index_.find(tile);
  if (it == tile_index_.end()) return;
  Tile* t = tiles_[it->second].get();
  uint64_t now = now_us();
  {
    // Which tiles make up this picture: tiles left out keep their last picture (PROTOCOL.md, Video).
    std::lock_guard<std::mutex> l(pic_mu_);
    forget_old(now);
    Picture& p = assembling_[pts_us];
    if (p.meta.empty()) { p.meta.resize(tiles_.size()); p.pts = pts_us; p.first_seen_us = now; }
    p.sent |= 1u << it->second;
    if (flags & 2) p.last_seen = true;
  }
  bool overflow = false, ask = false;
  {
    std::lock_guard<std::mutex> l(t->mu);
    if (flags & 1) {
      t->queue.clear();  // a keyframe makes everything queued before it pointless
      t->waiting_for_key = false;  // packets after it are decodable (set here, not when the decoder gets to it)
    } else if (!t->queue.empty() && now - t->queue.front().recv_us > kMaxBacklogUs) {
      overflow = true;  // hopelessly behind: drop the backlog, restart at the next keyframe
      t->queue.clear();
      t->waiting_for_key = true;
    }
    if (t->waiting_for_key && !(flags & 1)) {
      // This packet cannot be decoded without its keyframe. Keep asking until one comes.
      ask = now - last_key_request_us_ > kRepeatKeyRequestUs;
    } else {
      t->queue.push_back(Packet{std::vector<uint8_t>(data, data + len), flags, pts_us, now});
    }
  }
  t->cv.notify_one();
  if (overflow) {
    std::lock_guard<std::mutex> l(stats_mu_);
    stats_.dropped++;
    fprintf(stderr, "video: tile %d more than %llu ms behind, dropping its backlog\n", t->cfg.id,
            (unsigned long long)(kMaxBacklogUs / 1000));
  }
  if (overflow || ask) {
    last_key_request_us_ = now;
    request_keyframe_();
  }
}

void Video::forget_old(uint64_t now) {
  for (auto it = assembling_.begin(); it != assembling_.end();) {
    Picture& p = it->second;
    bool old = now - p.first_seen_us > kForgetUs;
    if ((p.abandoned || old) && p.writers == 0) {
      release(p);
      it = assembling_.erase(it);
    } else {
      ++it;
    }
  }
}

void Video::decode_loop(Tile* t) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  // Low latency: no frame threading (each extra frame thread delays output by one frame). Parallelism
  // comes from tiles. A single big tile gets frame threads, trading ~2 frames of delay for speed,
  // because one core decodes 4K High profile at only ~48 fps on this iMac.
  long pixels = long(t->cfg.w) * t->cfg.h;
  if (cfg_.tiles.size() == 1 && pixels > 2560L * 1440) {
    ctx->thread_count = 3;
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  } else {
    ctx->thread_count = 2;
    ctx->thread_type = FF_THREAD_SLICE;  // helps only if the encoder emits several slices
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
  }
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    fprintf(stderr, "video: cannot open the H.264 decoder\n");
    avcodec_free_context(&ctx);
    return;
  }
  fprintf(stderr, "video: tile %d %dx%d: %d thread(s), %s\n", t->cfg.id, t->cfg.w, t->cfg.h, ctx->thread_count,
          ctx->thread_type & FF_THREAD_FRAME ? "frame threading (+2 frames latency)" : "low delay");
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::map<int64_t, uint64_t> recv_by_pts;  // frame threading returns frames later: remember arrival times

  while (!stop_) {
    Packet p;
    {
      std::unique_lock<std::mutex> l(t->mu);
      t->cv.wait(l, [&] { return stop_ || !t->queue.empty(); });
      if (stop_) break;
      p = std::move(t->queue.front());
      t->queue.pop_front();
      if (t->waiting_for_key) {
        if (!(p.flags & 1)) continue;
        t->waiting_for_key = false;
      }
    }
    uint64_t t0 = now_us();
    av_new_packet(pkt, int(p.data.size()));
    memcpy(pkt->data, p.data.data(), p.data.size());
    pkt->pts = int64_t(p.pts);
    if (p.flags & 1) pkt->flags |= AV_PKT_FLAG_KEY;
    recv_by_pts[pkt->pts] = p.recv_us;
    int rc = avcodec_send_packet(ctx, pkt);
    av_packet_unref(pkt);
    bool error = rc < 0 && rc != AVERROR(EAGAIN);
    while (!error) {
      rc = avcodec_receive_frame(ctx, frame);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) { error = true; break; }
      if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) error = true;
      uint64_t recv = recv_by_pts.count(frame->pts) ? recv_by_pts[frame->pts] : p.recv_us;
      recv_by_pts.erase(recv_by_pts.begin(), recv_by_pts.upper_bound(frame->pts));
      on_decoded(t->index, uint64_t(frame->pts), recv, frame, (now_us() - t0) / 1000.0);
      av_frame_unref(frame);
    }
    if (error) {
      fprintf(stderr, "video: tile %d: decode error, waiting for a keyframe\n", t->cfg.id);
      avcodec_flush_buffers(ctx);
      recv_by_pts.clear();
      {
        std::lock_guard<std::mutex> l(t->mu);
        t->waiting_for_key = true;
      }
      last_key_request_us_ = now_us();
      request_keyframe_();
    }
  }
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
}

void Video::on_decoded(int index, uint64_t pts, uint64_t recv_us, const AVFrame* f, double ms) {
  {
    std::lock_guard<std::mutex> l(stats_mu_);
    if (index == 0) stats_.decoded++;
    decode_sum_ += ms;
    decode_n_++;
  }
  const auto& tc = cfg_.tiles[index];
  if ((f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) || f->width != tc.w ||
      f->height != tc.h) {
    fprintf(stderr, "video: tile %d: got %dx%d format %d, expected %dx%d yuv420p; skipped\n", tc.id, f->width,
            f->height, f->format, tc.w, tc.h);
    return;
  }

  // Find this picture's slot (taking a free one when its first tile arrives), then copy outside the lock.
  // Never wait here: a decoder that waits loses packets, and lost packets cost a keyframe. If the picture
  // cannot be kept (GPU not set up yet, no slot), only this picture is lost; the decoder stays in step.
  uint8_t* dst = nullptr;
  {
    std::lock_guard<std::mutex> l(pic_mu_);
    if (stop_ || !gpu_ready_ || gpu_failed_) return;
    auto it = assembling_.find(pts);
    if (it == assembling_.end()) return;  // given up already
    Picture& p = it->second;
    if (p.abandoned) return;
    if (p.slot < 0) p.slot = take_slot();
    if (p.slot < 0) return;
    p.writers++;
    dst = slots_[p.slot].ptr[index];
  }
  uint64_t c0 = now_us();
  int w = f->width, h = f->height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  copy_plane(dst, f->data[0], f->linesize[0], w, h);
  copy_plane(dst + size_t(w) * h, f->data[1], f->linesize[1], cw, ch);
  copy_plane(dst + size_t(w) * h + size_t(cw) * ch, f->data[2], f->linesize[2], cw, ch);
  double copy_ms = ms_since(c0);

  std::lock_guard<std::mutex> l(pic_mu_);
  {
    std::lock_guard<std::mutex> s(stats_mu_);
    copy_sum_ += copy_ms;
  }
  auto it = assembling_.find(pts);
  if (it == assembling_.end()) return;
  Picture& p = it->second;
  p.writers--;
  if (p.abandoned) {  // a newer picture completed while we copied
    if (p.writers == 0) { release(p); assembling_.erase(it); }
    return;
  }
  p.meta[index] = TileMeta{w, h, f->colorspace, f->color_range, f->format};
  p.have |= 1u << index;
  if (recv_us > p.last_recv_us) p.last_recv_us = recv_us;
  // Complete when the last tile sent for it has arrived and every tile that was sent is decoded.
  if (!p.last_seen || p.have != p.sent || p.writers > 0) return;

  // Older incomplete pictures will never complete now: drop them.
  Picture done = std::move(p);
  assembling_.erase(it);
  for (auto o = assembling_.begin(); o != assembling_.end() && o->first < pts;) {
    if (o->second.have) { std::lock_guard<std::mutex> s(stats_mu_); stats_.dropped++; }
    if (o->second.writers > 0) { o->second.abandoned = true; ++o; continue; }
    release(o->second);
    o = assembling_.erase(o);
  }
  if (has_ready_) {
    // Not shown yet and now replaced. Its tiles are merged into the new picture where the new one
    // left them out, so no tile update is lost.
    for (size_t i = 0; i < tiles_.size(); i++) {
      uint32_t bit = 1u << i;
      if ((ready_.have & bit) && !(done.have & bit) && done.slot >= 0 && ready_.slot >= 0) {
        memcpy(slots_[done.slot].ptr[i], slots_[ready_.slot].ptr[i], tile_bytes(int(i)));
        done.meta[i] = ready_.meta[i];
        done.have |= bit;
      }
    }
    release(ready_);
    std::lock_guard<std::mutex> s(stats_mu_);
    stats_.dropped++;
  }
  ready_ = std::move(done);
  ready_.complete_us = now_us();
  has_ready_ = true;
  pic_cv_.notify_all();
}

VideoStats Video::take_stats() {
  std::lock_guard<std::mutex> l(stats_mu_);
  VideoStats s = stats_;
  if (decode_n_) { s.decode_ms = decode_sum_ / decode_n_; s.copy_ms = copy_sum_ / decode_n_; }
  if (present_n_) {
    s.present_ms = present_sum_ / present_n_;
    s.wait_ms = wait_sum_ / present_n_;
    s.upload_ms = upload_sum_ / present_n_;
    s.swap_ms = swap_sum_ / present_n_;
    s.flip_ms = flip_sum_ / present_n_;
  }
  stats_ = VideoStats{};
  decode_sum_ = present_sum_ = copy_sum_ = wait_sum_ = upload_sum_ = swap_sum_ = flip_sum_ = 0;
  decode_n_ = present_n_ = 0;
  return s;
}

void Video::present_loop() {
  size_t nt = tiles_.size();

  if (screen_.fd < 0) {  // headless: decode and copy into plain memory, count pictures, show nothing
    {
      std::lock_guard<std::mutex> l(pic_mu_);
      size_t total = 0;
      for (size_t i = 0; i < nt; i++) total += tile_bytes(int(i));
      headless_buf_.resize(total);
      slots_.resize(kSlots);
      for (auto& s : slots_) {
        size_t off = 0;
        for (size_t i = 0; i < nt; i++) { s.ptr.push_back(headless_buf_.data() + off); off += tile_bytes(int(i)); }
      }
      gpu_ready_ = true;
    }
    pic_cv_.notify_all();
    while (!stop_) {
      std::unique_lock<std::mutex> l(pic_mu_);
      pic_cv_.wait_for(l, std::chrono::milliseconds(100), [&] { return stop_ || has_ready_; });
      if (!has_ready_) continue;
      uint64_t last = ready_.last_recv_us;
      release(ready_);
      has_ready_ = false;
      std::lock_guard<std::mutex> s(stats_mu_);
      stats_.shown++;
      present_sum_ += ms_since(last);
      present_n_++;
    }
    return;
  }

  // GPU setup happens right away (not at the first picture), so the first pictures are not dropped.
  auto fail = [&](const char* what) {
    fprintf(stderr, "video: %s (egl error 0x%x)\n", what, eglGetError());
    std::lock_guard<std::mutex> l(pic_mu_);
    gpu_failed_ = true;
    pic_cv_.notify_all();
  };
  gbm_device* gbm = gbm_create_device(screen_.fd);
  gbm_surface* gs = gbm ? gbm_surface_create(gbm, screen_.w, screen_.h, GBM_FORMAT_XRGB8888,
                                             GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)
                        : nullptr;
  EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
  eglInitialize(dpy, nullptr, nullptr);
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
  EGLContext ectx = conf ? eglCreateContext(dpy, conf, EGL_NO_CONTEXT, ctxattr) : EGL_NO_CONTEXT;
  EGLSurface surf = ectx != EGL_NO_CONTEXT ? eglCreateWindowSurface(dpy, conf, (EGLNativeWindowType)gs, nullptr)
                                           : EGL_NO_SURFACE;
  if (!gs || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ectx)) {
    fail("EGL/GBM setup failed");
    return;
  }
  const char* ext = (const char*)glGetString(GL_EXTENSIONS);
  if (!ext || !strstr(ext, "GL_EXT_buffer_storage")) {
    fail("GL_EXT_buffer_storage missing");
    return;
  }
  eglSwapInterval(dpy, 0);
  auto buffer_storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
  if (!buffer_storage) {
    fail("glBufferStorageEXT not found");
    return;
  }

  GLuint prog = glCreateProgram();
  glAttachShader(prog, compile(GL_VERTEX_SHADER, kVertex));
  glAttachShader(prog, compile(GL_FRAGMENT_SHADER, kFragment));
  glBindAttribLocation(prog, 0, "pos");
  glBindAttribLocation(prog, 1, "uv");
  glLinkProgram(prog);
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "ty"), 0);
  glUniform1i(glGetUniformLocation(prog, "tu"), 1);
  glUniform1i(glGetUniformLocation(prog, "tv"), 2);
  GLint u_m = glGetUniformLocation(prog, "m"), u_off = glGetUniformLocation(prog, "off");

  // Three textures (Y, U, V) per tile, allocated at the tile size.
  std::vector<GLuint> tex(nt * 3);
  glGenTextures(GLsizei(tex.size()), tex.data());
  for (size_t i = 0; i < nt; i++) {
    for (int p = 0; p < 3; p++) {
      int w = p ? (cfg_.tiles[i].w + 1) / 2 : cfg_.tiles[i].w, h = p ? (cfg_.tiles[i].h + 1) / 2 : cfg_.tiles[i].h;
      glBindTexture(GL_TEXTURE_2D, tex[i * 3 + p]);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, w, h);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  // Pixel buffers: one per tile per slot, persistently mapped so decoder threads can write into them.
  std::vector<GLuint> pbo(kSlots * nt);
  glGenBuffers(GLsizei(pbo.size()), pbo.data());
  const GLbitfield map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
  {
    std::lock_guard<std::mutex> l(pic_mu_);
    slots_.resize(kSlots);
    for (int s = 0; s < kSlots; s++) {
      for (size_t i = 0; i < nt; i++) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[s * nt + i]);
        buffer_storage(GL_PIXEL_UNPACK_BUFFER, tile_bytes(int(i)), nullptr, map_flags);
        void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, tile_bytes(int(i)), map_flags);
        slots_[s].ptr.push_back((uint8_t*)ptr);
        if (!ptr) gpu_failed_ = true;
      }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!gpu_failed_) gpu_ready_ = true;
  }
  pic_cv_.notify_all();
  if (gpu_failed_) {
    fprintf(stderr, "video: mapping pixel buffers failed\n");
    return;
  }
  fprintf(stderr, "video: GPU ready (%s), %d picture slots\n", (const char*)glGetString(GL_RENDERER), kSlots);

  // Fit the full picture into the panel, keeping its aspect ratio.
  double sc = std::min(double(screen_.w) / cfg_.w, double(screen_.h) / cfg_.h);
  double ox = (screen_.w - cfg_.w * sc) / 2, oy = (screen_.h - cfg_.h * sc) / 2;
  // A 16:9 picture covers the whole 16:9 panel. Then the screen is not cleared before drawing: on this
  // GPU a clear of a scan-out buffer costs about 3 ms at 3840x2160 (measured).
  const bool covers = ox < 0.5 && oy < 0.5;
  std::vector<float> quads;  // per tile: 4 vertices of (x, y, u, v), triangle strip
  for (auto& t : tiles_) {
    auto nx = [&](double px) { return float((ox + px * sc) / screen_.w * 2 - 1); };
    auto ny = [&](double py) { return float(1 - (oy + py * sc) / screen_.h * 2); };
    const auto& c = t->cfg;
    float v[] = {nx(c.x), ny(c.y), 0, 0, nx(c.x + c.w), ny(c.y), 1, 0,
                 nx(c.x), ny(c.y + c.h), 0, 1, nx(c.x + c.w), ny(c.y + c.h), 1, 1};
    quads.insert(quads.end(), v, v + 16);
  }
  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, quads.size() * sizeof(float), quads.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);

  std::vector<TileMeta> tile_meta(nt);
  std::vector<bool> tile_valid(nt, false);
  gbm_bo* shown_bo = nullptr;
  bool crtc_set = false;
  bool flip_pending = false;
  // Behind by one refresh: the picture was complete while the previous flip was still waiting for its
  // refresh, so it can only be shown one refresh later; with pictures arriving at the panel's rate this
  // repeats for every picture (about 8 ms more delay than needed, measured). Skipping one picture leaves
  // the presenter idle when the next one arrives, which then makes the refresh right after it.
  uint64_t flip_done_us = 0;
  double gpu_us = 0;  // GPU work per picture (upload, draw, finish), running average
  int behind = 0;
  uint64_t next_skip_us = 0, skip_backoff_us = kSkipBackoffUs, last_skip_us = 0;
  drmEventContext ev{};
  ev.version = 2;
  ev.page_flip_handler = [](int, unsigned, unsigned, unsigned, void* data) { *(bool*)data = false; };

  while (!stop_) {
    Picture pic;
    {
      std::unique_lock<std::mutex> l(pic_mu_);
      pic_cv_.wait_for(l, std::chrono::milliseconds(100), [&] { return stop_ || has_ready_; });
      if (stop_) break;
      if (!has_ready_) continue;
      pic = std::move(ready_);
      has_ready_ = false;
    }
    uint64_t t_start = now_us();
    // Only when the picture finished early enough before that refresh that, drawn right away, it would
    // have made it; otherwise a skip only costs a picture.
    if (crtc_set && gpu_us > 0 && pic.complete_us + uint64_t(gpu_us) + kBehindMarginUs < flip_done_us) {
      if (++behind >= kBehindToSkip && t_start >= next_skip_us) {
        // A skip that is followed by being behind again within a second did not help (the GPU work does
        // not fit before the refresh at this phase): wait longer before the next one.
        skip_backoff_us = last_skip_us && t_start - last_skip_us < kSkipBackoffUs + 1500000
                              ? std::min(skip_backoff_us * 2, kSkipBackoffMaxUs)
                              : kSkipBackoffUs;
        last_skip_us = t_start;
        next_skip_us = t_start + skip_backoff_us;
        behind = 0;
        fprintf(stderr, "video: one refresh behind; skipping a picture to catch up\n");
        std::lock_guard<std::mutex> l(pic_mu_);
        release(pic);
        std::lock_guard<std::mutex> s(stats_mu_);
        stats_.dropped++;
        continue;
      }
    } else {
      behind = 0;
    }

    // GPU transfer from the pixel buffers into the textures (no CPU copy here). Only tiles that came with
    // this picture are uploaded; the others keep their last picture.
    for (size_t i = 0; i < nt; i++) {
      if (!(pic.have & (1u << i))) continue;
      const TileMeta& m = pic.meta[i];
      int w = m.w, h = m.h, cw = (w + 1) / 2, ch = (h + 1) / 2;
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[pic.slot * nt + i]);
      const size_t offs[3] = {0, size_t(w) * h, size_t(w) * h + size_t(cw) * ch};
      for (int p = 0; p < 3; p++) {
        glActiveTexture(GL_TEXTURE0 + p);
        glBindTexture(GL_TEXTURE_2D, tex[i * 3 + p]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p ? cw : w, p ? ch : h, GL_RED, GL_UNSIGNED_BYTE, (void*)offs[p]);
      }
      tile_meta[i] = m;
      tile_valid[i] = true;
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    glViewport(0, 0, screen_.w, screen_.h);
    bool all_valid = true;
    for (size_t i = 0; i < nt; i++) all_valid = all_valid && tile_valid[i];
    if (!covers || !all_valid) {  // black borders, or tiles not received yet
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    for (size_t i = 0; i < nt; i++) {
      if (!tile_valid[i]) continue;  // never received: stays black
      for (int p = 0; p < 3; p++) {
        glActiveTexture(GL_TEXTURE0 + p);
        glBindTexture(GL_TEXTURE_2D, tex[i * 3 + p]);
      }
      const TileMeta& m = tile_meta[i];
      float mat[9], off[3];
      yuv_matrix(m.colorspace, m.range, m.format, m.h, mat, off);
      glUniformMatrix3fv(u_m, 1, GL_FALSE, mat);
      glUniform3fv(u_off, 1, off);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)(i * 64));
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)(i * 64 + 8));
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // Wait for the GPU here, before the flip is requested. Requested with the drawing still running, the
    // flip lands one refresh later than it could (measured: flip 16.4 ms after the swap, every picture);
    // with the GPU done first it makes the next refresh. The GPU work at 3840x2160 is about 7 ms
    // (measured: 2.8 ms from the pixel buffers into the textures, 4.4 ms to draw).
    glFinish();
    uint64_t t_drawn = now_us();
    gpu_us = gpu_us > 0 ? gpu_us * 0.9 + (t_drawn - t_start) * 0.1 : double(t_drawn - t_start);

    eglSwapBuffers(dpy, surf);
    uint64_t t_swapped = now_us();
    gbm_bo* bo = gbm_surface_lock_front_buffer(gs);
    uint32_t fb = fb_for(screen_.fd, bo);
    if (!crtc_set) {
      if (take_screen_) take_screen_();  // the waiting screen stops and finishes its last flip first
      if (drmModeSetCrtc(screen_.fd, screen_.crtc_id, fb, 0, 0, &screen_.conn_id, 1,
                         (drmModeModeInfo*)screen_.mode) != 0)
        fprintf(stderr, "video: drmModeSetCrtc failed: %s\n", strerror(errno));
      crtc_set = true;
      shown_any_ = true;
    } else if (drmModePageFlip(screen_.fd, screen_.crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, &flip_pending) == 0) {
      flip_pending = true;
      while (flip_pending && !stop_) {  // shown at the next refresh
        pollfd pf{screen_.fd, POLLIN, 0};
        if (poll(&pf, 1, 100) > 0) drmHandleEvent(screen_.fd, &ev);
      }
    } else {
      fprintf(stderr, "video: page flip failed: %s\n", strerror(errno));
    }
    flip_done_us = now_us();
    if (shown_bo) gbm_surface_release_buffer(gs, shown_bo);
    shown_bo = bo;
    {
      // The picture is on screen, so the GPU has finished reading its pixel buffers: free the slot.
      std::lock_guard<std::mutex> l(pic_mu_);
      release(pic);
    }
    std::lock_guard<std::mutex> l(stats_mu_);
    stats_.shown++;
    present_sum_ += ms_since(pic.last_recv_us);
    present_n_++;
    wait_sum_ += (t_start - pic.complete_us) / 1000.0;
    upload_sum_ += (t_drawn - t_start) / 1000.0;
    swap_sum_ += (t_swapped - t_drawn) / 1000.0;
    flip_sum_ += ms_since(t_swapped);
  }

  while (flip_pending) {
    pollfd pf{screen_.fd, POLLIN, 0};
    if (poll(&pf, 1, 100) > 0) drmHandleEvent(screen_.fd, &ev);
    else break;
  }
  // Our buffer is still on the panel. Let the owner put its picture up before the buffer is freed
  // (removing a framebuffer that is being scanned out turns the panel off).
  if (crtc_set && release_screen_) release_screen_();
  {
    std::lock_guard<std::mutex> l(pic_mu_);
    for (auto& s : slots_) s.ptr.clear();
    gpu_ready_ = false;
  }
  for (GLuint b : pbo) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  }
  glDeleteBuffers(GLsizei(pbo.size()), pbo.data());
  glDeleteTextures(GLsizei(tex.size()), tex.data());
  glDeleteBuffers(1, &vbo);
  glDeleteProgram(prog);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(dpy, surf);
  eglDestroyContext(dpy, ectx);
  eglTerminate(dpy);
  if (shown_bo) gbm_surface_release_buffer(gs, shown_bo);
  gbm_surface_destroy(gs);
  gbm_device_destroy(gbm);
}
