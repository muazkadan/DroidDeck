/*
 * Steam's ARM64 streaming_client only offers H.264/HEVC when it finds a KMS card (via a dlopen'd
 * libdrm) and a V4L2 stateful M2M decoder node; it has no software decoder. Android denies apps both
 * the KMS node and the Qualcomm V4L2 nodes, so inside streaming_client this provides a fake KMS
 * libdrm whose dumb buffers are dma-heap dma-bufs, and a fake /dev/video0 that decodes with the
 * libavcodec Steam ships next to streaming_client.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>

#define RP_MAX_FD 4096
#define RP_KIND_DRM 1
#define RP_KIND_V4L2 2
#define RP_MAX_BUFS 32
#define RP_MAX_MAPS 64
#define RP_MAX_DUMB 64
#define RP_VIDEO_PATH "/dev/video0"
#define RP_CARD_PATH "/dev/dri/card0"

static int rp_state;
static unsigned char rp_kind[RP_MAX_FD];
static void *rp_dev[RP_MAX_FD];
static pthread_mutex_t rp_lock = PTHREAD_MUTEX_INITIALIZER;

static void rp_log(const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  line[n++] = '\n';
  int saved = errno;
  if (write(2, "droiddeck-remoteplay: ", 22) > 0 && write(2, line, n) > 0) {}
  errno = saved;
}

static int rp_active(void) {
  int state = __atomic_load_n(&rp_state, __ATOMIC_ACQUIRE);
  if (state) return state > 0;
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  state = -1;
  if (n > 0) {
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    const char *mode = getenv("BL_REMOTEPLAY_DECODER");
    if (!strcmp(base ? base + 1 : exe, "streaming_client") && !(mode && !strcmp(mode, "0"))) state = 1;
  }
  __atomic_store_n(&rp_state, state, __ATOMIC_RELEASE);
  return state > 0;
}

static int rp_fake_fd(int fd) {
  return fd >= 0 && fd < RP_MAX_FD && __atomic_load_n(&rp_kind[fd], __ATOMIC_ACQUIRE);
}

/* libavcodec 62 (FFmpeg 8); only these leading fields of AVPacket and AVFrame are touched. */
typedef struct {
  void *buf;
  int64_t pts, dts;
  uint8_t *data;
  int size;
} rp_avpacket;

typedef struct {
  uint8_t *data[8];
  int linesize[8];
  uint8_t **extended_data;
  int width, height, nb_samples, format;
} rp_avframe;

#define RP_PIX_YUV420P 0
#define RP_PIX_YUVJ420P 12
#define RP_PIX_NV12 23
#define RP_EAGAIN (-EAGAIN)

static struct {
  int state;
  void *(*find_decoder_by_name)(const char *);
  void *(*alloc_context3)(const void *);
  int (*open2)(void *, const void *, void **);
  int (*send_packet)(void *, const void *);
  int (*receive_frame)(void *, void *);
  void (*free_context)(void **);
  void (*flush_buffers)(void *);
  void *(*packet_alloc)(void);
  void (*packet_free)(void **);
  void *(*frame_alloc)(void);
  void (*frame_free)(void **);
  int (*opt_set)(void *, const char *, const char *, int);
  int (*opt_set_int)(void *, const char *, int64_t, int);
} av;

static int rp_load_av(void) {
  if (av.state) return av.state > 0;
  av.state = -1;
  char dir[PATH_MAX], path[PATH_MAX + 32];
  ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
  if (n <= 0) return 0;
  dir[n] = 0;
  char *slash = strrchr(dir, '/');
  if (!slash) return 0;
  *slash = 0;
  snprintf(path, sizeof path, "%s/libavutil.so.60", dir);
  void *util = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  snprintf(path, sizeof path, "%s/libavcodec.so.62", dir);
  void *codec = util ? dlopen(path, RTLD_NOW | RTLD_GLOBAL) : NULL;
  if (!codec) {
    rp_log("cannot load %s: %s", path, dlerror());
    return 0;
  }
#define RP_SYM(lib, field, name) \
  if (!(*(void **)&av.field = dlsym(lib, name))) { rp_log("libavcodec lacks %s", name); return 0; }
  RP_SYM(codec, find_decoder_by_name, "avcodec_find_decoder_by_name");
  RP_SYM(codec, alloc_context3, "avcodec_alloc_context3");
  RP_SYM(codec, open2, "avcodec_open2");
  RP_SYM(codec, send_packet, "avcodec_send_packet");
  RP_SYM(codec, receive_frame, "avcodec_receive_frame");
  RP_SYM(codec, free_context, "avcodec_free_context");
  RP_SYM(codec, flush_buffers, "avcodec_flush_buffers");
  RP_SYM(codec, packet_alloc, "av_packet_alloc");
  RP_SYM(codec, packet_free, "av_packet_free");
  RP_SYM(util, frame_alloc, "av_frame_alloc");
  RP_SYM(util, frame_free, "av_frame_free");
  RP_SYM(util, opt_set, "av_opt_set");
  RP_SYM(util, opt_set_int, "av_opt_set_int");
#undef RP_SYM
  av.state = 1;
  return 1;
}

