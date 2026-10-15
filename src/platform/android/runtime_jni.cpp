#include <jni.h>
#include <unistd.h>
#include <rex/runtime.h>
#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/system/function_dispatcher.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>
#include <android/log.h>
#include <fstream>
#include <functional>
#include <stdlib.h>
#include <sstream>
#include <thread>
#include <atomic>
#include <android/native_window_jni.h>
#include <rex/graphics/xenos.h>
#include "d3d/native_d3d.h"
#include "plume_graphics.h"
#include "android_game_window.h"
#include "touch_input.h"
#include "d3d/d3d_layer.h"
#include "plume/plume_renderer.h"
#include "plume/plume_gpu.h"
#include "plume/frame_pacing.h"
#include <rex/chrono/clock.h>
#include <rex/ui/presenter.h>
#include <rex/audio/audio_system.h>
#include "renut_audio.h"
#include <rex/system/xthread.h>
#include <rex/input/input_system.h>
#include "renut_android_init.h"
#include "adpf.h"
#include "guest/host_support.h"
using rex::X_STATUS;
extern const rex::PPCImageInfo PPCImageConfig;
void renutSetContentActivity(JNIEnv*, jobject);
std::string renutCheatsSummary();
namespace renut {
void AchievementsStart(JNIEnv*, jobject, rex::Runtime&, const std::filesystem::path&);
void AchievementsStop(rex::Runtime&);
void TextInputStart(JNIEnv*, jobject);
void TextInputStop();
}
void renutConfigureXenosDriver(const std::string&,const std::string&,const std::string&,bool,const std::string&);
// Own this entry point so the loader cannot resolve SDL's JNI_OnLoad through
// our transitive dependencies. SDL is explicitly loaded/initialized by Java.
// Android drops a native process's stderr unless a system property asks for it,
// which hides Plume's own error lines (failed shader modules and pipelines).
// Forward it to logcat instead.
static void ForwardStderrToLogcat() {
  int fds[2];
  if (pipe(fds) != 0) return;
  if (dup2(fds[1], STDERR_FILENO) < 0) { close(fds[0]); close(fds[1]); return; }
  close(fds[1]);
  std::thread([fd = fds[0]] {
    std::string line;
    char buffer[512];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof buffer)) > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') {
          __android_log_write(ANDROID_LOG_ERROR, "renut-stderr", line.c_str());
          line.clear();
        } else line.push_back(buffer[i]);
      }
    }
  }).detach();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
  ForwardStderrToLogcat();
  return JNI_VERSION_1_6;
}
static std::atomic<bool> render_stopped{false};
// The window the game loop pumps, reachable from the UI thread for surface changes.
static std::mutex game_window_mutex;
static renut::GameWindow* live_game_window = nullptr;
// The player's pause reaches everything the game runs. Guest threads park at their
// next kernel wait (renut::SetTitlePaused), the renderer parks at its next
// present, the guest clock stops crawling, and the audio mixer and the XMA decoder
// stop. The audio part can wait on a worker that is inside a title callback, so
// it runs on its own thread and never on the UI thread; `pause_wanted` is what the
// player last asked, so a quick pause-resume-pause settles on the last one.
static std::mutex audio_mutex;                 // keeps g_audio valid against Shutdown
static rex::audio::AudioSystem* g_audio = nullptr;
static std::mutex pause_op_mutex;
static std::atomic<bool> pause_wanted{false};
static void ApplyAudioPause() {
  std::lock_guard<std::mutex> op(pause_op_mutex);
  std::lock_guard<std::mutex> lock(audio_mutex);
  if (!g_audio) return;
  if (pause_wanted.load()) g_audio->Pause(); else g_audio->Resume();
}
// Releases every pause before the game stops, so no thread is left parked.
static void ReleasePause() {
  pause_wanted = false;
  renut::SetTitlePaused(false);
  if(auto* plume=renut::PlumeRenderer::Get())plume->SetPaused(false);
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeStop(JNIEnv*,jclass) {
  render_stopped=true;
  ReleasePause();
  // A game paused in the background is parked in the renderer: wake it to shut down.
  if(auto* plume=renut::PlumeRenderer::Get())plume->ReleaseForShutdown();
}
// The app left the screen: its surface is gone. The game keeps its state and
// stops at its next present, and its clock crawls so that coming back is not a
// jump of minutes for the physics.
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeBackground(JNIEnv*,jclass) {
  renut::d3d::SetBackground(true);
  renut::adpf::SetActive(false);
  if(auto* plume=renut::PlumeRenderer::Get())plume->PauseForBackground();
  rex::chrono::Clock::set_guest_time_scalar(0.02);
  {
    std::lock_guard<std::mutex> lock(game_window_mutex);
    if(live_game_window)live_game_window->SetWindow(nullptr);
  }
}
// A new surface: swapchain replaced, the game runs again.
extern "C" JNIEXPORT jboolean JNICALL Java_org_renut_android_RuntimeActivity_nativeForeground(JNIEnv* env,jclass,jobject surface) {
  auto* window=surface?ANativeWindow_fromSurface(env,surface):nullptr;
  if(!window)return JNI_FALSE;
  {
    std::lock_guard<std::mutex> lock(game_window_mutex);
    if(live_game_window)live_game_window->SetWindow(window);
  }
  bool attached=false;
  if(auto* plume=renut::PlumeRenderer::Get())attached=plume->AttachWindow(window);
  ANativeWindow_release(window);
  if(attached){
    rex::chrono::Clock::set_guest_time_scalar(1.0);
    renut::d3d::SetBackground(false);
    renut::adpf::SetActive(!pause_wanted.load());
  }
  return attached?JNI_TRUE:JNI_FALSE;
}
// The pause the player asks for from the game menu. The surface stays, so the last
// frame is still shown. False while there is no renderer yet (the game has not
// started drawing): nothing was paused.
extern "C" JNIEXPORT jboolean JNICALL Java_org_renut_android_RuntimeActivity_nativeSetPaused(JNIEnv*,jclass,jboolean paused) {
  auto* plume=renut::PlumeRenderer::Get();
  if(!plume)return JNI_FALSE;
  pause_wanted=paused;
  renut::adpf::SetActive(!paused);
  renut::SetTitlePaused(paused);
  plume->SetPaused(paused);
  renut::d3d::SetBackground(paused);
  rex::chrono::Clock::set_guest_time_scalar(paused?0.02:1.0);
  std::thread(ApplyAudioPause).detach();
  return JNI_TRUE;
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeButton(JNIEnv*,jclass,jint button,jboolean down) {
  renut::SetTouchButton(uint16_t(button),down);
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeStick(JNIEnv*,jclass,jint stick,jfloat x,jfloat y) {
  renut::SetTouchStick(stick,x,y);
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeTrigger(JNIEnv*,jclass,jint trigger,jfloat value) {
  renut::SetTouchTrigger(trigger,value);
}
// The Android gamepads: player 0 shares the guest's first pad with the touch controller, player
// 1 is the second guest user (local multiplayer).
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeExternalButton(JNIEnv*,jclass,jint player,jint button,jboolean down) {
  renut::SetExternalButton(player,uint16_t(button),down);
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeExternalStick(JNIEnv*,jclass,jint player,jint stick,jfloat x,jfloat y) {
  renut::SetExternalStick(player,stick,x,y);
}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeExternalTrigger(JNIEnv*,jclass,jint player,jint trigger,jfloat value) {
  renut::SetExternalTrigger(player,trigger,value);
}
// The rumble the game asked for, both players: see rumble_packed in touch_input.h.
extern "C" JNIEXPORT jlong JNICALL Java_org_renut_android_RuntimeActivity_nativeRumble(JNIEnv*,jclass) {
  return jlong(renut::rumble_packed.load());
}
// A gamepad is (or is no longer) assigned to player 1. The game notices on its next poll and is
// told through the sign-in notification.
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeSetPlayerAttached(JNIEnv*,jclass,jint player,jboolean attached) {
  renut::SetPlayerAttached(player,attached);
}
// The frame rate the player picked in the sidebar (30..240, 0 unlimited), taken up at
// the game's next frame: the swap stops or starts waiting for the vblank, the command
// thread's pacer and the swapchain's present mode follow (plume/frame_pacing.h).
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeSetFpsTarget(JNIEnv*,jclass,jint fps) {
  renut::pacing::SetTarget(fps);
  renut::adpf::SetTargetFps(renut::pacing::Target());
}
extern "C" uint64_t renut_guest_swap_count();
extern "C" JNIEXPORT jlong JNICALL Java_org_renut_android_RuntimeActivity_nativeFrameCount(JNIEnv*,jclass) {
  return jlong(renut_guest_swap_count());
}
// The pipelines built ahead at start: done in bits 0-23, total in 24-47, bit 48
// while they are being built, bit 49 while a scene load waits for them.
extern "C" JNIEXPORT jlong JNICALL Java_org_renut_android_RuntimeActivity_nativePrecompileProgress(JNIEnv*,jclass) {
  const renut::PlumePrecompileProgress p=renut::GetPlumePrecompileProgress();
  return jlong(uint64_t(p.done&0xFFFFFF)|uint64_t(p.total&0xFFFFFF)<<24|uint64_t(p.running)<<48|uint64_t(p.waiting)<<49);
}
static std::string javaString(JNIEnv* env,jstring value){
  if(!value)return {};const char* chars=env->GetStringUTFChars(value,nullptr);
  if(!chars)throw std::runtime_error("String JNI indisponível.");
  std::string result(chars);env->ReleaseStringUTFChars(value,chars);return result;
}

static void validateSwap(rex::Runtime& runtime,renut::PlumeGraphics& graphics) {
  namespace xenos=rex::graphics::xenos;
  auto* memory=runtime.memory();
  const uint32_t width=320,height=180,pitch=320;
  REXSYS_INFO("Plume test: allocating guest framebuffer");
  uint32_t pixels=memory->SystemHeapAlloc(pitch*height*4,4096,rex::memory::kSystemHeapPhysical);
  uint32_t parameters=memory->SystemHeapAlloc(1024,32);
  if(!pixels||!parameters)throw std::runtime_error("Memória do teste gráfico indisponível.");
  struct Free {rex::memory::Memory* memory;uint32_t a,b;~Free(){memory->SystemHeapFree(a);memory->SystemHeapFree(b);}} free{memory,pixels,parameters};
  rex::runtime::ThreadState thread(UINT32_MAX,parameters+512,0,memory);
  rex::runtime::ThreadState::Bind(&thread);
  auto& ctx=*thread.context();
  auto store=[&](uint32_t address,uint32_t value){rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(address),value);};
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type=xenos::FetchConstantType::kTexture;
  fetch.format=xenos::TextureFormat::k_8_8_8_8;fetch.pitch=pitch>>5;
  fetch.base_address=pixels>>12;fetch.dimension=xenos::DataDimension::k2DOrStacked;
  fetch.size_2d.width=width-1;fetch.size_2d.height=height-1;
  fetch.swizzle=0|(1<<3)|(2<<6)|(3<<9);
  // Invalid guest descriptors must fail before a GPU submission or unsafe read.
  auto invalid=fetch;invalid.base_address=memory->GetPhysicalAddress(pixels)>>12;
  auto rejected=[&](const xenos::xe_gpu_texture_fetch_t& value,const char* expected){
    try {graphics.PresentFrontBuffer(&value.dword_0,width,height);}
    catch(const std::runtime_error& e){if(std::string(e.what()).find(expected)!=std::string::npos)return;throw;}
    throw std::runtime_error("Descriptor inválido foi aceito pelo renderer.");
  };
  auto badFormat=invalid;badFormat.format=static_cast<xenos::TextureFormat>(63);
  rejected(badFormat,"formato");
  auto badPitch=invalid;badPitch.pitch=0;rejected(badPitch,"Pitch");
  auto badAddress=invalid;badAddress.base_address=0x1FFFF;rejected(badAddress,"memória física");
  REXSYS_INFO("Plume test: invalid format, pitch and address rejected");
  for(unsigned i=0;i<6;++i)store(parameters+256+i*4,(&fetch.dword_0)[i]);
  store(parameters+288,pixels);store(parameters+292,uint32_t(fetch.format));store(parameters+296,0);
  store(parameters+300,width);store(parameters+304,height);
  auto* bytes=memory->TranslateVirtual(pixels);
  REXSYS_INFO("Plume test: framebuffer {:08X}, parameters {:08X}",pixels,parameters);
  for(unsigned frame=0;frame<180&&!render_stopped;++frame){
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
      auto offset=(y*pitch+x)*4;
      bytes[offset]=uint8_t(x*255/width);bytes[offset+1]=uint8_t(y*255/height);
      bytes[offset+2]=((x+frame*3)/20+y/20)%2?220:30;bytes[offset+3]=255;
    }
    // Exercise the same imported Xbox kernel ABI called by the recompiled game.
    ctx.r3.u64=parameters;ctx.r4.u64=parameters+256;
    ctx.r5.u64=ctx.r6.u64=ctx.r7.u64=parameters+320;
    ctx.r8.u64=parameters+288;ctx.r9.u64=parameters+292;ctx.r10.u64=parameters+296;
    store(ctx.r1.u32+0x54,parameters+300);store(ctx.r1.u32+0x5C,parameters+304);
    if(frame==0)REXSYS_INFO("Plume test: calling VdSwap");
    __imp__VdSwap(ctx,runtime.virtual_membase());
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  if(render_stopped)throw std::runtime_error("Teste gráfico interrompido.");
}

static void checkpoint(const std::filesystem::path& root, const std::string& message) {
  __android_log_print(ANDROID_LOG_INFO, "reNutRuntime", "%s", message.c_str());
  std::ofstream(root / "runtime-report.txt", std::ios::app) << message << '\n';
}

static void validateFibers() {
  // Guard the ABI assumptions used by libucontext's AArch64 assembly.
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, regs) == 184);
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, sp) == 432);
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, pc) == 440);
  using rex::thread::Fiber;
  struct State { Fiber* parent; int value; } state{Fiber::ConvertCurrentThread(), 0};
  if (!state.parent) throw std::runtime_error("Falha ao criar contexto principal.");
  auto* child = Fiber::Create(128 * 1024, [](void* argument) {
    auto& s = *static_cast<State*>(argument);
    s.value = 17;
    Fiber::SwitchTo(s.parent);
    s.value = 29;
    Fiber::SwitchTo(s.parent);
    std::terminate();
  }, &state);
  if (!child) { state.parent->Destroy(); throw std::runtime_error("Falha ao criar fiber."); }
  Fiber::SwitchTo(child);
  bool first = state.value == 17;
  Fiber::SwitchTo(child);
  bool second = state.value == 29;
  child->Destroy(); state.parent->Destroy();
  if (!first || !second) throw std::runtime_error("Troca de contexto ARM64 divergente.");
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_renut_android_RuntimeActivity_nativeValidate(JNIEnv* env, jobject activity, jstring directory,
    jobject surface,jstring hooks,jstring driverDirectory,jstring driverLibrary,jboolean launch,
    jboolean oneUiFix,jboolean sceneMsaa,jstring tuDebug,jstring ir3Debug) {
  const char* chars = env->GetStringUTFChars(directory, nullptr);
  if (!chars) return nullptr;
  std::filesystem::path root(chars);
  env->ReleaseStringUTFChars(directory, chars);
  renutSetContentActivity(env, activity);
  std::ofstream(root / "runtime-report.txt", std::ios::trunc).close();
  auto note = [&](const std::string& s) { checkpoint(root, s); };
  try {
    // /data/user/0 and /data/data may name the same directory on Android.
    root = std::filesystem::canonical(root);
    auto log_path = (root / "rexglue-runtime.log").string();
    // Optional cvar overrides for on-device experiments, pushed with
    // `adb shell run-as org.renut.android` into files/renut.toml. Loaded before
    // logging so that log_level applies from the first line.
    bool has_config = std::filesystem::is_regular_file(root / "renut.toml");
    if (has_config) rex::cvar::LoadConfig(root / "renut.toml");
    renut::pacing::ApplyConfiguredTarget();
    auto log_level = spdlog::level::from_str(rex::cvar::GetFlagByName("log_level"));
    rex::InitLogging(log_path.c_str(), log_level);
    if (has_config) note("Configuração: renut.toml aplicado.");
    if (launch) {
      const auto cheats = renutCheatsSummary();
      note(cheats.empty() ? std::string("Cheats: desligados") : "Cheats: " + cheats);
    }
    note("Renderizador: Plume");
    // So does its MSAA choice.
    rex::cvar::SetFlagByName("renut_scene_msaa", sceneMsaa ? "true" : "false");
    note(std::string("MSAA: ") + (sceneMsaa ? "ativado" : "desativado"));
    note("RexGlue 0.10.0 / ARM64: biblioteca carregada.");
    size_t mappings = 0;
    while (PPCFuncMappings[mappings].host) ++mappings;
    note("Funções compiladas: " + std::to_string(mappings));
    rex::memory::AndroidInitialize();
    rex::thread::AndroidInitialize();
    validateFibers();
    note("Fibers ARM64: suspensão e retomada verificadas.");
    std::string active;
    std::ifstream(root / "active-game.txt") >> active;
    auto game = std::filesystem::weakly_canonical(root / active);
    if (active.empty() || game.string().find(root.string() + "/") != 0 ||
        !std::filesystem::is_regular_file(game / "default.xex"))
      throw std::runtime_error("Importe o XEX europeu antes de validar.");
    std::unique_ptr<renut::GameContext> game_context;
    std::unique_ptr<renut::GameWindow> game_window;
    rex::Runtime runtime(game, root / "runtime-user", {}, root / "runtime-cache", root / "runtime-metadata");
    rex::RuntimeConfig config;
    config.tool_mode = surface==nullptr;
    renut::PlumeGraphics* graphics=nullptr;
    if(launch){
      if(!surface)throw std::runtime_error("Superfície indisponível para o jogo.");
      if(!std::filesystem::is_directory(game / "Bundle"))
        throw std::runtime_error("Importe a ISO completa: os assets Bundle não estão presentes.");
      // Debug: <files>/plume-env holds KEY=VALUE lines put in the environment
      // before the driver loads, to try Turnip and ir3 flags (TU_DEBUG,
      // IR3_SHADER_DEBUG, FD_DEV_FEATURES...) without a build. The pipeline
      // cache is kept apart per content, so binaries compiled under other flags
      // are not handed back.
      std::string plume_env;
      {
        std::ifstream env_file(root / "plume-env");
        for(std::string line;std::getline(env_file,line);){
          while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.pop_back();
          const auto equals=line.find('=');
          if(line.empty()||line[0]=='#'||equals==std::string::npos||equals==0)continue;
          setenv(line.substr(0,equals).c_str(),line.substr(equals+1).c_str(),1);
          note("Ambiente: "+line);
          plume_env+=line+"\n";
        }
      }
      // The app selection, including an empty selection, takes precedence.
      // Include it in the pipeline cache key because IR3 changes shader code.
      const auto ir3=javaString(env,ir3Debug);
      setenv("IR3_SHADER_DEBUG",ir3.c_str(),1);
      note("IR3_SHADER_DEBUG: "+(ir3.empty()?std::string("desativado"):ir3));
      plume_env+="IR3_SHADER_DEBUG="+ir3+"\n";
      renutConfigureXenosDriver(javaString(env,hooks),javaString(env,driverDirectory),javaString(env,driverLibrary),oneUiFix,
                                javaString(env,tuDebug));
      note("Driver solicitado: "+(javaString(env,driverLibrary).empty()?std::string("sistema"):javaString(env,driverLibrary)));
      if(!javaString(env,tuDebug).empty())note("TU_DEBUG: "+javaString(env,tuDebug));
      auto* window=ANativeWindow_fromSurface(env,surface);
      if(!window)throw std::runtime_error("Janela Android indisponível.");
      std::unique_ptr<ANativeWindow,decltype(&ANativeWindow_release)> owned(window,ANativeWindow_release);
      game_context=std::make_unique<renut::GameContext>();
      game_window=std::make_unique<renut::GameWindow>(*game_context,window);
      if(!game_window->Open())throw std::runtime_error("Falha ao abrir a superfície do jogo.");
      {
        std::lock_guard<std::mutex> lock(game_window_mutex);
        live_game_window=game_window.get();
      }
      renut::d3d::SetShaderDumpDirectory((root / "shader-dump").string());
      renut::d3d::SetRuntimeFilesDirectory(root.string());
      renut::native_d3d::SetFilesDirectory(root.string());
      // The emulated Xenos keeps fences, vertical blank and swaps but does not
      // present: the native Plume renderer owns the window.
      std::string plume_error;
      renut::ArmPlumeMemoryTrace(root.string());
      if(!renut::PlumeRenderer::Start(window,&plume_error))throw std::runtime_error(plume_error);
      renut::d3d::RegisterPlumeHandlers();
      note("Plume nativo: janela assumida; desenhos, resolves e swaps executados pelo Plume com os shaders recompilados.");
      runtime.set_display_window(game_window.get());
      renut::SetPlumeMissDumpDirectory((root / "plume-miss").string());
      renut::SetPlumePipelineCachePath((root / (plume_env.empty() ? std::string("plume-pipelines.bin")
          : "plume-pipelines-" + std::to_string(std::hash<std::string>{}(plume_env)) + ".bin")).string());
      config.graphics.reset(renut::CreatePlumeGraphicsSystem());
      if(!config.graphics)throw std::runtime_error("Backend gráfico indisponível.");
      // reNut's own AAudio output (renut_audio.cpp), not the SDK's SDL backend.
      config.audio_factory=[](rex::runtime::FunctionDispatcher* dispatcher){return renut::audio::Create(dispatcher);};
      config.input_factory=[](bool tool_mode){
        auto input=rex::input::CreateDefaultInputSystem(tool_mode);
        input->AddDriver(std::make_unique<renut::TouchInput>());
        // Two guest users: the touch controller feeds the first, a second gamepad the second.
        input->SetDeviceAssignment(std::make_unique<renut::PlayerAssignment>());
        return input;
      };
      note("Inicializando tradutor Xenos → SPIR-V e renderizador Vulkan.");
    }else if(surface){
      auto* window=ANativeWindow_fromSurface(env,surface);
      if(!window)throw std::runtime_error("Superfície Android indisponível.");
      std::unique_ptr<ANativeWindow,decltype(&ANativeWindow_release)> owned(window,ANativeWindow_release);
      auto backend=std::make_unique<renut::PlumeGraphics>(window,javaString(env,hooks),javaString(env,driverDirectory),javaString(env,driverLibrary),render_stopped);
      graphics=backend.get();config.graphics=std::move(backend);
      note("Conectando VdSwap ao Plume; imagem de teste, não cena do jogo.");
    }
    config.kernel_init = rex::kernel::InitializeKernel;
    note("Inicializando memória, kernel e tabela de funções...");
    auto status = runtime.Setup(PPCImageConfig, std::move(config));
    if (status != X_STATUS_SUCCESS) throw std::runtime_error(fmt::format("Setup falhou: {:08X}", status));
    note("Runtime inicializado.");
    {
      std::lock_guard<std::mutex> lock(audio_mutex);
      g_audio=static_cast<rex::audio::AudioSystem*>(runtime.audio_system());
    }
    for (size_t i = 0; i < mappings; ++i) {
      if (runtime.function_dispatcher()->GetFunction(uint32_t(PPCFuncMappings[i].guest)) != PPCFuncMappings[i].host)
        throw std::runtime_error("Tabela de funções inconsistente.");
    }
    note("Tabela completa de funções verificada.");
    status = runtime.LoadXexImage("game:\\default.xex");
    if (status != X_STATUS_SUCCESS) throw std::runtime_error(fmt::format("Carregamento XEX falhou: {:08X}", status));
    note("XEX carregado na memória do runtime.");
    renut::StartKernelTime(runtime.kernel_state());
    renut::AchievementsStart(env,activity,runtime,root);
    renut::TextInputStart(env,activity);
    // Actual recompiled leaf: copies the byte at r3+61 to r4 and returns zero.
    // Scratch storage isolates the test from the loaded game image and saves.
    alignas(32) uint8_t scratch[128]{};
    scratch[61] = 0xA7;
    PPCContext context{};
    context.r3.u64 = 0; context.r4.u64 = 96;
    auto* function = runtime.function_dispatcher()->GetFunction(0x82296D30);
    if (!function) throw std::runtime_error("Função de validação ausente.");
    function(context, scratch);
    if (scratch[96] != 0xA7 || context.r3.u64 != 0)
      throw std::runtime_error("Resultado ARM64 divergente.");
    note("Função original 0x82296D30 executada em ARM64: resultado correto.");
    if(launch){
      if(auto* presenter=runtime.graphics_system()->presenter())game_window->SetPresenter(presenter);
      runtime.graphics_system()->InitializeShaderStorage(root / "runtime-cache",runtime.kernel_state()->title_id(),false);
      note("Iniciando o ponto de entrada do Banjo; acompanhe rexglue-runtime.log.");
      auto thread=runtime.LaunchModule();
      if(!thread)throw std::runtime_error("Falha ao iniciar a thread principal do jogo.");
      renut::adpf::Start();
      const auto started=std::chrono::steady_clock::now();
      while(!render_stopped && !game_context->HasQuitFromUIThread()){
        game_context->ExecutePendingFunctionsFromUIThread();game_window->Pump();
        if(std::chrono::steady_clock::now()-started>std::chrono::seconds(1) && !thread->is_running())break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      }
      note("Encerrando a sessão do jogo.");
      ReleasePause();
      {
        std::lock_guard<std::mutex> lock(game_window_mutex);
        live_game_window=nullptr;
      }
      runtime.kernel_state()->TerminateTitle();
      game_window->SetPresenter(nullptr);
      game_context->ExecutePendingFunctionsFromUIThread();
    }
    if(graphics){validateSwap(runtime,*graphics);note(graphics->report());}
    renut::TextInputStop();
    renut::adpf::Stop();
    renut::AchievementsStop(runtime);
    ReleasePause();
    renut::StopKernelTime();
    {
      std::lock_guard<std::mutex> lock(audio_mutex);
      g_audio=nullptr;
    }
    runtime.Shutdown();
    if(auto* plume=renut::PlumeRenderer::Get()){note(plume->report());renut::PlumeRenderer::Stop();}
    note(launch?"Sessão experimental encerrada; consulte o log para verificar a renderização.":surface?"Ponte VdSwap → Plume validada com framebuffer de teste. As cenas e os shaders do jogo ainda não são renderizados.":
        "Validação concluída. O jogo ainda não foi iniciado; gráficos e áudio não são exercitados neste teste.");
  } catch (const std::exception& e) {
    note(std::string("Falha: ") + e.what());
  }
  renutSetContentActivity(env, nullptr);
  rex::thread::AndroidShutdown();
  rex::memory::AndroidShutdown();
  std::ifstream report(root / "runtime-report.txt");
  std::ostringstream content; content << report.rdbuf();
  return env->NewStringUTF(content.str().c_str());
}
