// pocket-tts-farsi-v2 ONNX engine — native C++ port of scripts/tts_onnx.py.
//
// Pure host logic (G2P chunking, FlowLM KV-cache stepping, Mimi decoder state
// wiring, retry/quality gates, stitching) wrapped around the four ONNX graphs
// via the ONNX Runtime C API.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "chunk.h"
#include "dict.h"
#include "g2p.h"
#include "ort_util.h"
#include "rng.h"
#include "spm.h"

namespace tts {

struct EngineConfig {
    std::string base_dir;       // dir containing model/onnx, model/v2, voices
    bool seed_set = false;
    uint64_t seed = 0;
    bool zero_noise = false;    // deterministic verification mode
    // ONNX Runtime intra-op threads; 0 = auto (all cores, capped at 8)
    int intra_threads = 0;
    int inter_threads = 1;
};

// Everything a single synthesis run can vary. Defaults reproduce the reference
// pipeline exactly; every field maps 1:1 to a CLI flag and an HTTP query
// parameter (see docs/win7.md §5).
//
//   CLI            HTTP
//   --mode         ?mode=split|pack
//   --pace         ?pace=
//   --seed         ?seed=
//   --temperature  ?temperature=
//   --retries      ?retries=
//   --chunk-tokens ?chunk-tokens=
//   --pause-scale  ?pause-scale=
//   --keep-ezafe   ?keep-ezafe=1
//   --phonemes-in  ?input=phonemes
//   --zero-noise   ?zero-noise=1
//   --dict         ?dict=word%3Dphon%3B...
struct GenOptions {
    std::string mode = "split";   // "split" = pause at punctuation, "pack" = merge
    double pace = 1.0;            // 0.6 .. 1.5, linear time-stretch
    bool seed_set = false;
    uint64_t seed = 0;
    double temperature = -1.0;    // <0 = model default (manifest "temp", 0.3)
    int retries = 3;              // quality-gate attempts per chunk, 1..4
    int chunk_tokens = 18;        // SentencePiece budget per chunk, 8..32
    double pause_scale = 1.0;     // multiplies every inter-phrase/chunk gap
    bool keep_ezafe = false;      // keep the "1" ezafe marker in phonemes
    bool input_phonemes = false;  // force "input is already phonemes"
    bool input_text = false;      // force "input is Persian text" (always run the
                                  // G2P — needed for numeric/symbol-only input,
                                  // which auto-detection would read as phonemes)
    bool zero_noise = false;      // deterministic noise (verification)
    std::string dict;             // extra dictionary entries for this run only
};

// What a run actually did — the web UI / API echo the phonemes and the chunk
// plan back so the caller can show (and re-use) them.
struct SynthResult {
    std::vector<float> audio;
    std::string phonemes;              // phoneme string that was spoken
    std::vector<std::string> chunks;   // model-sized chunks in play order
    bool from_phonemes = false;        // input was phonemes, G2P was skipped
    // Quality gate outcome: chunks that stayed below the acceptance test even
    // after every retry. quiet_chunks > 0 means part of the audio may be
    // silent or truncated — callers surface that instead of handing back
    // quietly broken output (engine also logs one warning per run to stderr).
    int total_chunks = 0;
    int quiet_chunks = 0;
};

class Engine {
public:
    explicit Engine(const EngineConfig& cfg);

    int sample_rate() const { return (int)sample_rate_; }
    double default_temperature() const { return temp_; }
    const EngineConfig& config() const { return cfg_; }
    int intra_threads() const { return cfg_.intra_threads; }

    // Full run: Persian text (auto-detected) or phonemes -> audio + metadata.
    SynthResult synthesize(const std::string& input, const std::string& voice_wav,
                           const GenOptions& opt = GenOptions());

    // Legacy wrappers (audio only).
    std::vector<float> synthesize_text(const std::string& text, const std::string& voice_wav,
                                       const std::string& mode = "split", double pace = 1.0);
    std::vector<float> synthesize_phonemes(const std::string& phonemes, const std::string& voice_wav,
                                           double pace = 1.0);

    // --- text front-end ---------------------------------------------------
    // Persian text -> phonemes (dictionary aware; `opt.dict` adds per-run
    // entries, `opt.keep_ezafe` keeps the "1" marker). A romanized phoneme
    // string is returned unchanged.
    std::string phonemise_text(const std::string& text, const GenOptions& opt = GenOptions());
    // The chunk plan a synthesis would use, without generating audio (fast).
    std::vector<std::string> plan_chunks(const std::string& input, const GenOptions& opt = GenOptions());
    // Phoneme chunking only (debug hook used by the verification harness).
    std::vector<std::string> debug_chunks(const std::string& ph);

