#include <chrono>
#include <condition_variable>
#include <deque>
#include <emscripten/bind.h>
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/val.h>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>

#include "../audio/audioworklet.hpp"
#include "../video/webgpu.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/bprint.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// tsreadex
#include <servicefilter.hpp>

CServiceFilter servicefilter;
int servicefilterRemain = 0;

const size_t MAX_INPUT_BUFFER = 20 * 1024 * 1024;
const size_t PROBE_SIZE = 1024 * 1024;
const size_t DEFAULT_WIDTH = 1920;
const size_t DEFAULT_HEIGHT = 1080;

// 高コストなのでスレッド間の処理の揺らぎを補償する最低限にする
const size_t MAX_VIDEO_FRAME_QUEUE_SIZE = 8;

// 映像と音声のパケットには時差があるので最低でも20程度必要
// (小さいとちょっとしたことでリップシンク条件によりキューの生産も消費も止まる)
const size_t MAX_VIDEO_PACKET_QUEUE_SIZE = 60;

int64_t currentPlaybackTime = 0;
int64_t currentPlaybackPtsTime = -1;

bool resetedDecoder = false;
std::uint8_t inputBuffer[MAX_INPUT_BUFFER];
std::mutex inputBufferMtx;
std::condition_variable waitCv;

size_t inputBufferReadIndex = 0;
size_t inputBufferWriteIndex = 0;

std::deque<AVFrame *> videoFrameQueue, audioFrameQueue;
std::deque<std::pair<int64_t, std::vector<uint8_t>>> captionDataQueue;
std::mutex captionDataMtx;
bool videoFrameFound = false;
double estimatedAudioPlayTime = -1;

std::deque<AVPacket *> videoPacketQueue, audioPacketQueue;
std::mutex videoPacketMtx, audioPacketMtx;
std::condition_variable videoPacketCv, audioPacketCv;

AVStream *videoStream = nullptr;
std::vector<AVStream *> audioStreamList;
AVStream *captionStream = nullptr;

int64_t initPts = -1;

emscripten::val captionCallback = emscripten::val::null();

std::string playFileUrl;
std::thread downloaderThread;

bool resetedDownloader = false;

std::vector<emscripten::val> statsBuffer;

emscripten::val statsCallback = emscripten::val::null();

const size_t donwloadRangeSize = 2 * 1024 * 1024;
size_t downloadCount = 0;

// Callback register
void setCaptionCallback(emscripten::val callback) {
  captionCallback = callback;
}

void setStatsCallback(emscripten::val callback) {
  //
  statsCallback = callback;
}

enum class DualMonoMode { MAIN = 0, SUB = 1 };
DualMonoMode dualMonoMode = DualMonoMode::MAIN;

void setDualMonoMode(int mode) {
  //
  dualMonoMode = (DualMonoMode)mode;
}

enum class DetelecineMode { NEVER = 0, FORCE = 1, AUTO = 2 };
DetelecineMode detelecineMode = DetelecineMode::NEVER;

void setDetelecineMode(int mode) {
  //
  if (mode < 0 || mode > 2) {
    spdlog::error("setDetelecineMode() unsupported mode");
    return;
  }
  std::lock_guard<std::mutex> lock(videoPacketMtx);
  detelecineMode = (DetelecineMode)mode;
}

double targetAudioTempo = 1.0;
double currentAudioTempo = 1.0;

void setPlaybackRate(double rate) {
  //
  if (!(rate >= 0.1 && rate <= 100.0)) {
    // atempoフィルタの対応範囲は[0.5-100]だが、[0.1-0.5)の区間も無音で対応する
    spdlog::error("setPlaybackRate() out of range [0.1 - 100]");
    return;
  }
  std::lock_guard<std::mutex> lock(audioPacketMtx);
  targetAudioTempo = rate;
}

// Buffer control
emscripten::val getNextInputBuffer(size_t nextSize) {
  std::lock_guard<std::mutex> lock(inputBufferMtx);
  if (inputBufferWriteIndex + nextSize >= MAX_INPUT_BUFFER &&
      inputBufferReadIndex > 0) {
    size_t remainSize = inputBufferWriteIndex - inputBufferReadIndex;
    memmove(&inputBuffer[0], &inputBuffer[inputBufferReadIndex], remainSize);
    inputBufferReadIndex = 0;
    inputBufferWriteIndex = remainSize;
  }
  if (inputBufferWriteIndex + nextSize >= MAX_INPUT_BUFFER) {
    return emscripten::val::null();
  }
  auto retVal = emscripten::val(emscripten::typed_memory_view<uint8_t>(
      nextSize, &inputBuffer[inputBufferWriteIndex]));
  waitCv.notify_all();
  return retVal;
}

int read_packet(void *opaque, uint8_t *buf, int bufSize) {
  std::unique_lock<std::mutex> lock(inputBufferMtx);
  waitCv.wait(lock, [&] {
    return inputBufferWriteIndex - inputBufferReadIndex >= bufSize ||
           resetedDecoder;
  });
  if (resetedDecoder) {
    spdlog::debug("resetedDecoder detected in read_packet");
    return -1;
  }

  // 0x47: TS packet header sync_byte
  while (inputBuffer[inputBufferReadIndex] != 0x47 &&
         inputBufferReadIndex < inputBufferWriteIndex) {
    inputBufferReadIndex++;
  }

  // 前回返しきれなかったパケットがあれば消費する
  int copySize = 0;
  if (servicefilterRemain) {
    copySize = bufSize / 188 * 188;
    if (copySize > servicefilterRemain) {
      copySize = servicefilterRemain;
    }
    const auto &packets = servicefilter.GetPackets();
    memcpy(buf, packets.data() + packets.size() - servicefilterRemain,
           copySize);
    servicefilterRemain -= copySize;
    if (!servicefilterRemain) {
      servicefilter.ClearPackets();
    }
  }

  // servicefilterに1パケット（188バイト）だけ入れたからといって、
  // 出てくるのは1パケットとは限らない。色々追加される可能性がある
  while (!servicefilterRemain &&
         inputBufferReadIndex + 188 < inputBufferWriteIndex) {
    servicefilter.AddPacket(&inputBuffer[inputBufferReadIndex]);
    inputBufferReadIndex += 188;
    const auto &packets = servicefilter.GetPackets();
    servicefilterRemain = static_cast<int>(packets.size());
    if (servicefilterRemain) {
      int addSize = bufSize / 188 * 188 - copySize;
      if (addSize > servicefilterRemain) {
        addSize = servicefilterRemain;
      }
      memcpy(buf + copySize, packets.data(), addSize);
      copySize += addSize;
      servicefilterRemain -= addSize;
      if (!servicefilterRemain) {
        servicefilter.ClearPackets();
      }
    }
  }

  waitCv.notify_all();
  return copySize;
}

