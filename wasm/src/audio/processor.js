class AudioFeederProcessor extends AudioWorkletProcessor {
  bufferedSamples = 0
  currentBufferReadSize = 0
  processCallCount = 0
  buffers0 = new Array(0)
  buffers1 = new Array(0)
  started = false
  constructor (...args) {
    super(...args)
    this.port.onmessage = e => {
      if (e.data.type === 'feed') {
        this.buffers0.push(e.data.buffer0)
        this.buffers1.push(e.data.buffer1)
        this.bufferedSamples += e.data.buffer0.length
        this.port.postMessage(this.bufferedSamples)
      } else if (e.data.type === 'reset') {
        this.started = false
        this.currentBufferReadSize = 0
        this.buffers0.length = 0
        this.buffers1.length = 0
        this.bufferedSamples = 0
        this.port.postMessage(0)
      }
    }
  }
  process (inputs, outputs, parameters) {
    const output = outputs[0]

    if (
      this.buffers0.length == 0 ||
      (!this.started && this.bufferedSamples < 48000 / 10)
    ) {
      output[0].fill(0)
      output[1].fill(0)
      return true
    }
    this.started = true

    for (let offset = 0;
         offset < output[0].length && this.bufferedSamples > 0;) {
      const buffer0 = this.buffers0[0]
      const buffer1 = this.buffers1[0]
      const bufferOffset = this.currentBufferReadSize
      let copySize = output[0].length - offset
      this.currentBufferReadSize += copySize
      if (this.currentBufferReadSize >= buffer0.length) {
        copySize = buffer0.length - bufferOffset
        this.currentBufferReadSize = 0
        this.buffers0.shift()
        this.buffers1.shift()
      }
      output[0].set(buffer0.subarray(bufferOffset, bufferOffset + copySize),
                    offset)
      output[1].set(buffer1.subarray(bufferOffset, bufferOffset + copySize),
                    offset)
      offset += copySize
      this.bufferedSamples -= copySize
    }
    if (this.processCallCount++ % 20 == 0) {
      this.port.postMessage(this.bufferedSamples)
    }
    return true
  }
}

registerProcessor('audio-feeder-processor', AudioFeederProcessor)
