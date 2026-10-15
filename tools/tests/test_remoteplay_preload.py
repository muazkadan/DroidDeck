from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PRELOAD = Path(__file__).resolve().parents[1] / "linuxfs/preload"

PROGRAM = r"""
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/videodev2.h>

int bl_remoteplay_open(const char *path, int flags);
int bl_remoteplay_ioctl(int fd, unsigned long request, void *arg, int *rc);
void bl_remoteplay_closed(int fd);
void *SDL_LoadObject(const char *name);
void *SDL_LoadFunction(void *handle, const char *name);

static int v4l2(int fd, unsigned long request, void *arg) {
  int rc;
  if (!bl_remoteplay_ioctl(fd, request, arg, &rc)) return -100;
  return rc < 0 ? -errno : rc;
}

int main(void) {
  int fd = bl_remoteplay_open("/dev/video0", O_RDWR);
  if (fd == -2) { puts("inactive"); return 0; }

  struct v4l2_capability cap = {0};
  v4l2(fd, VIDIOC_QUERYCAP, &cap);
  printf("caps %d\n", (cap.capabilities & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING))
                      == (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING));
  struct v4l2_fmtdesc desc = {.index = 0, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
  int listed = v4l2(fd, VIDIOC_ENUM_FMT, &desc);
  printf("fmt0 %d %.4s\n", listed, (char *)&desc.pixelformat);
  desc = (struct v4l2_fmtdesc){.index = 1, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
  printf("fmt1 %d\n", v4l2(fd, VIDIOC_ENUM_FMT, &desc));

  struct v4l2_format out = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
  out.fmt.pix_mp.width = 1280;
  out.fmt.pix_mp.height = 720;
  out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
  v4l2(fd, VIDIOC_S_FMT, &out);
  struct v4l2_format capfmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
  capfmt.fmt.pix_mp.width = 1280;
  capfmt.fmt.pix_mp.height = 720;
  capfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  v4l2(fd, VIDIOC_S_FMT, &capfmt);
  printf("capture %.4s %u planes %u stride %u\n", (char *)&capfmt.fmt.pix_mp.pixelformat,
         capfmt.fmt.pix_mp.num_planes, capfmt.fmt.pix_mp.plane_fmt[0].bytesperline % 256,
         capfmt.fmt.pix_mp.plane_fmt[0].sizeimage >= 1280 * 720 * 3 / 2);

  struct v4l2_requestbuffers req = {.count = 2, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP};
  int requested = v4l2(fd, VIDIOC_REQBUFS, &req);
  printf("reqbufs %d %u\n", requested, req.count);
  struct v4l2_plane plane = {0};
  struct v4l2_buffer buf = {.index = 1, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                            .m.planes = &plane, .length = 1};
  v4l2(fd, VIDIOC_QUERYBUF, &buf);
  uint8_t *bits = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, plane.m.mem_offset);
  printf("mapped %d\n", bits != MAP_FAILED);
  memcpy(bits, "\0\0\0\1", 4);

  void *drm = SDL_LoadObject("libdrm.so.2");
  int (*get_devices)(uint32_t, void **, int) = SDL_LoadFunction(drm, "drmGetDevices2");
  int (*drm_ioctl)(int, unsigned long, void *) = SDL_LoadFunction(drm, "drmIoctl");
  void *(*get_resources)(int) = SDL_LoadFunction(drm, "drmModeGetResources");
  struct { char **nodes; int available; } *device;
  printf("devices %d", get_devices(0, NULL, 0));
  get_devices(0, (void **)&device, 1);
  int card = bl_remoteplay_open(device->nodes[0], O_RDWR);
  int *res = get_resources(card);
  printf(" %s primary %d crtcs %d connectors %d\n", device->nodes[0], device->available & 1, res[4], res[8]);

  struct drm_mode_create_dumb dumb = {.width = capfmt.fmt.pix_mp.width, .height = capfmt.fmt.pix_mp.height * 3 / 2, .bpp = 8};
  int made = drm_ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb);
  struct drm_prime_handle prime = {.handle = dumb.handle, .flags = DRM_CLOEXEC | DRM_RDWR};
  int exported = drm_ioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
  printf("dumb %d prime %d destroy %d\n", made, exported, drm_ioctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb.handle));

  req = (struct v4l2_requestbuffers){.count = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF};
  v4l2(fd, VIDIOC_REQBUFS, &req);
  struct v4l2_plane cplane = {.m.fd = prime.fd};
  struct v4l2_buffer cbuf = {.index = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
                             .m.planes = &cplane, .length = 1};
  printf("qbuf capture %d", v4l2(fd, VIDIOC_QBUF, &cbuf));
  printf(" again %d\n", v4l2(fd, VIDIOC_QBUF, &cbuf));

  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT};
  printf("idle poll %d\n", poll(&pfd, 1, 0));
  plane.bytesused = 4;
  buf.timestamp.tv_sec = 7;
  buf.timestamp.tv_usec = 42;
  printf("qbuf output %d\n", v4l2(fd, VIDIOC_QBUF, &buf));
  int ready = poll(&pfd, 1, 30);
  printf("poll %d %d %d\n", ready, !!(pfd.revents & POLLIN), !!(pfd.revents & POLLOUT));

  struct v4l2_buffer done = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
                             .m.planes = &cplane, .length = 1};
  int got = v4l2(fd, VIDIOC_DQBUF, &done);
  printf("dqbuf capture %d index %u stamp %ld.%ld fd %d\n", got, done.index,
         (long)done.timestamp.tv_sec, (long)done.timestamp.tv_usec, cplane.m.fd == prime.fd);
  done = (struct v4l2_buffer){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                              .m.planes = &plane, .length = 1};
  got = v4l2(fd, VIDIOC_DQBUF, &done);
  printf("dqbuf output %d index %u\n", got, done.index);
  printf("empty %d\n", v4l2(fd, VIDIOC_DQBUF, &done));

  bl_remoteplay_closed(fd);
  close(fd);
  printf("closed %d\n", v4l2(fd, VIDIOC_QUERYCAP, &cap));
  printf("other %d\n", bl_remoteplay_open("/dev/video1", O_RDWR));
  return 0;
}
"""