struct rp_map {
  dev_t dev;
  ino_t ino;
  uint8_t *ptr;
  size_t size;
};

struct rp_capture {
  int fd;
  int queued;
};

struct rp_v4l2 {
  int fd;
  pthread_cond_t cond;
  uint32_t out_width, out_height, out_size;
  uint32_t cap_width, cap_height, cap_stride, cap_size;
  uint32_t out_count, cap_count;
  uint8_t *out_map;
  size_t out_map_size;
  struct timeval out_stamp[RP_MAX_BUFS];
  uint32_t out_used[RP_MAX_BUFS];
  int out_done[RP_MAX_BUFS], out_done_n;
  struct rp_capture cap[RP_MAX_BUFS];
  int cap_queue[RP_MAX_BUFS], cap_queue_n;
  struct {
    int index;
    struct timeval stamp;
    uint32_t sequence;
    int error;
  } cap_done[RP_MAX_BUFS];
  int cap_done_n;
  uint32_t sequence;
  struct rp_map maps[RP_MAX_MAPS];
  int nmaps;
  void *ctx, *pkt, *frame;
  int warned_format;
};

static void rp_unmap_all(struct rp_v4l2 *d) {
  for (int i = 0; i < d->nmaps; i++) munmap(d->maps[i].ptr, d->maps[i].size);
  d->nmaps = 0;
}

static uint8_t *rp_map_dmabuf(struct rp_v4l2 *d, int fd, size_t *size) {
  struct stat st;
  if (fstat(fd, &st)) return NULL;
  for (int i = 0; i < d->nmaps; i++)
    if (d->maps[i].dev == st.st_dev && d->maps[i].ino == st.st_ino) {
      *size = d->maps[i].size;
      return d->maps[i].ptr;
    }
  off_t end = lseek(fd, 0, SEEK_END);
  if (end <= 0 || d->nmaps == RP_MAX_MAPS) return NULL;
  uint8_t *ptr = mmap(NULL, (size_t)end, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    rp_log("cannot map capture dma-buf: %s", strerror(errno));
    return NULL;
  }
  d->maps[d->nmaps++] = (struct rp_map){st.st_dev, st.st_ino, ptr, (size_t)end};
  *size = (size_t)end;
  return ptr;
}

static void rp_close_decoder(struct rp_v4l2 *d) {
  if (d->ctx) av.free_context(&d->ctx);
  if (d->pkt) av.packet_free(&d->pkt);
  if (d->frame) av.frame_free(&d->frame);
}

static int rp_open_decoder(struct rp_v4l2 *d) {
  if (d->ctx) return 1;
  if (!rp_load_av()) return 0;
  const void *codec = av.find_decoder_by_name("h264");
  if (!codec) {
    rp_log("libavcodec has no h264 decoder");
    return 0;
  }
  d->ctx = av.alloc_context3(codec);
  d->pkt = av.packet_alloc();
  d->frame = av.frame_alloc();
  if (!d->ctx || !d->pkt || !d->frame) {
    rp_close_decoder(d);
    return 0;
  }
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  av.opt_set(d->ctx, "flags", "+low_delay", 0);
  av.opt_set(d->ctx, "flags2", "+fast", 0);
  av.opt_set_int(d->ctx, "thread_type", 2, 0);
  av.opt_set_int(d->ctx, "threads", cpus > 4 ? 4 : cpus > 0 ? cpus : 1, 0);
  if (av.open2(d->ctx, codec, NULL) < 0) {
    rp_log("cannot open the h264 decoder");
    rp_close_decoder(d);
    return 0;
  }
  rp_log("decoding h264 in software (%ux%u)", d->out_width, d->out_height);
  return 1;
}

