// WAV I/O and sample-rate conversion. No external dependencies.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tts {

struct Audio {
    std::vector<float> samples;  // mono
    int sample_rate = 24000;
};

// Reads PCM (8/16/24/32-bit) or IEEE-float WAV; multi-channel is downmixed.
Audio read_wav(const std::string& path);

// Writes 16-bit PCM mono WAV.
void write_wav16(const std::string& path, const std::vector<float>& samples, int sample_rate);

// Windowed-sinc polyphase resample (Kaiser, ~equivalent to scipy.resample_poly).
std::vector<float> resample_poly(const std::vector<float>& in, int sr_from, int sr_to);

// Drops a hot opening syllable from a reference prompt (in place).
// Returns the number of milliseconds dropped (0 = untouched).
int trim_hot_onset(std::vector<float>& audio, int sample_rate);

}  // namespace tts
