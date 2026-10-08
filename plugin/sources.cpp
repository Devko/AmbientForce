// Weather's sources by key: see sources.h. The listing's walk is SubForce's plugin/library.cpp (8846421).
#include "sources.h"

#include "paths.h"
#include "wav.h"
#include "../dsp/fields.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace af {
namespace fs = std::filesystem;

namespace {

constexpr int kMaxDepth = 4;        // folders below the source folder: deep enough, never a runaway walk
constexpr size_t kMaxFiles = 4096;  // the stepper walks this list; so much is already a library
constexpr auto kScanEvery = std::chrono::seconds(2);
constexpr const char* kBuiltin = "builtin:";

// The folders a file key may name: a root's label and the folder under that root.
struct Place {
    const char* label;
    const char* folder;
};
constexpr Place kPlaces[] = {{"plugin", "Weather"}, {"ssd", "AmbientForce/Weather"}, {"ssd", "AmbientForce/Memories"}};
constexpr const char* kMemoriesFolder = "AmbientForce/Memories";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWithWav(const std::string& s) {
    return s.size() > 4 && lower(s.substr(s.size() - 4)) == ".wav";
}

std::string stemOf(const std::string& file) {
    const size_t slash = file.find_last_of('/');
    std::string s = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t dot = s.rfind('.');
    return dot == std::string::npos || dot == 0 ? s : s.substr(0, dot);
}

bool inAPlace(const std::string& key) {
    for (const Place& p : kPlaces) {
        const std::string prefix = std::string(p.label) + ":" + p.folder + "/";
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0) return true;
    }
    return false;
}

struct Found {
    std::string key, folded;   // folded: the stem in lower case, what the list is sorted by
};

void walk(const Root& root, const Place& place, std::vector<Found>& out) {
    const std::string base = root.dir + "/" + place.folder;
    const fs::path basePath(base);
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && out.size() < kMaxFiles; it.increment(ec)) {
        if (it.depth() >= kMaxDepth) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::directory_entry& e = *it;
        std::error_code fe;   // one entry's trouble skips that entry; `ec` is the walk's own
        if (e.is_symlink(fe)) {   // never follow links: no loops, no surprises
            if (e.is_directory(fe)) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(fe)) continue;
        // The path relative to the folder, worked out from the names (fs::relative would look at the
        // disk for every component of every file).
        const std::string rel = e.path().lexically_relative(basePath).generic_string();
        if (rel.empty() || rel.compare(0, 2, "..") == 0 || !endsWithWav(rel)) continue;
        if (rel[0] == '.' || rel.find("/.") != std::string::npos) continue;   // hidden files, "._" macOS junk
        // A control character (a newline) would break state text, and resolveKey refuses such keys.
        if (std::any_of(rel.begin(), rel.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) continue;
        out.push_back({std::string(place.label) + ":" + place.folder + "/" + rel, lower(stemOf(rel))});
    }
}

// The last scan of the WAV folders, kept for kScanEvery (and for the roots it was made with).
struct Scan {
    bool have = false;
    std::string roots;
    std::chrono::steady_clock::time_point at{};
    std::vector<std::string> keys;
};
std::mutex g_scanMtx;
Scan g_scan;

std::string rootsId(const std::vector<Root>& roots) {
    std::string id;
    for (const Root& r : roots) id += r.label + "=" + r.dir + "\n";
    return id;
}

std::atomic<int> g_made{0};   // sources made by loadSource, not found in the cache
std::mutex g_keepMtx;         // one Keep at a time: the number is chosen and claimed under it
KeepHook g_keepHook = nullptr;
void* g_keepCtx = nullptr;

// A file's size and time, as the cache keeps them with what it made from it.
FileStamp stampOf(const std::string& path) {
    FileStamp s;
    struct stat st {};
    if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        s.valid = true;
        s.size = static_cast<uint64_t>(st.st_size);
        s.mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
    }
    return s;
}

