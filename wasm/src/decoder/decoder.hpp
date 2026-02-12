#pragma once
#include <emscripten/val.h>

void initDecoder();
void decoderMainloop(bool calledByRaf);

emscripten::val getNextInputBuffer(size_t nextSize);
void commitInputData(size_t nextSize);
void setCaptionCallback(emscripten::val callback);
void setStatsCallback(emscripten::val callback);
void reset();
void playFile(std::string url);
void setDualMonoMode(int mode);
void setDetelecineMode(int mode);
std::string setDeinterlace(std::string filter);
void setPlaybackRate(double rate);
