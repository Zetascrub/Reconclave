#include "camera_capture.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

namespace reconclave {

CaptureResult captureStill(const std::string& device, const std::string& output_path,
                           int timeout_ms) {
  CaptureResult result;
  result.path = output_path;
  // ffmpeg's own -y overwrites, but remove any stale file first so a
  // failed capture can never be mistaken for a fresh one by a caller that
  // only checks "does the file exist".
  unlink(output_path.c_str());

  const pid_t pid = fork();
  if (pid < 0) {
    result.error = "fork() failed";
    return result;
  }
  if (pid == 0) {
    // Child: silence ffmpeg's own progress/banner chatter (not evidence,
    // just noise on this process's stdout/stderr) before exec. No shell
    // involved, so device/output_path are never shell-interpreted.
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    // -pix_fmt yuvj420p forces standard 4:2:0 chroma subsampling
    // (component sampling factors 0x22/0x11/0x11) on the encoded JPEG.
    // Without it, ffmpeg auto-negotiates a format matching this sensor's
    // native 4:2:2 raw output, which encodes with a Cb/Cr sampling factor
    // of 0x12 - valid JPEG, but LVGL's on-device decoder (TJpgDec, a
    // deliberately minimal embedded decoder - see tjpgd.c's jd_prepare())
    // only accepts Cb/Cr sampling factor 0x11 and returns JDR_FMT3
    // ("not supported JPEG standard") for anything else, which is exactly
    // what silently produced a black image in the UI before this was
    // narrowed down (via a temporary lv_image_decoder_get_info() probe,
    // see devices/k230/README.md's Vision section) to this one flag.
    execlp("ffmpeg", "ffmpeg", "-y", "-f", "v4l2", "-video_size", "1920x1080", "-i",
           device.c_str(), "-frames:v", "1", "-pix_fmt", "yuvj420p", output_path.c_str(),
           static_cast<char*>(nullptr));
    _exit(127);  // execlp only returns on failure.
  }

  // Bounded wait: poll for the child's exit rather than blocking on
  // waitpid() unconditionally, so a wedged camera/driver can never hang
  // whichever caller invoked this.
  constexpr int kPollIntervalMs = 100;
  int elapsed_ms = 0;
  int status = 0;
  bool exited = false;
  while (elapsed_ms < timeout_ms) {
    const pid_t outcome = waitpid(pid, &status, WNOHANG);
    if (outcome == pid) {
      exited = true;
      break;
    }
    if (outcome < 0) break;
    usleep(static_cast<useconds_t>(kPollIntervalMs) * 1000);
    elapsed_ms += kPollIntervalMs;
  }
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    result.error = "camera capture timed out";
    unlink(output_path.c_str());
    return result;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    result.error = "ffmpeg exited with an error";
    unlink(output_path.c_str());
    return result;
  }

  struct stat info {};
  if (stat(output_path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size == 0) {
    result.error = "capture produced no image";
    return result;
  }
  result.ok = true;
  result.size_bytes = static_cast<std::uint64_t>(info.st_size);
  return result;
}

}  // namespace reconclave
