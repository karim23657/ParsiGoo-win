// farsi-tts CLI — the native equivalent of scripts/tts_onnx.py.
//
//   farsi-tts "سلام، حال شما چطور است؟" --voice voices/male_hello.wav --out out.wav
//
// Every flag below has a 1:1 counterpart in the HTTP API (docs/win7.md §5).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include "audio.h"
#include "dict.h"
#include "tts.h"

#define VERSION "1.1.0"

static void usage(FILE* f) {
    fprintf(f,
"farsi-tts — Persian text-to-speech (ONNX, no Python)   parsi-go " VERSION "\n"
"\n"
"usage:\n"
"  farsi-tts [options] \"<persian text | phonemes>\"\n"
"  farsi-tts [options] --text-file text.txt\n"
"  farsi-tts [options] --bulk lines.txt --out-dir output/bulk\n"
"\n"
"input\n"
"  --text STR          text to speak (same as the positional argument)\n"
"  --text-file PATH    read UTF-8 text from PATH (\"-\" = stdin). USE THIS ON\n"
"                      WINDOWS: cmd/PowerShell cannot carry Persian on argv\n"
"  --phonemes-in       treat the input as phonemes, never run the G2P\n"
"  --text-in           the opposite: always run the G2P, even when the input has\n"
"                      no Persian characters (numeric/symbol-only input, which\n"
"                      auto-detection would read as phonemes)\n"
"  --phonemes          print the phoneme string for the text and exit\n"
"  --phonemes-out P    with --phonemes: also write the phoneme string to P\n"
"  --chunks            print the chunk plan (what is generated, one per line)\n"
"                      and exit — no audio\n"
"  --bulk PATH         one text per line; synthesise every line. The model and\n"
"                      the reference voice are loaded/encoded ONCE for the\n"
"                      whole batch (fast for many short lines)\n"
"  --out-dir DIR       output folder for --bulk (default output/bulk)\n"
"\n"
"reference voice\n"
"  --voice PATH        reference voice WAV, ~5 s (default voices/male_hello.wav)\n"
"  --preload-voice     encode the voice before doing anything else and print\n"
"                      the cost (it is cached for every later synthesis)\n"
"  --list-voices       print the reference voices found in <base>/voices and\n"
"                      <base>/uploads (one path per line) and exit\n"
"\n"
"output\n"
"  --out PATH          output WAV (default output/tts.wav)\n"
"  --base DIR          package base dir holding model/, voices/, dicts/\n"
"                      (default: current directory)\n"
"  --play              play the result through the speakers after writing it\n"
"                      (Windows; blocking until playback ends)\n"
"\n"
"speech parameters  (--mode is the only one without a direct \"model\" meaning;\n"
"                    see \"modes\" below)\n"
"  --mode MODE         split (default) | pack\n"
"  --pack              shorthand for --mode pack\n"
"  --pace F            speech pace, time-stretch factor 0.6 .. 1.5 (1.0 = as-is)\n"
"  --seed N            reproducible run (default: random each run)\n"
"  --temperature F     flow-LM sampling temperature, 0.05 .. 2.0\n"
"                      (default -1 = the model value, 0.3). Lower = calmer,\n"
"                      more repeatable; higher = more variation\n"
"  --retries N         quality-gate attempts per chunk, 1 .. 4 (default 3).\n"
"                      Attempt 0 runs at --temperature; every later attempt\n"
"                      warms it by +0.25 so a retry is a genuinely different\n"
"                      sample (at the default 0.3 sampling is nearly greedy\n"
"                      and retries used to reproduce the same chunk). The\n"
"                      first attempt that clears the gate wins; if none does,\n"
"                      the best one is returned and a warning goes to stderr\n"
"                      (that chunk may be silent or short). 1 = fastest,\n"
"                      4 = pickiest/slowest\n"
"  --chunk-tokens N    SentencePiece budget per generated chunk, 8 .. 32\n"
"                      (default 18 = model training value). Bigger = longer\n"
"                      breaths but riskier chunks\n"
"  --pause-scale F     multiply every pause, 0 .. 3 (1.0 = normal, 0 = tight)\n"
"  --keep-ezafe        keep the \"1\" ezafe marker in the phoneme string\n"
"  --zero-noise        deterministic noise (harness only; can silence output)\n"
"\n"
"pronunciation dictionary  (<base>/dicts/user.dict)\n"
"  --dict PATH         merge an extra dictionary file for this run (repeatable)\n"
"  --dict-set W=PH     add/replace an entry in <base>/dicts/user.dict, then exit\n"
"  --dict-del WORD     delete an entry from <base>/dicts/user.dict, then exit\n"
"  --dict-import PATH  merge a dictionary file into <base>/dicts/user.dict\n"
"  --dict-list         print the dictionary (word<TAB>phonemes) and exit\n"
"\n"
"engine\n"
"  --threads N         ONNX Runtime intra-op threads (default: all cores, max 8)\n"
"  --version           print version and exit\n"
"  -h, --help          this help\n"
"\n"
"modes\n"
"  split   (default)  every comma / semicolon / colon keeps its pause — the\n"
"                     natural \"reading aloud\" rhythm.\n"
"  pack    (--pack)   comma phrases are merged into longer units so the voice\n"
"                     takes one breath: fewer and shorter pauses, the sentence\n"
"                     flows on. Same text, same voice, only the phrasing and\n"
"                     the silences differ.\n"
"  Both modes still pack the phonemes into model-sized chunks; --pause-scale\n"
"  is the knob that changes how long the silences actually are.\n"
"\n"
"phonemes\n"
"  Persian text is converted to a romanised phoneme string (G2P) before it is\n"
"  spoken. Anything without Persian characters is taken AS phonemes already, so\n"
"  this works:\n"
"      farsi-tts --phonemes \"بردارسازی\"        -> prints bardArsAzi\n"
"      farsi-tts \"bordArAzi\" --phonemes-in     -> speaks your edited string\n"
"\n"
"dictionary format (UTF-8, one entry per line, TAB between the two fields):\n"
"      # comment\n"
"      بُردارسازی<TAB>bordArAzi\n"
"  Lookup ignores case, punctuation, ZWNJ and Arabic diacritics, so\n"
"  «بُردارسازی» and «بردارسازی» are the same key.\n"
"\n"
"examples\n"
"  farsi-tts \"سلام، حال شما چطور است؟\" --voice voices/male_hello.wav --out out.wav\n"
"  farsi-tts --phonemes \"بردارسازی\"\n"
"  farsi-tts --dict-set \"بردارسازی=bordArAzi\"\n"
"  farsi-tts --text-file text.txt --pace 1.2 --mode pack --seed 7\n"
"  farsi-tts --bulk lines.txt --out-dir output/bulk --voice voices/male_news.wav\n"
"\n"
"Exit codes: 0 ok, 1 error. See docs/win7.md §5 for the HTTP API.\n");
}

