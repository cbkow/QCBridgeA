# QCBridgeA

What [QCView](https://qcview.app) shows and plays, out of an AJA device's
SDI and HDMI outputs.

QCBridgeA is a small application that runs beside QCView. Switch it on in
QCView's **Settings ▸ Output** and the viewport — single or dual view,
splits, wipes and difference, the OCIO or minColor display transform,
safety guides and note strokes — plus the mixed audio go out of the AJA
card (T-TAP Pro, Io, KONA) as a video signal at the source's frame rate.
Nothing is sent until the switch is on; switch it off, or quit QCView, and
the sink releases the card.

## Status

In development; nothing is released. See `PLAN.md`.

## Requirements

- An AJA device with playback and AJA's own software installed for the
  driver (AJA Desktop Software 17.x; the development machine runs 17.5.0).
- QCView 2.5.7 or later (the output bus).
- Windows 10 1809 or later, x64. macOS follows when a Mac with the AJA
  software is at hand.

## Building

Needs CMake 3.24+, a C++20 compiler and the submodule:

    git clone --recurse-submodules https://github.com/cbkow/QCBridgeA.git
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    build/qcvo-probe produce --id aja --fps 25 --seconds 10   # fakes QCView
    build/QCBridgeA --id aja --seconds 10                      # the sink

For development QCView reads manifests from the directory
`QCV_OUTPUT_MANIFEST_DIR` names as well as from its Outputs folders:

    { "id": "aja", "name": "QCBridgeA (AJA SDI/HDMI)", "label": "AJA", "product": "QCBridgeA", "raster": "auto", "protocol": 1,
      "exec": "C:\\Program Files\\QCBridgeA\\QCBridgeA.exe",
      "args": ["--id", "aja"] }

`label` is what the Output row leads with and `product` the name shown
muted beside it; a manifest without them gets both read out of the name.
`"raster": "auto"` says the sink asks QCView for the raster (a 0x0 request),
which puts the Raster dropdown (Auto / 1080p / UHD) on the row.

## How it works

QCView publishes its corrected canvas and its mixed audio into shared
memory, one ring pair per enabled output, and starts the sink registered
for that output. The sink asks for a raster and format, keeps a heartbeat,
copies each published frame into a DMA buffer and hands it to the card
once per output frame — repeating the last picture when nothing changed,
with the audio due for that frame — and reports the signal format back to
QCView's Settings. The design is in `PLAN.md`; the bus itself is
documented in [QCBridgeN](https://github.com/cbkow/QCBridgeN)'s plan.

## Licence

MIT (`LICENSE`). The AJA NTV2 SDK (`external/libajantv2`, a submodule) is
MIT, copyright AJA Video Systems. The wire contract (`vendor/qcbae/`) is
MIT, copied from [QCBridgeAE](https://github.com/cbkow/QCBridgeAE). AJA,
KONA, Io and T-TAP are trademarks of AJA Video Systems, Inc.; QCBridgeA
is not an AJA product. The AJA device driver is not part of this
repository; install it from aja.com.
