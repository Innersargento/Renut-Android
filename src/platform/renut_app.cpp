#include "platform/renut_app.h"

#include <android/log.h>
#include <android/native_window_jni.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include <rex/apu/audio_system.h>
#include <rex/app/title_app.h>
#include <rex/chrono.h>
#include <rex/driver.h>
#include <rex/kernel/title_exit.h>
#include <rex/kernel/title_paths.h>
#include <rex/logging/logger.h>
#include <rex/memory/virtual_memory.h>
#include <rex/platform/thread.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_system.h>

#include "renut_android_init.h"  // the generated image of the game: kPPCImageConfig
#include "adpf.h"
#include "audio_threads.h"
#include "android_drivers.h"
#include "d3d/frontend/native_d3d.h"
#include "d3d/hooks/layer.h"
#include "guest/host_support.h"
#include "plume/frame_pacing.h"
#include "plume/plume_gpu.h"
#include "plume/plume_renderer.h"
#include "platform/guest.h"
#include "platform/log.h"
#include "platform/settings.h"

std::string renutCheatsSummary();
namespace renut {
void AchievementsStart(JNIEnv*, jobject, rex::kernel::TitleRuntime&, const std::filesystem::path&);
void AchievementsStop();
void TextInputStart(JNIEnv*, jobject);
void TextInputStop();
void ReleasePause();
}  // namespace renut

namespace renut {
namespace {

std::atomic<bool> g_stop{false};

// The settings the player's file may name by their old, flat names (renut_msaa = 1): the setting is
// renut.msaa. The log level had a flat name too.
void AddLegacyNames(rex::SettingsRegistry& settings) {
  for (rex::SettingBase* setting : GetSettings().GetOwnedSettings()) {
    // GetName() is the full name, "renut.msaa": the old name was "renut_msaa".
    const std::string& full = setting->GetName();
    settings.AddAlias("renut_" + full.substr(full.find('.') + 1), full);
  }
  // Removed settings (an empty new name: a value in an old file is ignored). The flat log_level is one of
  // the SDK's own old names.
  for (const char* removed : {"renut_pace_original_fps", "renut.pace_original_fps", "renut_vsync", "renut.vsync"})
    settings.AddAlias(removed, "");
}

class RenutApp final : public rex::app::TitleApp {
 public:
  explicit RenutApp(Session& session) : TitleApp("renut", kPPCImageConfig), session_(session) {}

 protected:
  rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
    REX_TRY_VOID(settings.Register(GetSettings()));
    AddLegacyNames(settings);
    static const rex::log::LogCategory* const categories[] = {&rex::log::renut};
    REX_TRY_VOID(rex::GetLogger().AddCategories(categories));
    // Where everything lives, and which drivers run the systems (the app's own, android_drivers.h).
    const auto set = [&](std::string_view name, const std::string& value) -> rex::Result<void> {
      return settings.SetText(rex::SettingLayer::kTitle, name, value);
    };
    REX_TRY_VOID(set("app.root", session_.root.string()));
    REX_TRY_VOID(set("app.user_root", (session_.root / "runtime-user").string()));
    REX_TRY_VOID(set("app.cache_root", (session_.root / "runtime-cache").string()));
    REX_TRY_VOID(set("title.game_root", session_.game.string()));
    REX_TRY_VOID(set("title.metadata_root", (session_.root / "runtime-metadata").string()));
    const std::filesystem::path log = session_.log_path;
    REX_TRY_VOID(set("app.log_root", log.parent_path().string()));
    REX_TRY_VOID(set("log.file_name", log.filename().string()));
    REX_TRY_VOID(set("ui.driver", "android"));
    REX_TRY_VOID(set("apu.driver", "aaudio"));
    REX_TRY_VOID(set("hid.drivers", "touch"));
    // Mixed frames the title may keep ahead of the device: 16 (85 ms) instead of the SDK's 8 (43 ms), which the
    // game's mixer, slowed by a load, a pipeline build or a hot phone, ran out of now and then (a crackle).
    // zolaware's reblue found the same with 8.
    REX_TRY_VOID(set("apu.max_queued_frames", "16"));
    // The game keeps variables in the pages of its code and read-only data, which the SDK loaded
    // writable before 0.11 and now loads read-only unless asked.
    REX_TRY_VOID(set("runtime.writable_code_segments", "true"));
    // 0.11 caps the memory a title can commit (480 MiB); the game commits more on the console's 512 MiB and
    // gets a null pointer back from the allocation the cap refuses.
    REX_TRY_VOID(set("runtime.title_memory_mb", "0"));
    // Unlike the console nothing maps the first guest page, and the game now and then reads a field of a null
    // object (and enters a lock at 0x580 of a null heap): with the page protected that faults the process.
    REX_TRY_VOID(set("runtime.protect_zero", "false"));
    return rex::Ok();
  }