static void rp_copy_nv12(struct rp_v4l2 *d, const rp_avframe *f, uint8_t *dst, size_t size) {
  uint32_t width = (uint32_t)f->width < d->cap_width ? (uint32_t)f->width : d->cap_width;
  uint32_t height = (uint32_t)f->height < d->cap_height ? (uint32_t)f->height : d->cap_height;
  size_t stride = d->cap_stride;
  if ((size_t)stride * d->cap_height * 3 / 2 > size) return;
  uint8_t *uv = dst + stride * d->cap_height;
  for (uint32_t y = 0; y < height; y++) memcpy(dst + y * stride, f->data[0] + (size_t)y * f->linesize[0], width);
  if (f->format == RP_PIX_NV12) {
    for (uint32_t y = 0; y < height / 2; y++)
      memcpy(uv + y * stride, f->data[1] + (size_t)y * f->linesize[1], width & ~1u);
    return;
  }
  for (uint32_t y = 0; y < height / 2; y++) {
    const uint8_t *u = f->data[1] + (size_t)y * f->linesize[1];
    const uint8_t *v = f->data[2] + (size_t)y * f->linesize[2];
    uint8_t *row = uv + y * stride;
    for (uint32_t x = 0; x < width / 2; x++) {
      row[2 * x] = u[x];
      row[2 * x + 1] = v[x];
    }
  }
}

static void rp_emit(struct rp_v4l2 *d, const struct timeval *stamp, int error) {
  if (!d->cap_queue_n) return;
  int index = d->cap_queue[0];
  memmove(d->cap_queue, d->cap_queue + 1, --d->cap_queue_n * sizeof d->cap_queue[0]);
  d->cap[index].queued = 0;
  if (!error) {
    const rp_avframe *f = d->frame;
    if (f->format != RP_PIX_YUV420P && f->format != RP_PIX_YUVJ420P && f->format != RP_PIX_NV12) {
      if (!d->warned_format++) rp_log("unsupported decoded pixel format %d", f->format);
      error = 1;
    } else {
      size_t size;
      uint8_t *dst = rp_map_dmabuf(d, d->cap[index].fd, &size);
      if (dst) {
        struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE};
        ioctl(d->cap[index].fd, DMA_BUF_IOCTL_SYNC, &sync);
        rp_copy_nv12(d, f, dst, size);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        ioctl(d->cap[index].fd, DMA_BUF_IOCTL_SYNC, &sync);
      } else {
        error = 1;
      }
    }
  }
  int n = d->cap_done_n++;
  d->cap_done[n].index = index;
  d->cap_done[n].stamp = *stamp;
  d->cap_done[n].sequence = d->sequence++;
  d->cap_done[n].error = error;
}

static void rp_decode(struct rp_v4l2 *d, uint32_t index) {
  const struct timeval *stamp = &d->out_stamp[index];
  if (!rp_open_decoder(d)) {
    rp_emit(d, stamp, 1);
    return;
  }
  rp_avpacket *pkt = d->pkt;
  pkt->data = d->out_map + (size_t)index * d->out_size;
  pkt->size = (int)d->out_used[index];
  pkt->pts = pkt->dts = (int64_t)stamp->tv_sec * 1000000 + stamp->tv_usec;
  int rc = av.send_packet(d->ctx, pkt);
  pkt->data = NULL;
  pkt->size = 0;
  int frames = 0;
  while (rc >= 0 && av.receive_frame(d->ctx, d->frame) >= 0) {
    rp_emit(d, stamp, 0);
    frames++;
  }
  if (!frames && rc < 0 && rc != RP_EAGAIN) rp_emit(d, stamp, 1);
}

static int rp_out_buffers(struct rp_v4l2 *d, uint32_t count) {
  if (d->out_map) munmap(d->out_map, d->out_map_size);
  d->out_map = NULL;
  d->out_map_size = 0;
  d->out_count = 0;
  d->out_done_n = 0;
  if (!count) return 0;
  if (count > RP_MAX_BUFS) count = RP_MAX_BUFS;
  size_t total = (size_t)count * d->out_size;
  if (ftruncate(d->fd, (off_t)total)) return -1;
  void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
  if (map == MAP_FAILED) return -1;
  d->out_map = map;
  d->out_map_size = total;
  d->out_count = count;
  return 0;
}

static void rp_cap_reset(struct rp_v4l2 *d) {
  for (int i = 0; i < RP_MAX_BUFS; i++) d->cap[i].queued = 0;
  d->cap_queue_n = 0;
  d->cap_done_n = 0;
}

static void rp_set_out_size(struct rp_v4l2 *d, uint32_t requested) {
  uint32_t size = d->out_width * d->out_height * 3 / 2;
  if (size < (4u << 20)) size = 4u << 20;
  if (requested > size && requested < (64u << 20)) size = requested;
  d->out_size = (size + 4095) & ~4095u;
}

