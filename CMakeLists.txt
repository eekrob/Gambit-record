cmake_minimum_required(VERSION 3.25)
project(GambitRecord VERSION 0.1.1 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Gambit Record worker supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(EVIDENCE_BUILD_TESTS "Build hardware-independent tests" ON)
set(GRECORD_BROKER_KEY "" CACHE STRING "Release-only shared broker key")

find_package(nlohmann_json CONFIG REQUIRED)

add_library(evidence_core
  recorder/src/config/Config.cpp
  recorder/src/logging/Logger.cpp
  recorder/src/common/Files.cpp
  recorder/src/replay/SegmentStore.cpp
  recorder/src/replay/ReplayBuffer.cpp
  recorder/src/recording/Mp4Muxer.cpp
  recorder/src/recording/RecordingSession.cpp
  recorder/src/ipc/IpcServer.cpp
  recorder/src/broker/BrokerUploader.cpp
  recorder/src/broker/UploadQueue.cpp
)
target_include_directories(evidence_core PUBLIC recorder/src)
target_compile_definitions(evidence_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 PRIVATE GRECORD_BROKER_KEY="${GRECORD_BROKER_KEY}")
target_compile_options(evidence_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(evidence_core PUBLIC
  nlohmann_json::nlohmann_json
  mfplat mfreadwrite mfuuid ole32 shlwapi bcrypt advapi32
  winhttp
)

add_executable(GambitRecord
  recorder/src/main.cpp
  recorder/src/app/Application.cpp
  recorder/src/capture/CaptureManager.cpp
  recorder/src/capture/WindowsGraphicsCapture.cpp
  recorder/src/capture/DesktopDuplication.cpp
  recorder/src/video/D3DDevice.cpp
  recorder/src/video/VideoProcessor.cpp
  recorder/src/video/MediaFoundationEncoder.cpp
  recorder/src/audio/WasapiCapture.cpp
)
target_include_directories(GambitRecord PRIVATE recorder/src)
target_compile_definitions(GambitRecord PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(GambitRecord PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(GambitRecord PRIVATE evidence_core
  d3d11 dxgi windowsapp avrt mmdevapi propsys wmcodecdspuuid
)
set_target_properties(GambitRecord PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(EVIDENCE_BUILD_TESTS)
  enable_testing()
  add_executable(evidence_tests
    recorder/tests/TestMain.cpp
    recorder/tests/ReplayTests.cpp
    recorder/tests/ConfigTests.cpp
    recorder/tests/StateTests.cpp
  )
  target_include_directories(evidence_tests PRIVATE recorder/src recorder/tests)
  target_compile_definitions(evidence_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_compile_options(evidence_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(evidence_tests PRIVATE evidence_core)
  add_test(NAME evidence_tests COMMAND evidence_tests)
endif()

install(TARGETS GambitRecord RUNTIME DESTINATION .)
install(FILES config.example.json DESTINATION grecord RENAME config.json)
