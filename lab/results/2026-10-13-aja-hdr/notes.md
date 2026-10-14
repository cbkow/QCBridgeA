# PQ and HLG on the bus, and the metadata the sink signals (2026-10-13)

Phase 1, step 2: the three outputs on the one 10-bit container (PLAN A4).

## The contract (QCBridgeAE 6a3f348)

- A request's `transfer` of 0 (Unspecified) now means "the producer's
  per-output setting"; the reply and every frame carry the one in use.
- `OutputHdrInfo` (transfer, mastering primaries Rec.2020 or P3-D65, max
  and min mastering luminance, MaxCLL, MaxFALL) behind a seqlock in an
  `OutputHdrBlock` at byte 1024 of the ring's first page, past the header
  and before the slots. The header flag `kOutputHeaderHasHdrInfo` says it
  is there; a consumer that predates it (the shipped QCBridgeN 0.0.2)
  never looks, and the ring version stays 1, so nothing already installed
  breaks.

## QCView (d3178411)

- The Output section gets, per output: the transfer (SDR Rec.709, PQ
  Rec.2100, HLG Rec.2100), the view on the config's Rec.2100 display for
  that transfer (the display is found by name: "Rec.2100-PQ" / "-HLG",
  else any PQ / HLG display that is not a P3 container), and for PQ the
  mastering primaries, peak, MaxCLL and MaxFALL. Persisted per output id;
  a running output is re-answered at once and the metadata block rewritten.
- Both output passes: for a PQ or HLG output the live chain is rebuilt
  through the output's own display and view (no display LUT: that belongs
  to the monitor on the viewport) in a per-output OCIO renderer, so the
  viewport and the SDR outputs are untouched. Guides and strokes take the
  output's transfer: PQ at 203 nits graphics white (BT.2408), HLG with
  white on 75 % — a new HLG stroke shader on D3D11, PQ and HLG stroke
  encodes on Metal (which had none; the viewport never needed them).
- Refusals: HDR with the minColor engine ("minColor later"), or a config
  without a Rec.2100 display.

Verified on the Mac (Metal) with three 10-bit probe consumers on one
QCView, the Output section set to SDR, PQ and HLG, the 720p25 bars clip
through the bundled ACES 2.0 config:

| bar (x) | SDR 709 | PQ Rec.2100 → nits | HLG Rec.2100 |
|---|---|---|---|
| yellow (840) | 1023, 963, 0 | 512, 506, 299 → 93, 87, 9 | 634, 620, 207 |
| blue (1080) | 0, 59, 1023 | 251, 182, 517 → 5, 2, 97 | 184, 108, 728 |
| white-ish max | 1023 | 554 → 160 nits | 764 (75 % is 767) |

The PQ picture sits where an SDR source belongs in an HDR 1000-nit view
(white around 100 nits), with non-zero blue in the yellow bar as the 2020
container demands; the HLG picture peaks at the 75 % reference. The SDR
output alongside is unchanged.

## Pass-through, and the box (QCView 08c24081, b297cc6f)

Chris, seeing the first PQ run on the monitor: PQ and HLG must also serve
the Premiere and After Effects feeds, which arrive already encoded — so
the transfer is a dropdown that says what the wire is told and signals,
not a conversion we impose. The view became optional: **"(as displayed)"**
(the default) sends the picture as the viewport shows it, tagged and
signalled; a named view on the config's Rec.2100 display re-renders the
live chain through it. No refusals for a config without such a display.

Two things found on the box on the way: the D3D11 OCIO renderer builds
every chain on a worker thread, so the first frame an HDR output published
was the raw picture and, the clip paused, nothing republished it — an HDR
output now publishes nothing until its chain exists; and a dev run's
default OCIO config is Blender 5.2 (the ACES view names used in the rig
do not exist there), while the box's `$OCIO` points at a studio config
with no Rec.2100 display at all — the rig switches to ACES 2.0 and
re-engages (switching the config disengages the chain).

D3D11 (box), the same bars clip, four 10-bit consumers on one QCView:

