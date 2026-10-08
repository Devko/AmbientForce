// Weather's sources on the plugin side: WAVs in and out (plugin/wav.h), the loader and its cache
// (plugin/loader.h), the keys, the library and Keep (plugin/sources.h). Needs no host: make
// test-module M=sources builds it with the plugin's loader, wav, sources and paths.
//
// What a source sounds like isn't looked at here (the grains' own suite does that): the fields and
// Memory are used through their interface only, and a stand-in with the same interface passes as well
// as dsp/fields.cpp and dsp/memory.cpp.
//
// The loader's threading is checked as the other suites check theirs: deterministic tests of its state
// machine (a load held on a gate, `rounds()` to know the worker has made another pass, the audio
// thread played by the test), and a stress run in which a thread plays the audio thread, keeping the
// pointer between blocks as Weather does, against a thread swapping sources as fast as it can. Under
// ASan a read of a freed source is an error, which is what the stress run is for.
#include "check.h"
#include "signal.h"
#include "../dsp/fields.h"
#include "../dsp/grainsrc.h"
#include "../dsp/memory.h"
#include "../plugin/loader.h"
#include "../plugin/paths.h"
#include "../plugin/sources.h"
#include "../plugin/wav.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
// The sanitizer runtime's (sanitizer/allocator_interface.h, which GCC doesn't install): hooks its
// allocator calls on every allocation and free.
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*malloc_hook)(const volatile void*, size_t),
                                                         void (*free_hook)(const volatile void*));
#define SOURCES_COUNTS_ALLOCS 1
#endif

namespace aft {
namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
using af::FD_COUNT;
using af::GrainSource;
using af::Loader;
using af::Memory;
using af::Root;
using af::SourceBuffer;
using af::WavData;
using af::defaultSourceKey;
using af::kFieldNames;
using af::kMemoryKey;
using af::keepMemory;
using af::loadSource;
using af::readWav;
using af::rescanSources;
using af::resolveKey;
using af::setKeepHook;
using af::sourceKeys;
using af::sourceName;
using af::sourceOf;
using af::sourceRoots;
using af::sourceSlotType;
using af::writeWav;

// --- the run's folder and small helpers ---------------------------------------------------------------

const std::string& dir() {
    static const std::string d = [] {
        char tmpl[] = "/tmp/aftest.sources.XXXXXX";
        const char* made = mkdtemp(tmpl);
        return std::string(made ? made : "/tmp/aftest.sources");
    }();
    return d;
}

#if SOURCES_COUNTS_ALLOCS
constexpr bool kCountsAllocs = true;
#else
constexpr bool kCountsAllocs = false;
#endif
// The waits are for the worker to get something done; the same work under qemu (no ASan, test-arm) takes
// a good deal longer than on x86, so a wait that gives up gives up later there.
constexpr int kSlow = kCountsAllocs ? 1 : 10;

template <class F>
bool until(F f, int ms = 5000) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms * kSlow);
    while (!f()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

bool writeBytes(const std::string& path, const Bytes& b) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

#if SOURCES_COUNTS_ALLOCS
// ASan calls these on every allocation; only the test's own thread, while tracking, is looked at.
thread_local bool t_tracking = false;
size_t g_biggest = 0;
void onMalloc(const volatile void*, size_t size) {
    if (t_tracking) g_biggest = std::max(g_biggest, size);
}
void onFree(const volatile void*) {}
#endif
struct Track {   // the largest single allocation made while this lives (0 without ASan)
    Track() {
#if SOURCES_COUNTS_ALLOCS
        static const bool hooked = __sanitizer_install_malloc_and_free_hooks(onMalloc, onFree) != 0;
        (void)hooked;
        g_biggest = 0;
        t_tracking = true;
#endif
    }
    ~Track() {
#if SOURCES_COUNTS_ALLOCS
        t_tracking = false;
#endif
    }
    size_t biggest() const {
#if SOURCES_COUNTS_ALLOCS
        return g_biggest;
#else
        return 0;
#endif
    }
};

// --- WAV bytes ----------------------------------------------------------------------------------------

void put16(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(Bytes& b, uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}
void putTag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

// A chunk; `declared` (if not -1) is the size it claims, whatever the body holds.
Bytes chunk(const char* id, const Bytes& body, int64_t declared = -1) {
    Bytes b;
    putTag(b, id);
    put32(b, declared < 0 ? static_cast<uint32_t>(body.size()) : static_cast<uint32_t>(declared));
    b.insert(b.end(), body.begin(), body.end());
    if (body.size() & 1) b.push_back(0);
    return b;
}

Bytes fmtBody(int tag, int channels, int rate, int bits, int align = -1) {
    Bytes b;
    const uint32_t a = align >= 0 ? static_cast<uint32_t>(align) : static_cast<uint32_t>(channels * bits / 8);
    put16(b, static_cast<uint32_t>(tag));
    put16(b, static_cast<uint32_t>(channels));
    put32(b, static_cast<uint32_t>(rate));
    put32(b, static_cast<uint32_t>(rate) * a);
    put16(b, a);
    put16(b, static_cast<uint32_t>(bits));
    return b;
}

// WAVE_FORMAT_EXTENSIBLE: the tag moves into the sub-format's first two bytes.
Bytes fmtExtensible(int subTag, int channels, int rate, int bits) {
    Bytes b = fmtBody(0xFFFE, channels, rate, bits);
    put16(b, 22);
    put16(b, static_cast<uint32_t>(bits));
    put32(b, 3);
    put16(b, static_cast<uint32_t>(subTag));
    const uint8_t guard[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    b.insert(b.end(), guard, guard + 14);
    return b;
}

Bytes riff(const std::vector<Bytes>& chunks) {
    Bytes body;
    putTag(body, "WAVE");
    for (const Bytes& c : chunks) body.insert(body.end(), c.begin(), c.end());
    Bytes b;
    putTag(b, "RIFF");
    put32(b, static_cast<uint32_t>(body.size()));
    b.insert(b.end(), body.begin(), body.end());
    return b;
}

// One sample of a format, quantised, appended as the file holds it; the value read back is returned.
float encode(Bytes& b, int tag, int bits, float v) {
    if (tag == 3) {
        uint8_t raw[4];
        std::memcpy(raw, &v, 4);
        b.insert(b.end(), raw, raw + 4);
        return v;
    }
    if (bits == 16) {
        const int q = static_cast<int>(std::lround(v * 32767.0f));
        put16(b, static_cast<uint32_t>(q));
        return static_cast<float>(q) / 32768.0f;
    }
    if (bits == 24) {
        const int q = static_cast<int>(std::lround(v * 8388607.0f));
        b.push_back(static_cast<uint8_t>(q));
        b.push_back(static_cast<uint8_t>(q >> 8));
        b.push_back(static_cast<uint8_t>(q >> 16));
        return static_cast<float>(q) / 8388608.0f;
    }
    const int32_t q = static_cast<int32_t>(std::llround(static_cast<double>(v) * 2147483647.0));
    put32(b, static_cast<uint32_t>(q));
    return static_cast<float>(q) / 2147483648.0f;
}

float signalAt(int i, int c) { return 0.9f * std::sin(0.05f * static_cast<float>(i) + static_cast<float>(c)); }

// A file of n frames of a format and channel count, and what readWav should give for channels 0 and 1.
struct Made {
    Bytes file;
    std::vector<float> l, r;
};
Made makeFormat(int tag, int bits, int channels, int rate, int n, bool extensible = false) {
    Made m;
    Bytes data;
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) {
            const float v = c < 2 ? signalAt(i, c) : 0.5f;   // channels past the second are not heard
            const float back = encode(data, tag, bits, v);
            if (c == 0) m.l.push_back(back);
            if (c == 1 || (c == 0 && channels == 1)) m.r.push_back(back);
        }
    if (channels == 1) m.r = m.l;
    m.file = riff({chunk("fmt ", extensible ? fmtExtensible(tag, channels, rate, bits) : fmtBody(tag, channels, rate, bits)),
                   chunk("data", data)});
    return m;
}

// A mono float sine at any rate.
Bytes sineFile(int rate, double hz, double seconds, double amp = 0.5) {
    Bytes data;
    const int n = static_cast<int>(seconds * rate);
    for (int i = 0; i < n; ++i) encode(data, 3, 32, static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / rate)));
    return riff({chunk("fmt ", fmtBody(3, 1, rate, 32)), chunk("data", data)});
}

// Rising zero crossings, interpolated: the frequency of a clean tone.
double crossingHz(const std::vector<float>& x, size_t from, size_t to) {
    double first = -1.0, last = 0.0;
    int n = 0;
    for (size_t i = from + 1; i < to && i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++n;
            }
        }
    return n > 0 ? 44100.0 * n / (last - first) : 0.0;
}

void makeWav(const std::string& path, int frames, double hz = 440.0) {
    std::vector<int16_t> pcm(2 * static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
        pcm[2 * static_cast<size_t>(i)] = pcm[2 * static_cast<size_t>(i) + 1] =
            static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * kPi * hz * i / 44100.0)));
    fs::create_directories(fs::path(path).parent_path());
    std::string err;
    writeWav(path, pcm.data(), frames, &err);
}

// --- WAV: write and read ------------------------------------------------------------------------------

void testRoundTrip() {
    std::printf("== sources: WAV round trip\n");
    const std::string folder = dir() + "/wav";
    fs::create_directories(folder);
    const int n = 3000;
    std::vector<int16_t> pcm(2 * static_cast<size_t>(n));
    uint32_t s = 12345;
    for (int16_t& v : pcm) {
        s = s * 1664525u + 1013904223u;
        v = static_cast<int16_t>(s >> 16);
    }
    pcm[0] = -32768;
    pcm[1] = 32767;
    std::string err;
    const std::string path = folder + "/rt.wav";
    CHECK(writeWav(path, pcm.data(), n, &err));
    WavData w;
    CHECK(readWav(path, w, 60.0f, &err));
    CHECK(w.rate == 44100 && w.fileRate == 44100 && w.channels == 2);
    CHECK(static_cast<int>(w.l.size()) == n && w.r.size() == w.l.size());
    bool same = static_cast<int>(w.l.size()) == n && static_cast<int>(w.r.size()) == n;
    for (int i = 0; same && i < n; ++i)
        same = w.l[static_cast<size_t>(i)] == static_cast<float>(pcm[2 * static_cast<size_t>(i)]) / 32768.0f &&
               w.r[static_cast<size_t>(i)] == static_cast<float>(pcm[2 * static_cast<size_t>(i) + 1]) / 32768.0f;
    CHECK(same);
    // A header of 44 bytes and the samples, and nothing else left in the folder (no temp file).
    CHECK(fs::file_size(path) == 44u + 4u * static_cast<unsigned>(n));
    int files = 0;
    for (const auto& e : fs::directory_iterator(folder)) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
    // Writing again over the file replaces it.
    CHECK(writeWav(path, pcm.data(), 10, &err));
    CHECK(readWav(path, w, 60.0f, &err) && w.l.size() == 10);

    // Nothing to write, or nowhere to write it: false with a reason and nothing left behind.
    CHECK(!writeWav(folder + "/none.wav", pcm.data(), 0, &err) && !err.empty());
    CHECK(!writeWav(folder + "/none.wav", nullptr, 10, &err));
    err.clear();
    CHECK(!writeWav(folder + "/no such folder/x.wav", pcm.data(), 10, &err) && !err.empty());
    CHECK(!fs::exists(folder + "/no such folder"));
    files = 0;
    for (const auto& e : fs::directory_iterator(folder)) {
        (void)e;
        ++files;
    }
    CHECK(files == 1);
}

