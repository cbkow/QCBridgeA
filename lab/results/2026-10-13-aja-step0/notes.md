# AJA output, step 0 and the phase-1 sink (2026-10-13)

The first day of QCBridgeA: the SDK on the box, the T-TAP Pro enumerated,
the sink written and built on both machines, and the first run against the
bus — which ran into the device's owner, by design.

## Step 0: the SDK and the device

- `libajantv2` branch `ntv2_17_5` (fd626dc81) built on the box with the
  VS 18 toolchain (Ninja, Release): the static library, demos and tools.
  Two things to know: the NVIDIA `cudalowlatencydemo` finds the installed
  CUDA 13.4, whose nvcc does not accept the VS 18 compiler, so the
  standalone build needs `-DCMAKE_DISABLE_FIND_PACKAGE_CUDA=TRUE
  -DAJANTV2_DISABLE_RDMA=ON`; and the SDK is C++11 code — under a parent
  project's `CMAKE_CXX_STANDARD 20` MSVC rejects `ajabase`'s
  goto-over-initialisation, so QCBridgeA adds the SDK before setting its own
  standard. On the Mac the SDK's CMake parses the SDK version out of
  `CMAKE_OSX_SYSROOT`, which a command-line-tools CMake leaves empty and
  which must name a versioned SDK (`MacOSX27.0.sdk`); the build file fills
  it in from `xcrun`.
- `ntv2enumerateboards` (read-only) on the box: one device, **TTapPro**
  serial 1T300310, 1 SDI out, 1 HDMI out, 16 embedded audio channels in and
  out; 54 video formats from 525i to 4Kp60 (1080p at 23.98/24/25/29.97/30/
  50/59.94/60, UHDp up to 60); 13 pixel formats including `NTV2_FBF_ARGB`,
  `NTV2_FBF_10BIT_YCBCR` (v210), `NTV2_FBF_10BIT_RGB`, `NTV2_FBF_12BIT_RGB_PACKED`.
- `NTV2_FBF_ARGB` is the bus's BGRA8: the SDK's `RGBAlphaPixel` is
  `{Blue, Green, Red, Alpha}` in memory (ntv2videodefines.h).
- The SDK's audio word is a plain signed 32-bit sample (the player demo
  generates its tone at 31 bits); the card uses the top 24.

## Phase 1: the sink

`src/sink/aja_output.cpp` (device open, exclusive acquire, video format
from the frame stamp's rate, ARGB frame store, 48 kHz audio, frame store →
CSC → SDI 1 + HDMI 1 routing, DMA-locked host buffers, AutoCirculate with
4 card frames) and `src/sink/bus_sink.cpp` (rings in, one transfer per
output interrupt repeating the last picture, the bus's stereo as 32-bit
PCM in channels 1–2, a status line for QCView's Output row). Builds on the
Mac (compile check only: no AJA software here) and on the box.

## First runs against the bus (box, desktop session)

`qcvo-probe produce --id aja --fps 25` as the fake QCView, the sink for
15 s:

| t | event |
|---|---|
| 0.6 s | attached, request 1920x1080 BGRA8 accepted, status "AJA: starting" |
| 1.6 s | status **"AJA: T-TAP Pro is in use by AfterFX.exe (pid 27172)"** |
| to 15.6 s | attached throughout, 705 audio packets taken, retrying the device every 2 s |

After Effects was open on the box with the AJA Transmit plugin holding the
device, so the acquire failed as PLAN A6 says it should, and the status line
names the holder (`GetStreamingApplication` + the process image name).
No picture was put on the T-TAP; that waits for the device to be free.

Two things fixed on the way:

- `qcvo-probe` had never run on Windows: `setvbuf(stdout, nullptr, _IOLBF, 0)`
  fail-fasts in the UCRT (0xC0000409 in ucrtbase, fault bucket type BEX64),
  with no output since the buffer is lost. Unbuffered on Windows now, in
  QCBridgeN's copy too (258651c).
- Acquiring a held device takes the driver about a second, which made the
  sink miss a heartbeat and look to the producer like a consumer that
  leaves and returns every two seconds. The open now runs on its own thread
  while the loop keeps beating; the producer saw "consumer present" for the
  whole run.

## Not done

- Playout: the device was held by After Effects for the whole session.
- Everything in PLAN "Phase 1" verification: cadence, audio continuity,
  latency, start/stop, the hand-off with Adobe's plugin.
- Packaging.