struct Args {
    std::string text, positional_text, text_file;
    std::string voice = "voices/male_hello.wav", out = "output/tts.wav";
    std::string base = ".", out_dir = "output/bulk", bulk_file;
    std::vector<std::string> dict_files;
    std::string dict_set, dict_del, dict_import;
    bool dict_list = false;
    bool seeded = false, zero_noise = false, print_ph = false, print_chunks = false;
    bool pack = false, phonemes_in = false, text_in = false, preload = false, version = false, help = false;
    bool list_voices = false, play = false;
    std::string dump_raw, phonemes_out;
    unsigned long long seed = 0;
    tts::GenOptions opt;
    int threads = 0;
};

static bool read_all(const std::string& path, std::string& out) {
    if (path == "-") {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        out = ss.str();
        return true;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// --list-voices: the reference voices the GUI / server / --voice accept.
static int list_voices(const std::string& base) {
    for (const std::string& sub : {"voices", "uploads"}) {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::path(base) / sub;
        if (!std::filesystem::exists(dir, ec)) continue;
        std::vector<std::string> names;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".wav") names.push_back(e.path().filename().string());  // the only decodable format
        }
        std::sort(names.begin(), names.end());
        for (auto& n : names) printf("%s/%s\n", sub.c_str(), n.c_str());
    }
    return 0;
}

