#!/usr/bin/env bash
# One-command local demo: moq_gateway --wt + static file server.
#
#   ./run_demo.sh --stream openhd0
#
# Starts nothing else: the SFU (and pi4_air) are only *consumers* through the
# IPC socket, so run this while the regular pipeline is already up. Without an
# SFU the page still connects and subscribes; no video appears until frames
# flow over IPC.
set -euo pipefail
cd "$(dirname "$0")"

STREAM=""
PORT=8443
HTTP_PORT=8080
IPC_PATH=/tmp/tj_rtc_sfu.sock
GATEWAY_BIN="${MOQ_GW_BIN:-}"
WT_URL=""
NO_GATEWAY=0
BIND_ALL=0
EXTRA_GW_ARGS=()

usage() {
  cat <<EOF
usage: ./run_demo.sh --stream <name> [options]

  --stream <name>       SFU stream to relay (required; e.g. openhd0)
  --port <n>            gateway QUIC port (default 8443)
  --http-port <n>       static page port (default 8080)
  --ipc <path>          SFU IPC socket (default /tmp/tj_rtc_sfu.sock)
  --gateway-bin <path>  moq_gateway binary (default: build/validation/moq/demo)
  --wt-url <url>        override the WT URL shown to the browser
  --codec <str>         catalog codec metadata (default avc1.42E01E)
  --no-gateway          do not start a gateway (use an already running one)
  --bind-all            serve the page on 0.0.0.0 (LAN access needs a Chrome flag)
  -h, --help            this help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --stream)     STREAM="$2"; shift 2 ;;
    --port)       PORT="$2"; shift 2 ;;
    --http-port)  HTTP_PORT="$2"; shift 2 ;;
    --ipc)        IPC_PATH="$2"; shift 2 ;;
    --gateway-bin) GATEWAY_BIN="$2"; shift 2 ;;
    --wt-url)     WT_URL="$2"; shift 2 ;;
    --codec)      EXTRA_GW_ARGS+=(--codec "$2"); shift 2 ;;
    --no-gateway) NO_GATEWAY=1; shift ;;
    --bind-all)   BIND_ALL=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
  esac
done

if [ -z "$STREAM" ]; then
  echo "error: --stream is required" >&2
  usage >&2
  exit 1
fi

# ── locate gateway binary ────────────────────────────────────────────────
if [ "$NO_GATEWAY" -eq 0 ] && [ -z "$GATEWAY_BIN" ]; then
  for cand in \
    ../../../rtc/libs/xquic/build/validation/moq/demo/moq_gateway \
    ../../../rtc/libs/xquic/build/demo/moq_gateway; do
    if [ -x "$cand" ]; then
      GATEWAY_BIN="$cand"
      break
    fi
  done
fi
if [ "$NO_GATEWAY" -eq 0 ] && [ -z "$GATEWAY_BIN" ]; then
  echo "error: moq_gateway not found. Build it first:" >&2
  echo "  (cd ../../../rtc/libs/xquic && ./scripts/validate.sh build --feature moq)" >&2
  echo "  or pass --gateway-bin <path>" >&2
  exit 1
fi

# ── certificate + fingerprint ────────────────────────────────────────────
if [ "$NO_GATEWAY" -eq 0 ]; then
  ./gen_cert.sh >/dev/null
  CERT="certs/server.crt"
  KEY="certs/server.key"
  FP=$(openssl x509 -in "$CERT" -outform der | openssl dgst -sha256 -binary | openssl base64)
else
  FP="${CERT_FP:-}"
  [ -n "$FP" ] || echo "warning: --no-gateway without CERT_FP: the browser needs a fingerprint for self-signed certs" >&2
fi

mkdir -p logs
GW_PID=""
HTTP_PID=""

cleanup() {
  [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
  if [ -n "$GW_PID" ]; then
    kill "$GW_PID" 2>/dev/null || true
    wait "$GW_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# ── start gateway ────────────────────────────────────────────────────────
if [ "$NO_GATEWAY" -eq 0 ]; then
  echo "starting gateway: $GATEWAY_BIN (stream=$STREAM, port=$PORT, ipc=$IPC_PATH)"
  stdbuf -oL -eL "$GATEWAY_BIN" \
    --wt --wt-cert "$CERT" --wt-key "$KEY" \
    -p "$PORT" --stream "$STREAM" --ipc "$IPC_PATH" \
    "${EXTRA_GW_ARGS[@]}" \
    > logs/gateway.log 2>&1 &
  GW_PID=$!
  sleep 0.5
  if ! kill -0 "$GW_PID" 2>/dev/null; then
    echo "error: gateway exited during startup — logs/gateway.log:" >&2
    cat logs/gateway.log >&2
    exit 1
  fi
fi

# ── start static server ──────────────────────────────────────────────────
BIND=127.0.0.1
[ "$BIND_ALL" -eq 1 ] && BIND=0.0.0.0
python3 -m http.server "$HTTP_PORT" --bind "$BIND" >/dev/null 2>&1 &
HTTP_PID=$!

# ── print the URL ────────────────────────────────────────────────────────
[ -n "$WT_URL" ] || WT_URL="https://localhost:${PORT}/moq"
PAGE_URL=$(python3 - "$HTTP_PORT" "$WT_URL" "$STREAM" "$FP" <<'PY'
import sys, urllib.parse
http_port, wt_url, stream, fp = sys.argv[1:5]
q = {
    "url": wt_url,
    "ns": f"tj/{stream}",
    "track": "video",
}
if fp:
    q["fp"] = fp
print(f"http://localhost:{http_port}/?{urllib.parse.urlencode(q)}")
PY
)

cat <<EOF

────────────────────────────────────────────────────────────────
  MoQ web demo ready

  open in Chrome:
    $PAGE_URL

  WT endpoint  : $WT_URL
  stream       : tj/$STREAM/video
  fingerprint  : sha-256/$FP
  gateway log  : logs/gateway.log   (pid ${GW_PID:-external})
  stop         : Ctrl-C

  Note: the SFU must be running and publishing '$STREAM' on
  $IPC_PATH for video to appear.
────────────────────────────────────────────────────────────────
EOF

if [ -n "$GW_PID" ]; then
  wait "$GW_PID"
else
  wait "$HTTP_PID"
fi