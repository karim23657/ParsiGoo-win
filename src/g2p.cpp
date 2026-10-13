#include "g2p.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>

#include "normalize_fa.h"

namespace tts {
namespace {

const int PAD = 0, EOS = 1, UNK = 2;

std::vector<int64_t> byt5_encode(const std::string& s) {
    std::vector<int64_t> ids;
    ids.reserve(s.size());
    for (unsigned char b : s) ids.push_back((int64_t)b + 3);
    return ids;
}

std::string byt5_decode(const std::vector<int64_t>& ids, int skip) {
    std::string out;
    for (size_t i = skip; i < ids.size(); i++) {
        int64_t v = ids[i];
        if (v >= 3 && v <= 258) out.push_back((char)(v - 3));
    }
    return out;
}

// ---- Latin -> Persian transliteration ------------------------------------
const std::map<std::string, std::string> LATIN_DIGRAPHS = {
    {"sh", "ش"}, {"ch", "چ"}, {"th", "ث"}, {"ph", "ف"}, {"gh", "غ"}, {"kh", "خ"},
    {"wh", "و"}, {"ck", "ک"}, {"qu", "کو"}, {"oo", "و"}, {"ee", "ی"}, {"ea", "ی"},
    {"ou", "او"}, {"au", "او"}, {"ai", "ای"}, {"ay", "ای"}, {"ey", "ای"},
    {"oi", "اوی"}, {"oy", "اوی"}};
const std::map<char, std::string> LATIN_CHAR = {
    {'a', "ا"}, {'b', "ب"}, {'c', "ک"}, {'d', "د"}, {'e', ""}, {'f', "ف"}, {'g', "گ"},
    {'h', "ه"}, {'i', "ی"}, {'j', "ج"}, {'k', "ک"}, {'l', "ل"}, {'m', "م"}, {'n', "ن"},
    {'o', "او"}, {'p', "پ"}, {'q', "ق"}, {'r', "ر"}, {'s', "س"}, {'t', "ت"}, {'u', "و"},
    {'v', "و"}, {'w', "و"}, {'x', "کس"}, {'y', "ی"}, {'z', "ز"}};
const std::map<char, std::string> LETTER_NAMES = {
    {'a', "ای"}, {'b', "بی"}, {'c', "سی"}, {'d', "دی"}, {'e', "ای"}, {'f', "اف"},
    {'g', "جی"}, {'h', "اچ"}, {'i', "آی"}, {'j', "جی"}, {'k', "کی"}, {'l', "ال"},
    {'m', "ام"}, {'n', "ان"}, {'o', "او"}, {'p', "پی"}, {'q', "کیو"}, {'r', "آر"},
    {'s', "اس"}, {'t', "تی"}, {'u', "یو"}, {'v', "وی"}, {'w', "دبلیو"}, {'x', "ایکس"},
    {'y', "وای"}, {'z', "زد"}};
const std::map<std::string, std::string> LATIN_EXCEPTIONS = {
    {"echo", "اکو"}, {"state", "استیت"}, {"network", "نت‌ورک"}, {"windows", "ویندوز"},
    {"notebook", "نوت‌بوک"}, {"photoshop", "فتوشاپ"}, {"python", "پایتون"},
    {"recurrency", "ریکارانسی"}};
const std::string CLUSTER_START = "پتکبجچذژزصضثفگسش";

bool is_alpha_ascii(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool is_word_char(char c) { return is_alpha_ascii(c) || c == '\'' || c == '-'; }

std::string transliterate_word(const std::string& w) {
    // hyphenated compounds transliterate part by part
    std::string stripped = w;
    while (!stripped.empty() && stripped.front() == '-') stripped.erase(stripped.begin());
    while (!stripped.empty() && stripped.back() == '-') stripped.pop_back();
    if (stripped.find('-') != std::string::npos) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : w) {
            if (c == '-') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) parts.push_back(cur);
        if (parts.size() > 1) {
            std::string out;
            for (size_t i = 0; i < parts.size(); i++) { if (i) out += " "; out += transliterate_word(parts[i]); }
            return out;
        }
    }
    std::string lw;
    for (char c : w) lw.push_back((char)std::tolower((unsigned char)c));
    auto ex = LATIN_EXCEPTIONS.find(lw);
    if (ex != LATIN_EXCEPTIONS.end()) return ex->second;
    bool upper = !w.empty();
    int alpha = 0;
    for (char c : w) { if (is_alpha_ascii(c)) alpha++; else upper = false; }
    for (char c : w) if (!(c >= 'A' && c <= 'Z')) upper = false;
    if (upper && alpha >= 2 && alpha <= 5 && (size_t)alpha == w.size()) {
        std::string out;
        for (char c : lw) { if (!out.empty()) out += "\u200C"; out += LETTER_NAMES.at(c); }
        return out;
    }
    std::string s;
    size_t i = 0;
    while (i < lw.size()) {
        if (i + 1 < lw.size()) {
            auto d = LATIN_DIGRAPHS.find(lw.substr(i, 2));
            if (d != LATIN_DIGRAPHS.end()) { s += d->second; i += 2; continue; }
        }
        auto c = LATIN_CHAR.find(lw[i]);
        s += (c != LATIN_CHAR.end()) ? c->second : "";
        i++;
    }
    // English onsets like sp/st/sk are impossible in Persian
    if (s.size() >= 2 && s[0] == '\xD8' && s[1] == '\xB3' /* س */) {
        // s starts with س: prefix ا when the next letter is an s-cluster stop
        std::string rest = s.substr(2);
        if (!rest.empty()) {
            // decode first char crudely: check a few known cluster starts
            static const std::vector<std::string> starts = {"پ","ت","ک","ب","ج","چ","ذ","ژ","ز","ص","ض","ث","ف","گ","س","ش"};
            for (auto& st : starts) if (rest.rfind(st, 0) == 0) { s = "ا" + s; break; }
        }
    }
    return s.empty() ? w : s;
}

void transliterate_into(const std::string& text, std::string& out) {
    size_t i = 0;
    while (i < text.size()) {
        if (is_alpha_ascii(text[i])) {
            size_t j = i + 1;
            while (j < text.size() && is_word_char(text[j])) j++;
            out += transliterate_word(text.substr(i, j - i));
            i = j;
        } else {
            out.push_back(text[i]);
            i++;
        }
    }
}

// ---- phoneme post-map ------------------------------------------------------
std::string to_phonemes(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        switch (c) {
            case '/': out.push_back('a'); break;
            case 'a': out.push_back('A'); break;
            case '@': out.push_back('?'); break;
            case '$': out.push_back('S'); break;
            case 'c': out.push_back('C'); break;
            default: out.push_back(c);
        }
    }
    return out;
}

