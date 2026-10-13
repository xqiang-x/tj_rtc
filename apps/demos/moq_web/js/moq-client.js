// MoQ client over WebTransport (h3), speaking the moq-transport draft-05
// subset implemented by rtc/libs/xquic (versions = 0xff000005).
//
// Wire summary (all fields QUIC varints unless noted):
//   control stream (bidi, client-initiated):
//     CLIENT_SETUP    0x40 versions_num versions[] params_num params[]
//     SERVER_SETUP    0x41 version params_num params[]
//     SUBSCRIBE       0x03 sub_id alias ns_len ns name_len name filter
//                     [start_g start_o if filter 3|4] [end_g end_o if filter 4]
//                     params_num params[]
//     SUBSCRIBE_OK    0x04 sub_id expire content_exist [largest_g largest_o if exist]
//     SUBSCRIBE_ERROR 0x05 sub_id error_code reason_len reason alias
//     SUBSCRIBE_UPDATE 0x02 sub_id start_g start_o end_g end_o params_num params[]
//   data: one server-initiated uni stream per object:
//     OBJECT_STREAM   0x00 sub_id alias group_id object_id send_order status payload
//   video payload = LOC container:
//     seq_num (vi) timestamp_us (u64 BE) 0x10 flags(0xE0 key | 0xC0 delta) annexb h264
//   catalog payload = JSON (NUL-terminated).
//
// Param encoding: type (vi) length (vi) value bytes.
// Roles: PUBLISHER 0x01, SUBSCRIBER 0x02, PUBSUB 0x03.
// Filters: LAST_GROUP 0x01, LAST_OBJECT 0x02, ABSOLUTE_START 0x03, ABSOLUTE_RANGE 0x04.
// Params: ROLE 0x00, PATH 0x01, AUTH 0x02, EXTDATA 0xA0.

export const MSG = {
  OBJECT_STREAM: 0x0,
  SUBSCRIBE_UPDATE: 0x2,
  SUBSCRIBE: 0x3,
  SUBSCRIBE_OK: 0x4,
  SUBSCRIBE_ERROR: 0x5,
  SUBSCRIBE_DONE: 0xb,
  GOAWAY: 0x10,
  CLIENT_SETUP: 0x40,
  SERVER_SETUP: 0x41,
};

const ROLE = { PUBLISHER: 0x01, SUBSCRIBER: 0x02, PUBSUB: 0x03 };
const FILTER = { LAST_GROUP: 1, LAST_OBJECT: 2, ABSOLUTE_START: 3, ABSOLUTE_RANGE: 4 };
const PARAM = { ROLE: 0x00, PATH: 0x01, AUTH: 0x02, EXTDATA: 0xa0 };

const MOQ_VERSION = 0xff000005;
const REQUEST_TIMEOUT_MS = 10000;

// ── QUIC varint (RFC 9000 §16) ────────────────────────────────────────────

export function varintSize(v) {
  if (v < 1 << 6) return 1;
  if (v < 1 << 14) return 2;
  if (v < 1 << 30) return 4;
  return 8;
}

function writeVarintInto(v, out, off) {
  if (v < 1 << 6) {
    out[off] = v;
    return off + 1;
  }
  if (v < 1 << 14) {
    out[off] = 0x40 | (v >> 8);
    out[off + 1] = v & 0xff;
    return off + 2;
  }
  if (v < 1 << 30) {
    out[off] = 0x80 | (v >>> 24);
    out[off + 1] = (v >>> 16) & 0xff;
    out[off + 2] = (v >>> 8) & 0xff;
    out[off + 3] = v & 0xff;
    return off + 4;
  }
  const big = BigInt(v);
  out[off] = 0xc0 | Number((big >> 56n) & 0x3fn);
  for (let i = 1; i < 8; i++) {
    out[off + i] = Number((big >> BigInt(8 * (7 - i))) & 0xffn);
  }
  return off + 8;
}

export function writeVarint(v) {
  const out = new Uint8Array(varintSize(v));
  writeVarintInto(v, out, 0);
  return out;
}