  // No graphics driver (GraphicsFamily::kNone): the game's Direct3D runs natively (d3d/), and the GPU backend that
  // executes what it records is the app's own (gpu/core/host.cpp), started with the game's device. The SDK's
  // graphics system is left with nothing to drive: its register window answers the few register accesses left
  // in the recompiled code, which the native Direct3D no longer reaches.
  rex::AppSysRequirements GetRequirements() const override {
    rex::AppSysRequirements needs = TitleApp::GetRequirements();
    needs.graphics_family = rex::GraphicsFamily::kNone;
    return needs;
  }

  void OnRegisterModules(std::vector<rex::IModule*>& modules) override {
    modules.push_back(android::DriverModule());
  }

  rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override {
    // The mode the SDK hands the kernel when the title launches, which the game queries (VdQueryVideoMode).
    host::SetVideoMode(GetUI().GetVideoMode());
    host::SetGraphics(&GetGraphics());
    audio::SetSystem(&GetAudio());
    host::Bind(title);
    if (session_.env && session_.activity) {
      AchievementsStart(session_.env, session_.activity, title, session_.root);
      TextInputStart(session_.env, session_.activity);
    }
    return rex::Ok();
  }

  void OnShutdown() override {
    static const rex::log::LogCategory* const categories[] = {&rex::log::renut};
    rex::GetLogger().RemoveCategories(categories);
    rex::GetSettingsRegistry().Unregister(GetSettings());
  }

  // The game left on its own (or crashed out): the session is over, the activity decides what next.
  void OnTitleExit(rex::kernel::TitleExitReason) override { RequestShutdown(); }

 private:
  Session& session_;
};

void Checkpoint(const std::filesystem::path& root, const std::string& message) {
  __android_log_print(ANDROID_LOG_INFO, "reNutRuntime", "%s", message.c_str());
  std::ofstream(root / "runtime-report.txt", std::ios::app) << message << '\n';
}

// Guards the ABI assumptions used by libucontext's AArch64 assembly.
void ValidateFibers() {
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, regs) == 184);
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, sp) == 432);
  static_assert(offsetof(ucontext_t, uc_mcontext) + offsetof(mcontext_t, pc) == 440);
  using rex::platform::Fiber;
  struct State {
    Fiber* parent;
    int value;
  } state{Fiber::ConvertCurrentThread(), 0};
  if (!state.parent) throw std::runtime_error("Falha ao criar contexto principal.");
  auto* child = Fiber::Create(128 * 1024, [](void* argument) {
    auto& s = *static_cast<State*>(argument);
    s.value = 17;
    Fiber::SwitchTo(s.parent);
    s.value = 29;
    Fiber::SwitchTo(s.parent);
    std::terminate();
  }, &state);
  if (!child) {
    state.parent->Destroy();
    throw std::runtime_error("Falha ao criar fiber.");
  }
  Fiber::SwitchTo(child);
  const bool first = state.value == 17;
  Fiber::SwitchTo(child);
  const bool second = state.value == 29;
  child->Destroy();
  state.parent->Destroy();
  if (!first || !second) throw std::runtime_error("Troca de contexto ARM64 divergente.");
}

}  // namespace

void RequestStop() { g_stop = true; }

