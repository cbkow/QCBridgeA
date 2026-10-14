// SPDX-License-Identifier: MIT
#include "aja_output.h"

#include <ntv2card.h>
#include <ntv2devicescanner.h>
#include <ntv2formatdescriptor.h>
#include <ntv2publicinterface.h>
#include <ntv2signalrouter.h>
#include <ntv2utils.h>
#include <ajabase/system/process.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

const ULWord kAppSignature = NTV2_FOURCC('Q', 'C', 'B', 'A');

// The largest audio chunk one frame can carry: 2002 sample frames at
// 23.976 fps, 16 channels, 4 bytes each; rounded up to pages.
constexpr uint32_t kAudioBufferBytes = 4096u * 16u * 4u;

bool rateIs(double a, double b) { return std::fabs(a - b) < 0.02; }

// PLAN A3: progressive UHD, 1080 and 720 at the rates QCView produces.
NTV2VideoFormat pickFormat(uint32_t w, uint32_t h, uint32_t num, uint32_t den)
{
    if (!num || !den) return NTV2_FORMAT_UNKNOWN;
    const double fps = double(num) / double(den);
    if (w == 3840 && h == 2160) {
        if (rateIs(fps, 23.976)) return NTV2_FORMAT_3840x2160p_2398;
        if (rateIs(fps, 24.0))   return NTV2_FORMAT_3840x2160p_2400;
        if (rateIs(fps, 25.0))   return NTV2_FORMAT_3840x2160p_2500;
        if (rateIs(fps, 29.97))  return NTV2_FORMAT_3840x2160p_2997;
        if (rateIs(fps, 30.0))   return NTV2_FORMAT_3840x2160p_3000;
        if (rateIs(fps, 50.0))   return NTV2_FORMAT_3840x2160p_5000;
        if (rateIs(fps, 59.94))  return NTV2_FORMAT_3840x2160p_5994;
        if (rateIs(fps, 60.0))   return NTV2_FORMAT_3840x2160p_6000;
    } else if (w == 1920 && h == 1080) {
        if (rateIs(fps, 23.976)) return NTV2_FORMAT_1080p_2398;
        if (rateIs(fps, 24.0))   return NTV2_FORMAT_1080p_2400;
        if (rateIs(fps, 25.0))   return NTV2_FORMAT_1080p_2500;
        if (rateIs(fps, 29.97))  return NTV2_FORMAT_1080p_2997;
        if (rateIs(fps, 30.0))   return NTV2_FORMAT_1080p_3000;
        if (rateIs(fps, 50.0))   return NTV2_FORMAT_1080p_5000_A;
        if (rateIs(fps, 59.94))  return NTV2_FORMAT_1080p_5994_A;
        if (rateIs(fps, 60.0))   return NTV2_FORMAT_1080p_6000_A;
    } else if (w == 1280 && h == 720) {
        if (rateIs(fps, 23.976)) return NTV2_FORMAT_720p_2398;
        if (rateIs(fps, 25.0))   return NTV2_FORMAT_720p_2500;
        if (rateIs(fps, 50.0))   return NTV2_FORMAT_720p_5000;
        if (rateIs(fps, 59.94))  return NTV2_FORMAT_720p_5994;
        if (rateIs(fps, 60.0))   return NTV2_FORMAT_720p_6000;
    }
    return NTV2_FORMAT_UNKNOWN;
}

} // namespace

struct AjaOutput::Impl {
    // Who holds the device: the registered application code and, when the
    // pid resolves, the process name.
    std::string ownerName();

    CNTV2Card        card;
    bool             acquired = false;
    NTV2EveryFrameTaskMode savedTaskMode = NTV2_TASK_MODE_INVALID;
    NTV2Channel      ch = NTV2_CHANNEL1;
    NTV2AudioSystem  audioSys = NTV2_AUDIOSYSTEM_1;
    NTV2VideoFormat  vf = NTV2_FORMAT_UNKNOWN;
    NTV2PixelFormat  pf = NTV2_FBF_ARGB;      // B,G,R,A in memory: the bus's BGRA8 (PLAN A4)
    NTV2FrameRate    frameRate = NTV2_FRAMERATE_UNKNOWN;
    bool             hdmi = false;
    bool             hdmiRgb = true;
    bool             hdmiYcbcrForced = false;   // UHD above 30p: RGB asked for, YCbCr sent (HDMI 2.0)
    bool             hdmi4k = false;            // the HDMI rasteriser is in 4K playback mode
    bool             tenBit = false;
    bool             cfgOk = false;
    AjaOutput::HdrSignal hdr;
    bool             configured = false;
    bool             running = false;
    uint32_t         width = 0, height = 0, frameBytes = 0, rowBytes = 0, audioCh = 0;
    uint32_t         cadence = 0;
    uint64_t         goodXfers = 0;
    NTV2Buffer       videoBuf, audioBuf;
    std::string      fmtName, devName;
    AjaOutputStats   stats;