@unittest.skipUnless(shutil.which("cc"), "needs cc")
class RemotePlayPreloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        source = cls.tmp / "program.c"
        source.write_text(PROGRAM)
        cls.client = cls.tmp / "streaming_client"
        subprocess.run(["cc", "-O2", "-Wall", "-Werror", "-pthread", "-o", str(cls.client), str(source),
                        str(PRELOAD / "remoteplay.c"), "-ldl"], check=True)
        cls.other = cls.tmp / "steam"
        shutil.copy(cls.client, cls.other)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_program(self, program, **env):
        result = subprocess.run([str(program)], env={"PATH": "/usr/bin:/bin", **env},
                                capture_output=True, text=True, timeout=30, check=True)
        return result.stdout.splitlines()

    def test_streaming_client_sees_a_decoder_and_a_kms_card(self):
        out = self.run_program(self.client)
        self.assertEqual(out[:6], ["caps 1", "fmt0 0 H264", "fmt1 -22", "capture NV12 1 planes 0 stride 1",
                                   "reqbufs 0 2", "mapped 1"])
        self.assertEqual(out[6], "devices 1 /dev/dri/card0 primary 1 crtcs 1 connectors 1")
        self.assertEqual(out[7], "dumb 0 prime 0 destroy 0")

    def test_a_queued_bitstream_buffer_returns_a_capture_buffer_with_its_timestamp(self):
        out = self.run_program(self.client)
        self.assertEqual(out[8:], ["qbuf capture 0 again 0", "idle poll 0", "qbuf output 0", "poll 1 1 1",
                                   "dqbuf capture 0 index 0 stamp 7.42 fd 1", "dqbuf output 0 index 1",
                                   "empty -11", "closed -100", "other -2"])

    def test_other_programs_and_the_off_switch_are_left_alone(self):
        self.assertEqual(self.run_program(self.other), ["inactive"])
        self.assertEqual(self.run_program(self.client, BL_REMOTEPLAY_DECODER="0"), ["inactive"])


if __name__ == "__main__":
    unittest.main()