// A file rewritten while it is read is whole in either state, never half: the writer renames a
// finished file into place.
void testAtomicReplace() {
    std::printf("== sources: replacing a file under a reader\n");
    const std::string path = dir() + "/wav/atomic.wav";
    std::vector<int16_t> a(2 * 20000, 1000), b(2 * 60000, -1000);
    std::string err;
    CHECK(writeWav(path, a.data(), 20000, &err));
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int i = 0; i < 80; ++i) {
            const std::vector<int16_t>& v = i % 2 ? a : b;
            writeWav(path, v.data(), static_cast<int>(v.size() / 2), nullptr);
        }
        stop = true;
    });
    int reads = 0, bad = 0;
    while (!stop) {
        WavData w;
        std::string e;
        if (!readWav(path, w, 60.0f, &e) || (w.l.size() != 20000 && w.l.size() != 60000)) ++bad;
        ++reads;
    }
    writer.join();
    std::printf("  %d reads, %d of them not whole\n", reads, bad);
    CHECK(reads > 0 && bad == 0);
}

void testFormats() {
    std::printf("== sources: formats\n");
    const std::string path = dir() + "/wav/format.wav";
    struct Case {
        int tag, bits, channels;
        bool ext;
    };
    const Case cases[] = {{1, 16, 1, false}, {1, 16, 2, false}, {1, 24, 2, false}, {1, 32, 2, false}, {3, 32, 2, false},
                          {1, 24, 1, false}, {3, 32, 1, false}, {1, 16, 6, false}, {3, 32, 8, false}, {1, 24, 2, true},
                          {3, 32, 2, true},  {1, 16, 1, true}};
    for (const Case& c : cases) {
        const Made m = makeFormat(c.tag, c.bits, c.channels, 44100, 700, c.ext);
        CHECK(writeBytes(path, m.file));
        WavData w;
        std::string err;
        const bool ok = readWav(path, w, 60.0f, &err);
        CHECK(ok);
        if (!ok) {
            std::printf("  tag %d, %d bit, %d ch%s: %s\n", c.tag, c.bits, c.channels, c.ext ? " (extensible)" : "", err.c_str());
            continue;
        }
        CHECK(w.channels == c.channels && w.rate == 44100);
        CHECK(w.l == m.l);   // exactly what the format holds
        CHECK(w.r == m.r);
    }

    // The unsupported: 8-bit, 12-bit, 64-bit float, ADPCM, A-law; no channels, nine channels; rates
    // outside 1000..384000; a block align that doesn't fit the frame.
    struct Bad {
        const char* what;
        Bytes fmt;
    };
    const Bad bad[] = {{"8-bit", fmtBody(1, 2, 44100, 8)},
                       {"12-bit", fmtBody(1, 2, 44100, 12, 3)},
                       {"64-bit float", fmtBody(3, 2, 44100, 64)},
                       {"16-bit float", fmtBody(3, 2, 44100, 16)},
                       {"ADPCM", fmtBody(2, 2, 44100, 4)},
                       {"A-law", fmtBody(6, 2, 44100, 8)},
                       {"0 channels", fmtBody(1, 0, 44100, 16, 4)},
                       {"9 channels", fmtBody(1, 9, 44100, 16)},
                       {"rate 0", fmtBody(1, 2, 0, 16)},
                       {"rate 999", fmtBody(1, 2, 999, 16)},
                       {"rate 384001", fmtBody(1, 2, 384001, 16)},
                       {"rate 4 billion", fmtBody(1, 2, static_cast<int>(0xFFFFFFFFu), 16)},
                       {"align 0", fmtBody(1, 2, 44100, 16, 0)},
                       {"align under the frame", fmtBody(1, 2, 44100, 16, 3)},
                       {"align 65535", fmtBody(1, 2, 44100, 16, 65535)}};
    Bytes data(300000, 0x11);   // enough frames for any block align below to be read, were it let through
    for (const Bad& b : bad) {
        CHECK(writeBytes(path, riff({chunk("fmt ", b.fmt), chunk("data", data)})));
        WavData w;
        std::string err;
        const bool ok = readWav(path, w, 60.0f, &err);
        CHECK(!ok);
        CHECK(!ok && !err.empty());
        if (ok) std::printf("  accepted: %s\n", b.what);
    }
    // 8-channel at the largest block align is fine; a block align over the frame's samples is
    // accepted (padding after the channels), the samples read from the start of each frame.
    {
        Bytes d;
        for (int i = 0; i < 100; ++i) {
            encode(d, 1, 16, 0.25f);
            encode(d, 1, 16, -0.25f);
            put32(d, 0x7FFF7FFF);   // padding
        }
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(1, 2, 44100, 16, 8)), chunk("data", d)})));
        WavData w;
        CHECK(readWav(path, w, 60.0f, nullptr) && w.l.size() == 100 && w.l[50] == 0.25f && w.r[50] == -0.25f);
    }
}

void testChunks() {
    std::printf("== sources: chunks\n");
    const std::string path = dir() + "/wav/chunks.wav";
    const Made m = makeFormat(1, 16, 2, 44100, 500);
    Bytes data;
    for (int i = 0; i < 500; ++i)
        for (int c = 0; c < 2; ++c) encode(data, 1, 16, signalAt(i, c));
    const Bytes fmt = chunk("fmt ", fmtBody(1, 2, 44100, 16));
    WavData w;
    std::string err;

    // Other chunks around them, one of an odd size (padded to even), a data chunk before fmt.
    CHECK(writeBytes(path, riff({chunk("LIST", Bytes(13, 'x')), fmt, chunk("fact", Bytes(4, 0)), chunk("junk", Bytes(7, 1)),
                                 chunk("data", data), chunk("LIST", Bytes(9, 'y'))})));
    CHECK(readWav(path, w, 60.0f, &err) && w.l == m.l && w.r == m.r);
    CHECK(writeBytes(path, riff({chunk("data", data), fmt})));
    CHECK(readWav(path, w, 60.0f, &err) && w.l == m.l);
    // A fmt chunk longer than 16 bytes (a cbSize and more).
    Bytes longFmt = fmtBody(1, 2, 44100, 16);
    longFmt.resize(40, 0);
    CHECK(writeBytes(path, riff({chunk("fmt ", longFmt), chunk("data", data)})));
    CHECK(readWav(path, w, 60.0f, &err) && w.l == m.l);
    // The RIFF size is not looked at (a streamed or edited file gets it wrong).
    Bytes f = riff({fmt, chunk("data", data)});
    f[4] = f[5] = f[6] = f[7] = 0;
    CHECK(writeBytes(path, f));
    CHECK(readWav(path, w, 60.0f, &err) && w.l == m.l);
    f[4] = f[5] = f[6] = f[7] = 0xFF;
    CHECK(writeBytes(path, f));
    CHECK(readWav(path, w, 60.0f, &err) && w.l == m.l);
    // A last partial frame is dropped.
    Bytes odd = data;
    odd.resize(odd.size() - 3);
    CHECK(writeBytes(path, riff({fmt, chunk("data", odd)})));
    CHECK(readWav(path, w, 60.0f, &err) && w.l.size() == 499);

    // No fmt, no data, RIFF without WAVE, an empty data chunk: failures with a reason.
    CHECK(writeBytes(path, riff({chunk("data", data)})));
    CHECK(!readWav(path, w, 60.0f, &err) && err.find("fmt") != std::string::npos);
    CHECK(writeBytes(path, riff({fmt})));
    CHECK(!readWav(path, w, 60.0f, &err) && err.find("data") != std::string::npos);
    CHECK(writeBytes(path, riff({fmt, chunk("data", Bytes())})) && !readWav(path, w, 60.0f, &err));
    CHECK(writeBytes(path, riff({fmt, chunk("data", Bytes(3, 0))})) && !readWav(path, w, 60.0f, &err));   // under one frame
    Bytes notWave = riff({fmt, chunk("data", data)});
    notWave[8] = 'X';
    CHECK(writeBytes(path, notWave) && !readWav(path, w, 60.0f, &err) && err == "not a WAV file");
    Bytes rf64 = riff({fmt, chunk("data", data)});
    rf64[0] = 'R', rf64[1] = 'F', rf64[2] = '6', rf64[3] = '4';
    CHECK(writeBytes(path, rf64) && !readWav(path, w, 60.0f, &err));
    // A bad fmt: under 16 bytes.
    CHECK(writeBytes(path, riff({chunk("fmt ", Bytes(14, 0)), chunk("data", data)})) && !readWav(path, w, 60.0f, &err));
    // Extensible with the tag cut off.
    Bytes ext = fmtExtensible(1, 2, 44100, 16);
    ext.resize(24);
    CHECK(writeBytes(path, riff({chunk("fmt ", ext), chunk("data", data)})) && !readWav(path, w, 60.0f, &err));

    // Not NaN, not infinite: a float file's bad samples are 0, its huge ones held.
    {
        Bytes d;
        const float vals[] = {0.5f, std::nanf(""), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), 3e38f,
                              -3e38f, 1.0f, 0.25f};
        for (float v : vals) encode(d, 3, 32, v);
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(3, 1, 44100, 32)), chunk("data", d)})));
        CHECK(readWav(path, w, 60.0f, &err) && w.l.size() == 8);
        CHECK(allFinite(w.l) && w.l[0] == 0.5f && w.l[1] == 0.0f && w.l[2] == 0.0f && w.l[3] == 0.0f);
        CHECK(w.l[4] <= 1.0e6f && w.l[4] > 0.0f && w.l[5] >= -1.0e6f && w.l[5] < 0.0f && w.l[7] == 0.25f);
    }
}