// The next Memory number: one past the highest ever used, the files there and the folder's own note
// of the last one kept (.last, hidden from the listing), so a deleted "Memory 007" is never used
// again: a project or preset that named it shows MISSING, not another recording (the presets' rule,
// plugin/presets.cpp's nextUserPreset). The name is claimed by creating it, exclusively. 0 with
// *err if the folder can't be written.
int claimMemory(const std::string& dir, std::string* path, std::string* name, std::string* err) {
    int top = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        int n = 0;
        char tail[8] = {};
        // %8d: a huge number in a file name can't overflow n, or top + 100 below.
        if (std::sscanf(it->path().filename().string().c_str(), "Memory %8d.wa%1s", &n, tail) == 2 && tail[0] == 'v' && n > 0)
            top = std::max(top, n);
    }
    std::string last;
    if (readFile(dir + "/.last", last, 64)) top = std::max(top, std::min(std::max(std::atoi(last.c_str()), 0), 99999999));
    if (g_keepHook) g_keepHook(g_keepCtx, KS_NUMBERED);
    for (int n = top + 1; n < top + 100; ++n) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "Memory %03d.wav", n);
        const std::string p = dir + "/" + buf;
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;   // taken since the listing
            break;                          // the folder can't be written: the next number won't do either
        }
        ::close(fd);
        *path = p;
        *name = buf;
        return n;
    }
    if (err) *err = "cannot write the Memories folder";
    return 0;
}

} // namespace

std::string defaultSourceKey() { return std::string(kBuiltin) + kFieldNames[FD_RAIN_ROOF]; }

int sourcesMade() { return g_made.load(); }

void rescanSources() {
    std::lock_guard<std::mutex> lk(g_scanMtx);
    g_scan.have = false;
}

std::vector<std::string> sourceKeys() {
    std::vector<std::string> keys;
    for (int i = 0; i < FD_COUNT; ++i) keys.push_back(std::string(kBuiltin) + kFieldNames[i]);
    keys.push_back(kMemoryKey);

    const std::vector<Root> roots = sourceRoots();
    const std::string id = rootsId(roots);
    {
        std::lock_guard<std::mutex> lk(g_scanMtx);
        if (g_scan.have && g_scan.roots == id && std::chrono::steady_clock::now() - g_scan.at < kScanEvery) {
            keys.insert(keys.end(), g_scan.keys.begin(), g_scan.keys.end());
            return keys;
        }
    }
    std::vector<Found> found;   // the disk, outside the lock
    for (const Place& p : kPlaces)
        for (const Root& r : roots)
            if (r.label == p.label) walk(r, p, found);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return a.folded != b.folded ? a.folded < b.folded : a.key < b.key;
    });
    std::vector<std::string> wavs;
    for (const Found& f : found) wavs.push_back(f.key);
    {
        std::lock_guard<std::mutex> lk(g_scanMtx);
        g_scan.have = true;
        g_scan.roots = id;
        g_scan.at = std::chrono::steady_clock::now();
        g_scan.keys = wavs;
    }
    keys.insert(keys.end(), wavs.begin(), wavs.end());
    return keys;
}

std::string sourceName(const std::string& key) {
    if (key == kMemoryKey) return "Memory";
    const std::string builtin = kBuiltin;
    if (key.compare(0, builtin.size(), builtin) == 0) return key.substr(builtin.size());
    const size_t colon = key.find(':');
    return stemOf(colon == std::string::npos ? key : key.substr(colon + 1));
}

