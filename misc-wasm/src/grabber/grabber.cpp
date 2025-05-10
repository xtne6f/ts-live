#include <cstring>
#include <emscripten/val.h>
#include <spdlog/spdlog.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

static std::vector<uint8_t> inputBuffer;
static std::vector<uint8_t> outputBuffer;

static AVIOContext *avioContext = nullptr;
static size_t avioInputSize;
static size_t avioInputPos;

static int read_packet(void *opaque, uint8_t *buf, int bufSize) {
  if (avioInputSize <= avioInputPos) {
    // 同じ入力を繰り返す
    avioInputPos = 0;
  }
  int copySize = static_cast<int>(avioInputSize - avioInputPos);
  if (copySize > bufSize) {
    copySize = bufSize;
  }
  memcpy(buf, inputBuffer.data() + avioInputPos, copySize);
  avioInputPos += copySize;
  return copySize;
}

emscripten::val getGrabberInputBuffer(size_t size) {
  inputBuffer.resize(size);
  return emscripten::val(
      emscripten::typed_memory_view<uint8_t>(size, inputBuffer.data()));
}

emscripten::val grabFirstFrame(size_t size) {
  if (size == 0 || inputBuffer.size() < size) {
    return emscripten::val::null();
  }
  // 今のところMPEG-2のRawストリームのみ
  const AVInputFormat *inputFormat = av_find_input_format("mpegvideo");
  if (!inputFormat) {
    spdlog::error("av_find_input_format error");
    return emscripten::val::null();
  }
  // AVIOContextは使いまわす
  if (!avioContext) {
    avioContext = avio_alloc_context(
        static_cast<uint8_t *>(av_malloc(64 * 1024)), 64 * 1024, 0, nullptr,
        &read_packet, nullptr, nullptr);
  }
  avioInputSize = size;
  avioInputPos = 0;
  avio_flush(avioContext);

  AVFormatContext *formatContext = avformat_alloc_context();
  formatContext->pb = avioContext;
  formatContext->probesize = size * 3;
  AVStream *stream = nullptr;
  AVCodecContext *codecContext = nullptr;
  if (avformat_open_input(&formatContext, nullptr, inputFormat, nullptr) != 0) {
    spdlog::error("avformat_open_input error");
  } else if (avformat_find_stream_info(formatContext, nullptr) < 0) {
    spdlog::error("avformat_find_stream_info error");
  } else {
    for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
      if (formatContext->streams[i]->codecpar->codec_type ==
          AVMEDIA_TYPE_VIDEO) {
        stream = formatContext->streams[i];
        break;
      }
    }
    const AVCodec *codec;
    if (!stream) {
      spdlog::error("No video stream ...");
    } else if (!(codec = avcodec_find_decoder(stream->codecpar->codec_id))) {
      spdlog::error("No supported decoder for Video ...");
    } else if (!(codecContext = avcodec_alloc_context3(codec))) {
      spdlog::error("avcodec_alloc_context3 for video failed");
    } else if (avcodec_parameters_to_context(codecContext, stream->codecpar) <
               0) {
      spdlog::error("avcodec_parameters_to_context failed");
      avcodec_free_context(&codecContext);
    } else if (avcodec_open2(codecContext, codec, nullptr) != 0) {
      spdlog::error("avcodec_open2 failed");
      avcodec_free_context(&codecContext);
    }
  }

  AVFrame *frame = nullptr;
  if (codecContext) {
    // 最初の1フレームだけ取得
    AVPacket *ppacket = av_packet_alloc();
    while (av_read_frame(formatContext, ppacket) == 0) {
      if (ppacket->stream_index == stream->index &&
          avcodec_send_packet(codecContext, ppacket) == 0) {
        frame = av_frame_alloc();
        if (avcodec_receive_frame(codecContext, frame) == 0) {
          break;
        }
        av_frame_free(&frame);
      }
      av_packet_unref(ppacket);
    }
    av_packet_free(&ppacket);
    avcodec_free_context(&codecContext);
  }
  avformat_close_input(&formatContext);

  emscripten::val retVal = emscripten::val::null();
  if (frame) {
    spdlog::debug(
        "VideoFrame: {}x{} pixfmt:{} key:{} interlace:{} tff:{} pts:{}",
        frame->width, frame->height, frame->format,
        frame->flags & AV_FRAME_FLAG_KEY,
        frame->flags & AV_FRAME_FLAG_INTERLACED,
        frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, frame->pts);

    if (frame->width > 0 && frame->height > 0 &&
        frame->format == AV_PIX_FMT_YUV420P && frame->linesize[0] > 0 &&
        frame->linesize[1] > 0 && frame->linesize[2] > 0) {
      // インタレやUV成分との兼ね合い、負荷も考えてさしあたり半分にリサイズ
      size_t outSize = (frame->width / 2) * (frame->height / 2) * 4;
      if (outputBuffer.size() < outSize) {
        outputBuffer.resize(outSize);
      }
      auto it = outputBuffer.begin();
      for (int i = 0, h2 = frame->height / 2; i < h2; ++i) {
        uint8_t *ly = frame->buf[0]->data + frame->linesize[0] * i * 2;
        uint8_t *lu = frame->buf[1]->data + frame->linesize[1] * i;
        uint8_t *lv = frame->buf[2]->data + frame->linesize[2] * i;
        for (int j = 0, w2 = frame->width / 2; j < w2; ++j) {
          // YUV -> RGBA
          int y = ((ly[0] + ly[1]) >> 1) - 16;
          ly += 2;
          int u = *(lu++) - 128;
          int v = *(lv++) - 128;
          *(it++) = av_clip_uint8((298 * y + 459 * v + 128) >> 8);
          *(it++) = av_clip_uint8((298 * y - 55 * u - 136 * v + 128) >> 8);
          *(it++) = av_clip_uint8((298 * y + 541 * u + 128) >> 8);
          *(it++) = 255;
        }
      }

      retVal = emscripten::val::object();
      retVal.set("width", emscripten::val(frame->width / 2));
      retVal.set("height", emscripten::val(frame->height / 2));
      retVal.set("buffer",
                 emscripten::val(emscripten::typed_memory_view<uint8_t>(
                     outSize, outputBuffer.data())));
    }
    av_frame_free(&frame);
  }
  return retVal;
}
