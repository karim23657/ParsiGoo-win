#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace tts {

static uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

Audio read_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("wav: cannot open " + path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("wav: not a RIFF/WAVE file: " + path);

    uint16_t fmt = 1, channels = 1, bits = 16;
    uint32_t rate = 24000;
    size_t data_off = 0, data_len = 0, pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= b.size()) {
        uint32_t csize = rd32(&b[pos + 4]);
        const uint8_t* id = &b[pos];
        size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0 && body + 16 <= b.size()) {
            fmt = rd16(&b[body]);
            channels = rd16(&b[body + 2]);
            rate = rd32(&b[body + 4]);
            bits = rd16(&b[body + 14]);
            if (fmt == 0xFFFE && body + 26 <= b.size()) fmt = rd16(&b[body + 24]);
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_off = body;
            data_len = std::min<size_t>(csize, b.size() - body);
        }
        pos = body + csize + (csize & 1);
    }
    if (!have_fmt || !data_off) throw std::runtime_error("wav: missing fmt/data chunk: " + path);
    if (channels < 1) throw std::runtime_error("wav: zero channels");

    int bytes = bits / 8;
    size_t frames = data_len / (bytes * channels);
    Audio out;
    out.sample_rate = (int)rate;
    out.samples.resize(frames);
    const uint8_t* p = &b[data_off];
    for (size_t i = 0; i < frames; i++) {
        double acc = 0;
        for (int c = 0; c < channels; c++) {
            const uint8_t* s = p + (i * channels + c) * bytes;
            double v = 0;
            if (fmt == 3) {
                if (bits == 32) { float t; std::memcpy(&t, s, 4); v = t; }
                else if (bits == 64) { double t; std::memcpy(&t, s, 8); v = t; }
            } else if (bits == 8) {
                v = ((int)s[0] - 128) / 128.0;
            } else if (bits == 16) {
                v = (int16_t)rd16(s) / 32768.0;
            } else if (bits == 24) {
                int32_t t = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16));
                if (t & 0x800000) t |= (int32_t)0xFF000000;
                v = t / 8388608.0;
            } else if (bits == 32) {
                v = (int32_t)rd32(s) / 2147483648.0;
            }
            acc += v;
        }
        out.samples[i] = (float)(acc / channels);
    }
    return out;
}

