#include "normalize_fa.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace tts {
namespace {

using U = std::u32string;

// ---- UTF-8 <-> UTF-32 -----------------------------------------------------
U decode_utf8(const std::string& s) {
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

std::string encode_utf8(const U& s) {
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

const char32_t ZWNJ = 0x200C;

const std::set<char32_t>& persian_letters() {
    static const std::set<char32_t> s = [] {
        U letters = decode_utf8(u8"\u0622\u0627\u0628\u067E\u062A\u062B\u062C\u0686\u062D\u062E\u062F\u0630\u0631\u0632\u0698\u0633\u0634\u0635\u0636\u0637\u0638\u0639\u063A\u0641\u0642\u06A9\u06AF\u0644\u0645\u0646\u0648\u0647\u06CC\u0626");
        return std::set<char32_t>(letters.begin(), letters.end());
    }();
    return s;
}

bool is_letter(char32_t c) { return persian_letters().count(c) > 0; }

const std::string KEPT_PUNCT = ".،؛؟!:";
std::set<char32_t> kept_punct_set() {
    static std::set<char32_t> s = [] { U p = decode_utf8(KEPT_PUNCT); return std::set<char32_t>(p.begin(), p.end()); }();
    return s;
}
bool is_space(char32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0C || c == 0x0B; }

bool in_ranges(char32_t c, std::initializer_list<std::pair<char32_t, char32_t>> rs) {
    for (auto& r : rs) if (c >= r.first && c <= r.second) return true;
    return false;
}

std::map<char32_t, char32_t> char_map() {
    static const std::map<char32_t, char32_t> m = {
        {0x064A, 0x06CC}, {0x0649, 0x06CC}, {0x06CD, 0x06CC}, {0x06D0, 0x06CC}, {0x0643, 0x06A9},
        {0x06AA, 0x06A9}, {0x0629, 0x0647}, {0x06C0, 0x0647}, {0x0623, 0x0627}, {0x0625, 0x0627},
        {0x0671, 0x0627}, {0x0672, 0x0627}, {0x0673, 0x0627}, {0x0624, 0x0648}, {0x06C2, 0x0647},
        {0x06C3, 0x0647}, {0x0621, 0},        {0x0640, 0},
    };
    return m;
}

// Persian / Arabic-Indic digits -> ASCII.
char32_t to_ascii_digit(char32_t c) {
    if (c >= 0x06F0 && c <= 0x06F9) return (char32_t)('0' + (c - 0x06F0));
    if (c >= 0x0660 && c <= 0x0669) return (char32_t)('0' + (c - 0x0660));
    return c;
}

const std::vector<std::string> ONES = {"", "یک", "دو", "سه", "چهار", "پنج", "شش", "هفت", "هشت", "نه"};
const std::vector<std::string> TEENS = {"ده", "یازده", "دوازده", "سیزده", "چهارده", "پانزده", "شانزده", "هفده", "هجده", "نوزده"};
const std::vector<std::string> TENS = {"", "", "بیست", "سی", "چهل", "پنجاه", "شصت", "هفتاد", "هشتاد", "نود"};
const std::vector<std::string> HUNDREDS = {"", "صد", "دویست", "سیصد", "چهارصد", "پانصد", "ششصد", "هفتصد", "هشتصد", "نهصد"};
const std::string JOIN = " و ";
struct Scale { long long value; std::string name; };
const std::vector<Scale> SCALES = {{1000000000000LL, "تریلیون"}, {1000000000LL, "میلیارد"}, {1000000LL, "میلیون"}, {1000LL, "هزار"}};
const std::map<int, std::string> FRACTION_UNITS = {{1, "دهم"}, {2, "صدم"}, {3, "هزارم"}};
const size_t MAX_NUMBER_DIGITS = 12;

std::string under_thousand(long long n) {
    std::vector<std::string> parts;
    if (n >= 100) { parts.push_back(HUNDREDS[n / 100]); n %= 100; }
    if (n >= 20) { parts.push_back(TENS[n / 10]); n %= 10; }
    if (n >= 10 && n < 20) { parts.push_back(TEENS[n - 10]); n = 0; }
    if (n > 0) parts.push_back(ONES[n]);
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) { if (i) out += JOIN; out += parts[i]; }
    return out;
}

std::string number_to_words(long long n) {
    if (n == 0) return "صفر";
    std::vector<std::string> parts;
    for (auto& sc : SCALES) {
        if (n >= sc.value) {
            long long count = n / sc.value;
            n %= sc.value;
            std::string head = (count == 1 && sc.value == 1000) ? "" : under_thousand(count) + " ";
            std::string s = head + sc.name;
            // trim
            size_t a = s.find_first_not_of(" "), b = s.find_last_not_of(" ");
            parts.push_back(s.substr(a, b - a + 1));
        }
    }
    if (n > 0) parts.push_back(under_thousand(n));
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) { if (i) out += JOIN; out += parts[i]; }
    return out;
}

std::string digits_one_by_one(const std::string& digits) {
    std::string out;
    for (char d : digits) {
        if (!out.empty()) out += " ";
        out += (d == '0') ? "صفر" : ONES[d - '0'];
    }
    return out;
}

std::string number_token_to_words(const std::string& whole, bool has_frac, const std::string& frac) {
    std::string w = whole;
    bool leading_zero = w.size() > 1 && w[0] == '0';
    if (w.size() > MAX_NUMBER_DIGITS || leading_zero) {
        std::string s = " " + digits_one_by_one(w);
        if (has_frac && !frac.empty()) s += " " + digits_one_by_one(frac);
        s += " ";
        return s;
    }
    size_t a = w.find_first_not_of("0");
    w = (a == std::string::npos) ? "0" : w.substr(a);
    std::string words = number_to_words(std::stoll(w));
    if (has_frac) {
        std::string fr = frac;
        while (!fr.empty() && fr.back() == '0') fr.pop_back();
        if (fr.empty()) return " " + words + " ";
        auto it = FRACTION_UNITS.find((int)fr.size());
        if (it != FRACTION_UNITS.end())
            words += " ممیز " + number_to_words(std::stoll(fr)) + " " + it->second;
        else
            words += " ممیز " + digits_one_by_one(fr);
    }
    return " " + words + " ";
}

// Split a UTF-8 std::string of the (already digit-mapped) text into u32.
U numbers_to_words(const U& text) {
    // 1,234,567 -> 1234567 (only digit-internal separators)
    U t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        char32_t c = text[i];
        if (c == ',' && i > 0 && i + 4 <= text.size() &&
            text[i - 1] >= '0' && text[i - 1] <= '9' &&
            text[i + 1] >= '0' && text[i + 1] <= '9' &&
            text[i + 2] >= '0' && text[i + 2] <= '9' &&
            text[i + 3] >= '0' && text[i + 3] <= '9' &&
            (i + 4 == text.size() || !(text[i + 4] >= '0' && text[i + 4] <= '9')))
            continue;
        t.push_back(c);
    }
    // percent -> " درصد "
    U t2;
    for (char32_t c : t) {
        if (c == '%' || c == 0x066A) { U r = decode_utf8(" درصد "); t2.insert(t2.end(), r.begin(), r.end()); }
        else t2.push_back(c);
    }
    // (\d+)(?:\.(\d+))?
    U out;
    size_t i = 0, n = t2.size();
    while (i < n) {
        if (t2[i] >= '0' && t2[i] <= '9') {
            size_t j = i;
            while (j < n && t2[j] >= '0' && t2[j] <= '9') j++;
            std::string whole;
            for (size_t k = i; k < j; k++) whole.push_back((char)t2[k]);
            bool has_frac = false;
            std::string frac;
            if (j < n && t2[j] == '.' && j + 1 < n && t2[j + 1] >= '0' && t2[j + 1] <= '9') {
                has_frac = true;
                size_t k = j + 1;
                while (k < n && t2[k] >= '0' && t2[k] <= '9') { frac.push_back((char)t2[k]); k++; }
                j = k;
            }
            std::string words = number_token_to_words(whole, has_frac, frac);
            U r = decode_utf8(words);
            out.insert(out.end(), r.begin(), r.end());
            i = j;
        } else {
            out.push_back(t2[i]);
            i++;
        }
    }
    return out;
}