export class Reader {
  constructor(bytes, pos = 0) {
    this.bytes = bytes;
    this.pos = pos;
  }
  get remaining() {
    return this.bytes.length - this.pos;
  }
  // Returns null when the buffer ends mid-varint (caller should feed more).
  varint() {
    const b = this.bytes;
    if (this.pos >= b.length) return null;
    const first = b[this.pos];
    const len = 1 << (first >> 6);
    if (this.pos + len > b.length) return null;
    let v = first & 0x3f;
    for (let i = 1; i < len; i++) v = v * 256 + b[this.pos + i];
    this.pos += len;
    return v;
  }
  // Returns null when not enough bytes are available.
  raw(n) {
    if (this.pos + n > this.bytes.length) return null;
    const v = this.bytes.subarray(this.pos, this.pos + n);
    this.pos += n;
    return v;
  }
  u8() {
    if (this.pos >= this.bytes.length) return null;
    return this.bytes[this.pos++];
  }
  u64be() {
    const v = this.raw(8);
    if (v === null) return null;
    let x = 0n;
    for (let i = 0; i < 8; i++) x = (x << 8n) | BigInt(v[i]);
    return Number(x);
  }
  str(n) {
    const v = this.raw(n);
    if (v === null) return null;
    return new TextDecoder().decode(v);
  }
}

// ── control message parse ─────────────────────────────────────────────────

// Reads one param list. Returns null when the buffer is short.
function parseParams(cur, count) {
  const out = [];
  for (let i = 0; i < count; i++) {
    const type = cur.varint();
    if (type === null) return null;
    const len = cur.varint();
    if (len === null) return null;
    const value = cur.raw(len);
    if (value === null) return null;
    out.push({ type, value });
  }
  return out;
}

// Parses one control message at cur.pos. Returns null (no message) when the
// buffer is exhausted or incomplete; on an unknown type throws so the caller
// can tear down instead of desynchronizing the stream.
export function parseControlMessage(cur) {
  if (cur.remaining === 0) return null;
  const start = cur.pos;
  const type = cur.varint();
  if (type === null) {
    cur.pos = start;
    return null;
  }
  const need = (v) => {
    if (v === null) {
      cur.pos = start;
      return true;
    }
    return false;
  };
  let msg;
  switch (type) {
    case MSG.SERVER_SETUP: {
      const version = cur.varint();
      const num = cur.varint();
      if (need(version) || need(num)) return null;
      const params = parseParams(cur, num);
      if (params === null) {
        cur.pos = start;
        return null;
      }
      msg = { type, version, params };
      break;
    }
    case MSG.SUBSCRIBE: {
      const subscribeId = cur.varint();
      const alias = cur.varint();
      const nsLen = cur.varint();
      if (need(subscribeId) || need(alias) || need(nsLen)) return null;
      const ns = cur.str(nsLen);
      const nameLen = cur.varint();
      if (need(nsLen) || ns === null || need(nameLen)) {
        cur.pos = start;
        return null;
      }
      const name = cur.str(nameLen);
      const filter = cur.varint();
      if (ns === null || name === null || need(filter)) {
        cur.pos = start;
        return null;
      }
      let startGroup = 0, startObject = 0, endGroup = 0, endObject = 0;
      if (filter === FILTER.ABSOLUTE_START || filter === FILTER.ABSOLUTE_RANGE) {
        startGroup = cur.varint();
        startObject = cur.varint();
        if (need(startGroup) || need(startObject)) {
          cur.pos = start;
          return null;
        }
      }
      if (filter === FILTER.ABSOLUTE_RANGE) {
        endGroup = cur.varint();
        endObject = cur.varint();
        if (need(endGroup) || need(endObject)) {
          cur.pos = start;
          return null;
        }
      }
      const paramsNum = cur.varint();
      if (need(paramsNum)) {
        cur.pos = start;
        return null;
      }
      const params = parseParams(cur, paramsNum);
      if (params === null) {
        cur.pos = start;
        return null;
      }
      msg = {
        type, subscribeId, alias,
        trackNamespace: ns, trackName: name, filter,
        startGroup, startObject, endGroup, endObject, params,
      };
      break;
    }
    case MSG.SUBSCRIBE_OK: {
      const subscribeId = cur.varint();
      const expire = cur.varint();
      const contentExists = cur.varint();
      if (need(subscribeId) || need(expire) || need(contentExists)) return null;
      let largestGroup = 0, largestObject = 0;
      if (contentExists === 1) {
        largestGroup = cur.varint();
        largestObject = cur.varint();
        if (need(largestGroup) || need(largestObject)) {
          cur.pos = start;
          return null;
        }
      }
      msg = { type, subscribeId, expire, contentExists, largestGroup, largestObject };
      break;
    }
    case MSG.SUBSCRIBE_ERROR: {
      const subscribeId = cur.varint();
      const errorCode = cur.varint();
      const reasonLen = cur.varint();
      if (need(subscribeId) || need(errorCode) || need(reasonLen)) return null;
      const reason = cur.str(reasonLen);
      const alias = cur.varint();
      if (reason === null || need(alias)) {
        cur.pos = start;
        return null;
      }
      msg = { type, subscribeId, errorCode, reason, alias };
      break;
    }
    case MSG.SUBSCRIBE_UPDATE: {
      const subscribeId = cur.varint();
      const startGroup = cur.varint();
      const startObject = cur.varint();
      const endGroup = cur.varint();
      const endObject = cur.varint();
      if (need(subscribeId) || need(startGroup) || need(startObject)
          || need(endGroup) || need(endObject)) {
        cur.pos = start;
        return null;
      }
      const paramsNum = cur.varint();
      if (need(paramsNum)) {
        cur.pos = start;
        return null;
      }
      const params = parseParams(cur, paramsNum);
      if (params === null) {
        cur.pos = start;
        return null;
      }
      msg = { type, subscribeId, startGroup, startObject, endGroup, endObject, params };
      break;
    }
    default:
      throw new Error(`unsupported control message type 0x${type.toString(16)}`);
  }
  return msg;
}