    void stopCirculate()
    {
        if (!configured) return;
        card.AutoCirculateStop(ch);
        card.AutoCirculateFlush(ch);
        running = false;
        stats.running = false;
    }
};

std::string AjaOutput::Impl::ownerName()
{
    ULWord appType = 0;
    int32_t pid = 0;
    if (!card.GetStreamingApplication(appType, pid) || !appType) return "another application";
    char code[5] = { char((appType >> 24) & 0xFF), char((appType >> 16) & 0xFF), char((appType >> 8) & 0xFF), char(appType & 0xFF), 0 };
    for (char &c : code) if (c && (c < 0x20 || c > 0x7E)) c = '?';
    std::string who = std::string("'") + code + "'";
#if defined(_WIN32)
    if (pid > 0) {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        if (h) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (::QueryFullProcessImageNameW(h, 0, path, &n)) {
                std::wstring ws(path, n);
                const auto slash = ws.find_last_of(L"\\/");
                const std::wstring base = slash == std::wstring::npos ? ws : ws.substr(slash + 1);
                std::string name;
                for (wchar_t c : base) name += (c < 0x80) ? char(c) : '?';
                who = name;
            }
            ::CloseHandle(h);
        }
    }
#endif
    if (pid > 0) who += " (pid " + std::to_string(pid) + ")";
    return who;
}

AjaOutput::AjaOutput() : m(std::make_unique<Impl>()) {}
AjaOutput::~AjaOutput() { close(); }

bool AjaOutput::isOpen() const { return m->acquired; }

bool AjaOutput::open(const std::string &deviceSpec, std::string *err)
{
    close();
    // A bare index opens directly (the scanner's full enumeration takes about
    // a second, long enough to miss a heartbeat); names and serials go through it.
    bool opened = false;
    if (!deviceSpec.empty() && deviceSpec.find_first_not_of("0123456789") == std::string::npos)
        opened = m->card.Open(UWord(std::stoul(deviceSpec)));
    if (!opened) opened = CNTV2DeviceScanner::GetFirstDeviceFromArgument(deviceSpec, m->card);
    if (!opened) {
        if (err) *err = deviceSpec == "0" ? "no AJA device" : "no AJA device matching '" + deviceSpec + "'";
        return false;
    }
    m->devName = ::NTV2DeviceIDToString(m->card.GetDeviceID(), true);
    if (!m->card.IsDeviceReady(false)) {
        if (err) *err = m->devName + " is not ready";
        return false;
    }
    if (!m->card.features().CanDoPlayback()) {
        if (err) *err = m->devName + " has no outputs";
        return false;
    }
    m->card.GetEveryFrameServices(m->savedTaskMode);
    // PLAN A6: exclusive use, the way AJA's own applications take a device.
    if (!m->card.AcquireStreamForApplication(kAppSignature, int32_t(AJAProcess::GetPid()))) {
        if (err) *err = m->devName + " is in use by " + m->ownerName();
        m->card.Close();
        return false;
    }
    m->acquired = true;
    m->card.SetEveryFrameServices(NTV2_OEM_TASKS);
    if (m->card.features().CanDoMultiFormat()) m->card.SetMultiFormatMode(false);
    if (m->ch == NTV2_CHANNEL1 && !m->card.features().CanDoFrameStore1Display()) m->ch = NTV2_CHANNEL2;
    return true;
}

void AjaOutput::close()
{
    if (!m->card.IsOpen()) return;
    m->stopCirculate();
    if (m->configured) {
        m->card.UnsubscribeOutputVerticalEvent(m->ch);
        m->card.DMABufferUnlockAll();
        m->configured = false;
    }
    if (m->acquired) {
        m->card.ReleaseStreamForApplication(kAppSignature, int32_t(AJAProcess::GetPid()));
        if (NTV2_IS_VALID_TASK_MODE(m->savedTaskMode)) m->card.SetEveryFrameServices(m->savedTaskMode);
        m->acquired = false;
    }
    m->card.Close();
}

