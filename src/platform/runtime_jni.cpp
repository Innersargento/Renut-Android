#include <jni.h>
#include <unistd.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdlib.h>
#include <thread>

#include <rex/chrono.h>
#include <rex/settings_registry.h>
#include "adpf.h"
#include "android_drivers.h"
#include "d3d/frontend/native_d3d.h"
#include "d3d/hooks/layer.h"
#include "guest/host_support.h"
#include "platform/guest.h"
#include "platform/log.h"
#include "platform/renut_app.h"
#include "platform/settings.h"
#include "plume/frame_pacing.h"
#include "plume/plume_gpu.h"
#include "plume/plume_renderer.h"
#include "touch_input.h"

std::string renutCheatsSummary();
void renutConfigureVulkanDriver(const std::string&, const std::string&, const std::string&, bool, const std::string&);
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
// The player's pause reaches everything the game runs. Guest threads park at their
// next kernel wait (renut::SetTitlePaused), the renderer parks at its next
// present, the guest clock stops crawling, and the audio mixer and the XMA decoder
// stop. The audio part can wait on a worker that is inside a title callback, so
// it runs on its own thread and never on the UI thread; `pause_wanted` is what the
// player last asked, so a quick pause-resume-pause settles on the last one.
static std::mutex pause_op_mutex;
static std::atomic<bool> pause_wanted{false};
static void ApplyAudioPause() {
  std::lock_guard<std::mutex> op(pause_op_mutex);
  if (auto* title = renut::host::Title()) title->SetAudioPaused(pause_wanted.load());
}
namespace renut {
// Releases every pause before the game stops, so no thread is left parked.
void ReleasePause() {
  pause_wanted = false;
  renut::SetTitlePaused(false);
  if (auto* plume = renut::PlumeRenderer::Get()) plume->SetPaused(false);
}
}  // namespace renut
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeStop(JNIEnv*,jclass) {
  renut::RequestStop();
  renut::android::NotifyClose();
  renut::ReleasePause();
  // A game paused in the background is parked in the renderer: wake it to shut down.
  if(auto* plume=renut::PlumeRenderer::Get())plume->ReleaseForShutdown();
}
// The app left the screen: its surface is gone. The game keeps its state and
// stops at its next present, and its clock crawls so that coming back is not a
// jump of minutes for the physics.
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeBackground(JNIEnv*,jclass) {
  renut::adpf::SetActive(false);
  if(auto* plume=renut::PlumeRenderer::Get())plume->PauseForBackground();
  rex::Clock::SetGuestTimeScalar(0.02);
  renut::android::SetSurface(nullptr);
  renut::android::NotifyFocus(false);
}
// SurfaceView.surfaceChanged: the surface's size, which the swapchain follows at its next
// present (a game started in portrait and turned to landscape stayed stretched).
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeSurfaceChanged(JNIEnv* env,jclass,jobject surface,jint width,jint height) {
  auto* window=surface?ANativeWindow_fromSurface(env,surface):nullptr;
  __android_log_print(ANDROID_LOG_INFO,"reNutRuntime","surfaceChanged %dx%d window %p (native %dx%d)",int(width),int(height),
                      static_cast<void*>(window),window?ANativeWindow_getWidth(window):-1,window?ANativeWindow_getHeight(window):-1);
  if(!window)return;
  renut::PlumeRenderer::SetSurfaceSize(window,uint32_t(width>0?width:0),uint32_t(height>0?height:0));
  renut::android::NotifyResized();
  ANativeWindow_release(window);
}
// A new surface: swapchain replaced, the game runs again.
extern "C" JNIEXPORT jboolean JNICALL Java_org_renut_android_RuntimeActivity_nativeForeground(JNIEnv* env,jclass,jobject surface) {
  auto* window=surface?ANativeWindow_fromSurface(env,surface):nullptr;
  __android_log_print(ANDROID_LOG_INFO,"reNutRuntime","foreground: new surface %p (native %dx%d)",static_cast<void*>(window),
                      window?ANativeWindow_getWidth(window):-1,window?ANativeWindow_getHeight(window):-1);
  if(!window)return JNI_FALSE;
  renut::android::SetSurface(window);
  renut::android::NotifyFocus(true);
  bool attached=false;
  if(auto* plume=renut::PlumeRenderer::Get())attached=plume->AttachWindow(window);
  ANativeWindow_release(window);
  if(attached){
    rex::Clock::SetGuestTimeScalar(1.0);
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
  rex::Clock::SetGuestTimeScalar(paused?0.02:1.0);
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
// Debug builds' live cvar setter (RuntimeActivity.registerCvarReceiver).
extern "C" JNIEXPORT jboolean JNICALL Java_org_renut_android_RuntimeActivity_nativeSetCvar(JNIEnv* env,jclass,jstring name,jstring value) {
  const char* n=env->GetStringUTFChars(name,nullptr);
  const char* v=env->GetStringUTFChars(value,nullptr);
  const bool set=bool(REX_SETTINGS().SetText(n,v));
  env->ReleaseStringUTFChars(name,n);
  env->ReleaseStringUTFChars(value,v);
  return set;
}
extern "C" uint64_t renut_guest_swap_count();
extern "C" JNIEXPORT jlong JNICALL Java_org_renut_android_RuntimeActivity_nativeFrameCount(JNIEnv*,jclass) {
  return jlong(renut_guest_swap_count());
}
extern "C" size_t renut_frame_times(float* out, size_t max);
// The last frame times in milliseconds, oldest first (host_support.cpp); empty when the game is not presenting.
extern "C" JNIEXPORT jfloatArray JNICALL Java_org_renut_android_RuntimeActivity_nativeFrameTimes(JNIEnv* env,jclass) {
  float times[256];
  const size_t count = renut_frame_times(times, 256);
  jfloatArray result = env->NewFloatArray(jsize(count));
  if (result && count) env->SetFloatArrayRegion(result, 0, jsize(count), times);
  return result;
}
// The GPU, the driver and the Vulkan version, one per line (empty until the renderer has started).
extern "C" JNIEXPORT jstring JNICALL Java_org_renut_android_RuntimeActivity_nativeDriverInfo(JNIEnv* env,jclass) {
  return env->NewStringUTF(renut::PlumeDriverText().c_str());
}
static std::string javaString(JNIEnv* env,jstring value){
  if(!value)return {};const char* chars=env->GetStringUTFChars(value,nullptr);
  if(!chars)throw std::runtime_error("String JNI indisponível.");
  std::string result(chars);env->ReleaseStringUTFChars(value,chars);return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_renut_android_RuntimeActivity_nativeValidate(JNIEnv* env, jobject activity, jstring directory,
    jobject surface,jstring hooks,jstring driverDirectory,jstring driverLibrary,jboolean launch,
    jboolean oneUiFix,jstring tuDebug,jstring ir3Debug) {
  const char* chars = env->GetStringUTFChars(directory, nullptr);
  if (!chars) return nullptr;
  std::filesystem::path root(chars);
  env->ReleaseStringUTFChars(directory, chars);
  std::ofstream(root / "runtime-report.txt", std::ios::trunc).close();
  auto note = [&](const std::string& s) {
    __android_log_print(ANDROID_LOG_INFO, "reNutRuntime", "%s", s.c_str());
    std::ofstream(root / "runtime-report.txt", std::ios::app) << s << '\n';
  };
  try {
    // /data/user/0 and /data/data may name the same directory on Android.
    root = std::filesystem::canonical(root);
    // The app asks for the log in a file of Documents/renut (LogStorage.java): its path, on one line.
    std::string log_path = (root / "rexglue-runtime.log").string();
    {
      std::string target;
      std::getline(std::ifstream(root / "log-target.txt"), target);
      while (!target.empty() && (target.back() == '\r' || target.back() == ' ')) target.pop_back();
      if (!target.empty()) log_path = target;
    }
    std::string active;
    std::ifstream(root / "active-game.txt") >> active;
    auto game = std::filesystem::weakly_canonical(root / active);
    if (active.empty() || game.string().find(root.string() + "/") != 0 ||
        !std::filesystem::is_regular_file(game / "default.xex"))
      throw std::runtime_error("Importe o XEX europeu antes de validar.");
    if (launch) {
      if (!surface) throw std::runtime_error("Superfície indisponível para o jogo.");
      // Debug: <files>/plume-env holds KEY=VALUE lines put in the environment
      // before the driver loads, to try Turnip and ir3 flags (TU_DEBUG,
      // IR3_SHADER_DEBUG, FD_DEV_FEATURES...) without a build. The pipeline
      // cache is kept apart per content, so binaries compiled under other flags
      // are not handed back.
      std::string plume_env;
      {
        std::ifstream env_file(root / "plume-env");
        for (std::string line; std::getline(env_file, line);) {
          while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
          const auto equals = line.find('=');
          if (line.empty() || line[0] == '#' || equals == std::string::npos || equals == 0) continue;
          setenv(line.substr(0, equals).c_str(), line.substr(equals + 1).c_str(), 1);
          note("Ambiente: " + line);
          plume_env += line + "\n";
        }
      }
      // The app selection, including an empty selection, takes precedence.
      // Include it in the pipeline cache key because IR3 changes shader code.
      const auto ir3 = javaString(env, ir3Debug);
      setenv("IR3_SHADER_DEBUG", ir3.c_str(), 1);
      note("IR3_SHADER_DEBUG: " + (ir3.empty() ? std::string("desativado") : ir3));
      plume_env += "IR3_SHADER_DEBUG=" + ir3 + "\n";
      renutConfigureVulkanDriver(javaString(env, hooks), javaString(env, driverDirectory), javaString(env, driverLibrary),
                                oneUiFix, javaString(env, tuDebug));
      note("Driver solicitado: " + (javaString(env, driverLibrary).empty() ? std::string("sistema") : javaString(env, driverLibrary)));
      if (!javaString(env, tuDebug).empty()) note("TU_DEBUG: " + javaString(env, tuDebug));
      auto* window = ANativeWindow_fromSurface(env, surface);
      if (!window) throw std::runtime_error("Janela Android indisponível.");
      __android_log_print(ANDROID_LOG_INFO, "reNutRuntime", "start: surface %p is %dx%d", static_cast<void*>(window),
                          ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
      renut::android::SetSurface(window);
      ANativeWindow_release(window);  // the window driver holds its own reference
      renut::d3d::SetShaderDumpDirectory((root / "shader-dump").string());
      renut::native_d3d::SetFilesDirectory(root.string());
      // The SDK runs no graphics driver: the native Plume renderer owns the window, and the GPU backend that
      // feeds it (gpu/core/host.cpp) starts when the game creates its Direct3D device.
      std::string plume_error;
      if (!renut::PlumeRenderer::Start(renut::android::Surface(), &plume_error)) throw std::runtime_error(plume_error);
      renut::d3d::RegisterPlumeHandlers();
      note("Plume nativo: janela assumida; desenhos, resolves e swaps executados pelo Plume com os shaders recompilados.");
      renut::SetPlumeMissDumpDirectory((root / "plume-miss").string());
      renut::SetPlumePipelineCachePath((root / (plume_env.empty() ? std::string("plume-pipelines.bin")
          : "plume-pipelines-" + std::to_string(std::hash<std::string>{}(plume_env)) + ".bin")).string());
      note("Inicializando tradutor Xenos → SPIR-V e renderizador Vulkan.");
    }
    renut::Session session;
    session.env = env;
    session.activity = activity;
    session.root = root;
    session.game = game;
    session.log_path = log_path;
    session.launch = launch;
    renut::RunSession(session);
  } catch (const std::exception& e) {
    note(std::string("Falha: ") + e.what());
  }
  renut::android::SetSurface(nullptr);
  std::ifstream report(root / "runtime-report.txt");
  std::ostringstream content;
  content << report.rdbuf();
  return env->NewStringUTF(content.str().c_str());
}