// G2P degrades on long inputs; split on conjunctions.
const size_t G2P_WINDOW_WORDS = 30;
const std::set<std::string> CUT_WORDS = {"و", "یا", "ولی", "اما", "که", "زیرا", "چون", "پس", "سپس", "بنابراین", "همچنین"};

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        size_t j = i;
        while (j < s.size() && !(s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

void g2p_windows(const std::string& text, std::vector<std::string>& out) {
    std::vector<std::string> w = split_words(text);
    if (w.size() <= G2P_WINDOW_WORDS) { out.push_back(text); return; }
    long long hi = std::min<long long>((long long)w.size() - 4, (long long)G2P_WINDOW_WORDS - 3);
    long long lo = G2P_WINDOW_WORDS / 2;
    long long cut = lo;
    for (long long i = lo; i < hi; i++)
        if (CUT_WORDS.count(w[(size_t)i])) cut = i;
    std::string a, b;
    for (long long i = 0; i < cut; i++) { if (i) a += " "; a += w[(size_t)i]; }
    for (size_t i = (size_t)cut; i < w.size(); i++) { if (i > (size_t)cut) b += " "; b += w[i]; }
    g2p_windows(a, out);
    g2p_windows(b, out);
}

}  // namespace

std::string transliterate_text(const std::string& text) {
    std::string out;
    transliterate_into(text, out);
    return out;
}

G2P::G2P(const std::string& enc_onnx, const std::string& dec_onnx) : enc_(enc_onnx), dec_(dec_onnx) {}

std::string G2P::decode_greedy(const std::string& text) {
    std::vector<int64_t> ids = byt5_encode(text);
    if (ids.empty()) return "";
    std::vector<int64_t> shape_enc = {1, (int64_t)ids.size()};
    OrtValue* in = make_tensor(shape_enc, ids.data(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    std::vector<const char*> names = {"input_ids"};
    std::vector<OrtValue*> feeds = {in};
    std::vector<OrtValue*> outs = enc_.run(names, feeds);
    ort_release(in);
    OrtValue* hidden = outs[0];

    std::vector<int64_t> dec_ids = {PAD};
    for (int step = 0; step < max_len_; step++) {
        std::vector<int64_t> dshape = {1, (int64_t)dec_ids.size()};
        OrtValue* di = make_tensor(dshape, dec_ids.data(), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        std::vector<const char*> dn = {"decoder_input_ids", "encoder_hidden"};
        std::vector<OrtValue*> df = {di, hidden};
        std::vector<OrtValue*> douts = dec_.run(dn, df);
        ort_release(di);
        std::vector<int64_t> lshape = tensor_shape(douts[0]);
        const float* logits = (const float*)tensor_data(douts[0]);
        int64_t S = lshape[1], V = lshape[2];
        const float* last = logits + (S - 1) * V;
        int64_t best = 0;
        for (int64_t v = 1; v < V; v++) if (last[v] > last[best]) best = v;
        ort_release(douts[0]);
        if (best == EOS) break;
        dec_ids.push_back(best);
    }
    ort_release(hidden);
    return byt5_decode(dec_ids, 1);
}

std::string G2P::phonemise(const std::string& text, bool keep_ezafe) {
    std::string t;
    transliterate_into(text, t);
    t = normalize_for_model(t);
    // a trailing colon makes the G2P repeat the final word
    std::string filtered;
    for (size_t i = 0; i < t.size();) {
        // strip ASCII '?' , ':' and Arabic '؟' (U+061F)
        if (t[i] == '?' || t[i] == ':') { i++; continue; }
        if ((unsigned char)t[i] == 0xD8 && i + 1 < t.size() && (unsigned char)t[i + 1] == 0x9F) { i += 2; continue; }
        filtered.push_back(t[i]);
        i++;
    }
    std::vector<std::string> windows;
    g2p_windows(filtered, windows);
    std::string raw;
    for (size_t i = 0; i < windows.size(); i++) { if (i) raw += " "; raw += decode_greedy(windows[i]); }
    std::string out = to_phonemes(raw);
    // bo?d -> ba?d
    std::vector<std::string> words = split_words(out);
    std::string fixed;
    for (size_t i = 0; i < words.size(); i++) {
        if (i) fixed += " ";
        fixed += (words[i] == "bo?d") ? "ba?d" : words[i];
    }
    out = fixed;
    if (!keep_ezafe) {
        std::string no;
        for (char c : out) if (c != '1') no.push_back(c);
        out = no;
    }
    return out;
}

}  // namespace tts