bool AjaOutput::configure(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, bool tenBit, uint32_t transfer,
                          uint32_t audioChannels, bool hdmi, HdmiColor hdmiColor, uint32_t cardFrames, std::string *err)
{
    if (!m->acquired) { if (err) *err = "device not open"; return false; }
    m->stopCirculate();
    m->cfgOk = false;
    // PLAN A4: the bus's RGB10 is the card's 10-bit RGB word for word; BGRA8 is ARGB (B,G,R,A).
    m->pf = tenBit ? NTV2_FBF_10BIT_RGB : NTV2_FBF_ARGB;

    const NTV2VideoFormat vf = pickFormat(width, height, fpsNum, fpsDen);
    if (vf == NTV2_FORMAT_UNKNOWN) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "no SDI format for %ux%u at %u/%u", width, height, fpsNum, fpsDen);
        if (err) *err = buf;
        return false;
    }
    if (!m->card.features().CanDoVideoFormat(vf)) {
        if (err) *err = m->devName + " cannot output " + ::NTV2VideoFormatToString(vf, true);
        return false;
    }
    // UHD here is one 12G stream from one frame store (T-TAP Pro, Kona 5 and
    // Corvid 44 12G firmware, Io 4K Plus): the frame store runs in two-sample
    // interleave mode and the routing is the HD routing. Quad-link 4K over
    // four 3G outputs is a different routing and is not done.
    const bool uhd = NTV2_IS_4K_VIDEO_FORMAT(vf);
    if (uhd && !(m->card.features().CanDo4KVideo() && m->card.features().CanDo12gRouting())) {
        if (err) *err = m->devName + " has no single-link 12G UHD output";
        return false;
    }
    if (!m->card.features().CanDoFrameBufferFormat(m->pf)) {
        if (err) *err = m->devName + (tenBit ? " has no 10-bit RGB frame store" : " has no 8-bit ARGB frame store");
        return false;
    }

    // Video: the frame store and its output interrupt.
    m->card.EnableChannel(m->ch);
    m->card.SetVANCMode(NTV2_VANCMODE_OFF, m->ch);
    m->card.SetVideoFormat(vf, false, false, m->ch);
    m->card.SetFrameBufferFormat(m->ch, m->pf);
    m->card.SubscribeOutputVerticalEvent(m->ch);
    m->card.SetReference(NTV2_REFERENCE_FREERUN);

    // Audio: 48 kHz, `audioChannels` embedded on SDI, the first eight on HDMI (PLAN A5).
    const uint32_t maxCh = m->card.features().GetMaxAudioChannels();
    m->audioCh = std::clamp<uint32_t>(audioChannels, 2u, std::max<uint32_t>(2u, maxCh));
    m->audioSys = m->card.features().GetNumAudioSystems() > 1 ? ::NTV2ChannelToAudioSystem(m->ch) : NTV2_AUDIOSYSTEM_1;
    m->card.SetNumberAudioChannels(m->audioCh, m->audioSys);
    m->card.SetAudioRate(NTV2_AUDIO_48K, m->audioSys);
    m->card.SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG, m->audioSys);
    m->card.SetSDIOutputAudioSystem(m->ch, m->audioSys);
    m->card.SetSDIOutputDS2AudioSystem(m->ch, m->audioSys);
    m->card.SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF, m->audioSys);
    const bool haveHdmi = hdmi && m->card.features().GetNumHDMIVideoOutputs() > 0;
    if (haveHdmi) {
        m->card.SetHDMIOutAudioRate(NTV2_AUDIO_48K);
        m->card.SetHDMIOutAudioFormat(NTV2_AUDIO_FORMAT_LPCM);
        m->card.SetHDMIOutAudioSource8Channel(NTV2_AudioChannel1_8, m->audioSys);
    }

    // Routing (the SDK's player demo, RGB branch): frame store → CSC → SDI as
    // Rec.709 YCbCr 4:2:2 from full-range RGB. HDMI takes the frame store's RGB
    // straight (nothing re-matrixed or subsampled on the way to a QC monitor),
    // or the CSC's YCbCr when asked.
    const bool canVerify = m->card.features().HasCrosspointConnectROM();
    const NTV2OutputXptID cscVidOut = ::GetCSCOutputXptFromChannel(m->ch, false /*key*/, false /*RGB out*/);
    const NTV2OutputXptID fsVidOut  = ::GetFrameStoreOutputXptFromChannel(m->ch, true /*RGB*/, false);
    const NTV2InputXptID  cscIn     = ::GetCSCInputXptFromChannel(m->ch, false);
    m->card.ClearRouting();
    m->card.Connect(cscIn, fsVidOut, canVerify);
    // Full-range RGB in; Rec.709 YCbCr out for SDR, Rec.2020 for PQ and HLG
    // (the enhanced CSC has the 2020 matrix; the original one is 709 only).
    const bool hdr = transfer == 2u || transfer == 3u;
    m->card.SetColorSpaceRGBBlackRange(NTV2_CSC_RGB_RANGE_FULL, m->ch);
    if (m->card.features().CanDoEnhancedCSC()) {
        m->card.SetColorSpaceMethod(NTV2_CSC_Method_Enhanced, m->ch);
        m->card.SetColorSpaceMatrixSelect(hdr ? NTV2_GBRFull_to_YCbCr_Rec2020_Matrix : NTV2_GBRFull_to_YCbCr_Rec709_Matrix, m->ch);
    } else {
        m->card.SetColorSpaceMatrixSelect(NTV2_Rec709Matrix, m->ch);
        if (hdr && err) *err = m->devName + " has no Rec.2020 matrix";   // reported, not fatal: the picture still goes out
    }
    if (m->card.features().HasBiDirectionalSDI()) m->card.SetSDITransmitEnable(m->ch, true);
    m->card.Connect(::GetSDIOutputInputXpt(m->ch, false), cscVidOut, canVerify);
    m->card.SetSDIOutputStandard(m->ch, ::GetNTV2StandardFromVideoFormat(vf));
    // UHD above 30p does not fit HDMI 2.0 as 10-bit RGB 4:4:4 (18 Gbit/s
    // carries UHD60 at 4:2:2); the CSC's YCbCr goes to the monitor then.
    const bool hdmiYcbcrForced = uhd && NTV2_IS_4K_HFR_VIDEO_FORMAT(vf) && hdmiColor == HdmiColor::Rgb;
    if (haveHdmi) {
        const bool rgb = hdmiColor == HdmiColor::Rgb && !hdmiYcbcrForced;
        m->card.Connect(::GetOutputDestInputXpt(NTV2_OUTPUTDESTINATION_HDMI1), rgb ? fsVidOut : cscVidOut, canVerify);
        m->card.SetHDMIOutColorSpace(rgb ? NTV2_HDMIColorSpaceRGB : NTV2_HDMIColorSpaceYCbCr);
        m->card.SetHDMIOutRange(rgb ? NTV2_HDMIRangeFull : NTV2_HDMIRangeSMPTE);
        m->card.SetHDMIOutBitDepth(tenBit ? NTV2_HDMI10Bit : NTV2_HDMI8Bit);
        // The HDMI rasteriser's own idea of the picture (the 4K player demo's
        // RouteHDMIOutput): the standard, the rate and 4K playback mode for
        // UHD, and back to the HD/SD mode after a UHD session. An HD session
        // that never went UHD is left as it always was.
        if (uhd || m->hdmi4k) {
            m->card.SetHDMIOutVideoStandard(::GetNTV2StandardFromVideoFormat(vf));
            m->card.SetHDMIOutVideoFPS(::GetNTV2FrameRateFromVideoFormat(vf));
            m->card.SetHDMIV2Mode(uhd ? NTV2_HDMI_V2_4K_PLAYBACK : NTV2_HDMI_V2_HDSD_BIDIRECTIONAL);
            m->hdmi4k = uhd;
        }
    }
    // SetVideoFormat put the frame store into quad-frame (two-sample interleave)
    // mode for UHD; said explicitly, as the 4K player demo does after routing.
    if (m->card.features().CanDo12gRouting()) m->card.SetTsiFrameEnable(uhd, m->ch);

    // Host buffers: page-aligned and locked for DMA.
    const NTV2FormatDescriptor fd(vf, m->pf, NTV2_VANCMODE_OFF);
    if (!fd.IsValid() || fd.GetRasterWidth() != width || fd.GetFullRasterHeight() != height) {
        if (err) *err = "format descriptor mismatch for " + ::NTV2VideoFormatToString(vf, true);
        return false;
    }
    m->frameBytes = fd.GetTotalBytes();
    m->rowBytes   = fd.GetBytesPerRow();
    m->card.DMABufferUnlockAll();
    if (!m->videoBuf.Allocate(m->frameBytes, true) || !m->audioBuf.Allocate(kAudioBufferBytes, true)) {
        if (err) *err = "could not allocate the DMA buffers";
        return false;
    }
    m->videoBuf.Fill(uint32_t(0));   // black until the first frame is staged
    m->audioBuf.Fill(uint32_t(0));
    m->card.DMABufferLock(m->videoBuf, true);
    m->card.DMABufferLock(m->audioBuf, true);

    // AutoCirculate over `cardFrames` on-card buffers (PLAN A2).
    const UWord frames = UWord(std::clamp<uint32_t>(cardFrames, 3u, 16u));
    m->card.AutoCirculateStop(m->ch);
    if (!m->card.AutoCirculateInitForOutput(m->ch, frames, m->audioSys, 0)) {
        if (err) *err = "AutoCirculateInitForOutput failed";
        return false;
    }

    m->vf = vf;
    m->hdmi = haveHdmi;
    m->hdmiRgb = hdmiColor == HdmiColor::Rgb && !hdmiYcbcrForced;
    m->hdmiYcbcrForced = hdmiYcbcrForced;
    m->tenBit = tenBit;
    m->hdr = HdrSignal{};
    m->hdr.transfer = transfer;
    m->cfgOk = true;
    setHdr(m->hdr);
    m->width = width; m->height = height;
    m->frameRate = ::GetNTV2FrameRateFromVideoFormat(vf);
    m->fmtName = ::NTV2VideoFormatToString(vf, true);
    m->cadence = 0;
    m->goodXfers = 0;
    m->stats = AjaOutputStats{};
    m->configured = true;
    return true;
}