void commitInputData(size_t nextSize) {
  std::lock_guard<std::mutex> lock(inputBufferMtx);
  inputBufferWriteIndex += nextSize;
  waitCv.notify_all();
  spdlog::debug("commit {} bytes", nextSize);
}

// reset
void resetInternal() {
  downloadCount = 0;
  playFileUrl = std::string("");

  spdlog::info("downloaderThread joinable: {}", downloaderThread.joinable());
  if (downloaderThread.joinable()) {
    spdlog::info("join to downloader thread");
    downloaderThread.join();
    spdlog::info("done.");
  }
  {
    std::lock_guard<std::mutex> lock(inputBufferMtx);
    inputBufferReadIndex = 0;
    inputBufferWriteIndex = 0;
    servicefilter.ClearPackets();
    servicefilterRemain = 0;
  }
  {
    std::lock_guard<std::mutex> lock(videoPacketMtx);
    while (!videoPacketQueue.empty()) {
      auto ppacket = videoPacketQueue.front();
      videoPacketQueue.pop_front();
      av_packet_free(&ppacket);
    }
    while (!videoFrameQueue.empty()) {
      auto frame = videoFrameQueue.front();
      videoFrameQueue.pop_front();
      av_frame_free(&frame);
    }
    currentPlaybackPtsTime = -1;
  }
  {
    std::lock_guard<std::mutex> lock(audioPacketMtx);
    while (!audioPacketQueue.empty()) {
      auto ppacket = audioPacketQueue.front();
      audioPacketQueue.pop_front();
      av_packet_free(&ppacket);
    }
    while (!audioFrameQueue.empty()) {
      auto frame = audioFrameQueue.front();
      audioFrameQueue.pop_front();
      av_frame_free(&frame);
    }
  }
  videoStream = nullptr;
  audioStreamList.clear();
  captionStream = nullptr;
  videoFrameFound = false;
}

void reset() {
  spdlog::debug("reset()");
  resetedDecoder = true;
  resetedDownloader = true;
  resetInternal();
}

void detectTelecine(AVFrame *frame, AVFrame *&prevFrame,
                    double (&telecineDetectCounts)[5], int &frameCount) {
  if (prevFrame && frame->width == prevFrame->width &&
      frame->height == prevFrame->height) {
    // 上下フィールドについて前後フレームの差分絶対値和を計算する
    int64_t topDiff = 0;
    int64_t bottomDiff = 0;
    for (int i = 0; i < 3; i++) {
      // テレシネ周期の推測が目的なので支障ない程度にサボる
      int odd = frameCount % 2;
      int w = frame->width / (i ? 2 : 1);
      int h = frame->height / (i ? 2 : 1) / (2 - odd);
      for (int y = h / 2 * odd; y < h; y++) {
        const uint8_t *prev = prevFrame->data[i] + y * prevFrame->linesize[i];
        const uint8_t *cur = frame->data[i] + y * frame->linesize[i];
        int sad = 0;
        for (int x = 0; x < w; x++) {
          int d = cur[x] - prev[x];
          sad += FFABS(d);
        }
        (y % 2 ? bottomDiff : topDiff) += sad;
      }
    }

    // 前後フレームが最も変化すると概ね1になるような値
    double reliability = FFMAX(topDiff, bottomDiff) /
                         (0.5 * 0.5 * 1.5 * 255 * frame->width * frame->height);
    // じゅうぶん変化していると判断するしきい値を定めてこれを信頼度とする
    reliability = FFMIN(reliability, 0.01) * 100;
    if (bottomDiff > 3 * topDiff) {
      // repeated-top
      telecineDetectCounts[frameCount % 5] += reliability;
    } else if (topDiff > 3 * bottomDiff) {
      // repeated-bottom
      telecineDetectCounts[(frameCount + 3) % 5] += reliability;
    }
    for (int i = 0; i < 5; i++) {
      // 信頼度に応じて半減期を∞～30フレームとする。2^(-1/30)≒0.977
      telecineDetectCounts[i] *= 1 - (1 - 0.977) * reliability;
    }

    // テレシネの周期を記録する
    int cycleAdjust = 0;
    for (int i = 0; i < 5; i++) {
      if (telecineDetectCounts[i] > telecineDetectCounts[cycleAdjust]) {
        cycleAdjust = i;
      }
    }
    av_dict_set_int(&frame->metadata, "ts-live.frame_cycle",
                    (frameCount + 5 - cycleAdjust) % 5, 0);

    // テレシネっぽいかどうか記録する
    // 安定のために前回の判定によってしきい値を変える
    if (telecineDetectCounts[cycleAdjust] >
        (av_dict_get(prevFrame->metadata, "ts-live.is_telecine", nullptr, 0)
             ? 1
             : 4)) {
      av_dict_set(&frame->metadata, "ts-live.is_telecine", "1", 0);
    }
    frameCount = (frameCount + 1) % 10;
  }

  av_frame_free(&prevFrame);
  prevFrame = av_frame_clone(frame);
}

