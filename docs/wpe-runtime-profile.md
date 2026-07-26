# WPE Runtime Profile

The Nokia 2780 Flip build targets Android API 29, ARMv7 Thumb-2, NEON/VFP,
and the Android armeabi-v7a softfp ABI. This profile is intended for the OOS
system shell rather than a general-purpose browser.

## Performance-Critical Features

The following features are required and are verified by
`scripts/build-wpe-sysroot.sh`:

- JavaScriptCore Baseline JIT;
- JavaScriptCore DFG JIT;
- WebAssembly with the ARMv7 BBQ JIT;
- accelerated compositing, EGL, GLES, and WebGL;
- the WPE legacy backend API used by the Nokia 2780 display manager.

FTL and WebAssembly OMG are disabled because WebKit does not support those
optimizing tiers on this 32-bit ARM target. The C-loop interpreter is disabled,
so a configuration that accidentally loses JIT support fails the build instead
of silently shipping a slow fallback.

Android uses the base AAPCS softfp ABI at shared-library boundaries. JSC's
ARMv7 generated code expects its internal operation calls to use the VFP
calling convention. The tracked ARMv7 patch marks only those internal entry
points and function-pointer wrappers as `aapcs-vfp`; public and system ABI
boundaries remain softfp.

## Disabled Features

The first release-size pass disables functionality that has no provider or no
planned OOS consumer on the stock KaiOS 3 device:

- GStreamer, video, WebAudio, WebRTC, WebCodecs, media capture, and recording;
- WebXR, gamepad, PDF.js, XSLT, MHTML, notifications, and speech synthesis;
- WebDriver, remote inspector, JavaScript shell, and documentation tooling;
- geolocation, context menus, drag support, fullscreen, cursor visibility,
  touch-event JavaScript APIs, and content extensions;
- MathML, offscreen canvas, image-diff tooling, memory sampling, resource
  usage reporting, periodic memory monitoring, and variation fonts;
- AVIF, LCMS, WOFF2, Skia encoders, Skia OpenType SVG, libbacktrace, and
  sysprof capture.

CSS/HTML/SVG, regular canvas, WebGL, networking, storage, workers, and the
modern JavaScript language/runtime remain enabled. Further removals should be
driven by measured package and resident-memory savings plus application
compatibility tests.

## Runtime Validation

`devices/nokia-2780-flip/assets/hello.html` warms integer and floating-point
JavaScript paths, instantiates integer and floating-point WebAssembly modules,
and reports the result in the DOM. The WPE display test reads that result back
after page load. On a device, JIT validation also requires an anonymous
executable mapping in `WPEWebProcess`; a successful API result alone does not
prove that generated code was enabled.
