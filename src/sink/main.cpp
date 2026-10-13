// SPDX-License-Identifier: MIT
// QCBridgeA — the application: a headless process over BusSink (PLAN A7).
//
//   QCBridgeA [--id aja] [--device 0] [--raster 1920x1080]
//             [--content composite|a|b] [--no-guides] [--no-annotations]
//             [--no-hdmi] [--audio-channels 8] [--card-frames 4] [--fps 25]
//             [--seconds N] [--version]
//
// Settings are arguments (the manifest's); --seconds quits after N seconds
// (tests). Ctrl-C releases the card and exits.

#include "aja_output.h"
#include "bus_sink.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit.store(true); }

bool parseRaster(const std::string &s, uint32_t *w, uint32_t *h)
{
    const auto x = s.find('x');
    if (x == std::string::npos) return false;
    *w = uint32_t(std::strtoul(s.substr(0, x).c_str(), nullptr, 10));
    *h = uint32_t(std::strtoul(s.substr(x + 1).c_str(), nullptr, 10));
    return *w > 0 && *h > 0;
}

void usage()
{
    std::printf("QCBridgeA %s — QCView's output bus onto an AJA device\n"
                "  --id <id>            output id in QCView (default aja)\n"
                "  --device <spec>      AJA device index, serial or model (default 0)\n"
                "  --raster WxH         1920x1080 (default) or 1280x720\n"
                "  --content <what>     composite (default), a or b\n"
                "  --no-guides          do not ask for safety guides\n"
                "  --no-annotations     do not ask for note strokes\n"
                "  --no-hdmi            SDI only\n"
                "  --audio-channels N   embedded channels, 2..16 (default 8)\n"
                "  --card-frames N      AutoCirculate buffers, 3..16 (default 4)\n"
                "  --fps N              rate when the stamp has none (default 25; 23.976, 29.97 and 59.94 accepted)\n"
                "  --seconds N          quit after N seconds\n", QCBRIDGEA_VERSION);
}

} // namespace

int main(int argc, char **argv)
{
    SinkConfig cfg;
    int seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "QCBridgeA: %s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--id")             cfg.id = next("--id");
        else if (a == "--device")         cfg.device = next("--device");
        else if (a == "--raster")         { if (!parseRaster(next("--raster"), &cfg.width, &cfg.height)) { std::fprintf(stderr, "QCBridgeA: bad --raster\n"); return 2; } }
        else if (a == "--content")        { const std::string c = next("--content");
                                            cfg.content = c == "a" ? qcbae::OutputContent::SideA : c == "b" ? qcbae::OutputContent::SideB : qcbae::OutputContent::Composite; }
        else if (a == "--no-guides")      cfg.layers &= ~qcbae::kOutputLayerGuides;
        else if (a == "--no-annotations") cfg.layers &= ~qcbae::kOutputLayerAnnotations;
        else if (a == "--no-hdmi")        cfg.hdmi = false;
        else if (a == "--audio-channels") cfg.audioChannels = uint32_t(std::strtoul(next("--audio-channels").c_str(), nullptr, 10));
        else if (a == "--card-frames")    cfg.cardFrames = uint32_t(std::strtoul(next("--card-frames").c_str(), nullptr, 10));
        else if (a == "--fps")            { const double f = std::strtod(next("--fps").c_str(), nullptr);
                                            if (f > 23.9 && f < 24.0)      { cfg.fallbackFpsNum = 24000; cfg.fallbackFpsDen = 1001; }
                                            else if (f > 29.9 && f < 30.0) { cfg.fallbackFpsNum = 30000; cfg.fallbackFpsDen = 1001; }
                                            else if (f > 59.9 && f < 60.0) { cfg.fallbackFpsNum = 60000; cfg.fallbackFpsDen = 1001; }
                                            else { cfg.fallbackFpsNum = uint32_t(f + 0.5); cfg.fallbackFpsDen = 1; } }
        else if (a == "--seconds")        seconds = std::atoi(next("--seconds").c_str());
        else if (a == "--version")        { std::printf("QCBridgeA %s\n", QCBRIDGEA_VERSION); return 0; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "QCBridgeA: unknown option %s\n", a.c_str()); usage(); return 2; }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::printf("QCBridgeA %s: id %s, device %s, %ux%u, %s%s\n", QCBRIDGEA_VERSION, cfg.id.c_str(), cfg.device.c_str(),
                cfg.width, cfg.height, cfg.hdmi ? "SDI + HDMI" : "SDI", seconds ? ", timed run" : "");
    std::fflush(stdout);

    AjaOutput aja;
    BusSink sink(aja, cfg);
    sink.setQuitRequest([] { g_quit.store(true); });
    sink.start();

    const auto t0 = std::chrono::steady_clock::now();
    while (!g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (seconds > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(seconds)) break;
    }
    sink.stop();
    aja.close();
    const SinkStats s = sink.stats();
    std::printf("QCBridgeA: done - %llu pictures, %llu transfers, %llu audio packets (%llu gaps, %llu dropped, "
                "%llu silence frames, %llu trimmed)\n",
                (unsigned long long)s.frames, (unsigned long long)s.transfers, (unsigned long long)s.audioPackets,
                (unsigned long long)s.audioGaps, (unsigned long long)s.audioDropped,
                (unsigned long long)s.silenceFrames, (unsigned long long)s.trimmedFrames);
    return 0;
}
