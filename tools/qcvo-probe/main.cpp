// SPDX-License-Identifier: MIT
// qcvo-probe — both ends of QCView's output bus, for measuring either side
// without the other (QCBridgeN/PLAN.md P1).
//
//   qcvo-probe consume --id ndi [--width 1920 --height 1080 --format bgra8|rgba16f]
//                      [--transfer sdr|pq|hlg --fit letterbox|native --content composite|a|b]
//                      [--layers guides,annotations|none] [--seconds 10] [--dump last.ppm]
//                      [--status "text"] [--die-after N]
//     The sink side: opens the rings by id, attaches, writes the request,
//     prints the reply, heartbeats, takes frames and audio, and reports what
//     it saw — cadence, stamps, pixel samples, audio continuity — until the
//     producer retires or dies, or the time is up. --die-after N exits
//     without closing after N frames, the way a crashed sink would.
//
//   qcvo-probe produce --id ndi [--width 1920 --height 1080 --fps 25 --seconds 10]
//                      [--no-audio] [--muted] [--launched] [--retire-after S] [--die-after S]
//     The QCView side: creates the rings, answers requests (anything up to
//     the canvas cap is accepted in the format asked for), publishes a
//     synthetic canvas at --fps only while a sink is present, pushes a 1 kHz
//     tone beside it, and prints every attach, leave, request and status
//     line. --retire-after retires the rings and exits; --die-after exits
//     without cleanup, leaving an orphaned ring with a dead producer pid.
//
// Everything printed is what the other side reads from the mapping. No SDK
// anywhere near this file.

#include "qcbae/output_ring.h"
#include "qcbae/audio_ring.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#  include <unistd.h>
#endif

using namespace qcbae;

namespace {

struct Args {
    std::string cmd, id = "probe", dump, status = "qcvo-probe consuming";
    uint32_t width = 1920, height = 1080, fps = 25;
    OutputPixelFormat format = OutputPixelFormat::BGRA8;
    OutputTransfer transfer = OutputTransfer::SdrRec709;
    OutputFit fit = OutputFit::Letterbox;
    OutputContent content = OutputContent::Composite;
    uint32_t layers = kOutputLayerGuides | kOutputLayerAnnotations;
    double seconds = 10.0, retire_after = -1.0, die_after = -1.0;
    bool audio = true, muted = false, launched = false;
};

bool parse(int argc, char** argv, Args* a) {
    if (argc < 2) return false;
    a->cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&](std::string* out) { if (i + 1 >= argc) return false; *out = argv[++i]; return true; };
        std::string v;
        if (k == "--id")            { if (!next(&v)) return false; a->id = v; }
        else if (k == "--width")    { if (!next(&v)) return false; a->width = static_cast<uint32_t>(std::atoi(v.c_str())); }
        else if (k == "--height")   { if (!next(&v)) return false; a->height = static_cast<uint32_t>(std::atoi(v.c_str())); }
        else if (k == "--fps")      { if (!next(&v)) return false; a->fps = static_cast<uint32_t>(std::atoi(v.c_str())); }
        else if (k == "--seconds")  { if (!next(&v)) return false; a->seconds = std::atof(v.c_str()); }
        else if (k == "--dump")     { if (!next(&v)) return false; a->dump = v; }
        else if (k == "--status")   { if (!next(&v)) return false; a->status = v; }
        else if (k == "--retire-after") { if (!next(&v)) return false; a->retire_after = std::atof(v.c_str()); }
        else if (k == "--die-after")    { if (!next(&v)) return false; a->die_after = std::atof(v.c_str()); }
        else if (k == "--format")   { if (!next(&v)) return false; a->format = v == "rgba16f" ? OutputPixelFormat::RGBA16F : OutputPixelFormat::BGRA8; }
        else if (k == "--transfer") { if (!next(&v)) return false; a->transfer = v == "pq" ? OutputTransfer::PqRec2020 : v == "hlg" ? OutputTransfer::HlgRec2020 : OutputTransfer::SdrRec709; }
        else if (k == "--fit")      { if (!next(&v)) return false; a->fit = v == "native" ? OutputFit::Native : OutputFit::Letterbox; }
        else if (k == "--content")  { if (!next(&v)) return false; a->content = v == "a" ? OutputContent::SideA : v == "b" ? OutputContent::SideB : OutputContent::Composite; }
        else if (k == "--layers")   { if (!next(&v)) return false; a->layers = 0; if (v.find("guides") != std::string::npos) a->layers |= kOutputLayerGuides; if (v.find("annotations") != std::string::npos) a->layers |= kOutputLayerAnnotations; }
        else if (k == "--no-audio") a->audio = false;
        else if (k == "--muted")    a->muted = true;
        else if (k == "--launched") a->launched = true;
        else { std::fprintf(stderr, "unknown argument %s\n", k.c_str()); return false; }
    }
    return a->cmd == "consume" || a->cmd == "produce";
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
double secs_since(int64_t t0) { return static_cast<double>(OutputRing::now_ns() - t0) / 1e9; }

