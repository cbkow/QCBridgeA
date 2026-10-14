// SPDX-License-Identifier: MIT
#include "bus_sink.h"

#include "aja_output.h"
#include "qcbae/audio_ring.h"
#include "qcbae/output_ring.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <vector>

using namespace qcbae;

namespace {

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The bus's stereo, waiting for the card's frame clock (PLAN A5). Frames
// are kept as interleaved float pairs; the card gets signed 32-bit words.
class AudioQueue {
public:
    void push(const float *const *planes, uint32_t channels, uint32_t frames, uint32_t planeStride)
    {
        const float *l = planes[0];
        const float *r = channels > 1 ? planes[0] + planeStride : planes[0];
        for (uint32_t i = 0; i < frames; ++i) { m_q.push_back(l[i]); m_q.push_back(r[i]); }
    }
    size_t frames() const { return m_q.size() / 2; }
    // Fills `out` (frames × channels words) from the queue, silence where short. Returns the silence frames.
    uint32_t pull(int32_t *out, uint32_t frames, uint32_t channels)
    {
        uint32_t silence = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            float l = 0.f, r = 0.f;
            if (m_q.size() >= 2) { l = m_q.front(); m_q.pop_front(); r = m_q.front(); m_q.pop_front(); }
            else ++silence;
            int32_t *w = out + size_t(i) * channels;
            w[0] = toWord(l);
            if (channels > 1) w[1] = toWord(r);
            for (uint32_t c = 2; c < channels; ++c) w[c] = 0;
        }
        return silence;
    }
    // Drops the oldest frames beyond `keep`. Returns what was dropped.
    uint32_t trim(size_t keep)
    {
        if (frames() <= keep) return 0;
        const size_t drop = frames() - keep;
        m_q.erase(m_q.begin(), m_q.begin() + std::ptrdiff_t(drop * 2));
        return uint32_t(drop);
    }
    void clear() { m_q.clear(); }

private:
    static int32_t toWord(float f)
    {
        const double v = std::clamp(double(f), -1.0, 1.0) * 2147483647.0;
        return int32_t(std::lrint(v));
    }
    std::deque<float> m_q;
};

bool sameRate(uint32_t an, uint32_t ad, uint32_t bn, uint32_t bd)
{
    return uint64_t(an) * bd == uint64_t(bn) * ad;
}

} // namespace

BusSink::BusSink(AjaOutput &aja, SinkConfig cfg) : m_aja(aja), m_cfg(std::move(cfg))
{
    m_aja.setDmaLock(m_cfg.dmaLock == 2 ? AjaOutput::DmaLock::None
                   : m_cfg.dmaLock == 1 ? AjaOutput::DmaLock::Pages : AjaOutput::DmaLock::Map);
}

BusSink::~BusSink() { stop(); }

void BusSink::start()
{
    if (m_thread.joinable()) return;
    m_stop.store(false);
    m_thread = std::thread([this] { run(); });
}

void BusSink::stop()
{
    m_stop.store(true);
    if (m_thread.joinable()) m_thread.join();
}

SinkStats BusSink::stats() const
{
    std::lock_guard<std::mutex> lk(m_statsMutex);
    return m_stats;
}

void BusSink::setState(const std::string &s)
{
    std::lock_guard<std::mutex> lk(m_statsMutex);
    if (m_stats.state != s) { std::printf("QCBridgeA: %s\n", s.c_str()); std::fflush(stdout); }
    m_stats.state = s;
}

