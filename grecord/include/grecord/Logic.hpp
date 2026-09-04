#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace grecord {

enum class Action {
  none,
  open_settings,
  start_recording,
  stop_and_upload,
  stop_local,
  show_start_prompt,
  show_finish_prompt,
  show_missing_evidence
};

struct Punishment {
  std::string command;
  std::string reason;
  std::chrono::steady_clock::time_point sent_at;
};

class Logic {
public:
  explicit Logic(std::string admin_nickname = {});
  Action on_command(std::string_view command, bool recording,
                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  Action on_server_message(std::string_view utf8_text, bool recording,
                           std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  Action on_spectating_player(int id, bool recording);
  void on_player_name(int id, std::string utf8_nickname);
  void clear_spectating();
  void reset_session();
  void set_admin(std::string nickname) { admin_ = std::move(nickname); }

  int target_id() const noexcept { return target_id_; }
  const std::string& target_name() const noexcept { return target_name_; }
  const std::optional<Punishment>& pending_punishment() const noexcept { return pending_; }
  const std::string& confirmed_command() const noexcept { return confirmed_command_; }
  const std::string& confirmed_reason() const noexcept { return confirmed_reason_; }

  static bool is_punishment_command(std::string_view command);
  static std::string punishment_reason(std::string_view command);
  static bool confirms_punishment(std::string_view message, std::string_view admin);

private:
  std::string admin_;
  std::unordered_map<int, std::string> player_names_;
  std::optional<Punishment> pending_;
  int target_id_{-1};
  std::string target_name_;
  std::string confirmed_command_;
  std::string confirmed_reason_;
  bool spectate_prompt_shown_{};
};

std::string cp1251_to_utf8(std::string_view input);

} // namespace grecord
