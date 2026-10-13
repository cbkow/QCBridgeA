# 10-bit RGB on the bus, and the first picture on the T-TAP Pro (2026-10-13)

Phase 1, step 1 of the format plan: a 10-bit wire for SDR Rec.709, with
PQ and HLG to follow on the same container.

## The format

`OutputPixelFormat::RGB10 = 3` in the wire contract (QCBridgeAE
`src/common/protocol/output_desc.h` 49ef624, re-copied into QCView,
QCBridgeN and this repo): 10-bit RGB in a 32-bit word, R in bits 0–9,
G 10–19, B 20–29, two unused on top, full range. Chosen because three
things share that layout byte for byte: D3D11's `R10G10B10A2_UNORM`,
Metal's `RGB10A2Unorm`, and the NTV2 SDK's `NTV2_FBF_10BIT_RGB`
(`PackRGB10BitFor10BitRGB`: `(Blue<<20) + (Green<<10) + Red`). So
QCView renders straight into it and the sink DMAs the slot as it is;
nobody repacks. The protocol version stays 1: a QCView that does not know
the value refuses it with UnsupportedFormat, and the sink asks again for
BGRA8.

## QCView (8a61424d)

Both output passes take the format from the request: the wire texture is
RGB10A2 / R10G10B10A2 instead of BGRA8, same 4 bytes per pixel, same
ring size; the compositor and the annotation renderer are initialised for
that target, the D3D11 annotation renderer with an explicit SDR encode
(by format alone it would have picked its HDR10 PQ stroke shader, the
swapchain's rule). The request gate accepts BGRA8 and RGB10.

Verified on both GPUs with two probe consumers attached to one QCView,
one asking RGB10 and one BGRA8, the same paused frame of the 720p25 bars
clip letterboxed into 1080p, dumped from the rings and compared channel
by channel (every 7th value, 888 686 samples, 62 % non-black):

| | Metal (Mac) | D3D11 (box, RTX 5090) |
|---|---|---|
| max \|v10 ÷ 4 − v8\| | 1 | 1 |
| equal / off by one | 830 022 / 58 664 | 828 705 / 59 981 |
| 10-bit values with non-zero low two bits | 61.6 % | 61.7 % |
| a pixel, 8-bit → 10-bit | (41, 53, 219) → (163, 211, 879) | (51, 57, 141) → (206, 230, 564) |

The off-by-ones are the 8-bit rounding; the low-bit figure says the
10-bit picture carries detail the 8-bit one cannot. The 16-bit PPM the
probe writes for RGB10 keeps every code value.

Found on the way: on Windows nothing redraws while the picture is paused,
so a sink that attached to a paused QCView got no frame until something
else redrew (the first run published only at quit). The bus now asks the
renderer for a frame when a sink becomes active.

## The sink (QCBridgeA 5a0e24d)

Asks for RGB10 first, BGRA8 on refusal; the frame store is 10-bit RGB or
ARGB to match; the CSC is told Rec.709 and full-range RGB; SDI gets the
CSC's YCbCr; HDMI is fed the frame store's RGB directly and set to RGB,
full range, 10-bit (`--hdmi-color ycbcr` routes it from the CSC instead).

**First run on the card.** The T-TAP Pro was free (After Effects no
longer held it). `qcvo-probe produce --fps 25` painting RGB10, the sink
for 12 s:

| item | value |
|---|---|
| acquired, configured | "T-TAP Pro 1080p25 10-bit, SDI + HDMI RGB" |
| transfers | 302 in 12 s, 25/s steady after the fill, 301 new pictures |
| card buffer level | 2–3 of 4 |
| dropped / refused transfers | 0 / 0 |
| audio | 563 packets, 0 gaps, 5376 silence frames (the fill before the first packet), 0 trimmed |

Not yet looked at: the picture on a monitor (nothing is attached to the
T-TAP's outputs from this seat), the HDMI colour-space setting (the SDK's
OEM macro suggests the routed signal decides it), and everything in the
plan's verification list beyond cadence.