// ── message builders ──────────────────────────────────────────────────────

export function buildClientSetup({ extdata } = {}) {
  const parts = [];
  const b = [];
  // header: type, versions_num, version, params_num
  const header = new Uint8Array(8 + 8 + 8 + 8);
  let off = 0;
  off = writeVarintInto(MSG.CLIENT_SETUP, header, off);
  off = writeVarintInto(1, header, off);
  off = writeVarintInto(MOQ_VERSION, header, off);
  off = writeVarintInto(extdata ? 2 : 1, header, off);
  parts.push(header.subarray(0, off));
  parts.push(writeVarint(PARAM.ROLE));
  parts.push(writeVarint(1));
  parts.push(Uint8Array.of(ROLE.SUBSCRIBER));
  if (extdata) {
    const v = new TextEncoder().encode(extdata);
    const lenBuf = writeVarint(v.length);
    parts.push(writeVarint(PARAM.EXTDATA), lenBuf, v);
  }
  return concat(parts);
}

export function buildSubscribe(subscribeId, alias, ns, name) {
  const nsBuf = new TextEncoder().encode(ns);
  const nameBuf = new TextEncoder().encode(name);
  const head = new Uint8Array(8 * 4);
  let off = 0;
  off = writeVarintInto(MSG.SUBSCRIBE, head, off);
  off = writeVarintInto(subscribeId, head, off);
  off = writeVarintInto(alias, head, off);
  off = writeVarintInto(nsBuf.length, head, off);
  return concat([
    head.subarray(0, off),
    nsBuf,
    writeVarint(nameBuf.length),
    nameBuf,
    writeVarint(FILTER.LAST_GROUP),
    writeVarint(0), // params_num
  ]);
}

export function buildSubscribeOk(subscribeId) {
  const out = new Uint8Array(8 * 4);
  let off = 0;
  off = writeVarintInto(MSG.SUBSCRIBE_OK, out, off);
  off = writeVarintInto(subscribeId, out, off);
  off = writeVarintInto(0, out, off); // expire
  off = writeVarintInto(0, out, off); // content_exist
  return out.subarray(0, off);
}

export function buildSubscribeUpdate(subscribeId, startGroup) {
  const out = new Uint8Array(8 * 7);
  let off = 0;
  off = writeVarintInto(MSG.SUBSCRIBE_UPDATE, out, off);
  off = writeVarintInto(subscribeId, out, off);
  off = writeVarintInto(startGroup, out, off);
  off = writeVarintInto(0, out, off); // start_object
  off = writeVarintInto(0, out, off); // end_group
  off = writeVarintInto(0, out, off); // end_object
  off = writeVarintInto(0, out, off); // params_num
  return out.subarray(0, off);
}