void videoDecoderThreadFunc(bool &terminateFlag) {
  // find decoder
  const AVCodec *videoCodec =
      avcodec_find_decoder(videoStream->codecpar->codec_id);
  if (videoCodec == nullptr) {
    spdlog::error("No supported decoder for Video ...");
    return;
  } else {
    spdlog::debug("Video Decoder created.");
  }

  // Codec Context
  AVCodecContext *videoCodecContext = avcodec_alloc_context3(videoCodec);
  if (videoCodecContext == nullptr) {
    spdlog::error("avcodec_alloc_context3 for video failed");
    return;
  } else {
    spdlog::debug("avcodec_alloc_context3 for video success.");
  }
  // open codec
  if (avcodec_parameters_to_context(videoCodecContext, videoStream->codecpar) <
      0) {
    spdlog::error("avcodec_parameters_to_context failed");
    return;
  }
  if (avcodec_open2(videoCodecContext, videoCodec, nullptr) != 0) {
    spdlog::error("avcodec_open2 failed");
    return;
  }
  spdlog::debug("avcodec for video open success.");

  AVFrame *frame = av_frame_alloc();
  AVFrame *prevFrame = nullptr;
  double ptsTimeForContinuityCheck = -1;

  // telecine検出用
  // 5フレーム周期のどこにrepeated-topがあるか
  // idetと同じく半減期を使う
  double telecineDetectCounts[5] = {};

  // 👆を参照するためのカウンタ
  int frameCount = 0;

  while (!terminateFlag) {
    AVPacket *ppacket;
    bool detectTelecineFlag;
    {
      std::unique_lock<std::mutex> lock(videoPacketMtx);
      // メインループの処理の頻度が下がったときにパケットが蓄積しないようにする
      if (videoFrameQueue.size() >= MAX_VIDEO_FRAME_QUEUE_SIZE &&
          estimatedAudioPlayTime != -1) {
        bool showFlag = true;
        for (AVFrame *checkFrame : videoFrameQueue) {
          double ptsTime = checkFrame->pts * av_q2d(checkFrame->time_base);
          showFlag = estimatedAudioPlayTime > ptsTime;
          if (!showFlag) {
            break;
          }
        }
        // リップシンク条件を満たしたフレームでキューが一杯のとき
        if (showFlag) {
          // 参照中かもしれない先頭要素を残してすべてスキップする
          while (videoFrameQueue.size() > 1) {
            auto popFrame = videoFrameQueue.back();
            videoFrameQueue.pop_back();
            if (!!av_dict_get(popFrame->metadata, "ts-live.discontinuity",
                              nullptr, 0)) {
              // 次フレームを不連続にする
              ptsTimeForContinuityCheck = -1;
            }
            av_frame_free(&popFrame);
          }
        }
      }
      videoPacketCv.wait(lock, [&] {
        return (videoFrameQueue.size() < MAX_VIDEO_FRAME_QUEUE_SIZE &&
                !videoPacketQueue.empty()) ||
               terminateFlag;
      });
      if (terminateFlag) {
        break;
      }
      ppacket = videoPacketQueue.front();
      videoPacketQueue.pop_front();
      detectTelecineFlag = detelecineMode != DetelecineMode::NEVER;
    }
    AVPacket &packet = *ppacket;

    int ret = avcodec_send_packet(videoCodecContext, &packet);
    if (ret != 0) {
      spdlog::error("avcodec_send_packet(video) failed: {} {}", ret,
                    av_err2str(ret));
      // return;
    }
    while (avcodec_receive_frame(videoCodecContext, frame) == 0) {
      const AVPixFmtDescriptor *desc =
          av_pix_fmt_desc_get((AVPixelFormat)(frame->format));
      int bufferSize = av_image_get_buffer_size((AVPixelFormat)frame->format,
                                                frame->width, frame->height, 1);
      spdlog::debug("VideoFrame: {}x{}x{} pixfmt:{} key:{} interlace:{} "
                    "tff:{} codecContext->field_order:?? pts:{} "
                    "stream.timebase:{} bufferSize:{}",
                    frame->width, frame->height, frame->ch_layout.nb_channels,
                    frame->format, frame->flags & AV_FRAME_FLAG_KEY,
                    frame->flags & AV_FRAME_FLAG_INTERLACED,
                    frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST, frame->pts,
                    av_q2d(videoStream->time_base), bufferSize);
      if (desc == nullptr) {
        spdlog::debug("desc is NULL");
      } else {
        spdlog::debug(
            "desc name:{} nb_components:{} comp[0].plane:{} .offet:{} "
            "comp[1].plane:{} .offset:{} comp[2].plane:{} .offset:{}",
            desc->name, desc->nb_components, desc->comp[0].plane,
            desc->comp[0].offset, desc->comp[1].plane, desc->comp[1].offset,
            desc->comp[2].plane, desc->comp[2].offset);
      }
      spdlog::debug(
          "buf[0]size:{} buf[1].size:{} buf[2].size:{} buffer_size:{}",
          frame->buf[0]->size, frame->buf[1]->size, frame->buf[2]->size,
          bufferSize);
      if (initPts < 0) {
        initPts = frame->pts;
      }
      frame->time_base.den = videoStream->time_base.den;
      frame->time_base.num = videoStream->time_base.num;

      // time_base が 0/0 な不正フレームは捨てる
      // yuv420p以外はたぶん来ないが一応確認する
      if (frame->time_base.den != 0 && frame->time_base.num != 0 &&
          frame->format == AV_PIX_FMT_YUV420P) {
        AVFrame *cloneFrame = av_frame_clone(frame);
        double ptsTime = frame->pts * av_q2d(frame->time_base);
        if (ptsTimeForContinuityCheck == -1 ||
            ptsTimeForContinuityCheck < ptsTime - 1 ||
            ptsTimeForContinuityCheck > ptsTime + 1) {
          // 不連続であることをフレームに記録する
          av_dict_set(&cloneFrame->metadata, "ts-live.discontinuity", "1", 0);
        }
        ptsTimeForContinuityCheck = ptsTime;
        if (detectTelecineFlag) {
          detectTelecine(cloneFrame, prevFrame, telecineDetectCounts,
                         frameCount);
        } else {
          av_frame_free(&prevFrame);
        }
        std::lock_guard<std::mutex> lock(videoPacketMtx);
        videoFrameFound = true;

        videoFrameQueue.push_back(cloneFrame);
      }
    }
    av_packet_free(&ppacket);
  }
  av_frame_free(&prevFrame);
  av_frame_free(&frame);

  spdlog::debug("freeing videoCodecContext");
  avcodec_free_context(&videoCodecContext);
}

