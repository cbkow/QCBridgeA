// SPDX-License-Identifier: MIT
// BusSink — QCView's output rings in, an AJA device out (QCBridgeA/PLAN.md).
//
// One worker thread owns everything: it waits for the rings, attaches,
// writes the request, keeps the heartbeat, opens the AJA device, picks the
// video format from the first frame's rate, and then runs on the card's
// clock: each output interrupt it transfers the newest picture (or the last
// one again) with the audio due for one frame. The status line QCView
// shows names the device, the format and the connectors — or what is
// wrong. O13 as the sink sees it: rings retired or producer dead → release
// the card and go back to waiting, or quit if QCView started us.

#pragma once

#include "qcbae/output_desc.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class AjaOutput;

struct SinkConfig {
    std::string id         = "aja";      // the manifest id: rings are /qcview-out-<id>
    std::string device     = "0";        // AJA device index, serial or model
    uint32_t    width      = 1920;
    uint32_t    height     = 1080;
    uint32_t    layers     = qcbae::kOutputLayerGuides | qcbae::kOutputLayerAnnotations;
    qcbae::OutputContent content = qcbae::OutputContent::Composite;
    bool        hdmi       = true;       // also drive the HDMI output
    uint32_t    audioChannels = 8;       // embedded channels (stereo in 1-2, the rest silent)
    uint32_t    cardFrames = 4;          // AutoCirculate buffers on the card
    uint32_t    fallbackFpsNum = 25, fallbackFpsDen = 1;   // when the stamp has no rate
};

struct SinkStats {
    std::string state;          // "waiting for QCView", "attached", "AJA: ...", "sending", "quitting"
    std::string reply;          // the producer's message
    std::string card;           // AjaOutput::describe()
    uint64_t    frames = 0;     // new pictures taken off the ring
    uint64_t    transfers = 0;  // frames handed to the card (new + repeated)
    uint64_t    audioPackets = 0, audioGaps = 0, audioDropped = 0;
    uint64_t    silenceFrames = 0, trimmedFrames = 0;   // audio made up / thrown away (sample frames)
    uint32_t    width = 0, height = 0;
    double      fps = 0.0, newFps = 0.0;   // transfers and new pictures per second, last second
    bool        producerAlive = false;
    bool        launchedByProducer = false;
};

class BusSink {
public:
    BusSink(AjaOutput &aja, SinkConfig cfg);
    ~BusSink();

    void start();
    void stop();   // joins

    SinkStats stats() const;
    // Called on the worker when the producer retired the rings or died and
    // the sink was started by it: the app should quit.
    void setQuitRequest(std::function<void()> fn) { m_quit = std::move(fn); }

private:
    void run();
    void setState(const std::string &s);

    AjaOutput            &m_aja;
    SinkConfig            m_cfg;
    std::thread           m_thread;
    std::atomic<bool>     m_stop{false};
    mutable std::mutex    m_statsMutex;
    SinkStats             m_stats;
    std::function<void()> m_quit;
};
