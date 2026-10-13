# MoQ Web Demo

Browser playback of an SFU stream over **MoQ (moq-transport draft-05) on
WebTransport**. The path is:

```
pi4_air / pushers ──UDP(FEC)──▶ SFU ──UDS IPC──▶ moq_gateway --wt ──QUIC h3 + WebTransport──▶ this page
                                  (组帧 + FrameDispatcher)          (MoQ publisher)                 (WebCodecs)
```

The page speaks the same wire protocol as `moq_demo_client`: it opens a
WebTransport session, sends `CLIENT_SETUP` (version `0xff000005`, role
subscriber), subscribes the `catalog` track for metadata, then subscribes
`tj/<stream>/video`, reassembles LOC objects and feeds H.264 to WebCodecs.

## Prerequisites

- The SFU server running with its IPC socket (default `/tmp/tj_rtc_sfu.sock`)
  and at least one publishing stream (e.g. `openhd0`).
- The gateway binary built once:

  ```sh
  cd ../../../rtc/libs/xquic
  ./scripts/validate.sh build --feature moq
  # binary: build/validation/moq/demo/moq_gateway
  ```

- Chrome/Edge (WebCodecs + WebTransport). The page must be a secure context:
  `http://localhost` qualifies, LAN pages do not (see below).
- WebTransport version: the gateway advertises draft-07
  (`SETTINGS_WEBTRANSPORT_MAX_SESSIONS 0xc671706a`) plus the draft-02
  aliases used by stock Chromium (`0x2b603742`, `0xffd277`). Client and
  server intersect their version sets, so stock Chrome/Edge negotiate
  draft-02 with **no flags**; Chrome started with
  `--enable-features=EnableWebTransportDraft07` negotiates draft-07.
  Stream and datagram framing is identical on both paths.

## Quick start

```sh
./run_demo.sh --stream openhd0
```

This generates a certificate (`certs/`, ECDSA P-256 / 14 days — required by
Chromium's `serverCertificateHashes`), starts `moq_gateway --wt` and a static
file server, and prints a ready-to-open URL:

```
http://localhost:8080/?url=https%3A%2F%2Flocalhost%3A8443%2Fmoq&ns=tj%2Fopenhd0&track=video&fp=<base64>
```

Open it in Chrome and press **Connect**. Video appears at the first keyframe.

Options: `--port`, `--http-port`, `--ipc`, `--gateway-bin`, `--codec`, `--wt-url`,
`--no-gateway` (page only; pair with `CERT_FP=...`).

## Manual steps

```sh
./gen_cert.sh                       # prints sha-256/<base64> fingerprint
../../../rtc/libs/xquic/build/validation/moq/demo/moq_gateway \
    --wt --wt-cert certs/server.crt --wt-key certs/server.key \
    -p 8443 --stream openhd0
python3 -m http.server 8080 --bind 127.0.0.1
# open http://localhost:8080/?ns=tj%2Fopenhd0&fp=<fingerprint>
```

## Using the page

| Control | Meaning |
| --- | --- |
| WT URL | WebTransport CONNECT endpoint; the gateway ignores the path |
| Fingerprint | pin the self-signed cert (`serverCertificateHashes`); leave empty for CA-signed certs |
| namespace / track | track to subscribe; `tj/openhd0` + `video` matches the gateway layout |
| Codec | WebCodecs codec string; prefilled from the catalog once connected |
| Connect / Disconnect | open/close the session |
| Request keyframe | sends `SUBSCRIBE_UPDATE` starting at the next group; the gateway turns it into a PLI toward the SFU |
| Apply codec | reconfigure the decoder and request a keyframe (use when the stream profile differs from `avc1.42E01E`) |

Stats: resolution, decode fps, video kbps, decoded/dropped/queue, input format
(Annex B or AVCC), MoQ object counters.

Troubleshooting:

- **No video, decoder errors** — the catalog codec string is gateway metadata;
  the actual stream profile comes from the pusher. Try `avc1.4D401F` /
  `avc1.64001F` and press *Apply codec*.
- **`WebTransport ready` times out** — wrong fingerprint, expired cert
  (regenerate with `./gen_cert.sh --force`), or the gateway is not running.
- **`SUBSCRIBE_ERROR track not found`** — the stream is not among the
  gateway's `--stream` list or the namespace is misspelled.
- **Connected but no frames** — check `logs/gateway.log`: the gateway needs
  the SFU IPC socket and a `SUB_OK` for the stream (it retries every few
  seconds until the stream exists, then auto-catches the next keyframe).

## LAN access

`http://192.168.x.x:8080` is not a secure context, so neither WebTransport nor
WebCodecs is enabled by default. For lab testing, add the origin to Chrome's
list: start Chrome with `--unsafely-treat-insecure-origin-as-secure=http://192.168.x.x:8080
--user-data-dir=/tmp/chrome-test` (and `--origin-to-force-quic-on=192.168.x.x:8443`
if QUIC is downgraded). Prefer the localhost flow for everyday testing.

## Files

```
index.html        UI: connect form, canvas, stats, log (prefills from ?url=&ns=&track=&fp=&codec=)
js/moq-client.js  wire protocol: varints, control stream, subscribes, OBJECT_STREAM/LOC
js/player.js      WebCodecs player: Annex B detection, AVCC conversion, keyframe gating, stats
gen_cert.sh       ECDSA P-256 certificate + fingerprint (14 days)
run_demo.sh       gateway --wt + static server in one command
```

## Protocol notes (draft-05 subset)

- Control messages on a browser-initiated bidi stream; server OBJECT_STREAM
  objects arrive as one uni stream each.
- Message types: `OBJECT_STREAM 0x00`, `SUBSCRIBE_UPDATE 0x02`,
  `SUBSCRIBE 0x03`, `SUBSCRIBE_OK 0x04`, `SUBSCRIBE_ERROR 0x05`,
  `CLIENT_SETUP 0x40`, `SERVER_SETUP 0x41`.
- The gateway auto-subscribes the `datachannel` track; the page answers
  `SUBSCRIBE_OK` immediately and does not publish datachannel objects.
- Catalog is a single JSON object (`tracks[]` with `name` + `selectionParams`);
  there is no `SUBSCRIBE_OK` for it — the object itself is the response.
- Video objects carry the LOC container:
  `seq(varint) | timestamp_us(u64 BE) | 0x10 | 0xE0 key / 0xC0 delta | Annex B H.264`.
  Key-frame payloads have cached SPS/PPS prepended, so the decoder runs
  description-less (Annex B input).
- Subscriptions use `SUBSCRIBE_UPDATE(start_group = last_seen + 1)` as a
  keyframe request; the gateway drops stale groups and sends a PLI to the SFU
  (rate-limited to once per second per stream).

## Limitations

- Video only; audio objects are ignored by the page.
- One video subscription at a time.
- No reconnection logic — press Disconnect/Connect after a gateway restart.