AVFilterGraph *allocAudioFilterGraph(double tempo, int sampleRate,
                                     AVSampleFormat format,
                                     const AVChannelLayout &chLayout,
                                     AVFilterContext *&abufferContext,
                                     AVFilterContext *&abuffersinkContext) {
  const AVFilter *abuffer = avfilter_get_by_name("abuffer");
  const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
  const AVFilter *aformat = avfilter_get_by_name("aformat");
  const AVFilter *atempo = avfilter_get_by_name("atempo");
  if (!abuffer || !abuffersink || !aformat || !atempo) {
    return nullptr;
  }
  AVFilterGraph *graph = avfilter_graph_alloc();
  if (!graph) {
    return nullptr;
  }

  AVFilterContext *aformatContext;
  AVBPrint desc;
  av_bprint_init(&desc, 0, AV_BPRINT_SIZE_UNLIMITED);
  if (av_channel_layout_describe_bprint(&chLayout, &desc) < 0) {
    av_bprint_finalize(&desc, nullptr);
  } else {
    std::string args =
        fmt::format("sample_rate={}:sample_fmt={}:channel_layout={}",
                    sampleRate, av_get_sample_fmt_name(format), desc.str);
    av_bprint_finalize(&desc, nullptr);
    if (avfilter_graph_create_filter(&abufferContext, abuffer, "af_in",
                                     args.c_str(), nullptr, graph) >= 0 &&
        avfilter_graph_create_filter(&abuffersinkContext, abuffersink, "af_out",
                                     nullptr, nullptr, graph) >= 0 &&
        avfilter_graph_create_filter(
            &aformatContext, aformat, "af_format",
            "sample_rates=48000:sample_fmts=fltp:channel_layouts=stereo",
            nullptr, graph) >= 0) {
      if (tempo != 1.0) {
        // atempoフィルタを挟む
        AVFilterContext *atempoContext;
        if (avfilter_graph_create_filter(&atempoContext, atempo, "af_tempo",
                                         fmt::format("{}", tempo).c_str(),
                                         nullptr, graph) >= 0 &&
            avfilter_link(abufferContext, 0, atempoContext, 0) >= 0 &&
            avfilter_link(atempoContext, 0, aformatContext, 0) >= 0 &&
            avfilter_link(aformatContext, 0, abuffersinkContext, 0) >= 0 &&
            avfilter_graph_config(graph, nullptr) >= 0) {
          return graph;
        }
      } else if (avfilter_link(abufferContext, 0, aformatContext, 0) >= 0 &&
                 avfilter_link(aformatContext, 0, abuffersinkContext, 0) >= 0 &&
                 avfilter_graph_config(graph, nullptr) >= 0) {
        return graph;
      }
    }
  }
  avfilter_graph_free(&graph);
  return nullptr;
}

