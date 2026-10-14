// Text entry: when the game asks for the Xbox keyboard (XamShowKeyboardUI, for instance to
// name a blueprint), renut_imp_XamShowKeyboardUI (below, in place of the SDK's export: see
// kernel_overrides.inc) copies the request out of guest memory and has the kernel's dispatch
// thread run TextInputHandler. It hands the request to RuntimeActivity.requestTextInput, which
// shows a text box over the game, and waits until nativeTextInputResult brings the player's
// answer back. With no text box (no activity yet) the SDK's own export answers.
//
// Strings cross as UTF-16 (jchar == char16_t) so nothing goes through JNI's modified UTF-8.
#include <jni.h>
#include <android/log.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>

#include "guest_address.h"

// The SDK's X_ERROR_* macros are written with the unqualified types.
using rex::X_RESULT;
using rex::X_STATUS;

extern "C" void __imp__XamShowKeyboardUI(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// What the title asked for, copied out of guest memory (it may reuse its strings while the
// player is typing).
struct TextInputRequest {
  std::u16string title, description, default_text;
  uint32_t flags = 0;
  uint32_t max_length = 0;
};
std::atomic<bool> text_box_available{false};

JavaVM* java_vm = nullptr;
jclass activity_class = nullptr;
jmethodID request_method = nullptr;

std::mutex bridge_mutex;
std::condition_variable answered;
// One question at a time: the game cannot open a second keyboard over the first.
struct Pending {
  bool active = false;
  bool done = false;
  bool cancelled = true;
  std::u16string text;
} pending;

struct ThreadAttachment {
  bool attached = false;
  ~ThreadAttachment() { if (attached && java_vm) java_vm->DetachCurrentThread(); }
};

JNIEnv* currentEnv() {
  JNIEnv* env = nullptr;
  thread_local ThreadAttachment attachment;
  if (!java_vm) return nullptr;
  if (java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
    if (java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    attachment.attached = true;
  }
  return env;
}

jstring toJava(JNIEnv* env, const std::u16string& text) {
  return env->NewString(reinterpret_cast<const jchar*>(text.data()), jsize(text.size()));
}

// Blocks until the player answers. False = cancelled (or the box could not be shown).
bool TextInputHandler(const TextInputRequest& request, std::u16string& result) {
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    if (!java_vm || !activity_class || !request_method || pending.active) return false;
    pending = Pending{};
    pending.active = true;
  }
  bool shown = false;
  if (JNIEnv* env = currentEnv()) {
    jstring title = toJava(env, request.title);
    jstring description = toJava(env, request.description);
    jstring text = toJava(env, request.default_text);
    if (title && description && text) {
      env->CallStaticVoidMethod(activity_class, request_method, title, description, text,
                                jint(request.max_length), jint(request.flags));
      shown = !env->ExceptionCheck();
    }
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    if (title) env->DeleteLocalRef(title);
    if (description) env->DeleteLocalRef(description);
    if (text) env->DeleteLocalRef(text);
  }
  std::unique_lock<std::mutex> lock(bridge_mutex);
  if (!shown) pending.done = true;
  answered.wait(lock, [] { return pending.done; });
  bool accepted = !pending.cancelled;
  if (accepted) result = std::move(pending.text);
  pending = Pending{};
  return accepted;
}

}  // namespace

namespace renut {

// `activity` supplies the class whose static requestTextInput(...) shows the text box.
void TextInputStart(JNIEnv* env, jobject activity) {
  {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    if (!java_vm) env->GetJavaVM(&java_vm);
    if (!activity_class) {
      jclass local = env->GetObjectClass(activity);
      activity_class = static_cast<jclass>(env->NewGlobalRef(local));
      env->DeleteLocalRef(local);
      request_method = env->GetStaticMethodID(
          activity_class, "requestTextInput",
          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II)V");
      if (!request_method) env->ExceptionClear();
    }
  }
  text_box_available.store(true);
}

// Before the runtime is torn down: a keyboard still open must not hold the dispatch thread.
void TextInputStop() {
  text_box_available.store(false);
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (pending.active) {
    pending.done = true;
    pending.cancelled = true;
    answered.notify_all();
  }
}

}  // namespace renut

