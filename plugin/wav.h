// From PolyForce dsp/wavetable.cpp (61543b4), "WAV import", pf -> af: the RIFF walk, the sample formats and
// EXTENSIBLE's tag. Here the reader streams, resamples and refuses more kinds of bad file, and the
// writer is new.
#pragma once
// WAV files in and out: Weather's sources are read from the SSD (plugin/sources.h) and Memory's Keep
// writes its remembered 16 s there. Both run on the loader thread: they block on the disk, allocate,
// and are never called from the audio thread.
//
// readWav(): made to stream (a source is minutes of audio, a wavetable a few MB), to resample, and to
// refuse more kinds of bad file than PolyForce's import.
// - Formats: PCM 16, 24 or 32 bit, or 32-bit float; also as WAVE_FORMAT_EXTENSIBLE. 1 to 8 channels,
//   the first two kept (a mono file is both sides). A block align over the samples' bytes (a container
//   wider than its samples) gives each channel a slot of align / channels bytes, the second after the
//   first. Rates 1000 to 384000 Hz. Anything else fails with a reason in *err.
// - What is trusted: nothing in the header but the chunk walk. The RIFF size is ignored. A chunk past
//   the end of the file ends the walk; at most 256 chunk headers are walked. The data runs for the
//   size its chunk says, and a data chunk that says more than the file holds is a truncated file and
//   fails ("truncated"), apart from the sizes a writer leaves when it doesn't know the length yet:
//   0xFFFFFFFF, and 0 when no chunk follows (with a LIST or id3 chunk after it, 0 is an empty data
//   chunk). Those run to the end of the file. A last partial frame is dropped.
// - What is kept: at most maxSeconds of the file (at its own rate), and the rest is not read. So the
//   audio held in memory is at most maxSeconds at 44.1 kHz, however large the file or its header
//   claims: the samples are decoded in blocks of 4096 frames and resampled as they come, so the
//   file's own rate never costs memory. Float samples that aren't finite are 0 and the rest are held
//   within +-1e6 (a NaN would otherwise run through the resampler into the grains).
// - The rate: a file at another rate than 44.1 kHz is resampled to it (WavData::rate is always 44100,
//   fileRate the file's own). Above 96 kHz the rate is first halved, as often as it takes to be no more
//   than 96 kHz (192 and 176.4 kHz once, 384 kHz twice), by halfband.h's decimator (a polyphase IIR,
//   flat to 0.227 of its input rate and 85 dB down from 0.273; its delay at low frequencies, 3.19 frames,
//   is made up);
//   32 taps alone leave what lies past 22 kHz folding into the band at those rates. The rest is a
//   windowed sinc of 32 taps (Kaiser, beta 7; 15 before the output's time, 16 after), 256 phases with the
//   taps interpolated between them. Its cutoff is 0.45 of the lower of the two rates (19.845 kHz), where
//   it is 6 dB down; the taps of each phase sum to 1, so DC passes at unity, and there is no delay.
//   Length: the frames of the file times 44100 / rate, rounded.
//   What it does, measured (the sources suite prints it). Gain at 15, 18, 19.845 and 21 kHz, by the
//   file's rate: 48 kHz 0.00, -0.61, -6.02, -14.83 dB; 88.2 and 176.4 kHz -0.10, -2.11, -6.02, -10.12;
//   96, 192 and 384 kHz -0.18, -2.32, -6.02, -9.72 (the sinc's transition is as wide in Hz as the rate it
//   works at is high: a file at 96 kHz and up is dulled from 15 kHz, one at 48 kHz from 17). Tones
//   that would fold into the band: from 96 kHz, 27 kHz and up are 73 dB down or more; from 192 and
//   384 kHz, 25 kHz lands at 19.1 kHz 37 dB down and 30 kHz and up 93 dB down or more. In time: a 15 kHz
//   tone (amplitude 0.5) from 48 kHz lands on the ideal one within 1e-4, a 3 kHz tone from 192 kHz
//   within 3e-4. A source (grains of weather, normalised to -20 dBFS RMS) can pay for the dulling.
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