export function concat(chunks) {
  let len = 0;
  for (const c of chunks) len += c.length;
  const out = new Uint8Array(len);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

// ── client ────────────────────────────────────────────────────────────────

export function parseLoc(payload) {
  const cur = new Reader(payload);
  const seq = cur.varint();
  const timestampUs = cur.u64be();
  const idl = cur.u8();
  const seid = cur.u8();
  if (seq === null || timestampUs === null || idl === null || seid === null) {
    throw new Error('LOC payload too short');
  }
  if (idl !== 0x10) throw new Error(`LOC bad ID_L 0x${idl.toString(16)}`);
  let key = false;
  if (seid === 0xe0) key = true;
  else if (seid !== 0xc0) throw new Error(`LOC bad flags 0x${seid.toString(16)}`);
  return { seq, timestampUs, key, data: payload.subarray(cur.pos) };
}

// fingerprint: base64 ("sha-256/xxxx" or bare) or hex; returns Uint8Array.
export function decodeFingerprint(text) {
  let s = (text || '').trim();
  if (!s) return null;
  if (s.startsWith('sha-256/')) s = s.slice('sha-256/'.length);
  if (/^[0-9a-fA-F]{64}$/.test(s)) {
    const out = new Uint8Array(32);
    for (let i = 0; i < 32; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
    return out;
  }
  s = s.replace(/-/g, '+').replace(/_/g, '/');
  while (s.length % 4) s += '=';
  const bin = atob(s);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  if (out.length !== 32) throw new Error('fingerprint must be 32 bytes');
  return out;
}

export class MoqClient {
  constructor() {
    this.listeners = new Map();
    this.wt = null;
    this._ctrlWriter = null;
    this._ctrlBuf = new Uint8Array(0);
    this._nextSubId = 0;
    this._pending = new Map(); // subscribe_id -> {resolve, reject, ns, name}
    this._catalogSubId = null;
    this._catalogResolve = null;
    this._lastGroup = new Map();
    this._closed = false;

    this.stats = { objects: 0, bytes: 0, catalogObjects: 0, dropped: 0 };
  }

  on(type, fn) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(fn);
    return this;
  }

  _emit(type, detail) {
    for (const fn of this.listeners.get(type) || []) {
      try {
        fn(detail);
      } catch (err) {
        console.error(`[moq] listener for '${type}' threw`, err);
      }
    }
  }

  _log(level, text) {
    this._emit('log', { level, text });
  }

  /**
   * Connect a WebTransport session and run CLIENT_SETUP.
   * opts: { fingerprint?: string, extdata?: string, protocols?: string[], timeoutMs?: number }
   * Resolves once SERVER_SETUP has been received.
   */
  async connect(url, opts = {}) {
    const wtOpts = {};
    if (opts.fingerprint) {
      wtOpts.serverCertificateHashes = [
        { algorithm: 'sha-256', value: decodeFingerprint(opts.fingerprint) },
      ];
    }
    this.wt = new WebTransport(url, wtOpts);
    const timeoutMs = opts.timeoutMs || REQUEST_TIMEOUT_MS;
    await withTimeout(this.wt.ready, timeoutMs, 'WebTransport ready');
    this._log('info', `connected ${url}`);

    this.wt.closed
      .then((info) => {
        if (this._closed) return;
        this._closed = true;
        this._emit('close', { reason: 'server', info });
      })
      .catch((err) => {
        if (this._closed) return;
        this._closed = true;
        this._emit('close', { reason: 'error', error: err });
      });

    // control stream: browser WT layer prefixes 0x41+session_id on the wire.
    const ctrl = await withTimeout(this.wt.createBidirectionalStream(),
      timeoutMs, 'create control stream');
    this._ctrlWriter = ctrl.writable.getWriter();
    this._pumpControl(ctrl.readable);
    this._pumpUniStreams();

    const setupDone = new Promise((resolve) => {
      this._setupResolve = resolve;
    });
    await this._ctrlWrite(buildClientSetup({ extdata: opts.extdata }));
    this._log('info', 'CLIENT_SETUP sent (version 0xff000005, role subscriber)');
    const serverSetup = await withTimeout(setupDone, timeoutMs, 'SERVER_SETUP');
    return serverSetup;
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    try {
      this.wt?.close();
    } catch {
      /* already gone */
    }
    this._failPending(new Error('client closed'));
    this._emit('close', { reason: 'local' });
  }

  /** Subscribe the catalog track ("catalog"/"catalog"). Resolves with the parsed JSON. */
  subscribeCatalog() {
    const id = this._nextSubscribeId();
    const p = new Promise((resolve) => {
      this._catalogResolve = resolve;
    });
    this._catalogSubId = id;
    this._sendSubscribe(id, id + 1000, 'catalog', 'catalog');
    return p;
  }

  /** Subscribe the video track. Resolves with {subscribeId} on SUBSCRIBE_OK. */
  subscribeVideo(trackNamespace, trackName) {
    const id = this._nextSubscribeId();
    const p = new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject, namespace: trackNamespace, name: trackName });
    });
    this._sendSubscribe(id, id + 1000, trackNamespace, trackName);
    this._videoSubId = id;
    return p.then(() => ({ subscribeId: id }));
  }

  /** Ask the gateway for a keyframe: SUBSCRIBE_UPDATE starting at the next group. */
  requestKeyframe(subscribeId = this._videoSubId) {
    if (subscribeId == null) throw new Error('no video subscription');
    const last = this._lastGroup.get(subscribeId) || 0;
    this._ctrlWrite(buildSubscribeUpdate(subscribeId, last + 1));
    this._log('info', `SUBSCRIBE_UPDATE sent (start_group=${last + 1}) -> keyframe request`);
  }

  _nextSubscribeId() {
    return ++this._nextSubId;
  }

  _sendSubscribe(id, alias, ns, name) {
    this._ctrlWrite(buildSubscribe(id, alias, ns, name));
    this._log('info', `SUBSCRIBE id=${id} alias=${alias} ${ns}/${name}`);
  }

  async _ctrlWrite(bytes) {
    if (!this._ctrlWriter) throw new Error('control stream not open');
    await this._ctrlWriter.write(bytes);
  }

  async _pumpControl(readable) {
    const reader = readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length) this._onCtrlBytes(value);
      }
    } catch (err) {
      if (!this._closed) this._log('error', `control read: ${err.message}`);
    }
    if (!this._closed) this._emit('close', { reason: 'control-eof' });
  }

  _onCtrlBytes(chunk) {
    this._ctrlBuf = concat([this._ctrlBuf, chunk]);
    for (;;) {
      const cur = new Reader(this._ctrlBuf);
      let msg;
      try {
        msg = parseControlMessage(cur);
      } catch (err) {
        this._log('error', err.message);
        this._handleFatal(err);
        return;
      }
      if (msg === null) break;
      this._ctrlBuf = this._ctrlBuf.subarray(cur.pos);
      try {
        this._handleControl(msg);
      } catch (err) {
        this._log('error', `control handling: ${err.message}`);
      }
    }
  }

  _handleControl(msg) {
    switch (msg.type) {
      case MSG.SERVER_SETUP: {
        const role = msg.params.find((p) => p.type === PARAM.ROLE);
        const roleV = role && role.value.length ? role.value[0] : -1;
        this._log('info', `SERVER_SETUP version=0x${(msg.version >>> 0).toString(16)} role=${roleV}`);
        this._setupResolve?.({ version: msg.version, role: roleV });
        this._emit('setup', { version: msg.version, role: roleV });
        break;
      }
      case MSG.SUBSCRIBE: {
        // The gateway auto-subscribes to our datachannel track. Answer OK so
        // its track becomes ready; we do not publish datachannel objects.
        this._ctrlWrite(buildSubscribeOk(msg.subscribeId));
        this._log('info', `SUBSCRIBE from server ${msg.trackNamespace}/${msg.trackName} -> SUBSCRIBE_OK`);
        this._emit('datachannel', msg);
        break;
      }
      case MSG.SUBSCRIBE_OK: {
        const pending = this._pending.get(msg.subscribeId);
        if (pending) {
          this._pending.delete(msg.subscribeId);
          pending.resolve(msg);
          this._log('info', `SUBSCRIBE_OK id=${msg.subscribeId} content_exist=${msg.contentExists}`);
        } else {
          this._log('info', `SUBSCRIBE_OK for id=${msg.subscribeId} (no pending request)`);
        }
        break;
      }
      case MSG.SUBSCRIBE_ERROR: {
        const pending = this._pending.get(msg.subscribeId);
        const err = new Error(`SUBSCRIBE_ERROR code=${msg.errorCode} reason="${msg.reason}"`);
        if (pending) {
          this._pending.delete(msg.subscribeId);
          pending.reject(err);
        }
        this._log('error', `SUBSCRIBE_ERROR id=${msg.subscribeId} code=${msg.errorCode} reason=${msg.reason}`);
        break;
      }
      case MSG.SUBSCRIBE_UPDATE:
        this._log('info', `SUBSCRIBE_UPDATE from server id=${msg.subscribeId}`);
        break;
      default:
        this._log('warn', `unhandled control message 0x${msg.type.toString(16)}`);
    }
  }

  async _pumpUniStreams() {
    try {
      for await (const stream of this.wt.incomingUnidirectionalStreams) {
        this._readObjectStream(stream);
      }
    } catch (err) {
      if (!this._closed) this._log('error', `incoming streams: ${err.message}`);
    }
  }

  async _readObjectStream(stream) {
    try {
      const reader = stream.getReader();
      const chunks = [];
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        chunks.push(value);
      }
      this._onObjectBytes(concat(chunks));
    } catch (err) {
      this._log('error', `object stream read: ${err.message}`);
    }
  }

  _onObjectBytes(bytes) {
    const cur = new Reader(bytes);
    const type = cur.varint();
    if (type !== MSG.OBJECT_STREAM) {
      this._log('warn', `unexpected uni stream type 0x${type.toString(16)}`);
      return;
    }
    const subscribeId = cur.varint();
    const alias = cur.varint();
    const groupId = cur.varint();
    const objectId = cur.varint();
    const sendOrder = cur.varint();
    const status = cur.varint();
    if ([subscribeId, alias, groupId, objectId, sendOrder, status].some((v) => v === null)) {
      this._log('warn', 'short OBJECT_STREAM header');
      return;
    }
    const payload = bytes.subarray(cur.pos);
    this.stats.objects++;
    this.stats.bytes += bytes.length;

    if (subscribeId === this._catalogSubId) {
      this.stats.catalogObjects++;
      let text = new TextDecoder().decode(payload);
      text = text.replace(/\0+$/, '');
      let json;
      try {
        json = JSON.parse(text);
      } catch (err) {
        this._log('error', `catalog JSON parse: ${err.message}`);
        return;
      }
      this._log('info', `catalog received (${payload.length} bytes, ${json.tracks?.length ?? 0} track(s))`);
      this._catalogResolve?.(json);
      this._catalogResolve = null;
      this._emit('catalog', json);
      return;
    }

    let loc;
    try {
      loc = parseLoc(payload);
    } catch (err) {
      this._log('error', `LOC parse: ${err.message}`);
      return;
    }
    const prev = this._lastGroup.get(subscribeId) || 0;
    if (groupId > prev) this._lastGroup.set(subscribeId, groupId);
    this._emit('frame', {
      subscribeId,
      groupId,
      objectId,
      key: loc.key,
      seq: loc.seq,
      timestampUs: loc.timestampUs,
      data: loc.data,
    });
  }

  _handleFatal(err) {
    if (this._closed) return;
    this._closed = true;
    this._failPending(err);
    this._emit('close', { reason: 'protocol-error', error: err });
    try {
      this.wt?.close();
    } catch {
      /* already gone */
    }
  }

  _failPending(err) {
    for (const [, pending] of this._pending) pending.reject(err);
    this._pending.clear();
  }
}

function withTimeout(promise, ms, what) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error(`${what} timed out after ${ms}ms`)), ms);
    promise.then(
      (v) => {
        clearTimeout(t);
        resolve(v);
      },
      (err) => {
        clearTimeout(t);
        reject(err);
      },
    );
  });
}