static void rp_set_cap_format(struct rp_v4l2 *d, uint32_t width, uint32_t height) {
  if (!width || !height) width = 1920, height = 1080;
  d->cap_width = (width + 15) & ~15u;
  d->cap_height = (height + 15) & ~15u;
  d->cap_stride = (d->cap_width + 255) & ~255u;
  d->cap_size = d->cap_stride * d->cap_height * 3 / 2;
}

static void rp_fill_format(struct rp_v4l2 *d, struct v4l2_format *f) {
  struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
  memset(&f->fmt, 0, sizeof f->fmt);
  p->field = V4L2_FIELD_NONE;
  p->num_planes = 1;
  if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
    p->width = d->out_width;
    p->height = d->out_height;
    p->pixelformat = V4L2_PIX_FMT_H264;
    p->plane_fmt[0].sizeimage = d->out_size;
  } else {
    p->width = d->cap_width;
    p->height = d->cap_height;
    p->pixelformat = V4L2_PIX_FMT_NV12;
    p->colorspace = V4L2_COLORSPACE_REC709;
    p->plane_fmt[0].bytesperline = d->cap_stride;
    p->plane_fmt[0].sizeimage = d->cap_size;
  }
}

static int rp_v4l2_ioctl(struct rp_v4l2 *d, unsigned long request, void *arg) {
  switch (request) {
  case VIDIOC_QUERYCAP: {
    struct v4l2_capability *cap = arg;
    memset(cap, 0, sizeof *cap);
    strcpy((char *)cap->driver, "droiddeck");
    strcpy((char *)cap->card, "DroidDeck software decoder");
    strcpy((char *)cap->bus_info, "platform:droiddeck-decoder");
    cap->version = 0x060000;
    cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
    cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
    return 0;
  }
  case VIDIOC_ENUM_FMT: {
    struct v4l2_fmtdesc *desc = arg;
    uint32_t fourcc = 0;
    if (desc->index == 0 && desc->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) fourcc = V4L2_PIX_FMT_H264;
    if (desc->index == 0 && desc->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) fourcc = V4L2_PIX_FMT_NV12;
    if (!fourcc) return -EINVAL;
    uint32_t index = desc->index, type = desc->type;
    memset(desc, 0, sizeof *desc);
    desc->index = index;
    desc->type = type;
    desc->pixelformat = fourcc;
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) desc->flags = V4L2_FMT_FLAG_COMPRESSED;
    snprintf((char *)desc->description, sizeof desc->description, "%.4s", (char *)&fourcc);
    return 0;
  }
  case VIDIOC_G_FMT: {
    struct v4l2_format *f = arg;
    if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) return -EINVAL;
    rp_fill_format(d, f);
    return 0;
  }
  case VIDIOC_S_FMT: {
    struct v4l2_format *f = arg;
    struct v4l2_pix_format_mplane *p = &f->fmt.pix_mp;
    if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
      d->out_width = p->width;
      d->out_height = p->height;
      if (!d->out_count) rp_set_out_size(d, p->plane_fmt[0].sizeimage);
      if (!d->cap_count) rp_set_cap_format(d, p->width, p->height);
    } else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      if (!d->cap_count) rp_set_cap_format(d, p->width, p->height);
    } else {
      return -EINVAL;
    }
    rp_fill_format(d, f);
    return 0;
  }
  case VIDIOC_REQBUFS: {
    struct v4l2_requestbuffers *req = arg;
    memset(req->reserved, 0, sizeof req->reserved);
    if (req->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
      if (req->memory != V4L2_MEMORY_MMAP) return -EINVAL;
      if (rp_out_buffers(d, req->count)) return -ENOMEM;
      req->count = d->out_count;
      req->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
      return 0;
    }
    if (req->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      if (req->memory != V4L2_MEMORY_DMABUF) return -EINVAL;
      rp_cap_reset(d);
      rp_unmap_all(d);
      d->cap_count = req->count > RP_MAX_BUFS ? RP_MAX_BUFS : req->count;
      req->count = d->cap_count;
      req->capabilities = V4L2_BUF_CAP_SUPPORTS_DMABUF;
      return 0;
    }
    return -EINVAL;
  }
  case VIDIOC_QUERYBUF: {
    struct v4l2_buffer *b = arg;
    if (b->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE || b->index >= d->out_count || !b->length || !b->m.planes) return -EINVAL;
    struct v4l2_plane *plane = b->m.planes;
    memset(plane, 0, sizeof *plane);
    plane->length = d->out_size;
    plane->m.mem_offset = b->index * d->out_size;
    b->length = 1;
    b->memory = V4L2_MEMORY_MMAP;
    b->flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    return 0;
  }
  case VIDIOC_QBUF: {
    struct v4l2_buffer *b = arg;
    if (!b->length || !b->m.planes) return -EINVAL;
    if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
      if (b->index >= d->out_count) return -EINVAL;
      uint32_t used = b->m.planes[0].bytesused;
      d->out_used[b->index] = used > d->out_size ? d->out_size : used;
      d->out_stamp[b->index] = b->timestamp;
      rp_decode(d, b->index);
      if (d->out_done_n < RP_MAX_BUFS) d->out_done[d->out_done_n++] = (int)b->index;
      pthread_cond_broadcast(&d->cond);
      return 0;
    }
    if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      if (b->index >= d->cap_count) return -EINVAL;
      d->cap[b->index].fd = b->m.planes[0].m.fd;
      if (d->cap[b->index].queued) return 0;
      for (int i = 0; i < d->cap_done_n; i++)
        if (d->cap_done[i].index == (int)b->index) return 0;
      d->cap[b->index].queued = 1;
      d->cap_queue[d->cap_queue_n++] = (int)b->index;
      return 0;
    }
    return -EINVAL;
  }
  case VIDIOC_DQBUF: {
    struct v4l2_buffer *b = arg;
    if (!b->length || !b->m.planes) return -EINVAL;
    if (b->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
      if (!d->out_done_n) return -EAGAIN;
      int index = d->out_done[0];
      memmove(d->out_done, d->out_done + 1, --d->out_done_n * sizeof d->out_done[0]);
      b->index = (uint32_t)index;
      b->flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
      b->field = V4L2_FIELD_NONE;
      b->timestamp = d->out_stamp[index];
      b->bytesused = 0;
      b->length = 1;
      b->m.planes[0].bytesused = d->out_used[index];
      b->m.planes[0].length = d->out_size;
      b->m.planes[0].m.mem_offset = (uint32_t)index * d->out_size;
      return 0;
    }
    if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      if (!d->cap_done_n) return -EAGAIN;
      int index = d->cap_done[0].index;
      b->index = (uint32_t)index;
      b->flags = V4L2_BUF_FLAG_TIMESTAMP_COPY | (d->cap_done[0].error ? V4L2_BUF_FLAG_ERROR : 0);
      b->field = V4L2_FIELD_NONE;
      b->timestamp = d->cap_done[0].stamp;
      b->sequence = d->cap_done[0].sequence;
      b->bytesused = 0;
      b->length = 1;
      b->m.planes[0].bytesused = d->cap_size;
      b->m.planes[0].length = d->cap_size;
      b->m.planes[0].m.fd = d->cap[index].fd;
      b->m.planes[0].data_offset = 0;
      memmove(d->cap_done, d->cap_done + 1, --d->cap_done_n * sizeof d->cap_done[0]);
      return 0;
    }
    return -EINVAL;
  }
  case VIDIOC_STREAMON:
    return 0;
  case VIDIOC_STREAMOFF: {
    int type = *(int *)arg;
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
      d->out_done_n = 0;
      if (d->ctx) av.flush_buffers(d->ctx);
    } else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      rp_cap_reset(d);
    }
    pthread_cond_broadcast(&d->cond);
    return 0;
  }
  }
  return -ENOTTY;
}

