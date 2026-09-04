#include "grecord/Logic.hpp"
#include <cassert>
#include <chrono>

using namespace grecord;
int main() {
  assert(cp1251_to_utf8(std::string("\xC7\xE0\xEF\xE8\xF1\xFC")) == "Запись");
  assert(Logic::is_punishment_command("/ban 52 3 5pp"));
  assert(Logic::is_punishment_command("/BMUTE 3 30 flood"));
  assert(!Logic::is_punishment_command("/banana"));
  assert(Logic::punishment_reason("/ban 52 3 5pp/a") == "5pp/a");

  Logic logic("omsky");
  logic.on_player_name(52, "Benjamin_Botsford");
  assert(logic.on_spectating_player(52, false) == Action::show_start_prompt);
  assert(logic.target_name() == "Benjamin_Botsford");
  assert(logic.on_spectating_player(52, false) == Action::none);
  const auto now = std::chrono::steady_clock::now();
  logic.on_command("/ban 52 3 5pp/a", true, now);
  assert(logic.on_server_message("Администратор dalevi заблокировал игрока Benjamin_Botsford на 3 дней. Причина: 5pp/a // omsky", true, now + std::chrono::seconds(1)) == Action::show_finish_prompt);
  assert(logic.confirmed_command() == "/ban 52 3 5pp/a");

  logic.on_command("/mute 3 30 spam", false, now);
  assert(logic.on_server_message("Администратор omsky заглушил игрока Test", false, now + std::chrono::seconds(2)) == Action::show_missing_evidence);
  logic.on_command("/jail 3 30 dm", true, now);
  assert(logic.on_server_message("Администратор omsky посадил игрока Test", true, now + std::chrono::seconds(31)) == Action::none);
  assert(logic.on_command("/grecord", false) == Action::open_settings);
  assert(logic.on_command("/estart", false) == Action::start_recording);
  return 0;
}
