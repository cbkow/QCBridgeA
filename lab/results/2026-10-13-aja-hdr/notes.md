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

## The sink (QCBridgeA 6cba5de)

Asks for transfer 0 (`--transfer sdr|pq|hlg` to force), takes the reply's
transfer and reconfigures when it changes; the enhanced CSC gets the
Rec.2020 matrix for PQ and HLG (Rec.709 otherwise, full-range RGB in
either case); the SDI VPID says SDR / PQ / HLG and Rec.709 / UHDTV; the
HDMI HDR InfoFrame carries the EOTF (2 = ST 2084, 3 = HLG) and, for PQ,
the mastering primaries (2020 or P3-D65 with a D65 white), luminance and
light levels from the bus's block — or is switched off for SDR. The
metadata is re-applied whenever QCView rewrites the block.
