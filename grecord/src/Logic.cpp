#include "grecord/Logic.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace grecord {
namespace {
std::string lower_ascii(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
  });
  return out;
}
std::string_view first_word(std::string_view input) {
  const auto pos = input.find_first_of(" \t\r\n");
  return input.substr(0, pos);
}
bool ends_with_admin(std::string_view message, std::string_view admin) {
  if (admin.empty()) return false;
  const std::string suffix = "// " + std::string(admin);
  return message.size() >= suffix.size() && message.substr(message.size() - suffix.size()) == suffix;
}
} // namespace

Logic::Logic(std::string admin_nickname) : admin_(std::move(admin_nickname)) {}

bool Logic::is_punishment_command(std::string_view command) {
  static constexpr std::array names{"/ban", "/mute", "/jail", "/bmute", "/accban"};
  const auto word = lower_ascii(first_word(command));
  return std::find(names.begin(), names.end(), word) != names.end();
}

std::string Logic::punishment_reason(std::string_view command) {
  // Gambit commands use: /command id [duration] reason.  Preserve the full
  // tail if the exact duration grammar changes server-side.
  auto cursor = command.find(' ');
  if (cursor == std::string_view::npos) return {};
  cursor = command.find_first_not_of(' ', cursor);
  cursor = command.find(' ', cursor);
  if (cursor == std::string_view::npos) return {};
  cursor = command.find_first_not_of(' ', cursor);
  if (cursor == std::string_view::npos) return {};
  cursor = command.find(' ', cursor);
  if (cursor == std::string_view::npos) return {};
  cursor = command.find_first_not_of(' ', cursor);
  return cursor == std::string_view::npos ? std::string{} : std::string(command.substr(cursor));
}

bool Logic::confirms_punishment(std::string_view message, std::string_view admin) {
  if (!message.starts_with("Администратор ")) return false;
  static constexpr std::array phrases{
      "заблокировал", "заблокировала", "выдал мут", "выдала мут", "заглушил",
      "заглушила", "посадил", "посадила", "заблокировал OOC", "заблокировал IC"};
  const bool action = std::any_of(phrases.begin(), phrases.end(),
      [&](std::string_view phrase) { return message.find(phrase) != std::string_view::npos; });
  if (!action) return false;
  const std::string direct = "Администратор " + std::string(admin) + " ";
  return (!admin.empty() && message.starts_with(direct)) || ends_with_admin(message, admin);
}

Action Logic::on_command(std::string_view command, bool recording,
                         std::chrono::steady_clock::time_point now) {
  const auto word = lower_ascii(first_word(command));
  if (word == "/grecord" || word == "/esettings") return Action::open_settings;
  if (word == "/estart") return Action::start_recording;
  if (word == "/estop") return recording ? Action::stop_local : Action::none;
  if (is_punishment_command(command)) {
    pending_ = Punishment{std::string(command), punishment_reason(command), now};
  }
  return Action::none;
}

Action Logic::on_server_message(std::string_view text, bool recording,
                                std::chrono::steady_clock::time_point now) {
  if (!pending_) return Action::none;
  if (now - pending_->sent_at > std::chrono::seconds(30)) { pending_.reset(); return Action::none; }
  if (!confirms_punishment(text, admin_)) return Action::none;
  confirmed_command_ = pending_->command;
  confirmed_reason_ = pending_->reason;
  pending_.reset();
  return recording ? Action::show_finish_prompt : Action::show_missing_evidence;
}

Action Logic::on_spectating_player(int id, bool recording) {
  target_id_ = id;
  if (const auto it = player_names_.find(id); it != player_names_.end()) target_name_ = it->second;
  else target_name_ = "ID " + std::to_string(id);
  if (!recording && !spectate_prompt_shown_) { spectate_prompt_shown_ = true; return Action::show_start_prompt; }
  return Action::none;
}

void Logic::on_player_name(int id, std::string nickname) {
  player_names_[id] = std::move(nickname);
  if (id == target_id_) target_name_ = player_names_[id];
}
void Logic::clear_spectating() { target_id_ = -1; target_name_.clear(); }
void Logic::reset_session() { pending_.reset(); target_id_ = -1; target_name_.clear(); confirmed_command_.clear(); confirmed_reason_.clear(); spectate_prompt_shown_ = false; }

} // namespace grecord
