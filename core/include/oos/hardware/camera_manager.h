#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace oos::hardware {

enum class LensFacing {
  Unknown,
  Front,
  Back,
  External,
};

struct CameraInfo {
  std::string id;
  LensFacing facing = LensFacing::Unknown;
  int sensor_orientation = 0;
  int hardware_level = -1;
  bool flash_available = false;
  int max_jpeg_width = 0;
  int max_jpeg_height = 0;
};

struct PhotoResult {
  std::string path;
  int width = 0;
  int height = 0;
  size_t byte_count = 0;
};

class CameraManager {
public:
  CameraManager();
  ~CameraManager();

  CameraManager(const CameraManager &) = delete;
  CameraManager &operator=(const CameraManager &) = delete;

  bool initialize();
  void shutdown();
  bool initialized() const;

  bool enumerate(std::vector<CameraInfo> &cameras);
  bool setTorch(const std::string &camera_id, bool enabled);
  bool captureJpeg(const std::string &camera_id, const std::string &path,
                   PhotoResult &result, int max_width = 1920,
                   int max_height = 1080, bool flash = false,
                   int timeout_ms = 15000);

  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

const char *lensFacingName(LensFacing facing);
const char *cameraHardwareLevelName(int level);

} // namespace oos::hardware
