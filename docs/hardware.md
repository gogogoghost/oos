# Audio, Power, Camera, and Codecs

The hardware layer provides native C++ APIs shared by all OOS targets. System
UI code consumes typed state and events from these managers; it must not invoke
shell commands or own Android HAL services directly.

## Architecture

| Area | Reusable API | Nokia 2780 | Nokia 8110 |
| --- | --- | --- | --- |
| Speaker and microphone | `AudioManager` | AAudio | OpenSL ES |
| Vibration | `VibratorManager` | Vibrator HIDL 1.0 | legacy vibrator HAL |
| Battery, wake locks, suspend, and flip/slider | `PowerManager` | Power HIDL, sysfs, RTC, evdev | Power C HAL, sysfs, RTC, evdev |
| Camera and flash | `CameraManager` | HAL1 over Camera HIDL | direct HAL1 and torch sysfs |
| Video codecs | `CodecManager` | Media NDK/Qualcomm OMX | planned |

The public headers contain C++ value types and PIMPL boundaries where needed.
Android HIDL, BufferQueue, AAudio, and Media NDK types stay in implementation
files. The production process can therefore expose a stable internal API
without leaking target transport details. Endpoint differences are supplied by
`oos::device::ServiceConfiguration`.

On Nokia 8110, 48 kHz mono playback passed through OpenSL ES. Microphone input
uses the working 16 kHz normal capture route; the vendor's advertised 48 kHz
low-latency route returned silence. Camera capture produced a valid 640x480
Exif JPEG, torch on/off passed, and battery/USB/slider plus wake-lock lifecycle
passed. RTC wake and deep suspend are implemented but remain below
`validated` until an unplugged suspend/resume cycle is completed.

## Audio and Vibration

`AudioManager::playTone` opens a shared 16-bit PCM AAudio output stream with a
media, voice, ringtone, alarm, or notification usage. `recordWav` opens the
microphone with the voice-recognition preset and writes a finalized PCM WAV,
including measured peak and RMS levels. These synchronous methods are smoke
primitives; production media playback and recording should use long-lived
callback streams owned by the native runtime.

`VibratorManager` exposes timed vibration, stop, and optional amplitude
control. The Nokia 2780 Vibrator HAL correctly reports that amplitude control
is unsupported, so callers must treat vibration as on/off hardware.

The device test validated a 48 kHz mono speaker stream, a 48 kHz mono
microphone recording, and timed vibration. Call audio routing, volume policy,
headset/Bluetooth routing, focus/ducking, and media-file playback remain
separate policy work; those cannot be inferred from a successful PCM smoke
test.

## Power and Flip Lifecycle

`PowerManager` reads battery status, percentage, voltage, current,
temperature, and USB charging state. Its netlink descriptor can be added to
the OOS event loop to receive `power_supply` changes without polling.

The manager also provides:

- Power HAL interactive/non-interactive transitions;
- named kernel wake-lock acquisition with automatic cleanup;
- Android `libsuspend` autosuspend enable/disable and forced suspend;
- RTC wake scheduling and clearing;
- current flip state from the hall sensor and a reusable flip-key state
  reducer for the shared evdev event loop.

The Nokia 2780 hall sensor uses key code 249. Pressed means closed and released
means open, matching the stock Gecko implementation. A production lifecycle
controller must serialize flip state, Web page visibility, panel ownership,
wake locks, and suspend decisions in one native state machine.

Battery queries, the uevent socket, flip-state discovery, wake locks, RTC wake,
and Power HAL display transitions are validated. Forced deep suspend returns
`EBUSY` while USB/ADB remains an active kernel wake source. The same
`libsuspend` entry point is used by stock Gecko, so final suspend/resume must be
tested with USB disconnected from the deployed `/system/oos` runtime.

## Camera and Flash

The stock Nokia 2780 publishes `android.hardware.camera.provider@2.4/legacy/0`
and a working `device@1.0/legacy/0`. Its `cameraserver` process runs but does
not publish the `media.camera` Binder service because conversion of the vendor
Camera 3.4 interface returns null. A Camera2 NDK client therefore waits
indefinitely and is not a viable dependency on this firmware.

`CameraManager` deliberately talks to the working HAL1 HIDL interface. It:

- enumerates camera ID, facing, orientation, JPEG sizes, and flash capability;
- creates an off-screen BufferQueue and drains preview frames;
- maps HAL callback shared memory and writes a complete JPEG;
- controls torch mode through the camera device.

The device test validated one back camera, orientation 90 degrees, a maximum
JPEG size of 2592x1944, torch control, and a complete 1920x1080 baseline JPEG.
The current public API supports still capture and torch. A camera Web UI will
add a retained preview surface or GPU texture bridge rather than copying
preview frames through JavaScript.

## Hardware Video Codec

`CodecManager::testH264RoundTrip` sends generated NV12 frames through Media
NDK, drains the encoder through EOS, supplies its codec configuration and
packets to a decoder, and verifies decoded output through EOS. This is an
active data-path test rather than component-name enumeration.

The Nokia 2780 selected `OMX.qcom.video.encoder.avc` and
`OMX.qcom.video.decoder.avc`; 30 input frames produced 30 decoded frames. Both
components are Qualcomm hardware codecs. Resolution/rate capability discovery,
camera-to-encoder zero-copy recording, audio codecs, muxing, and protected
content are future media-pipeline work.

## On-Device Test

Build, deploy, and run all non-destructive smoke groups with:

```sh
./scripts/test-hardware.sh smoke
```

Individual groups are `audio`, `power`, `camera`, and `codec`. `deploy` only
pushes the binary. `suspend` is explicit because it turns the displays off and
is expected to fail with `EBUSY` while USB/ADB is connected.

The smoke test leaves a microphone WAV and camera JPEG under
`/data/local/tmp` for inspection. It restores the interactive display state,
turns vibration and torch off, closes camera and codec sessions, releases wake
locks, and does not modify the system partition.
