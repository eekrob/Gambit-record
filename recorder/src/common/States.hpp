#pragma once
#include <string_view>

namespace evidence {
enum class CaptureState { WaitingForGame, Starting, Running, Restarting, Error };
enum class RecordingState { Stopped, WaitingForKeyframe, Recording, Stopping, Error };
enum class ReplayState { Stopped, Starting, Running, Saving, Error };

constexpr std::string_view to_string(CaptureState v) {
  switch (v) { case CaptureState::WaitingForGame: return "waiting_for_game"; case CaptureState::Starting: return "starting"; case CaptureState::Running: return "running"; case CaptureState::Restarting: return "restarting"; case CaptureState::Error: return "error"; }
  return "error";
}
constexpr std::string_view to_string(RecordingState v) {
  switch (v) { case RecordingState::Stopped: return "stopped"; case RecordingState::WaitingForKeyframe: return "waiting_for_keyframe"; case RecordingState::Recording: return "recording"; case RecordingState::Stopping: return "stopping"; case RecordingState::Error: return "error"; }
  return "error";
}
constexpr std::string_view to_string(ReplayState v) {
  switch (v) { case ReplayState::Stopped: return "stopped"; case ReplayState::Starting: return "starting"; case ReplayState::Running: return "running"; case ReplayState::Saving: return "saving"; case ReplayState::Error: return "error"; }
  return "error";
}
}

