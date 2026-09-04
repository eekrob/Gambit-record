#pragma once
#include "common/Media.hpp"
#include "video/D3DDevice.hpp"
#include <Windows.h>
#include <functional>
#include <string>

namespace evidence {
using FrameCallback = std::function<void(VideoFrame)>;
using CaptureErrorCallback = std::function<void(std::string)>;
class ICaptureBackend {
public:
  virtual ~ICaptureBackend() = default;
  virtual bool supported() const = 0;
  virtual void start(HWND hwnd, D3DDevice& device, FrameCallback frame, CaptureErrorCallback error) = 0;
  virtual void stop() noexcept = 0;
  virtual std::string name() const = 0;
};
}