static void rp_v4l2_free(struct rp_v4l2 *d) {
  rp_close_decoder(d);
  rp_out_buffers(d, 0);
  rp_unmap_all(d);
  pthread_cond_destroy(&d->cond);
  free(d);
}

static int rp_new_fd(const char *name, int flags) {
  int fd = (int)syscall(SYS_memfd_create, name, (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0);
  if (fd >= RP_MAX_FD) {
    close(fd);
    errno = EMFILE;
    return -1;
  }
  return fd;
}

__attribute__((visibility("hidden"))) int bl_remoteplay_open(const char *path, int flags) {
  if (!path || strncmp(path, "/dev/", 5) || !rp_active()) return -2;
  int kind = !strcmp(path, RP_VIDEO_PATH) ? RP_KIND_V4L2 : !strcmp(path, RP_CARD_PATH) ? RP_KIND_DRM : 0;
  if (!kind) return -2;
  int fd = rp_new_fd(kind == RP_KIND_V4L2 ? "droiddeck-v4l2" : "droiddeck-kms", flags);
  if (fd < 0) return -1;
  if (kind == RP_KIND_V4L2) {
    struct rp_v4l2 *d = calloc(1, sizeof *d);
    if (!d) {
      close(fd);
      errno = ENOMEM;
      return -1;
    }
    d->fd = fd;
    pthread_cond_init(&d->cond, NULL);
    rp_set_out_size(d, 0);
    rp_set_cap_format(d, 0, 0);
    pthread_mutex_lock(&rp_lock);
    rp_dev[fd] = d;
    __atomic_store_n(&rp_kind[fd], kind, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&rp_lock);
  } else {
    __atomic_store_n(&rp_kind[fd], kind, __ATOMIC_RELEASE);
  }
  return fd;
}

__attribute__((visibility("hidden"))) int bl_remoteplay_ioctl(int fd, unsigned long request, void *arg, int *rc) {
  if (!rp_fake_fd(fd)) return 0;
  if (__atomic_load_n(&rp_kind[fd], __ATOMIC_ACQUIRE) != RP_KIND_V4L2) {
    *rc = -1;
    errno = ENOTTY;
    return 1;
  }
  pthread_mutex_lock(&rp_lock);
  struct rp_v4l2 *d = rp_dev[fd];
  int r = d ? rp_v4l2_ioctl(d, request, arg) : -EBADF;
  pthread_mutex_unlock(&rp_lock);
  if (r < 0) {
    errno = -r;
    *rc = -1;
  } else {
    *rc = r;
  }
  return 1;
}

__attribute__((visibility("hidden"))) void bl_remoteplay_closed(int fd) {
  if (!rp_fake_fd(fd)) return;
  pthread_mutex_lock(&rp_lock);
  struct rp_v4l2 *d = rp_dev[fd];
  rp_dev[fd] = NULL;
  __atomic_store_n(&rp_kind[fd], 0, __ATOMIC_RELEASE);
  if (d) rp_v4l2_free(d);
  pthread_mutex_unlock(&rp_lock);
}

static short rp_revents(struct rp_v4l2 *d, short events) {
  short ready = 0;
  if (d->cap_done_n) ready |= POLLIN | POLLRDNORM;
  if (d->out_done_n) ready |= POLLOUT | POLLWRNORM;
  return ready & (events | POLLERR | POLLHUP);
}

static int rp_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  static int (*real)(struct pollfd *, nfds_t, int);
  if (nfds == 1 && fds && rp_fake_fd(fds[0].fd) && rp_kind[fds[0].fd] == RP_KIND_V4L2) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (timeout > 0) {
      deadline.tv_sec += timeout / 1000;
      deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
      if (deadline.tv_nsec >= 1000000000) deadline.tv_sec++, deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&rp_lock);
    for (;;) {
      struct rp_v4l2 *d = rp_dev[fds[0].fd];
      if (!d) {
        fds[0].revents = POLLNVAL;
        break;
      }
      fds[0].revents = rp_revents(d, fds[0].events);
      if (fds[0].revents || !timeout) break;
      int r = timeout < 0 ? pthread_cond_wait(&d->cond, &rp_lock) : pthread_cond_timedwait(&d->cond, &rp_lock, &deadline);
      if (r == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&rp_lock);
    return fds[0].revents ? 1 : 0;
  }
  if (!real) real = (int (*)(struct pollfd *, nfds_t, int))dlsym(RTLD_NEXT, "poll");
  return real(fds, nfds, timeout);
}