std::map<char32_t, char32_t> punct_map() {
    static const std::map<char32_t, char32_t> m = {
        {',', 0x060C}, {'?', 0x061F}, {';', 0x061B}, {0x066C, 0}, {0x066B, '.'}, {0x2026, '.'},
        {0x00BB, ' '}, {0x00AB, ' '}, {0x201C, ' '}, {0x201D, ' '}, {0x2018, ' '}, {0x2019, ' '},
        {'"', ' '}, {'\'', ' '}, {'`', ' '}, {'(', ' '}, {')', ' '}, {'[', ' '}, {']', ' '},
        {'{', ' '}, {'}', ' '}, {0x2013, ' '}, {0x2014, ' '}, {0x2212, ' '}, {'-', ' '}, {0x2010, ' '},
        {'/', ' '}, {'\\', ' '}, {'*', ' '}, {'_', ' '}, {'|', ' '}, {0x00D7, ' '}, {'=', ' '}, {'+', ' '},
        {0x060C, 0x060C}, {0x061B, 0x061B}, {0x061F, 0x061F},
    };
    return m;
}

std::string normalize_impl(const U& in) {
    const auto& cm = char_map();
    const auto& pm = punct_map();
    const auto& letters = persian_letters();
    const auto& kp = kept_punct_set();

    // strip invisibles + diacritics, fold letters, map digits
    U t;
    t.reserve(in.size());
    for (char32_t c : in) {
        if (c == 0x200B || c == 0x200D || c == 0x200E || c == 0x200F ||
            (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
            c == 0xFEFF || c == 0x00AD) continue;
        if (in_ranges(c, {{0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06ED}})) continue;
        auto it = cm.find(c);
        if (it != cm.end()) { if (it->second) t.push_back(it->second); continue; }
        t.push_back(to_ascii_digit(c));
    }
    // number-internal separators before reading digit runs
    U t3;
    for (char32_t c : t) {
        if (c == 0x066C) continue;          // ٬
        t3.push_back(c == 0x066B ? '.' : c); // ٫ -> .
    }
    U t4 = numbers_to_words(t3);
    // punctuation map
    U t5;
    for (char32_t c : t4) {
        auto it = pm.find(c);
        t5.push_back(it != pm.end() ? it->second : c);
    }
    // anything outside the allowed set -> space
    U t6;
    for (char32_t c : t5) {
        bool allowed = letters.count(c) || c == ZWNJ || kp.count(c) || c == ' ';
        t6.push_back(allowed ? c : ' ');
    }
    // ZWNJ+ -> single
    U t7;
    for (char32_t c : t6) {
        if (c == ZWNJ && !t7.empty() && t7.back() == ZWNJ) continue;
        t7.push_back(c);
    }
    // \s*ZWNJ\s* -> ZWNJ if the match had no space, else ' '
    U t8;
    {
        size_t i = 0, n = t7.size();
        while (i < n) {
            size_t j = i;
            bool has_space = false;
            while (j < n && is_space(t7[j])) { has_space = true; j++; }
            if (j < n && t7[j] == ZWNJ) {
                j++;
                size_t k = j;
                while (k < n && is_space(t7[k])) { has_space = true; k++; }
                t8.push_back(has_space ? ' ' : ZWNJ);
                i = k;
            } else {
                t8.push_back(t7[i]);
                i++;
            }
        }
    }
    // ZWNJ not between two letters -> remove
    U t9;
    for (size_t i = 0; i < t8.size(); i++) {
        if (t8[i] == ZWNJ) {
            bool left = i > 0 && letters.count(t8[i - 1]);
            bool right = i + 1 < t8.size() && letters.count(t8[i + 1]);
            if (!(left && right)) continue;
        }
        t9.push_back(t8[i]);
    }
    // \s+([punct]) -> punct
    U t10;
    for (size_t i = 0; i < t9.size(); i++) {
        if (is_space(t9[i])) {
            size_t j = i;
            while (j < t9.size() && is_space(t9[j])) j++;
            if (j < t9.size() && kp.count(t9[j])) { i = j - 1; continue; }  // drop spaces, keep punct at j
            for (size_t k = i; k < j; k++) t10.push_back(' ');
            i = j - 1;
        } else t10.push_back(t9[i]);
    }
    // ([punct])\1+ -> single
    U t11;
    for (char32_t c : t10) {
        if (!t11.empty() && t11.back() == c && kp.count(c)) continue;
        t11.push_back(c);
    }
    // \s+ -> ' ', strip
    U t12;
    for (char32_t c : t11) {
        if (is_space(c)) { if (!t12.empty() && t12.back() != ' ') t12.push_back(' '); }
        else t12.push_back(c);
    }
    while (!t12.empty() && t12.back() == ' ') t12.pop_back();
    // lstrip(KEPT_PUNCT + " ")
    size_t start = 0;
    while (start < t12.size() && (is_space(t12[start]) || kp.count(t12[start]))) start++;
    U t13(t12.begin() + start, t12.end());
    return encode_utf8(t13);
}


}  // namespace

