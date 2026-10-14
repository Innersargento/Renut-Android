// Windows host for the recompiled game with reNut's native Plume renderer.
//
// The boot sequence is the one src/platform/android/runtime_jni.cpp runs in its
// "launch" branch, without the Android app around it: the window is a plain
// Win32 window owned by the main thread, PlumeRenderer takes its HWND, the
// emulated Xenos only keeps the control flow (fences, vertical blank, swaps),
// and the game's draws are executed by Plume with the offline-recompiled shaders.
//
//   renut_plume.exe --game <folder with default.xex and Bundle> [--data <folder>]
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/audio/audio_system.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include "d3d/d3d_layer.h"
#include "d3d/native_d3d.h"
#include "plume/plume_gpu.h"
#include "plume/plume_renderer.h"

using rex::X_STATUS;
extern "C" uint64_t renut_guest_swap_count();  // frames the game presented (platform/windows/platform_windows.cpp)
extern const rex::PPCImageInfo PPCImageConfig;
extern PPCFuncMapping PPCFuncMappings[];

namespace {

std::atomic<bool> g_quit{false};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CLOSE:
      g_quit = true;
      return 0;
    case WM_DESTROY:
      g_quit = true;
      PostQuitMessage(0);
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateGameWindow(int width, int height) {
  std::puts("[trace] begin");
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = L"RenutPlumeWindow";
  std::puts("[trace] register");
  RegisterClassExW(&window_class);
  RECT rect{0, 0, width, height};
  std::puts("[trace] adjust");
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"reNut (Plume)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                                rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
  std::puts("[trace] created");
  if (window) ShowWindow(window, SW_SHOWNORMAL);
  std::puts("[trace] shown");
  return window;
}

// Names the guest code behind a fault in guest memory: the SDK only logs this on
// ARM64, and the guest registers are what identifies the function and its caller.
LONG CALLBACK GuestFaultLogger(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1) >= 4) return EXCEPTION_CONTINUE_SEARCH;
  auto* state = rex::runtime::ThreadState::Get();
  if (!state) return EXCEPTION_CONTINUE_SEARCH;
  const auto& g = *state->context();
  std::string line = "Falha de acesso em " + std::to_string(info->ExceptionRecord->ExceptionInformation[1]) +
                     " (host image offset=" + fmt::format("{:x}", reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) +
                     ") guest lr=" + std::to_string(uint32_t(g.lr)) + " r1=" + std::to_string(g.r1.u32) +
                     " r3=" + std::to_string(g.r3.u32) + " r4=" + std::to_string(g.r4.u32) + " r30=" + std::to_string(g.r30.u32) +
                     " r31=" + std::to_string(g.r31.u32);
  REXLOG_ERROR("{}", line);
  const PPCRegister* r[32] = {&g.r0,  &g.r1,  &g.r2,  &g.r3,  &g.r4,  &g.r5,  &g.r6,  &g.r7,
                              &g.r8,  &g.r9,  &g.r10, &g.r11, &g.r12, &g.r13, &g.r14, &g.r15,
                              &g.r16, &g.r17, &g.r18, &g.r19, &g.r20, &g.r21, &g.r22, &g.r23,
                              &g.r24, &g.r25, &g.r26, &g.r27, &g.r28, &g.r29, &g.r30, &g.r31};
  std::string regs = fmt::format("Guest regs: lr={:08X}", uint32_t(g.lr));
  for (int i = 0; i < 32; ++i) regs += fmt::format(" r{}={:08X}", i, r[i]->u32);
  REXLOG_ERROR("{}", regs);
  std::string stack = fmt::format("Guest stack at {:08X}:", g.r1.u32);
  for (uint32_t i = 0; i < 48; ++i) {
    const uint32_t address = g.r1.u32 + i * 4;
    stack += fmt::format(" {:08X}", rex::byte_swap(*state->memory()->TranslateVirtual<const uint32_t*>(address)));
  }
  REXLOG_ERROR("{}", stack);
  return EXCEPTION_CONTINUE_SEARCH;
}