extern __typeof(rp_poll) poll __attribute__((alias("rp_poll"), visibility("default")));

/* libdrm 2.4 ABI */
typedef struct {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh, flags, type;
  char name[32];
} rp_drm_mode;

typedef struct {
  int count_fbs;
  uint32_t *fbs;
  int count_crtcs;
  uint32_t *crtcs;
  int count_connectors;
  uint32_t *connectors;
  int count_encoders;
  uint32_t *encoders;
  uint32_t min_width, max_width, min_height, max_height;
} rp_drm_res;

typedef struct {
  uint32_t connector_id, encoder_id, connector_type, connector_type_id;
  int connection;
  uint32_t mm_width, mm_height;
  int subpixel;
  int count_modes;
  rp_drm_mode *modes;
  int count_props;
  uint32_t *props;
  uint64_t *prop_values;
  int count_encoders;
  uint32_t *encoders;
} rp_drm_connector;

typedef struct {
  uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones;
} rp_drm_encoder;

typedef struct {
  uint32_t crtc_id, buffer_id, x, y, width, height;
  int mode_valid;
  rp_drm_mode mode;
  int gamma_size;
} rp_drm_crtc;

typedef struct {
  char **nodes;
  int available_nodes;
  int bustype;
  void *businfo;
  void *deviceinfo;
} rp_drm_device;

#define RP_CRTC_ID 31
#define RP_CONNECTOR_ID 32
#define RP_ENCODER_ID 33

