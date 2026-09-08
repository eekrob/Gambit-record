// Exercises the actual plugin render functions in a hidden D3D9 window, without GTA hooks.
#include "../src/Plugin.cpp"
#include <imgui_internal.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void hr(HRESULT result) { if (FAILED(result)) throw std::runtime_error("D3D/WIC failure " + std::to_string(result)); }
void snapshot(IDirect3DDevice9* device, const std::filesystem::path& path) {
  ComPtr<IDirect3DSurface9> source, staging;
  hr(device->GetRenderTarget(0, &source));
  D3DSURFACE_DESC desc{}; hr(source->GetDesc(&desc));
  hr(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr));
  hr(device->GetRenderTargetData(source.Get(), staging.Get()));
  D3DLOCKED_RECT rect{}; hr(staging->LockRect(&rect, nullptr, D3DLOCK_READONLY));
  ComPtr<IWICImagingFactory> factory;
  hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  ComPtr<IWICStream> stream; hr(factory->CreateStream(&stream)); hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder; hr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
  hr(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame; hr(encoder->CreateNewFrame(&frame, nullptr)); hr(frame->Initialize(nullptr));
  hr(frame->SetSize(desc.Width, desc.Height));
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR; hr(frame->SetPixelFormat(&format));
  require(format == GUID_WICPixelFormat24bppBGR, "Unexpected PNG pixel format");
  std::vector<BYTE> pixels(desc.Width * desc.Height * 3);
  for (UINT y = 0; y < desc.Height; ++y) {
    const auto* source_row = static_cast<const BYTE*>(rect.pBits) + y * rect.Pitch;
    for (UINT x = 0; x < desc.Width; ++x)
      std::memcpy(pixels.data() + (y * desc.Width + x) * 3, source_row + x * 4, 3);
  }
  hr(frame->WritePixels(desc.Height, desc.Width * 3, static_cast<UINT>(pixels.size()), pixels.data()));
  hr(frame->Commit()); hr(encoder->Commit()); hr(staging->UnlockRect());
}
}
int main() {
  try {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_module = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = g_module; wc.lpszClassName = L"GrecordUiTest";
    RegisterClassW(&wc);
    g_window = CreateWindowW(wc.lpszClassName, L"Gambit Record UI test", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr, nullptr, g_module, nullptr);
    require(g_window != nullptr, "Hidden window creation failed");
    ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    require(d3d != nullptr, "D3D9 unavailable");
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720; pp.hDeviceWindow = g_window;
    ComPtr<IDirect3DDevice9> device;
    hr(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device));
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1280, 720}; io.DeltaTime = 1.f / 60;
    g_fonts = grecord::ui::load_fonts(g_module); g_font_regular = g_fonts.regular; g_font_bold = g_fonts.bold;
    require(g_fonts.regular && g_fonts.bold && g_fonts.light && g_fonts.icons, "Embedded fonts missing");
    grecord::ui::apply_style(g_theme); ImGui_ImplDX9_Init(device.Get());
    const auto output = game_directory() / "ui-preview"; std::filesystem::create_directories(output);
    g_menu_open = true; g_worker_online = true; g_capture_ready = true; g_recording = true;
    g_logic.on_player_name(52, "Benjamin_Botsford"); g_logic.on_spectating_player(52, true);
    g_status = {{"recording_seconds", 83}, {"youtube_enabled", true}, {"audio_enabled", true}, {"archive_limit_gb", 20}, {"youtube_channel", "Gambit Record"}};
    auto frame = [&] {
      ImGui_ImplDX9_NewFrame(); ImGui::NewFrame();
      render_window(); render_hud(); render_prompt(); ImGui::Render();
      device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(35, 39, 45), 1, 0);
      hr(device->BeginScene()); ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); hr(device->EndScene());
    };
    auto frames = [&](int n = 45) { while (n--) frame(); };
    frames(); snapshot(device.Get(), output / "recording.png");
    auto* hud = ImGui::FindWindowByName("##grecord-hud");
    require(hud && std::abs(hud->Pos.x + hud->Size.x / 2 - 640) < 2 && std::abs(hud->Pos.y + hud->Size.y - 702) < 2, "HUD initial alignment failed");
    auto mouse = [&](float x, float y, bool down) {
      io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(ImGuiMouseButton_Left, down); frames(3);
    };
    const ImVec2 start = hud->Pos;
    mouse(start.x + 10, start.y + 10, false);
    mouse(start.x + 10, start.y + 10, true);
    mouse(100, 100, true); mouse(100, 100, false);
    require(g_ui.custom_position && hud->Pos.x < 110 && hud->Pos.y < 110, "HUD drag failed");
    std::string error;
    const auto saved = grecord::ui::load_settings(game_directory() / "grecord" / "ui.json", error);
    require(error.empty() && saved.custom_position, "Drag position not persisted");
    g_menu_open = false; frames();
    const auto anchor = g_ui.anchor;
    mouse(hud->Pos.x + 5, hud->Pos.y + 5, false); mouse(hud->Pos.x + 5, hud->Pos.y + 5, true);
    mouse(400, 100, true); mouse(400, 100, false);
    require(g_ui.anchor.x == anchor.x && g_ui.anchor.y == anchor.y && (hud->Flags & ImGuiWindowFlags_NoInputs), "Closed HUD captured input");
    g_recording = false; g_menu_open = true; frames();
    require(hud->Active, "Idle HUD preview missing");
    g_menu_open = false; frames(); require(!hud->Active, "Empty HUD visible during gameplay");
    g_recording = true;
    g_menu_open = true; g_ui = {}; frames();
    g_sidebar_expanded = true; frames(); snapshot(device.Get(), output / "navigation.png");
    g_sidebar_expanded = false; g_page = Page::settings; frames();
    snapshot(device.Get(), output / "settings.png");
    bool scrollable = false;
    for (auto* window : ImGui::GetCurrentContext()->Windows)
      if (std::string(window->Name).find("##content_") != std::string::npos && window->ParentWindow == ImGui::FindWindowByName("Gambit Record##main"))
        scrollable = window->ScrollMax.y > 0;
    require(scrollable, "Long settings are not scrollable");
    g_theme.surface = {0xfff5f5f5, 0xffe8e8e8}; g_theme.text = {0xff202020, 0xff555555};
    g_theme.overlay = {0xffdddddd, 0xffcccccc, 0xffaaaaaa}; g_theme.green = 0xff237a26; g_theme.red = 0xff3333bb; g_theme.yellow = 0xff006699;
    grecord::ui::apply_style(g_theme); g_page = Page::uploads; g_last_url = "https://youtu.be/ExampleVideo";
    g_uploading = true; g_upload_percent = 42;
    frames(); snapshot(device.Get(), output / "light-uploads.png");
    g_prompt = Prompt::finish; frames();
    const auto modal_anchor = g_ui.anchor;
    mouse(hud->Pos.x + 5, hud->Pos.y + 5, false); mouse(hud->Pos.x + 5, hud->Pos.y + 5, true);
    mouse(50, 50, true); mouse(50, 50, false);
    require(g_ui.anchor.x == modal_anchor.x && g_ui.anchor.y == modal_anchor.y && !g_hud_dragging, "Modal allowed HUD drag");
    snapshot(device.Get(), output / "dialog.png");
    g_dismiss_prompt = true; frames();
    require(g_prompt == Prompt::none && ImGui::GetCurrentContext()->OpenPopupStack.empty(), "Escape did not close the modal");
    io.DisplaySize = {640, 480}; g_ui.scale = 2; g_ui.custom_position = true; g_ui.anchor = {1, 1};
    g_notice = "Длинное уведомление: проверка переноса текста и положения статус-бара при изменении разрешения экрана.";
    g_notice_until = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    frames();
    require(hud->Pos.x >= 0 && hud->Pos.y >= 0 && hud->Pos.x + hud->Size.x <= 641 && hud->Pos.y + hud->Size.y <= 481, "HUD exceeded small display");
    snapshot(device.Get(), output / "small-screen.png");
    ImGui_ImplDX9_Shutdown(); ImGui::DestroyContext(); DestroyWindow(g_window); CoUninitialize();
    std::cout << "Embedded fonts, actual HUD drag, persistence, input gating, modal blocking and resolution tests passed\n";
    std::cout << "Screenshots: " << output.string() << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