void testBadFiles() {
    std::printf("== sources: bad files\n");
    const std::string path = dir() + "/wav/bad.wav";
    const Made m = makeFormat(1, 16, 2, 44100, 800);
    WavData w;
    std::string err;

    // Truncated: every cut of the file inside its header or before one frame of audio fails (the walk
    // cannot find what it needs, or the data chunk claims more than the file holds).
    int failedCuts = 0, cuts = 0;
    for (size_t cut = 0; cut < m.file.size(); cut += (cut < 64 ? 1 : 97)) {
        const Bytes part(m.file.begin(), m.file.begin() + static_cast<std::ptrdiff_t>(cut));
        CHECK(writeBytes(path, part));
        ++cuts;
        err.clear();
        if (!readWav(path, w, 60.0f, &err)) {
            ++failedCuts;
            CHECK(!err.empty());
        }
    }
    CHECK(failedCuts == cuts);
    CHECK(writeBytes(path, m.file) && readWav(path, w, 60.0f, &err));   // the whole of it is fine
    {
        const Bytes cut(m.file.begin(), m.file.begin() + 44 + 4 * 400);   // half of the audio gone
        CHECK(writeBytes(path, cut) && !readWav(path, w, 60.0f, &err) && err == "truncated");
    }

    // The sizes a writer leaves when it doesn't know the length: the data runs to the end.
    for (const uint32_t placeholder : {0u, 0xFFFFFFFFu}) {
        Bytes f = m.file;
        f[40] = static_cast<uint8_t>(placeholder);
        f[41] = static_cast<uint8_t>(placeholder >> 8);
        f[42] = static_cast<uint8_t>(placeholder >> 16);
        f[43] = static_cast<uint8_t>(placeholder >> 24);
        CHECK(writeBytes(path, f) && readWav(path, w, 60.0f, &err) && w.l == m.l);
    }

    // Huge claims in a small file: no crash, no allocation to match, a refusal or what is there.
    {
        Track t;
        for (const uint32_t claim : {0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0x10000000u}) {
            Bytes f = m.file;
            f[40] = static_cast<uint8_t>(claim);
            f[41] = static_cast<uint8_t>(claim >> 8);
            f[42] = static_cast<uint8_t>(claim >> 16);
            f[43] = static_cast<uint8_t>(claim >> 24);
            CHECK(writeBytes(path, f) && !readWav(path, w, 60.0f, &err));
            // fmt claiming to be enormous
            Bytes g = m.file;
            g[16] = static_cast<uint8_t>(claim);
            g[17] = static_cast<uint8_t>(claim >> 8);
            g[18] = static_cast<uint8_t>(claim >> 16);
            g[19] = static_cast<uint8_t>(claim >> 24);
            CHECK(writeBytes(path, g) && !readWav(path, w, 60.0f, &err));
        }
        if (kCountsAllocs) CHECK(t.biggest() < (1u << 20));   // the file is 3 KB
    }
    // Chunks that point past the end or back on themselves: the walk ends.
    {
        Bytes f = riff({chunk("junk", Bytes(10, 0)), chunk("junk", Bytes(10, 0))});
        f[16] = 0xFF, f[17] = 0xFF, f[18] = 0xFF, f[19] = 0x7F;
        CHECK(writeBytes(path, f) && !readWav(path, w, 60.0f, &err));
        std::vector<Bytes> many(2000, chunk("junk", Bytes(2, 0)));
        many.push_back(chunk("fmt ", fmtBody(1, 2, 44100, 16)));
        many.push_back(chunk("data", Bytes(400, 0)));
        CHECK(writeBytes(path, riff(many)) && !readWav(path, w, 60.0f, &err));   // past 256 chunk headers
    }

    // Not files that are WAVs: text, nothing, a folder, a file that isn't there, a FIFO (which would
    // block a read for ever), and a limit that makes no sense.
    CHECK(writeBytes(path, Bytes(1000, 'a')) && !readWav(path, w, 60.0f, &err) && err == "not a WAV file");
    CHECK(writeBytes(path, Bytes()) && !readWav(path, w, 60.0f, &err));
    CHECK(!readWav(dir() + "/wav", w, 60.0f, &err) && !err.empty());
    CHECK(!readWav(dir() + "/wav/absent.wav", w, 60.0f, &err) && err == "cannot open");
    const std::string fifo = dir() + "/wav/pipe.wav";
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(!readWav(fifo, w, 60.0f, &err) && err == "not a file");
    CHECK(writeBytes(path, m.file));
    CHECK(!readWav(path, w, 0.0f, &err) && !readWav(path, w, -1.0f, &err) && !readWav(path, w, std::nanf(""), &err));
    CHECK(!readWav(path, w, 1e-9f, &err));   // under one frame

    // A failure leaves `out` as it was.
    w = WavData{};
    w.l = {1.0f};
    w.rate = 7;
    CHECK(!readWav(dir() + "/wav/absent.wav", w, 60.0f, &err) && w.rate == 7 && w.l.size() == 1);
}

// A small fuzz: the header's every byte set to a few values, every cut, and random changes and
// appendages, under ASan/UBSan. Whatever comes back, a WAV that reads is bounded and finite.
void testFuzz() {
    std::printf("== sources: header fuzz\n");
    const std::string path = dir() + "/wav/fuzz.wav";
    const Made base = makeFormat(1, 16, 2, 44100, 600);
    Track track;
    WavData w;
    std::string err;
    int tried = 0, accepted = 0;
    const auto probe = [&](const Bytes& f) {
        CHECK(writeBytes(path, f));
        ++tried;
        err.clear();
        if (readWav(path, w, 60.0f, &err)) {
            ++accepted;
            const bool ok = w.rate == 44100 && !w.l.empty() && w.l.size() == w.r.size() && w.l.size() <= 60u * 44100u + 1 &&
                            allFinite(w.l) && allFinite(w.r);
            CHECK(ok);
        } else {
            CHECK(!err.empty());
        }
    };
    for (size_t at = 0; at < 64 && at < base.file.size(); ++at)
        for (const uint8_t v : {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF}) {
            Bytes f = base.file;
            f[at] = v;
            probe(f);
        }
    uint32_t s = 2024;
    const auto rnd = [&s](uint32_t n) {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) % n;
    };
    for (int i = 0; i < 1500; ++i) {
        Bytes f = base.file;
        const int edits = 1 + static_cast<int>(rnd(4));
        for (int e = 0; e < edits; ++e) f[rnd(static_cast<uint32_t>(rnd(3) == 0 ? f.size() : 64))] = static_cast<uint8_t>(rnd(256));
        if (rnd(4) == 0) f.resize(rnd(static_cast<uint32_t>(f.size())) + 1);
        if (rnd(4) == 0)
            for (uint32_t k = rnd(40); k > 0; --k) f.push_back(static_cast<uint8_t>(rnd(256)));
        probe(f);
    }
    std::printf("  %d files, %d read\n", tried, accepted);
    CHECK(accepted > 0 && accepted < tried);   // some survive their edit (a sample byte), most do not
    if (kCountsAllocs) CHECK(track.biggest() < (8u << 20));   // 2 KB files: a 1000 Hz one upsampled 44 times is 100 KB of floats
}

void testLimit() {
    std::printf("== sources: how much is read\n");
    const std::string path = dir() + "/wav/long.wav";
    WavData w;
    std::string err;
    {
        // 90 s of mono at 44.1 kHz.
        const int n = 90 * 44100;
        Bytes data;
        data.reserve(2 * static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) put16(data, static_cast<uint32_t>(static_cast<int16_t>(i & 0x7FFF)));
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(1, 1, 44100, 16)), chunk("data", data)})));
        {
            Track t;
            CHECK(readWav(path, w, 60.0f, &err));
            CHECK(w.l.size() == 60u * 44100u && w.r.size() == w.l.size());
            CHECK(w.l[1000] == static_cast<float>(1000) / 32768.0f);
            // Nothing the size of the whole file was made.
            if (kCountsAllocs) CHECK(t.biggest() <= 60u * 44100u * sizeof(float) + 64);
        }
        CHECK(readWav(path, w, 1.5f, &err) && w.l.size() == 66150);
        CHECK(readWav(path, w, 300.0f, &err) && w.l.size() == 90u * 44100u);
        // A mono file's right side is its left.
        CHECK(w.l == w.r);
    }
    {
        // 5 s at 48 kHz: 2 s of it, resampled.
        CHECK(writeBytes(path, sineFile(48000, 1000.0, 5.0)));
        CHECK(readWav(path, w, 2.0f, &err) && w.l.size() == 88200 && w.fileRate == 48000 && w.rate == 44100);
    }
    {
        // 90 s at 48 kHz, 60 s of it: the resampler holds the input it needs next, not the input so far.
        const int n = 90 * 48000;
        Bytes data;
        data.reserve(2 * static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) put16(data, static_cast<uint32_t>(static_cast<int16_t>(3000.0 * std::sin(0.1 * i))));
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(1, 1, 48000, 16)), chunk("data", data)})));
        Track t;
        CHECK(readWav(path, w, 60.0f, &err) && w.l.size() == 60u * 44100u && allFinite(w.l));
        if (kCountsAllocs) CHECK(t.biggest() <= 60u * 44100u * sizeof(float) + 64);
    }
}

// --- WAV: other rates ---------------------------------------------------------------------------------