static char *rp_nodes[3] = {RP_CARD_PATH, NULL, NULL};
static rp_drm_device rp_device = {rp_nodes, 1, 2, NULL, NULL};
static rp_drm_mode rp_mode = {148500, 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0, 60, 5, 0x48, "1920x1080"};
static uint32_t rp_crtc_ids[1] = {RP_CRTC_ID}, rp_connector_ids[1] = {RP_CONNECTOR_ID}, rp_encoder_ids[1] = {RP_ENCODER_ID};

static struct {
  uint32_t handle;
  int fd;
} rp_dumb[RP_MAX_DUMB];
static uint32_t rp_next_handle = 1;

static int rp_drm_get_devices2(uint32_t flags, rp_drm_device *devices[], int max) {
  (void)flags;
  if (devices && max > 0) devices[0] = &rp_device;
  return 1;
}

static void rp_drm_free_devices(rp_drm_device *devices[], int count) {
  (void)devices;
  (void)count;
}

static int rp_drm_ok(void) {
  return 0;
}

static int rp_drm_fail(void) {
  errno = EOPNOTSUPP;
  return -1;
}

static void *rp_drm_null(void) {
  errno = ENOENT;
  return NULL;
}

static rp_drm_res *rp_drm_get_resources(int fd) {
  (void)fd;
  rp_drm_res *res = calloc(1, sizeof *res);
  if (!res) return NULL;
  res->count_crtcs = res->count_connectors = res->count_encoders = 1;
  res->crtcs = rp_crtc_ids;
  res->connectors = rp_connector_ids;
  res->encoders = rp_encoder_ids;
  res->max_width = res->max_height = 8192;
  return res;
}

static rp_drm_connector *rp_drm_get_connector(int fd, uint32_t id) {
  (void)fd;
  if (id != RP_CONNECTOR_ID) return rp_drm_null();
  rp_drm_connector *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->connector_id = RP_CONNECTOR_ID;
  c->encoder_id = RP_ENCODER_ID;
  c->connector_type = DRM_MODE_CONNECTOR_DSI;
  c->connector_type_id = 1;
  c->connection = 1;
  c->count_modes = 1;
  c->modes = &rp_mode;
  c->count_encoders = 1;
  c->encoders = rp_encoder_ids;
  return c;
}

static rp_drm_encoder *rp_drm_get_encoder(int fd, uint32_t id) {
  (void)fd;
  if (id != RP_ENCODER_ID) return rp_drm_null();
  rp_drm_encoder *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  *e = (rp_drm_encoder){RP_ENCODER_ID, DRM_MODE_ENCODER_DSI, RP_CRTC_ID, 1, 0};
  return e;
}

static rp_drm_crtc *rp_drm_get_crtc(int fd, uint32_t id) {
  (void)fd;
  if (id != RP_CRTC_ID) return rp_drm_null();
  rp_drm_crtc *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->crtc_id = RP_CRTC_ID;
  c->width = rp_mode.hdisplay;
  c->height = rp_mode.vdisplay;
  c->mode_valid = 1;
  c->mode = rp_mode;
  return c;
}

static void rp_drm_free(void *p) {
  free(p);
}

static int rp_heap_alloc(size_t size) {
  static int heap = -2;
  if (heap == -2) {
    heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) rp_log("no /dev/dma_heap/system (%s), frames go through memfd", strerror(errno));
  }
  if (heap >= 0) {
    struct dma_heap_allocation_data data = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) == 0) return (int)data.fd;
  }
  int fd = (int)syscall(SYS_memfd_create, "droiddeck-frame", MFD_CLOEXEC);
  if (fd >= 0 && ftruncate(fd, (off_t)size)) {
    close(fd);
    return -1;
  }
  return fd;
}