// --play: hand the finished WAV to the sound card (Windows only — the Linux
// build is the test/verification build and has no audio stack).
static void play_wav(const std::string& path) {
#ifdef _WIN32
    std::wstring wpath;
    if (!path.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
        wpath.assign(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &wpath[0], n);
    }
    if (!PlaySoundW(wpath.c_str(), nullptr, SND_FILENAME | SND_SYNC))
        fprintf(stderr, "play: could not play %s\n", path.c_str());
#else
    (void)path;
    fprintf(stderr, "play: playback is built into the Windows build only (%s)\n", path.c_str());
#endif
}

static int dict_command(const Args& a) {
    tts::Dictionary d;
    std::string path = a.base + "/dicts/user.dict";
    d.load_file(path);
    if (a.dict_list) {
        for (auto& e : d.entries()) printf("%s\t%s\n", e.word.c_str(), e.phonemes.c_str());
        if (d.empty())
            fprintf(stderr, "dictionary is empty — add an entry with --dict-set \"word=phonemes\"\n");
        return 0;
    }
    if (!a.dict_set.empty()) {
        size_t eq = a.dict_set.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 >= a.dict_set.size()) {
            fprintf(stderr, "error: --dict-set wants WORD=PHONEMES, got \"%s\"\n", a.dict_set.c_str());
            return 1;
        }
        std::string w = a.dict_set.substr(0, eq), p = a.dict_set.substr(eq + 1);
        if (!d.set(w, p)) { fprintf(stderr, "error: empty word or phonemes\n"); return 1; }
        if (!d.save_file(path)) { fprintf(stderr, "error: cannot write %s\n", path.c_str()); return 1; }
        printf("dictionary: %s -> %s  (%zu entries)  [%s]\n", w.c_str(), p.c_str(), d.size(), path.c_str());
        return 0;
    }
    if (!a.dict_del.empty()) {
        if (!d.erase(a.dict_del)) {
            fprintf(stderr, "dictionary: no entry for \"%s\"\n", a.dict_del.c_str());
            return 1;
        }
        if (!d.save_file(path)) { fprintf(stderr, "error: cannot write %s\n", path.c_str()); return 1; }
        printf("dictionary: removed \"%s\" (%zu entries left)\n", a.dict_del.c_str(), d.size());
        return 0;
    }
    if (!a.dict_import.empty()) {
        tts::Dictionary tmp;
        size_t n = tmp.load_file(a.dict_import);
        if (!n) { fprintf(stderr, "error: no entries read from %s\n", a.dict_import.c_str()); return 1; }
        for (auto& e : tmp.entries()) d.set(e.word, e.phonemes);
        if (!d.save_file(path)) { fprintf(stderr, "error: cannot write %s\n", path.c_str()); return 1; }
        printf("dictionary: merged %zu entries from %s (%zu total)\n", n, a.dict_import.c_str(), d.size());
        return 0;
    }
    return -1;  // no dictionary command
}

