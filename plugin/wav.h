#pragma once
// WAV files in and out: Weather's sources are read from the SSD (plugin/sources.h) and Memory's Keep
// writes its remembered 16 s there. Both run on the loader thread: they block on the disk, allocate,
// and are never called from the audio thread.
//
// readWav(): the RIFF walk, the sample formats and EXTENSIBLE's tag are PolyForce's WAV import
// (dsp/wavetable.cpp, 61543b4), made to stream (a source is minutes of audio, a wavetable a few MB),
// to resample, and to refuse more kinds of bad file.
// - Formats: PCM 16, 24 or 32 bit, or 32-bit float; also as WAVE_FORMAT_EXTENSIBLE. 1 to 8 channels,
//   the first two kept (a mono file is both sides). Rates 1000 to 384000 Hz. Anything else fails with
//   a reason in *err.
// - What is trusted: nothing in the header but the chunk walk. The RIFF size is ignored. A chunk past
//   the end of the file ends the walk; at most 256 chunk headers are walked. The data runs for the
//   size its chunk says, and a data chunk that says more than the file holds is a truncated file and
//   fails ("truncated"), apart from the sizes a writer leaves when it doesn't know the length yet, 0
//   and 0xFFFFFFFF: those run to the end of the file. A last partial frame is dropped.
// - What is kept: at most maxSeconds of the file (at its own rate), and the rest is not read. So the
//   audio held in memory is at most maxSeconds at 44.1 kHz, however large the file or its header
//   claims: the samples are decoded in blocks of 4096 frames and resampled as they come, so the
//   file's own rate never costs memory. Float samples that aren't finite are 0 and the rest are held
//   within +-1e6 (a NaN would otherwise run through the resampler into the grains).
// - The rate: a file at another rate than 44.1 kHz is resampled to it (WavData::rate is always
//   44100, fileRate the file's own) by a windowed sinc of 32 taps (Kaiser, beta 7; 15 before the
//   output's time, 16 after), 256 phases with the taps interpolated between them. Its cutoff is 0.45
//   of the lower of the two rates (19.845 kHz), where it is 6 dB down; the taps of each phase sum to
//   1, so DC passes at unity, and there is no delay. Length: the frames of the file times 44100 / rate,
//   rounded. What 32 taps do, measured (the sources suite prints it): from 48 kHz, flat to 15 kHz
//   (0.00 dB), -0.05 dB at 17 kHz, -0.6 dB at 18 kHz, -6.0 dB at 19.8 kHz, -15 dB at 21 kHz, -57 dB
//   at 23 kHz; from 96 kHz, what would fold back under 22 kHz (27 kHz and up) is 73 dB down or more.
//   A 15 kHz tone (amplitude 0.5) lands on the ideal one within 1e-4. The last few kHz of a 48 kHz
//   file are dulled, a price a source (grains of weather, normalised to -20 dBFS RMS) can pay.
// - Only regular files are opened: a file is opened without waiting and looked at before it is
//   read, since a FIFO named *.wav would hold the loader thread for good.
//
// writeWav(): a 44-byte header and the samples, written next to the path and renamed into place
// (paths.h's writeFileAtomic, with its fsync of the file and its folder: a Force is often switched
// off hard), so a crash leaves either the old file or the whole new one, and a failure leaves
// nothing behind.
#include <cstdint>
#include <string>
#include <vector>

namespace af {

struct WavData {
    int rate = 0;          // l and r are at this rate: always 44100
    int channels = 0;      // the file's
    std::vector<float> l, r;   // r equals l for a mono file
    int fileRate = 0;      // the file's rate
};

// False with a reason in *err (if given) and `out` untouched on anything wrong with the file; never
// throws.
bool readWav(const std::string& path, WavData& out, float maxSeconds, std::string* err);

// 16-bit stereo at 44.1 kHz. `interleaved` is L R L R ..., `frames` of them (at least 1). The folder
// must exist; the file is replaced if it exists (the caller picks free names).
bool writeWav(const std::string& path, const int16_t* interleaved, int frames, std::string* err);

} // namespace af