void BusSink::run()
{
    const std::string ringName  = OutputRing::name_for(m_cfg.id);
    const std::string audioName = OutputRing::audio_name_for(m_cfg.id);
    if (ringName.empty()) { setState("bad output id '" + m_cfg.id + "'"); return; }

    std::vector<float>   planes(static_cast<size_t>(kMaxAudioChannels) * kDefaultAudioSlotFrames);
    std::vector<int32_t> words;
    AudioQueue           queue;

    while (!m_stop.load()) {
        // ---- wait for QCView's rings ----
        OutputRing ring;
        AudioRing  audio;
        setState("waiting for QCView (" + ringName + ")");
        while (!m_stop.load() && !ring.open(ringName)) sleep_ms(250);
        if (m_stop.load()) break;
        if (ring.state() == OutputState::Retired || !ring.producer_alive()) {
            sleep_ms(500);   // a leftover from a QCView that is gone: wait for a live one
            continue;
        }
        const bool haveAudio = audio.open(audioName);
        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.launchedByProducer = ring.launched_by_producer();
            m_stats.producerAlive = true;
            m_stats.frames = 0; m_stats.transfers = 0; m_stats.audioPackets = 0; m_stats.audioGaps = 0;
            m_stats.audioDropped = 0; m_stats.silenceFrames = 0; m_stats.trimmedFrames = 0;
        }
        std::printf("QCBridgeA: attached to %s (producer pid %u, %s, audio ring %s)\n", ringName.c_str(),
                    ring.header()->producer_pid, ring.launched_by_producer() ? "started by QCView" : "user-started",
                    haveAudio ? "open" : "absent");

        ring.attach(OutputRing::now_ns());
        // RGB10 first (PLAN A4); a QCView that only speaks BGRA8 refuses the
        // format and the request is repeated at 8 bits.
        OutputPixelFormat wantFmt = m_cfg.tenBit ? OutputPixelFormat::RGB10 : OutputPixelFormat::BGRA8;
        OutputRequest req {};
        req.protocol_version = kOutputDescVersion;
        req.width = m_cfg.width;  req.height = m_cfg.height;
        req.pixel_format = static_cast<uint32_t>(wantFmt);
        req.transfer     = m_cfg.transfer;   // 0: QCView's Output section decides
        req.fit          = static_cast<uint32_t>(OutputFit::Letterbox);
        req.content      = static_cast<uint32_t>(m_cfg.content);
        req.layers       = m_cfg.layers;
        ring.write_request(req);
        ring.write_status("AJA: starting");
        setState("attached, waiting for the reply");

        uint64_t replyGen = 0, lastSeen = 0, aSeen = 0, aKnownGen = 0, dropped = 0;
        bool accepted = false;
        int64_t lastBeat = 0, lastSecond = OutputRing::now_ns(), lastOpenTry = 0;
        uint64_t transfersAtSecond = 0, framesAtSecond = 0;
        bool haveExpected = false;
        uint64_t expectedFirst = 0;
        AudioSession session {};
        if (haveAudio) audio.skip_to_latest(&aSeen);
        std::string why, lastStatus, cardErr;
        std::future<bool> opening;
        bool configured = false;
        OutputPixelFormat gotFmt = OutputPixelFormat::Unspecified, cfgFmt = OutputPixelFormat::Unspecified;
        uint32_t gotTransfer = 1, cfgTransfer = 0;
        uint32_t gotW = 0, gotH = 0, cfgW = 0, cfgH = 0;   // the raster QCView answered with / the card runs
        OutputHdrInfo hdrInfo {};
        uint64_t hdrGen = 0;
        bool haveHdr = false;
        uint32_t curNum = 0, curDen = 0, pendNum = 0, pendDen = 0, pendCount = 0;
        uint32_t perFrame = 0;   // nominal audio frames per video frame, for the queue bounds
        queue.clear();

        auto status = [&](const std::string &line) {
            if (line == lastStatus) return;
            lastStatus = line;
            ring.write_status(line.c_str());
            std::printf("QCBridgeA: %s\n", line.c_str());
            std::fflush(stdout);
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.card = line;
        };

        auto applyHdr = [&]() {
            AjaOutput::HdrSignal h;
            h.transfer = hdrInfo.transfer ? hdrInfo.transfer : gotTransfer;
            h.primaries = hdrInfo.primaries;
            h.maxMasteringNits = hdrInfo.max_mastering_nits;
            h.minMasteringNits = hdrInfo.min_mastering_nits;
            h.maxCll = hdrInfo.max_cll;  h.maxFall = hdrInfo.max_fall;
            m_aja.setHdr(h);
            std::printf("QCBridgeA: HDR metadata: transfer %u primaries %u mastering %.0f/%.4f nits MaxCLL %u MaxFALL %u\n",
                        h.transfer, h.primaries, h.maxMasteringNits, h.minMasteringNits, h.maxCll, h.maxFall);
        };
        auto configureFor = [&](uint32_t num, uint32_t den) -> bool {
            std::string err;
            const bool tenBit = gotFmt == OutputPixelFormat::RGB10;
            if (!m_aja.configure(gotW, gotH, num, den, tenBit, gotTransfer, m_cfg.audioChannels, m_cfg.hdmi,
                                 m_cfg.hdmiRgb ? AjaOutput::HdmiColor::Rgb : AjaOutput::HdmiColor::YCbCr,
                                 m_cfg.cardFrames, &err)) {
                configured = false;
                status("AJA: " + err);
                return false;
            }
            configured = true;
            cfgFmt = gotFmt;
            cfgTransfer = gotTransfer;
            cfgW = gotW; cfgH = gotH;
            if (haveHdr) applyHdr();
            curNum = num; curDen = den;
            pendCount = 0;
            perFrame = uint32_t(std::lround(48000.0 * den / num));
            words.assign(size_t(perFrame + 64) * m_aja.audioChannels(), 0);
            queue.clear();
            status("AJA " + m_aja.describe());
            return true;
        };

        while (!m_stop.load()) {
            const int64_t now = OutputRing::now_ns();
            if (now - lastBeat > 250000000LL) { ring.heartbeat(now); lastBeat = now; }
            if (ring.state() == OutputState::Retired) { why = "QCView retired the output"; break; }
            if (!ring.producer_alive())               { why = "QCView is gone"; break; }

            OutputReply reply {};
            if (ring.read_reply(&reply, &replyGen)) {
                accepted = reply.accepted != 0;
                if (accepted) {
                    gotFmt = static_cast<OutputPixelFormat>(reply.pixel_format);
                    if (gotFmt != OutputPixelFormat::RGB10 && gotFmt != OutputPixelFormat::BGRA8) accepted = false;
                    gotTransfer = reply.transfer ? reply.transfer : 1u;
                    gotW = reply.width; gotH = reply.height;
                }
                const bool fallBack = !accepted && wantFmt == OutputPixelFormat::RGB10
                                   && static_cast<OutputRefusal>(reply.refusal) == OutputRefusal::UnsupportedFormat;
                {
                    std::lock_guard<std::mutex> lk(m_statsMutex);
                    m_stats.reply = reply.message;
                    m_stats.width = reply.width; m_stats.height = reply.height;
                    m_stats.format = accepted ? (gotFmt == OutputPixelFormat::RGB10 ? "RGB10" : "BGRA8") : "";
                    if (accepted) std::printf("QCBridgeA: accepted %ux%u \"%s\"\n", reply.width, reply.height, reply.message);
                    else        { std::printf("QCBridgeA: REFUSED \"%s\"%s\n", reply.message, fallBack ? " - asking for BGRA8" : "");
                                  m_stats.state = std::string("refused: ") + reply.message; }
                }
                if (fallBack) {
                    wantFmt = OutputPixelFormat::BGRA8;
                    req.pixel_format = static_cast<uint32_t>(wantFmt);
                    ring.write_request(req);
                }
                // Raster, depth or transfer changed (the Output row's dropdowns, or
                // a source of another class under an automatic raster): the card is
                // reconfigured in place — the device stays acquired; the video
                // format, CSC matrix, VPID and InfoFrame follow — rather than
                // released and re-acquired (a second of black, and nothing until
                // the next published frame when the clip is paused).
                if (configured && accepted && (gotFmt != cfgFmt || gotTransfer != cfgTransfer || gotW != cfgW || gotH != cfgH)) {
                    char what[64];
                    if (gotW != cfgW || gotH != cfgH) std::snprintf(what, sizeof what, "raster changed to %ux%u", gotW, gotH);
                    else if (gotFmt != cfgFmt) std::snprintf(what, sizeof what, "pixel format changed to %s", gotFmt == OutputPixelFormat::RGB10 ? "RGB10" : "BGRA8");
                    else std::snprintf(what, sizeof what, "transfer changed to %s", gotTransfer == 2 ? "PQ" : gotTransfer == 3 ? "HLG" : "SDR");
                    std::printf("QCBridgeA: %s - reconfiguring\n", what);
                    configureFor(curNum, curDen);
                }
            }

            // ---- HDR metadata from QCView's Output section, whenever it changes ----
            if (ring.read_hdr_info(&hdrInfo, &hdrGen)) {
                haveHdr = true;
                if (configured) applyHdr();
            }

            // ---- audio: every packet into the queue (always, so the ring never laps us) ----
            if (haveAudio) {
                if (audio.read_session(&session, &aKnownGen)) haveExpected = false;
                for (int n = 0; n < 32; ++n) {
                    AudioPacketDesc ad {};
                    const auto r = audio.next_packet(&aSeen, &ad, planes.data(), &dropped);
                    if (r == AudioRing::Next::None) break;
                    if (r == AudioRing::Next::Resynced) { haveExpected = false; continue; }
                    if (haveExpected && ad.first_frame != expectedFirst) {
                        std::lock_guard<std::mutex> lk(m_statsMutex); ++m_stats.audioGaps;
                    }
                    expectedFirst = ad.first_frame + ad.frames; haveExpected = true;
                    if (configured) {
                        const float *pl[1] = { planes.data() };
                        queue.push(pl, ad.channels, ad.frames, kDefaultAudioSlotFrames);
                    }
                    std::lock_guard<std::mutex> lk(m_statsMutex);
                    ++m_stats.audioPackets;
                    m_stats.audioDropped = dropped;
                }
                // Bound the queue: more than three frames ahead of the card is skew, not buffering.
                if (configured && perFrame) {
                    const uint32_t trimmed = queue.trim(size_t(perFrame) * 3);
                    if (trimmed) { std::lock_guard<std::mutex> lk(m_statsMutex); m_stats.trimmedFrames += trimmed; }
                }
            }

            if (!accepted) { sleep_ms(1); continue; }

            // ---- the card: open it (retrying while another application holds it) ----
            if (!m_aja.isOpen()) {
                // Acquiring a device another application holds takes the driver
                // about a second, so the attempt runs on its own thread and the
                // heartbeat keeps going here.
                if (opening.valid()) {
                    if (opening.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                        const bool ok = opening.get();
                        if (ok) status("AJA " + m_aja.describe() + ", waiting for the first frame");
                        else    status("AJA: " + cardErr);
                    }
                } else if (now - lastOpenTry > 2000000000LL) {
                    lastOpenTry = now;
                    opening = std::async(std::launch::async, [this, &cardErr] { return m_aja.open(m_cfg.device, &cardErr); });
                }
                sleep_ms(1);
                continue;
            }

            // ---- video: the newest picture, copied into the DMA buffer ----
            OutputFrameDesc d {};
            const void *px = nullptr;
            if (ring.acquire_latest(&lastSeen, &d, &px)) {
                // Frames of the previous shape still in the ring after a re-answer are left alone.
                bool take = d.pixel_format == static_cast<uint32_t>(gotFmt)
                         && d.width == gotW && d.height == gotH;
                if (take) {
                    uint32_t num = d.source_fps_num, den = d.source_fps_den;
                    if (!num || !den) { num = m_cfg.fallbackFpsNum; den = m_cfg.fallbackFpsDen; }
                    if (!configured) {
                        take = configureFor(num, den);
                    } else if (!sameRate(num, den, curNum, curDen)) {
                        // A new rate has to hold for a few frames before the card is reconfigured (PLAN A3).
                        if (sameRate(num, den, pendNum, pendDen)) ++pendCount; else { pendNum = num; pendDen = den; pendCount = 1; }
                        if (pendCount >= 5) {
                            std::printf("QCBridgeA: rate changed to %u/%u — reconfiguring\n", num, den);
                            take = configureFor(num, den);
                        }
                    } else {
                        pendCount = 0;
                    }
                    if (take && configured) {
                        m_aja.stageFrame(px, d.bytes_per_row);
                        ring.report_seen(lastSeen);
                        std::lock_guard<std::mutex> lk(m_statsMutex);
                        ++m_stats.frames;
                    }
                }
                ring.release();
            }

            if (!configured) { sleep_ms(1); continue; }

            // ---- the card's clock: one transfer per output frame, repeating the last picture ----
            if (m_aja.canAcceptFrame()) {
                const uint32_t n = m_aja.audioFramesForNextFrame();
                if (words.size() < size_t(n) * m_aja.audioChannels()) words.resize(size_t(n) * m_aja.audioChannels());
                const uint32_t silence = queue.pull(words.data(), n, m_aja.audioChannels());
                m_aja.transfer(words.data(), n);
                std::lock_guard<std::mutex> lk(m_statsMutex);
                ++m_stats.transfers;
                m_stats.silenceFrames += silence;
            } else {
                m_aja.waitForOutputInterrupt();
            }

            // ---- once a second: rates for the log ----
            if (now - lastSecond > 1000000000LL) {
                lastSecond = now;
                const AjaOutputStats as = m_aja.stats();
                uint64_t transfers, frames;
                {
                    std::lock_guard<std::mutex> lk(m_statsMutex);
                    transfers = m_stats.transfers; frames = m_stats.frames;
                    m_stats.fps = double(transfers - transfersAtSecond);
                    m_stats.newFps = double(frames - framesAtSecond);
                    m_stats.state = as.running ? "sending" : "filling the card";
                }
                std::printf("QCBridgeA: %s, %.0f/s to the card, %.0f/s new, card level %u, dropped %llu, bad %llu\n",
                            m_aja.formatName().c_str(), double(transfers - transfersAtSecond), double(frames - framesAtSecond),
                            as.bufferLevel, (unsigned long long)as.cardDropped, (unsigned long long)as.badTransfers);
                std::fflush(stdout);
                transfersAtSecond = transfers; framesAtSecond = frames;
            }
        }
        if (m_stop.load()) { if (opening.valid()) opening.wait(); break; }

        const bool launched = ring.launched_by_producer();
        std::printf("QCBridgeA: %s\n", why.c_str());
        {
            std::lock_guard<std::mutex> lk(m_statsMutex);
            m_stats.producerAlive = false;
        }
        if (opening.valid()) opening.wait();
        m_aja.close();   // release the card for whoever wants it next (PLAN A6)
        // Explicit close so the pid leaves the header before QCView's next poll.
        ring = OutputRing();
        audio = AudioRing();
        if (launched) {
            setState("quitting (started by QCView)");
            if (m_quit) m_quit();
            break;
        }
        // User-started: back to waiting.
    }
    m_aja.close();
}