void audioDecoderThreadFunc(bool &terminateFlag) {
  const AVCodec *audioCodec =
      avcodec_find_decoder(audioStreamList[0]->codecpar->codec_id);
  if (audioCodec == nullptr) {
    spdlog::error("No supported decoder for Audio ...");
    return;
  } else {
    spdlog::debug("Audio Decoder created.");
  }
  AVCodecContext *audioCodecContext = avcodec_alloc_context3(audioCodec);
  if (audioCodecContext == nullptr) {
    spdlog::error("avcodec_alloc_context3 for audio failed");
    return;
  } else {
    spdlog::debug("avcodec_alloc_context3 for audio success.");
  }
  // open codec
  if (avcodec_parameters_to_context(audioCodecContext,
                                    audioStreamList[0]->codecpar) < 0) {
    spdlog::error("avcodec_parameters_to_context failed");
    return;
  }

  if (avcodec_open2(audioCodecContext, audioCodec, nullptr) != 0) {
    spdlog::error("avcodec_open2 failed");
    return;
  }
  spdlog::debug("avcodec for audio open success.");

  // 巻き戻す
  // inputBufferReadIndex = 0;

  AVFrame *frame = av_frame_alloc();

  bool reallocGraph = true;
  int currentSampleRate = 0;
  int currentFormat = -1;
  AVChannelLayout currentChLayout = {};
  AVChannelLayout defaultStereoChLayout;
  av_channel_layout_default(&defaultStereoChLayout, 2);
  AVFilterGraph *filterGraph = nullptr;
  AVFilterContext *abufferContext = nullptr;
  AVFilterContext *abuffersinkContext = nullptr;
  AVFrame *filtFrame = av_frame_alloc();

  while (!terminateFlag) {
    AVPacket *ppacket;
    {
      std::unique_lock<std::mutex> lock(audioPacketMtx);
      audioPacketCv.wait(
          lock, [&] { return !audioPacketQueue.empty() || terminateFlag; });
      if (terminateFlag) {
        break;
      }
      ppacket = audioPacketQueue.front();
      audioPacketQueue.pop_front();
      if (currentAudioTempo != targetAudioTempo) {
        currentAudioTempo = targetAudioTempo;
        reallocGraph = true;
      }
    }
    AVPacket &packet = *ppacket;

    int ret = avcodec_send_packet(audioCodecContext, &packet);
    if (ret != 0) {
      spdlog::error("avcodec_send_packet(audio) failed: {} {}", ret,
                    av_err2str(ret));
      // return;
    }
    while (avcodec_receive_frame(audioCodecContext, frame) == 0) {
      spdlog::debug("AudioFrame: format:{} pts:{} frame timebase:{} stream "
                    "timebase:{} buf[0].size:{} buf[1].size:{} nb_samples:{} "
                    "ch:{}",
                    frame->format, frame->pts, av_q2d(frame->time_base),
                    av_q2d(audioStreamList[0]->time_base),
                    frame->buf[0] ? frame->buf[0]->size : 0,
                    frame->buf[1] ? frame->buf[1]->size : 0, frame->nb_samples,
                    frame->ch_layout.nb_channels);
      if (initPts < 0) {
        initPts = frame->pts;
      }
      frame->time_base = audioStreamList[0]->time_base;
      if (videoFrameFound) {
        if (currentSampleRate != frame->sample_rate ||
            currentFormat != frame->format ||
            av_channel_layout_compare(&currentChLayout, &frame->ch_layout) ==
                1) {
          spdlog::info("AudioFrame {}: sample_rate:{}->{} ch:{}->{}",
                       currentFormat < 0 ? "Received initially" : "Changed",
                       currentSampleRate, frame->sample_rate,
                       currentChLayout.nb_channels,
                       frame->ch_layout.nb_channels);
          currentSampleRate = frame->sample_rate;
          currentFormat = frame->format;
          av_channel_layout_copy(&currentChLayout, &frame->ch_layout);
          reallocGraph = true;
        }
        if (reallocGraph) {
          reallocGraph = false;
          avfilter_graph_free(&filterGraph);
          // atempoフィルタの対応範囲になるように必要なら除数に分ける
          int tempoDiv =
              currentAudioTempo < 0.5 ? (int)(1 / currentAudioTempo) : 1;
          if (currentAudioTempo * tempoDiv != 1.0) {
            filterGraph = allocAudioFilterGraph(
                currentAudioTempo * tempoDiv, currentSampleRate,
                (AVSampleFormat)currentFormat, currentChLayout, abufferContext,
                abuffersinkContext);
            if (!filterGraph) {
              spdlog::error("allocAudioFilterGraph({}) failed",
                            currentAudioTempo * tempoDiv);
            }
          }
          if (!filterGraph &&
              (currentSampleRate != 48000 ||
               currentFormat != AV_SAMPLE_FMT_FLTP ||
               av_channel_layout_compare(&currentChLayout,
                                         &defaultStereoChLayout) == 1)) {
            filterGraph = allocAudioFilterGraph(
                1.0, currentSampleRate, (AVSampleFormat)currentFormat,
                currentChLayout, abufferContext, abuffersinkContext);
            if (!filterGraph) {
              spdlog::error("allocAudioFilterGraph(1.0) failed");
            }
          }
        }

        if (filterGraph) {
          // PTSはそのままで音声サンプル数だけを増減させる
          auto pts = frame->pts;
          ret = av_buffersrc_add_frame(abufferContext, frame);
          if (ret >= 0) {
            while (av_buffersink_get_frame(abuffersinkContext, filtFrame) >=
                   0) {
              // 常にFLTP,48000Hz,stereoのはず
              spdlog::debug("AudioFrame(Filtered): format:{} pts:{} frame "
                            "timebase:{} buf[0].size:{} buf[1].size:{} "
                            "nb_samples:{} ch:{}",
                            filtFrame->format, filtFrame->pts,
                            av_q2d(filtFrame->time_base),
                            filtFrame->buf[0]->size, filtFrame->buf[1]->size,
                            filtFrame->nb_samples,
                            filtFrame->ch_layout.nb_channels);
              filtFrame->pts = pts;
              filtFrame->time_base = audioStreamList[0]->time_base;
              // time_base が 0/0 な不正フレームは捨てる
              if (filtFrame->time_base.den != 0 &&
                  filtFrame->time_base.num != 0) {
                AVFrame *cloneFrame = av_frame_clone(filtFrame);
                std::lock_guard<std::mutex> lock(audioPacketMtx);
                audioFrameQueue.push_back(cloneFrame);
              }
              av_frame_unref(filtFrame);
            }
          } else {
            spdlog::error("av_buffersrc_add_frame(audio) failed: {} {}", ret,
                          av_err2str(ret));
          }
        } else {
          // time_base が 0/0 な不正フレームは捨てる
          if (frame->time_base.den != 0 && frame->time_base.num != 0) {
            AVFrame *cloneFrame = av_frame_clone(frame);
            std::lock_guard<std::mutex> lock(audioPacketMtx);
            audioFrameQueue.push_back(cloneFrame);
          }
        }
      }
    }
    av_packet_free(&ppacket);
  }
  av_frame_free(&filtFrame);
  av_frame_free(&frame);
  avfilter_graph_free(&filterGraph);
  av_channel_layout_uninit(&currentChLayout);
  spdlog::debug("freeing videoCodecContext");
  avcodec_free_context(&audioCodecContext);
}

