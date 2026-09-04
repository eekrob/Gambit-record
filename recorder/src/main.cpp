#include "app/Application.hpp"
#include "common/Files.hpp"
#include "config/Config.hpp"
#include "logging/Logger.hpp"
#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <iostream>
#include <optional>
#include <thread>
#include <winrt/base.h>

namespace {
std::atomic_bool g_stop{};

BOOL WINAPI console_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_SHUTDOWN_EVENT) {
    g_stop = true;
    return TRUE;
  }
  return FALSE;
}

bool process_running(DWORD pid) {
  if (!pid) return false;
  HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}

std::optional<DWORD> find_process(const std::wstring& executable) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::optional<DWORD> result;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, executable.c_str()) == 0) { result = entry.th32ProcessID; break; }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

struct Arguments { DWORD parent_pid{}; std::wstring pipe; };

Arguments parse_arguments(int argc, wchar_t** argv) {
  Arguments result;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view value(argv[i]);
    if (value == L"--parent-pid" && i + 1 < argc) result.parent_pid = std::wcstoul(argv[++i], nullptr, 10);
    else if (value == L"--pipe" && i + 1 < argc) result.pipe = argv[++i];
  }
  return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  auto arguments = parse_arguments(argc, argv);
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto base = evidence::executable_directory();
    auto config = evidence::Config::load_or_create(base / "grecord" / "config.json");
    config.resolve_paths(base);
    config.validate(base);
    evidence::Logger::instance().initialize(config.logging.directory, config.logging.level);
    SetConsoleCtrlHandler(console_handler, TRUE);

    if (!arguments.parent_pid) arguments.parent_pid = find_process(config.capture.process).value_or(0);
    if (!process_running(arguments.parent_pid)) {
      evidence::log_info("GAME_PROCESS_NOT_FOUND_EXITING");
      return 0;
    }
    if (arguments.pipe.empty()) arguments.pipe = LR"(\\.\pipe\GambitRecord-)" + std::to_wstring(arguments.parent_pid);

    const std::wstring mutex_name = L"Local\\GambitRecordWorker-" + std::to_wstring(arguments.parent_pid);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (!mutex) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 2; }

    evidence::Application app(std::move(config), std::move(arguments.pipe));
    app.initialize();
    std::jthread watcher([&](std::stop_token token) {
      while (!token.stop_requested() && !g_stop && process_running(arguments.parent_pid)) Sleep(1000);
      app.request_stop();
    });
    app.run();
    watcher.request_stop();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
  } catch (const std::exception& e) {
    std::cerr << "GambitRecord fatal error: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
