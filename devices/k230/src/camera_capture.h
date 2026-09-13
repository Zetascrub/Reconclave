#pragma once

#include <cstdint>
#include <string>

namespace reconclave {

struct CaptureResult {
  bool ok = false;
  std::string path;
  std::uint64_t size_bytes = 0;
  std::string error;
};

// Captures a single still frame from `device` (a V4L2 capture node, e.g.
// /dev/video2 - the GC2093 sensor's ISP output on this board, confirmed via
// `v4l2-ctl --list-formats-ext` showing "camera: ok" and real image content
// on a test capture) and encodes it to JPEG at `output_path`.
//
// Implemented by shelling out to the on-device `ffmpeg` build (confirmed
// present, with --enable-libv4l2, on the shipped image) rather than writing
// our own V4L2 mmap/queue-buffer/encode loop: the ISP capture nodes only
// expose raw YUV pixel formats (NV16/NV12/YUYV, no MJPEG), and the actual
// hardware JPEG encoder is a *separate* memory-to-memory node
// (/dev/video0, the Linlon "mvx" codec) that takes a raw frame as input -
// ffmpeg's own v4l2 demuxer + software mjpeg encoder does this whole
// capture-and-encode pipeline in one bounded subprocess call instead of
// this project reimplementing a second V4L2 M2M client for the encoder.
//
// `execlp` (no shell) so `device`/`output_path` are never shell-interpreted
// even if a future caller's paths become less trusted, matching the
// execvp-only pattern already used for `iwlist` in wifi_scanner.cpp.
// Bounded by `timeout_ms`: a wedged camera/driver is killed and reported as
// a failure rather than left to block whichever caller invoked this (the
// touch UI's own event loop, currently).
CaptureResult captureStill(const std::string& device, const std::string& output_path,
                           int timeout_ms = 6000, bool enhance_preview = false);

}  // namespace reconclave
