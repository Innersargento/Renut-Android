#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr const char* kTag = "renut-video";

std::string error(const char* action, int code) {
  char detail[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, detail, sizeof(detail));
  return std::string(action) + ": " + detail;
}
void check(int code, const char* action) {
  if (code < 0) throw std::runtime_error(error(action, code));
}
// CLOCK_MONOTONIC, the clock behind Java's System.nanoTime().
int64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

struct Player {
  AVFormatContext* input = nullptr;
  AVCodecContext* decoder = nullptr;
  SwsContext* scaler = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  ANativeWindow* window = nullptr;
  ~Player() {
    if (window) ANativeWindow_release(window);
    av_packet_free(&packet);
    av_frame_free(&frame);
    sws_freeContext(scaler);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
  }
};

// Plays the movie against the guest's clock: start_us is when the game began
// showing it, so a slow open or a slow decode drops frames instead of letting
// the picture drift behind the game's audio.
void play(JNIEnv* env, const char* path, jobject surface, jobject worker, jobject activity,
          int64_t start_us) {
  jclass thread_class = env->GetObjectClass(worker);
  jmethodID is_interrupted = env->GetMethodID(thread_class, "isInterrupted", "()Z");
  env->DeleteLocalRef(thread_class);
  auto interrupted = [&] { return env->CallBooleanMethod(worker, is_interrupted) == JNI_TRUE; };

  Player p;
  check(avformat_open_input(&p.input, path, nullptr, nullptr), "Abrir vídeo do jogo");
  int stream_index = av_find_best_stream(p.input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  // The ASF header carries the WMV3 sequence header; probing packets only
  // delays the first frame.
  if (stream_index < 0 || p.input->streams[stream_index]->codecpar->width <= 0) {
    check(avformat_find_stream_info(p.input, nullptr), "Ler vídeo do jogo");
    stream_index = av_find_best_stream(p.input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  }
  check(stream_index, "Encontrar vídeo");
  AVStream* stream = p.input->streams[stream_index];
  for (unsigned i = 0; i < p.input->nb_streams; ++i)
    if (int(i) != stream_index) p.input->streams[i]->discard = AVDISCARD_ALL;
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) throw std::runtime_error("Codec de vídeo indisponível.");
  p.decoder = avcodec_alloc_context3(codec);
  if (!p.decoder) throw std::runtime_error("Memória insuficiente para vídeo.");
  check(avcodec_parameters_to_context(p.decoder, stream->codecpar), "Configurar vídeo");
  p.decoder->pkt_timebase = stream->time_base;
  check(avcodec_open2(p.decoder, codec, nullptr), "Iniciar vídeo");
  int width = p.decoder->width, height = p.decoder->height;
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
    throw std::runtime_error("Dimensões de vídeo inválidas.");

  p.window = ANativeWindow_fromSurface(env, surface);
  if (!p.window) throw std::runtime_error("Superfície de vídeo indisponível.");
  check(ANativeWindow_setBuffersGeometry(p.window, width, height, WINDOW_FORMAT_RGBA_8888),
        "Configurar superfície de vídeo");
  // Clear what the view kept from the previous movie.
  ANativeWindow_Buffer buffer;
  if (ANativeWindow_lock(p.window, &buffer, nullptr) == 0) {
    for (int y = 0; y < buffer.height; ++y)
      std::memset(static_cast<uint8_t*>(buffer.bits) + size_t(y) * buffer.stride * 4, 0,
                  size_t(buffer.width) * 4);
    ANativeWindow_unlockAndPost(p.window);
  }
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID on_size = env->GetMethodID(activity_class, "onNativeMovieSize", "(II)V");
  env->DeleteLocalRef(activity_class);
  if (on_size) env->CallVoidMethod(activity, on_size, width, height);
  if (env->ExceptionCheck()) env->ExceptionClear();

  p.frame = av_frame_alloc();
  p.packet = av_packet_alloc();
  if (!p.frame || !p.packet) throw std::runtime_error("Memória insuficiente para quadros.");

  AVRational rate = av_guess_frame_rate(p.input, stream, nullptr);
  const int64_t frame_us = rate.num > 0 ? av_rescale(1000000, rate.den, rate.num) : 33333;
  if (start_us <= 0) start_us = now_us();
  int64_t first_pts = AV_NOPTS_VALUE;
  int shown = 0, dropped = 0;
  bool cancelled = false;

  auto present = [&] {
    if (ANativeWindow_lock(p.window, &buffer, nullptr) != 0) {
      ++dropped;
      return;
    }
    p.scaler = sws_getCachedContext(p.scaler, p.frame->width, p.frame->height,
        static_cast<AVPixelFormat>(p.frame->format), buffer.width, buffer.height,
        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (p.scaler) {
      uint8_t* planes[4] = {static_cast<uint8_t*>(buffer.bits)};
      int strides[4] = {buffer.stride * 4};
      sws_scale(p.scaler, p.frame->data, p.frame->linesize, 0, p.frame->height, planes, strides);
    }
    ANativeWindow_unlockAndPost(p.window);
    if (!p.scaler) throw std::runtime_error("Conversão de cor indisponível.");
    ++shown;
  };
  auto drain = [&] {
    while (!cancelled) {
      int result = avcodec_receive_frame(p.decoder, p.frame);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
      check(result, "Decodificar WMV3");
      int64_t pts = p.frame->best_effort_timestamp;
      int64_t late_us = 0;
      if (pts != AV_NOPTS_VALUE) {
        if (first_pts == AV_NOPTS_VALUE) first_pts = pts;
        int64_t target = start_us + av_rescale_q(pts - first_pts, stream->time_base, {1, 1000000});
        for (int64_t wait; (wait = target - now_us()) > 0;) {
          if (interrupted()) { cancelled = true; break; }
          std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(wait, 10000)));
        }
        late_us = now_us() - target;
      }
      if (cancelled || interrupted()) { cancelled = true; av_frame_unref(p.frame); return; }
      // Behind by more than a frame: skip the conversion, and past a few
      // frames let the decoder skip non-reference frames until it catches up.
      p.decoder->skip_frame = late_us > 4 * frame_us ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
      if (late_us > frame_us) ++dropped;
      else present();
      av_frame_unref(p.frame);
    }
  };
  while (!cancelled) {
    int result = av_read_frame(p.input, p.packet);
    if (result == AVERROR_EOF) break;
    check(result, "Ler filme ASF");
    if (p.packet->stream_index == stream_index) {
      result = avcodec_send_packet(p.decoder, p.packet);
      // A damaged packet only costs its frame.
      if (result < 0 && result != AVERROR(EAGAIN))
        __android_log_print(ANDROID_LOG_WARN, kTag, "%s", error("Enviar quadro WMV3", result).c_str());
      drain();
    }
    av_packet_unref(p.packet);
  }
  if (!cancelled) {
    avcodec_send_packet(p.decoder, nullptr);
    drain();
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "%s: %dx%d, %d quadros exibidos, %d descartados%s",
                      path, width, height, shown, dropped, cancelled ? " (interrompido)" : "");
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_renut_android_NativeMoviePlayer_play(JNIEnv* env, jclass, jstring source, jobject surface,
                                              jobject worker, jobject activity, jlong start_nanos) {
  const char* path = env->GetStringUTFChars(source, nullptr);
  std::string failure;
  try {
    if (!path) throw std::runtime_error("Caminho de vídeo indisponível.");
    play(env, path, surface, worker, activity, start_nanos / 1000);
  } catch (const std::exception& exception) {
    failure = exception.what();
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s: %s", path ? path : "?", failure.c_str());
  }
  if (path) env->ReleaseStringUTFChars(source, path);
  return failure.empty() ? nullptr : env->NewStringUTF(failure.c_str());
}