void RunSession(Session& session) {
  const std::filesystem::path root = session.root;
  auto note = [&](const std::string& s) { Checkpoint(root, s); };
  g_stop = false;
  rex::memory::AndroidInitialize();
  rex::platform::AndroidInitialize();
  try {
    if (session.launch) {
      const auto cheats = renutCheatsSummary();
      note(cheats.empty() ? std::string("Cheats: desligados") : "Cheats: " + cheats);
    }
    note("Renderizador: Plume");
    note("RexGlue 0.11.0 / ARM64: biblioteca carregada.");
    size_t mappings = 0;
    while (kPPCFuncMappings[mappings].host) ++mappings;
    note("Funções compiladas: " + std::to_string(mappings));
    ValidateFibers();
    note("Fibers ARM64: suspensão e retomada verificadas.");
    if (!std::filesystem::is_regular_file(session.game / "default.xex"))
      throw std::runtime_error("Importe o XEX europeu antes de validar.");
    if (session.launch && !std::filesystem::is_directory(session.game / "Bundle"))
      throw std::runtime_error("Importe a ISO completa: os assets Bundle não estão presentes.");

    {
    RenutApp app(session);
    // App::Shutdown runs whatever happens after this point, a failed Init or launch included (the SDK's
    // entry macros do the same; Shutdown is safe after a failed Init and unloads a loaded title).
    struct Teardown {
      RenutApp& app;
      ~Teardown() {
        renut::ReleasePause();
        renut::TextInputStop();
        renut::adpf::Stop();
        renut::AchievementsStop();
        // The game's threads stop first (and its terminate notifications run, which may still wait on the GPU),
        // then the GPU backend, whose worker is a kernel thread of the title: it must be gone before the title
        // unloads (Shutdown). The unload's own TerminateTitle then finds nothing left to stop.
        rex::kernel::TitleRuntime& title = app.GetTitleRuntime();
        if (title.IsRunning()) title.GetTitleExit().TerminateTitle();
        renut::PlumeGpuStop();
        renut::audio::SetSystem(nullptr);
        (void)app.Shutdown();
        host::SetGraphics(nullptr);
        host::Unbind();
      }
    } teardown{app};
    const char* argv[] = {"renut"};
    note("Inicializando o SDK, o kernel e a tabela de funções...");
    auto init = app.Init(rex::CLIArgList::Parse(1, const_cast<char**>(argv)));
    if (!init) throw std::runtime_error("Init falhou: " + init.error().GetDescription());
    note("SDK inicializado.");
    // The settings exist and hold renut.toml's values only once Init has run (OnConfigure registers them, then
    // the file is read): before it, the frame-rate target would be the default.
    renut::pacing::ApplyConfiguredTarget();

    if (session.launch) {
      auto launched = app.LaunchTitle();
      if (!launched) throw std::runtime_error("Falha ao iniciar o jogo: " + launched.error().GetDescription());
      note("Iniciando o ponto de entrada do Banjo; acompanhe rexglue-runtime.log.");
      renut::adpf::Start();
      while (!g_stop && app.RunFrame()) {
        if (renut::PlumeDeviceLost()) {
          note("A GPU foi perdida (VK_ERROR_DEVICE_LOST); encerrando a sessão. Detalhes em rexglue-runtime.log e plume-lost.txt.");
          break;
        }
      }
      note("Encerrando a sessão do jogo.");
    } else {
      // Load only: the function table has to match what the generated code registered.
      rex::kernel::TitleRuntime& title = app.GetTitleRuntime();
      if (auto loaded = title.Load(); !loaded)
        throw std::runtime_error("Carregamento XEX falhou: " + loaded.error().GetDescription());
      note("XEX carregado na memória do runtime.");
      for (size_t i = 0; i < mappings; ++i) {
        auto function = title.FindFunction(uint32_t(kPPCFuncMappings[i].address));
        if (!function || *function != kPPCFuncMappings[i].host)
          throw std::runtime_error("Tabela de funções inconsistente.");
      }
      note("Tabela completa de funções verificada.");
      title.Unload();
    }
    }  // Teardown, then the app
    if (auto* plume = renut::PlumeRenderer::Get()) {
      note(plume->report());
      renut::PlumeRenderer::Stop();
    }
    note(session.launch ? "Sessão experimental encerrada; consulte o log para verificar a renderização."
                        : "Validação concluída. O jogo ainda não foi iniciado; gráficos e áudio não são exercitados neste teste.");
  } catch (const std::exception& e) {
    note(std::string("Falha: ") + e.what());
  }
  rex::platform::AndroidShutdown();
  rex::memory::AndroidShutdown();
}

}  // namespace renut
