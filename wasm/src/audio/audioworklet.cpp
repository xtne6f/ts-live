#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include "../util/util.hpp"

int bufferedAudioSamples = 0;
bool startedAudioWorklet = false;

void setBufferedAudioSamples(int samples) {
  // set buffereredAudioSamples
  bufferedAudioSamples = samples;
}

void startAudioWorklet();

void feedAudioData(float *buffer0, float *buffer1, int samples) {
  if (!startedAudioWorklet &&
      // clang-format off
      EM_ASM_INT({return Module && Module.myAudio && Module.myAudio.discardIntervalId ? 0 : 1;})
      // clang-format on
  ) {
    startAudioWorklet();
  }

  // clang-format off
  EM_ASM({
    if (Module && Module.myAudio && Module.myAudio.discardIntervalId) {
      const buffer0 = HEAPF32.slice($0>>2, ($0>>2) + $2);
      const buffer1 = HEAPF32.slice($1>>2, ($1>>2) + $2);
      Module.myAudio.discardSamples.push({
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

void startAudioWorklet() {
  if (startedAudioWorklet) {
    return;
  }
  startedAudioWorklet = true;

  std::string scriptSource = slurp("/processor.js");

  // clang-format off
  EM_ASM({
    (async function(){
      const audioContext = new AudioContext({sampleRate: 48000});
      await audioContext.audioWorklet.addModule(`data:text/javascript,${encodeURI(UTF8ToString($0))}`);
      const audioNode = new AudioWorkletNode(
          audioContext, 'audio-feeder-processor',
          {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]});
      const gainNode = audioContext.createGain();
      audioNode.connect(gainNode);
      gainNode.connect(audioContext.destination);
      console.log('AudioSetup OK');
      let samples = [];
      if (Module.myAudio && Module.myAudio.discardIntervalId) {
        samples = Module.myAudio.discardSamples;
        clearInterval(Module.myAudio.discardIntervalId);
      }
      Module['myAudio'] = {ctx: audioContext, node: audioNode, gain: gainNode};
      audioContext.resume();
      audioNode.port.onmessage = e => {Module.setBufferedAudioSamples(e.data)};
      console.log('latency', Module['myAudio']['ctx'].baseLatency);
      if (Module.myAudio.gainValue === undefined) {
        Module.myAudio.gainValue = 1.0;
      }
      Module.myAudio.gain.gain.setValueAtTime(Module.myAudio.gainValue,
                                                Module.myAudio.ctx.currentTime);
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
  }, scriptSource.c_str());
  // clang-format on
}

void setAudioGain(double val) {
  if (val != 0.0) {
    startAudioWorklet();
  }

  // clang-format off
  EM_ASM(
      {
        if ($0 == 0.0 && !Module.myAudio) {
          let discardBaseTime = performance.now();
          Module.myAudio = {discardSamples: []};
          Module.myAudio.discardIntervalId = setInterval(() => {
            // AudioWorklet起動前の入力を等速で捨てるため
            const samples = Module.myAudio.discardSamples;
            while (samples.length > 0) {
              const duration = samples[0].buffer0.length / (48000 / 1000);
              if (discardBaseTime + duration > performance.now()) break;
              discardBaseTime += duration;
              samples.shift();
            }
            let sum = 0;
            for (let i = 0; i < samples.length; i++) {
              sum += samples[i].buffer0.length;
            }
            Module.setBufferedAudioSamples(sum);
          }, 30);
        }
        if (Module.myAudio && Module.myAudio.gain)
          Module.myAudio.gain.gain.setValueAtTime($0,
                                                Module.myAudio.ctx.currentTime);
      },
      val);
  // clang-format on
}
