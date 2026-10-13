#include "chunk.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "normalize_fa.h"
#include "dict.h"

namespace tts {
namespace {

using U = std::u32string;

U u32(const std::string& s) {
    U out;
    size_t i = 0, n = s.size();
    while (i < n) {
        uint32_t c = (uint8_t)s[i];
        if (c < 0x80) { out.push_back(c); i += 1; }
        else if ((c >> 5) == 0x6 && i + 1 < n) { out.push_back(((c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F)); i += 2; }
        else if ((c >> 4) == 0xE && i + 2 < n) { out.push_back(((c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F)); i += 3; }
        else if ((c >> 3) == 0x1E && i + 3 < n) { out.push_back(((c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) | (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F)); i += 4; }
        else { out.push_back(0xFFFD); i += 1; }
    }
    return out;
}

std::string u8(const U& s) {
    std::string out;
    for (char32_t c : s) {
        uint32_t v = (uint32_t)c;
        if (v < 0x80) out.push_back((char)v);
        else if (v < 0x800) { out.push_back((char)(0xC0 | (v >> 6))); out.push_back((char)(0x80 | (v & 0x3F))); }
        else if (v < 0x10000) { out.push_back((char)(0xE0 | (v >> 12))); out.push_back((char)(0x80 | ((v >> 6) & 0x3F))); out.push_back((char)(0x80 | (v & 0x3F))); }
        else { out.push_back((char)(0xF0 | (v >> 18))); out.push_back((char)(0x80 | ((v >> 12) & 0x3F))); out.push_back((char)(0x80 | ((v >> 6) & 0x3F))); out.push_back((char)(0x80 | (v & 0x3F))); }
    }
    return out;
}

bool is_ws(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0C; }

std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    U u = u32(s);
    size_t i = 0;
    while (i < u.size()) {
        while (i < u.size() && is_ws(u[i])) i++;
        size_t j = i;
        while (j < u.size() && !is_ws(u[j])) j++;
        if (j > i) out.push_back(u8(U(u.begin() + i, u.begin() + j)));
        i = j;
    }
    return out;
}

int letter_words(const std::string& tp) {
    int n = 0;
    for (auto& w : words_of(tp)) if (fa_has_alnum(w)) n++;
    return n;
}

bool ends_with_any(const std::string& s, const std::vector<std::string>& suffixes) {
    std::string t = s;
    while (!t.empty() && is_ws((char32_t)(unsigned char)t.back())) t.pop_back();
    for (auto& suf : suffixes) {
        if (t.size() >= suf.size() && t.compare(t.size() - suf.size(), suf.size(), suf) == 0) return true;
    }
    return false;
}

std::string strip_chars(const std::string& w, const std::string& set) {
    U u = u32(w), setu = u32(set);
    std::set<char32_t> ss(setu.begin(), setu.end());
    size_t a = 0, b = u.size();
    while (a < b && ss.count(u[a])) a++;
    while (b > a && ss.count(u[b - 1])) b--;
    return u8(U(u.begin() + a, u.begin() + b));
}

std::string remove_zwnj(const std::string& w) {
    std::string out;
    for (size_t i = 0; i < w.size();) {
        if ((unsigned char)w[i] == 0xE2 && i + 2 < w.size() && (unsigned char)w[i + 1] == 0x80 && (unsigned char)w[i + 2] == 0x8C) { i += 3; continue; }
        out.push_back(w[i]);
        i++;
    }
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += " "; out += v[i]; }
    return out;
}

// ---- text-layer sets -------------------------------------------------------
const std::vector<std::string> STRONG_LEADIN = {":", "؛", "\u2014", "\u2013"};
const std::set<std::string> TEXT_PREPS = {"برای", "بدون", "درباره", "مثل"};
const std::set<std::string> TEXT_LIGHT_VERBS = {
    "است", "هست", "هستم", "هستی", "هستیم", "هستید", "هستند",
    "بود", "بودم", "بودی", "بودیم", "بودید", "بودند", "باشد", "باشند",
    "شد", "شدم", "شدی", "شدیم", "شدید", "شدند", "شود", "شوند",
    "کرد", "کردم", "کردی", "کردیم", "کردید", "کردند", "کرده",
    "کنم", "کنی", "کند", "کنیم", "کنید", "کنند",
    "داد", "دادم", "دادی", "دادیم", "دادید", "دادند", "داده",
    "بدهد", "بدهند",
    "میشود", "میشوند", "میکند", "میکنند", "میکرد", "میکردند",
    "میداد", "میدادند", "میدهد", "میدهند",
    "دارد", "دارم", "داری", "داریم", "دارید", "دارند", "داشت",
    "میباشد", "میباشند"};

// ---- phoneme-layer sets ----------------------------------------------------
const std::set<std::string> PREPS = {"?az", "bA", "dar", "be", "barAye", "tA", "ruye", "bedune", "vase"};
const std::set<std::string> FUNCTION_WORDS = {
    "dar", "be", "?az", "tA", "va", "ke", "bA", "bedune", "age", "vali",
    "yA", "barAye", "vase", "dAr", "mi", "beture", "besurate"};
const std::set<std::string> CONJUNCTIONS = {"va", "yA", "vali", "amA", "hattA", "ke", "rA"};
const std::set<std::string> LIGHT_VERBS = {
    "miSavad", "miSavand", "miSavam", "miSavid", "miSavim", "Savad", "Savand", "Sod", "Sodand", "Sodan",
    "mikonad", "mikonand", "mikonam", "mikonid", "mikonim", "kard", "karde", "konad", "konand",
    "dAd", "dAde", "dAdand", "dAdam", "dAdi", "dAdim", "dAdid", "dAhad", "dAhand", "midAd", "midAdand",
    "dArad", "dArand", "dAsht", "Ast", "?ast", "bud", "budand"};

const int CHUNK_TOKENS = 18, CHUNK_HEADROOM = 2, TAIL_MERGE_CAP = 19;
const int MIN_CHUNK_WORDS = 3, MIN_CHUNK_TOKENS = 6, RUNT_TAIL_TOKENS = 2;
const int LOOKBACK_LIGHTVERB = 5, LOOKBACK_PREP = 3;

// ---- text splitting --------------------------------------------------------
bool is_sentence_end(char32_t c) { return c == '.' || c == '!' || c == '?' || c == 0x061F; }
bool is_phrase_punct(char32_t c) {
    return c == 0x060C || c == 0x061B || c == ':' || c == 0x2014 || c == 0x2013 ||
           c == ',' || c == ';' || c == ')';
}

std::vector<std::string> split_by(const std::string& text, bool sentences) {
    U u = u32(text);
    std::vector<std::string> out;
    std::string cur;
    U curu;
    size_t i = 0;
    while (i < u.size()) {
        if (is_ws(u[i])) {
            size_t j = i;
            while (j < u.size() && is_ws(u[j])) j++;
            bool split = false;
            if (!curu.empty()) {
                char32_t prev = curu.back();
                if (sentences) split = is_sentence_end(prev);
                else split = is_phrase_punct(prev) || (j < u.size() && u[j] == '(');
            }
            if (split) {
                std::string piece = u8(curu);
                // trim
                size_t a = piece.find_first_not_of(" \t\n\r");
                size_t b = piece.find_last_not_of(" \t\n\r");
                if (a != std::string::npos) out.push_back(piece.substr(a, b - a + 1));
                curu.clear();
                i = j;
                continue;
            }
            curu.push_back(' ');
            i = j;
        } else {
            curu.push_back(u[i]);
            i++;
        }
    }
    if (!curu.empty()) {
        std::string piece = u8(curu);
        size_t a = piece.find_first_not_of(" \t\n\r");
        size_t b = piece.find_last_not_of(" \t\n\r");
        if (a != std::string::npos) out.push_back(piece.substr(a, b - a + 1));
    }
    if (sentences) return out;
    // phrase mode: drop debris with no letters/digits
    std::vector<std::string> kept;
    for (auto& p : out) if (letter_words(p)) kept.push_back(p);
    return kept;
}

}  // namespace

std::vector<std::string> split_sentences(const std::string& text) {
    std::string t = text;
    size_t a = t.find_first_not_of(" \t\n\r");
    size_t b = t.find_last_not_of(" \t\n\r");
    if (a == std::string::npos) return {};
    return split_by(t.substr(a, b - a + 1), true);
}

std::vector<Phrase> plan_phrases(const std::string& sentence, G2P& g2p, const Spm& sp,
                                 const Dictionary* dict, bool keep_ezafe, int chunk_tokens) {
    if (chunk_tokens <= 0) chunk_tokens = CHUNK_TOKENS;
    auto split_phrases = [](const std::string& s) { return split_by(s, false); };

    auto protected_strong = [&](const std::string& p, const std::function<bool(const std::string&)>* keep) {
        if (!ends_with_any(p, STRONG_LEADIN)) return false;
        if (keep && !(*keep)(p)) return false;
        return true;
    };
    // clean(tp): lead-in may stand alone only if phoneme word count == text word count
    std::map<std::string, bool> cache;
    std::function<bool(const std::string&)> clean = [&](const std::string& tp) -> bool {
        auto it = cache.find(tp);
        if (it != cache.end()) return it->second;
        std::string trans = transliterate_text(tp);
        std::string norm = normalize_for_model(trans);
        // strip ؟ ? :
        std::string cleaned;
        for (size_t i = 0; i < norm.size();) {
            if (norm[i] == '?' || norm[i] == ':') { i++; continue; }
            if ((unsigned char)norm[i] == 0xD8 && i + 1 < norm.size() && (unsigned char)norm[i + 1] == 0x9F) { i += 2; continue; }
            cleaned.push_back(norm[i]);
            i++;
        }
        int want = letter_words(cleaned);
        std::string ph = phonemise_phrase(g2p, dict, tp, keep_ezafe);
        bool ok = !ph.empty() && letter_words(ph) == want;
        cache[tp] = ok;
        return ok;
    };

    // merge_short_phrases(min_words=2, keep_standalone=clean)
    std::vector<std::string> merged;
    for (auto& p : split_phrases(sentence)) {
        if (!merged.empty() && letter_words(merged.back()) < 2 &&
            !protected_strong(merged.back(), &clean))
            merged.back() += " " + p;
        else
            merged.push_back(p);
    }
    if (merged.size() >= 2 && letter_words(merged.back()) < 2 &&
        !protected_strong(merged[merged.size() - 2], &clean)) {
        std::string last = merged.back();
        merged.pop_back();
        merged.back() += " " + last;
    }

    // merge_leading_light_verbs
    std::vector<std::string> m2;
    for (auto& p : merged) {
        std::vector<std::string> ws = words_of(p);
        std::string first;
        if (!ws.empty()) {
            std::string f = ws[0];
            // stripped of the punctuation set, but keep ZWNJ for now
            std::string set = "«»()\"'.,;:!?،؛:-";
            // '«' etc are multibyte; strip_chars handles codepoints
            first = strip_chars(f, set + "\u00AB\u00BB");
            first = remove_zwnj(first);
        }
        if (!m2.empty() && (TEXT_LIGHT_VERBS.count(first) || first == "را"))
            m2.back() += " " + p;
        else
            m2.push_back(p);
    }

    std::function<std::vector<std::string>(const std::string&, int)> split_long =
        [&](const std::string& tp, int max_words) -> std::vector<std::string> {
        std::vector<std::string> words = words_of(tp);
        if ((int)words.size() <= max_words) return {tp};
        int cut = -1;
        for (int i = 0; i < (int)words.size(); i++) {
            if (TEXT_PREPS.count(words[i]) && i >= 4 && (int)words.size() - i >= 3) cut = i;
        }
        if (cut < 0) return {tp};
        std::vector<std::string> a = split_long(join(std::vector<std::string>(words.begin(), words.begin() + cut)), max_words);
        std::vector<std::string> b = split_long(join(std::vector<std::string>(words.begin() + cut, words.end())), max_words);
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };

    std::vector<Phrase> out;
    std::string prev_tp;
    bool have_prev = false;
    for (auto& tp : m2) {
        std::string ph_tp = phonemise_phrase(g2p, dict, tp, keep_ezafe);
        std::vector<std::string> parts;
        if (!ph_tp.empty() && (int)sp.encode(ph_tp).size() <= chunk_tokens + CHUNK_HEADROOM)
            parts = {tp};
        else
            parts = split_long(tp, 9);
        for (auto& q : parts) {
            std::string ph = (q == tp) ? ph_tp : phonemise_phrase(g2p, dict, q, keep_ezafe);
            if (ph.empty()) continue;
            bool strong = have_prev && ends_with_any(prev_tp, STRONG_LEADIN);
            out.push_back({ph, strong ? STRONG_GAP : PHRASE_GAP});
            prev_tp = q;
            have_prev = true;
        }
    }
    return out;
}

std::vector<Phrase> pack_phrases(const std::vector<Phrase>& plan) {
    std::vector<Phrase> groups;
    for (auto& p : plan) {
        if (!groups.empty() && p.gap != STRONG_GAP)
            groups.back().phonemes += " " + p.phonemes;
        else
            groups.push_back(p);
    }
    return groups;
}

namespace {

std::vector<std::string> pack_words(const std::vector<std::string>& words, const Spm& sp,
                                    int max_tokens, int slack) {
    std::vector<std::string> chunks, cur;
    for (auto& w : words) {
        std::vector<std::string> cand = cur;
        cand.push_back(w);
        int n = (int)sp.encode(join(cand)).size();
        bool over = !cur.empty() && n > max_tokens + slack;
        bool bound = (!cur.empty() && cur.back().size() && cur.back().back() == '1') ||
                     LIGHT_VERBS.count(w) || w == "rA";
        if (over && !(bound && n <= max_tokens + CHUNK_HEADROOM)) {
            int cut = (int)cur.size();
            for (int k = (int)cur.size() - 1; k >= 0 && k > (int)cur.size() - 1 - LOOKBACK_LIGHTVERB; k--) {
                if (LIGHT_VERBS.count(cur[k]) || cur[k] == "rA") { cut = k + 1; break; }
            }
            if (cut == (int)cur.size()) {
                for (int k = (int)cur.size() - 1; k >= 0 && k > (int)cur.size() - 1 - LOOKBACK_PREP; k--) {
                    if (PREPS.count(cur[k]) && (int)cur.size() - k <= 2) { cut = k; break; }
                }
            }
            bool small = cut >= MIN_CHUNK_WORDS &&
                         (int)sp.encode(join(std::vector<std::string>(cur.begin(), cur.begin() + cut))).size() >= MIN_CHUNK_TOKENS;
            if (small) {
                chunks.push_back(join(std::vector<std::string>(cur.begin(), cur.begin() + cut)));
                cur = std::vector<std::string>(cur.begin() + cut, cur.end());
            } else {
                chunks.push_back(join(cur));
                cur.clear();
            }
            cur.push_back(w);
        } else {
            cur.push_back(w);
        }
    }
    if (!cur.empty()) chunks.push_back(join(cur));
    return chunks;
}

std::vector<std::string> fix_boundaries(std::vector<std::string> chunks, int min_words) {
    int i = 1;
    while (i < (int)chunks.size()) {
        std::vector<std::string> prev = words_of(chunks[i - 1]);
        std::vector<std::string> nxt = words_of(chunks[i]);
        bool left_bound = prev.size() >= 2 &&
            ((prev[prev.size() - 2].size() && prev[prev.size() - 2].back() == '1') ||
             LIGHT_VERBS.count(prev.back()) || prev.back() == "rA");
        bool bad = false;
        if ((int)prev.size() > min_words) {
            const std::string& pl = prev.back();
            if (!pl.empty() && pl.back() == '1') bad = true;
            else if (!left_bound && !nxt.empty() && nxt[0].size() && nxt[0].back() == '1') bad = true;
            else if (!left_bound && FUNCTION_WORDS.count(pl)) bad = true;
            else if (!left_bound && !nxt.empty() && CONJUNCTIONS.count(nxt[0])) bad = true;
            else if (!left_bound && !pl.empty() && pl.back() == 'e' && !nxt.empty() && !nxt[0].empty() && nxt[0][0] == '?') bad = true;
        }
        if (bad) {
            std::string last = prev.back();
            prev.pop_back();
            chunks[i - 1] = join(prev);
            chunks[i] = last + " " + chunks[i];
        } else {
            i++;
        }
    }
    return chunks;
}

std::vector<std::string> enforce_budget(std::vector<std::string> chunks, const Spm& sp, int max_tokens) {
    for (int iter = 0; iter < 3; iter++) {
        bool ok = true;
        for (auto& c : chunks) if ((int)sp.encode(c).size() > max_tokens + CHUNK_HEADROOM) { ok = false; break; }
        if (ok) return chunks;
        std::vector<std::string> repacked;
        for (auto& c : chunks) {
            if ((int)sp.encode(c).size() <= max_tokens + CHUNK_HEADROOM) {
                repacked.push_back(c);
            } else {
                std::vector<std::string> r = pack_words(words_of(c), sp, max_tokens, CHUNK_HEADROOM);
                repacked.insert(repacked.end(), r.begin(), r.end());
            }
        }
        chunks = fix_boundaries(repacked, 3);
    }
    return chunks;
}

}  // namespace

std::vector<std::string> chunk_phonemes(const std::string& phonemes, const Spm& sp, int max_tokens) {
    std::vector<std::string> words = words_of(phonemes);
    auto pack = [&](int slack) -> std::vector<std::string> {
        std::vector<std::string> c = pack_words(words, sp, max_tokens, slack);
        c = fix_boundaries(c, 3);
        c = enforce_budget(c, sp, max_tokens);
        // runt trailing chunk (1-2 tokens) merges into the previous one
        if (c.size() >= 2) {
            int tail = (int)sp.encode(c.back()).size();
            int merged = (int)sp.encode(c[c.size() - 2] + " " + c.back()).size();
            if (tail <= RUNT_TAIL_TOKENS && merged <= TAIL_MERGE_CAP) {
                c[c.size() - 2] += " " + c.back();
                c.pop_back();
            }
        }
        return c;
    };
    std::vector<std::string> chunks = pack(CHUNK_HEADROOM);
    if (chunks.size() >= 2 &&
        (letter_words(chunks.back()) < MIN_CHUNK_WORDS ||
         (int)sp.encode(chunks.back()).size() < MIN_CHUNK_TOKENS)) {
        chunks = pack(0);
    }
    for (auto& c : chunks) {
        std::string out;
        for (char ch : c) if (ch != '1') out.push_back(ch);
        c = out;
    }
    return chunks;
}

}  // namespace tts