| output | yellow bar (x = 840) | blue bar (x = 1080) | max |
|---|---|---|---|
| SDR Rec.709 | 1023, 963, 0 | 0, 59, 1023 | 1023 |
| PQ, view "ACES 2.0 - HDR 1000 nits (P3 D65)" | 517, 512, 325 → 97, 93, 12 nits | 231, 157, 490 → 4, 1, 75 nits | 551 |
| HLG, same view | 643, 631, 243 | 163, 89, 680 | 759 |
| PQ, as displayed | 1023, 963, 0 (the SDR picture, tagged PQ) | 0, 59, 1023 | 1023 |

Metal gives the same numbers to within a code value. Then QCView itself
drove the sink at PQ through that view with P3-D65 mastering metadata, 30
s on the T-TAP Pro; the picture reached Chris's monitor (his word: "wow").

## The sink (QCBridgeA 6cba5de)

Asks for transfer 0 (`--transfer sdr|pq|hlg` to force), takes the reply's
transfer and reconfigures when it changes; the enhanced CSC gets the
Rec.2020 matrix for PQ and HLG (Rec.709 otherwise, full-range RGB in
either case); the SDI VPID says SDR / PQ / HLG and Rec.709 / UHDTV; the
HDMI HDR InfoFrame carries the EOTF (2 = ST 2084, 3 = HLG) and, for PQ,
the mastering primaries (2020 or P3-D65 with a D65 white), luminance and
light levels from the bus's block — or is switched off for SDR. The
metadata is re-applied whenever QCView rewrites the block.

## 2026-10-14 — manual session, and the view option dropped

QCView's dev build ran on the box desktop for a manual session with the
T-TAP Pro (SDR, PQ as displayed, PQ and HLG through the ACES 2.0 view).
Verdict: "working very well". Two notes came back: the Output row was too
wide for the rail, and the per-output view re-render duplicated the colour
panel — OCIO or minColor already choose the display there. The view option
is gone (QCView commit of 2026-10-14, plan A4c). Each output keeps one
dropdown, the transfer, which is what the wire is told; PQ keeps its
mastering primaries and nits on their own line.

minColor through the bus, checked on the Mac (engine minColor, display
"Rec.2100 - PQ / Rec.2020 (P3 Limited)", the bars clip, two RGB10 probe
consumers): the PQ output received the viewport's PQ picture (yellow bar
512, 505, 300 = 93 / 87 / 9 nits, max 556 — within a few code values of
the ACES 2.0 result) and the SDR output the SDR capture (1023, 958, 0).
Both engines take the same two paths, so an AJA output under minColor
needs nothing more.

### Switching the transfer while the output runs (2026-10-14, box)

Chris had to stop and start the AJA output after every transfer change.
The sink used to release the device and re-acquire it on a changed reply
(a second of black, and the T-TAP sometimes came back only with the next
published frame). It now reconfigures the card in place (58cfdf4). Scripted
run, QCView → QCBridgeA on the T-TAP Pro, the transfer switched every 8 s
with the clip paused: SDR → PQ → HLG → SDR. The sink logged "transfer
changed to PQ - reconfiguring", re-described the card ("1080p25 10-bit PQ
2020"), the InfoFrame metadata followed each step (transfer 2 / primaries 1
/ 1000 nits, then transfer 3, then back to SDR), 25 transfers/s throughout,
0 dropped, 0 bad, 786 transfers in all. With pass-through the picture is
unchanged across the switch when the viewport is SDR, so QCView publishes
nothing new — the card simply re-tags; a PQ or HLG viewport re-publishes.

## UHD on the T-TAP Pro (2026-10-14)