void AjaOutput::setHdr(const HdrSignal &hdr)
{
    if (!m->acquired || !m->cfgOk) return;
    m->hdr = hdr;
    const bool pq = hdr.transfer == 2u, hlg = hdr.transfer == 3u, isHdr = pq || hlg;

    // SDI: the VPID says what the samples are.
    m->card.SetVPIDTransferCharacteristics(pq ? NTV2_VPID_TC_PQ : hlg ? NTV2_VPID_TC_HLG : NTV2_VPID_TC_SDR_TV, m->ch);
    m->card.SetVPIDColorimetry(isHdr ? NTV2_VPID_Color_UHDTV : NTV2_VPID_Color_Rec709, m->ch);
    m->card.SetVPIDLuminance(NTV2_VPID_Luminance_YCbCr, m->ch);

    // HDMI: the HDR InfoFrame (CTA-861 static metadata) or none.
    if (!m->hdmi || !m->card.features().CanDoHDMIHDROut()) return;
    if (!isHdr) { m->card.EnableHDMIHDR(false); return; }
    HDRFloatValues v {};
    const bool p3 = hdr.primaries == 2u;
    v.greenPrimaryX = p3 ? 0.265f : 0.170f;  v.greenPrimaryY = p3 ? 0.690f : 0.797f;
    v.bluePrimaryX  = p3 ? 0.150f : 0.131f;  v.bluePrimaryY  = p3 ? 0.060f : 0.046f;
    v.redPrimaryX   = p3 ? 0.680f : 0.708f;  v.redPrimaryY   = p3 ? 0.320f : 0.292f;
    v.whitePointX   = 0.3127f;               v.whitePointY   = 0.3290f;
    v.maxMasteringLuminance = pq ? uint16_t(std::clamp(hdr.maxMasteringNits, 0.f, 65535.f)) : 0u;
    v.minMasteringLuminance = pq ? hdr.minMasteringNits : 0.f;
    v.maxContentLightLevel      = pq ? uint16_t(std::min<uint32_t>(hdr.maxCll, 65535u)) : 0u;
    v.maxFrameAverageLightLevel = pq ? uint16_t(std::min<uint32_t>(hdr.maxFall, 65535u)) : 0u;
    v.electroOpticalTransferFunction = pq ? 2u : 3u;   // CTA-861: 2 = ST 2084, 3 = HLG
    v.staticMetadataDescriptorID = 0u;
    m->card.SetHDRData(v);
    m->card.SetHDMIHDRConstantLuminance(false);
    m->card.EnableHDMIHDR(true);
}