// Writes the window's own contents (never the desktop) to a .bmp, for checking
// what the renderer shows without a screen to look at: --screenshot <file>
// --screenshot-after <seconds>.
bool SaveWindowBitmap(HWND window, const std::filesystem::path& path) {
  RECT rect{};
  GetClientRect(window, &rect);
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  HGDIOBJ old = SelectObject(memory, bitmap);
  // PW_CLIENTONLY | PW_RENDERFULLCONTENT: what the compositor holds for the window.
  const BOOL printed = PrintWindow(window, memory, 1 | 2);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  const int rows = GetDIBits(memory, bitmap, 0, UINT(height), pixels.data(), &info, DIB_RGB_COLORS);
  SelectObject(memory, old);
  DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (!printed || rows != height) {
    std::fprintf(stderr, "[renut] captura: PrintWindow=%d linhas=%d/%d erro=%lu\n", int(printed), rows, height,
                 GetLastError());
    return false;
  }
  BITMAPFILEHEADER header{};
  header.bfType = 0x4D42;
  header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  header.bfSize = DWORD(header.bfOffBits + pixels.size());
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
  file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
  return bool(file);
}

void Note(const std::string& message) {
  std::fprintf(stderr, "[renut] %s\n", message.c_str());
  std::fflush(stderr);
  REXLOG_INFO("Renut: {}", message);
}