static int rp_drm_ioctl(int fd, unsigned long request, void *arg) {
  (void)fd;
  int r = -1;
  pthread_mutex_lock(&rp_lock);
  if (request == DRM_IOCTL_MODE_CREATE_DUMB) {
    struct drm_mode_create_dumb *c = arg;
    int slot = 0;
    while (slot < RP_MAX_DUMB && rp_dumb[slot].handle) slot++;
    uint32_t pitch = ((c->width * ((c->bpp + 7) / 8)) + 255) & ~255u;
    uint64_t size = ((uint64_t)pitch * c->height + 4095) & ~(uint64_t)4095;
    int buf = slot < RP_MAX_DUMB ? rp_heap_alloc((size_t)size) : -1;
    if (buf >= 0) {
      rp_dumb[slot].handle = rp_next_handle++;
      rp_dumb[slot].fd = buf;
      c->handle = rp_dumb[slot].handle;
      c->pitch = pitch;
      c->size = size;
      r = 0;
    } else {
      errno = ENOMEM;
    }
  } else if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
    struct drm_prime_handle *p = arg;
    errno = ENOENT;
    for (int i = 0; i < RP_MAX_DUMB; i++)
      if (rp_dumb[i].handle && rp_dumb[i].handle == p->handle) {
        p->fd = fcntl(rp_dumb[i].fd, (p->flags & DRM_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
        r = p->fd >= 0 ? 0 : -1;
        break;
      }
  } else if (request == DRM_IOCTL_MODE_DESTROY_DUMB || request == DRM_IOCTL_GEM_CLOSE) {
    uint32_t handle = *(uint32_t *)arg;
    errno = ENOENT;
    for (int i = 0; i < RP_MAX_DUMB; i++)
      if (rp_dumb[i].handle && rp_dumb[i].handle == handle) {
        close(rp_dumb[i].fd);
        rp_dumb[i].handle = 0;
        r = 0;
        break;
      }
  } else {
    errno = ENOTTY;
  }
  pthread_mutex_unlock(&rp_lock);
  return r;
}

static const struct {
  const char *name;
  void *fn;
} rp_drm_functions[] = {
    {"drmGetDevices2", (void *)rp_drm_get_devices2},
    {"drmFreeDevices", (void *)rp_drm_free_devices},
    {"drmSetMaster", (void *)rp_drm_fail},
    {"drmSetClientCap", (void *)rp_drm_ok},
    {"drmModeGetResources", (void *)rp_drm_get_resources},
    {"drmModeFreeResources", (void *)rp_drm_free},
    {"drmModeGetConnector", (void *)rp_drm_get_connector},
    {"drmModeFreeConnector", (void *)rp_drm_free},
    {"drmModeGetEncoder", (void *)rp_drm_get_encoder},
    {"drmModeFreeEncoder", (void *)rp_drm_free},
    {"drmModeGetCrtc", (void *)rp_drm_get_crtc},
    {"drmModeSetCrtc", (void *)rp_drm_fail},
    {"drmModeFreeCrtc", (void *)rp_drm_free},
    {"drmModeGetPlaneResources", (void *)rp_drm_null},
    {"drmModeFreePlaneResources", (void *)rp_drm_free},
    {"drmModeGetPlane", (void *)rp_drm_null},
    {"drmModeFreePlane", (void *)rp_drm_free},
    {"drmModeObjectGetProperties", (void *)rp_drm_null},
    {"drmModeFreeObjectProperties", (void *)rp_drm_free},
    {"drmModeGetProperty", (void *)rp_drm_null},
    {"drmModeFreeProperty", (void *)rp_drm_free},
    {"drmModeAddFB2", (void *)rp_drm_fail},
    {"drmModeAddFB2WithModifiers", (void *)rp_drm_fail},
    {"drmModeRmFB", (void *)rp_drm_ok},
    {"drmModeAtomicAlloc", (void *)rp_drm_null},
    {"drmModeAtomicFree", (void *)rp_drm_free},
    {"drmModeAtomicAddProperty", (void *)rp_drm_fail},
    {"drmModeAtomicCommit", (void *)rp_drm_fail},
    {"drmIoctl", (void *)rp_drm_ioctl},
};

static char rp_libdrm;

void *SDL_LoadObject(const char *name) {
  static void *(*real)(const char *);
  if (name && !strcmp(name, "libdrm.so.2") && rp_active()) return &rp_libdrm;
  if (!real) real = (void *(*)(const char *))dlsym(RTLD_NEXT, "SDL_LoadObject");
  return real ? real(name) : NULL;
}

void *SDL_LoadFunction(void *handle, const char *name) {
  static void *(*real)(void *, const char *);
  if (handle == &rp_libdrm) {
    for (size_t i = 0; i < sizeof rp_drm_functions / sizeof rp_drm_functions[0]; i++)
      if (!strcmp(name, rp_drm_functions[i].name)) return rp_drm_functions[i].fn;
    return NULL;
  }
  if (!real) real = (void *(*)(void *, const char *))dlsym(RTLD_NEXT, "SDL_LoadFunction");
  return real ? real(handle, name) : NULL;
}

void SDL_UnloadObject(void *handle) {
  static void (*real)(void *);
  if (handle == &rp_libdrm) return;
  if (!real) real = (void (*)(void *))dlsym(RTLD_NEXT, "SDL_UnloadObject");
  if (real) real(handle);
}
