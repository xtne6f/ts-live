#pragma once
#include <emscripten/val.h>

void initDecoder();
void decoderMainloop();

emscripten::val getNextInputBuffer(size_t nextSize);
void commitInputData(size_t nextSize);
void setCaptionCallback(emscripten::val callback);
void setStatsCallback(emscripten::val callback);
void reset();
void playFile(std::string url);
void setDualMonoMode(int mode);
void setPlaybackRate(double rate);
