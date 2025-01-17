#include <cstdio>
#include <emscripten/bind.h>
#include <exception>
#include <spdlog/spdlog.h>
#include <string>

#include "grabber/grabber.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
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

EMSCRIPTEN_BINDINGS(ts_live_misc_module) {
  emscripten::function("getExceptionMsg", &getExceptionMsg);
  emscripten::function("showVersionInfo", &showVersionInfo);
  emscripten::function("setLogLevelDebug", &setLogLevelDebug);
  emscripten::function("setLogLevelInfo", &setLogLevelInfo);
  emscripten::function("getGrabberInputBuffer", &getGrabberInputBuffer);
  emscripten::function("grabFirstFrame", &grabFirstFrame);
}