    // --- voice cache ------------------------------------------------------
    // Encode the reference voice NOW (Mimi encoder + one flow step) so the
    // first/each later synthesis reuses it. Costs ~0.3 s once instead of on
    // every run of a batch.
    void preload_voice(const std::string& wav);
    bool voice_loaded(const std::string& wav) const;
    // Level of the prompt that produced a cached voice: `rms` is its original
    // RMS, `gain` the loudness correction applied (>1 = the prompt was quiet
    // and was amplified). Returns false when the voice is not encoded yet.
    bool voice_stats(const std::string& wav, float* rms = nullptr, float* gain = nullptr) const;
    std::vector<std::string> loaded_voices() const;
    // Drop an encoded voice again (frees its KV prompt, ~1-2 MB). The GUI uses
    // this when the user picks a different reference voice: the selected voice
    // stays cached for many generations, every other one is offloaded. Returns
    // false when the path was not cached.
    bool unload_voice(const std::string& wav);
    // Drop every cached voice (not the model itself). Returns how many went.
    int unload_all_voices();

    // --- pronunciation dictionary ----------------------------------------
    // Loaded from <base>/dicts/*.dict at construction; reload() re-reads them.
    Dictionary& dict() { return dict_; }
    const Dictionary& dict() const { return dict_; }
    void set_dict(const Dictionary& d) { dict_ = d; }
    void reload_dictionaries();
    std::string dict_path() const;      // <base>/dicts/user.dict
    std::string dict_dir() const;       // <base>/dicts
    // Save the current dictionary to dict_path().
    bool save_dict() const { return dict_.save_file(dict_path()); }

    // Debug hooks used by the verification harness.
    void debug_raw(const std::string& text, const std::string& voice_wav, const std::string& prefix);

private:
    struct State {
        std::vector<int64_t> shape;
        ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
        std::vector<uint8_t> data;
    };
    struct VoiceCache {
        std::vector<float> kv;   // L*2*CAP*H*D
        int64_t off = 0;
        std::string path;
        uint64_t mtime = 0;
        float prompt_rms = 0;    // level of the prompt as it was read
        float prompt_gain = 1;   // loudness correction applied before encoding
    };

    std::string onnx(const std::string& f) const { return onnx_dir_ + "/" + f; }

    // --- FlowLM ------------------------------------------------------------
    // Runs one flow-LM step; scatters new K/V into `cache` at `offset`.
    // `cache` is L*2*CAP*H*D floats.
    void flow_step(const float* seq, int64_t seq_len, const float* text_emb, int64_t text_len,
                   int64_t& offset, const float* noise, std::vector<float>& cache,
                   std::vector<float>& latent_out, float& eos_out, int64_t& new_S);

    VoiceCache& voice_cache(const std::string& wav_path);

    // --- generation --------------------------------------------------------
    std::vector<float> generate_chunk(const VoiceCache& vc, const std::string& chunk, Rng& rng, double temp,
                                       bool zero_noise);
    std::vector<float> decode_all(const std::vector<float>& latents, int n_steps);
    std::vector<float> generate_with_retry(const VoiceCache& vc, const std::string& chunk, Rng& rng,
                                           double temp, int retries, bool zero_noise, bool* quiet = nullptr);
    std::vector<float> synthesize_plan(const std::vector<Phrase>& plan, const std::string& voice_wav,
                                       const GenOptions& opt, std::vector<std::string>* chunks_out,
                                       int* total_out = nullptr, int* quiet_out = nullptr);

    // Sentence/phrase planning for one input (text or phonemes).
    std::vector<std::vector<Phrase>> build_plans(const std::string& input, const Dictionary* d, const GenOptions& o);
    // Engine dictionary + per-run opt.dict overrides.
    const Dictionary* run_dictionary(const GenOptions& opt, Dictionary& local) const;

    // --- audio quality helpers (static, ports of the reference) -----------
    static void window_mean(const std::vector<float>& x, int w, std::vector<float>& out);
    static std::vector<std::pair<int, int>> dead_air_regions(const std::vector<float>& p, double rel_floor,
                                                             double win, double density, int sr);
    static double solid_fraction(const std::vector<float>& speech, double floor, double win, int sr);
    static std::pair<int, int> speech_bounds(const std::vector<float>& p, int sr);
    std::vector<float> compress_pauses(const std::vector<float>& audio, double max_pause, double keep, double rel_floor);
    std::vector<float> stitch(const std::vector<std::pair<std::vector<float>, double>>& segments);

    EngineConfig cfg_;
    std::string base_dir_, onnx_dir_, v2_dir_;
    std::vector<int64_t> dec_shape_cache_;
    int64_t ldim_ = 32, dim_ = 1024, L_ = 6, H_ = 16, D_ = 64, CAP_ = 384;
    int64_t sample_rate_ = 24000, steps_per_latent_ = 16;
    double temp_ = 0.3, tps_est_ = 3.0, gen_pad_ = 2.0, frame_rate_ = 12.5;

    std::vector<float> lut_;        // [4001, 1024]
    std::vector<float> spk_proj_;   // [1024, 32]
    std::vector<float> bos_voice_;  // [1024]
    std::vector<float> emb_std_, emb_mean_;

    // decoder state layout
    std::vector<State> dec_init_;
    std::vector<int> cache_slots_, small_slots_;
    std::vector<int> out_small_idx_, out_kv_idx_;

    std::unique_ptr<Spm> sp_;
    std::unique_ptr<G2P> g2p_;
    std::unique_ptr<Session> s_flow_, s_enc_, s_dec_;
    Rng rng_;
    std::map<std::string, std::shared_ptr<VoiceCache>> voice_memo_;
    Dictionary dict_;
};

}  // namespace tts