namespace {

std::u16string GuestString(const uint8_t* base, uint32_t address) {
  std::u16string text;
  if (!address) return text;
  const uint8_t* p = renut::GuestPtr(base, address);
  for (size_t i = 0; i < 1024; ++i) {
    const char16_t c = char16_t(uint16_t(p[i * 2] << 8 | p[i * 2 + 1]));
    if (!c) break;
    text.push_back(c);
  }
  return text;
}

}  // namespace

// XamShowKeyboardUI(user, flags, default_text, title, description, buffer, buffer_length,
// overlapped): the title's text entry. The request goes to the app's text box and the answer
// is written into the title's buffer; the overlapped completes when the player has answered.
extern "C" void renut_imp_XamShowKeyboardUI(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t buffer_address = ctx.r8.u32;
  const uint32_t buffer_length = ctx.r9.u32;
  const uint32_t overlapped = ctx.r10.u32;
  auto* kernel = rex::system::kernel_state();
  if (!text_box_available.load() || !kernel || !buffer_address || !overlapped) {
    __imp__XamShowKeyboardUI(ctx, base);
    return;
  }
  TextInputRequest request;
  request.flags = ctx.r4.u32;
  request.default_text = GuestString(base, ctx.r5.u32);
  request.title = GuestString(base, ctx.r6.u32);
  request.description = GuestString(base, ctx.r7.u32);
  request.max_length = buffer_length ? buffer_length - 1 : 0;
  if (request.default_text.size() > request.max_length) request.default_text.resize(request.max_length);
  REXKRNL_INFO("XamShowKeyboardUI: flags {:08X}, up to {} characters", request.flags, request.max_length);
  uint8_t* buffer = renut::GuestPtr(base, buffer_address);
  // The buffer holds big-endian UTF-16, terminated, at most buffer_length characters.
  auto store = [buffer, buffer_length](const std::u16string& text) {
    if (!buffer_length) return;
    const size_t count = std::min<size_t>(text.size(), buffer_length - 1);
    for (size_t i = 0; i < count; ++i) {
      buffer[i * 2] = uint8_t(uint16_t(text[i]) >> 8);
      buffer[i * 2 + 1] = uint8_t(uint16_t(text[i]));
    }
    buffer[count * 2] = buffer[count * 2 + 1] = 0;
  };
  auto run = [request = std::move(request), store](uint32_t& extended_error, uint32_t& length) -> rex::X_RESULT {
    length = 0;
    std::u16string text;
    if (!request.max_length || !TextInputHandler(request, text)) {
      // Cancelled: the title reads ERROR_CANCELLED from the overlapped and must not find
      // anything but an empty string in its buffer.
      store(std::u16string());
      extended_error = X_ERROR_CANCELLED;
      return X_ERROR_SUCCESS;
    }
    store(text);
    extended_error = X_ERROR_SUCCESS;
    return X_ERROR_SUCCESS;
  };
  // XN_SYS_UI while the box is up, as the SDK's own dialogs do.
  auto pre = [kernel] { kernel->BroadcastNotification(0x9, true); };
  auto post = [kernel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    kernel->BroadcastNotification(0x9, false);
  };
  kernel->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
  ctx.r3.u64 = X_ERROR_IO_PENDING;
}

// The player pressed OK (cancelled=false, text = what was typed) or cancelled.
extern "C" JNIEXPORT void JNICALL Java_org_renut_android_RuntimeActivity_nativeTextInputResult(
    JNIEnv* env, jclass, jstring text, jboolean cancelled) {
  std::u16string value;
  if (text && !cancelled) {
    const jchar* chars = env->GetStringChars(text, nullptr);
    if (chars) {
      value.assign(reinterpret_cast<const char16_t*>(chars), size_t(env->GetStringLength(text)));
      env->ReleaseStringChars(text, chars);
    }
  }
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (!pending.active || pending.done) return;
  pending.text = std::move(value);
  pending.cancelled = cancelled != JNI_FALSE;
  pending.done = true;
  answered.notify_all();
}
