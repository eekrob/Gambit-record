cmake_minimum_required(VERSION 3.25)
project(grecord VERSION 0.1.5 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "grecord.asi supports Windows only")
endif()
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "grecord.asi must be configured with a 32-bit toolchain (-A Win32 or i686 MinGW)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_SHARED_LIBRARY_PREFIX "")
include(FetchContent)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG fbcf95193f40fc57a3f0a3e8f59798de06e69bc6
)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG d33ecd3f3bd11e30aa8bbabb00e0a9cd3f2456d8
)
FetchContent_MakeAvailable(imgui minhook json)

configure_file(resources/fonts.rc.in ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc @ONLY)
add_library(grecord_ui_config STATIC src/UiConfig.cpp)
target_include_directories(grecord_ui_config PUBLIC include)
target_compile_definitions(grecord_ui_config PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(grecord_ui_config PUBLIC nlohmann_json::nlohmann_json)

add_library(grecord_core STATIC src/Logic.cpp src/Encoding.cpp src/BitReader.cpp)
target_include_directories(grecord_core PUBLIC include)
target_compile_definitions(grecord_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(grecord_render STATIC
  src/UiStyle.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(grecord_render PUBLIC include ${imgui_SOURCE_DIR})
target_compile_definitions(grecord_render PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(grecord_render PUBLIC d3d9 dwmapi)
if(MSVC)
  target_compile_options(grecord_render PRIVATE /W4 /EHsc /utf-8)
endif()

add_library(grecord SHARED src/Plugin.cpp src/IpcClient.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
target_include_directories(grecord PRIVATE include ${imgui_SOURCE_DIR})
target_compile_definitions(grecord PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(grecord PRIVATE grecord_core grecord_ui_config grecord_render minhook nlohmann_json::nlohmann_json shell32)
set_target_properties(grecord PROPERTIES SUFFIX ".asi" OUTPUT_NAME "grecord")

if(MSVC)
  set_property(TARGET grecord grecord_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(grecord PRIVATE /W4 /EHsc /utf-8)
  target_compile_options(grecord_core PRIVATE /W4 /EHsc /utf-8)
  target_compile_options(grecord_ui_config PRIVATE /W4 /EHsc /utf-8)
else()
  target_compile_options(grecord PRIVATE -Wall -Wextra -Wpedantic)
  # ASI plugins are loaded directly by gta_sa.exe. Ship no MinGW runtime DLLs:
  # users must not need libgcc, libstdc++ or libwinpthread beside the game.
  target_link_options(grecord PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
option(GRECORD_UI_RENDER_TESTS "Build hidden-window D3D9 UI tests and screenshots (requires a desktop GPU)" OFF)
if(BUILD_TESTING)
  add_executable(grecord_logic_tests tests/LogicTests.cpp)
  target_link_libraries(grecord_logic_tests PRIVATE grecord_core)
  add_test(NAME grecord_logic_tests COMMAND grecord_logic_tests)
  add_executable(grecord_bitreader_tests tests/BitReaderTests.cpp)
  target_link_libraries(grecord_bitreader_tests PRIVATE grecord_core)
  add_test(NAME grecord_bitreader_tests COMMAND grecord_bitreader_tests)
  add_executable(grecord_ui_tests tests/UiTests.cpp)
  target_link_libraries(grecord_ui_tests PRIVATE grecord_ui_config)
  if(MSVC)
    target_compile_options(grecord_ui_tests PRIVATE /W4 /EHsc /utf-8)
  endif()
  add_test(NAME grecord_ui_tests COMMAND grecord_ui_tests)
  if(GRECORD_UI_RENDER_TESTS)
    add_executable(grecord_ui_render_tests tests/UiRenderTests.cpp src/IpcClient.cpp ${CMAKE_CURRENT_BINARY_DIR}/fonts.rc)
    target_link_libraries(grecord_ui_render_tests PRIVATE grecord_core grecord_ui_config grecord_render minhook shell32 ole32 windowscodecs)
    if(MSVC)
      target_compile_options(grecord_ui_render_tests PRIVATE /W4 /EHsc /utf-8)
    endif()
    add_test(NAME grecord_ui_render_tests COMMAND grecord_ui_render_tests)
  endif()
endif()