The sink now asks QCView for its raster (a 0x0 request = the Output row's
Raster setting: Auto, 1080p or UHD; Auto = the source's class). Scripted run
on the box (`aja-uhd.cmd`, build QCView 7fa386c7 + this sink): a 3840x2160
25p bars clip playing, the output enabled with Raster = Auto; at 18 s a
1920x1080 clip opened (Auto re-answers 1080p); at 28 s Raster = UHD; at
38 s Raster = 1080p; quit at 48 s.

    accepted 3840x2160 ... AJA T-TAP Pro UHDp25 10-bit SDR 709, SDI + HDMI RGB
    UHDp25, 25/s to the card, 25/s new, card level 2, dropped 0, bad 0   (x12)
    accepted 1920x1080 -> raster changed to 1920x1080 - reconfiguring -> 1080p25 ...
    accepted 3840x2160 -> raster changed to 3840x2160 - reconfiguring -> UHDp25 ...
    accepted 1920x1080 -> raster changed to 1920x1080 - reconfiguring -> 1080p25 ...
    done - 1019 pictures, 1087 transfers, 0 dropped, 0 bad

Every raster change was an in-place reconfigure (device held, AutoCirculate
restarted); the card re-described itself each time. UHD is one 12G stream
from the single frame store in two-sample-interleave mode (the SDK's
`SetVideoFormat` puts it there, said again explicitly), the HD routing
otherwise; the HDMI rasteriser goes to 4K playback mode. On the Mac the
same QCView logic was checked with `qcvo-probe consume --width 0 --height 0`
(frame descs 3840x2160 RGB10 row 15360; re-answer on the source change;
forced UHD and 1080p). D3D11 and Metal output passes both render UHD RGB10.
Not yet looked at: the monitor's info page at UHD (3840x2160 and the HDMI
colour format), UHD above 30p (the HDMI side is switched to the CSC's YCbCr
there), UHD PQ.

## HDR at UHD on the T-TAP Pro (2026-10-14, later)

`aja-uhd-hdr.cmd`: the UHD 25p clip through the minColor PQ display
(engine 1, display 6), Raster = Auto, transfer PQ; HLG at 18 s; 1080p + PQ
at 28 s; UHD at 38 s; quit at 48 s.

    accepted 3840x2160 "3840x2160 RGB10 PQ Rec.2100 from QCView"
    HDR metadata: transfer 2 primaries 1 mastering 1000/0.0050 nits MaxCLL 1000 MaxFALL 400
    AJA T-TAP Pro UHDp25 10-bit PQ 2020, SDI + HDMI RGB
    transfer changed to HLG - reconfiguring  -> UHDp25 10-bit HLG 2020 (metadata transfer 3)
    raster changed to 1920x1080 - reconfiguring -> 1080p25 10-bit HLG 2020
    transfer changed to PQ - reconfiguring -> 1080p25 10-bit PQ 2020
    raster changed to 3840x2160 - reconfiguring -> UHDp25 10-bit PQ 2020
    done - 1024 pictures, 1085 transfers, 0 dropped, 0 bad; UHDp25 25/s throughout

Every step an in-place reconfigure; the VPID and InfoFrame followed each
time. On the Mac the same run through `qcvo-probe` at 0x0 gave the UHD PQ
frame the 1080p PQ code values exactly (bars yellow 512,505,300 ≈ 93/87/9
nits, peak 576); the HLG dump equals the PQ one, as it should — the
picture is the viewport's (minColor's PQ display) and the transfer is a tag.
Two small things seen, not bugs: a transfer change applies the previous
HDR block once before the new block lands in the same poll (two "HDR
metadata" lines); two settings changed in one second answer twice (two
reconfigures). Still owed: the monitor's info page at UHD (3840x2160 and
ST 2084 / BT.2020), UHD above 30p (HDMI then YCbCr), 23.976 at UHD.

## Packaging and a 23.976 run (2026-10-14, evening)

First release artifacts, 0.0.1: `dist/QCBridgeA-0.0.1-Setup-x64.exe` (Inno,
one static exe — the whole tree now builds against the static MSVC runtime,
`CMAKE_MSVC_RUNTIME_LIBRARY` before `project()`, the SDK's library included;
the first try set it per target and the SDK's objects came out `/MD`,
`__imp_system` unresolved) and `dist/QCBridgeA-0.0.1-arm64.pkg` (signed,
notarized app and pkg; the Mac build links only /usr/lib and /System; smoke
test = `--seconds 1` reaches the bus; NOT run against an AJA device on a Mac).
The static exe ran the switch rig on the T-TAP Pro as before (SDR → PQ →
HLG → SDR in place); Chris loaded a 23.976 clip into that run and the sink
reconfigured: "rate changed to 24000/1001 — reconfiguring → 1080p23.98,
24/s to the card, 0 dropped". 23.976 cadence ticked.
