# QCBridgeA — QCView's output bus onto an AJA card

The plan for sending what QCView shows and plays out of an AJA device's
SDI and HDMI connectors, written 2026-10-13. Three repositories take
part: QCBridgeAE (the ring code), QCView-Player (the bus) and this one
(the AJA sink). The bus and its decisions are QCBridgeN's (`PLAN.md`
there, cited as `PLAN On`); this file only adds what an SDI output
needs. Measured results go in `lab/results/<date>-<slug>/`.

## Why a repository of its own

QCView is GPLv3 and links the GPL build of FFmpeg. AJA's NTV2 SDK
(`libajantv2`) is MIT, so unlike the NDI SDK it *could* be linked into
QCView; it is kept out anyway, for the same reasons the bus exists: the
driver is AJA's closed component and its failure modes stay out of the
player, and every sink is one process behind one documented contract.
A separate repository, rather than a second target in QCBridgeN, keeps
each sink's licence file about one SDK: QCBridgeN ships an NDI runtime
under Vizrt's terms; this one ships nothing but MIT code and asks the
user to install AJA's own software for the driver.

## Decisions

- **A1 — The SDK is a pinned submodule, never vendored.** `external/
  libajantv2` tracks the `ntv2_17_5` branch (the installed driver on the
  development machine is 17.5.0; AJA asks for the SDK and driver to match
  on the major version). Its licence text travels with it. The build
  disables the SDK's demos, tools, tests and driver.
- **A2 — The card runs the clock; the sink repeats.** The bus publishes
  on change (PLAN O5). AutoCirculate wants a frame every frame period and
  its audio rides with the frames, so the sink transfers once per output
  interrupt: the newest slot when one arrived, its own copy of the last
  frame otherwise, each with the audio due for one frame. One memcpy per
  new frame into a page-aligned, DMA-locked host buffer; the ring claim
  is held only for the copy.
- **A3 — Video format from the stamp, raster from the request.** The
  sink requests 1920x1080 (or 1280x720) letterboxed and picks the NTV2
  video format from the frame stamp's fps: 23.976, 24, 25, 29.97, 30, 50,
  59.94 and 60 progressive. A rate change mid-session reconfigures the
  card (a short glitch, logged). UHD (3840x2160 over 12G) is a later step
  with its own routing.
- **A4 — BGRA8 first, through the card's colour-space converter.** The
  frame store takes the ring's BGRA8 as `NTV2_FBF_ARGB` (byte order
  B, G, R, A in memory) and the card's CSC produces Rec.709 YCbCr on the
  SDI and HDMI outputs; RGB on the wire is an option. The 10-bit wire
  (`v210`, written by QCView's output shader when a sink asks, PLAN O7)
  and the HDR transfers (PQ / HLG with the HDMI infoframe and the SDI
  VPID) are phase 2 and need the QCView side as well.
- **A5 — Audio is the bus's stereo in channels 1 and 2.** Planar float
  from the audio ring becomes interleaved signed 32-bit PCM at 48 kHz
  (the SDK's native word), the remaining embedded channels silent; the
  per-frame sample count follows the NTSC cadence helper. Audio that is
  not there (a paused player) is silence, so the embedder never starves.
- **A6 — Exclusive use, released on exit.** The sink acquires the device
  the way AJA's applications do (`AcquireStreamForApplication`), sets the
  OEM task mode, and releases both on stop. If Adobe's AJA Transmit or
  Control Room holds the card the sink reports "in use" in QCView's
  Output row and retries while the rings live.
- **A7 — Headless first.** No tray in phase 1: QCView's Output row shows
  the status line ("SDI 1080p25 · HDMI", "in use by another application",
  "no AJA device"). Settings are manifest arguments.

## Phases

1. **Probe (step 0).** Build the SDK on the box; enumerate the device and
   its formats; `ntv2player` to the T-TAP Pro to prove ownership hand-off
   and the toolchain.
2. **Phase 1.** The sink above, measured against `qcvo-probe produce`
   first, then QCView: cadence at 23.976 / 25 / 29.97, audio continuity,
   latency viewport→SDI monitor (photo method), start/stop 20×, quit
   while attached, Premiere's AJA Transmit hand-off.
3. **Phase 2.** `v210` on the bus (QCView output shader + request enum),
   PQ/HLG transfers with metadata; UHD routing.
4. **Packaging.** Windows Setup (Inno) writing `%ProgramData%\QCView\
   Outputs\aja.json`; macOS pkg when a Mac has the AJA software installed.

## Measurements the plan owes

- Output interrupt to transfer time; frames repeated vs new per second at
  each rate; audio samples per frame cadence; viewport→SDI latency in
  frames; DMA time per 1080p frame (locked vs unlocked buffer).
