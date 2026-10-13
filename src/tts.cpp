#include "tts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <thread>

#include "audio.h"
#include "npz.h"

namespace tts {
namespace {

bool debug_flow() {
    static int v = -1;
    if (v < 0) v = std::getenv("FARSI_DEBUG_FLOW") ? 1 : 0;
    return v == 1;
}

double json_num(const std::string& s, const std::string& key, double def) {
    auto p = s.find("\"" + key + "\"");
    if (p == std::string::npos) return def;
    p = s.find(':', p);
    if (p == std::string::npos) return def;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    size_t q = p;
    while (q < s.size() && (std::isdigit((unsigned char)s[q]) || s[q] == '-' || s[q] == '+' || s[q] == '.' || s[q] == 'e' || s[q] == 'E')) q++;
    return std::stod(s.substr(p, q - p));
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double rms_of(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0;
    for (float x : v) s += (double)x * x;
    return std::sqrt(s / v.size());
}

uint64_t file_mtime(const std::string& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    return (uint64_t)t.time_since_epoch().count();
}

// scatter_axis2: dst [A,B,Cap,C,D] <- src [A,B,S,C,D] at time offset.
void scatter_axis2_f(float* dst, int64_t A, int64_t B, int64_t Cap, int64_t C, int64_t D,
                     const float* src, int64_t S, int64_t offset) {
    int64_t per = C * D;
    for (int64_t a = 0; a < A; a++)
        for (int64_t b = 0; b < B; b++) {
            float* d = dst + ((a * B + b) * Cap + offset) * per;
            const float* s = src + (a * B + b) * S * per;
            std::memcpy(d, s, (size_t)(S * per) * sizeof(float));
        }
}

void scatter_axis2(std::vector<float>& dst, int64_t A, int64_t B, int64_t Cap, int64_t C, int64_t D,
                   const float* src, int64_t S, int64_t offset) {
    scatter_axis2_f(dst.data(), A, B, Cap, C, D, src, S, offset);
}

void* ptr_or_dummy(std::vector<uint8_t>& d) {
    static uint8_t dummy[64];
    return d.empty() ? (void*)dummy : (void*)d.data();
}

}  // namespace

Engine::Engine(const EngineConfig& cfg) : cfg_(cfg) {
    if (cfg_.intra_threads <= 0) {
        // The FlowLM and Mimi steps are small, latency-bound matmuls, so they
        // scale with cores; the cap keeps ORT's synchronisation overhead sane.
        unsigned hc = std::thread::hardware_concurrency();
        cfg_.intra_threads = (int)std::min<unsigned>(std::max(hc, 2u), 8u);
    }
    base_dir_ = cfg.base_dir;
    onnx_dir_ = base_dir_ + "/model/onnx";
    v2_dir_ = base_dir_ + "/model/v2";

    std::string man = read_file(onnx_dir_ + "/manifest.json");
    ldim_ = (int64_t)json_num(man, "ldim", 32);
    dim_ = (int64_t)json_num(man, "dim", 1024);
    L_ = (int64_t)json_num(man, "layers", 6);
    H_ = (int64_t)json_num(man, "heads", 16);
    D_ = (int64_t)json_num(man, "dim_per_head", 64);
    CAP_ = (int64_t)json_num(man, "cache_capacity", 384);
    sample_rate_ = (int64_t)json_num(man, "sample_rate", 24000);
    steps_per_latent_ = (int64_t)json_num(man, "mimi_steps_per_latent", 16);
    temp_ = json_num(man, "temp", 0.3);
    tps_est_ = json_num(man, "tokens_per_second_estimate", 3.0);
    gen_pad_ = json_num(man, "gen_seconds_padding", 2.0);
    frame_rate_ = json_num(man, "frame_rate", 12.5);

    Npz w = load_npz(onnx_dir_ + "/weights.npz");
    lut_ = std::vector<float>(w["lut_weight"].f32(), w["lut_weight"].f32() + w["lut_weight"].numel());
    spk_proj_ = std::vector<float>(w["speaker_proj"].f32(), w["speaker_proj"].f32() + w["speaker_proj"].numel());
    bos_voice_ = std::vector<float>(w["bos_before_voice"].f32(), w["bos_before_voice"].f32() + w["bos_before_voice"].numel());
    emb_std_ = std::vector<float>(w["emb_std"].f32(), w["emb_std"].f32() + w["emb_std"].numel());
    emb_mean_ = std::vector<float>(w["emb_mean"].f32(), w["emb_mean"].f32() + w["emb_mean"].numel());
    if ((int64_t)bos_voice_.size() != dim_) throw std::runtime_error("bos_before_voice size mismatch");
    if ((int64_t)emb_std_.size() != ldim_ || (int64_t)emb_mean_.size() != ldim_)
        throw std::runtime_error("emb stats size mismatch");

    Npz init = load_npz(onnx_dir_ + "/decode_state_init.npz");
    for (size_t i = 0; i < init.size(); i++) {
        const Tensor& t = init.at(i);
        State s;
        s.shape = t.shape;
        if (t.dtype == DType::F32) s.type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        else if (t.dtype == DType::I64) s.type = ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64;
        else if (t.dtype == DType::BOOL) s.type = ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL;
        else throw std::runtime_error("decoder state has unsupported dtype");
        s.data = t.data;
        dec_init_.push_back(std::move(s));
        bool is_cache = init.names[i].size() >= 6 &&
                        init.names[i].compare(init.names[i].size() - 6, 6, ".cache") == 0;
        if (is_cache) cache_slots_.push_back((int)i);
        else small_slots_.push_back((int)i);
    }

    sp_.reset(new Spm(v2_dir_ + "/tokenizer_ph.model"));
    g2p_.reset(new G2P(onnx("g2p_encoder.onnx"), onnx("g2p_decoder.onnx")));
    // pronunciation dictionary: <base>/dicts/*.dict (optional)
    {
        size_t n = dict_.load_dir(dict_dir());
        if (n) fprintf(stderr, "dictionary: %zu entries from %s\n", n, dict_dir().c_str());
    }
    s_flow_.reset(new Session(onnx("flow_lm_step.onnx"), cfg.intra_threads, cfg.inter_threads));
    s_enc_.reset(new Session(onnx("mimi_encoder.onnx"), cfg.intra_threads, cfg.inter_threads));
    s_dec_.reset(new Session(onnx("mimi_decoder_step_kv.onnx"), cfg.intra_threads, cfg.inter_threads));

    const auto& din = s_dec_->in_names();
    if ((int)din.size() != (int)dec_init_.size() + 1)
        throw std::runtime_error("mimi decoder input count mismatch (" + std::to_string(din.size()) +
                                 " inputs vs " + std::to_string(dec_init_.size()) + " states)");
    const auto& dout = s_dec_->out_names();
    for (size_t i = 0; i < dout.size(); i++) {
        if (dout[i] == "audio") continue;
        if (dout[i].size() && dout[i].back() == 'o') out_small_idx_.push_back((int)i);
        else if (dout[i].rfind("kv", 0) == 0) out_kv_idx_.push_back((int)i);
    }
    if (out_small_idx_.size() != small_slots_.size() || out_kv_idx_.size() != cache_slots_.size())
        throw std::runtime_error("decoder output/state mapping mismatch");

    rng_ = cfg.seed_set ? Rng(cfg.seed) : Rng();
}

// ---------------------------------------------------------------------------
void Engine::flow_step(const float* seq, int64_t seq_len, const float* text_emb, int64_t text_len,
                       int64_t& offset, const float* noise, std::vector<float>& cache,
                       std::vector<float>& latent_out, float& eos_out, int64_t& new_S) {
    int64_t S = seq_len + text_len;
    if (offset + S > CAP_)
        throw std::runtime_error("flow cache overflow (chunk too long for one generation pass)");
    int64_t start = offset;
    static float dummy = 0;
    std::vector<int64_t> sshape = {1, seq_len, ldim_};
    std::vector<int64_t> tshape = {1, text_len, dim_};
    std::vector<int64_t> nshape = {1, ldim_};
    std::vector<int64_t> cshape = {L_, 2, CAP_, H_, D_};
    std::vector<int64_t> oshape = {};
    int64_t off = start;
    OrtValue* i_seq = make_tensor(sshape, seq_len ? (void*)seq : (void*)&dummy, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue* i_text = make_tensor(tshape, text_len ? (void*)text_emb : (void*)&dummy, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue* i_off = make_tensor(oshape, &off, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    OrtValue* i_noise = make_tensor(nshape, (void*)noise, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue* i_cache = make_tensor(cshape, cache.data(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    std::vector<const char*> names = {"sequence", "text_emb", "offset", "noise", "cache"};
    std::vector<OrtValue*> feeds = {i_seq, i_text, i_off, i_noise, i_cache};
    std::vector<OrtValue*> outs = s_flow_->run(names, feeds);
    ort_release(i_seq); ort_release(i_text); ort_release(i_off); ort_release(i_noise); ort_release(i_cache);

    const float* lat = (const float*)tensor_data(outs[0]);
    latent_out.assign(lat, lat + ldim_);
    const float* eos = (const float*)tensor_data(outs[1]);
    eos_out = eos[0];
    std::vector<int64_t> kshape = tensor_shape(outs[2]);
    new_S = kshape[2];
    const float* kv = (const float*)tensor_data(outs[2]);
    if (debug_flow()) fprintf(stderr, "[dbg] kv shape=%lld,%lld,%lld,%lld,%lld kv[0]=%.6f\n",
                              (long long)kshape[0], (long long)kshape[1], (long long)kshape[2],
                              (long long)kshape[3], (long long)kshape[4], kv[0]);
    scatter_axis2(cache, L_, 2, CAP_, H_, D_, kv, new_S, start);
    offset = start + new_S;
    for (auto* o : outs) ort_release(o);
}

// ---------------------------------------------------------------------------
Engine::VoiceCache& Engine::voice_cache(const std::string& wav_path) {
    uint64_t mt = file_mtime(wav_path);
    auto it = voice_memo_.find(wav_path);
    if (it != voice_memo_.end() && it->second->mtime == mt) return *it->second;

    Audio a = read_wav(wav_path);
    if (a.sample_rate != (int)sample_rate_) a.samples = resample_poly(a.samples, a.sample_rate, (int)sample_rate_);
    if ((int64_t)a.samples.size() > 5 * sample_rate_) a.samples.resize(5 * sample_rate_);
    int dropped = trim_hot_onset(a.samples, (int)sample_rate_);
    if (dropped) fprintf(stderr, "voice prompt: dropped the first %d ms (hot opening syllable)\n", dropped);

    // Loudness safety net. The prompt's level leaks straight into the clone:
    // measured with a fixed text and seed, a -40 dB reference produced -37 dB
    // speech and a silent reference produced silence — the flow LM is being
    // conditioned on an embedding far below the level it was trained with.
    // A normal prompt is left byte-identical (voices/male_hello.wav sits at
    // rms 0.11; anything within 6x of that passes through untouched, which is
    // what keeps the reference verification exact); only a genuinely quiet
    // one is amplified back to that reference level, never enough to clip.
    double prompt_rms = rms_of(a.samples);
    float prompt_gain = 1.0f;
    if (prompt_rms < 1e-4) {
        fprintf(stderr, "voice prompt: no speech energy (rms %.2g) - the clone will be silent: %s\n",
                prompt_rms, wav_path.c_str());
    } else if (prompt_rms < 0.10 / 6.0) {
        double peak = 0;
        for (float v : a.samples) peak = std::max(peak, (double)std::fabs(v));
        double g = 0.10 / prompt_rms;  // 0.10 = the level of voices/male_hello.wav
        if (peak > 1e-9 && g * peak > 0.95) g = 0.95 / peak;  // never clip
        if (g > 256) g = 256;  // ...and never turn a digital-noise floor into speech
        prompt_gain = (float)g;
        for (float& v : a.samples) v = (float)(v * g);
        fprintf(stderr, "voice prompt: quiet (rms %.4f), amplified %.1fx to the reference level\n",
                prompt_rms, g);
    }

    std::vector<int64_t> ashape = {1, 1, (int64_t)a.samples.size()};
    OrtValue* ia = make_tensor(ashape, a.samples.data(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    std::vector<const char*> names = {"audio"};
    std::vector<OrtValue*> feeds = {ia};
    std::vector<OrtValue*> outs = s_enc_->run(names, feeds);
    ort_release(ia);
    std::vector<int64_t> lshape = tensor_shape(outs[0]);  // [1,Tn,ldim]
    int64_t Tn = lshape[1];
    const float* lat = (const float*)tensor_data(outs[0]);

    std::vector<float> text_emb((size_t)(Tn + 1) * dim_);
    std::memcpy(text_emb.data(), bos_voice_.data(), dim_ * sizeof(float));
    for (int64_t i = 0; i < Tn; i++)
        for (int64_t j = 0; j < dim_; j++) {
            double acc = 0;
            for (int64_t k = 0; k < ldim_; k++) acc += (double)lat[i * ldim_ + k] * spk_proj_[j * ldim_ + k];
            text_emb[(size_t)(i + 1) * dim_ + j] = (float)acc;
        }
    ort_release(outs[0]);

    if (const char* te = std::getenv("FARSI_DUMP_TEXTEMB")) {
        std::ofstream f(te, std::ios::binary);
        f.write(reinterpret_cast<const char*>(text_emb.data()), (std::streamsize)text_emb.size() * 4);
    }
    auto vc = std::make_shared<VoiceCache>();
    vc->path = wav_path;
    vc->mtime = mt;
    vc->prompt_rms = (float)prompt_rms;
    vc->prompt_gain = prompt_gain;
    vc->kv.assign((size_t)(L_ * 2 * CAP_ * H_ * D_), 0.0f);
    std::vector<float> latent;
    std::vector<float> noise(ldim_, 0.0f);
    float eos = 0;
    int64_t newS = 0;
    int64_t off0 = 0;
    flow_step(nullptr, 0, text_emb.data(), Tn + 1, off0, noise.data(), vc->kv, latent, eos, newS);
    vc->off = newS;
    // bound the cache (~12.6 MB per voice); never evict the one being returned
    voice_memo_[wav_path] = vc;
    while (voice_memo_.size() > 4) {
        auto it2 = voice_memo_.begin();
        if (it2->first == wav_path) it2++;
        if (it2 == voice_memo_.end()) break;
        voice_memo_.erase(it2);
    }
    if (debug_flow()) {
        double csum = 0;
        for (size_t i = dim_; i < text_emb.size(); i++) csum += std::abs(text_emb[i]);
        fprintf(stderr, "[dbg] voice Tn=%lld off=%lld cond0=%.6f %.6f cond_sum=%.3f\n", (long long)Tn, (long long)vc->off,
                text_emb[dim_], text_emb[dim_ + 1], csum);
    }
    return *vc;
}

// ---------------------------------------------------------------------------
std::vector<float> Engine::decode_all(const std::vector<float>& latents, int n_steps) {
    std::vector<State> st = dec_init_;
    std::vector<float> audio;
    int64_t off = 0;
    std::vector<int64_t> lshape = {1, 1, ldim_};
    int64_t cap = 0;
    for (int j : cache_slots_)
        if (dec_init_[j].shape.size() > 2) cap = std::max(cap, dec_init_[j].shape[2]);
    for (int s = 0; s < n_steps; s++) {
        if (cap > 0 && off + steps_per_latent_ > cap) break;  // state cache full
        float un[32];
        for (int64_t k = 0; k < ldim_; k++) un[k] = latents[(size_t)s * ldim_ + k] * emb_std_[k] + emb_mean_[k];
        OrtValue* i_lat = make_tensor(lshape, un, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        std::vector<OrtValue*> feeds;
        feeds.push_back(i_lat);
        for (auto& state : st) feeds.push_back(make_tensor(state.shape, ptr_or_dummy(state.data), state.type));
        std::vector<OrtValue*> outs = s_dec_->run_ordered(feeds);
        for (size_t i = 1; i < feeds.size(); i++) ort_release(feeds[i]);
        ort_release(i_lat);

        const float* au = (const float*)tensor_data(outs[0]);
        std::vector<int64_t> ash = tensor_shape(outs[0]);
        int64_t n = 1;
        for (auto d : ash) n *= d;
        audio.insert(audio.end(), au, au + n);

        for (size_t j = 0; j < out_small_idx_.size(); j++) {
            OrtValue* o = outs[out_small_idx_[j]];
            State& dst = st[small_slots_[j]];
            std::vector<int64_t> sh = tensor_shape(o);
            size_t bytes = 1;
            for (auto d : sh) bytes *= (size_t)d;
            size_t elem = (tensor_type(o) == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) ? 8 : (tensor_type(o) == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? 4 : 1);
            const void* p = tensor_data(o);
            dst.shape = sh;
            dst.type = tensor_type(o);
            dst.data.assign((const uint8_t*)p, (const uint8_t*)p + bytes * elem);
        }
        for (size_t j = 0; j < out_kv_idx_.size(); j++) {
            OrtValue* o = outs[out_kv_idx_[j]];
            State& dst = st[cache_slots_[j]];
            std::vector<int64_t> sh = tensor_shape(o);  // [A,B,S,C,D]
            const float* p = (const float*)tensor_data(o);
            int64_t A = dst.shape.size() > 0 ? dst.shape[0] : 2;
            int64_t B = dst.shape.size() > 1 ? dst.shape[1] : 1;
            int64_t Cap = dst.shape.size() > 2 ? dst.shape[2] : sh[2];
            int64_t C = dst.shape.size() > 3 ? dst.shape[3] : sh[3];
            int64_t D = dst.shape.size() > 4 ? dst.shape[4] : sh[4];
            scatter_axis2_f(reinterpret_cast<float*>(dst.data.data()), A, B, Cap, C, D, p, sh[2], off);
        }
        off += steps_per_latent_;
        for (auto* o : outs) ort_release(o);
    }
    return audio;
}

// ---------------------------------------------------------------------------
std::vector<float> Engine::generate_chunk(const VoiceCache& vc, const std::string& chunk, Rng& rng, double temp,
                                           bool zero_noise) {
    bool znoise = zero_noise || cfg_.zero_noise;
    std::vector<int> tokens = sp_->encode(chunk);
    size_t nt = tokens.size();
    std::vector<float> text_emb(nt * dim_);
    for (size_t i = 0; i < nt; i++) {
        int id = tokens[i];
        if (id < 0 || (size_t)id * dim_ + dim_ > lut_.size()) throw std::runtime_error("token id out of LUT range");
        std::memcpy(&text_emb[i * dim_], &lut_[(size_t)id * dim_], dim_ * sizeof(float));
    }
    std::vector<float> cache = vc.kv;
    int64_t off = vc.off;
    std::vector<float> latent;
    float eos = 0;
    int64_t newS = 0;
    std::vector<float> noise = znoise ? std::vector<float>(ldim_, 0.0f) : rng.normal((int)ldim_);
    for (auto& v : noise) v *= (float)std::sqrt(temp);
    std::vector<float> bos(ldim_, NAN);
    if (debug_flow()) {
        fprintf(stderr, "[dbg] tokens:");
        for (int t : tokens) fprintf(stderr, " %d", t);
        double cs = 0;
        for (float v : cache) cs += std::abs(v);
        fprintf(stderr, "\n[dbg] te0=%.6f %.6f %.6f cache_sum=%.1f\n", text_emb[0], text_emb[1], text_emb[2], cs);
    }
    flow_step(bos.data(), 1, text_emb.data(), (int64_t)nt, off, noise.data(), cache, latent, eos, newS);
    std::vector<float> latents = latent;
    if (debug_flow()) {
        fprintf(stderr, "[dbg] bos nt=%zu off=%lld eos=%.4f lat0=%.5f %.5f %.5f\n", nt, (long long)off, eos,
                latent[0], latent[1], latent[2]);
    }

    int words = 1;
    {
        size_t c = 0;
        for (char ch : chunk) if (ch == ' ') c++;
        words = (int)c + 1;
    }
    int frames_after_eos = (words <= 4 ? 3 : 1) + 2;
    int max_gen_len = (int)std::ceil((nt / tps_est_ + gen_pad_) * frame_rate_);
    int eos_step = -1;
    for (int step = 0; step < max_gen_len; step++) {
        noise = znoise ? std::vector<float>(ldim_, 0.0f) : rng.normal((int)ldim_);
        for (auto& v : noise) v *= (float)std::sqrt(temp);
        flow_step(latents.data() + latents.size() - ldim_, 1, nullptr, 0, off, noise.data(), cache, latent, eos, newS);
        if (debug_flow()) fprintf(stderr, "[dbg] step %d eos=%.4f lat0=%.5f\n", step, eos, latent[0]);
        if (eos != 0.0f && eos_step < 0) eos_step = step;
        if (eos_step >= 0 && step >= eos_step + frames_after_eos) break;
        latents.insert(latents.end(), latent.begin(), latent.end());
    }
    return latents;
}

// ---------------------------------------------------------------------------
void Engine::window_mean(const std::vector<float>& x, int w, std::vector<float>& out) {
    int n = (int)x.size();
    out.assign(n, 0.0f);
    if (n == 0) return;
    std::vector<double> c(n + 1, 0.0);
    for (int i = 0; i < n; i++) c[i + 1] = c[i] + x[i];
    int widen = n + w - 1;
    std::vector<double> full(widen);
    for (int m = 0; m < widen; m++) {
        long long lo = std::max<long long>(m - w + 1, 0);
        long long hi = std::min<long long>(m + 1, n);
        full[m] = (c[hi] - c[lo]) / w;
    }
    int start = (w - 1) / 2;
    for (int i = 0; i < n; i++) out[i] = (float)full[start + i];
}

std::vector<std::pair<int, int>> Engine::dead_air_regions(const std::vector<float>& p, double rel_floor,
                                                          double win, double density, int sr) {
    std::vector<std::pair<int, int>> out;
    int n = (int)p.size();
    if (n < 3) return out;
    double rms = rms_of(p);
    if (rms < 1e-6) return out;
    std::vector<float> quiet(n);
    for (int i = 0; i < n; i++) quiet[i] = (std::abs(p[i]) < rel_floor * rms) ? 1.0f : 0.0f;
    int w = std::max(1, (int)(win * sr));
    std::vector<float> dens;
    window_mean(quiet, w, dens);
    std::vector<int> starts, ends;
    int prev = 0;
    for (int i = 0; i < n; i++) {
        int s = dens[i] >= density ? 1 : 0;
        if (s && !prev) starts.push_back(i);
        if (!s && prev) ends.push_back(i);
        prev = s;
    }
    if (prev) ends.push_back(n);
    for (size_t k = 0; k < starts.size(); k++) {
        int i = starts[k], j = ends[k];
        while (i > 0 && quiet[i - 1] > 0.5f) i--;
        while (j < n && quiet[j] > 0.5f) j++;
        out.push_back({i, j});
    }
    std::vector<std::pair<int, int>> merged;
    for (auto& r : out) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    return merged;
}

double Engine::solid_fraction(const std::vector<float>& speech, double floor, double win, int sr) {
    if (speech.size() < 3) return 1.0;
    int w = std::max(1, (int)(win * sr));
    int n = (int)speech.size() / w;
    if (n == 0) return 1.0;
    int solid = 0;
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int j = 0; j < w; j++) { double v = speech[i * w + j]; s += v * v; }
        if (std::sqrt(s / w) >= floor) solid++;
    }
    return (double)solid / n;
}

std::pair<int, int> Engine::speech_bounds(const std::vector<float>& p, int sr) {
    int n = (int)p.size();
    double rms = rms_of(p);
    if (rms < 1e-6) return {0, n};
    std::vector<float> loud(n);
    for (int i = 0; i < n; i++) loud[i] = (std::abs(p[i]) >= 0.03 * rms) ? 1.0f : 0.0f;
    int w = std::max(1, (int)(0.03 * sr));
    std::vector<float> dens;
    window_mean(loud, 2 * w, dens);
    int first = -1, last = -1;
    for (int i = 0; i < n; i++) if (dens[i] >= 0.6f) { if (first < 0) first = i; last = i; }
    if (first < 0) return {0, n};
    int start = std::max(0, first - (int)(0.02 * sr));
    int end = std::min(n, last + (int)(0.06 * sr));
    return {start, end};
}

std::vector<float> Engine::compress_pauses(const std::vector<float>& audio, double max_pause, double keep, double rel_floor) {
    auto regions = dead_air_regions(audio, rel_floor, 0.25, 0.90, (int)sample_rate_);
    std::vector<std::pair<int, int>> keepr;
    for (auto& r : regions)
        if ((r.second - r.first) > (int)(max_pause * sample_rate_)) keepr.push_back(r);
    if (keepr.empty()) return audio;
    int sr = (int)sample_rate_;
    int f = std::max(1, (int)(0.008 * sr));
    std::vector<float> out;
    int pos = 0;
    for (auto& r : keepr) {
        out.insert(out.end(), audio.begin() + pos, audio.begin() + r.first);
        int a = r.first;
        int b = std::min(a + (int)(keep * sr), r.second);
        std::vector<float> seg(audio.begin() + a, audio.begin() + b);
        if ((int)seg.size() > 2 * f)
            for (int i = 0; i < f; i++) seg[seg.size() - f + i] *= (float)(1.0 - (double)i / f);
        out.insert(out.end(), seg.begin(), seg.end());
        int nb = r.second;
        if (nb + f <= (int)audio.size()) {
            for (int i = 0; i < f; i++) out.push_back(audio[nb + i] * (float)((double)i / f));
        }
        pos = nb + f;
        if (pos > (int)audio.size()) pos = (int)audio.size();
    }
    out.insert(out.end(), audio.begin() + pos, audio.end());
    return out;
}

// ---------------------------------------------------------------------------
std::vector<float> Engine::generate_with_retry(const VoiceCache& vc, const std::string& chunk, Rng& rng,
                                               double temp, int retries, bool zero_noise, bool* quiet) {
    int tokens = (int)sp_->encode(chunk).size();
    double expected = tokens / tps_est_;
    std::vector<float> best;
    // ranking keys of the current best — the tuple below is compared as a whole
    double k_loud = -1, k_dur = -1, k_rms = -1, k_solid = -1, k_dead = 1e9;
    bool have_best = false, accepted = false;
    if (retries < 1) retries = 1;
    if (retries > 4) retries = 4;
    for (int attempt = 0; attempt < retries; attempt++) {
        // Attempt 0 runs at exactly the requested temperature (that is the
        // reference pipeline, and the only attempt verification sees). Later
        // attempts warm the sampler up: at the model default of 0.3 sampling
        // is nearly greedy, so a fresh draw of noise reproduced the very same
        // — possibly silent — chunk every time and the retry loop bought
        // nothing at all.
        double t = (attempt == 0) ? temp : std::min(1.0, temp + 0.25 * attempt);
        std::vector<float> latents = generate_chunk(vc, chunk, rng, t, zero_noise);
        std::vector<float> audio = decode_all(latents, (int)(latents.size() / ldim_));
        auto bounds = speech_bounds(audio, (int)sample_rate_);
        std::vector<float> speech(audio.begin() + bounds.first, audio.begin() + bounds.second);
        speech = compress_pauses(speech, 0.30, 0.12, 0.03);
        double dur = speech.size() / (double)sample_rate_;
        double rms = rms_of(speech);
        bool loud_ok = rms >= 0.02;
        double frac = solid_fraction(speech, 0.05, 0.04, (int)sample_rate_);
        double solid_dur = frac * dur;
        bool dur_ok = (0.30 * expected <= solid_dur) && (solid_dur <= 1.6 * expected);
        auto regions = dead_air_regions(speech, 0.03, 0.25, 0.90, (int)sample_rate_);
        double dead = 0;
        for (auto& r : regions) dead = std::max(dead, (r.second - r.first) / (double)sample_rate_);
        double key_loud = loud_ok ? 1 : 0, key_dur = dur_ok ? 1 : 0, key_dead = (dead <= 0.35) ? 0 : -dead;
        if (getenv("FARSI_GATE_DEBUG")) {
            fprintf(stderr, "[gate] attempt=%d temp=%.2f rms=%.5f loud_ok=%d dur_ok=%d expected=%.2f solid=%.2f frac=%.2f dead=%.3f\n",
                    attempt, t, rms, (int)loud_ok, (int)dur_ok, expected, solid_dur, frac, dead);
        }
        // Accepted attempts rank first; among attempts that all failed the
        // gate the deciding key is the REAL loudness — without it every
        // failure tied and the first (often the quietest) attempt was kept.
        auto key = std::make_tuple(key_loud, key_dur, rms, solid_dur, key_dead);
        bool better = !have_best ||
                      key > std::make_tuple(k_loud, k_dur, k_rms, k_solid, k_dead);
        if (better) {
            best = speech;
            k_loud = key_loud; k_dur = key_dur; k_rms = rms; k_solid = solid_dur; k_dead = key_dead;
            have_best = true;
        }
        if (loud_ok && dur_ok && frac >= 0.55 && dead <= 0.35) { accepted = true; break; }
    }
    if (quiet) *quiet = !accepted;
    if (getenv("FARSI_GATE_DEBUG"))
        fprintf(stderr, "[gate] chosen rms=%.5f accepted=%d over %zu samples\n",
                best.empty() ? 0.0 : rms_of(best), (int)accepted, best.size());
    return best;
}

std::vector<float> Engine::stitch(const std::vector<std::pair<std::vector<float>, double>>& segments) {
    std::vector<float> empty;
    if (segments.empty()) return empty;
    std::vector<double> levels;
    for (auto& s : segments) if (!s.first.empty()) levels.push_back(rms_of(s.first));
    double target = 0;
    if (!levels.empty()) {
        std::sort(levels.begin(), levels.end());
        size_t m = levels.size() / 2;
        target = (levels.size() & 1) ? levels[m] : 0.5 * (levels[m - 1] + levels[m]);
    }
    int f = std::max(1, (int)(0.008 * sample_rate_));
    std::vector<float> out;
    for (auto& seg : segments) {
        std::vector<float> p = seg.first;
        double rms = rms_of(p);
        double g_dbg = 1.0;
        if (rms > 1e-6 && target > 1e-6) {
            g_dbg = std::min(2.5, std::max(0.4, target / rms));
            for (auto& v : p) v *= (float)g_dbg;
        }
        if (getenv("FARSI_GATE_DEBUG"))
            fprintf(stderr, "[stitch] seg rms=%.5f gain=%.3f target=%.5f nseg=%zu gap=%.3f\n",
                    rms, g_dbg, target, segments.size(), seg.second);
        if ((int)p.size() > 2 * f) {
            for (int i = 0; i < f; i++) p[i] *= (float)((double)i / f);
            for (int i = 0; i < f; i++) p[p.size() - f + i] *= (float)(1.0 - (double)i / f);
        }
        if (seg.second > 0) out.insert(out.end(), (size_t)(seg.second * sample_rate_), 0.0f);
        out.insert(out.end(), p.begin(), p.end());
    }
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::abs(v));
    if (peak > 0.98f) for (auto& v : out) v *= (0.98f / peak);
    return compress_pauses(out, 0.38, 0.28, 0.03);
}

// ---------------------------------------------------------------------------
std::vector<float> Engine::synthesize_plan(const std::vector<Phrase>& plan, const std::string& voice_wav,
                                           const GenOptions& opt, std::vector<std::string>* chunks_out,
                                           int* total_out, int* quiet_out) {
    double pace = std::min(1.5, std::max(0.6, opt.pace));
    double pause = opt.pause_scale < 0 ? 0 : opt.pause_scale;
    double temp = opt.temperature > 0 ? opt.temperature : temp_;
    int retries = opt.retries < 1 ? 1 : (opt.retries > 4 ? 4 : opt.retries);
    int budget = opt.chunk_tokens < 8 ? 8 : (opt.chunk_tokens > 32 ? 32 : opt.chunk_tokens);
    VoiceCache& vc = voice_cache(voice_wav);
    struct Job { std::string chunk; double gap; };
    std::vector<Job> jobs;
    for (size_t pi = 0; pi < plan.size(); pi++) {
        std::vector<std::string> chunks = chunk_phonemes(plan[pi].phonemes, *sp_, budget);
        for (size_t ci = 0; ci < chunks.size(); ci++) {
            double gap;
            if (pi == 0 && ci == 0) gap = 0.0;
            else if (ci == 0) gap = plan[pi].gap;
            else gap = CHUNK_GAP;
            jobs.push_back({chunks[ci], gap * pause});
            if (chunks_out) chunks_out->push_back(chunks[ci]);
        }
    }
    std::vector<std::pair<std::vector<float>, double>> segments;
    for (auto& j : jobs) {
        bool quiet = false;
        segments.push_back({generate_with_retry(vc, j.chunk, rng_, temp, retries, opt.zero_noise, &quiet), j.gap});
        if (total_out) (*total_out)++;
        if (quiet && quiet_out) (*quiet_out)++;
    }
    std::vector<float> audio = stitch(segments);
    if (std::abs(pace - 1.0) >= 0.03 && !audio.empty()) {
        size_t out_len = (size_t)std::llround(audio.size() / pace);
        std::vector<float> res(out_len);
        for (size_t i = 0; i < out_len; i++) {
            double x = i * pace;
            size_t i0 = (size_t)x;
            double fr = x - i0;
            float a = audio[std::min(i0, audio.size() - 1)];
            float b = audio[std::min(i0 + 1, audio.size() - 1)];
            res[i] = (float)(a + (b - a) * fr);
        }
        audio.swap(res);
    }
    return audio;
}

// --- helpers ---------------------------------------------------------------
// Persian script present? (U+0600..U+06FF / U+0640.. / U+06C0..)
static bool looks_persian(const std::string& text) {
    for (size_t i = 0; i + 1 < text.size(); i++) {
        unsigned char a = text[i], b = text[i + 1];
        if (a == 0xD8 && b >= 0x80) return true;
        if (a == 0xD9 && b >= 0x80) return true;
        if (a == 0xDB && b >= 0x80) return true;
    }
    return false;
}

static void clamp_options(GenOptions& o) {
    if (o.mode != "pack") o.mode = "split";
    o.pace = std::min(1.5, std::max(0.6, o.pace));
    if (o.chunk_tokens <= 0) o.chunk_tokens = 18;
    if (o.chunk_tokens < 8) o.chunk_tokens = 8;
    if (o.chunk_tokens > 32) o.chunk_tokens = 32;
    if (o.pause_scale < 0) o.pause_scale = 0;
    if (o.pause_scale > 3) o.pause_scale = 3;
    if (o.retries <= 0) o.retries = 3;
    if (o.retries > 4) o.retries = 4;
    if (o.temperature > 2) o.temperature = 2;
}

std::vector<std::vector<Phrase>> Engine::build_plans(const std::string& input, const Dictionary* d,
                                                     const GenOptions& o) {
    std::vector<std::vector<Phrase>> plans;
    for (auto& sent : split_sentences(input)) {
        std::vector<Phrase> plan = plan_phrases(sent, *g2p_, *sp_, d, o.keep_ezafe, o.chunk_tokens);
        if (o.mode == "pack") plan = pack_phrases(plan);
        if (!plan.empty()) plans.push_back(plan);
    }
    return plans;
}

// The run's dictionary: the engine's, plus any per-run `opt.dict` entries on
// top (they win).
const Dictionary* Engine::run_dictionary(const GenOptions& opt, Dictionary& local) const {
    if (opt.dict.empty()) return &dict_;
    local = dict_;
    local.load_text(opt.dict);
    return &local;
}

// One warning per run: a chunk that never cleared the quality gate means part
// of the audio is silence, and nobody should have to guess why.
static void warn_quiet_chunks(const SynthResult& r) {
    if (r.quiet_chunks <= 0) return;
    fprintf(stderr,
            "warning: %d of %d chunk(s) stayed below the quality gate after every retry - "
            "that part of the audio may be silent or shorter than it should be "
            "(try another --seed, --retries 4, or check the input phonemes)\n",
            r.quiet_chunks, r.total_chunks);
}

SynthResult Engine::synthesize(const std::string& input, const std::string& voice_wav, const GenOptions& opt) {
    GenOptions o = opt;
    clamp_options(o);
    if (o.seed_set) rng_ = Rng(o.seed);

    Dictionary local;
    const Dictionary* d = run_dictionary(o, local);

    SynthResult r;
    bool persian = o.input_text || (!o.input_phonemes && looks_persian(input));
    r.from_phonemes = !persian;

    if (!persian) {
        // Romanized phoneme string — G2P skipped entirely.
        r.phonemes = input;
        std::vector<Phrase> plan = {{input, 0.0}};
        r.audio = synthesize_plan(plan, voice_wav, o, &r.chunks, &r.total_chunks, &r.quiet_chunks);
        warn_quiet_chunks(r);
        return r;
    }

    std::vector<std::vector<Phrase>> plans = build_plans(input, d, o);
    if (plans.empty()) return r;
    {
        std::string ph;
        for (auto& plan : plans)
            for (auto& p : plan) { if (!ph.empty()) ph += ' '; ph += p.phonemes; }
        r.phonemes = ph;
    }
    std::vector<float> parts;
    size_t silence_n = (size_t)(SENTENCE_GAP * o.pause_scale * sample_rate_);
    std::vector<float> silence(silence_n, 0.0f);
    for (size_t i = 0; i < plans.size(); i++) {
        std::vector<float> a = synthesize_plan(plans[i], voice_wav, o, &r.chunks, &r.total_chunks, &r.quiet_chunks);
        parts.insert(parts.end(), a.begin(), a.end());
        if (i + 1 < plans.size()) parts.insert(parts.end(), silence.begin(), silence.end());
    }
    r.audio = parts;
    warn_quiet_chunks(r);
    return r;
}

std::vector<std::string> Engine::plan_chunks(const std::string& input, const GenOptions& opt) {
    GenOptions o = opt;
    clamp_options(o);
    Dictionary local;
    const Dictionary* d = run_dictionary(o, local);
    std::vector<std::string> out;
    if (!o.input_text && (o.input_phonemes || !looks_persian(input)))
        return chunk_phonemes(input, *sp_, o.chunk_tokens);
    for (auto& plan : build_plans(input, d, o))
        for (auto& p : plan) {
            std::vector<std::string> c = chunk_phonemes(p.phonemes, *sp_, o.chunk_tokens);
            out.insert(out.end(), c.begin(), c.end());
        }
    return out;
}

std::vector<float> Engine::synthesize_phonemes(const std::string& phonemes, const std::string& voice_wav, double pace) {
    GenOptions o;
    o.input_phonemes = true;
    o.pace = pace;
    return synthesize(phonemes, voice_wav, o).audio;
}

std::vector<float> Engine::synthesize_text(const std::string& text, const std::string& voice_wav,
                                           const std::string& mode, double pace) {
    GenOptions o;
    o.mode = mode;
    o.pace = pace;
    return synthesize(text, voice_wav, o).audio;
}

// --- voice cache ----------------------------------------------------------
void Engine::preload_voice(const std::string& wav) { voice_cache(wav); }

bool Engine::voice_loaded(const std::string& wav) const {
    std::error_code ec;
    uint64_t mt = file_mtime(wav);
    if (!mt) return false;
    auto it = voice_memo_.find(wav);
    return it != voice_memo_.end() && it->second->mtime == mt;
}

bool Engine::voice_stats(const std::string& wav, float* rms, float* gain) const {
    uint64_t mt = file_mtime(wav);
    if (!mt) return false;
    auto it = voice_memo_.find(wav);
    if (it == voice_memo_.end() || it->second->mtime != mt) return false;
    if (rms) *rms = it->second->prompt_rms;
    if (gain) *gain = it->second->prompt_gain;
    return true;
}

std::vector<std::string> Engine::loaded_voices() const {
    std::vector<std::string> out;
    for (auto& kv : voice_memo_) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

bool Engine::unload_voice(const std::string& wav) {
    auto it = voice_memo_.find(wav);
    if (it == voice_memo_.end()) return false;
    voice_memo_.erase(it);
    return true;
}

int Engine::unload_all_voices() {
    int n = (int)voice_memo_.size();
    voice_memo_.clear();
    return n;
}

// --- pronunciation dictionary --------------------------------------------
std::string Engine::dict_dir() const { return base_dir_ + "/dicts"; }
std::string Engine::dict_path() const { return dict_dir() + "/user.dict"; }

void Engine::reload_dictionaries() {
    Dictionary d;
    d.load_dir(dict_dir());
    dict_ = d;
}

void Engine::debug_raw(const std::string& text, const std::string& voice_wav, const std::string& prefix) {
    std::string chunk;
    if (looks_persian(text)) {
        for (auto& sent : split_sentences(text)) {
            std::vector<Phrase> plan = plan_phrases(sent, *g2p_, *sp_, &dict_, false, 18);
            if (plan.empty()) continue;
            std::vector<std::string> ch = chunk_phonemes(plan[0].phonemes, *sp_, 18);
            if (!ch.empty()) { chunk = ch[0]; break; }
        }
    } else {
        chunk = text;
    }
    if (chunk.empty()) throw std::runtime_error("debug_raw: no chunk");
    VoiceCache& vc = voice_cache(voice_wav);
    std::vector<float> latents = generate_chunk(vc, chunk, rng_, temp_, cfg_.zero_noise);
    std::vector<float> audio = decode_all(latents, (int)(latents.size() / ldim_));
    {
        std::ofstream f(prefix + ".lat", std::ios::binary);
        f.write(reinterpret_cast<const char*>(latents.data()), (std::streamsize)latents.size() * 4);
    }
    write_wav16(prefix + ".raw.wav", audio, (int)sample_rate_);
    fprintf(stderr, "debug_raw: chunk=\"%s\" latents=%zu raw_samples=%zu\n", chunk.c_str(), latents.size() / ldim_, audio.size());
}

std::string Engine::phonemise_text(const std::string& text, const GenOptions& opt) {
    // auto: a string with no Persian script is taken as phonemes already.
    // input_text forces the G2P (numeric/symbol-only input), input_phonemes
    // forces the opposite — the two never both apply (they are set exclusive).
    if (!opt.input_text && (opt.input_phonemes || !looks_persian(text))) return text;  // already phonemes
    Dictionary local;
    const Dictionary* d = run_dictionary(opt, local);
    return phonemise_phrase(*g2p_, d, text, opt.keep_ezafe);
}

std::vector<std::string> Engine::debug_chunks(const std::string& ph) { return chunk_phonemes(ph, *sp_, 18); }

}  // namespace tts
