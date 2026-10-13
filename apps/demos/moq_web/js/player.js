// WebCodecs H.264 player for MoQ LOC objects.
//
// The gateway relays Annex B access units (start codes 00 00 01), with cached
// SPS/PPS prepended to every key frame. The decoder is configured without a
// description so Chrome accepts Annex B directly. If a payload turns out to
// be AVCC (4-byte length-prefixed NALUs) it is converted one-shot.

export function looksAnnexB(data) {
  if (data.length >= 3 && data[0] === 0 && data[1] === 0 && data[2] === 1) return true;
  if (data.length >= 4 && data[0] === 0 && data[1] === 0 && data[2] === 0 && data[3] === 1) return true;
  return false;
}

export function avccToAnnexB(data) {
  const out = [];
  let off = 0;
  while (off + 4 <= data.length) {
    const len = (data[off] << 24 | data[off + 1] << 16 | data[off + 2] << 8 | data[off + 3]) >>> 0;
    off += 4;
    if (len === 0 || off + len > data.length) return null; // malformed
    const nalu = new Uint8Array(4 + len);
    nalu[3] = 1;
    nalu.set(data.subarray(off, off + len), 4);
    out.push(nalu);
    off += len;
  }
  if (out.length === 0) return null;
  let total = 0;
  for (const c of out) total += c.length;
  const merged = new Uint8Array(total);
  let p = 0;
  for (const c of out) {
    merged.set(c, p);
    p += c.length;
  }
  return merged;
}

export class VideoPlayer {
  /**
   * opts: { maxQueue?: number, onFrame?: (info) => void, onError?: (err) => void }
   */
  constructor(canvas, opts = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.maxQueue = opts.maxQueue ?? 12;
    this.onFrame = opts.onFrame || (() => {});
    this.onError = opts.onError || (() => {});

    this.decoder = null;
    this.awaitKey = true;
    this.format = 'unknown';
    this.codec = null;

    this.counters = {
      decoded: 0,
      dropped: 0,
      bytesIn: 0,
      bytesOut: 0,
      keyFrames: 0,
      width: 0,
      height: 0,
      lastTsUs: 0,
    };
    this._window = []; // {t, bytes} for fps/bitrate
  }

  get running() {
    return this.decoder != null && this.decoder.state === 'configured';
  }

  /**
   * (Re)create the decoder. codec: 'avc1.42E01E' etc.
   * Any frames pushed while awaitKey is set are dropped until a key frame.
   */
  configure(codec) {
    this.close();
    this.codec = codec;
    this.awaitKey = true;
    this.format = 'unknown';
    this.counters.decoded = 0;
    this.counters.dropped = 0;
    this.counters.bytesIn = 0;
    this.counters.bytesOut = 0;
    this.counters.keyFrames = 0;
    this._window = [];
    const self = this;
    this.decoder = new VideoDecoder({
      output(frame) {
        try {
          self._render(frame);
        } finally {
          frame.close();
        }
      },
      error(err) {
        self.onError(err);
      },
    });
    this.decoder.configure({
      codec,
      optimizeForLatency: true,
    });
  }

  /**
   * Push one LOC object. info: { data, key, timestampUs, seq }.
   * Deltas are dropped until the first key frame (and when the decode queue
   * exceeds maxQueue), which keeps join latency snappy.
   */
  push(info) {
    if (!this.running) return;
    let { data, key, timestampUs } = info;
    this.counters.bytesIn += data.length;

    if (this.format === 'unknown') {
      this.format = looksAnnexB(data) ? 'annexb' : 'avcc';
    }
    if (this.format === 'avcc') {
      const converted = avccToAnnexB(data);
      if (converted === null) {
        // Not a valid AVCC chain after all; assume the probe was fooled.
        this.format = 'annexb';
      } else {
        data = converted;
      }
    }

    if (this.awaitKey) {
      if (!key) {
        this.counters.dropped++;
        return;
      }
      this.awaitKey = false;
    }
    if (!key && this.decoder.decodeQueueSize > this.maxQueue) {
      this.counters.dropped++;
      return;
    }

    try {
      this.decoder.decode(new EncodedVideoChunk({
        type: key ? 'key' : 'delta',
        timestamp: timestampUs,
        data,
      }));
    } catch (err) {
      this.onError(err);
      return;
    }
    if (key) this.counters.keyFrames++;
    this.counters.lastTsUs = timestampUs;
    this._window.push({ t: performance.now(), bytes: data.length });
  }

  _render(frame) {
    const w = frame.displayWidth || frame.codedWidth;
    const h = frame.displayHeight || frame.codedHeight;
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.counters.width = w;
      this.counters.height = h;
    }
    this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
    this.counters.decoded++;
    this.counters.bytesOut += w * h * 1.5; // approximate yuv420p bytes
    this.onFrame(this.snapshot());
  }

  /** Current rolling stats: fps/bitrate over the last second. */
  snapshot() {
    const now = performance.now();
    while (this._window.length && now - this._window[0].t > 1000) this._window.shift();
    let bytes = 0;
    for (const e of this._window) bytes += e.bytes;
    return {
      ...this.counters,
      fps: this._window.length,
      kbps: Math.round(bytes * 8 / 1000),
      queue: this.decoder?.decodeQueueSize ?? 0,
      format: this.format,
    };
  }

  close() {
    if (this.decoder) {
      try {
        this.decoder.close();
      } catch {
        /* already closed */
      }
      this.decoder = null;
    }
  }
}