void testResample() {
    std::printf("== sources: other rates\n");
    const std::string path = dir() + "/wav/rate.wav";
    WavData w;
    std::string err;
    for (const int rate : {8000, 11025, 22050, 32000, 48000, 88200, 96000, 176400, 192000}) {
        CHECK(writeBytes(path, sineFile(rate, 1000.0, 1.0)));
        const bool ok = readWav(path, w, 60.0f, &err);
        CHECK(ok);
        if (!ok) continue;
        const size_t expect = static_cast<size_t>(std::floor(static_cast<double>(rate) * 44100.0 / rate + 0.5));
        CHECK(w.rate == 44100 && w.fileRate == rate && w.channels == 1);
        CHECK(w.l.size() == expect && w.r == w.l);
        const double hz = crossingHz(w.l, 1000, w.l.size() - 1000);
        const double amp = rms(w.l, 2000, w.l.size() - 2000) * std::sqrt(2.0);
        std::printf("  %6d Hz: %zu frames, a 1 kHz tone at %.3f Hz, %.3f of its amplitude\n", rate, w.l.size(), hz, amp / 0.5);
        CHECK(std::fabs(hz - 1000.0) < 0.5);       // 1 kHz +- 0.5 Hz (the spec's)
        CHECK(std::fabs(db(amp / 0.5)) < 0.1);     // flat at 1 kHz
    }
    // Lengths come out in proportion to the rate for any frame count, not only whole seconds.
    for (const int frames : {1, 2, 7, 31, 100, 12345}) {
        Bytes data;
        for (int i = 0; i < frames; ++i) encode(data, 3, 32, 0.25f);
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(3, 1, 48000, 32)), chunk("data", data)})));
        const size_t expect = static_cast<size_t>(std::floor(frames * 44100.0 / 48000.0 + 0.5));
        const bool ok = readWav(path, w, 60.0f, &err);
        CHECK(expect == 0 ? !ok : (ok && w.l.size() == expect && allFinite(w.l)));
    }
    // In time and phase: a tone high up, where a fraction of a sample is a good part of a degree,
    // lands on the ideal one at 44.1 kHz (the kernel has no delay, and its phases are interpolated).
    {
        CHECK(writeBytes(path, sineFile(48000, 15000.0, 1.0)));
        CHECK(readWav(path, w, 60.0f, &err));
        double worst = 0.0;
        for (size_t j = 2000; j + 2000 < w.l.size(); ++j)
            worst = std::max(worst, std::fabs(static_cast<double>(w.l[j]) - 0.5 * std::sin(2.0 * kPi * 15000.0 * static_cast<double>(j) / 44100.0)));
        std::printf("  a 15 kHz tone from 48 kHz is %.1e off the ideal at the most\n", worst);
        CHECK(worst < 5e-4);
    }
    // A stereo file keeps its sides apart through the resampler.
    {
        Bytes data;
        const int n = 48000;
        for (int i = 0; i < n; ++i) {
            encode(data, 3, 32, static_cast<float>(0.5 * std::sin(2.0 * kPi * 1000.0 * i / 48000.0)));
            encode(data, 3, 32, static_cast<float>(0.25 * std::sin(2.0 * kPi * 3000.0 * i / 48000.0)));
        }
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(3, 2, 48000, 32)), chunk("data", data)})));
        CHECK(readWav(path, w, 60.0f, &err));
        CHECK(std::fabs(crossingHz(w.l, 1000, w.l.size() - 1000) - 1000.0) < 0.5);
        CHECK(std::fabs(crossingHz(w.r, 1000, w.r.size() - 1000) - 3000.0) < 0.5);
    }
    // The response of the 32 taps, 48 kHz down to 44.1: what is in the header, and its limits.
    std::printf("  48 kHz -> 44.1 kHz, gain (dB) at:");
    double g10 = 0.0;
    for (const double hz : {5000.0, 10000.0, 15000.0, 17000.0, 18000.0, 19845.0, 21000.0, 22000.0, 23000.0}) {
        CHECK(writeBytes(path, sineFile(48000, hz, 0.5)));
        CHECK(readWav(path, w, 60.0f, &err));
        const double g = db(rms(w.l, 2000, w.l.size() - 2000) * std::sqrt(2.0) / 0.5);
        std::printf(" %g kHz %.2f;", hz / 1000.0, g);
        if (hz == 10000.0) g10 = g;
        if (hz == 5000.0) CHECK(std::fabs(g) < 0.05);
    }
    std::printf("\n");
    CHECK(std::fabs(g10) < 0.1);
    // 96 kHz down: what lies over 22.05 kHz goes, not folded back under it.
    std::printf("  96 kHz -> 44.1 kHz, gain (dB) of tones that would fold to 12-17 kHz:");
    for (const double hz : {27000.0, 30000.0, 32000.0, 35000.0}) {
        CHECK(writeBytes(path, sineFile(96000, hz, 0.5)));
        CHECK(readWav(path, w, 60.0f, &err));
        const double g = db(rms(w.l, 2000, w.l.size() - 2000) * std::sqrt(2.0) / 0.5);
        std::printf(" %.0f kHz %.1f;", hz / 1000.0, g);
        CHECK(g < -40.0);
    }
    std::printf("\n");
    // DC passes at unity, whatever the rate.
    for (const int rate : {22050, 48000, 96000}) {
        Bytes data;
        for (int i = 0; i < rate / 4; ++i) encode(data, 3, 32, 0.5f);
        CHECK(writeBytes(path, riff({chunk("fmt ", fmtBody(3, 1, rate, 32)), chunk("data", data)})));
        CHECK(readWav(path, w, 60.0f, &err));
        const size_t mid = w.l.size() / 2;
        CHECK(std::fabs(w.l[mid] - 0.5f) < 1e-5f);
    }
}

// --- keys and the listing -----------------------------------------------------------------------------

void useRoots(const std::string& plugin, const std::string& ssd) {
    setenv("AF_SOURCE_ROOTS", (plugin + ":" + ssd).c_str(), 1);
    rescanSources();
}

void testKeys() {
    std::printf("== sources: keys\n");
    const std::string plug = dir() + "/keys/plugin", ssd = dir() + "/keys/ssd";
    fs::create_directories(plug);
    fs::create_directories(ssd);
    useRoots(plug, ssd);

    const std::vector<Root> roots = sourceRoots();
    CHECK(roots.size() == 2 && roots[0].label == "plugin" && roots[0].dir == plug && roots[1].label == "ssd" && roots[1].dir == ssd);
    CHECK(resolveKey("ssd:../x.wav", roots).empty());
    CHECK(resolveKey("ssd:AmbientForce/Weather/../../x.wav", roots).empty());
    CHECK(resolveKey("plugin:/etc/passwd", roots).empty());
    CHECK(resolveKey("plugin:Weather/Creek.wav", roots) == plug + "/Weather/Creek.wav");
    CHECK(resolveKey("ssd:AmbientForce/Memories/Memory 001.wav", roots) == ssd + "/AmbientForce/Memories/Memory 001.wav");
    CHECK(resolveKey("other:x.wav", roots).empty());

    // With nothing on the disk: the fields, then Memory.
    std::vector<std::string> keys = sourceKeys();
    CHECK(keys.size() == static_cast<size_t>(FD_COUNT) + 1);
    for (int i = 0; i < FD_COUNT; ++i) CHECK(keys[static_cast<size_t>(i)] == std::string("builtin:") + kFieldNames[i]);
    CHECK(keys.back() == kMemoryKey && kMemoryKey == std::string("memory:"));
    CHECK(defaultSourceKey() == "builtin:Rain on Roof" && keys.front() == defaultSourceKey());

    // The WAVs: by name, case folded; both folders of the SSD and the plugin's; a folder below; the
    // extension in any case. Not listed: other files, hidden ones, links, files outside the folders.
    makeWav(plug + "/Weather/Creek.wav", 500);
    makeWav(plug + "/Weather/zebra.wav", 500);
    makeWav(plug + "/Weather/Sub/Deep.wav", 500);
    makeWav(plug + "/Weather/.hidden.wav", 500);
    makeWav(plug + "/Weather/Sub/.hid/Under.wav", 500);
    makeWav(plug + "/Other/Elsewhere.wav", 500);
    makeWav(plug + "/Weather/Too/A/B/C/D/Deep.wav", 500);   // below the walk's depth
    makeWav(ssd + "/AmbientForce/Weather/Alpha.WAV", 500);
    makeWav(ssd + "/AmbientForce/Weather/beta.wav", 500);
    makeWav(ssd + "/AmbientForce/Memories/Memory 001.wav", 500);
    makeWav(ssd + "/AmbientForce/Memories/Same.wav", 500);   // two of one name: the key puts Memories first, the walk Weather
    makeWav(ssd + "/AmbientForce/Weather/Same.wav", 500);
    makeWav(ssd + "/Elsewhere.wav", 500);
    makeWav(ssd + "/AmbientForce/Elsewhere.wav", 500);
    { std::ofstream(plug + "/Weather/notes.txt") << "not audio"; }
    { std::ofstream(plug + "/Weather/Creek.wav.bak") << "not audio"; }
    std::error_code ec;
    fs::create_symlink(plug + "/Weather/Creek.wav", plug + "/Weather/link.wav", ec);
    fs::create_directory_symlink(plug + "/Weather/Sub", plug + "/Weather/SubLink", ec);
    CHECK(fs::is_symlink(plug + "/Weather/link.wav") && fs::is_symlink(plug + "/Weather/SubLink"));
    rescanSources();
    keys = sourceKeys();
    const std::vector<std::string> wavs(keys.begin() + FD_COUNT + 1, keys.end());
    const std::vector<std::string> expect = {"ssd:AmbientForce/Weather/Alpha.WAV",     "ssd:AmbientForce/Weather/beta.wav",
                                             "plugin:Weather/Creek.wav",               "plugin:Weather/Sub/Deep.wav",
                                             "ssd:AmbientForce/Memories/Memory 001.wav", "ssd:AmbientForce/Memories/Same.wav",
                                             "ssd:AmbientForce/Weather/Same.wav",      "plugin:Weather/zebra.wav"};
    CHECK(wavs == expect);
    if (wavs != expect)
        for (const std::string& k : wavs) std::printf("  listed: %s\n", k.c_str());
    for (size_t i = 0; i < keys.size(); ++i) CHECK(!keys[i].empty());

    // Names: the field, Memory, the file's stem.
    CHECK(sourceName("builtin:Surf") == "Surf" && sourceName("memory:") == "Memory");
    CHECK(sourceName("plugin:Weather/Creek.wav") == "Creek" && sourceName("plugin:Weather/Sub/Deep.wav") == "Deep");
    CHECK(sourceName("ssd:AmbientForce/Memories/Memory 003.wav") == "Memory 003");
    CHECK(sourceName("ssd:AmbientForce/Weather/gone.wav") == "gone");

    // The scan is looked at again after 2 s, not before: a file added shows within that, not at once.
    keys = sourceKeys();
    const size_t before = keys.size();
    makeWav(plug + "/Weather/Late.wav", 500);
    CHECK(sourceKeys().size() == before);
    CHECK(until([&] { return contains(sourceKeys(), "plugin:Weather/Late.wav"); }, 3500));
    // ... and rescanSources() makes it now.
    makeWav(plug + "/Weather/Later.wav", 500);
    rescanSources();
    CHECK(contains(sourceKeys(), "plugin:Weather/Later.wav"));
    // A file that goes is gone from the next scan.
    fs::remove(plug + "/Weather/Later.wav");
    rescanSources();
    CHECK(!contains(sourceKeys(), "plugin:Weather/Later.wav"));
    // Other roots are other keys at once (no rescan asked for).
    setenv("AF_SOURCE_ROOTS", (dir() + "/keys/none1:" + dir() + "/keys/none2").c_str(), 1);
    CHECK(sourceKeys().size() == static_cast<size_t>(FD_COUNT) + 1);
    // A root that is missing contributes nothing, a lone plugin root has no SSD.
    setenv("AF_SOURCE_ROOTS", plug.c_str(), 1);
    rescanSources();
    keys = sourceKeys();
    CHECK(sourceRoots().size() == 1 && contains(keys, "plugin:Weather/Creek.wav") && !contains(keys, "ssd:AmbientForce/Weather/beta.wav"));

    // Without the override: the plugin's own folder (what it is loaded from) and the SSD's root.
    unsetenv("AF_SOURCE_ROOTS");
    {
        const std::vector<Root> def = sourceRoots();
        CHECK(!def.empty() && def.back().label == "ssd" && def.back().dir == "/media/AkaiForce");
        if (!af::pluginDir().empty()) CHECK(def.size() == 2 && def[0].label == "plugin" && def[0].dir == af::pluginDir());
    }

    // Loading a key refuses what is not a source.
    useRoots(plug, ssd);
    std::string err;
    CHECK(!loadSource("ssd:../x.wav", &err) && !err.empty());
    CHECK(!loadSource("ssd:AmbientForce/Weather/../../x.wav", &err));
    CHECK(!loadSource("ssd:Elsewhere.wav", &err) && err == "no such source");    // a WAV, but not in a source folder
    CHECK(!loadSource("plugin:Other/Elsewhere.wav", &err) && err == "no such source");
    CHECK(!loadSource("plugin:Weather/notes.txt", &err) && err == "no such source");
    CHECK(!loadSource("plugin:Weather/", &err) && !loadSource("plugin:Weather/.wav", &err));
    CHECK(!loadSource("builtin:Nonesuch", &err) && err == "no such field");
    CHECK(!loadSource("", &err) && !loadSource("garbage", &err));
    CHECK(!loadSource("memory:", &err) && !err.empty());
    CHECK(!loadSource("plugin:Weather/absent.wav", &err));
}