const char* format_name(uint32_t f) {
    switch (static_cast<OutputPixelFormat>(f)) {
        case OutputPixelFormat::BGRA8:   return "BGRA8";
        case OutputPixelFormat::RGBA16F: return "RGBA16F";
        default:                         return "?";
    }
}
const char* refusal_name(uint32_t r) {
    switch (static_cast<OutputRefusal>(r)) {
        case OutputRefusal::None: return "none";
        case OutputRefusal::VersionMismatch: return "version mismatch";
        case OutputRefusal::TooLarge: return "too large";
        case OutputRefusal::UnsupportedFormat: return "unsupported format";
        case OutputRefusal::UnsupportedTransfer: return "unsupported transfer";
        case OutputRefusal::UnsupportedContent: return "unsupported content";
        case OutputRefusal::NoSource: return "no source";
        case OutputRefusal::Busy: return "busy";
    }
    return "?";
}

// --- the sink side -------------------------------------------------------------

int consume(const Args& a) {
    const std::string name = OutputRing::name_for(a.id);
    if (name.empty()) { std::fprintf(stderr, "bad id '%s'\n", a.id.c_str()); return 2; }
    OutputRing ring;
    AudioRing audio;
    const int64_t t0 = OutputRing::now_ns();
    // Wait for the producer, the way a sink started before QCView would.
    while (!ring.open(name)) {
        if (secs_since(t0) > a.seconds) { std::printf("no ring %s within %.0f s\n", name.c_str(), a.seconds); return 1; }
        sleep_ms(250);
    }
    const bool have_audio = audio.open(OutputRing::audio_name_for(a.id));
    std::printf("opened %s: producer pid %u (%s), protocol %u, launched-by-producer %s, audio ring %s\n",
                name.c_str(), ring.header()->producer_pid, ring.producer_alive() ? "alive" : "DEAD",
                ring.producer_protocol(), ring.launched_by_producer() ? "yes" : "no",
                have_audio ? "open" : "absent");

    ring.attach(OutputRing::now_ns());
    ring.write_status(a.status.c_str());
    OutputRequest req {};
    req.protocol_version = kOutputDescVersion;
    req.width = a.width; req.height = a.height;
    req.pixel_format = static_cast<uint32_t>(a.format);
    req.transfer = static_cast<uint32_t>(a.transfer);
    req.fit = static_cast<uint32_t>(a.fit);
    req.content = static_cast<uint32_t>(a.content);
    req.layers = a.layers;
    ring.write_request(req);
    std::printf("request: %ux%u %s transfer %u fit %u content %u layers 0x%x\n",
                req.width, req.height, format_name(req.pixel_format), req.transfer, req.fit, req.content, req.layers);

    uint64_t reply_gen = 0, last_seen = 0, a_seen = 0, dropped = 0, a_known_gen = 0;
    OutputReply reply {};
    bool accepted = false;
    uint64_t frames = 0, audio_packets = 0, audio_gaps = 0, muted_packets = 0;
    int64_t first_frame_ns = 0, last_frame_ns = 0, last_beat = 0, last_report = t0;
    uint64_t expected_first_frame = 0; bool have_expected = false;
    std::vector<uint8_t> last_pixels; OutputFrameDesc last_desc {};
    std::vector<float> planes(static_cast<size_t>(kMaxAudioChannels) * kDefaultAudioSlotFrames);
    if (have_audio) audio.skip_to_latest(&a_seen);
    AudioSession session {};
    std::string why_stopped;

    for (;;) {
        const int64_t now = OutputRing::now_ns();
        if (now - last_beat > 250000000LL) { ring.heartbeat(now); last_beat = now; }

        if (ring.state() == OutputState::Retired) { why_stopped = "producer retired the ring"; break; }
        if (!ring.producer_alive())               { why_stopped = "producer is dead (pid gone)"; break; }
        if (secs_since(t0) > a.seconds)           { why_stopped = "time is up"; break; }

        if (ring.read_reply(&reply, &reply_gen)) {
            if (reply.accepted) {
                accepted = true;
                std::printf("reply: accepted %ux%u %s transfer %u content %u layers 0x%x \"%s\"\n",
                            reply.width, reply.height, format_name(reply.pixel_format), reply.transfer,
                            reply.content, reply.layers, reply.message);
            } else {
                accepted = false;
                std::printf("reply: REFUSED (%s) \"%s\"\n", refusal_name(reply.refusal), reply.message);
            }
        }

        OutputFrameDesc d {}; const void* px = nullptr;
        if (ring.acquire_latest(&last_seen, &d, &px)) {
            if (frames == 0) first_frame_ns = now;
            last_frame_ns = now;
            ++frames;
            ring.report_seen(last_seen);
            if (a.die_after > 0 && static_cast<double>(frames) >= a.die_after) {
                std::printf("dying after %llu frames without closing\n", static_cast<unsigned long long>(frames));
                std::fflush(stdout);
                std::_Exit(0);
            }
            if (frames == 1 || frames % 100 == 0) {
                const auto* b = static_cast<const uint8_t*>(px);
                const uint32_t bpp = output_bytes_per_pixel(static_cast<OutputPixelFormat>(d.pixel_format));
                const size_t centre = static_cast<size_t>(d.height / 2) * d.bytes_per_row + static_cast<size_t>(d.width / 2) * bpp;
                std::printf("frame seq %llu: %ux%u %s row %u content %u layers 0x%x flags 0x%x pts %.3f s src #%lld @ %u/%u tc %08x  centre bytes %02x %02x %02x %02x\n",
                            static_cast<unsigned long long>(last_seen), d.width, d.height, format_name(d.pixel_format),
                            d.bytes_per_row, d.content, d.layers, d.flags,
                            static_cast<double>(d.presentation_ns - t0) / 1e9, static_cast<long long>(d.source_frame_index),
                            d.source_fps_num, d.source_fps_den, d.timecode,
                            b[centre], b[centre + 1], b[centre + 2], b[centre + 3]);
            }
            if (!a.dump.empty()) {
                last_desc = d;
                last_pixels.assign(static_cast<const uint8_t*>(px),
                                   static_cast<const uint8_t*>(px) + static_cast<size_t>(d.bytes_per_row) * d.height);
            }
            ring.release();
        }

        if (have_audio) {
            if (audio.read_session(&session, &a_known_gen)) {
                std::printf("audio session: %u Hz, %u ch, flags 0x%x\n", session.sample_rate, session.channels, session.flags);
                have_expected = false;
            }
            for (int n = 0; n < 64; ++n) {
                AudioPacketDesc ad {};
                const auto r = audio.next_packet(&a_seen, &ad, planes.data(), &dropped);
                if (r == AudioRing::Next::None) break;
                if (r == AudioRing::Next::Resynced) { have_expected = false; continue; }
                ++audio_packets;
                if (ad.flags & kAudioPacketMuted) ++muted_packets;
                if (have_expected && ad.first_frame != expected_first_frame) ++audio_gaps;
                expected_first_frame = ad.first_frame + ad.frames; have_expected = true;
            }
        }

        if (now - last_report > 1000000000LL) {
            last_report = now;
            std::printf("t=%5.1f s  frames %llu  audio packets %llu (gaps %llu, dropped %llu, muted %llu)  producer %s  state %u\n",
                        secs_since(t0), static_cast<unsigned long long>(frames),
                        static_cast<unsigned long long>(audio_packets), static_cast<unsigned long long>(audio_gaps),
                        static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(muted_packets),
                        ring.producer_alive() ? "alive" : "dead", static_cast<unsigned>(ring.state()));
            std::fflush(stdout);
        }
        sleep_ms(2);
    }

    if (!a.dump.empty() && !last_pixels.empty() && last_desc.pixel_format == static_cast<uint32_t>(OutputPixelFormat::BGRA8)) {
        if (FILE* f = std::fopen(a.dump.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", last_desc.width, last_desc.height);
            for (uint32_t y = 0; y < last_desc.height; ++y) {
                const uint8_t* row = last_pixels.data() + static_cast<size_t>(y) * last_desc.bytes_per_row;
                for (uint32_t x = 0; x < last_desc.width; ++x) {
                    const uint8_t rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0] };
                    std::fwrite(rgb, 1, 3, f);
                }
            }
            std::fclose(f);
            std::printf("dumped the last frame to %s\n", a.dump.c_str());
        }
    }
    const double span = frames > 1 ? static_cast<double>(last_frame_ns - first_frame_ns) / 1e9 : 0.0;
    std::printf("stopped: %s. %llu frames%s, %llu audio packets, %llu gaps, %llu dropped, %llu muted; request %s\n",
                why_stopped.c_str(), static_cast<unsigned long long>(frames),
                span > 0 ? (" in " + std::to_string(span).substr(0, 5) + " s = " + std::to_string(static_cast<double>(frames - 1) / span).substr(0, 5) + " fps").c_str() : "",
                static_cast<unsigned long long>(audio_packets), static_cast<unsigned long long>(audio_gaps),
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(muted_packets),
                accepted ? "accepted" : "not accepted");
    return 0;
}

