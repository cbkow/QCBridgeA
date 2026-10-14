// SPDX-License-Identifier: MIT
// AjaOutput — one AJA device, one frame store, SDI (+ HDMI) out, driven by
// AutoCirculate (QCBridgeA/PLAN.md A2–A6). The SDK stays inside the .cpp.
//
// Life of a session: open() acquires the device exclusively; configure()
// sets the video format from a raster and a rate, the ARGB frame store,
// the audio system and the routing; transfer() hands the card one frame
// and its audio; close() releases everything. The caller paces itself on
// canAcceptFrame() / waitForOutputInterrupt().

#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct AjaOutputStats {
    uint64_t transfers = 0;       // frames handed to the card
    uint64_t badTransfers = 0;    // AutoCirculateTransfer refused
    uint64_t cardDropped = 0;     // the card's own drop counter
    uint64_t vbiWaits = 0;        // output interrupts waited on
    uint32_t bufferLevel = 0;     // frames queued on the card at the last status read
    bool     running = false;     // AutoCirculate started
};

class AjaOutput {
public:
    AjaOutput();
    ~AjaOutput();

    // deviceSpec: index, serial or model as AJA's tools take it ("0" = first).
    bool open(const std::string &deviceSpec, std::string *err);
    void close();
    bool isOpen() const;

    enum class HdmiColor { Rgb, YCbCr };
    // How the host DMA buffers are handed to the driver: pinned and mapped
    // for the session (the SDK's fast path), pinned only, or left to the
    // driver per transfer. A lever for the box's 2026-10-13/14 bugchecks
    // (dxgmms2 host-aperture mapping in Adobe's process, with the sink up).
    enum class DmaLock { Map, Pages, None };
    void setDmaLock(DmaLock m);

    // What the frames are and how the wire says so: transfer 1 SDR Rec.709,
    // 2 PQ Rec.2100, 3 HLG Rec.2100 (the bus's OutputTransfer); for PQ the
    // mastering display (primaries 1 Rec.2020, 2 P3-D65) and light levels.
    struct HdrSignal {
        uint32_t transfer  = 1;
        uint32_t primaries = 1;
        float    maxMasteringNits = 0.f, minMasteringNits = 0.f;
        uint32_t maxCll = 0, maxFall = 0;
    };

    // Picks the NTV2 video format for the raster and rate (UHD, 1080p and 720p
    // progressive, see PLAN A3; UHD as one 12G stream, so it needs a device
    // with 12G routing), the frame store (10-bit RGB for the bus's
    // RGB10, else 8-bit ARGB; both full range), `audioChannels` embedded
    // channels (2..16; HDMI carries the first 8), routes frame store → CSC →
    // SDI 1 and, when `hdmi`, HDMI 1 — fed the frame store's RGB directly
    // (HdmiColor::Rgb, full range, 10-bit when the picture is) or the CSC's
    // YCbCr; UHD above 30p always YCbCr, HDMI 2.0 has no room for 10-bit RGB
    // 4:4:4 there — and initialises AutoCirculate with `cardFrames` buffers.
    // Stops any previous session first.
    // `transfer` picks the CSC matrix (Rec.709 or Rec.2020 from full-range RGB)
    // and the signalling (SDI VPID; the HDMI HDR InfoFrame comes from setHdr).
    bool configure(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, bool tenBit, uint32_t transfer,
                   uint32_t audioChannels, bool hdmi, HdmiColor hdmiColor, uint32_t cardFrames, std::string *err);

    // The HDR metadata for the running session: SDI VPID transfer and
    // colorimetry, HDMI InfoFrame (EOTF, mastering primaries and luminance,
    // MaxCLL / MaxFALL) — or the InfoFrame off for SDR. Any time after configure().
    void setHdr(const HdrSignal &hdr);

    // Copies one picture (width*height*4 bytes, rows `bytesPerRow` apart,
    // in the configured format) into the DMA buffer. The next transfer() sends it; until a
    // new one is staged every transfer repeats it (PLAN A2).
    void stageFrame(const void *pixels, uint32_t bytesPerRow);

    // One frame period: the staged picture plus `audioFrames` sample frames
    // of interleaved signed 32-bit PCM, audioChannels() wide (nullptr =
    // silence). Starts AutoCirculate once two frames are on the card.
    bool transfer(const int32_t *audio, uint32_t audioFrames);

    bool     canAcceptFrame();              // the card has room for another frame
    void     waitForOutputInterrupt();      // one output VBI
    uint32_t audioFramesForNextFrame();     // the NTSC cadence helper, advances per call
    uint32_t audioChannels() const;
    uint32_t frameBytes() const;            // width*height*4 for the configured format

    std::string deviceName() const;         // "TTapPro"
    std::string formatName() const;         // "1080p25"
    std::string describe() const;           // for the status line
    AjaOutputStats stats();                 // refreshes bufferLevel / cardDropped

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
