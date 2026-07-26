#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/android-nokia-2780-flip"
BINARY="$BUILD_DIR/bin/tests/nokia-2780-flip/oos_test_nokia_2780_hardware_headless"
REMOTE_BINARY=/data/local/tmp/oos-hardware-test
REMOTE_WAV=/data/local/tmp/oos-mic-test.wav
REMOTE_JPEG=/data/local/tmp/oos-camera-test.jpg
ADB=${ADB:-adb}
MODE=${1:-smoke}

case "$MODE" in
  smoke | audio | power | camera | codec | suspend | deploy) ;;
  *)
    echo "Usage: $0 [smoke|audio|power|camera|codec|suspend|deploy]" >&2
    exit 2
    ;;
esac

cmake --build "$BUILD_DIR" \
  --target oos_test_nokia_2780_hardware_headless -j"$(nproc)"
"$ADB" push "$BINARY" "$REMOTE_BINARY"
"$ADB" shell "su -c 'chmod 755 $REMOTE_BINARY; setenforce 0'"

if [[ "$MODE" == deploy ]]; then
  echo "Deployed $REMOTE_BINARY"
  exit 0
fi

run_test() {
  echo "> $REMOTE_BINARY $*"
  "$ADB" shell "su -c 'timeout 35 $REMOTE_BINARY $*'"
}

if [[ "$MODE" == smoke || "$MODE" == audio ]]; then
  run_test vibrate 500
  run_test audio-tone 1000 440
  run_test audio-record "$REMOTE_WAV" 1000
  "$ADB" shell "su -c 'test -s $REMOTE_WAV'"
fi

if [[ "$MODE" == smoke || "$MODE" == power ]]; then
  run_test power-status
  run_test power-cycle 500
fi

if [[ "$MODE" == smoke || "$MODE" == camera ]]; then
  run_test camera-list
  "$ADB" shell "su -c 'rm -f $REMOTE_JPEG'"
  run_test camera-capture 0 "$REMOTE_JPEG"
  "$ADB" shell "su -c 'test -s $REMOTE_JPEG'"
  run_test torch 0 500
fi

if [[ "$MODE" == smoke || "$MODE" == codec ]]; then
  codec_output=$(run_test codec-h264 30 | tr -d '\r')
  printf '%s\n' "$codec_output"
  for expected in \
    "encoder=OMX.qcom.video.encoder.avc" \
    "encoder_hardware=1" \
    "decoder=OMX.qcom.video.decoder.avc" \
    "decoder_hardware=1" \
    "decoded_frames=30"; do
    if [[ "$codec_output" != *"$expected"* ]]; then
      echo "Missing expected codec result: $expected" >&2
      exit 1
    fi
  done
fi

if [[ "$MODE" == suspend ]]; then
  echo "Deep suspend normally returns EBUSY while USB/ADB is an active wake source."
  run_test suspend-test 5
fi

echo "Hardware test completed."
if [[ "$MODE" == smoke || "$MODE" == audio ]]; then
  echo "Recorded microphone sample: $REMOTE_WAV"
fi
if [[ "$MODE" == smoke || "$MODE" == camera ]]; then
  echo "Captured camera sample: $REMOTE_JPEG"
fi
