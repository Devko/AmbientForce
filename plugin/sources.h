#pragma once
// Weather's sources by key, never by index (docs/CONCEPT.md §9): a key is what state, presets and the
// stepper store, and survives files coming and going.
//
//   builtin:<field name>        a procedural field (dsp/fields.h)
//   memory:                     Memory's remembered 16 s (the engine's; nothing to load)
//   plugin:Weather/<file>.wav   in the plugin's folder
//   ssd:AmbientForce/Weather/<file>.wav, ssd:AmbientForce/Memories/<file>.wav   on the SSD
//
// Those three folders are the only places a file key may name, and only .wav (any case): a key that
// names anything else is refused, as one that climbs out of its root is (paths.h's resolveKey). The
// folders are walked to four levels (a file in a subfolder is listed under its path), links and
// hidden files skipped, at most 4096 files.
//
// UI and worker threads only: the audio thread never touches a key. The listing is a directory
// listing and no file is opened; reading a WAV and making it a source (loadSource) is the loader
// thread's work, as is Keep's write.
//
// The cache: a source is kept in SourceCache (loader.h) by its key, shared by every instance, and a
// file's with the size and time it had (one stat at each loadSource): a file replaced under its key
// is read again, and a file that has gone is Missing, cached or not. A slot that has the key loaded
// takes the new file at the next explicit pick of that key (Loader::want with now), not before.
#include "loader.h"

#include "../dsp/grainsrc.h"
#include "../dsp/memory.h"

#include <memory>
#include <string>
#include <vector>

namespace af {

constexpr const char* kMemoryKey = "memory:";
constexpr float kMaxWavSeconds = 60.0f;   // a WAV is read up to this much (18.5 MB as a source)

std::string defaultSourceKey();                  // builtin:Rain on Roof

// Every source in browsing order: the fields (their order), Memory, then the WAVs found by name
// (the file's stem, case-folded; the key breaks ties), scanned at most every 2 s: a file added shows
// within that. Keys of files that went away are not listed (state still holds them).
std::vector<std::string> sourceKeys();
void rescanSources();                            // forget the last scan: the next sourceKeys() looks again
std::string sourceName(const std::string& key);  // what the stepper shows: "Rain on Roof", "Memory", "Creek"

// The key's source, from the cache or made: a field rendered, a WAV read (kMaxWavSeconds of it) and
// made a source by buildSource. nullptr with *err for memory:, a key that names no source, a file
// that is missing or not a WAV we read, or too little memory to make it (never throws). Worker thread
// (it allocates and reads the disk).
std::shared_ptr<const SourceBuffer> loadSource(const std::string& key, std::string* err = nullptr);

// The loader's slot for Weather's source (worker thread): loadSource() per key, `info` the source's
// frames. No fallback: while nothing has loaded, or a key fails, live() is nullptr, and Weather is
// given none (it fades out). memory: loads nothing, so a slot wanting it is Missing with that said in
// its error: the plugin tells it from a real failure by the key.
Loader::SlotType sourceSlotType();

// What a slot's live() pointer is for Weather: the source it points at, nullptr for none.
inline const GrainSource* sourceOf(const void* live) {
    return live ? &static_cast<const SourceBuffer*>(live)->src : nullptr;
}

// Keep: the remembered Memory written as ssd:AmbientForce/Memories/Memory NNN.wav, 16-bit stereo at
// 44.1 kHz, from the oldest frame on, at the level the instrument played it (the ring holds it 6 dB
// down for headroom; the file has that back, clamped). NNN is one past the highest number ever used
// (001 the first): never a deleted file's, as the user presets do, so a project or preset that
// named one shows MISSING rather than playing another recording; the folder's hidden .last keeps
// the highest, and the name is claimed by creating it exclusively. Its key, or "" with *err:
// nothing remembered, no SSD (its root must exist: the folders under it are made), a folder or
// file that can't be written. Call it from the loader's thread (Loader::post): it reads and writes
// the disk. The ring is pinned while it is copied and let go before the write (the SSD's, which can
// take a second): a Remember is refused only for that copy, and the file is the ring as it was
// then. Keeps are one at a time across instances (the number is chosen under a lock). The new file
// is listed at once.
std::string keepMemory(Memory& memory, std::string* err);

// For tests: how many sources loadSource() has made (a WAV read, a field rendered) rather than found
// in the cache.
int sourcesMade();

// For tests: called by keepMemory at KS_COPY with the ring pinned, after the check that something
// is remembered and before it is read; at KS_NUMBERED when the number is chosen and before the file
// is claimed; and at KS_WRITE after the ring is let go, with the file claimed, just before it is
// written.
enum KeepStage : int { KS_COPY, KS_NUMBERED, KS_WRITE };
using KeepHook = void (*)(void* ctx, int stage);
void setKeepHook(KeepHook hook, void* ctx);

} // namespace af