// --- the cache ----------------------------------------------------------------------------------------

std::shared_ptr<const SourceBuffer> makeSource(int frames, double hz = 300.0) {
    const Buf x = sine(hz, frames, 0.4f);
    return std::shared_ptr<const SourceBuffer>(af::buildSource(x.data(), x.data(), frames));
}

void testCache() {
    std::printf("== sources: the cache\n");
    af::SourceCache& c = af::SourceCache::get();
    c.clear();
    c.setCap(1u << 30);
    const auto a = makeSource(40000, 200), b = makeSource(40000, 300), cc = makeSource(40000, 400), d = makeSource(40000, 500);
    const size_t each = a->bytes();
    CHECK(each > 0 && b->bytes() == each);

    CHECK(c.find("a") == nullptr);
    CHECK(c.put("a", a) == a);
    CHECK(c.find("a") == a && c.entries() == 1 && c.bytes() == each);
    CHECK(c.put("a", b) == a);               // a second load of one key keeps the first
    CHECK(c.entries() == 1 && c.bytes() == each);
    c.put("b", b);
    CHECK(c.entries() == 2 && c.bytes() == 2 * each);
    c.clear();
    CHECK(c.entries() == 0 && c.bytes() == 0 && c.find("a") == nullptr);

    // Least recently used goes first, and a find counts as a use.
    {
        auto x = makeSource(40000, 210), y = makeSource(40000, 310), z = makeSource(40000, 410), w = makeSource(40000, 510);
        const size_t sz = x->bytes();
        c.setCap(sz * 5 / 2);
        c.put("x", x);
        c.put("y", y);
        x = y = nullptr;   // the cache holds them alone
        c.put("z", z);
        z = nullptr;
        CHECK(c.find("x") == nullptr && c.entries() == 2);   // x was the oldest
        CHECK(c.find("y") != nullptr);                         // y used: z is now the oldest
        c.put("w", w);
        w = nullptr;
        CHECK(c.find("z") == nullptr && c.find("y") != nullptr && c.find("w") != nullptr);
        CHECK(c.bytes() == 2 * sz);
    }

    // One in use is never evicted, however far over the cap, and goes when it is let go.
    {
        c.clear();
        c.setCap(1u << 30);
        auto held = makeSource(40000, 600);
        auto other = makeSource(40000, 700);
        c.put("held", held);
        c.put("other", other);
        other = nullptr;
        c.setCap(1);
        CHECK(c.find("other") == nullptr);
        CHECK(c.find("held") == held && c.entries() == 1);
        held = nullptr;
        c.setCap(1);
        CHECK(c.entries() == 0 && c.bytes() == 0);   // let go: nobody holds it now
    }
    // A source over the cap by itself stays while its loader holds it (put returned it), and goes after.
    {
        c.clear();
        c.setCap(1);
        {
            auto big = c.put("big", makeSource(40000, 800));
            CHECK(c.entries() == 1 && c.find("big") == big);
        }
        c.setCap(1);
        CHECK(c.entries() == 0 && c.bytes() == 0);
    }
    c.clear();
    c.setCap(48u << 20);
}

// --- the loader: its state machine --------------------------------------------------------------------

// Objects that know when they are freed. "bad..." keys fail, "throw" throws, `gate` holds a load.
struct Fake {
    std::atomic<int> created{0};
    std::atomic<bool> freed[256];
    std::function<void(const std::string&)> gate;
    std::mutex mtx;
    std::vector<std::string> keys;                       // every load, in order
    Clock::time_point lastStart{};
    Fake() {
        for (auto& f : freed) f = false;
    }
    Loader::SlotType type(const std::string& fallback = "") {
        Loader::SlotType t;
        t.fallbackKey = fallback;
        t.load = [this](const std::string& key, std::string* err, int* info) -> std::shared_ptr<const void> {
            {
                std::lock_guard<std::mutex> lk(mtx);
                keys.push_back(key);
                lastStart = Clock::now();
            }
            if (gate) gate(key);
            if (key == "throw") throw std::runtime_error("no");
            if (key.compare(0, 3, "bad") == 0) {
                if (err) *err = "bad key";
                return nullptr;
            }
            const int n = created.fetch_add(1);
            if (info) *info = n;
            return std::shared_ptr<const int>(new int(n), [this, n](const int* p) {
                delete p;
                freed[n] = true;
            });
        };
        return t;
    }
    int loads() {
        std::lock_guard<std::mutex> lk(mtx);
        return static_cast<int>(keys.size());
    }
    std::string keyAt(size_t i) {
        std::lock_guard<std::mutex> lk(mtx);
        return i < keys.size() ? keys[i] : std::string();
    }
    Clock::time_point started() {
        std::lock_guard<std::mutex> lk(mtx);
        return lastStart;
    }
};

int liveId(const Loader& L, int slot = 0) {
    const void* p = L.live(slot);
    return p ? *static_cast<const int*>(p) : -1;
}

// The worker has made a few more passes: a graveyard that has not freed something by now will not.
void settle(Loader& L, int passes = 3) {
    const int r0 = L.rounds();
    until([&] { return L.rounds() >= r0 + passes; });
}