// --- the QCView side -----------------------------------------------------------

// A canvas anyone can recognise: colour bars with a moving white bar and the
// frame number in the top-left byte, so a consumer can tell frames apart
// without decoding anything.
void paint(uint8_t* dst, const OutputFrameDesc& d, uint64_t n) {
    const uint32_t bar = static_cast<uint32_t>((n * 8) % d.width);
    const uint8_t bars[8][3] = { {255,255,255},{0,255,255},{255,255,0},{0,255,0},{255,0,255},{0,0,255},{255,0,0},{0,0,0} };
    for (uint32_t y = 0; y < d.height; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * d.bytes_per_row;
        for (uint32_t x = 0; x < d.width; ++x) {
            const uint8_t* c = bars[(x * 8) / d.width];
            const bool white = x >= bar && x < bar + 16;
            if (d.pixel_format == static_cast<uint32_t>(OutputPixelFormat::BGRA8)) {
                uint8_t* p = row + static_cast<size_t>(x) * 4;
                p[0] = white ? 255 : c[2]; p[1] = white ? 255 : c[1]; p[2] = white ? 255 : c[0]; p[3] = 255;
            } else {
                // RGBA16F: half floats. 1.0 = 0x3C00, 0.0 = 0.
                auto* p = reinterpret_cast<uint16_t*>(row + static_cast<size_t>(x) * 8);
                p[0] = (white || c[0]) ? 0x3C00 : 0; p[1] = (white || c[1]) ? 0x3C00 : 0;
                p[2] = (white || c[2]) ? 0x3C00 : 0; p[3] = 0x3C00;
            }
        }
    }
    dst[0] = static_cast<uint8_t>(n & 0xFF);   // the frame number, in the first byte
}