void Usage() {
  std::fprintf(stderr,
               "Uso: renut_plume.exe --game <pasta com default.xex e Bundle> [--data <pasta de dados>]\n"
               "     [--config <renut.toml>] [--screenshot <arquivo.bmp> --screenshot-after <segundos>]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path game_dir, data_dir, config_file, screenshot;
  double screenshot_after = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto value = [&]() -> std::filesystem::path { return i + 1 < argc ? argv[++i] : ""; };
    if (argument == "--game") game_dir = value();
    else if (argument == "--data") data_dir = value();
    else if (argument == "--config") config_file = value();
    else if (argument == "--screenshot") screenshot = value();
    else if (argument == "--screenshot-after") screenshot_after = std::atof(value().string().c_str());
    else {
      Usage();
      return 2;
    }
  }
  if (game_dir.empty()) {
    Usage();
    return 2;
  }
  game_dir = std::filesystem::absolute(game_dir);
  if (!std::filesystem::is_regular_file(game_dir / "default.xex")) {
    std::fprintf(stderr, "default.xex não encontrado em %s\n", game_dir.string().c_str());
    return 1;
  }
  if (!std::filesystem::is_directory(game_dir / "Bundle")) {
    std::fprintf(stderr, "Pasta Bundle ausente em %s: extraia a ISO completa.\n", game_dir.string().c_str());
    return 1;
  }
  if (data_dir.empty()) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    data_dir = std::filesystem::path(exe).parent_path() / "renut-data";
  }
  data_dir = std::filesystem::absolute(data_dir);
  std::filesystem::create_directories(data_dir);
  if (config_file.empty() && std::filesystem::is_regular_file(data_dir / "renut.toml"))
    config_file = data_dir / "renut.toml";

  try {
    if (!config_file.empty()) rex::cvar::LoadConfig(config_file);
    const auto log_level = spdlog::level::from_str(rex::cvar::GetFlagByName("log_level"));
    rex::InitLogging((data_dir / "rexglue-runtime.log").string().c_str(), log_level);
    Note("Renderizador: Plume (Windows, Vulkan)");
    // MSAA off, as the Android default: the scene is drawn in one pass.
    if (rex::cvar::GetFlagByName("renut_scene_msaa").empty())
      rex::cvar::SetFlagByName("renut_scene_msaa", "false");

    // Unlike the console, nothing here maps the first guest page: a read of a null
    // object field (the game does that now and then) would fault the process.
    if (rex::cvar::GetFlagByName("protect_zero") == "true") rex::cvar::SetFlagByName("protect_zero", "false");

    size_t mappings = 0;
    while (PPCFuncMappings[mappings].host) ++mappings;
    Note("Funções compiladas: " + std::to_string(mappings));

    Note("Criando a janela...");
    AddVectoredExceptionHandler(1, GuestFaultLogger);
    HWND window = CreateGameWindow(1280, 720);
    if (!window) throw std::runtime_error("Não foi possível criar a janela.");

    Note("Criando o runtime...");
    rex::Runtime runtime(game_dir, data_dir / "runtime-user", {}, data_dir / "runtime-cache",
                         data_dir / "runtime-metadata");
    rex::RuntimeConfig config;
    config.tool_mode = false;

    renut::d3d::SetShaderDumpDirectory((data_dir / "shader-dump").string());
    renut::d3d::SetRuntimeFilesDirectory(data_dir.string());
    renut::native_d3d::SetFilesDirectory(data_dir.string());
    renut::ArmPlumeMemoryTrace(data_dir.string());

    Note("Iniciando o Plume...");
    std::string plume_error;
    if (!renut::PlumeRenderer::Start(reinterpret_cast<renut::PlumeWindow*>(window), &plume_error))
      throw std::runtime_error(plume_error);
    renut::d3d::RegisterPlumeHandlers();
    Note("Plume nativo: janela assumida; desenhos, resolves e swaps executados pelo Plume.");
    renut::SetPlumeMissDumpDirectory((data_dir / "plume-miss").string());
    renut::SetPlumePipelineCachePath((data_dir / "plume-pipelines.bin").string());

    config.graphics.reset(renut::CreatePlumeGraphicsSystem());
    if (!config.graphics) throw std::runtime_error("Backend gráfico indisponível.");
    config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
    config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
    config.kernel_init = rex::kernel::InitializeKernel;

    Note("Inicializando memória, kernel e tabela de funções...");
    auto status = runtime.Setup(PPCImageConfig, std::move(config));
    if (status != X_STATUS_SUCCESS) throw std::runtime_error("Setup falhou: " + std::to_string(status));
    Note("Runtime inicializado.");
    status = runtime.LoadXexImage("game:\\default.xex");
    if (status != X_STATUS_SUCCESS)
      throw std::runtime_error("Carregamento do XEX falhou: " + std::to_string(status));
    Note("XEX carregado.");

    runtime.graphics_system()->InitializeShaderStorage(data_dir / "runtime-cache",
                                                       runtime.kernel_state()->title_id(), false);
    Note("Iniciando o ponto de entrada do jogo.");
    auto thread = runtime.LaunchModule();
    if (!thread) throw std::runtime_error("Falha ao iniciar a thread principal do jogo.");

    const auto started = std::chrono::steady_clock::now();
    // The frame counter, in the title bar: frames the game presented, over the
    // last half second.
    auto counted_at = started;
    uint64_t counted = renut_guest_swap_count();
    MSG message;
    while (!g_quit) {
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      const auto now = std::chrono::steady_clock::now();
      if (!screenshot.empty() && now - started >= std::chrono::duration<double>(screenshot_after)) {
        Note(SaveWindowBitmap(window, screenshot) ? "Captura da janela: " + screenshot.string()
                                                  : "Captura da janela falhou.");
        screenshot.clear();
      }
      if (now - counted_at >= std::chrono::milliseconds(500)) {
        const uint64_t frames = renut_guest_swap_count();
        const double seconds = std::chrono::duration<double>(now - counted_at).count();
        wchar_t title[96];
        std::swprintf(title, sizeof(title) / sizeof(title[0]), L"reNut (Plume) - %.1f FPS - quadro %llu",
                      double(frames - counted) / seconds, static_cast<unsigned long long>(frames));
        SetWindowTextW(window, title);
        counted = frames;
        counted_at = now;
      }
      if (now - started > std::chrono::seconds(1) && !thread->is_running()) {
        Note("A thread principal do jogo terminou.");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (g_quit) Note("Janela fechada.");
    Note("Encerrando a sessão do jogo.");
    if (auto* plume = renut::PlumeRenderer::Get()) plume->ReleaseForShutdown();
    runtime.kernel_state()->TerminateTitle();
    runtime.Shutdown();
    if (auto* plume = renut::PlumeRenderer::Get()) {
      Note(plume->report());
      renut::PlumeRenderer::Stop();
    }
    DestroyWindow(window);
  } catch (const std::exception& error) {
    Note(std::string("Falha: ") + error.what());
    return 1;
  }
  return 0;
}