std::shared_ptr<const SourceBuffer> loadSource(const std::string& key, std::string* err) {
    const auto fail = [&](const char* why) -> std::shared_ptr<const SourceBuffer> {
        if (err) *err = why;
        return nullptr;
    };
    try {
        if (key == kMemoryKey) return fail("Memory plays from the engine");
        const std::string builtin = kBuiltin;
        if (key.compare(0, builtin.size(), builtin) == 0) {
            const std::string name = key.substr(builtin.size());
            int id = -1;
            for (int i = 0; i < FD_COUNT; ++i)
                if (name == kFieldNames[i]) id = i;
            if (id < 0) return fail("no such field");
            if (auto hit = SourceCache::get().find(key)) return hit;
            std::shared_ptr<const SourceBuffer> field = renderField(id);
            if (!field) return fail("could not render the field");
            ++g_made;
            return SourceCache::get().put(key, std::move(field));
        }
        if (!inAPlace(key) || !endsWithWav(key)) return fail("no such source");
        const std::string path = resolveKey(key, sourceRoots());
        if (path.empty()) return fail("unknown folder");
        // A cached source stands for the file as it was when it was made: one stat says whether it still
        // does. A file that has gone is gone, cached or not.
        const FileStamp stamp = stampOf(path);
        if (auto hit = SourceCache::get().find(key, &stamp)) return hit;
        std::unique_ptr<SourceBuffer> made;
        {
            WavData wav;
            if (!readWav(path, wav, kMaxWavSeconds, err)) return nullptr;
            made = buildSource(wav.l.data(), wav.r.data(), static_cast<int>(wav.l.size()));
        }   // the WAV's floats go before the source is cached
        if (!made) return fail("no audio");
        ++g_made;
        return SourceCache::get().put(key, std::shared_ptr<const SourceBuffer>(std::move(made)), stamp);
    } catch (...) {   // out of memory, in a field or a source being made: a failed load
        return fail("out of memory");
    }
}

Loader::SlotType sourceSlotType() {
    Loader::SlotType t;
    t.load = [](const std::string& key, std::string* err, int* info) -> std::shared_ptr<const void> {
        auto source = loadSource(key, err);
        if (source && info) *info = source->src.frames;
        return source;
    };
    return t;
}

void setKeepHook(KeepHook hook, void* ctx) {
    g_keepHook = hook;
    g_keepCtx = ctx;
}

std::string keepMemory(Memory& memory, std::string* err) {
    const auto fail = [&](const char* why) {
        if (err) *err = why;
        return std::string();
    };
    try {
        // The ring from its oldest frame on, at the level it was played (the headroom put back), copied
        // while pinned: Remember is refused only for as long as the copy takes. The file is written from
        // the copy, so a Remember during the write (a second or two on the SSD) changes nothing in it.
        std::vector<int16_t> pcm;
        int frames = 0;
        {
            if (!memory.pin()) return fail("nothing remembered");
            struct Unpin {
                Memory& m;
                ~Unpin() { m.unpin(); }
            } unpin{memory};
            const GrainSource* src = memory.remembered();
            if (!src || !src->ready()) return fail("nothing remembered");
            if (g_keepHook) g_keepHook(g_keepCtx, KS_COPY);
            frames = src->frames;
            const float scale = src->gain * 32768.0f;
            const int16_t* ring = src->level[0];
            pcm.resize(2 * static_cast<size_t>(frames));
            for (int i = 0; i < frames; ++i) {
                int from = src->origin + i;
                if (from >= frames) from -= frames;
                for (int c = 0; c < 2; ++c) {
                    const float v = std::floor(static_cast<float>(ring[2 * static_cast<size_t>(from) + static_cast<size_t>(c)]) * scale + 0.5f);
                    pcm[2 * static_cast<size_t>(i) + static_cast<size_t>(c)] =
                        static_cast<int16_t>(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
                }
            }
        }

        // The folder: the SSD's root has to be there (a missing card isn't made up), the rest is ours.
        std::string ssd;
        for (const Root& r : sourceRoots())
            if (r.label == "ssd") ssd = r.dir;
        std::error_code ec;
        if (ssd.empty() || !fs::is_directory(ssd, ec)) return fail("no SSD");
        const std::string dir = ssd + "/" + kMemoriesFolder;
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec)) return fail("cannot make the Memories folder");

        std::lock_guard<std::mutex> lk(g_keepMtx);
        std::string path, name;
        const int n = claimMemory(dir, &path, &name, err);
        if (n == 0) return std::string();
        if (g_keepHook) g_keepHook(g_keepCtx, KS_WRITE);
        if (!writeWav(path, pcm.data(), frames, err)) {
            ::unlink(path.c_str());   // the claim: nothing is left behind
            return std::string();
        }
        writeFileAtomic(dir + "/.last", std::to_string(n) + "\n");
        rescanSources();
        return std::string("ssd:") + kMemoriesFolder + "/" + name;
    } catch (...) {   // out of memory
        return fail("out of memory");
    }
}

} // namespace af