bool is_phonemic(const std::string& text) {
    U u = decode_utf8(text);
    bool has_persian = false, has_ascii_alpha = false;
    for (char32_t c : u) {
        if (is_letter(c)) has_persian = true;
        if (c < 128 && std::isalpha((int)c)) has_ascii_alpha = true;
    }
    return !has_persian && has_ascii_alpha;
}

std::string normalize_text(const std::string& text) { return normalize_impl(decode_utf8(text)); }

bool fa_has_alnum(const std::string& word) {
    for (char32_t c : decode_utf8(word)) {
        if (is_letter(c)) return true;
        if (c < 128 && std::isalnum((int)c)) return true;
        if (c >= 0x06F0 && c <= 0x06F9) return true;
        if (c >= 0x0660 && c <= 0x0669) return true;
    }
    return false;
}

std::string normalize_for_model(const std::string& text) {
    if (is_phonemic(text)) return text;
    std::string out = normalize_text(text);
    bool text_nonempty = text.find_first_not_of(" \t\n\r") != std::string::npos;
    bool out_empty = out.find_first_not_of(" \t\n\r") == std::string::npos;
    if (text_nonempty && out_empty)
        throw std::runtime_error("normalisation emptied the text: " + text.substr(0, 60));
    return out;
}

}  // namespace tts