// decoder
void decoderThreadFunc() {
  spdlog::info("Decoder Thread started.");
  resetInternal();
  AVFormatContext *formatContext = nullptr;
  AVIOContext *avioContext = nullptr;
  uint8_t *ibuf = nullptr;
  size_t ibufSize = 64 * 1024;
  size_t requireBufSize = 2 * 1024 * 1024;

  AVFrame *frame = nullptr;

  // probe phase
  {
    // probe
    if (ibuf == nullptr) {
      ibuf = static_cast<uint8_t *>(av_malloc(ibufSize));
    }
    if (avioContext == nullptr) {
      avioContext = avio_alloc_context(ibuf, ibufSize, 0, 0, &read_packet,
                                       nullptr, nullptr);
    }
    if (formatContext == nullptr) {
      formatContext = avformat_alloc_context();
      formatContext->pb = avioContext;
      spdlog::debug("calling avformat_open_input");

      if (avformat_open_input(&formatContext, nullptr, nullptr, nullptr) != 0) {
        spdlog::error("avformat_open_input error");
        return;
      }
      spdlog::debug("open success");
      formatContext->probesize = PROBE_SIZE;
    }

    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
      spdlog::error("avformat_find_stream_info error");
      return;
    }
    spdlog::debug("avformat_find_stream_info success");
    spdlog::debug("nb_streams:{}", formatContext->nb_streams);

    // find video/audio/caption stream
    for (int i = 0; i < (int)formatContext->nb_streams; ++i) {
      spdlog::debug(
          "stream[{}]: tag:{:x} codecName:{} video_delay:{} "
          "dim:{}x{}",
          i, formatContext->streams[i]->codecpar->codec_tag,
          avcodec_get_name(formatContext->streams[i]->codecpar->codec_id),
          formatContext->streams[i]->codecpar->video_delay,
          formatContext->streams[i]->codecpar->width,
          formatContext->streams[i]->codecpar->height);

      if (formatContext->streams[i]->codecpar->codec_type ==
              AVMEDIA_TYPE_VIDEO &&
          videoStream == nullptr) {
        videoStream = formatContext->streams[i];
      }
      if (formatContext->streams[i]->codecpar->codec_type ==
          AVMEDIA_TYPE_AUDIO) {
        audioStreamList.push_back(formatContext->streams[i]);
      }
      if (formatContext->streams[i]->codecpar->codec_type ==
              AVMEDIA_TYPE_SUBTITLE &&
          formatContext->streams[i]->codecpar->codec_id ==
              AV_CODEC_ID_ARIB_CAPTION &&
          captionStream == nullptr) {
        captionStream = formatContext->streams[i];
      }
    }
    if (videoStream == nullptr) {
      spdlog::error("No video stream ...");
      return;
    }
    if (audioStreamList.empty()) {
      spdlog::error("No audio stream ...");
      return;
    }
    spdlog::info("Found video stream index:{} codec:{} dim:{}x{} "
                 "colorspace:{} colorrange:{} delay:{}",
                 videoStream->index,
                 avcodec_get_name(videoStream->codecpar->codec_id),
                 videoStream->codecpar->width, videoStream->codecpar->height,
                 av_color_space_name(videoStream->codecpar->color_space),
                 av_color_range_name(videoStream->codecpar->color_range),
                 videoStream->codecpar->video_delay);
    for (auto &&audioStream : audioStreamList) {
      spdlog::info("Found audio stream index:{} codecID:{} channels:{} "
                   "sample_rate:{}",
                   audioStream->index,
                   avcodec_get_name(audioStream->codecpar->codec_id),
                   audioStream->codecpar->ch_layout.nb_channels,
                   audioStream->codecpar->sample_rate);
    }

    if (captionStream) {
      spdlog::info("Found caption stream index:{} codecID:{}",
                   captionStream->index,
                   avcodec_get_name(captionStream->codecpar->codec_id));
    }
  }

  bool videoTerminateFlag = false;
  bool audioTerminateFlag = false;
  std::thread videoDecoderThread =
      std::thread([&]() { videoDecoderThreadFunc(videoTerminateFlag); });
  std::thread audioDecoderThread =
      std::thread([&]() { audioDecoderThreadFunc(audioTerminateFlag); });

  // decode phase
  while (!resetedDecoder) {
    if (videoPacketQueue.size() >= MAX_VIDEO_PACKET_QUEUE_SIZE) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }
    // decode frames
    if (frame == nullptr) {
      frame = av_frame_alloc();
    }
    AVPacket *ppacket = av_packet_alloc();
    int videoCount = 0;
    int audioCount = 0;
    int ret = av_read_frame(formatContext, ppacket);
    if (ret != 0) {
      spdlog::info("av_read_frame: {} {}", ret, av_err2str(ret));
      continue;
    }
    if (ppacket->stream_index == videoStream->index) {
      AVPacket *clonePacket = av_packet_clone(ppacket);
      {
        std::lock_guard<std::mutex> lock(videoPacketMtx);
        videoPacketQueue.push_back(clonePacket);
        videoPacketCv.notify_all();
      }
    }
    if (audioStreamList.size() > 0 &&
        (ppacket->stream_index ==
         audioStreamList[(int)dualMonoMode % audioStreamList.size()]->index)) {
      AVPacket *clonePacket = av_packet_clone(ppacket);
      {
        std::lock_guard<std::mutex> lock(audioPacketMtx);
        audioPacketQueue.push_back(clonePacket);
        audioPacketCv.notify_all();
      }
    }
    if (ppacket->stream_index == captionStream->index) {
      char buffer[ppacket->size + 2];
      memcpy(buffer, ppacket->data, ppacket->size);
      buffer[ppacket->size + 1] = '\0';
      std::string str = fmt::format("{:02X}", ppacket->data[0]);
      for (int i = 1; i < ppacket->size; i++) {
        str += fmt::format(" {:02x}", ppacket->data[i]);
      }
      spdlog::debug("CaptionPacket received. size: {} data: [{}]",
                    ppacket->size, str);
      if (!captionCallback.isNull()) {
        std::vector<uint8_t> buffer(ppacket->size);
        memcpy(&buffer[0], ppacket->data, ppacket->size);
        {
          std::lock_guard<std::mutex> lock(captionDataMtx);
          int64_t pts = ppacket->pts;
          captionDataQueue.push_back(
              std::make_pair<int64_t, std::vector<uint8_t>>(std::move(pts),
                                                            std::move(buffer)));
        }
      }
    }
    av_packet_free(&ppacket);
  }
  av_frame_free(&frame);

  spdlog::debug("decoderThreadFunc breaked.");

  {
    std::lock_guard<std::mutex> lock(videoPacketMtx);
    videoTerminateFlag = true;
    videoPacketCv.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(audioPacketMtx);
    audioTerminateFlag = true;
    audioPacketCv.notify_all();
  }
  spdlog::debug("join to videoDecoderThread");
  videoDecoderThread.join();
  spdlog::debug("join to audioDecoderThread");
  audioDecoderThread.join();

  spdlog::debug("freeing avio_context");
  avio_context_free(&avioContext);
  // spdlog::debug("freeing avformat context");
  avformat_free_context(formatContext);

  spdlog::debug("decoderThreadFunc end.");
}

std::thread decoderThread;

void initDecoder() {
  // デコーダスレッド起動
  spdlog::info("Starting decoder thread.");
  decoderThread = std::thread([]() {
    while (true) {
      resetedDecoder = false;
      decoderThreadFunc();
    }
  });

  servicefilter.SetProgramNumberOrIndex(-1);
  servicefilter.SetAudio1Mode(13);
  servicefilter.SetAudio2Mode(7);
  servicefilter.SetCaptionMode(1);
  servicefilter.SetSuperimposeMode(2);
}

