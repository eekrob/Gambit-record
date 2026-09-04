#include "Test.hpp"
#include "common/States.hpp"
#include "recording/RecordingSession.hpp"

using namespace evidence;
TEST_CASE("recording start is idempotent") { RecordingSession session(std::filesystem::temp_directory_path()); StreamDescription d{1920, 1080, 60, 16000000}; REQUIRE(session.start({}, d)); REQUIRE(!session.start({}, d)); REQUIRE(session.state() == RecordingState::WaitingForKeyframe); session.stop(); REQUIRE(session.state() == RecordingState::Stopped); }
TEST_CASE("state names remain protocol stable") { REQUIRE(to_string(CaptureState::WaitingForGame) == "waiting_for_game"); REQUIRE(to_string(ReplayState::Saving) == "saving"); REQUIRE(to_string(RecordingState::WaitingForKeyframe) == "waiting_for_keyframe"); }
