#include "dict.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tts {
namespace {

namespace fs = std::filesystem;

std::u32string u32(const std::string& s) {
    std::u32string out;
    size_t i = 0, n = s.size();
    while (i < n) {
        uint32_t c = (uint8_t)s[i];
        if (c < 0x80) { out.push_back(c); i += 1; }
        else if ((c >> 5) == 0x6 && i + 1 < n) { out.push_back(((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F)); i += 2; }
        else if ((c >> 4) == 0xE && i + 2 < n) { out.push_back(((c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F)); i += 3; }
        else if ((c >> 3) == 0x1E && i + 3 < n) {
            out.push_back(((c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) |
                          (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F));
            i += 4;
        } else { out.push_back(0xFFFD); i += 1; }
    }
    return out;
}

std::string u8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        uint32_t v = (uint32_t)c;
        if (v < 0x80) out.push_back((char)v);
        else if (v < 0x800) { out.push_back((char)(0xC0 | (v >> 6))); out.push_back((char)(0x80 | (v & 0x3F))); }
        else if (v < 0x10000) {
            out.push_back((char)(0xE0 | (v >> 12)));
            out.push_back((char)(0x80 | ((v >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (v & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (v >> 18)));
            out.push_back((char)(0x80 | ((v >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((v >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (v & 0x3F)));
        }
    }
    return out;
}

bool is_space(char32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0C || c == 0x00A0;
}

// Punctuation / decorations that may wrap a word in running text.
bool is_punct(char32_t c) {
    if (c < 0x80) return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
                         (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
    switch (c) {
        case 0x00AB: case 0x00BB: case 0x060C: case 0x061B: case 0x061F:  // « » ، ؛ ؟
        case 0x0640:                                                       // tatweel
        case 0x066A: case 0x066B: case 0x066C: case 0x066D:                // ٪ ٫ ٬ ..
        case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201C:
        case 0x201D: case 0x2026: case 0x00D7: case 0x00F7:
            return true;
        default: return false;
    }
}

// Arabic diacritics (incl. the damma in «بُردارسازی») — dropped by the
// reference normaliser before G2P, so drop them from the key too.
bool is_diacritic(char32_t c) {
    return (c >= 0x064B && c <= 0x065F) || c == 0x0670 || c == 0x0640;
}

bool is_zwnj(char32_t c) { return c == 0x200C; }

char32_t fold(char32_t c) {
    switch (c) {
        case 0x064A: return 0x06CC;  // ي -> ی
        case 0x0649: return 0x06CC;  // ى -> ی
        case 0x06CC: return 0x06CC;
        case 0x0643: return 0x06A9;  // ك -> ک
        case 0x06AA: return 0x06A9;  // ک (swash kaf)
        case 0x0629: return 0x0647;  // ة -> ه
        case 0x0623: return 0x0627;  // أ -> ا
        case 0x0625: return 0x0627;  // إ -> ا
        case 0x0671: return 0x0627;  // ٱ -> ا
        case 0x0622: return 0x0627;  // آ -> ا (dictionary keys only)
        case 0x0624: return 0x0648;  // ؤ -> و
        case 0x0626: return 0x06CC;  // ئ -> ی
        default: break;
    }
    if (c >= 0x0660 && c <= 0x0669) return 0x06F0 + (c - 0x0660);  // Arabic -> Persian digits
    if (c >= 'A' && c <= 'Z') return 'a' + (c - 'A');
    return c;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Split "word<sep>phonemes"; TAB wins, otherwise the first whitespace run.
bool split_entry(const std::string& line, std::string& word, std::string& ph) {
    size_t tab = line.find('\t');
    if (tab != std::string::npos) {
        word = trim(line.substr(0, tab));
        ph = trim(line.substr(tab + 1));
        return !word.empty() && !ph.empty();
    }
    size_t sp = line.find_first_of(" \v");
    if (sp == std::string::npos) return false;
    size_t sp2 = line.find_first_not_of(" \v", sp);
    if (sp2 == std::string::npos) return false;
    word = trim(line.substr(0, sp));
    ph = trim(line.substr(sp2));
    return !word.empty() && !ph.empty();
}

}  // namespace

std::vector<std::string> dict_split_words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space((char32_t)(uint8_t)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !is_space((char32_t)(uint8_t)s[j])) j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string Dictionary::key(const std::string& word) {
    std::u32string u = u32(word);
    std::u32string out;
    for (char32_t c : u) {
        if (is_space(c) || is_punct(c) || is_diacritic(c) || is_zwnj(c)) continue;
        out.push_back(fold(c));
    }
    return u8(out);
}

bool Dictionary::set(const std::string& word, const std::string& phonemes) {
    std::string w = trim(word), p = trim(phonemes);
    if (w.empty() || p.empty()) return false;
    std::string k = key(w);
    if (k.empty()) return false;
    map_[k] = Entry{w, p};
    return true;
}

bool Dictionary::erase(const std::string& word) { return map_.erase(key(word)) > 0; }

std::string Dictionary::lookup(const std::string& word) const {
    auto it = map_.find(key(word));
    return it == map_.end() ? "" : it->second.phonemes;
}

bool Dictionary::has_match(const std::string& text) const {
    if (map_.empty()) return false;
    for (auto& w : dict_split_words(text))
        if (map_.count(key(w))) return true;
    return false;
}

bool Dictionary::value_has_spaces() const {
    for (auto& kv : map_)
        if (kv.second.phonemes.find(' ') != std::string::npos) return true;
    return false;
}

std::vector<Dictionary::Entry> Dictionary::entries() const {
    std::vector<Entry> out;
    out.reserve(map_.size());
    for (auto& kv : map_) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return key(a.word) < key(b.word); });
    return out;
}

size_t Dictionary::load_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_text(all);
}

size_t Dictionary::load_dir(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".dict" || ext == ".txt") files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    size_t n = 0;
    for (auto& f : files) n += load_file(f);
    return n;
}

size_t Dictionary::load_text(const std::string& text) {
    std::istringstream ss(text);
    std::string line;
    size_t n = 0;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::string w, p;
        if (split_entry(t, w, p) && set(w, p)) n++;
    }
    return n;
}

bool Dictionary::save_file(const std::string& path) const {
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# parsi-go pronunciation dictionary — word<TAB>phonemes (UTF-8)\n";
    f << "# one entry per line, '#' starts a comment, lookup ignores case,\n";
    f << "# punctuation and diacritics (bArDArAzi == بُردارسازی).\n";
    for (auto& e : entries()) f << e.word << '\t' << e.phonemes << '\n';
    return (bool)f;
}

// ---------------------------------------------------------------------------
std::string phonemise_phrase(G2P& g2p, const Dictionary* dict, const std::string& text, bool keep_ezafe) {
    if (!dict || dict->empty() || !dict->has_match(text)) return g2p.phonemise(text, keep_ezafe);

    auto strip_ezafe = [keep_ezafe](std::string s) {
        if (keep_ezafe) return s;
        std::string out;
        for (char c : s) if (c != '1') out.push_back(c);
        return out;
    };

    std::vector<std::string> tw = dict_split_words(text);

    // A dictionary value that is itself several words cannot be aligned
    // positionally — go straight to the splice path below.
    bool multiword_hit = false;
    for (auto& w : tw) {
        std::string e = dict->lookup(w);
        if (!e.empty() && dict_split_words(e).size() > 1) { multiword_hit = true; break; }
    }

    if (!multiword_hit) {
        // One G2P call for the phrase keeps its context (ezafe, assimilation);
        // the known words are then swapped in by position.
        std::string ph = g2p.phonemise(text, keep_ezafe);
        std::vector<std::string> pw = dict_split_words(ph);
        if (pw.size() == tw.size()) {
            bool changed = false;
            for (size_t i = 0; i < tw.size(); i++) {
                std::string e = dict->lookup(tw[i]);
                if (!e.empty()) { pw[i] = strip_ezafe(e); changed = true; }
            }
            if (changed) {
                std::string out;
                for (size_t i = 0; i < pw.size(); i++) { if (i) out += ' '; out += pw[i]; }
                return out;
            }
        }
    }

    // Splice path: runs of unknown words stay together for the G2P, the known
    // words are inserted verbatim.
    std::string out;
    std::vector<std::string> run;
    auto flush = [&]() {
        if (run.empty()) return;
        std::string joined;
        for (size_t i = 0; i < run.size(); i++) { if (i) joined += ' '; joined += run[i]; }
        std::string ph = g2p.phonemise(joined, keep_ezafe);
        if (!out.empty()) out += ' ';
        out += ph;
        run.clear();
    };
    for (auto& w : tw) {
        std::string e = dict->lookup(w);
        if (e.empty()) {
            run.push_back(w);
        } else {
            flush();
            if (!out.empty()) out += ' ';
            out += strip_ezafe(e);
        }
    }
    flush();
    return out;
}

}  // namespace tts