void AjaOutput::stageFrame(const void *pixels, uint32_t bytesPerRow)
{
    if (!m->configured || !pixels) return;
    auto *dst = static_cast<uint8_t *>(m->videoBuf.GetHostPointer());
    if (bytesPerRow == m->rowBytes) {
        std::memcpy(dst, pixels, m->frameBytes);
    } else {
        const auto *src = static_cast<const uint8_t *>(pixels);
        for (uint32_t y = 0; y < m->height; ++y)
            std::memcpy(dst + size_t(y) * m->rowBytes, src + size_t(y) * bytesPerRow, m->rowBytes);
    }
}

bool AjaOutput::transfer(const int32_t *audio, uint32_t audioFrames)
{
    if (!m->configured) return false;
    AUTOCIRCULATE_TRANSFER xfer;
    xfer.SetVideoBuffer(static_cast<ULWord *>(m->videoBuf.GetHostPointer()), m->frameBytes);
    const uint32_t audioBytes = std::min<uint32_t>(audioFrames * m->audioCh * 4u, m->audioBuf.GetByteCount());
    if (audioBytes) {
        if (audio) std::memcpy(m->audioBuf.GetHostPointer(), audio, audioBytes);
        else       std::memset(m->audioBuf.GetHostPointer(), 0, audioBytes);
        xfer.SetAudioBuffer(static_cast<ULWord *>(m->audioBuf.GetHostPointer()), audioBytes);
    }
    if (!m->card.AutoCirculateTransfer(m->ch, xfer)) {
        ++m->stats.badTransfers;
        return false;
    }
    ++m->stats.transfers;
    if (!m->running && ++m->goodXfers >= 2) {
        m->card.AutoCirculateStart(m->ch);
        m->running = true;
        m->stats.running = true;
    }
    return true;
}