void write_wav16(const std::string& path, const std::vector<float>& samples, int sample_rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("wav: cannot write " + path);
    uint32_t data_len = (uint32_t)(samples.size() * 2);
    uint32_t riff_len = 36 + data_len;
    uint8_t hdr[44];
    std::memcpy(hdr, "RIFF", 4);
    hdr[4] = riff_len & 0xFF; hdr[5] = (riff_len >> 8) & 0xFF; hdr[6] = (riff_len >> 16) & 0xFF; hdr[7] = (riff_len >> 24) & 0xFF;
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;  // fmt chunk size 16
    hdr[20] = 1; hdr[21] = 0;                              // PCM
    hdr[22] = 1; hdr[23] = 0;                              // mono
    hdr[24] = sample_rate & 0xFF; hdr[25] = (sample_rate >> 8) & 0xFF; hdr[26] = (sample_rate >> 16) & 0xFF; hdr[27] = (sample_rate >> 24) & 0xFF;
    uint32_t byte_rate = (uint32_t)sample_rate * 2;
    hdr[28] = byte_rate & 0xFF; hdr[29] = (byte_rate >> 8) & 0xFF; hdr[30] = (byte_rate >> 16) & 0xFF; hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = 2; hdr[33] = 0;  // block align
    hdr[34] = 16; hdr[35] = 0; // bits
    std::memcpy(hdr + 36, "data", 4);
    hdr[40] = data_len & 0xFF; hdr[41] = (data_len >> 8) & 0xFF; hdr[42] = (data_len >> 16) & 0xFF; hdr[43] = (data_len >> 24) & 0xFF;
    f.write(reinterpret_cast<const char*>(hdr), 44);
    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); i++) {
        float v = samples[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)std::lround(v * 32767.0f);
    }
    f.write(reinterpret_cast<const char*>(pcm.data()), (std::streamsize)pcm.size() * 2);
}

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, y = x * x / 4.0;
    for (int k = 1; k < 40; k++) {
        term *= y / (double)(k * k);
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

// Kaizer-windowed sinc lowpass prototype, matching scipy.signal.resample_poly
// defaults (half_len=10, Kaiser beta=5.0), scaled by `up` for unit gain.
static std::vector<double> firwin_kaiser(int numtaps, double cutoff, double beta) {
    std::vector<double> h(numtaps);
    double center = (numtaps - 1) / 2.0;
    double denom = bessel_i0(beta);
    for (int i = 0; i < numtaps; i++) {
        double n = i - center;
        double sinc = (std::abs(n) < 1e-12) ? 2.0 * cutoff : std::sin(2.0 * M_PI * cutoff * n) / (M_PI * n);
        double r = 2.0 * n / (numtaps - 1);
        double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom;
        h[i] = sinc * w;
    }
    return h;
}

std::vector<float> resample_poly(const std::vector<float>& in, int sr_from, int sr_to) {
    if (sr_from == sr_to || in.empty()) return in;
    int g = (int)std::gcd(sr_from, sr_to);
    int up = sr_to / g, down = sr_from / g;
    int max_rate = std::max(up, down);
    int numtaps = 2 * 10 * max_rate + 1;
    std::vector<double> h = firwin_kaiser(numtaps, 1.0 / max_rate, 5.0);
    for (double& v : h) v *= up;

    long long in_len = (long long)in.size();
    long long out_len = (long long)std::ceil(((in_len - 1) * (long long)up + numtaps) / (double)down);
    std::vector<float> out(std::max<long long>(0, out_len), 0.0f);
    int hlen = numtaps;
    for (long long n = 0; n < out_len; n++) {
        long long base = n * down;
        int r = (int)(base % up);
        double acc = 0;
        // i = r + k*up ; x index = (base - i)/up
        for (int i = r; i < hlen; i += up) {
            long long xi = (base - i) / up;
            if (xi < 0 || xi >= in_len) continue;
            acc += h[i] * (double)in[(size_t)xi];
        }
        out[(size_t)n] = (float)acc;
    }
    return out;
}

static double median_copy(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t m = v.size() / 2;
    return (v.size() & 1) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

// Faithful port of trim_hot_onset: drops a hot opening syllable from a
// reference prompt. Returns dropped milliseconds (0 = untouched).
int trim_hot_onset(std::vector<float>& audio, int sample_rate) {
    int len = (int)audio.size();
    if (len < sample_rate) return 0;
    int h = std::max(1, (int)(0.01 * sample_rate));
    int nf = len / h;
    double peak = 0;
    for (float v : audio) peak = std::max(peak, (double)std::abs(v));
    if (nf < 8 || peak < 1e-6) return 0;
    std::vector<double> frames(nf);
    for (int i = 0; i < nf; i++) {
        double s = 0;
        for (int j = 0; j < h; j++) { double v = audio[i * h + j]; s += v * v; }
        frames[i] = std::sqrt(s / h);
    }
    std::vector<double> speech;
    int onset = -1;
    for (int i = 0; i < nf; i++) {
        if (frames[i] > 0.02 * peak) { if (onset < 0) onset = i; speech.push_back(frames[i]); }
    }
    if (speech.empty() || onset < 0) return 0;
    double typical = median_copy(speech);
    int alen = std::max(1, (int)(300 / 10));  // head_ms/10 = 30 frames
    double asum = 0; double amax = 0; int acount = 0;
    for (int i = onset; i < onset + alen && i < nf; i++) { asum += frames[i] * frames[i]; amax = std::max(amax, frames[i]); acount++; }
    double attack_rms = acount ? std::sqrt(asum / acount) : 0.0;
    double loud_enough = typical * std::pow(10.0, 4.0 / 20.0);
    bool hot = (attack_rms > loud_enough) || (peak >= 0.99 && amax > loud_enough);
    if (!hot) return 0;
    int drop = onset * h + (int)(0.3 * sample_rate);
    if (len - drop < (int)(1.5 * sample_rate)) return 0;
    audio.erase(audio.begin(), audio.begin() + drop);
    int f = std::max(1, (int)(0.02 * sample_rate));
    if ((int)audio.size() > 2 * f)
        for (int i = 0; i < f; i++) audio[i] *= (float)i / f;
    return (int)((long long)drop * 1000 / sample_rate);
}

}  // namespace tts
