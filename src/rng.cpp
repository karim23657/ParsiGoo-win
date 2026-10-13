#include "rng.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace tts {

Rng::Rng() {
    // default: seed from a fixed constant (callers pass an explicit seed when
    // they want reproducibility)
    state_ = 0x9E3779B97F4A7C15ULL;
}

uint64_t Rng::next_u64() {
    uint64_t x = state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state_ = x;
    return x * 0x2545F4914F6CDD1DULL;
}

std::vector<float> Rng::normal(int n) {
    std::vector<float> out(n);
    for (int i = 0; i < n; i += 2) {
        // Box-Muller with two uniforms in (0,1]
        double u1 = (double)(next_u64() >> 11) / (double)(1ULL << 53);
        double u2 = (double)(next_u64() >> 11) / (double)(1ULL << 53);
        if (u1 <= 0.0) u1 = 1e-12;
        double r = std::sqrt(-2.0 * std::log(u1));
        double theta = 2.0 * M_PI * u2;
        out[i] = (float)(r * std::cos(theta));
        if (i + 1 < n) out[i + 1] = (float)(r * std::sin(theta));
    }
    return out;
}

}  // namespace tts
