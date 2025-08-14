#include <condition_variable>
#include <cstring>
#include <deque>
#include <emscripten/bind.h>
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/html5.h>
#include <emscripten/val.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

#include "../../misc-wasm/src/grabber/grabber.hpp"
#include "audio/audioworklet.hpp"
#include "decoder/decoder.hpp"
#include "video/webgpu.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// utility
std::string getExceptionMsg(intptr_t ptr) {
  auto e = reinterpret_cast<std::exception *>(ptr);
  return std::string(e->what());
}

void showVersionInfo() {
  printf("version: %s\nconfigure: %s\n", av_version_info(),
         avutil_configuration());
}

void setLogLevelDebug() { spdlog::set_level(spdlog::level::debug); }
void setLogLevelInfo() { spdlog::set_level(spdlog::level::info); }

void mainloop(void *arg) {
  // mainloop
  decoderMainloop(false);
}

static long rafID = 0;

static void pauseRafLoop() {
  if (rafID) {
    emscripten_cancel_animation_frame(rafID);
    rafID = 0;
  }
}

static void resumeRafLoop() {
  static bool (*const rafCb)(double, void *) = [](double time, void *userData) {
    decoderMainloop(true);
    long &rafID = *static_cast<long *>(userData);
    rafID = emscripten_request_animation_frame(rafCb, &rafID);
    return false;
  };
  if (!rafID) {
    rafID = emscripten_request_animation_frame(rafCb, &rafID);
  }
}

static void pause() {
  emscripten_pause_main_loop();
  pauseRafLoop();
  pauseAudioWorklet();
}

static void resume() {
  resumeAudioWorklet();
  resumeRafLoop();
  emscripten_resume_main_loop();
}

int main() {
  spdlog::info("Wasm main() started.");

  // デコーダスレッド起動
  spdlog::info("initializing Decoder");
  initDecoder();

  // WebGPU起動
  spdlog::info("initializing webgpu");
  initWebGpu();

  // requestAnimationFrameも使う。バックグラウンド状態では基本的に一時停止してしまう。
  resumeRafLoop();

  // //
  // fps指定するとrAFループじゃなくタイマーになるので裏周りしても再生が続く。fps<=0だとrAFが使われるらしい。
  const int fps = 10;
  const int simulate_infinite_loop = 1;
  spdlog::info("Starting main loop.");
  emscripten_set_main_loop_arg(mainloop, NULL, fps, simulate_infinite_loop);

  pauseRafLoop();
  // SDL_DestroyRenderer(ctx.renderer);
  // SDL_DestroyWindow(ctx.window);
  // SDL_Quit();

  return EXIT_SUCCESS;
}

EMSCRIPTEN_BINDINGS(ts_live_module) {
  emscripten::function("getExceptionMsg", &getExceptionMsg);
  emscripten::function("showVersionInfo", &showVersionInfo);
  emscripten::function("setCaptionCallback", &setCaptionCallback);
  emscripten::function("setStatsCallback", &setStatsCallback);
  emscripten::function("playFile", &playFile);
  emscripten::function("getNextInputBuffer", &getNextInputBuffer);
  emscripten::function("commitInputData", &commitInputData);
  emscripten::function("reset", &reset);
  emscripten::function("setLogLevelDebug", &setLogLevelDebug);
  emscripten::function("setLogLevelInfo", &setLogLevelInfo);
  emscripten::function("pause", &pause);
  emscripten::function("resume", &resume);
  emscripten::function("setBufferedAudioSamples", &setBufferedAudioSamples);
  emscripten::function("setAudioGain", &setAudioGain);
  emscripten::function("setDualMonoMode", &setDualMonoMode);
  emscripten::function("setDetelecineMode", &setDetelecineMode);
  emscripten::function("setPlaybackRate", &setPlaybackRate);
  emscripten::function("getGrabberInputBuffer", &getGrabberInputBuffer);
  emscripten::function("grabFirstFrame", &grabFirstFrame);
}