bool AjaOutput::canAcceptFrame()
{
    if (!m->configured) return false;
    AUTOCIRCULATE_STATUS st;
    if (!m->card.AutoCirculateGetStatus(m->ch, st)) return false;
    m->stats.bufferLevel = st.GetBufferLevel();
    m->stats.cardDropped = st.GetDroppedFrameCount();
    return st.CanAcceptMoreOutputFrames();
}

void AjaOutput::waitForOutputInterrupt()
{
    if (!m->configured) return;
    m->card.WaitForOutputVerticalInterrupt(m->ch);
    ++m->stats.vbiWaits;
}

uint32_t AjaOutput::audioFramesForNextFrame()
{
    if (!m->configured) return 0;
    return ::GetAudioSamplesPerFrame(m->frameRate, NTV2_AUDIO_48K, m->cadence++);
}

uint32_t AjaOutput::audioChannels() const { return m->audioCh; }
uint32_t AjaOutput::frameBytes() const { return m->frameBytes; }
std::string AjaOutput::deviceName() const { return m->devName; }
std::string AjaOutput::formatName() const { return m->fmtName; }

std::string AjaOutput::describe() const
{
    if (!m->acquired) return "no device";
    if (!m->configured) return m->devName;
    const char *tf = m->hdr.transfer == 2u ? " PQ 2020" : m->hdr.transfer == 3u ? " HLG 2020" : " SDR 709";
    return m->devName + " " + m->fmtName + (m->tenBit ? " 10-bit" : " 8-bit") + tf + ", SDI"
         + (m->hdmi ? (m->hdmiRgb ? " + HDMI RGB" : m->hdmiYcbcrForced ? " + HDMI YCbCr (RGB needs 30p or below at UHD)" : " + HDMI YCbCr") : "");
}

AjaOutputStats AjaOutput::stats()
{
    if (m->configured) {
        AUTOCIRCULATE_STATUS st;
        if (m->card.AutoCirculateGetStatus(m->ch, st)) {
            m->stats.bufferLevel = st.GetBufferLevel();
            m->stats.cardDropped = st.GetDroppedFrameCount();
        }
    }
    return m->stats;
}