int produce(const Args& a) {
    const std::string name = OutputRing::name_for(a.id);
    if (name.empty()) { std::fprintf(stderr, "bad id '%s'\n", a.id.c_str()); return 2; }
    OutputRing ring;
    AudioRing audio;
    const uint64_t cap = output_frame_bytes(kOutputMaxWidth, kOutputMaxHeight, OutputPixelFormat::RGBA16F);
    if (!ring.create(name, cap, kDefaultSlots, a.launched ? kOutputLaunchedByProducer : 0u)) {
        std::fprintf(stderr, "create failed: %s\n", ring.error().c_str()); return 1;
    }
    if (a.audio && !audio.create(OutputRing::audio_name_for(a.id), 1000000000LL, kDefaultAudioSlots, 1024, 2)) {
        std::fprintf(stderr, "audio create failed: %s\n", audio.error().c_str()); return 1;
    }
    std::printf("created %s (pid %u, slot capacity %llu bytes, %s)%s\n", name.c_str(), ring.header()->producer_pid,
                static_cast<unsigned long long>(ring.header()->pixels_capacity), a.launched ? "launched-by-producer" : "user-started",
                a.audio ? " + audio ring" : "");

    const int64_t t0 = OutputRing::now_ns();
    uint64_t req_gen = 0, status_gen = 0, frames = 0, audio_frames = 0;
    OutputRequest req {}; OutputReply reply {}; bool accepted = false;
    bool present = false; uint32_t last_pid = 0;
    int64_t next_frame = t0, next_audio = t0, last_report = t0;
    const int64_t frame_ns = static_cast<int64_t>(1e9 / (a.fps > 0 ? a.fps : 25));
    std::vector<float> tone(2 * 1024);
    double phase = 0.0;
    std::string status;
    OutputFrameDesc d {};

    if (a.audio) {
        AudioSession s {}; s.sample_rate = 48000; s.channels = 2; s.speed = 1.0f;
        audio.begin_session(s);
    }

    for (;;) {
        const int64_t now = OutputRing::now_ns();
        const double t = secs_since(t0);
        if (a.retire_after > 0 && t >= a.retire_after) {
            ring.set_state(OutputState::Retired);
            if (a.audio) audio.set_state(AudioState::Retired);
            std::printf("retired the rings at %.1f s\n", t);
            break;
        }
        if (a.die_after > 0 && t >= a.die_after) {
            std::printf("dying at %.1f s without cleanup (%llu frames published)\n", t, static_cast<unsigned long long>(frames));
            std::fflush(stdout);
            std::_Exit(0);
        }
        if (t >= a.seconds) break;

        const bool now_present = ring.consumer_present(now);
        if (now_present != present || (now_present && ring.consumer_pid() != last_pid)) {
            present = now_present; last_pid = ring.consumer_pid();
            std::printf("t=%5.1f s  consumer %s%s\n", t, present ? "attached: pid " : "gone",
                        present ? std::to_string(last_pid).c_str() : "");
            if (!present) accepted = false;
        }
        if (ring.read_request(&req, &req_gen)) {
            std::printf("t=%5.1f s  request %llu: %ux%u %s transfer %u fit %u content %u layers 0x%x\n", t,
                        static_cast<unsigned long long>(req_gen), req.width, req.height, format_name(req.pixel_format),
                        req.transfer, req.fit, req.content, req.layers);
            reply = OutputReply {};
            const auto fmt = static_cast<OutputPixelFormat>(req.pixel_format);
            if (req.protocol_version != kOutputDescVersion) {
                reply.refusal = static_cast<uint32_t>(OutputRefusal::VersionMismatch);
                std::snprintf(reply.message, sizeof reply.message, "protocol %u, this producer speaks %u", req.protocol_version, kOutputDescVersion);
            } else if (req.width == 0 || req.height == 0 || req.width > kOutputMaxWidth || req.height > kOutputMaxHeight) {
                reply.refusal = static_cast<uint32_t>(OutputRefusal::TooLarge);
                std::snprintf(reply.message, sizeof reply.message, "above the %ux%u canvas cap", kOutputMaxWidth, kOutputMaxHeight);
            } else if (output_bytes_per_pixel(fmt) == 0) {
                reply.refusal = static_cast<uint32_t>(OutputRefusal::UnsupportedFormat);
                std::snprintf(reply.message, sizeof reply.message, "pixel format %u", req.pixel_format);
            } else if (req.transfer == 0 || req.transfer > static_cast<uint32_t>(OutputTransfer::HlgRec2020)) {
                reply.refusal = static_cast<uint32_t>(OutputRefusal::UnsupportedTransfer);
                std::snprintf(reply.message, sizeof reply.message, "transfer %u", req.transfer);
            } else {
                reply.accepted = 1;
                reply.width = req.width; reply.height = req.height;
                reply.pixel_format = req.pixel_format; reply.transfer = req.transfer;
                reply.content = req.content == 0 ? static_cast<uint32_t>(OutputContent::Composite) : req.content;
                reply.layers = req.layers;
                std::snprintf(reply.message, sizeof reply.message, "%ux%u %s from qcvo-probe", req.width, req.height, format_name(req.pixel_format));
                d = OutputFrameDesc {};
                d.width = req.width; d.height = req.height;
                d.pixel_format = req.pixel_format; d.transfer = req.transfer;
                d.content = reply.content; d.layers = reply.layers;
                d.bytes_per_row = output_aligned_bytes_per_row(req.width, fmt);
                d.flags = kOutputFrameHasA | kOutputFrameTimecodeValid;
                d.source_fps_num = a.fps; d.source_fps_den = 1;
            }
            accepted = reply.accepted != 0;
            ring.write_reply(reply, req_gen);
            std::printf("t=%5.1f s  reply: %s \"%s\"\n", t, accepted ? "accepted" : refusal_name(reply.refusal), reply.message);
        }
        if (ring.read_status(&status, &status_gen)) std::printf("t=%5.1f s  status: \"%s\"\n", t, status.c_str());

        // Frames only while a sink is present and accepted (O4, O13).
        if (present && accepted && now >= next_frame) {
            next_frame += frame_ns;
            if (next_frame < now) next_frame = now;   // fell behind (a sleep): resume from now, don't burst
            const uint64_t bytes = static_cast<uint64_t>(d.bytes_per_row) * d.height;
            if (void* dst = ring.begin_write(bytes)) {
                paint(static_cast<uint8_t*>(dst), d, frames);
                d.presentation_ns = now;
                d.source_frame_index = static_cast<int64_t>(frames);
                const uint64_t f = frames % a.fps, s = (frames / a.fps) % 60, m = (frames / a.fps / 60) % 60;
                d.timecode = pack_timecode(0, static_cast<uint32_t>(m), static_cast<uint32_t>(s), static_cast<uint32_t>(f));
                ring.commit(d);
                ++frames;
            }
        }
        // Audio keeps its cadence whether or not frames flow (a paused
        // transport still has a clock), muted when asked.
        if (a.audio && now >= next_audio) {
            next_audio += static_cast<int64_t>(1024.0 * 1e9 / 48000.0);
            if (next_audio < now) next_audio = now;
            for (int i = 0; i < 1024; ++i) {
                const float v = a.muted ? 0.0f : 0.2f * static_cast<float>(std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
                tone[i] = v; tone[1024 + i] = v;
            }
            const float* planes[2] = { tone.data(), tone.data() + 1024 };
            audio.push(planes, 2, 48000, 1024, now, a.muted ? kAudioPacketMuted : 0u);
            audio_frames += 1024;
        }
        if (now - last_report > 1000000000LL) {
            last_report = now;
            std::printf("t=%5.1f s  frames %llu  audio frames %llu  consumer %s (seen seq %llu)\n", t,
                        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(audio_frames),
                        present ? "present" : "absent", static_cast<unsigned long long>(ring.consumer_last_seen()));
            std::fflush(stdout);
        }
        sleep_ms(1);
    }
    std::printf("done: %llu frames, %llu audio frames\n", static_cast<unsigned long long>(frames), static_cast<unsigned long long>(audio_frames));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parse(argc, argv, &a)) {
        std::fprintf(stderr, "usage: qcvo-probe consume|produce --id <id> [options]  (see the header of main.cpp)\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    return a.cmd == "consume" ? consume(a) : produce(a);
}