int main(int argc, char** argv) {
    Args a;
    a.opt.chunk_tokens = 18;
    a.opt.retries = 3;
    a.opt.pause_scale = 1.0;
    a.opt.temperature = -1.0;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s needs a value\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (s == "--voice") a.voice = next("--voice");
        else if (s == "--out") a.out = next("--out");
        else if (s == "--out-dir") a.out_dir = next("--out-dir");
        else if (s == "--base") a.base = next("--base");
        else if (s == "--bulk") a.bulk_file = next("--bulk");
        else if (s == "--seed") { a.seed = std::stoull(next("--seed")); a.seeded = true; }
        else if (s == "--threads") a.threads = std::stoi(next("--threads"));
        else if (s == "--text-file") a.text_file = next("--text-file");
        else if (s == "--text") a.positional_text = next("--text");
        else if (s == "--dump-raw") a.dump_raw = next("--dump-raw");
        else if (s == "--phonemes-out") a.phonemes_out = next("--phonemes-out");
        else if (s == "--dict") a.dict_files.push_back(next("--dict"));
        else if (s == "--dict-set") a.dict_set = next("--dict-set");
        else if (s == "--dict-del") a.dict_del = next("--dict-del");
        else if (s == "--dict-import") a.dict_import = next("--dict-import");
        else if (s == "--dict-list") a.dict_list = true;
        else if (s == "--mode") {
            a.opt.mode = next("--mode");
            if (a.opt.mode != "split" && a.opt.mode != "pack") {
                fprintf(stderr, "error: --mode must be split or pack (got \"%s\")\n", a.opt.mode.c_str());
                return 1;
            }
        } else if (s == "--pack") a.pack = true;
        else if (s == "--pace") a.opt.pace = std::stod(next("--pace"));
        else if (s == "--temperature" || s == "--temp") a.opt.temperature = std::stod(next("--temperature"));
        else if (s == "--retries") a.opt.retries = std::stoi(next("--retries"));
        else if (s == "--chunk-tokens") a.opt.chunk_tokens = std::stoi(next("--chunk-tokens"));
        else if (s == "--pause-scale") a.opt.pause_scale = std::stod(next("--pause-scale"));
        else if (s == "--zero-noise") a.zero_noise = true;
        else if (s == "--keep-ezafe") a.opt.keep_ezafe = true;
        else if (s == "--phonemes-in") a.phonemes_in = true;
        else if (s == "--text-in") a.text_in = true;
        else if (s == "--phonemes") a.print_ph = true;
        else if (s == "--chunks") a.print_chunks = true;
        else if (s == "--preload-voice") a.preload = true;
        else if (s == "--list-voices") a.list_voices = true;
        else if (s == "--play") a.play = true;
        else if (s == "--version") a.version = true;
        else if (s == "-h" || s == "--help") a.help = true;
        else if (!s.empty() && s[0] == '-' && s != "-") {
            fprintf(stderr, "error: unknown option \"%s\" (try --help)\n", s.c_str());
            return 1;
        } else positional.push_back(s);
    }

    if (a.help) { usage(stdout); return 0; }
    if (a.version) { printf("farsi-tts %s (pocket-tts-farsi-v2, ONNX)\n", VERSION); return 0; }
    if (a.pack) a.opt.mode = "pack";
    // the two input switches are mutually exclusive; the last one wins
    if (a.phonemes_in) { a.opt.input_phonemes = true; a.opt.input_text = false; }
    if (a.text_in) { a.opt.input_text = true; a.opt.input_phonemes = false; }
    a.opt.zero_noise = a.zero_noise;

    // dictionary commands run without loading the model
    int dc = dict_command(a);
    if (dc >= 0) return dc;
    if (a.list_voices) return list_voices(a.base);

    if (!a.positional_text.empty()) a.text = a.positional_text;
    else if (!positional.empty()) a.text = positional[0];
    else a.text = "سلام، حال شما چطور است؟";
    if (!a.text_file.empty()) {
        // UTF-8 file input: Windows consoles and Wine mangle non-ASCII argv
        if (!read_all(a.text_file, a.text)) {
            fprintf(stderr, "error: cannot read %s\n", a.text_file.c_str());
            return 1;
        }
    }
    if (!a.seeded) a.seed = (unsigned long long)std::chrono::system_clock::now().time_since_epoch().count();
    a.opt.seed_set = true;
    a.opt.seed = a.seed;

    // a relative --voice resolves against --base, not the current directory
    bool voice_abs = !a.voice.empty() && (a.voice[0] == '/' || a.voice[0] == '\\' || a.voice.find(':') != std::string::npos);
    if (!voice_abs) a.voice = a.base + "/" + a.voice;

    try {
        tts::EngineConfig cfg;
        cfg.base_dir = a.base;
        cfg.seed_set = true;
        cfg.seed = a.seed;
        cfg.zero_noise = a.zero_noise;
        cfg.intra_threads = a.threads;

        // --phonemes / --chunks without an engine-level dump still need the
        // model (G2P + SentencePiece), but never the voice encoder.
        if (!a.dump_raw.empty()) {
            tts::Engine eng(cfg);
            eng.debug_raw(a.text, a.voice, a.dump_raw);
            return 0;
        }

        auto t_load0 = std::chrono::steady_clock::now();
        tts::Engine eng(cfg);
        auto t_load1 = std::chrono::steady_clock::now();

        // extra dictionaries for this run (merged over <base>/dicts/*.dict)
        for (auto& f : a.dict_files) eng.dict().load_file(f);

        if (a.print_ph) {
            std::string ph = eng.phonemise_text(a.text, a.opt);
            printf("%s\n", ph.c_str());
            if (!a.phonemes_out.empty()) {
                std::ofstream f(a.phonemes_out, std::ios::binary);
                if (!f) { fprintf(stderr, "error: cannot write %s\n", a.phonemes_out.c_str()); return 1; }
                f << ph << "\n";
            }
            return 0;
        }
        if (a.print_chunks) {
            for (auto& c : eng.plan_chunks(a.text, a.opt)) printf("%s\n", c.c_str());
            return 0;
        }

        if (a.preload || !a.bulk_file.empty()) {
            auto v0 = std::chrono::steady_clock::now();
            eng.preload_voice(a.voice);
            auto v1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(v1 - v0).count();
            fprintf(stderr, "voice loaded: %s (%.0f ms)\n", a.voice.c_str(), ms);
            if (a.preload && a.bulk_file.empty() && a.text_file.empty() && positional.empty() &&
                a.positional_text.empty()) {
                fprintf(stderr, "model load %.1f s\n", std::chrono::duration<double>(t_load1 - t_load0).count());
                return 0;
            }
        }

        if (!a.bulk_file.empty()) {
            std::string all;
            if (!read_all(a.bulk_file, all)) {
                fprintf(stderr, "error: cannot read %s\n", a.bulk_file.c_str());
                return 1;
            }
            std::vector<std::string> lines;
            std::istringstream ss(all);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t x = line.find_first_not_of(" \t");
                if (x == std::string::npos) continue;
                if (line[x] == '#') continue;
                lines.push_back(line.substr(x, line.find_last_not_of(" \t") - x + 1));
            }
            if (lines.empty()) { fprintf(stderr, "error: %s has no text lines\n", a.bulk_file.c_str()); return 1; }
            std::error_code ec;
            std::filesystem::create_directories(a.out_dir, ec);
            auto b0 = std::chrono::steady_clock::now();
            double audio_total = 0;
            for (size_t i = 0; i < lines.size(); i++) {
                tts::GenOptions o = a.opt;
                o.seed = a.seed + i;  // stable per-line seeds, reproducible batch
                auto s0 = std::chrono::steady_clock::now();
                tts::SynthResult r = eng.synthesize(lines[i], a.voice, o);
                auto s1 = std::chrono::steady_clock::now();
                double wall = std::chrono::duration<double>(s1 - s0).count();
                char name[64];
                snprintf(name, sizeof(name), "part-%04zu.wav", i + 1);
                std::string path = a.out_dir + "/" + name;
                tts::write_wav16(path, r.audio, eng.sample_rate());
                double secs = r.audio.size() / (double)eng.sample_rate();
                audio_total += secs;
                printf("[%zu/%zu] %.2fs audio in %.1fs -> %s\n", i + 1, lines.size(), secs, wall, path.c_str());
                fflush(stdout);
            }
            double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
            printf("bulk done: %zu lines, %.1fs of audio, %.1fs wall -> %s\n",
                   lines.size(), audio_total, wall, a.out_dir.c_str());
            return 0;
        }

        auto t0 = std::chrono::steady_clock::now();
        tts::SynthResult r = eng.synthesize(a.text, a.voice, a.opt);
        auto t1 = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();

        if (r.audio.empty()) {
            fprintf(stderr, "error: nothing to synthesise (empty text?)\n");
            return 1;
        }
        tts::write_wav16(a.out, r.audio, eng.sample_rate());
        printf("phonemes: %s\n", r.phonemes.c_str());
        printf("generated %.2fs audio in %.2fs -> %s\n", r.audio.size() / (double)eng.sample_rate(), dt,
               a.out.c_str());
        if (a.play) play_wav(a.out);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
