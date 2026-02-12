#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include "../util/util.hpp"

int bufferedAudioSamples = 0;
static bool audioWorkletStarted = false;
static bool audioWorkletPaused = false;

void setBufferedAudioSamples(int samples) {
  // set buffereredAudioSamples
  bufferedAudioSamples = samples;
}

static void startAudioWorklet(double gainValue);

void feedAudioData(float *buffer0, float *buffer1, int samples) {
  if (!audioWorkletStarted &&
      // clang-format off
      EM_ASM_INT({return Module.myAudio && Module.myAudio.discard ? 0 : 1;})
      // clang-format on
  ) {
    startAudioWorklet(1.0);
  }

  // clang-format off
  EM_ASM({
    if (Module.myAudio && Module.myAudio.discard) {
      const buffer0 = HEAPF32.slice($0>>2, ($0>>2) + $2);
      const buffer1 = HEAPF32.slice($1>>2, ($1>>2) + $2);
      Module.myAudio.discard.samples.push({
        buffer0: buffer0,
        buffer1: buffer1
      });
      return;
    }
    if (Module && Module['myAudio'] && Module['myAudio']['ctx'] && Module['myAudio']['ctx'].state === 'suspended') {
      Module['myAudio']['ctx'].resume()
    }
    if (Module && Module['myAudio'] && Module['myAudio']['node']) {
      const buffer0 = HEAPF32.slice($0>>2, ($0>>2) + $2);
      const buffer1 = HEAPF32.slice($1>>2, ($1>>2) + $2);
      Module['myAudio']['node'].port.postMessage({
        type: 'feed',
        buffer0: buffer0,
        buffer1: buffer1
      }, [buffer0.buffer, buffer1.buffer]);
    }
  }, buffer0, buffer1, samples);
  // clang-format on
}

void clearAudioSamples() {
  // clang-format off
  EM_ASM({
    if (Module.myAudio && Module.myAudio.discard) {
      Module.myAudio.discard = {samples: []};
      Module.setBufferedAudioSamples(0);
      return;
    }
    if (Module.myAudio && Module.myAudio.node) {
      Module.myAudio.node.port.postMessage({type: 'reset'});
    }
  });
  // clang-format on
}

void pauseAudioWorklet() {
  audioWorkletPaused = true;
  // clang-format off
  EM_ASM({
    if (Module.myAudio && Module.myAudio.node) {
      Module.myAudio.node.port.postMessage({type: 'pause'});
    }
  });
  // clang-format on
}

void resumeAudioWorklet() {
  audioWorkletPaused = false;
  // clang-format off
  EM_ASM({
    if (Module.myAudio && Module.myAudio.node) {
      Module.myAudio.node.port.postMessage({type: 'resume'});
    }
  });
  // clang-format on
}

static void startAudioWorklet(double gainValue) {
  if (audioWorkletStarted) {
    return;
  }
  audioWorkletStarted = true;

  std::string scriptSource = slurp("/processor.js");

  // clang-format off
  EM_ASM({
    (async function(){
      const audioContext = new AudioContext({sampleRate: 48000});
      await audioContext.audioWorklet.addModule(
          URL.createObjectURL(new Blob([UTF8ToString($0)], {type: 'text/javascript'})));
      const audioNode = new AudioWorkletNode(
          audioContext, 'audio-feeder-processor',
          {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]});
      const gainNode = audioContext.createGain();
      audioNode.connect(gainNode);
      gainNode.connect(audioContext.destination);
      console.log('AudioSetup OK');
      let samples = [];
      if (Module.myAudio && Module.myAudio.discard) {
        samples = Module.myAudio.discard.samples;
      }
      Module['myAudio'] = {ctx: audioContext, node: audioNode, gain: gainNode};
      audioContext.resume();
      audioNode.port.onmessage = e => {Module.setBufferedAudioSamples(e.data)};
      console.log('latency', Module['myAudio']['ctx'].baseLatency);
      Module.myAudio.gain.gain.setValueAtTime($1,
                                                Module.myAudio.ctx.currentTime);
      if ($2) {
        audioNode.port.postMessage({type: 'pause'});
      }
      // まだ捨てていないAudioWorklet起動前の入力を拾う
      while (samples.length > 0) {
        audioNode.port.postMessage({
          type: 'feed',
          buffer0: samples[0].buffer0,
          buffer1: samples[0].buffer1
        }, [samples[0].buffer0.buffer, samples[0].buffer1.buffer]);
        samples.shift();
      }
    })();
  }, scriptSource.c_str(), gainValue, audioWorkletPaused);
  // clang-format on
}

void discardMutedAudioSamples() {
  // clang-format off
  EM_ASM({
    if (Module.myAudio && Module.myAudio.discard) {
      // AudioWorklet起動前の入力を等速で捨てるため
      const discard = Module.myAudio.discard;
      if ($0) {
        discard.baseTime = 0;
      }
      while (discard.samples.length > 0) {
        if (!discard.baseTime) discard.baseTime = performance.now();
        const duration = discard.samples[0].buffer0.length / (48000 / 1000);
        if (discard.baseTime + duration > performance.now()) break;
        discard.baseTime += duration;
        discard.samples.shift();
      }
      let sum = 0;
      for (let i = 0; i < discard.samples.length; i++) {
        sum += discard.samples[i].buffer0.length;
      }
      Module.setBufferedAudioSamples(sum);
    }
  }, audioWorkletPaused);
  // clang-format on
}

void setAudioGain(double val) {
  if (val != 0.0) {
    startAudioWorklet(val);
  }

  // clang-format off
  EM_ASM(
      {
        if ($0 == 0.0 && !Module.myAudio) {
          Module.myAudio = {discard: {samples: []}};
        }
        if (Module.myAudio && Module.myAudio.gain)
          Module.myAudio.gain.gain.setValueAtTime($0,
                                                Module.myAudio.ctx.currentTime);
      },
      val);
  // clang-format on
}
