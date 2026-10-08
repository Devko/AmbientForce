// WAV files in and out: see wav.h. The RIFF walk and the sample formats are PolyForce's WAV import
// (dsp/wavetable.cpp, 61543b4).
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64   // a WAV can pass 2 GB, and off_t is 32 bits on the device unless told
#endif
#include "wav.h"

#include "paths.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace af {
namespace {

static_assert(sizeof(off_t) == 8, "offsets into a big WAV");

constexpr int kOutRate = 44100;
constexpr uint32_t kMinRate = 1000, kMaxRate = 384000;
constexpr int kMaxChannels = 8;
constexpr int kMaxChunks = 256;          // chunk headers walked: a file of tiny chunks isn't crawled through
constexpr size_t kReadFrames = 4096;     // frames decoded at a time
constexpr float kMaxSample = 1.0e6f;     // a float WAV's samples are held within this (+120 dBFS)

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
void wr16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t* p, uint32_t v) {
    wr16(p, v);
    wr16(p + 2, v >> 16);
}

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

struct File {
    FILE* f;
    explicit File(FILE* file) : f(file) {}
    ~File() {
        if (f) std::fclose(f);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

bool readAt(FILE* f, uint64_t at, void* buf, size_t n) {
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0 && std::fread(buf, 1, n, f) == n;
}

struct Format {
    uint16_t tag = 0, channels = 0, bits = 0, align = 0;
    uint32_t rate = 0;
};

// The walk: fmt and data wherever they are. fmt is read; data is found (where it starts and how long
// it runs).
bool walk(FILE* f, uint64_t size, Format& fmt, uint64_t& dataAt, uint64_t& dataBytes, std::string* err) {
    uint8_t head[12];
    if (size < 12 || !readAt(f, 0, head, 12) || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0)
        return fail(err, "not a WAV file");
    bool gotFmt = false, gotData = false;
    uint64_t pos = 12;
    for (int n = 0; n < kMaxChunks && pos + 8 <= size && !(gotFmt && gotData); ++n) {
        uint8_t c[8];
        if (!readAt(f, pos, c, 8)) return fail(err, "cannot read");
        const uint64_t sz = rd32(c + 4), body = pos + 8;
        if (!std::memcmp(c, "fmt ", 4) && !gotFmt) {
            if (sz < 16) return fail(err, "bad fmt chunk");
            if (body + sz > size) return fail(err, "truncated");
            uint8_t b[40] = {};
            const size_t take = static_cast<size_t>(std::min<uint64_t>(sz, sizeof b));
            if (!readAt(f, body, b, take)) return fail(err, "cannot read");
            fmt.tag = rd16(b);
            fmt.channels = rd16(b + 2);
            fmt.rate = rd32(b + 4);
            fmt.align = rd16(b + 12);
            fmt.bits = rd16(b + 14);
            if (fmt.tag == 0xFFFE) {   // WAVE_FORMAT_EXTENSIBLE: the real tag opens the sub-format
                if (take < 26) return fail(err, "bad fmt chunk");
                fmt.tag = rd16(b + 24);
            }
            gotFmt = true;
        } else if (!std::memcmp(c, "data", 4) && !gotData) {
            gotData = true;
            dataAt = body;
            const uint64_t avail = size - body;
            // 0 and 0xFFFFFFFF are what a writer leaves before it knows the length: to the end.
            const bool open = sz == 0 || sz == 0xFFFFFFFFu;
            dataBytes = open ? avail : sz;
            if (dataBytes > avail) return fail(err, "truncated");
            if (open) break;   // nothing can follow it
        }
        pos = body + sz + (sz & 1);   // 64-bit: no overflow on a bogus size
    }
    if (!gotFmt) return fail(err, "no fmt chunk");
    if (!gotData) return fail(err, "no data chunk");
    return true;
}

bool checkFormat(const Format& f, std::string* err) {
    const bool pcm = f.tag == 1 && (f.bits == 16 || f.bits == 24 || f.bits == 32);
    const bool flt = f.tag == 3 && f.bits == 32;
    if (!pcm && !flt) return fail(err, "unsupported sample format (16, 24, 32-bit PCM or 32-bit float)");
    if (f.channels < 1 || f.channels > kMaxChannels) return fail(err, "unsupported channel count (1 to 8)");
    if (f.rate < kMinRate || f.rate > kMaxRate) return fail(err, "unsupported sample rate");
    // Frames are packed: a block align under the samples' bytes reads past a frame, and one far over
    // them is no WAV we know (it also sizes the read buffer).
    if (f.align < f.channels * (f.bits / 8) || f.align > kMaxChannels * 4) return fail(err, "bad fmt chunk");
    return true;
}

float oneSample(const Format& f, const uint8_t* p) {
    if (f.tag == 3) {
        float v;
        std::memcpy(&v, p, sizeof v);
        return std::isfinite(v) ? std::max(-kMaxSample, std::min(v, kMaxSample)) : 0.0f;
    }
    if (f.bits == 16) return static_cast<float>(static_cast<int16_t>(rd16(p))) / 32768.0f;
    if (f.bits == 24) {
        int32_t v = static_cast<int32_t>(p[0] | p[1] << 8 | p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        return static_cast<float>(v) / 8388608.0f;
    }
    return static_cast<float>(static_cast<int32_t>(rd32(p))) / 2147483648.0f;
}

// n frames of the file as L R L R ... (a mono file twice).
void decode(const Format& f, const uint8_t* src, size_t n, float* lr) {
    const size_t bytes = f.bits / 8;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = src + i * f.align;
        const float a = oneSample(f, p);
        lr[2 * i] = a;
        lr[2 * i + 1] = f.channels > 1 ? oneSample(f, p + bytes) : a;
    }
}

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60 && term > 1e-14 * sum; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

// A stream of stereo frames from rateIn to 44.1 kHz, pushed in pieces: each output frame is the
// 32 input frames around its time (15 before the frame at or before it, 16 after), weighted by a
// windowed sinc. The 256 phases of the kernel are worked out once; between two phases the taps are
// interpolated, so a time falling between them is exact to ~1e-5. Only the window of input the next
// output needs is kept.
class Resampler {
public:
    static constexpr int kTaps = 32, kBefore = 15, kPhases = 256;
    static constexpr double kBeta = 7.0;   // the Kaiser window's shape
    explicit Resampler(uint32_t rateIn) : step_(static_cast<double>(rateIn) / kOutRate), table_((kPhases + 1) * kTaps) {
        // The passband to 0.45 of the lower rate, in cycles per input sample.
        const double fc = 0.45 * std::min<double>(rateIn, kOutRate) / rateIn;
        const double pi = 3.14159265358979323846, i0Beta = besselI0(kBeta);
        for (int p = 0; p <= kPhases; ++p) {
            double sum = 0.0;
            float* h = &table_[static_cast<size_t>(p) * kTaps];
            std::vector<double> t(kTaps);
            for (int k = 0; k < kTaps; ++k) {
                const double x = (k - kBefore) - static_cast<double>(p) / kPhases;   // the tap's distance from the output time
                const double u = x / (kTaps / 2);
                const double w = std::fabs(u) >= 1.0 ? 0.0 : besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0Beta;
                const double y = 2.0 * fc * x;
                t[static_cast<size_t>(k)] = (std::fabs(y) < 1e-12 ? 1.0 : std::sin(pi * y) / (pi * y)) * 2.0 * fc * w;
                sum += t[static_cast<size_t>(k)];
            }
            for (int k = 0; k < kTaps; ++k) h[k] = static_cast<float>(t[static_cast<size_t>(k)] / sum);
        }
    }
    static uint64_t outFrames(uint64_t in, uint32_t rateIn) {
        return static_cast<uint64_t>(std::floor(static_cast<double>(in) * kOutRate / rateIn + 0.5));
    }
    // `n` input frames (interleaved L R); the output they complete is appended.
    void push(const float* lr, size_t n, std::vector<float>& l, std::vector<float>& r) {
        win_.insert(win_.end(), lr, lr + 2 * n);
        total_ += n;
        run(false, l, r);
    }
    // The end of the input: the rest of the output, the input past its end silence.
    void finish(std::vector<float>& l, std::vector<float>& r) {
        last_ = static_cast<uint64_t>(std::floor(static_cast<double>(total_) / step_ + 0.5));
        run(true, l, r);
    }

private:
    void run(bool end, std::vector<float>& l, std::vector<float>& r) {
        for (;; ++j_) {
            if (end && j_ >= last_) break;
            const double t = static_cast<double>(j_) * step_;
            const uint64_t i0 = static_cast<uint64_t>(t);
            if (!end && i0 + (kTaps - kBefore) >= total_) break;   // the taps after it aren't in yet
            const double ph = (t - static_cast<double>(i0)) * kPhases;
            const int p = std::min(static_cast<int>(ph), kPhases - 1);
            const float a = static_cast<float>(ph - p);
            const float* h0 = &table_[static_cast<size_t>(p) * kTaps];
            const float* h1 = h0 + kTaps;
            float accL = 0.0f, accR = 0.0f;
            for (int k = 0; k < kTaps; ++k) {
                const int64_t at = static_cast<int64_t>(i0) - kBefore + k;   // the input frame this tap reads
                if (at < 0 || static_cast<uint64_t>(at) >= total_) continue;   // before the start, past the end: silence
                const float w = h0[k] + a * (h1[k] - h0[k]);
                const float* s = &win_[2 * (static_cast<uint64_t>(at) - base_)];
                accL += w * s[0];
                accR += w * s[1];
            }
            l.push_back(accL);
            r.push_back(accR);
        }
        if (end) return;
        // What the next output needs starts 15 frames before its own: the rest can go.
        const uint64_t next = static_cast<uint64_t>(static_cast<double>(j_) * step_);
        const uint64_t keep = std::min<uint64_t>(next > kBefore ? next - kBefore : 0, total_);
        if (keep > base_) {
            win_.erase(win_.begin(), win_.begin() + static_cast<std::ptrdiff_t>(2 * (keep - base_)));
            base_ = keep;
        }
    }

    double step_;                 // input frames per output frame
    std::vector<float> table_;    // kPhases + 1 phases of kTaps taps
    std::vector<float> win_;      // input frames base_ ..., interleaved
    uint64_t base_ = 0, total_ = 0, j_ = 0, last_ = 0;
};

} // namespace

bool readWav(const std::string& path, WavData& out, float maxSeconds, std::string* err) {
    if (!(maxSeconds > 0.0f) || !std::isfinite(maxSeconds)) return fail(err, "no length to read");
    try {
        // Opened without blocking, looked at, and only then read: a FIFO named *.wav would otherwise
        // hold the loader thread until someone wrote to it.
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return fail(err, "cannot open");
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return fail(err, "not a file");
        }
        File file(::fdopen(fd, "rb"));
        if (!file.f) {
            ::close(fd);
            return fail(err, "cannot open");
        }
        const uint64_t size = static_cast<uint64_t>(st.st_size);

        Format fmt;
        uint64_t dataAt = 0, dataBytes = 0;
        if (!walk(file.f, size, fmt, dataAt, dataBytes, err) || !checkFormat(fmt, err)) return false;

        // The frames to keep: the data's, at most maxSeconds of them.
        const uint64_t have = dataBytes / fmt.align;
        const uint64_t keep = static_cast<uint64_t>(std::min(static_cast<double>(have), std::floor(static_cast<double>(maxSeconds) * fmt.rate)));
        const bool resample = fmt.rate != static_cast<uint32_t>(kOutRate);
        const uint64_t frames = resample ? Resampler::outFrames(keep, fmt.rate) : keep;
        if (frames == 0) return fail(err, "no audio");

        WavData wav;
        wav.rate = kOutRate;
        wav.channels = fmt.channels;
        wav.fileRate = static_cast<int>(fmt.rate);
        wav.l.reserve(static_cast<size_t>(frames) + 1);
        wav.r.reserve(static_cast<size_t>(frames) + 1);
        std::unique_ptr<Resampler> rs(resample ? new Resampler(fmt.rate) : nullptr);
        std::vector<uint8_t> raw(kReadFrames * fmt.align);
        std::vector<float> lr(2 * kReadFrames);
        if (fseeko(file.f, static_cast<off_t>(dataAt), SEEK_SET) != 0) return fail(err, "cannot read");
        for (uint64_t done = 0; done < keep;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kReadFrames, keep - done));
            if (std::fread(raw.data(), 1, n * fmt.align, file.f) != n * fmt.align) return fail(err, "cannot read");
            decode(fmt, raw.data(), n, lr.data());
            if (rs) {
                rs->push(lr.data(), n, wav.l, wav.r);
            } else {
                for (size_t i = 0; i < n; ++i) {
                    wav.l.push_back(lr[2 * i]);
                    wav.r.push_back(lr[2 * i + 1]);
                }
            }
            done += n;
        }
        if (rs) rs->finish(wav.l, wav.r);
        out = std::move(wav);
        return true;
    } catch (...) {   // out of memory: a failed load, not the host's end
        return fail(err, "out of memory");
    }
}

