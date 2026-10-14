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