void decoderMainloop(bool calledByRaf) {
  spdlog::debug("decoderMainloop videoFrameQueue:{} audioFrameQueue:{} "
                "videoPacketQueue:{} audioPacketQueue:{}",
                videoFrameQueue.size(), audioFrameQueue.size(),
                videoPacketQueue.size(), audioPacketQueue.size());

  discardMutedAudioSamples();

  static bool telecineFlag = false;
  if (videoStream && !audioStreamList.empty() && !statsCallback.isNull()) {
    auto data = emscripten::val::object();
    data.set("time", currentPlaybackTime / 1000.0);
    data.set("VideoFrameQueueSize", videoFrameQueue.size());
    data.set("AudioFrameQueueSize", audioFrameQueue.size());
    data.set("AudioWorkletBufferSize", bufferedAudioSamples);
    data.set("InputBufferSize",
             (inputBufferWriteIndex - inputBufferReadIndex) / 1000000.0);
    data.set("CaptionDataQueueSize",
             captionStream ? captionDataQueue.size() : 0);
    if (detelecineMode != DetelecineMode::NEVER) {
      data.set("TelecineFlag", telecineFlag);
    }
    statsBuffer.push_back(std::move(data));
    if (!calledByRaf) {
      auto statsArray = emscripten::val::array();
      for (int i = 0; i < statsBuffer.size(); i++) {
        statsArray.set(i, statsBuffer[i]);
      }
      statsBuffer.clear();
      statsCallback(statsArray);
    }
  }

  // requestAnimationFrameによる呼び出しが一時停止している(rafPauseCount>1)かどうか
  static int64_t rafPauseCount = 0;
  if (calledByRaf ? (rafPauseCount > 1) : (rafPauseCount == 1)) {
    spdlog::debug("rafPauseCount:{}->{}", rafPauseCount,
                  calledByRaf ? 0 : rafPauseCount + 1);
  }
  rafPauseCount = calledByRaf ? 0 : rafPauseCount + 1;
  // なるべくリフレッシュレートに合わせて処理するため
  if (!calledByRaf && rafPauseCount <= 1) {
    return;
  }

  AVFrame *currentFrame = nullptr;
  int audioTempoDiv;
  {
    // estimatedAudioPlayTimeを計算するため
    static std::chrono::steady_clock::time_point lastTime;
    auto nowTime = std::chrono::steady_clock::now();
    double audioPtsTime = -1;
    double audioTempo;
    {
      std::lock_guard<std::mutex> lock(audioPacketMtx);
      if (!audioFrameQueue.empty()) {
        // AudioのPTSをクロックから時間に直す
        // TODO: クロック一回転したときの処理
        audioPtsTime = audioFrameQueue.front()->pts *
                       av_q2d(audioFrameQueue.front()->time_base);
      }
      audioTempo = currentAudioTempo;
    }
    // ここはatempoフィルタに適用する計算式と一致させること
    audioTempoDiv = audioTempo < 0.5 ? (int)(1 / audioTempo) : 1;

    std::lock_guard<std::mutex> lock(videoPacketMtx);
    if (audioPtsTime != -1) {
      // 経過時間から予測した今回のestimatedAudioPlayTime
      double predictedTime =
          estimatedAudioPlayTime +
          std::chrono::duration<double>(nowTime - lastTime).count() *
              audioTempo;
      // 音声のPTSとバッファ量から計算した今回のestimatedAudioPlayTime
      double measuredTime =
          audioPtsTime - (double)bufferedAudioSamples * audioTempo / 48000;
      // 上記から推定される、現在再生している音声のPTS（時間）
      // 基本的にはmeasuredTimeに従うがbufferedAudioSamplesの更新間隔の都合など
      // でゆらぐので、予測とのずれが小さいときはpredictedTimeも参照する
      if (predictedTime > measuredTime - 0.5 &&
          predictedTime < measuredTime + 0.5) {
        estimatedAudioPlayTime = predictedTime * 0.9 + measuredTime * 0.1;
      } else {
        estimatedAudioPlayTime = measuredTime;
      }
    } else {
      estimatedAudioPlayTime = -1;
    }
    if (!videoFrameQueue.empty()) {
      currentFrame = videoFrameQueue.front();
    }
    lastTime = nowTime;
  }

  if (currentFrame && estimatedAudioPlayTime != -1) {
    // 次のVideoFrameをまずは見る（条件を満たせばpopする）
    // spdlog::info("found Current Frame {}x{} bufferSize:{}",
    // currentFrame->width,
    //              currentFrame->height, bufferSize);
    spdlog::debug(
        "VideoFrame@mainloop pts:{} time_base:{} {}/{} AudioQueueSize:{}",
        currentFrame->pts, av_q2d(currentFrame->time_base),
        currentFrame->time_base.num, currentFrame->time_base.den,
        audioFrameQueue.size());

    // WindowSize確認＆リサイズ
    // TODO:
    // if (ww != videoStream->codecpar->width ||
    //     wh != videoStream->codecpar->height) {
    //   set_style(videoStream->codecpar->width);
    // }

    // VideoのPTSをクロックから時間に直す
    // TODO: クロック一回転したときの処理
    double videoPtsTime = currentFrame->pts * av_q2d(currentFrame->time_base);

    // 1フレーム分くらいはズレてもいいからこれでいいか。フレーム真面目に考えると良くわからない。
    static double videoPtsAdjustment = 0;
    bool showFlag = estimatedAudioPlayTime > videoPtsTime + videoPtsAdjustment;

    // リップシンク条件を満たしてたらVideoFrame再生
    if (showFlag) {
      {
        std::lock_guard<std::mutex> lock(videoPacketMtx);
        videoFrameQueue.pop_front();
        // キューが減ることでスレッドがデコードを再開するかもしれないため
        videoPacketCv.notify_all();
      }

      int64_t ptsDiff = 0;
      int64_t ptsTime = (int64_t)(videoPtsTime * 1000);
      if (currentPlaybackPtsTime == -1 ||
          !!av_dict_get(currentFrame->metadata, "ts-live.discontinuity",
                        nullptr, 0)) {
        // 初期状態か不連続なのでリセット
        currentPlaybackPtsTime = ptsTime;
      } else if (currentPlaybackPtsTime < ptsTime) {
        // 再生時刻を増やす
        ptsDiff = ptsTime - currentPlaybackPtsTime;
        currentPlaybackTime += ptsDiff;
        currentPlaybackPtsTime = ptsTime;
      }

      // 表示されてなさそうなときは間引く
      if (rafPauseCount <= 1 || rafPauseCount % 10 == 2) {
        // このフレームの本来の表示期間を推測する。外れ値は補正する
        double frameDuration = (ptsDiff > 200 ? 200 : ptsDiff) / 1000.0;

        telecineFlag = detelecineMode != DetelecineMode::NEVER;
        if (telecineFlag) {
          telecineFlag = false;
          auto entry = av_dict_get(currentFrame->metadata,
                                   "ts-live.frame_cycle", nullptr, 0);
          if (entry) {
            telecineFlag = detelecineMode == DetelecineMode::FORCE ||
                           av_dict_get(currentFrame->metadata,
                                       "ts-live.is_telecine", nullptr, 0);
            if (telecineFlag) {
              // フレームレートを4/5に下げて5枚ごとに1枚だけスキップする
              long adjusted = strtol(entry->value, nullptr, 10);
              // drawWebGpu()は少なくとも1フレーム遅れるので打ち消す
              videoPtsAdjustment =
                  frameDuration * ((adjusted == 0 ? 5 : adjusted * 2) - 8) / 8;
              drawWebGpu(currentFrame, adjusted != 1);
            }
          }
        }
        if (!telecineFlag) {
          // drawWebGpu()は少なくとも1フレーム遅れるので打ち消す
          videoPtsAdjustment = -frameDuration;
          drawWebGpu(currentFrame, true);
        }
      }

      av_frame_free(&currentFrame);
    }
  }

  if (!captionCallback.isNull() && estimatedAudioPlayTime != -1) {
    while (captionDataQueue.size() > 0) {
      std::pair<int64_t, std::vector<uint8_t>> p;
      {
        std::lock_guard<std::mutex> lock(captionDataMtx);
        p = std::move(captionDataQueue.front());
        captionDataQueue.pop_front();
      }
      double pts = (double)p.first;
      std::vector<uint8_t> &buffer = p.second;
      double ptsTime = pts * av_q2d(captionStream->time_base);

      auto data = emscripten::val(
          emscripten::typed_memory_view<uint8_t>(buffer.size(), &buffer[0]));
      captionCallback(pts, ptsTime - estimatedAudioPlayTime, data);
    }
  }

  // AudioFrameはVideoFrame処理でのPTS参照用に1個だけキューに残す
  while (audioFrameQueue.size() > 1) {
    AVFrame *frame = nullptr;
    {
      std::lock_guard<std::mutex> lock(audioPacketMtx);
      frame = audioFrameQueue.front();
      audioFrameQueue.pop_front();
    }
    spdlog::debug("AudioFrame@mainloop pts:{} time_base:{} nb_samples:{} ch:{}",
                  frame->pts, av_q2d(frame->time_base), frame->nb_samples,
                  frame->ch_layout.nb_channels);

    if (frame->sample_rate == 48000 && frame->format == AV_SAMPLE_FMT_FLTP &&
        frame->ch_layout.nb_channels == 2) {
      if (audioTempoDiv > 1) {
        // 無音にして除数分だけ引き伸ばす
        float *zeroData = reinterpret_cast<float *>(frame->data[0]);
        for (int i = 0; i < frame->nb_samples; i++) {
          zeroData[i] = 0;
        }
        for (int i = 0; i < audioTempoDiv; i++) {
          feedAudioData(zeroData, zeroData, frame->nb_samples);
        }
      } else {
        feedAudioData(reinterpret_cast<float *>(frame->data[0]),
                      reinterpret_cast<float *>(frame->data[1]),
                      frame->nb_samples);
      }
    }

    av_frame_free(&frame);
  }
}