void testLoaderStates() {
    std::printf("== sources: the loader's states\n");
    {
        // Ten wants inside the debounce load one, and not before the debounce is over.
        Fake f;
        Loader L({f.type()});
        const auto t0 = Clock::now();
        for (int i = 1; i <= 10; ++i) L.want(0, "k" + std::to_string(i), false);
        CHECK(L.busy());
        CHECK(until([&] { return L.loads() >= 1; }));
        settle(L);
        CHECK(L.loads() == 1 && f.loads() == 1 && f.keyAt(0) == "k10");
        CHECK(f.started() - t0 >= std::chrono::milliseconds(Loader::kDebounceMs));
        const Loader::View v = L.view(0);
        CHECK(v.state == Loader::Ready && v.key == "k10" && v.loadedKey == "k10" && v.info == 0 && v.error.empty());
        CHECK(liveId(L) == 0 && !L.busy() && L.wanted(0) == "k10");
        // Wanting what is loaded does nothing.
        L.want(0, "k10", false);
        L.want(0, "k10", true);
        settle(L);
        CHECK(L.loads() == 1);
    }
    {
        // `now` skips the debounce: the load starts long before 150 ms (three tries: a stall of the
        // machine can spoil one, a broken `now` all of them).
        Fake f;
        Loader L({f.type()});
        bool quick = false;
        for (int attempt = 0; attempt < 3 && !quick; ++attempt) {
            const auto t0 = Clock::now();
            const int before = f.loads();
            L.want(0, "n" + std::to_string(attempt), true);
            CHECK(until([&] { return f.loads() > before; }));
            quick = f.started() - t0 < std::chrono::milliseconds(Loader::kDebounceMs - 30);
        }
        CHECK(quick);
    }
    {
        // A key that fails: nothing is published (no fallback), the wanted key is kept, a scroll
        // doesn't retry it, a pick does.
        Fake f;
        Loader L({f.type()});
        L.want(0, "good", true);
        CHECK(until([&] { return L.loads() == 1; }));
        CHECK(liveId(L) == 0);
        L.want(0, "bad1", true);
        CHECK(until([&] { return L.loads() == 2; }));
        CHECK(L.live(0) == nullptr);
        Loader::View v = L.view(0);
        CHECK(v.state == Loader::Missing && v.key == "bad1" && v.loadedKey.empty() && v.error == "bad key");
        CHECK(!L.busy() && L.wanted(0) == "bad1");
        L.want(0, "bad1", false);
        std::this_thread::sleep_for(std::chrono::milliseconds(Loader::kDebounceMs + 100));   // what a retry would wait for
        settle(L);
        CHECK(L.loads() == 2);
        L.want(0, "bad1", true);
        CHECK(until([&] { return L.loads() == 3; }));
        // A good key afterwards loads and clears the error.
        L.want(0, "good2", true);
        CHECK(until([&] { return L.loads() == 4; }));
        v = L.view(0);
        CHECK(v.state == Loader::Ready && v.error.empty() && liveId(L) == 1);
        // A throw is a failed load, not the end of the worker.
        L.want(0, "throw", true);
        CHECK(until([&] { return L.loads() == 5; }));
        v = L.view(0);
        CHECK(v.state == Loader::Missing && !v.error.empty() && L.live(0) == nullptr);
        L.want(0, "again", true);
        CHECK(until([&] { return L.loads() == 6; }));
        CHECK(L.view(0).state == Loader::Ready && L.live(0) != nullptr);
        // Out-of-range slots and empty keys are ignored.
        L.want(5, "x", true);
        L.want(-1, "x", true);
        settle(L);
        CHECK(L.loads() == 6);
    }
    {
        // A fallback: a key that fails shows the fallback's object, the wanted key kept.
        Fake f;
        Loader L({f.type("fb")});
        CHECK(liveId(L) == 0 && L.view(0).loadedKey == "fb" && L.view(0).state == Loader::Ready);   // ready before any block
        L.want(0, "bad", true);
        CHECK(until([&] { return L.loads() == 1; }));
        const Loader::View v = L.view(0);
        CHECK(v.state == Loader::Missing && v.key == "bad" && v.loadedKey == "fb" && liveId(L) == 1);
    }
    {
        // A load overtaken by another want: its result is dropped (and freed), the new one loads.
        Fake f;
        std::mutex m;
        std::condition_variable cv;
        bool inSlow = false, release = false;
        f.gate = [&](const std::string& key) {
            if (key != "slow") return;
            std::unique_lock<std::mutex> lk(m);
            inSlow = true;
            cv.notify_all();
            cv.wait(lk, [&] { return release; });
        };
        Loader L({f.type()});
        L.want(0, "slow", true);
        {
            std::unique_lock<std::mutex> lk(m);
            CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return inSlow; }));
        }
        L.want(0, "fast", true);
        CHECK(L.view(0).state == Loader::Loading && L.busy());
        {
            std::lock_guard<std::mutex> lk(m);
            release = true;
        }
        cv.notify_all();
        CHECK(until([&] { return L.view(0).loadedKey == "fast"; }));
        CHECK(L.loads() == 1 && L.view(0).state == Loader::Ready);
        CHECK(liveId(L) == 1);   // "slow" made object 0; it was dropped, not published
        CHECK(until([&] { return f.freed[0].load(); }));
    }
    {
        // The listener: told of every publish, in order, off the lock (it may ask the loader), and
        // never after stop().
        Fake f;
        Loader L({f.type()});
        std::mutex m;
        std::vector<std::string> told;
        L.setListener([&](int slot, const std::string& key, bool ok) {
            const std::string w = L.wanted(slot);   // takes the loader's lock: deadlocks if it were held
            std::lock_guard<std::mutex> lk(m);
            told.push_back(key + (ok ? " ok " : " failed ") + w);
        });
        L.want(0, "a", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.want(0, "bad", true);
        CHECK(until([&] { return L.loads() == 2; }));
        L.want(0, "c", true);
        CHECK(until([&] { return L.loads() == 3; }));
        L.stop();
        L.want(0, "late", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lk(m);
        CHECK(told == (std::vector<std::string>{"a ok a", "bad failed bad", "c ok c"}));
        CHECK(L.loads() == 3);
    }
    {
        // stop() with a load in hand waits for it, then nothing runs: the destructor does the same.
        Fake f;
        std::atomic<bool> in{false}, go{false};
        f.gate = [&](const std::string&) {
            in = true;
            while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        auto L = std::make_unique<Loader>(std::vector<Loader::SlotType>{f.type()});
        L->want(0, "held", true);
        CHECK(until([&] { return in.load(); }));
        std::atomic<bool> stopped{false};
        std::thread stopper([&] {
            L->stop();
            stopped = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!stopped);   // it waits for the load
        go = true;
        stopper.join();
        CHECK(stopped);
        L.reset();
        CHECK(f.freed[0].load());   // the Loader's objects go with it
    }
}

void testGraveyard() {
    std::printf("== sources: when a replaced object is freed\n");
    {
        // No block has ever run: a replaced object goes (PolyForce's test of the same).
        Fake f;
        Loader L({f.type()});
        L.want(0, "x", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.want(0, "y", true);
        CHECK(until([&] { return f.freed[0].load(); }));
        CHECK(!f.freed[1].load());
    }
    {
        // A block in progress at the swap may be reading the old object, and so may the next: the
        // object stays until a block that began after the swap has ended.
        Fake f;
        Loader L({f.type()});
        L.want(0, "x", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.blockStart();
        const void* seen = L.live(0);
        CHECK(seen != nullptr && liveId(L) == 0);
        L.want(0, "y", true);
        CHECK(until([&] { return L.loads() == 2; }));
        settle(L);
        CHECK(!f.freed[0].load());                 // the running block may be reading it
        L.blockDone();
        settle(L);
        CHECK(!f.freed[0].load());                 // the next block is the first to see "y": it may still read x (Weather's copy)
        L.blockStart();
        CHECK(liveId(L) == 1);
        settle(L);
        CHECK(!f.freed[0].load());                 // ... and it is still running
        L.blockDone();
        CHECK(until([&] { return f.freed[0].load(); }));
        CHECK(!f.freed[1].load());
    }
    {
        // Between blocks, holding what it was given (Weather audible): not idle. The next block
        // is the one that may read the old object; it frees after that block.
        Fake f;
        Loader L({f.type()});
        L.want(0, "x", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.blockStart();
        CHECK(liveId(L) == 0);
        L.blockDone(true);
        L.want(0, "y", true);
        CHECK(until([&] { return L.loads() == 2; }));
        settle(L);
        CHECK(!f.freed[0].load());                 // no block running, but one holds it
        L.blockStart();
        CHECK(liveId(L) == 1);                     // it sees the new one, and copies from the old
        CHECK(!f.freed[0].load());
        L.blockDone(true);
        CHECK(until([&] { return f.freed[0].load(); }));
        // Without the argument a block holds: the default is the safe one.
        L.want(0, "z", true);
        CHECK(until([&] { return L.loads() == 3; }));
        settle(L);
        CHECK(!f.freed[1].load());
        L.blockStart();
        L.blockDone();
        CHECK(until([&] { return f.freed[1].load(); }));
    }
    {
        // Between blocks, holding nothing (Weather not audible): free at once, no block needed.
        Fake f;
        Loader L({f.type()});
        L.want(0, "x", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.blockStart();
        CHECK(liveId(L) == 0);
        L.blockDone(false);
        L.want(0, "y", true);
        CHECK(until([&] { return f.freed[0].load(); }));
        // ... but not while a block runs, whatever the last one said.
        L.blockStart();
        L.want(0, "z", true);
        CHECK(until([&] { return L.loads() == 3; }));
        settle(L);
        CHECK(!f.freed[1].load());
        L.blockDone(false);
        CHECK(until([&] { return f.freed[1].load(); }));
    }
    {
        // Several swaps in a row: each goes by the same rule, and the one the block read last stays.
        Fake f;
        Loader L({f.type()});
        L.want(0, "a", true);
        CHECK(until([&] { return L.loads() == 1; }));
        L.blockStart();
        L.blockDone(true);   // holds a
        for (int i = 0; i < 5; ++i) {
            L.want(0, "k" + std::to_string(i), true);
            CHECK(until([&] { return L.loads() == 2 + i; }));
        }
        settle(L);
        for (int i = 0; i < 5; ++i) CHECK(!f.freed[i].load());   // none goes while a block may read the first
        L.blockStart();
        CHECK(liveId(L) == 5);
        L.blockDone(true);
        CHECK(until([&] { return f.freed[0].load() && f.freed[1].load() && f.freed[2].load() && f.freed[3].load() && f.freed[4].load(); }));
        CHECK(!f.freed[5].load());
    }
}

void testPost() {
    std::printf("== sources: jobs on the worker\n");
    Fake f;
    Loader L({f.type()});
    std::mutex m;
    std::vector<int> order;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 20; ++i)
        L.post([&, i] {
            std::lock_guard<std::mutex> lk(m);
            order.push_back(i);
            threads.insert(std::this_thread::get_id());
        });
    CHECK(until([&] {
        std::lock_guard<std::mutex> lk(m);
        return order.size() == 20;
    }));
    {
        std::lock_guard<std::mutex> lk(m);
        std::vector<int> want(20);
        for (int i = 0; i < 20; ++i) want[static_cast<size_t>(i)] = i;
        CHECK(order == want);
        CHECK(threads.size() == 1 && *threads.begin() != std::this_thread::get_id());
    }
    // A job that throws is dropped, the worker goes on (with loads too); a job may post another.
    std::atomic<int> after{0};
    L.post([] { throw std::runtime_error("job"); });
    L.post([&] { L.post([&] { after = 2; }); after = 1; });
    CHECK(until([&] { return after.load() == 2; }));
    L.want(0, "x", true);
    CHECK(until([&] { return L.loads() == 1; }));
    // A job is destroyed on the worker after it runs (what it captured does not outlive it).
    {
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        L.post([token] { (void)token; });
        token.reset();
        CHECK(until([&] { return weak.expired(); }));
    }
    // ... and not under the loader's lock: a destructor that asks the loader something would wait for itself.
    {
        std::atomic<bool> gone{false};
        std::shared_ptr<int> t(new int(1), [&](int* q) {
            L.wanted(0);
            delete q;
            gone = true;
        });
        L.post([t] { (void)t; });
        t.reset();
        CHECK(until([&] { return gone.load(); }));
    }
    // Queued behind a job that is running when stop() comes: dropped, and never run after stop().
    {
        Loader L2({f.type()});
        std::atomic<bool> in{false}, go{false}, ranSecond{false};
        L2.post([&] {
            in = true;
            while (!go) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        CHECK(until([&] { return in.load(); }));
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        L2.post([&, token] { ranSecond = true; });
        token.reset();
        std::thread stopper([&] { L2.stop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        go = true;
        stopper.join();
        CHECK(!ranSecond);
        CHECK(weak.expired());   // what it held was let go
        L2.post([&] { ranSecond = true; });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(!ranSecond);
    }
}

// The audio thread played by a thread of the test, against a thread that swaps as fast as it can.
// Like Weather it keeps the pointer from the block before while it "holds" (and uses it first thing
// in the next block, the copy from the old source), so a free that the rule allows too early is a read
// of freed memory, which ASan stops. Objects are real allocations of a few KB.
struct Blob {
    std::vector<int> v;
    explicit Blob(int n) : v(static_cast<size_t>(n), n) {}
};
std::atomic<long long> g_sink{0};   // what the audio thread read (so that reading it is not optimised away)
void touch(const void* p) {
    if (!p) return;
    long long s = 0;
    for (int x : static_cast<const Blob*>(p)->v) s += x;
    g_sink.fetch_add(s, std::memory_order_relaxed);
}

void testStress() {
    std::printf("== sources: stress, a swapper against an audio thread\n");
    std::atomic<int> created{0}, freed{0};
    Loader::SlotType t;
    t.load = [&](const std::string& key, std::string* err, int*) -> std::shared_ptr<const void> {
        if (key.compare(0, 3, "bad") == 0) {
            if (err) *err = "no";
            return nullptr;
        }
        ++created;
        return std::shared_ptr<const Blob>(new Blob(500 + static_cast<int>(key.size()) * 37 + created.load() % 11), [&](const Blob* b) {
            delete b;
            ++freed;
        });
    };
    long long blocks = 0, heldReads = 0;
    {
        Loader L({t});
        std::atomic<bool> stop{false};
        std::thread audio([&] {
            const void* prev = nullptr;
            uint32_t s = 7;
            while (!stop) {
                L.blockStart();
                if (prev) {   // what Weather does on the first render() with a new source
                    touch(prev);
                    ++heldReads;
                }
                const void* cur = L.live(0);
                touch(cur);
                s = s * 1664525u + 1013904223u;
                const bool holds = cur && ((s >> 16) % 4 != 0);
                prev = holds ? cur : nullptr;
                L.blockDone(holds);
                ++blocks;
                // MPC's blocks are 2.9 ms apart; this is faster, with gaps for the worker to see
                std::this_thread::sleep_for(std::chrono::microseconds(30 + (s >> 20) % 200));
            }
        });
        const auto end = Clock::now() + std::chrono::milliseconds(1500);
        uint32_t s = 99;
        for (int i = 0; Clock::now() < end; ++i) {
            s = s * 1664525u + 1013904223u;
            L.want(0, (s >> 28) == 0 ? "bad" + std::to_string(i) : "key" + std::string(static_cast<size_t>(1 + (s >> 20) % 9), 'k') + std::to_string(i), true);
            if ((s >> 12) % 5 == 0) L.post([] {});
            std::this_thread::sleep_for(std::chrono::microseconds((s >> 8) % 1500));
        }
        stop = true;
        audio.join();
        std::printf("  %lld blocks, %lld of them reading the block before's pointer, %d loads\n", blocks, heldReads, L.loads());
        CHECK(blocks > 100 && heldReads > 50 && L.loads() > 20);
    }   // the loader goes: everything it held is freed
    CHECK(created.load() > 50 && freed.load() == created.load());
}

// --- sources through the loader -----------------------------------------------------------------------

void testSourceSlot() {
    std::printf("== sources: the source slot\n");
    const std::string plug = dir() + "/slot/plugin", ssd = dir() + "/slot/ssd";
    fs::create_directories(plug);
    fs::create_directories(ssd);
    useRoots(plug, ssd);
    af::SourceCache::get().clear();
    makeWav(plug + "/Weather/Tone.wav", 44100, 440.0);
    makeWav(ssd + "/AmbientForce/Weather/Short.wav", 3);
    const std::string tone = "plugin:Weather/Tone.wav";

    Loader L({sourceSlotType()});
    CHECK(L.live(0) == nullptr && sourceOf(L.live(0)) == nullptr);   // nothing until a key loads
    // A field: a ready source within 2 s (x86, ASan).
    const auto t0 = Clock::now();
    L.want(0, "builtin:Surf", false);
    CHECK(until([&] { return L.loads() == 1; }, 2000));
    CHECK(Clock::now() - t0 < std::chrono::milliseconds(2000 * kSlow));
    const GrainSource* g = sourceOf(L.live(0));
    CHECK(g && g->ready() && g->frames > 0 && g->frames % 4 == 0);
    Loader::View v = L.view(0);
    CHECK(v.state == Loader::Ready && v.loadedKey == "builtin:Surf" && g && v.info == g->frames);
    // The same key from another instance is the same source (the cache), made once.
    {
        Loader L2({sourceSlotType()});
        L2.want(0, "builtin:Surf", true);
        CHECK(until([&] { return L2.loads() == 1; }));
        CHECK(L2.live(0) == L.live(0));
    }
    // A missing file: nothing published, the wanted key kept.
    L.want(0, "plugin:Weather/absent.wav", true);
    CHECK(until([&] { return L.loads() == 2; }));
    v = L.view(0);
    CHECK(L.live(0) == nullptr && v.state == Loader::Missing && v.key == "plugin:Weather/absent.wav" && !v.error.empty());
    // memory: loads nothing: Missing, saying what it is.
    L.want(0, kMemoryKey, true);
    CHECK(until([&] { return L.loads() == 3; }));
    v = L.view(0);
    CHECK(L.live(0) == nullptr && v.state == Loader::Missing && v.key == kMemoryKey && v.error.find("Memory") != std::string::npos);
    // A WAV: a source of its length less the loop's fade, normalised.
    L.want(0, tone, true);
    CHECK(until([&] { return L.loads() == 4; }));
    g = sourceOf(L.live(0));
    CHECK(g && g->ready() && L.view(0).state == Loader::Ready);
    if (g) {
        const int fade = static_cast<int>(af::kLoopFadeS * af::kRate);
        CHECK(g->frames <= 44100 && g->frames >= 44100 - fade - 4 && g->frames % 4 == 0);
        double s = 0.0;
        for (int i = 0; i < g->frames; ++i) {
            const double x = g->level[0][2 * i] * static_cast<double>(g->gain);
            s += x * x;
        }
        CHECK(std::fabs(20.0 * std::log10(std::sqrt(s / g->frames)) - af::kSourceRmsDb) < 0.3);
    }
    // A WAV of 61 s is read as 60 s: its source is that less the loop's fade.
    {
        const int n = 61 * 44100;
        Bytes data;
        data.reserve(2 * static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) put16(data, static_cast<uint32_t>(static_cast<int16_t>(2000.0 * std::sin(0.05 * i))));
        CHECK(writeBytes(plug + "/Weather/Long.wav", riff({chunk("fmt ", fmtBody(1, 1, 44100, 16)), chunk("data", data)})));
        std::string e;
        const auto s = loadSource("plugin:Weather/Long.wav", &e);
        const int fade = static_cast<int>(af::kLoopFadeS * af::kRate);
        CHECK(s && s->src.frames <= 60 * 44100 && s->src.frames >= 60 * 44100 - fade - 4);
        fs::remove(plug + "/Weather/Long.wav");
    }
    // A WAV of 3 frames is a source (a loop of 4).
    L.want(0, "ssd:AmbientForce/Weather/Short.wav", true);
    CHECK(until([&] { return L.loads() == 5; }));
    CHECK(sourceOf(L.live(0)) && sourceOf(L.live(0))->ready() && sourceOf(L.live(0))->frames == 4);
    // A file that isn't a WAV: Missing with the reason.
    { std::ofstream(plug + "/Weather/Notaudio.wav") << "this is not a wave file at all, but it is named like one"; }
    L.want(0, "plugin:Weather/Notaudio.wav", true);
    CHECK(until([&] { return L.loads() == 6; }));
    v = L.view(0);
    CHECK(L.live(0) == nullptr && v.state == Loader::Missing && v.error == "not a WAV file");
    // A truncated one too.
    {
        Bytes f = riff({chunk("fmt ", fmtBody(1, 2, 44100, 16)), chunk("data", Bytes(4000, 0))});
        f.resize(f.size() - 1000);
        CHECK(writeBytes(plug + "/Weather/Cut.wav", f));
    }
    L.want(0, "plugin:Weather/Cut.wav", true);
    CHECK(until([&] { return L.loads() == 7; }));
    CHECK(L.live(0) == nullptr && L.view(0).error == "truncated");

    // Scrolling through ten keys inside the debounce loads the one it stops on.
    rescanSources();
    const std::vector<std::string> keys = sourceKeys();
    CHECK(keys.size() >= 10);
    std::vector<std::string> scroll(keys.begin(), keys.begin() + 8);   // the fields
    scroll.push_back(tone);
    scroll.push_back("builtin:Night");
    const int before = L.loads();
    for (const std::string& k : scroll) L.want(0, k, false);
    CHECK(until([&] { return L.loads() > before; }));
    settle(L);
    CHECK(L.loads() == before + 1 && L.view(0).key == "builtin:Night" && L.view(0).loadedKey == "builtin:Night");

    // Two wants of one key load it once: the second finds it cached and is the same object.
    af::SourceCache& cache = af::SourceCache::get();
    const int made0 = af::sourcesMade();
    auto first = loadSource(tone);
    const size_t entries = cache.entries();
    auto second = loadSource(tone);
    CHECK(first && first == second && cache.entries() == entries);
    CHECK(af::sourcesMade() == made0);   // the slot has loaded it: both of these found it
    CHECK(loadSource("builtin:Stream") == loadSource("builtin:Stream"));
    CHECK(af::sourcesMade() == made0 + 1);
    {   // a file gone from the disk is still a source while it is cached
        makeWav(plug + "/Weather/Fleeting.wav", 4000);
        auto a = loadSource("plugin:Weather/Fleeting.wav");
        CHECK(a && af::sourcesMade() == made0 + 2);
        fs::remove(plug + "/Weather/Fleeting.wav");
        CHECK(loadSource("plugin:Weather/Fleeting.wav") == a && af::sourcesMade() == made0 + 2);
    }
    first = second = nullptr;
    // Over the cap, the least recently used one not in use goes. The slot's source is in use.
    L.want(0, tone, true);
    CHECK(until([&] { return L.view(0).loadedKey == tone; }));
    cache.setCap(1);
    CHECK(cache.find(tone) != nullptr);                  // in use by the slot
    CHECK(cache.find("builtin:Stream") == nullptr);      // nobody's
    CHECK(cache.find("builtin:Surf") == nullptr);
    cache.setCap(48u << 20);
    L.stop();
}

// --- Keep ---------------------------------------------------------------------------------------------

// A Memory that has recorded `seconds` of a sine (amp 1.2 clips Keep's headroom back) and kept it.
std::unique_ptr<Memory> remembered(double seconds, double amp = 0.5, bool ramp = false) {
    auto m = std::make_unique<Memory>();
    const int n = static_cast<int>(seconds * 44100);
    std::vector<float> x(128);
    for (int at = 0; at < n; at += 128) {
        const int len = std::min(128, n - at);
        for (int i = 0; i < len; ++i) {
            const int k = at + i;
            x[static_cast<size_t>(i)] = ramp ? static_cast<float>(k) * 1.0e-6f
                                             : static_cast<float>(amp * std::sin(2.0 * kPi * 500.0 * k / 44100.0));
        }
        m->write(x.data(), x.data(), len);
    }
    CHECK(m->remember());
    return m;
}

bool keepIsRing(const WavData& w, const GrainSource& src) {
    if (static_cast<int>(w.l.size()) != src.frames || w.r.size() != w.l.size()) return false;
    const float k = src.gain * 32768.0f;
    for (int i = 0; i < src.frames; ++i) {
        const int from = (src.origin + i) % src.frames;
        for (int c = 0; c < 2; ++c) {
            const float v = std::floor(static_cast<float>(src.level[0][2 * from + c]) * k + 0.5f);
            const float want = std::max(-32768.0f, std::min(v, 32767.0f)) / 32768.0f;
            if ((c == 0 ? w.l : w.r)[static_cast<size_t>(i)] != want) return false;
        }
    }
    return true;
}

struct HookCtx {
    Memory* mem = nullptr;
    int calls = 0;
    bool refused = false;
    uint32_t generation = 0;
};

void testKeep() {
    std::printf("== sources: Keep\n");
    const std::string plug = dir() + "/keep/plugin", ssd = dir() + "/keep/ssd";
    fs::create_directories(plug);
    fs::create_directories(ssd);
    useRoots(plug, ssd);
    const std::string folder = ssd + "/AmbientForce/Memories";
    std::string err;

    // Nothing remembered: nothing written.
    {
        Memory empty;
        CHECK(keepMemory(empty, &err).empty() && err == "nothing remembered");
        CHECK(!fs::exists(folder));
    }

    auto mem = remembered(3.0, 1.2);   // loud enough that the file's doubling clips
    const GrainSource* src = mem->remembered();
    CHECK(src && src->ready() && std::abs(src->frames - 3 * 44100) <= 4);
    CHECK(src && src->gain == 2.0f / 32768.0f);

    // The first, the second, then a hole refilled. The first is listed at once (the list was looked at before).
    sourceKeys();
    err.clear();
    std::string key = keepMemory(*mem, &err);
    CHECK(key == "ssd:AmbientForce/Memories/Memory 001.wav" && err.empty());
    CHECK(contains(sourceKeys(), key));
    CHECK(fs::exists(folder + "/Memory 001.wav"));
    CHECK(keepMemory(*mem, &err) == "ssd:AmbientForce/Memories/Memory 002.wav");
    CHECK(keepMemory(*mem, &err) == "ssd:AmbientForce/Memories/Memory 003.wav");
    fs::remove(folder + "/Memory 001.wav");
    CHECK(keepMemory(*mem, &err) == "ssd:AmbientForce/Memories/Memory 001.wav");
    CHECK(keepMemory(*mem, &err) == "ssd:AmbientForce/Memories/Memory 004.wav");

    // The file is the ring from its oldest frame on, at the level it was played: the 16-bit ring
    // times two (the headroom back), clamped.
    {
        WavData w;
        CHECK(readWav(folder + "/Memory 002.wav", w, 60.0f, &err));
        CHECK(w.rate == 44100 && w.channels == 2 && src && keepIsRing(w, *src));
        float top = 0.0f;
        for (float x : w.l) top = std::max(top, std::fabs(x));
        CHECK(top > 0.99f && top <= 1.0f);   // clipped at full scale
    }
    // It leaves no temporary file in the folder, and is listed at once.
    {
        int files = 0;
        for (const auto& e : fs::directory_iterator(folder)) {
            ++files;
            CHECK(e.path().extension() == ".wav");
        }
        CHECK(files == 4);
        CHECK(contains(sourceKeys(), key));
        CHECK(contains(sourceKeys(), "ssd:AmbientForce/Memories/Memory 004.wav"));
        // ... and loads as a source: its frames less the loop's fade.
        const auto s = loadSource(key, &err);
        CHECK(s && s->src.ready() && s->src.frames <= src->frames);
    }

    // A full ring (20 s of a ramp through 16 s): the file starts at the oldest frame and runs in time order.
    {
        auto full = remembered(20.0, 0.5, true);
        const GrainSource* fsrc = full->remembered();
        CHECK(fsrc && fsrc->frames == 16 * 44100);
        const std::string fkey = keepMemory(*full, &err);
        CHECK(!fkey.empty());
        WavData w;
        CHECK(readWav(ssd + "/" + fkey.substr(4), w, 60.0f, &err));
        CHECK(fsrc && keepIsRing(w, *fsrc));
        // In time order away from the 5 ms fades at the seam: a rising ramp from 4 s on.
        bool rising = true;
        for (size_t i = 1000; i + 1000 < w.l.size(); ++i) rising = rising && w.l[i] >= w.l[i - 1] - 1e-6f;
        CHECK(rising);
        CHECK(std::fabs(w.l[2000] - (4.0f * 44100.0f + 2000.0f) * 1.0e-6f) < 0.002f);
    }

    // The ring is pinned while Keep reads and writes: a Remember in the middle is refused, and after
    // it Remember works again (the recorded 3 s are past the gap).
    {
        HookCtx ctx;
        ctx.mem = mem.get();
        ctx.generation = mem->generation();
        setKeepHook(
            [](void* p) {
                HookCtx& c = *static_cast<HookCtx*>(p);
                ++c.calls;
                std::vector<float> x(128, 0.2f);
                for (int i = 0; i < 3 * 344; ++i) c.mem->write(x.data(), x.data(), 128);   // 3 s more
                c.refused = !c.mem->remember() && c.mem->generation() == c.generation;
            },
            &ctx);
        const GrainSource copy = *mem->remembered();
        const int16_t first = copy.level[0][0];
        err.clear();
        key = keepMemory(*mem, &err);
        setKeepHook(nullptr, nullptr);
        CHECK(ctx.calls == 1 && ctx.refused);
        CHECK(!key.empty() && mem->remembered() && mem->remembered()->level[0][0] == first && mem->generation() == ctx.generation);
        CHECK(mem->remember());
        CHECK(mem->generation() == ctx.generation + 1);
    }

    // Keeps from four instances at once get four names.
    {
        std::vector<std::unique_ptr<Memory>> mems;
        for (int i = 0; i < 4; ++i) mems.push_back(remembered(1.0));
        fs::remove_all(folder);
        std::vector<std::string> keys(4);
        std::vector<std::thread> th;
        for (int i = 0; i < 4; ++i)
            th.emplace_back([&, i] {
                std::string e;
                keys[static_cast<size_t>(i)] = keepMemory(*mems[static_cast<size_t>(i)], &e);
            });
        for (std::thread& t : th) t.join();
        std::set<std::string> distinct(keys.begin(), keys.end());
        CHECK(distinct.size() == 4 && !distinct.count(""));
        CHECK(fs::exists(folder + "/Memory 001.wav") && fs::exists(folder + "/Memory 004.wav"));
    }

    // Through the loader's thread, as the plugin does it.
    {
        Fake f;
        Loader L({f.type()});
        std::string k, e;
        std::atomic<bool> done{false};
        L.post([&] {
            k = keepMemory(*mem, &e);
            done = true;
        });
        CHECK(until([&] { return done.load(); }));
        CHECK(!k.empty() && e.empty());
    }

    // It can't write: no SSD (one root, or a root that isn't there or isn't a folder), a folder in the
    // way. A clean failure each, nothing left, and the ring is let go (Remember works again).
    {
        auto m2 = remembered(1.0);
        const auto letGo = [&](Memory& m) {
            std::vector<float> x(128, 0.1f);
            for (int i = 0; i < 3 * 344; ++i) m.write(x.data(), x.data(), 128);
            return m.remember();
        };
        setenv("AF_SOURCE_ROOTS", plug.c_str(), 1);
        CHECK(keepMemory(*m2, &err).empty() && err == "no SSD");
        CHECK(letGo(*m2));
        setenv("AF_SOURCE_ROOTS", (plug + ":" + dir() + "/keep/absent").c_str(), 1);
        CHECK(keepMemory(*m2, &err).empty() && err == "no SSD");
        CHECK(!fs::exists(dir() + "/keep/absent"));   // a missing card isn't made up
        CHECK(letGo(*m2));
        { std::ofstream(dir() + "/keep/afile") << "x"; }
        setenv("AF_SOURCE_ROOTS", (plug + ":" + dir() + "/keep/afile").c_str(), 1);
        CHECK(keepMemory(*m2, &err).empty() && err == "no SSD");
        CHECK(letGo(*m2));
        const std::string blocked = dir() + "/keep/blocked";
        fs::create_directories(blocked);
        { std::ofstream(blocked + "/AmbientForce") << "a file where the folder should be"; }
        setenv("AF_SOURCE_ROOTS", (plug + ":" + blocked).c_str(), 1);
        CHECK(keepMemory(*m2, &err).empty() && err == "cannot make the Memories folder");
        CHECK(letGo(*m2));
        // A name that can't be written (the file is a folder): the write fails and nothing is left.
        const std::string trap = dir() + "/keep/trap";
        for (int n = 1; n <= 3; ++n) {
            char b[32];
            std::snprintf(b, sizeof b, "Memory %03d.wav", n);
            fs::create_directories(trap + "/AmbientForce/Memories/" + b + "/in");   // the first three names are folders
        }
        setenv("AF_SOURCE_ROOTS", (plug + ":" + trap).c_str(), 1);
        key = keepMemory(*m2, &err);   // 001..003 exist (as folders): it takes 004
        CHECK(key == "ssd:AmbientForce/Memories/Memory 004.wav");
        // The Memories folder not writable (a read-only folder; not for root, which writes anywhere).
        if (::geteuid() != 0) {
            const std::string ro = dir() + "/keep/ro";
            fs::create_directories(ro + "/AmbientForce/Memories");
            fs::permissions(ro + "/AmbientForce/Memories", fs::perms::owner_read | fs::perms::owner_exec);
            setenv("AF_SOURCE_ROOTS", (plug + ":" + ro).c_str(), 1);
            CHECK(keepMemory(*m2, &err).empty() && !err.empty());
            int files = 0;
            for (const auto& e : fs::directory_iterator(ro + "/AmbientForce/Memories")) {
                (void)e;
                ++files;
            }
            CHECK(files == 0);
            fs::permissions(ro + "/AmbientForce/Memories", fs::perms::owner_all);
            CHECK(letGo(*m2));
        }
    }
    useRoots(plug, ssd);
}

} // namespace

void sourcesTests() {
    const char* old = std::getenv("AF_SOURCE_ROOTS");
    const std::string saved = old ? old : "";
    af::SourceCache::get().clear();
    testRoundTrip();
    testAtomicReplace();
    testFormats();
    testChunks();
    testBadFiles();
    testFuzz();
    testLimit();
    testResample();
    testKeys();
    testCache();
    testLoaderStates();
    testGraveyard();
    testPost();
    testStress();
    testSourceSlot();
    testKeep();
    if (old) setenv("AF_SOURCE_ROOTS", saved.c_str(), 1);
    else unsetenv("AF_SOURCE_ROOTS");
    rescanSources();
    af::SourceCache::get().clear();
    std::error_code ec;
    fs::permissions(dir() + "/keep/ro/AmbientForce/Memories", fs::perms::owner_all, ec);
    fs::remove_all(dir(), ec);
}

} // namespace aft
