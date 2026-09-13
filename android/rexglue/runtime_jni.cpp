#include <jni.h>
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/system/function_dispatcher.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>
#include <rex/thread/fiber.h>
#include <android/log.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <android/native_window_jni.h>
#include <rex/graphics/xenos.h>
#include "plume_graphics.h"
#include "android_game_window.h"
#include "touch_input.h"
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/input/input_system.h>
#include "renut_android_init.h"
using rex::X_STATUS;
extern const rex::PPCImageInfo PPCImageConfig;
extern "C" rex::system::IGraphicsSystem* rex_gpu_create(uint32_t, const rex::system::GpuCreateInfo*);
void renutSetContentActivity(JNIEnv*, jobject);
void renutConfigureXenosDriver(const std::string&,const std::string&,const std::string&);
// Own this entry point so the loader cannot resolve SDL's JNI_OnLoad through
// our transitive dependencies. SDL is explicitly loaded/initialized by Java.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }
static std::atomic<bool> render_stopped{false};
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeStop(JNIEnv*,jclass) {render_stopped=true;}
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeButton(JNIEnv*,jclass,jint button,jboolean down) {
  renut::SetTouchButton(uint16_t(button)&0x301F,down);
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
    jobject surface,jstring hooks,jstring driverDirectory,jstring driverLibrary,jboolean launch) {
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
    rex::InitLogging(log_path.c_str(), spdlog::level::info);
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
      renutConfigureXenosDriver(javaString(env,hooks),javaString(env,driverDirectory),javaString(env,driverLibrary));
      note("Driver solicitado: "+(javaString(env,driverLibrary).empty()?std::string("sistema"):javaString(env,driverLibrary)));
      auto* window=ANativeWindow_fromSurface(env,surface);
      if(!window)throw std::runtime_error("Janela Android indisponível.");
      std::unique_ptr<ANativeWindow,decltype(&ANativeWindow_release)> owned(window,ANativeWindow_release);
      game_context=std::make_unique<renut::GameContext>();
      game_window=std::make_unique<renut::GameWindow>(*game_context,window);
      if(!game_window->Open())throw std::runtime_error("Falha ao abrir a superfície do jogo.");
      runtime.set_app_context(game_context.get());runtime.set_display_window(game_window.get());
      rex::system::GpuCreateInfo info{sizeof(rex::system::GpuCreateInfo),"vulkan"};
      config.graphics.reset(rex_gpu_create(rex::system::kGpuPluginAbiVersion,&info));
      if(!config.graphics)throw std::runtime_error("Backend Xenos indisponível.");
      config.audio_factory=REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
      config.input_factory=[](bool tool_mode){
        auto input=rex::input::CreateDefaultInputSystem(tool_mode);
        input->AddDriver(std::make_unique<renut::TouchInput>());
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
    for (size_t i = 0; i < mappings; ++i) {
      if (runtime.function_dispatcher()->GetFunction(uint32_t(PPCFuncMappings[i].guest)) != PPCFuncMappings[i].host)
        throw std::runtime_error("Tabela de funções inconsistente.");
    }
    note("Tabela completa de funções verificada.");
    status = runtime.LoadXexImage("game:\\default.xex");
    if (status != X_STATUS_SUCCESS) throw std::runtime_error(fmt::format("Carregamento XEX falhou: {:08X}", status));
    note("XEX carregado na memória do runtime.");
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
      game_window->SetPresenter(runtime.graphics_system()->presenter());
      runtime.graphics_system()->InitializeShaderStorage(root / "runtime-cache",runtime.kernel_state()->title_id(),false);
      note("Iniciando o ponto de entrada do Banjo; acompanhe rexglue-runtime.log.");
      auto thread=runtime.LaunchModule();
      if(!thread)throw std::runtime_error("Falha ao iniciar a thread principal do jogo.");
      const auto started=std::chrono::steady_clock::now();
      while(!render_stopped && !game_context->HasQuitFromUIThread()){
        game_context->ExecutePendingFunctionsFromUIThread();game_window->Pump();
        if(std::chrono::steady_clock::now()-started>std::chrono::seconds(1) && !thread->is_running())break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
      }
      note("Encerrando a sessão do jogo.");
      runtime.kernel_state()->TerminateTitle();
      game_window->SetPresenter(nullptr);
      game_context->ExecutePendingFunctionsFromUIThread();
    }
    if(graphics){validateSwap(runtime,*graphics);note(graphics->report());}
    runtime.Shutdown();
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
