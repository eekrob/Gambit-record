#include "Test.hpp"
#include "common/Files.hpp"
#include "config/Config.hpp"
#include "broker/BrokerUploader.hpp"

using namespace evidence;
TEST_CASE("filename sanitization removes Windows metacharacters") { REQUIRE(sanitize_filename("John:<Smith>?*") == "John__Smith___"); REQUIRE(sanitize_filename("name. ") == "name"); }
TEST_CASE("cache path validation rejects paths outside grecord") {
  auto base = std::filesystem::temp_directory_path() / "grecord_config_test"; std::filesystem::create_directories(base / "grecord/cache"); Config config; config.resolve_paths(base); config.validate(base); config.replay.cache_directory = base.parent_path() / "foreign-cache"; bool threw = false; try { config.validate(base); } catch (...) { threw = true; } REQUIRE(threw); std::filesystem::remove_all(base);
}
TEST_CASE("config validation catches invalid values") { auto base = std::filesystem::temp_directory_path() / "grecord_config_bad"; std::filesystem::create_directories(base / "grecord/cache"); Config c; c.resolve_paths(base); c.video.fps = 0; bool threw = false; try { c.validate(base); } catch (...) { threw = true; } REQUIRE(threw); std::filesystem::remove_all(base); }
TEST_CASE("YouTube title follows the Gambit Record format") { EvidenceMetadata m; m.admin = "Admin_Name"; m.timestamp = "2026-09-04 23-15-07"; REQUIRE(BrokerUploader::video_title(m) == "Admin_Name | 2026-09-04 | 23-15-07"); }