bool writeWav(const std::string& path, const int16_t* interleaved, int frames, std::string* err) {
    constexpr uint32_t kHeader = 44;
    if (!interleaved || frames < 1 || static_cast<uint64_t>(frames) * 4 > 0xFFFFFFFFu - kHeader)
        return fail(err, "nothing to write");
    try {
        const uint32_t data = static_cast<uint32_t>(frames) * 4;
        std::string bytes(kHeader + data, '\0');
        uint8_t* b = reinterpret_cast<uint8_t*>(&bytes[0]);
        std::memcpy(b, "RIFF", 4);
        wr32(b + 4, kHeader - 8 + data);
        std::memcpy(b + 8, "WAVEfmt ", 8);
        wr32(b + 16, 16);
        wr16(b + 20, 1);                       // PCM
        wr16(b + 22, 2);
        wr32(b + 24, kOutRate);
        wr32(b + 28, kOutRate * 4);
        wr16(b + 32, 4);
        wr16(b + 34, 16);
        std::memcpy(b + 36, "data", 4);
        wr32(b + 40, data);
        for (size_t i = 0; i < 2 * static_cast<size_t>(frames); ++i) wr16(b + kHeader + 2 * i, static_cast<uint16_t>(interleaved[i]));
        if (!writeFileAtomic(path, bytes, true)) return fail(err, "cannot write");
        return true;
    } catch (...) {
        return fail(err, "out of memory");
    }
}

} // namespace af
