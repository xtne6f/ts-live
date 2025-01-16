#pragma once
#include <emscripten/val.h>

emscripten::val getGrabberInputBuffer(size_t size);
emscripten::val grabFirstFrame(size_t size);