void downloadNextRange() {
  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  strcpy(attr.requestMethod, "GET");
  attr.attributes =
      EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
  std::string range = fmt::format("bytes={}-{}", downloadCount,
                                  downloadCount + donwloadRangeSize - 1);
  const char *headers[] = {"Range", range.c_str(), NULL};
  attr.requestHeaders = headers;

  spdlog::debug("request {} Range: {}", playFileUrl, range);
  emscripten_fetch_t *fetch = emscripten_fetch(&attr, playFileUrl.c_str());
  if (fetch->status == 206) {
    spdlog::debug("fetch success size: {}", fetch->numBytes);
    {
      std::lock_guard<std::mutex> lock(inputBufferMtx);
      if (inputBufferWriteIndex + fetch->numBytes >= MAX_INPUT_BUFFER) {
        size_t remainSize = inputBufferWriteIndex - inputBufferReadIndex;
        memcpy(&inputBuffer[0], &inputBuffer[inputBufferReadIndex], remainSize);
        inputBufferReadIndex = 0;
        inputBufferWriteIndex = remainSize;
      }
      memcpy(&inputBuffer[inputBufferWriteIndex], &fetch->data[0],
             fetch->numBytes);
      inputBufferWriteIndex += fetch->numBytes;
      downloadCount += fetch->numBytes;
      waitCv.notify_all();
    }
  } else {
    spdlog::error("fetch failed URL: {} status code: {}", playFileUrl,
                  fetch->status);
  }
  emscripten_fetch_close(fetch);
}

void downloaderThraedFunc() {
  resetedDownloader = false;
  while (!resetedDownloader) {
    size_t remainSize;
    {
      std::lock_guard<std::mutex> lock(inputBufferMtx);
      remainSize = inputBufferWriteIndex - inputBufferReadIndex;
    }
    if (remainSize < donwloadRangeSize / 2) {
      downloadNextRange();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void playFile(std::string url) {
  spdlog::info("playFile: {}", url);
  playFileUrl = url;
  downloaderThread = std::thread([]() { downloaderThraedFunc(); });
}
