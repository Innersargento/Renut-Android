// Text entry: when the game asks for the Xbox keyboard (XamShowKeyboardUI, for instance to
// name a blueprint), the kernel calls TextInputHandler below. It hands the request to
// RuntimeActivity.requestTextInput, which shows a text box over the game, and waits on the
// kernel's dispatch thread until nativeTextInputResult brings the player's answer back.
//
// Strings cross as UTF-16 (jchar == char16_t) so nothing goes through JNI's modified UTF-8.
#include <jni.h>
#include <android/log.h>
#include <rex/logging.h>
#include <rex/kernel/xam/private.h>
#include <condition_variable>
#include <mutex>
#include <string>

namespace {

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
bool TextInputHandler(const rex::kernel::xam::TextInputRequest& request, std::u16string& result) {
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
  rex::kernel::xam::SetTextInputHandler(&TextInputHandler);
}

// Before the runtime is torn down: a keyboard still open must not hold the dispatch thread.
void TextInputStop() {
  rex::kernel::xam::SetTextInputHandler(nullptr);
  std::lock_guard<std::mutex> lock(bridge_mutex);
  if (pending.active) {
    pending.done = true;
    pending.cancelled = true;
    answered.notify_all();
  }
}

}  // namespace renut

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
