// farsi-tts-gui — native Win32 GUI (no browser, no runtime beyond the CRTs).
//
// Works on Windows 7: plain user32/comctl32/comdlg32/winmm/wininet, the
// Win7-era Common Controls v6 (themed via cmake/gui.rc), and the bundled
// Sahel font (embedded in the exe, or the sahel.ttf next to it) so every
// Persian label renders with it.
//
// Five tabs — text, phonemes (آوانگاشت), dictionary, parameters, model
// download — so every knob the CLI and the HTTP API expose (--pace, --mode,
// --seed, --temperature, --retries, --chunk-tokens, --pause-scale,
// --keep-ezafe, --zero-noise, dictionary) is reachable from the GUI too.
//
// Two first-class languages (Persian / English, switch at runtime), an input
// box that follows the text direction automatically (IMF_AUTORTL), Ctrl+A in
// every edit box, and a reference-voice policy of "load once, reuse for every
// synthesis, offload when another voice is picked" (see ID_VOICE handler).
//
// Build with /utf-8 (see CMakeLists.txt) so the wide Persian string literals
// below are decoded correctly.
#ifdef _WIN32

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wininet.h>
#include <stdio.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "sahel_font.h"
#include "tts.h"

// The plain EDIT control understands the Rich Edit language options (they are
// simply not exported by every SDK header): IMF_AUTORTL makes each paragraph
// follow the direction of its first strong character, so Persian runs render
// right-to-left and Latin runs left-to-right in the SAME box.
#ifndef EM_SETLANGOPTIONS
#define EM_SETLANGOPTIONS 0x00CC
#endif
#ifndef IMF_AUTORTL
#define IMF_AUTORTL 0x0040
#endif

namespace fs = std::filesystem;

#define ID_TEXT     1001
#define ID_VOICE    1002
#define ID_ADDBTN   1003
#define ID_MODE     1004
#define ID_PACE     1005
#define ID_GO       1006
#define ID_SAVE     1007
#define ID_STATUS   1008
#define ID_PLAY     1009
#define ID_TAB      1010
#define ID_PRELOAD  1011
#define ID_PACEVAL  1012
#define ID_LANG     1013

#define ID_PH       1101
#define ID_EXTRACT  1102
#define ID_USEPH    1103
#define ID_CHUNKS   1104

#define ID_DICT     1201
#define ID_DICTSAVE 1202
#define ID_DICTLOAD 1203
#define ID_DICTINFO 1204

#define ID_SEED      1301
#define ID_SEEDRAND  1302
#define ID_TEMP      1303
#define ID_TEMPVAL   1304
#define ID_RETRIES   1305
#define ID_CHUNKTOK  1306
#define ID_CHUNKVAL  1307
#define ID_PAUSE     1308
#define ID_PAUSEVAL  1309
#define ID_THREADS   1310
#define ID_ZERONOISE 1311
#define ID_EZAFE     1312
#define ID_PARAMINFO 1313
#define ID_FORCETEXT 1314

// tab 4 — model download (models.json)
#define ID_DLLIST    1401
#define ID_DLBAR     1402
#define ID_DLGET     1403
#define ID_DLGETALL  1404
#define ID_DLCANCEL  1405
#define ID_DLRELOAD  1406
#define ID_DLSTATUS  1407
#define ID_DLJSON    1408

#define IDM_SELALL   2001   // Ctrl+A in any edit box

#define WM_APP_TASK     (WM_APP + 1)   // wParam = task id, success
#define WM_APP_TASKFAIL (WM_APP + 2)   // wParam = task id, failure
#define WM_APP_ENGINE   (WM_APP + 3)   // engine finished loading
#define WM_APP_DLPROG   (WM_APP + 4)   // wParam = bytes done, lParam = total
#define WM_APP_DLDONE   (WM_APP + 5)   // wParam = 0 ok, 1 failed, 2 cancelled

enum Task { TASK_SYNTH = 1, TASK_PHONEMES = 2, TASK_VOICE = 3 };

// One downloadable file from models.json.
struct ModelItem {
    std::string name;   // file name, e.g. flow_lm_step.onnx
    std::string url;    // where it comes from (huggingface resolve URL)
    std::string dir;    // where it lands, relative to the base dir
    std::string kind;   // "model" | "voice" | "dict"
    std::string desc;   // one line of description (Persian)
    long long size = 0; // expected bytes (0 = unknown, progress shows %)
    bool have = false;  // already on disk when the list was built
};

struct AppState {
    // tab 0 — text
    HWND text = nullptr, voice = nullptr, mode = nullptr, pace = nullptr, paceval = nullptr;
    HWND go = nullptr, play = nullptr, save = nullptr, addbtn = nullptr, preload = nullptr;
    // tab 1 — phonemes
    HWND ph = nullptr, extract = nullptr, useph = nullptr, chunks = nullptr;
    // tab 2 — dictionary
    HWND dict = nullptr, dictsave = nullptr, dictload = nullptr, dictinfo = nullptr;
    // tab 3 — parameters
    HWND seed = nullptr, seedrand = nullptr, temp = nullptr, tempval = nullptr;
    HWND retries = nullptr, chunktok = nullptr, chunkval = nullptr, pause = nullptr, pauseval = nullptr;
    HWND threads = nullptr, znoise = nullptr, ezafe = nullptr, forcetext = nullptr, paraminfo = nullptr;
    // tab 4 — model download
    HWND dlList = nullptr, dlBar = nullptr, dlGet = nullptr, dlGetAll = nullptr;
    HWND dlCancel = nullptr, dlReload = nullptr, dlStatus = nullptr, dlJson = nullptr;
    // chrome
    HWND tab = nullptr, status = nullptr, langsel = nullptr;
    HWND pages[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    HWND hints[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

    tts::Engine* eng = nullptr;
    fs::path base, voices, uploads, modelsJson;
    std::vector<std::string> voiceList;   // relative, e.g. voices/x.wav
    std::vector<float> audio;
    std::vector<char> wav;                // last render, in memory
    std::string phText, lastPhonemes;
    std::vector<std::string> chunkList;
    // quality-gate outcome of the last run (see SynthResult): a non-zero
    // quietChunks means that many chunks stayed silent after every retry
    int totalChunks = 0, quietChunks = 0;
    long long voiceMs = 0;
    std::mutex mu;
    std::atomic<bool> busy{false};
    std::atomic<bool> loading{false};
    bool engineReady = false;
    double modelTemp = 0.3;
    int tabSel = 0;

    // language + downloads
    int lang = 0;                          // 0 = فارسی, 1 = English
    std::vector<ModelItem> models;
    std::atomic<bool> dlAbort{false};
    std::atomic<bool> dlActive{false};
    std::atomic<bool> dlBatch{false};      // "download all" is running
    int dlIndex = -1;                      // item currently downloading
};

static AppState g;
static HFONT g_font = nullptr, g_fontSm = nullptr, g_fontBig = nullptr;
static HBRUSH g_brush = nullptr;
static wchar_t g_face[LF_FACESIZE] = L"Tahoma";
static HACCEL g_accel = nullptr;
static HWND g_hwnd = nullptr;   // main window (for tasks started outside WndProc)

static void run_preload();
static void run_engine_load(HWND hwnd);

// --- strings (Persian / English) -------------------------------------------
// One row per string; T() picks the language. Status lines that need printf
// formatting use langfmt() instead so both languages stay side by side.
enum StrId {
    S_TITLE = 0, S_TAB0, S_TAB1, S_TAB2, S_TAB3, S_TAB4,
    S_VOICE_CAP, S_MODE_CAP, S_PACE_CAP, S_MODE_SPLIT, S_MODE_PACK,
    S_GO, S_PLAY, S_SAVE, S_ADDVOICE, S_PRELOAD, S_HINT0,
    S_PH_HINT, S_EXTRACT, S_USEPH, S_LASTPH, S_CHUNK_CAP,
    S_DICT_HINT, S_DICT_SAVE, S_DICTLOAD,
    S_SEED, S_SEED_RAND, S_TEMP, S_RETRIES, S_CHUNKTOK, S_PAUSE,
    S_THREADS, S_AUTO, S_ZNOISE, S_EZAFE, S_PARAMINFO, S_FORCETEXT,
    S_DL_HINT, S_DL_GET, S_DL_GETALL, S_DL_CANCEL, S_DL_RELOAD, S_DL_JSON,
    S_DEFAULT_TEXT, S__COUNT
};

struct L10n { const wchar_t* fa; const wchar_t* en; };
static const L10n STR[S__COUNT] = {
    /* S_TITLE      */ {L"پارسی‌گو — تبدیل متن فارسی به گفتار", L"Parsi-go — Persian text-to-speech"},
    /* S_TAB0       */ {L"متن و ساخت", L"Text and speech"},
    /* S_TAB1       */ {L"آوانگاشت", L"Phonemes"},
    /* S_TAB2       */ {L"واژه‌نامه", L"Dictionary"},
    /* S_TAB3       */ {L"تنظیمات", L"Settings"},
    /* S_TAB4       */ {L"دانلود مدل", L"Model download"},
    /* S_VOICE_CAP  */ {L"صدای مرجع", L"Reference voice"},
    /* S_MODE_CAP   */ {L"حالت گفتار", L"Speech mode"},
    /* S_PACE_CAP   */ {L"سرعت گفتار", L"Speech pace"},
    /* S_MODE_SPLIT */ {L"با مکث ویرگول‌ها (split)", L"Pause at commas (split)"},
    /* S_MODE_PACK  */ {L"یکپارچه و روان (pack)", L"One breath (pack)"},
    /* S_GO         */ {L"ساخت گفتار", L"Synthesise"},
    /* S_PLAY       */ {L"پخش", L"Play"},
    /* S_SAVE       */ {L"ذخیرهٔ WAV…", L"Save WAV…"},
    /* S_ADDVOICE   */ {L"افزودن صدا…", L"Add voice…"},
    /* S_PRELOAD    */ {L"پیش‌بارگذاری صدا", L"Preload voice"},
    /* S_HINT0      */ {L"صدای مرجع یک‌بار رمزنگاری می‌شود و برای همهٔ ساخت‌های بعدی استفاده می‌شود؛ "
                         L"با انتخاب صدای دیگر، صدای پیشین از حافظه رها می‌شود.",
                        L"The reference voice is encoded once and reused for every later synthesis; "
                        "picking another voice offloads the previous one."},
    /* S_PH_HINT    */ {L"آوانگاشتِ متنِ تبِ «متن و ساخت» را ببینید، ویرایش کنید و با همان‌ها بسازید (بدون G2P).",
                        L"See and edit the phonemes of the text on the first tab, then synthesise from them (no G2P)."},
    /* S_EXTRACT    */ {L"استخراج آوانگاشت", L"Extract phonemes"},
    /* S_USEPH      */ {L"ساخت از روی همین آوانگاشت‌ها", L"Synthesise from these phonemes"},
    /* S_LASTPH     */ {L"آخرین آوانگاشت‌های ساخت‌شده اینجا نمایش داده می‌شود.", L"The last generated phonemes appear here."},
    /* S_CHUNK_CAP  */ {L"برنامهٔ چانک‌ها (هر سطر = یک چانک مدل):", L"Chunk plan (one model chunk per line):"},
    /* S_DICT_HINT  */ {L"هر سطر: واژه و آوانگاشت‌ها که با کلید جدول (Tab) از هم جدا می‌شوند — مثلاً «بُردارسازی\tbordArAzi». "
                         L"جست‌وجو به حروف کوچک، نشانه‌گذاری و اعراب کاری ندارد. # کامنت است.",
                        L"Each line: word and phonemes separated by TAB — e.g. \"بُردارسازی\\tbordArAzi\". "
                        "Lookup ignores case, punctuation and diacritics. # starts a comment."},
    /* S_DICT_SAVE  */ {L"ذخیرهٔ واژه‌نامه", L"Save dictionary"},
    /* S_DICTLOAD   */ {L"بازخوانی", L"Reload"},
    /* S_SEED       */ {L"بذر (seed) — خالی = تصادفی", L"Seed — empty = random"},
    /* S_SEED_RAND  */ {L"تصادفی", L"Random"},
    /* S_TEMP       */ {L"دما (temperature) — پیش‌فرض مدل", L"Temperature — model default"},
    /* S_RETRIES    */ {L"تلاش دوباره (retries) ۱…۴", L"Retries 1…4"},
    /* S_CHUNKTOK   */ {L"بودجهٔ چانک (chunk-tokens) ۸…۳۲", L"Chunk budget (chunk-tokens) 8…32"},
    /* S_PAUSE      */ {L"مقیاس مکث (pause-scale) ۰…۳", L"Pause scale 0…3"},
    /* S_THREADS    */ {L"رشتهٔ ONNX (threads) — خودکار", L"ONNX threads — auto"},
    /* S_AUTO       */ {L"خودکار", L"Auto"},
    /* S_ZNOISE     */ {L"نویز صفر (فقط آزمون — ممکن است بی‌صدا شود)", L"Zero noise (testing only — may go silent)"},
    /* S_EZAFE      */ {L"نگه‌داشتن نشانهٔ اضافه (ezafe = 1)", L"Keep the ezafe marker (ezafe = 1)"},
    /* S_PARAMINFO  */ {L"این‌ها همان \u200E--pace / --mode / --seed / --temperature / --retries / "
                         L"--chunk-tokens / --pause-scale\u200E خط فرمان‌اند.",
                        L"These are the CLI's --pace / --mode / --seed / --temperature / --retries / "
                        "--chunk-tokens / --pause-scale flags."},
    /* S_FORCETEXT  */ {L"همیشه G2P کن (مانند \u200E--text-in\u200E) — برای ورودی عددی/نمادی",
                        L"Always run G2P (--text-in) — for numeric/symbol input"},
    /* S_DL_HINT    */ {L"فایل‌های مدل و صداها از پروندهٔ models.json خوانده می‌شوند (کنار همین برنامه). "
                         L"یک ردیف را انتخاب و «دانلود» را بزنید؛ پس از پایان، «بازخوانی مدل» مدل را تازه می‌کند.",
                        L"Model and voice files come from models.json (next to this program). "
                        "Pick a row and press Download; when it finishes, Reload model refreshes the engine."},
    /* S_DL_GET     */ {L"دانلود", L"Download"},
    /* S_DL_GETALL  */ {L"دانلود همهٔ باقی‌مانده‌ها", L"Download all missing"},
    /* S_DL_CANCEL  */ {L"لغو", L"Cancel"},
    /* S_DL_RELOAD  */ {L"بازخوانی مدل", L"Reload model"},
    /* S_DL_JSON    */ {L"فهرست دانلود از:", L"Download list from:"},
    /* S_DEFAULT_TEXT */ {L"سلام، حال شما چطور است؟ این یک آزمایش است.", L"Hello, how are you? This is a test."},
};

static const wchar_t* T(int id) { return g.lang ? STR[id].en : STR[id].fa; }

// printf-style status/message string in the current language.
static std::wstring langfmt(const wchar_t* fa, const wchar_t* en, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, en);
    const wchar_t* f = g.lang ? en : fa;
#if defined(__MINGW32__) || defined(__MINGW64__)
    // mingw's ISO-C99 vswprintf takes the buffer size (same as MSVC here)
    vswprintf(buf, 512, f, ap);
#else
    vswprintf(buf, 512, f, ap);
#endif
    va_end(ap);
    return buf;
}

// --- small helpers ----------------------------------------------------------
static void logf(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        std::wstring p(path);
        size_t pos = p.find_last_of(L'\\');
        if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
        p += L"farsi-tts-gui.log";
        FILE* f = nullptr;
        _wfopen_s(&f, p.c_str(), L"a");
        if (f) { fprintf(f, "%s\n", line); fclose(f); }
    }
}

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string get_text(HWND h) {
    int len = GetWindowTextLengthW(h);
    if (len <= 0) return "";
    std::wstring w(len + 1, L'\0');
    GetWindowTextW(h, &w[0], len + 1);
    w.resize(len);
    return narrow(w);
}

static void set_text(HWND h, const std::string& s) { SetWindowTextW(h, widen(s).c_str()); }

static void set_status(const std::wstring& s) { if (g.status) SetWindowTextW(g.status, s.c_str()); }

static void set_statusf(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 512, fmt, ap);
    va_end(ap);
    set_status(buf);
}

static BOOL CALLBACK set_font_cb(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}
static void apply_font(HWND wnd, HFONT f) { EnumChildWindows(wnd, set_font_cb, (LPARAM)f); }

static int tb_get(HWND h) { return (int)SendMessageW(h, TBM_GETPOS, 0, 0); }

// Width of a wide string in the given font (for buttons that fit their text).
static int text_width(HFONT font, const wchar_t* s) {
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r = {0, 0, 0, 0};
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return r.right - r.left;
}

// "325.5 MB" / "412 kB" — used by the download list.
static std::wstring fmt_size(long long n) {
    wchar_t b[64];
    if (n <= 0) return L"?";
    if (n >= 1048576LL)
        swprintf(b, 64, L"%.1f MB", n / 1048576.0);
    else if (n >= 1024)
        swprintf(b, 64, L"%.0f kB", n / 1024.0);
    else
        swprintf(b, 64, L"%lld B", n);
    return b;
}

static std::vector<char> make_wav(const std::vector<float>& audio, int sr) {
    std::vector<int16_t> pcm(audio.size());
    for (size_t i = 0; i < audio.size(); i++) {
        float v = audio[i];
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        pcm[i] = (int16_t)lround(v * 32767.0f);
    }
    uint32_t dl = (uint32_t)(pcm.size() * 2);
    std::vector<char> w;
    auto put = [&](uint32_t v, int n) { for (int i = 0; i < n; i++) w.push_back((char)((v >> (8 * i)) & 0xFF)); };
    auto put16 = [&](uint16_t v, int n) { for (int i = 0; i < n; i++) w.push_back((char)((v >> (8 * i)) & 0xFF)); };
    w.insert(w.end(), {'R', 'I', 'F', 'F'}); put(36 + dl, 4);
    w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '}); put(16, 4);
    put16(1, 2); put16(1, 2); put((uint32_t)sr, 4); put((uint32_t)sr * 2, 4); put16(2, 2); put16(16, 2);
    w.insert(w.end(), {'d', 'a', 't', 'a'}); put(dl, 4);
    const char* p = reinterpret_cast<const char*>(pcm.data());
    w.insert(w.end(), p, p + dl);
    return w;
}

// --- font (Sahel) -----------------------------------------------------------
// Common controls draw their text with plain GDI and no complex-script shaping,
// so a face without Arabic glyphs paints the Persian labels as tofu boxes.
// Sahel ships with the exe (embedded) and can be overridden by putting a
// sahel.ttf next to it; if neither works we fall back to whatever Arabic
// TrueType face the machine has, then Tahoma. In English mode Segoe UI (present
// on every Windows 7) is preferred for a native look.
struct enum_ctx {
    const wchar_t* want;
    bool found;
    wchar_t fallback[LF_FACESIZE];
};

static int CALLBACK enum_font_cb(const LOGFONTW* lf, const TEXTMETRICW*, DWORD type, LPARAM param) {
    enum_ctx* c = (enum_ctx*)param;
    if (!(type & TRUETYPE_FONTTYPE)) return 1;
    if (c->want && _wcsicmp(lf->lfFaceName, c->want) == 0) { c->found = true; return 0; }
    if (!c->fallback[0]) wcsncpy(c->fallback, lf->lfFaceName, LF_FACESIZE - 1);
    return 1;  // keep looking (first Arabic-capable TrueType face is the fallback)
}

static bool face_available(const wchar_t* name) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    enum_ctx c{name, false, {0}};
    EnumFontFamiliesExW(dc, &lf, enum_font_cb, (LPARAM)&c, 0);
    ReleaseDC(nullptr, dc);
    return c.found;
}

static void init_font() {
    // 1. sahel.ttf next to the exe (lets a user swap the font file)
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path dir = fs::path(exe).parent_path();
    for (fs::path p : {dir / L"sahel.ttf", g.base / L"sahel.ttf"}) {
        if (fs::exists(p)) {
            if (AddFontResourceW(p.c_str())) logf("font: loaded %s", p.string().c_str());
        }
    }
    // 2. the copy embedded in this exe
    DWORD n = 0;
    if (AddFontMemResourceEx((LPVOID)kFontData, (DWORD)kFontDataLen, nullptr, &n))
        logf("font: embedded Sahel registered (%u copies)", (unsigned)n);

    // 3. use it if it really is there
    if (face_available(L"Sahel")) {
        wcsncpy(g_face, L"Sahel", LF_FACESIZE - 1);
        logf("font: Sahel active");
    } else {
        HDC dc = GetDC(nullptr);
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfCharSet = ARABIC_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        enum_ctx c{nullptr, false, {0}};
        EnumFontFamiliesExW(dc, &lf, enum_font_cb, (LPARAM)&c, 0);
        ReleaseDC(nullptr, dc);
        wcsncpy(g_face, c.fallback[0] ? c.fallback : L"Tahoma", LF_FACESIZE - 1);
        logf("font: Sahel unavailable, fallback %ls", g_face);
    }
}

static HFONT make_font(int height) {
    // English UI reads better in Segoe UI; Persian always in Sahel (g_face)
    const wchar_t* face = (g.lang && face_available(L"Segoe UI")) ? L"Segoe UI" : g_face;
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void rebuild_fonts() {
    HFONT o1 = g_font, o2 = g_fontSm, o3 = g_fontBig;
    g_font = make_font(-16);
    g_fontSm = make_font(-14);
    g_fontBig = make_font(-19);
    // Apply to OUR window, not GetActiveWindow(): during WM_CREATE (and when
    // the window is in the background) that is NULL, the new fonts would reach
    // nobody and the old handles — deleted below — would dangle under every
    // control (its text then falls back to a face without Arabic glyphs).
    apply_font(g_hwnd ? g_hwnd : GetActiveWindow(), g_font);
    for (HWND h : {g.hints[0], g.hints[1], g.hints[2], g.hints[3], g.hints[4], g.hints[5], g.hints[6]})
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_fontSm, TRUE);
    for (HFONT f : {o1, o2, o3}) if (f) DeleteObject(f);
}

// --- voices -----------------------------------------------------------------
static void scan_voices() {
    g.voiceList.clear();
    SendMessageW(g.voice, CB_RESETCONTENT, 0, 0);
    auto add = [&](const fs::path& dir, const std::string& prefix) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".wav") {  // the only format read_wav() can decode
                std::string rel = prefix + e.path().filename().string();
                g.voiceList.push_back(rel);
                SendMessageW(g.voice, CB_ADDSTRING, 0, (LPARAM)widen(rel).c_str());
            }
        }
    };
    add(g.voices, "voices/");
    add(g.uploads, "uploads/");
    if (!g.voiceList.empty() && SendMessageW(g.voice, CB_GETCURSEL, 0, 0) < 0)
        SendMessageW(g.voice, CB_SETCURSEL, 0, 0);
}

static std::string current_voice() {
    int vi = (int)SendMessageW(g.voice, CB_GETCURSEL, 0, 0);
    return (vi >= 0 && vi < (int)g.voiceList.size()) ? g.voiceList[vi] : "voices/male_hello.wav";
}

// The selected voice is the ONLY one kept encoded: pick another voice and the
// previous one is offloaded (frees its KV prompt), then the new one is encoded
// once and reused for every synthesis until the selection changes again.
static void on_voice_change() {
    std::string sel = (g.base / current_voice()).string();
    int freed = 0;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.eng) return;
        for (auto& v : g.eng->loaded_voices())
            if (v != sel && g.eng->unload_voice(v)) freed++;
    }
    if (freed)
        set_status(langfmt(L"صدای پیشین از حافظه رها شد (%d) — صدای انتخاب‌شده بارگذاری می‌شود",
                           L"Previous voice offloaded (%d) — loading the selected one", freed));
    // encode the new voice once, ready for many generations
    run_preload();
}

// --- options ----------------------------------------------------------------
// Reads every control into a GenOptions (the same struct the CLI and the HTTP
// API use), so the GUI, the CLI and the server cannot drift apart.
static tts::GenOptions read_options(uint64_t* seed_out, bool* use_phonemes) {
    tts::GenOptions o;
    int mi = (int)SendMessageW(g.mode, CB_GETCURSEL, 0, 0);
    o.mode = (mi == 1) ? "pack" : "split";
    o.pace = tb_get(g.pace) / 100.0;
    o.temperature = tb_get(g.temp) / 100.0;
    o.chunk_tokens = tb_get(g.chunktok);
    o.pause_scale = tb_get(g.pause) / 100.0;
    int ri = (int)SendMessageW(g.retries, CB_GETCURSEL, 0, 0);
    o.retries = (ri < 0) ? 3 : ri + 1;
    o.zero_noise = SendMessageW(g.znoise, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.keep_ezafe = SendMessageW(g.ezafe, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::string seed = get_text(g.seed);
    if (!seed.empty()) {
        try {
            o.seed = std::stoull(seed);
            o.seed_set = true;
        } catch (...) { o.seed_set = false; }
    }
    // input mode: «ساخت از روی همین آوانگاشت‌ها» skips the G2P, «همیشه G2P کن»
    // forces it (numeric/symbol-only input). The two are exclusive; the G2P
    // switch wins if somehow both are ticked.
    bool ph = SendMessageW(g.useph, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool ft = g.forcetext && SendMessageW(g.forcetext, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.input_text = ft;
    o.input_phonemes = ph && !ft;
    if (use_phonemes) *use_phonemes = o.input_phonemes;
    if (seed_out) *seed_out = o.seed_set ? o.seed : 0;
    return o;
}

// --- dictionary -------------------------------------------------------------
static void dict_load_edit() {
    if (!g.eng) return;
    std::lock_guard<std::mutex> lk(g.mu);
    std::string s = "# " + narrow(T(S_DICT_HINT)) + "\n";
    for (auto& e : g.eng->dict().entries()) s += e.word + "\t" + e.phonemes + "\n";
    set_text(g.dict, s);
    std::wstring wpath = widen(g.eng->dict_path());
    wchar_t buf[320];
    // %ls (not %s): this build uses mingw's ISO-C99 wide printf, where %s is a
    // narrow string; %ls means wchar_t* in both the ISO and the MSVC worlds.
    swprintf(buf, 320, L"%d %ls — %ls", (int)g.eng->dict().size(),
             g.lang ? L"entries" : L"مداخل", wpath.c_str());
    SetWindowTextW(g.dictinfo, buf);
}

static bool dict_save_edit() {
    if (!g.eng) {
        set_status(langfmt(L"مدل هنوز بارگذاری نشده است", L"The model is not loaded yet"));
        return false;
    }
    std::string body = get_text(g.dict);
    tts::Dictionary d;
    size_t n = d.load_text(body);
    if (n == 0 && !body.empty()) {
        // allow a single "word phonemes" line typed without a tab
        std::string one = body;
        for (auto& c : one) if (c == '\n' || c == '\r') { c = ' '; }
        d.load_text(one);
        n = d.size();
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.eng->set_dict(d);
        ok = g.eng->save_dict();
    }
    dict_load_edit();
    if (!ok) {
        set_status(langfmt(L"ذخیرهٔ واژه‌نامه شکست خورد", L"Saving the dictionary failed"));
        return false;
    }
    set_status(langfmt(L"واژه‌نامه ذخیره شد — %d مدخل", L"Dictionary saved — %d entries", (int)n));
    return true;
}

// --- tasks ------------------------------------------------------------------
static bool start_task(HWND hwnd, Task task, std::function<void()> fn) {
    if (g.loading) {
        set_status(langfmt(L"مدل در حال بارگذاری است…", L"Loading the model…"));
        return false;
    }
    if (g.busy.exchange(true)) {
        set_status(langfmt(L"کاری در حال اجراست…", L"Another job is running…"));
        return false;
    }
    EnableWindow(g.go, FALSE);
    std::thread([hwnd, task, fn]() {
        try {
            fn();
            PostMessageW(hwnd, WM_APP_TASK, task, 0);
        } catch (const std::exception& e) {
            logf("task %d failed: %s", (int)task, e.what());
            PostMessageW(hwnd, WM_APP_TASKFAIL, task, 0);
        } catch (...) {
            logf("task %d failed: unknown exception", (int)task);
            PostMessageW(hwnd, WM_APP_TASKFAIL, task, 0);
        }
    }).detach();
    return true;
}

static void run_synth(HWND hwnd) {
    if (!g.engineReady || !g.eng) {
        set_status(langfmt(L"مدل بارگذاری نشده است", L"The model is not loaded"));
        return;
    }
    std::string text = get_text(g.text);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        set_status(langfmt(L"متنی وارد نشده است", L"No text entered"));
        return;
    }
    bool use_ph = false;
    tts::GenOptions o = read_options(nullptr, &use_ph);
    std::string voice = current_voice();
    if (use_ph) text = get_text(g.ph);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        set_status(langfmt(L"آوانگاشتی وارد نشده است", L"No phonemes entered"));
        return;
    }
    o.input_phonemes = use_ph;

    set_status(langfmt(L"در حال ساخت…", L"Synthesising…"));
    PlaySoundW(nullptr, nullptr, SND_PURGE);  // stop playback before we replace g.wav
    logf("synth: voice=%s mode=%s pace=%.2f temp=%.2f retries=%d chunks=%d pause=%.2f ph=%d chars=%zu",
         voice.c_str(), o.mode.c_str(), o.pace, o.temperature, o.retries, o.chunk_tokens, o.pause_scale,
         (int)use_ph, text.size());
    start_task(hwnd, TASK_SYNTH, [text, voice, o]() {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.eng) return;
        tts::SynthResult r = g.eng->synthesize(text, (g.base / voice).string(), o);
        g.audio = r.audio;
        g.lastPhonemes = r.phonemes;
        g.chunkList = r.chunks;
        g.totalChunks = r.total_chunks;
        g.quietChunks = r.quiet_chunks;
        g.wav = make_wav(r.audio, g.eng->sample_rate());
    });
}

static void run_phonemise(HWND hwnd) {
    std::string text = get_text(g.text);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        set_status(langfmt(L"متنی وارد نشده است", L"No text entered"));
        return;
    }
    set_status(langfmt(L"در حال استخراج آوانگاشت…", L"Extracting phonemes…"));
    tts::GenOptions o = read_options(nullptr, nullptr);
    start_task(hwnd, TASK_PHONEMES, [text, o]() {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.eng) return;
        std::string ph = g.eng->phonemise_text(text, o);
        std::vector<std::string> ch = g.eng->plan_chunks(text, o);
        g.phText = ph;
        g.chunkList = ch;
    });
}

static void run_preload() {
    std::string voice = current_voice();
    start_task(g_hwnd, TASK_VOICE, [voice]() {
        auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.eng) return;
        g.eng->preload_voice((g.base / voice).string());
        g.voiceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    });
}

// --- actions ----------------------------------------------------------------
static std::wstring open_file_dialog(HWND hwnd) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Reference voice (*.wav)\0*.wav\0All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) return file;
    return L"";
}

static void save_wav(HWND hwnd) {
    if (g.wav.empty()) {
        set_status(langfmt(L"چیزی برای ذخیره نیست", L"Nothing to save"));
        return;
    }
    wchar_t file[MAX_PATH] = L"parsigo.wav";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"WAV\0*.wav\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) {
        std::ofstream f(file, std::ios::binary);
        f.write(g.wav.data(), (std::streamsize)g.wav.size());
        set_status(langfmt(L"ذخیره شد", L"Saved"));
    }
}

// --- model download (tab 4) --------------------------------------------------
// models.json: a flat list of {"name","url","dir","kind","size","desc"}
// objects. A missing file is created from the built-in list so the user can
// edit it (add voices, change mirrors, ...).
static const char* DEFAULT_MODELS_JSON =
    "{\n"
    "  \"comment\": \"farsi-tts download list — edit freely; one object per file. "
    "dir is relative to the program folder, kind is model|voice|dict.\",\n"
    "  \"items\": [\n"
    "    {\"name\": \"manifest.json\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 3126,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/manifest.json\",\n"
    "     \"desc\": \"راهنمای نسخهٔ مدل\"},\n"
    "    {\"name\": \"flow_lm_step.onnx\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 850237,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/flow_lm_step.onnx\",\n"
    "     \"desc\": \"مدل زبان جریانی (گراف ONNX)\"},\n"
    "    {\"name\": \"flow_lm_step.onnx.data\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 341311488,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/flow_lm_step.onnx.data\",\n"
    "     \"desc\": \"وزن‌های مدل زبان جریانی (بزرگ‌ترین پرونده)\"},\n"
    "    {\"name\": \"g2p_encoder.onnx\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 13429334,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/g2p_encoder.onnx\",\n"
    "     \"desc\": \"رمزگذار دوزبانه G2P\"},\n"
    "    {\"name\": \"g2p_decoder.onnx\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 436473,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/g2p_decoder.onnx\",\n"
    "     \"desc\": \"رمزگشای G2P (گراف ONNX)\"},\n"
    "    {\"name\": \"g2p_decoder.onnx.data\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 20462336,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/g2p_decoder.onnx.data\",\n"
    "     \"desc\": \"وزن‌های رمزگشای G2P\"},\n"
    "    {\"name\": \"mimi_encoder.onnx\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 401224,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/mimi_encoder.onnx\",\n"
    "     \"desc\": \"رمزگذار صوتی Mimi (گراف ONNX)\"},\n"
    "    {\"name\": \"mimi_encoder.onnx.data\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 39059456,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/mimi_encoder.onnx.data\",\n"
    "     \"desc\": \"وزن‌های رمزگذار Mimi\"},\n"
    "    {\"name\": \"mimi_decoder_step_kv.onnx\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 637846,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/mimi_decoder_step_kv.onnx\",\n"
    "     \"desc\": \"رمزگشای Mimi با KV-cache (گراف ONNX)\"},\n"
    "    {\"name\": \"mimi_decoder_step_kv.onnx.data\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 41222144,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/mimi_decoder_step_kv.onnx.data\",\n"
    "     \"desc\": \"وزن‌های رمزگشای Mimi\"},\n"
    "    {\"name\": \"weights.npz\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 16524808,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/weights.npz\",\n"
    "     \"desc\": \"وزن‌های جانبی (LUT، پروجکشن بلندگو)\"},\n"
    "    {\"name\": \"decode_state_init.npz\", \"dir\": \"model/onnx\", \"kind\": \"model\", \"size\": 16843958,\n"
    "     \"url\": \"https://huggingface.co/Nimaone/pocket-tts-farsi-v2-onnx/resolve/main/decode_state_init.npz\",\n"
    "     \"desc\": \"حالت اولیهٔ رمزگشای\"},\n"
    "    {\"name\": \"tokenizer_ph.model\", \"dir\": \"model/v2\", \"kind\": \"model\", \"size\": 301825,\n"
    "     \"url\": \"https://huggingface.co/mehdi-hf/pocket-tts-farsi-v2/resolve/main/tokenizer_ph.model\",\n"
    "     \"desc\": \"توکن‌ساز آوانگاشت (SentencePiece)\"},\n"
    "    {\"name\": \"normalize_fa.py\", \"dir\": \"model/v2\", \"kind\": \"model\", \"size\": 14437,\n"
    "     \"url\": \"https://huggingface.co/mehdi-hf/pocket-tts-farsi-v2/resolve/main/normalize_fa.py\",\n"
    "     \"desc\": \"نرمال‌ساز متن فارسی\"},\n"
    "    {\"name\": \"prompt_hello.wav\", \"dir\": \"voices\", \"kind\": \"voice\", \"size\": 206286,\n"
    "     \"url\": \"https://huggingface.co/mehdi-hf/pocket-tts-farsi-v2/resolve/main/samples/prompt_hello.wav\",\n"
    "     \"desc\": \"صدای مرجع نمونه (سلام)\"},\n"
    "    {\"name\": \"prompt_short_sentence.wav\", \"dir\": \"voices\", \"kind\": \"voice\", \"size\": 237390,\n"
    "     \"url\": \"https://huggingface.co/mehdi-hf/pocket-tts-farsi-v2/resolve/main/samples/prompt_short_sentence.wav\",\n"
    "     \"desc\": \"صدای مرجع نمونه (جملهٔ کوتاه)\"},\n"
    "    {\"name\": \"prompt_news_paragraph.wav\", \"dir\": \"voices\", \"kind\": \"voice\", \"size\": 246606,\n"
    "     \"url\": \"https://huggingface.co/mehdi-hf/pocket-tts-farsi-v2/resolve/main/samples/prompt_news_paragraph.wav\",\n"
    "     \"desc\": \"صدای مرجع نمونه (خبر)\"}\n"
    "  ]\n"
    "}\n";

// Tolerant reader for the flat model objects (no nested braces in this file).
static bool jstr(const std::string& s, const char* key, std::string& out) {
    size_t p = s.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    if (p >= s.size() || s[p] != '"') return false;
    p++;
    std::string v;
    for (; p < s.size(); p++) {
        char c = s[p];
        if (c == '\\' && p + 1 < s.size()) {
            char n2 = s[p + 1];
            if (n2 == 'n') v += '\n';
            else if (n2 == 't') v += '\t';
            else v += n2;
            p++;
        } else if (c == '"') {
            out = v;
            return true;
        } else v += c;
    }
    return false;
}
static long long jnum(const std::string& s, const char* key, long long def = 0) {
    size_t p = s.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return def;
    p = s.find(':', p);
    if (p == std::string::npos) return def;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    size_t e = p;
    while (e < s.size() && (std::isdigit((unsigned char)s[e]) || s[e] == '-')) e++;
    if (e == p) return def;
    try { return std::stoll(s.substr(p, e - p)); } catch (...) { return def; }
}

static bool safe_rel(const std::string& rel) {
    return !rel.empty() && rel.find("..") == std::string::npos && rel.find(':') == std::string::npos;
}

static void parse_models(const std::string& text, std::vector<ModelItem>& out) {
    size_t p = 0;
    while ((p = text.find('{', p)) != std::string::npos) {
        size_t e = text.find('}', p);
        if (e == std::string::npos) break;
        std::string obj = text.substr(p, e - p + 1);
        ModelItem it;
        jstr(obj, "name", it.name);
        jstr(obj, "url", it.url);
        jstr(obj, "dir", it.dir);
        jstr(obj, "kind", it.kind);
        jstr(obj, "desc", it.desc);
        it.size = jnum(obj, "size", 0);
        if (!it.name.empty() && !it.url.empty() && safe_rel(it.dir)) {
            if (it.kind.empty()) it.kind = "model";
            std::error_code ec;
            it.have = fs::exists(g.base / it.dir / it.name, ec);
            out.push_back(it);
        }
        p = e + 1;
    }
}

static void load_model_list() {
    g.models.clear();
    g.modelsJson = g.base / "models.json";
    std::string text;
    std::ifstream f(g.modelsJson, std::ios::binary);
    if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    if (text.empty()) {
        text = DEFAULT_MODELS_JSON;
        std::ofstream o(g.modelsJson, std::ios::binary);
        if (o) o << text;  // write the built-in list so it can be edited
    }
    parse_models(text, g.models);
    if (g.dlJson) {
        std::wstring p = T(S_DL_JSON);
        p += L" ";
        p += g.modelsJson.wstring();
        SetWindowTextW(g.dlJson, p.c_str());
    }
}

static void fill_model_list() {
    SendMessageW(g.dlList, LB_RESETCONTENT, 0, 0);
    for (auto& it : g.models) {
        // ASCII markers: Sahel has no U+2713/U+00B7, they would paint as tofu
        std::wstring row = (it.have ? L"[x] " : L"[ ] ") + widen(it.name) + L"  —  " + fmt_size(it.size);
        if (!it.desc.empty()) row += L"  — " + widen(it.desc);
        SendMessageW(g.dlList, LB_ADDSTRING, 0, (LPARAM)row.c_str());
    }
}

// One file: WinInet pull with byte-level progress. Runs on a worker thread.
static void download_one(HWND hwnd, size_t idx) {
    ModelItem it = g.models[idx];
    g.dlIndex = (int)idx;
    g.dlAbort = false;
    std::wstring dest = (g.base / it.dir / it.name).wstring();
    std::wstring tmp = dest + L".part";
    std::error_code ec;
    fs::create_directories(g.base / it.dir, ec);

    HINTERNET hInet = InternetOpenW(L"farsi-tts-gui/1.1", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        PostMessageW(hwnd, WM_APP_DLDONE, 1, 0);
        return;
    }
    HINTERNET hUrl = InternetOpenUrlW(hInet, widen(it.url).c_str(), nullptr, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                          INTERNET_FLAG_KEEP_CONNECTION,
                                      0);
    if (!hUrl) {
        InternetCloseHandle(hInet);
        PostMessageW(hwnd, WM_APP_DLDONE, 1, 0);
        return;
    }
    // Content-Length when the server says it, else the size from models.json
    DWORD total = 0, sz = sizeof(total);
    if (!HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &sz, nullptr) ||
        total == 0)
        total = (it.size > 0) ? (DWORD)it.size : 0;

    FILE* out = nullptr;
    _wfopen_s(&out, tmp.c_str(), L"wb");
    if (!out) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);
        PostMessageW(hwnd, WM_APP_DLDONE, 1, 0);
        return;
    }
    char buf[64 * 1024];
    ULONGLONG done = 0;
    bool ok = true, cancelled = false;
    for (;;) {
        if (g.dlAbort) { cancelled = true; break; }
        DWORD rd = 0;
        if (!InternetReadFile(hUrl, buf, sizeof(buf), &rd)) { ok = false; break; }
        if (rd == 0) break;
        if (fwrite(buf, 1, rd, out) != rd) { ok = false; break; }
        done += rd;
        PostMessageW(hwnd, WM_APP_DLPROG, (WPARAM)done, (LPARAM)(ULONGLONG)total);
    }
    fclose(out);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);

    if (ok && !cancelled) {
        fs::remove(dest, ec);
        fs::rename(tmp, dest, ec);
        if (ec) {  // rename can fail across volumes — copy instead
            fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
            fs::remove(tmp, ec);
            if (ec) ok = false;
        }
    } else {
        fs::remove(tmp, ec);
    }
    PostMessageW(hwnd, WM_APP_DLDONE, cancelled ? 2 : (ok ? 0 : 1), 0);
}

static void start_download_selection(HWND hwnd) {
    int sel = (int)SendMessageW(g.dlList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g.models.size()) {
        set_status(langfmt(L"یک ردیف را از فهرست انتخاب کنید", L"Select a row in the list first"));
        return;
    }
    if (g.models[sel].have) {
        set_status(langfmt(L"این پرونده از قبل روی دیسک هست — «بازخوانی مدل» را بزنید",
                           L"That file is already on disk — press Reload model"));
        return;
    }
    if (!g.dlActive.exchange(true)) {
        EnableWindow(g.dlGet, FALSE);
        EnableWindow(g.dlGetAll, FALSE);
        EnableWindow(g.dlCancel, TRUE);
        set_status(langfmt(L"در حال دانلود…", L"Downloading…"));
        std::thread([hwnd, sel]() { download_one(hwnd, (size_t)sel); }).detach();
    }
}

static void start_download_all(HWND hwnd) {
    if (g.dlActive) return;
    std::vector<size_t> pending;
    for (size_t i = 0; i < g.models.size(); i++)
        if (!g.models[i].have) pending.push_back(i);
    if (pending.empty()) {
        set_status(langfmt(L"همهٔ پرونده‌ها روی دیسک هستند", L"All files are already on disk"));
        return;
    }
    if (!g.dlActive.exchange(true)) {
        g.dlBatch = true;
        g.dlAbort = false;
        EnableWindow(g.dlGet, FALSE);
        EnableWindow(g.dlGetAll, FALSE);
        EnableWindow(g.dlCancel, TRUE);
        std::thread([hwnd, pending]() {
            for (size_t idx : pending) {
                if (g.dlAbort) break;
                download_one(hwnd, idx);          // posts WM_APP_DLDONE for this file
                // wait until the UI thread has processed it (it clears dlActive)
                while (g.dlActive) Sleep(20);
                if (g.dlAbort) break;
                g.dlActive = true;                // claim the next file
            }
            g.dlActive = false;
            g.dlBatch = false;
            PostMessageW(hwnd, WM_APP_DLDONE, 2, 0);  // batch finished (or cancelled)
        }).detach();
    }
}

// --- layout -----------------------------------------------------------------
static void show_page(int idx) {
    g.tabSel = idx;
    for (int i = 0; i < 5; i++) {
        if (!g.pages[i]) continue;
        ShowWindow(g.pages[i], i == idx ? SW_SHOW : SW_HIDE);
        if (i == idx) SetWindowPos(g.pages[i], HWND_TOP, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (g.tab) SendMessageW(g.tab, TCM_SETCURSEL, idx, 0);
}

// A tidy grid: consistent margins/gaps, captions above their controls, buttons
// on one aligned row sized to their text, sliders with their value at the end.
static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    if (W < 200 || H < 200) return;
    if (g.tab) MoveWindow(g.tab, 10, 8, W - 20, H - 66, TRUE);
    // status + language switch live on one bottom row
    if (g.langsel) MoveWindow(g.langsel, W - 130, H - 46, 118, 220, TRUE);
    if (g.status) MoveWindow(g.status, 12, H - 44, W - 150, 22, TRUE);

    RECT tr = {0, 0, W - 20, H - 66};
    POINT pts[2] = {{tr.left, tr.top}, {tr.right, tr.bottom}};
    if (g.tab) {
        SendMessageW(g.tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
        pts[0].x = tr.left; pts[0].y = tr.top;
        pts[1].x = tr.right; pts[1].y = tr.bottom;
        MapWindowPoints(g.tab, hwnd, pts, 2);
    }
    int px = pts[0].x, py = pts[0].y, pw = pts[1].x - pts[0].x, ph = pts[1].y - pts[0].y;
    if (pw < 100) pw = 100;
    if (ph < 100) ph = 100;
    for (int i = 0; i < 5; i++)
        if (g.pages[i]) MoveWindow(g.pages[i], px, py, pw, ph, TRUE);

    const int m = 14, ggap = 10;
    int cw = (pw - 2 * m - 2 * ggap) / 3;  // three-column width
    int x1 = m, x2 = m + cw + ggap, x3 = m + 2 * (cw + ggap);

    // ---- tab 0: text + voice + transport ----
    int th = ph / 3;
    if (th < 120) th = 120;
    if (th > 240) th = 240;
    if (g.text) MoveWindow(g.text, m, m, pw - 2 * m, th, TRUE);
    int y1 = m + th + ggap;
    // captions above their controls (ids 1051..1053, see WM_CREATE)
    HWND c;
    c = GetDlgItem(g.pages[0], 1051); if (c) MoveWindow(c, x1, y1, cw, 18, TRUE);
    c = GetDlgItem(g.pages[0], 1052); if (c) MoveWindow(c, x2, y1, cw, 18, TRUE);
    c = GetDlgItem(g.pages[0], 1053); if (c) MoveWindow(c, x3, y1, cw, 18, TRUE);
    if (g.voice) MoveWindow(g.voice, x1, y1 + 20, cw, 240, TRUE);
    if (g.mode) MoveWindow(g.mode, x2, y1 + 20, cw, 240, TRUE);
    if (g.pace) MoveWindow(g.pace, x3, y1 + 20, cw - 70, 28, TRUE);
    if (g.paceval) MoveWindow(g.paceval, x3 + cw - 66, y1 + 22, 66, 20, TRUE);
    int y2 = y1 + 20 + 34 + ggap;
    // action row: weights keep the buttons aligned and evenly filled
    {
        struct Btn { HWND h; double w; } btns[] = {
            {g.go, 1.35}, {g.play, 1.0}, {g.save, 1.1}, {g.addbtn, 1.15}, {g.preload, 1.4}};
        double sum = 0;
        for (auto& b : btns) sum += b.w;
        int avail = pw - 2 * m - 4 * ggap;
        int x = m;
        for (auto& b : btns) {
            int w = (int)(avail * b.w / sum);
            if (b.h) MoveWindow(b.h, x, y2, w, 38, TRUE);
            x += w + ggap;
        }
    }
    int y3 = y2 + 38 + ggap;
    if (g.hints[0]) MoveWindow(g.hints[0], m, y3, pw - 2 * m, ph - y3 - m, TRUE);

    // ---- tab 1: phonemes ----
    if (g.hints[1]) MoveWindow(g.hints[1], m, m, pw - 2 * m, 22, TRUE);
    if (g.ph) MoveWindow(g.ph, m, m + 26, pw - 2 * m, 110, TRUE);
    int p1y = m + 26 + 110 + ggap;
    if (g.extract) MoveWindow(g.extract, m, p1y, text_width(g_font, T(S_EXTRACT)) + 40, 34, TRUE);
    if (g.useph) MoveWindow(g.useph, m + 220, p1y + 5, 300, 24, TRUE);
    if (g.hints[2]) MoveWindow(g.hints[2], m + 530, p1y + 5, pw - 2 * m - 530, 24, TRUE);
    int p1y2 = p1y + 34 + ggap;
    if (g.hints[3]) MoveWindow(g.hints[3], m, p1y2, pw - 2 * m, 20, TRUE);
    if (g.chunks) MoveWindow(g.chunks, m, p1y2 + 22, pw - 2 * m, ph - (p1y2 + 22) - m, TRUE);

    // ---- tab 2: dictionary ----
    if (g.hints[4]) MoveWindow(g.hints[4], m, m, pw - 2 * m, 40, TRUE);
    int dH = ph - m - 40 - ggap - 40 - m;
    if (dH < 80) dH = 80;
    if (g.dict) MoveWindow(g.dict, m, m + 40 + ggap, pw - 2 * m, dH, TRUE);
    int dY = m + 40 + ggap + dH + ggap;
    if (g.dictsave) MoveWindow(g.dictsave, m, dY, text_width(g_font, T(S_DICT_SAVE)) + 40, 34, TRUE);
    if (g.dictload) MoveWindow(g.dictload, m + 210, dY, text_width(g_font, T(S_DICTLOAD)) + 40, 34, TRUE);
    if (g.dictinfo) MoveWindow(g.dictinfo, m + 380, dY + 6, pw - 2 * m - 380, 28, TRUE);

    // ---- tab 3: parameters (label column + control column) ----
    {
        const int lw = 300, cx = m + lw + 16;
        int y = m;
        auto row = [&](int* yy) { int t = *yy; *yy += 42; return t; };
        int r;
        r = row(&y);
        if (g.seed) MoveWindow(g.seed, cx, r - 4, 200, 26, TRUE);
        if (g.seedrand) MoveWindow(g.seedrand, cx + 210, r - 4, 110, 26, TRUE);
        r = row(&y);
        if (g.temp) MoveWindow(g.temp, cx, r - 6, 240, 30, TRUE);
        if (g.tempval) MoveWindow(g.tempval, cx + 250, r, 80, 20, TRUE);
        r = row(&y);
        if (g.retries) MoveWindow(g.retries, cx, r - 4, 90, 240, TRUE);
        r = row(&y);
        if (g.chunktok) MoveWindow(g.chunktok, cx, r - 6, 240, 30, TRUE);
        if (g.chunkval) MoveWindow(g.chunkval, cx + 250, r, 80, 20, TRUE);
        r = row(&y);
        if (g.pause) MoveWindow(g.pause, cx, r - 6, 240, 30, TRUE);
        if (g.pauseval) MoveWindow(g.pauseval, cx + 250, r, 80, 20, TRUE);
        r = row(&y);
        if (g.threads) MoveWindow(g.threads, cx, r - 4, 120, 240, TRUE);
        y += 6;
        if (g.znoise) MoveWindow(g.znoise, cx, y, pw - cx - m, 24, TRUE);
        y += 30;
        if (g.ezafe) MoveWindow(g.ezafe, cx, y, pw - cx - m, 24, TRUE);
        y += 30;
        if (g.forcetext) MoveWindow(g.forcetext, cx, y, pw - cx - m, 24, TRUE);
        y += 34;
        if (g.paraminfo) MoveWindow(g.paraminfo, m, y, pw - 2 * m, 30, TRUE);
        // parameter captions sit in the label column, right-aligned
        struct Cap { int id; int y; } caps[] = {
            {ID_SEED, m}, {ID_TEMP, m + 42}, {ID_RETRIES, m + 84}, {ID_CHUNKTOK, m + 126},
            {ID_PAUSE, m + 168}, {ID_THREADS, m + 210}};
        for (auto& c : caps) {
            HWND h = GetDlgItem(g.pages[3], c.id + 500);  // caption ids = control id + 500
            if (h) MoveWindow(h, m, c.y - 2, lw, 22, TRUE);
        }
    }

    // ---- tab 4: model download ----
    if (g.hints[5]) MoveWindow(g.hints[5], m, m, pw - 2 * m, 40, TRUE);
    int lH = ph - m - 40 - ggap - 70 - ggap - 24 - m;
    if (lH < 80) lH = 80;
    if (g.dlList) MoveWindow(g.dlList, m, m + 40 + ggap, pw - 2 * m, lH, TRUE);
    int bY = m + 40 + ggap + lH + ggap;
    if (g.dlBar) MoveWindow(g.dlBar, m, bY, pw - 2 * m, 16, TRUE);
    int aY = bY + 16 + ggap;
    if (g.dlGet) MoveWindow(g.dlGet, m, aY, text_width(g_font, T(S_DL_GET)) + 44, 34, TRUE);
    if (g.dlGetAll) MoveWindow(g.dlGetAll, m + 130, aY, text_width(g_font, T(S_DL_GETALL)) + 44, 34, TRUE);
    if (g.dlCancel) MoveWindow(g.dlCancel, m + 380, aY, text_width(g_font, T(S_DL_CANCEL)) + 44, 34, TRUE);
    if (g.dlReload) MoveWindow(g.dlReload, m + 500, aY, text_width(g_font, T(S_DL_RELOAD)) + 44, 34, TRUE);
    if (g.dlStatus) MoveWindow(g.dlStatus, m + 660, aY + 6, pw - 2 * m - 660, 24, TRUE);
    if (g.dlJson) MoveWindow(g.dlJson, m, aY + 40, pw - 2 * m, 20, TRUE);
}

// Pages are plain child windows sitting on the tab's display area (a Win32
// tab control has no real page object). They forward the notifications their
// children generate (WM_COMMAND, WM_HSCROLL, painting) to the main window, so
// the handlers live in one place.
static LRESULT CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_COMMAND:
        case WM_NOTIFY:
        case WM_HSCROLL:
        case WM_VSCROLL:
        case WM_CTLCOLORSTATIC:
            return SendMessageW(GetParent(h), m, w, l);
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

static HWND mk(HWND parent, const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y,
               int w, int h, int id) {
    return CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, nullptr, nullptr);
}
// same, with the Win32 "client edge" border (the modern sunken look)
static HWND mke(HWND parent, const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y,
                int w, int h, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                           parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}

static void retranslate(HWND hwnd);

// --- window -----------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            g_brush = (HBRUSH)(COLOR_BTNFACE + 1);
            init_font();
            g_font = make_font(-16);
            g_fontSm = make_font(-14);
            g_fontBig = make_font(-19);

            g.tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                    12, 10, 860, 520, hwnd, (HMENU)ID_TAB, nullptr, nullptr);

            // Pages are plain child windows that sit on the tab's display area;
            // the tab has no real "page" control, so we show/hide them ourselves.
            WNDCLASSEXW pc{};
            pc.cbSize = sizeof(pc);
            pc.lpfnWndProc = PageProc;
            pc.hInstance = GetModuleHandleW(nullptr);
            pc.hbrBackground = g_brush;
            pc.lpszClassName = L"FarsiTtsPanel";
            if (!GetClassInfoExW(pc.hInstance, pc.lpszClassName, &pc)) RegisterClassExW(&pc);
            for (int i = 0; i < 5; i++)
                g.pages[i] = CreateWindowExW(0, L"FarsiTtsPanel", L"", WS_CHILD | WS_CLIPSIBLINGS,
                                             10, 40, 840, 460, hwnd, nullptr, nullptr, nullptr);

            HWND p0 = g.pages[0], p1 = g.pages[1], p2 = g.pages[2], p3 = g.pages[3], p4 = g.pages[4];

            // ---- tab 0: text + voice + transport ----
            g.text = mke(p0, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                         WS_VSCROLL | ES_NOHIDESEL,
                         10, 10, 820, 190, ID_TEXT);
            SendMessageW(g.text, EM_SETLANGOPTIONS, 0, IMF_AUTORTL);
            // True RTL input box: with the plain Win32 EDIT the paragraph
            // direction is fixed LTR, WS_EX_RTLREADING is the standard way to
            // make Persian text start at the right (same as Explorer rename).
            SetWindowLongPtrW(g.text, GWL_EXSTYLE,
                              GetWindowLongPtrW(g.text, GWL_EXSTYLE) | WS_EX_RTLREADING);
            g.voice = mk(p0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 10, 230, 220, 300, ID_VOICE);
            g.addbtn = mk(p0, L"BUTTON", L"", 0, 240, 229, 110, 28, ID_ADDBTN);
            g.preload = mk(p0, L"BUTTON", L"", 0, 358, 229, 150, 28, ID_PRELOAD);
            g.mode = mk(p0, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 516, 229, 180, 300, ID_MODE);
            g.pace = mk(p0, TRACKBAR_CLASSW, L"", 0, 660, 268, 190, 30, ID_PACE);
            SendMessageW(g.pace, TBM_SETRANGE, TRUE, MAKELPARAM(60, 150));
            SendMessageW(g.pace, TBM_SETPOS, TRUE, 100);
            g.paceval = mk(p0, L"STATIC", L"1.00x", SS_RIGHT, 704, 244, 150, 18, ID_PACEVAL);
            g.go = mk(p0, L"BUTTON", L"", BS_DEFPUSHBUTTON, 10, 312, 160, 40, ID_GO);
            EnableWindow(g.go, FALSE);  // until the model finishes loading
            g.play = mk(p0, L"BUTTON", L"", 0, 180, 312, 100, 40, ID_PLAY);
            g.save = mk(p0, L"BUTTON", L"", 0, 290, 312, 130, 40, ID_SAVE);
            // caption statics for tab 0 (ids = 105x so layout/retranslate can find them)
            mk(p0, L"STATIC", L"", 0, 10, 210, 200, 18, 1051);
            mk(p0, L"STATIC", L"", 0, 516, 210, 180, 18, 1052);
            mk(p0, L"STATIC", L"", SS_RIGHT, 660, 210, 190, 18, 1053);
            g.hints[0] = mk(p0, L"STATIC", L"", 0, 10, 366, 700, 40, 0);

            // ---- tab 1: phonemes ----
            g.hints[1] = mk(p1, L"STATIC", L"", 0, 10, 8, 700, 20, 0);
            g.ph = mke(p1, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                      WS_VSCROLL | ES_NOHIDESEL,
                       10, 34, 820, 120, ID_PH);
            g.extract = mk(p1, L"BUTTON", L"", 0, 10, 164, 170, 34, ID_EXTRACT);
            g.useph = mk(p1, L"BUTTON", L"", BS_AUTOCHECKBOX, 196, 170, 260, 24, ID_USEPH);
            g.hints[2] = mk(p1, L"STATIC", L"", 0, 470, 170, 360, 24, 0);
            g.hints[3] = mk(p1, L"STATIC", L"", 0, 10, 212, 400, 20, 0);
            g.chunks = mke(p1, L"EDIT", L"", ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                           10, 236, 820, 200, ID_CHUNKS);
            SendMessageW(g.chunks, EM_SETREADONLY, TRUE, 0);

            // ---- tab 2: dictionary ----
            g.hints[4] = mk(p2, L"STATIC", L"", 0, 10, 8, 780, 40, 0);
            g.dict = mke(p2, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                         WS_VSCROLL | ES_NOHIDESEL,
                         10, 52, 820, 300, ID_DICT);
            // dictionary is Persian-first: mirror it like the main input
            SetWindowLongPtrW(g.dict, GWL_EXSTYLE,
                              GetWindowLongPtrW(g.dict, GWL_EXSTYLE) | WS_EX_RTLREADING);
            g.dictsave = mk(p2, L"BUTTON", L"", 0, 10, 364, 160, 36, ID_DICTSAVE);
            g.dictload = mk(p2, L"BUTTON", L"", 0, 180, 364, 120, 36, ID_DICTLOAD);
            g.dictinfo = mk(p2, L"STATIC", L"", 0, 314, 370, 500, 30, ID_DICTINFO);

            // ---- tab 3: parameters ----
            // captions carry control id + 500 so layout() can place them
            auto param_cap = [&](int id, const wchar_t* txt) {
                return mk(p3, L"STATIC", txt, SS_RIGHT, 14, 12, 250, 20, id + 500);
            };
            param_cap(ID_SEED, L"");
            g.seed = mk(p3, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 280, 10, 200, 26, ID_SEED);
            g.seedrand = mk(p3, L"BUTTON", L"", 0, 490, 10, 100, 26, ID_SEEDRAND);
            param_cap(ID_TEMP, L"");
            g.temp = mk(p3, TRACKBAR_CLASSW, L"", 0, 280, 50, 240, 30, ID_TEMP);
            SendMessageW(g.temp, TBM_SETRANGE, TRUE, MAKELPARAM(5, 200));
            SendMessageW(g.temp, TBM_SETPOS, TRUE, 30);
            g.tempval = mk(p3, L"STATIC", L"0.30", 0, 530, 56, 70, 20, ID_TEMPVAL);
            param_cap(ID_RETRIES, L"");
            g.retries = mk(p3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 94, 90, 300, ID_RETRIES);
            for (int i = 1; i <= 4; i++) {
                wchar_t b[8];
                swprintf(b, 8, L"%d", i);
                SendMessageW(g.retries, CB_ADDSTRING, 0, (LPARAM)b);
            }
            SendMessageW(g.retries, CB_SETCURSEL, 2, 0);
            param_cap(ID_CHUNKTOK, L"");
            g.chunktok = mk(p3, TRACKBAR_CLASSW, L"", 0, 280, 136, 240, 30, ID_CHUNKTOK);
            SendMessageW(g.chunktok, TBM_SETRANGE, TRUE, MAKELPARAM(8, 32));
            SendMessageW(g.chunktok, TBM_SETPOS, TRUE, 18);
            g.chunkval = mk(p3, L"STATIC", L"18", 0, 530, 142, 70, 20, ID_CHUNKVAL);
            param_cap(ID_PAUSE, L"");
            g.pause = mk(p3, TRACKBAR_CLASSW, L"", 0, 280, 178, 240, 30, ID_PAUSE);
            SendMessageW(g.pause, TBM_SETRANGE, TRUE, MAKELPARAM(0, 300));
            SendMessageW(g.pause, TBM_SETPOS, TRUE, 100);
            g.pauseval = mk(p3, L"STATIC", L"1.00", 0, 530, 184, 70, 20, ID_PAUSEVAL);
            param_cap(ID_THREADS, L"");
            g.threads = mk(p3, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 220, 120, 300, ID_THREADS);
            SendMessageW(g.threads, CB_ADDSTRING, 0, (LPARAM)L"");
            for (int i = 1; i <= 8; i++) {
                wchar_t b[8];
                swprintf(b, 8, L"%d", i);
                SendMessageW(g.threads, CB_ADDSTRING, 0, (LPARAM)b);
            }
            SendMessageW(g.threads, CB_SETCURSEL, 0, 0);
            g.znoise = mk(p3, L"BUTTON", L"", BS_AUTOCHECKBOX, 280, 262, 420, 24, ID_ZERONOISE);
            g.ezafe = mk(p3, L"BUTTON", L"", BS_AUTOCHECKBOX, 280, 292, 340, 24, ID_EZAFE);
            g.forcetext = mk(p3, L"BUTTON", L"", BS_AUTOCHECKBOX, 280, 322, 460, 24, ID_FORCETEXT);
            g.paraminfo = mk(p3, L"STATIC", L"", 0, 14, 356, 700, 40, ID_PARAMINFO);

            // ---- tab 4: model download ----
            g.hints[5] = mk(p4, L"STATIC", L"", 0, 10, 8, 780, 40, 0);
            g.dlList = mke(p4, L"LISTBOX", L"", WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                           10, 52, 820, 260, ID_DLLIST);
            g.dlBar = mk(p4, PROGRESS_CLASSW, L"", 0, 10, 322, 820, 16, ID_DLBAR);
            SendMessageW(g.dlBar, PBM_SETRANGE32, 0, 1000);
            g.dlGet = mk(p4, L"BUTTON", L"", 0, 10, 348, 120, 34, ID_DLGET);
            g.dlGetAll = mk(p4, L"BUTTON", L"", 0, 140, 348, 160, 34, ID_DLGETALL);
            g.dlCancel = mk(p4, L"BUTTON", L"", 0, 380, 348, 90, 34, ID_DLCANCEL);
            EnableWindow(g.dlCancel, FALSE);
            g.dlReload = mk(p4, L"BUTTON", L"", 0, 500, 348, 130, 34, ID_DLRELOAD);
            g.dlStatus = mk(p4, L"STATIC", L"", 0, 660, 354, 160, 24, ID_DLSTATUS);
            g.dlJson = mk(p4, L"STATIC", L"", 0, 10, 392, 820, 20, ID_DLJSON);

            // ---- chrome ----
            g.status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 470, 700, 22, hwnd,
                                       (HMENU)ID_STATUS, nullptr, nullptr);
            g.langsel = mk(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 780, 468, 118, 220, ID_LANG);
            SendMessageW(g.langsel, CB_ADDSTRING, 0, (LPARAM)L"فارسی");
            SendMessageW(g.langsel, CB_ADDSTRING, 0, (LPARAM)L"English");
            SendMessageW(g.langsel, CB_SETCURSEL, 0, 0);

            // Ctrl+A in every edit box (works wherever the focus is)
            ACCEL acc[1];
            acc[0].fVirt = FVIRTKEY | FCONTROL;
            acc[0].key = 'A';
            acc[0].cmd = IDM_SELALL;
            g_accel = CreateAcceleratorTableW(acc, 1);

            apply_font(hwnd, g_font);
            for (int i = 0; i < 6; i++)
                if (g.hints[i]) SendMessageW(g.hints[i], WM_SETFONT, (WPARAM)g_fontSm, TRUE);

            retranslate(hwnd);
            show_page(0);
            layout(hwnd);
            return 0;
        }

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_NOTIFY: {
            NMHDR* nm = (NMHDR*)lp;
            if (nm && nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
                show_page((int)SendMessageW(g.tab, TCM_GETCURSEL, 0, 0));
                return 0;
            }
            return 0;
        }

        case WM_HSCROLL: {
            // live values for the four sliders
            wchar_t b[32];
            swprintf(b, 32, L"%.2fx", tb_get(g.pace) / 100.0);
            SetWindowTextW(g.paceval, b);
            swprintf(b, 32, L"%.2f", tb_get(g.temp) / 100.0);
            SetWindowTextW(g.tempval, b);
            swprintf(b, 32, L"%d", tb_get(g.chunktok));
            SetWindowTextW(g.chunkval, b);
            swprintf(b, 32, L"%.2f", tb_get(g.pause) / 100.0);
            SetWindowTextW(g.pauseval, b);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)g_brush;
        }

        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case ID_GO: run_synth(hwnd); return 0;
                case ID_PLAY:
                    if (!g.wav.empty()) PlaySoundW((LPCWSTR)g.wav.data(), nullptr, SND_MEMORY | SND_ASYNC);
                    else set_status(langfmt(L"چیزی برای پخش نیست", L"Nothing to play"));
                    return 0;
                case ID_SAVE: save_wav(hwnd); return 0;
                case ID_EXTRACT: run_phonemise(hwnd); return 0;
                case ID_USEPH:  // the two input switches are mutually exclusive
                case ID_FORCETEXT: {
                    bool ft = g.forcetext &&
                              SendMessageW(g.forcetext, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    bool ph = g.useph && SendMessageW(g.useph, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    if (LOWORD(wp) == ID_USEPH && ph)
                        SendMessageW(g.forcetext, BM_SETCHECK, BST_UNCHECKED, 0);
                    else if (LOWORD(wp) == ID_FORCETEXT && ft)
                        SendMessageW(g.useph, BM_SETCHECK, BST_UNCHECKED, 0);
                    return 0;
                }
                case ID_PRELOAD: run_preload(); return 0;
                case ID_DICTSAVE: dict_save_edit(); return 0;
                case ID_DICTLOAD: dict_load_edit(); return 0;
                case ID_SEEDRAND: {
                    std::random_device rd;
                    unsigned long long s = (((unsigned long long)rd() << 32) ^ rd()) % 1000000000ULL;
                    SetWindowTextW(g.seed, std::to_wstring(s).c_str());
                    return 0;
                }
                case ID_ADDBTN: {
                    std::wstring src = open_file_dialog(hwnd);
                    if (src.empty()) return 0;
                    fs::path p(src);
                    std::string name = p.filename().string();
                    std::error_code ec;
                    fs::create_directories(g.uploads, ec);
                    fs::copy_file(p, g.uploads / name, fs::copy_options::overwrite_existing, ec);
                    scan_voices();
                    for (size_t i = 0; i < g.voiceList.size(); i++)
                        if (g.voiceList[i] == "uploads/" + name)
                            SendMessageW(g.voice, CB_SETCURSEL, (WPARAM)i, 0);
                    set_status(langfmt(L"صدا اضافه شد: ", L"Voice added: ") + widen(name));
                    return 0;
                }
                case ID_VOICE:
                    if (HIWORD(wp) == CBN_SELCHANGE) on_voice_change();
                    return 0;
                case ID_LANG:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        g.lang = (int)SendMessageW(g.langsel, CB_GETCURSEL, 0, 0);
                        retranslate(hwnd);
                        layout(hwnd);
                    }
                    return 0;
                case ID_DLGET: start_download_selection(hwnd); return 0;
                case ID_DLGETALL: start_download_all(hwnd); return 0;
                case ID_DLCANCEL:
                    g.dlAbort = true;
                    return 0;
                case ID_DLRELOAD: {
                    if (g.busy || g.loading) {
                        set_status(langfmt(L"کاری در حال اجراست — صبر کنید", L"Busy — wait for it to finish"));
                        return 0;
                    }
                    // release the weights and load them again (picks up new files)
                    {
                        std::lock_guard<std::mutex> lk(g.mu);
                        delete g.eng;
                        g.eng = nullptr;
                    }
                    g.engineReady = false;
                    EnableWindow(g.go, FALSE);
                    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                    set_status(langfmt(L"در حال بازخوانی مدل…", L"Reloading the model…"));
                    run_engine_load(hwnd);
                    return 0;
                }
                case IDM_SELALL: {
                    // Ctrl+A (from the accelerator table): select everything in
                    // whichever edit box has the focus — text, phonemes,
                    // dictionary, seed — the same as clicking in it and pressing
                    // the OS shortcut. Only real EDIT controls, never a combo.
                    HWND f = GetFocus();
                    if (f) {
                        wchar_t cls[32] = L"";
                        GetClassNameW(f, cls, 32);
                        if (_wcsicmp(cls, L"Edit") == 0) SendMessageW(f, EM_SETSEL, 0, (LPARAM)-1);
                    }
                    return 0;
                }
            }
            return 0;
        }

        case WM_APP_DLPROG: {
            ULONGLONG done = (ULONGLONG)wp, total = (ULONGLONG)lp;
            if (g.dlBar) {
                int pos = (total > 0) ? (int)(done * 1000 / total) : 0;
                SendMessageW(g.dlBar, PBM_SETPOS, pos, 0);
            }
            wchar_t b[96];
            swprintf(b, 96, L"%ls / %ls", fmt_size((long long)done).c_str(),
                     fmt_size((long long)total).c_str());
            SetWindowTextW(g.dlStatus, b);
            return 0;
        }

        case WM_APP_DLDONE: {
            int st = (int)wp;
            g.dlActive = false;
            if (!g.dlBatch) {  // a running "download all" keeps the buttons off
                EnableWindow(g.dlGet, TRUE);
                EnableWindow(g.dlGetAll, TRUE);
                EnableWindow(g.dlCancel, FALSE);
            }
            int idx = g.dlIndex;
            if (st == 0 && idx >= 0 && idx < (int)g.models.size()) {
                g.models[idx].have = true;
                fill_model_list();
                if (g.dlList) SendMessageW(g.dlList, LB_SETCURSEL, idx, 0);
                std::string k = g.models[idx].kind;
                if (k == "voice") scan_voices();
                const wchar_t* note = (k == "voice")
                                          ? (g.lang ? L" — ready in the voice list"
                                                    : L" — در فهرست صداها آماده است")
                                          : (g.lang ? L" — press Reload model"
                                                    : L" — «بازخوانی مدل» را بزنید");
                set_status(langfmt(L"دانلود پایان یافت: ", L"Download finished: ") +
                           widen(g.models[idx].name) + note);
                SendMessageW(g.dlBar, PBM_SETPOS, 1000, 0);
            } else if (st == 1) {
                set_status(langfmt(L"دانلود ناموفق بود — اتصال و آدرس را بررسی کنید (جزئیات در farsi-tts-gui.log)",
                                   L"Download failed — check the connection and the URL (see farsi-tts-gui.log)"));
                SendMessageW(g.dlBar, PBM_SETPOS, 0, 0);
            } else if (idx >= 0 && idx < (int)g.models.size()) {
                set_status(langfmt(L"دانلود لغو شد", L"Download cancelled"));
                SendMessageW(g.dlBar, PBM_SETPOS, 0, 0);
            }
            return 0;
        }

        case WM_APP_TASK: {
            g.busy = false;
            EnableWindow(g.go, g.engineReady);
            if (wp == TASK_SYNTH) {
                double secs = g.audio.empty() || !g.eng ? 0 : g.audio.size() / (double)g.eng->sample_rate();
                if (g.quietChunks > 0)
                    set_status(langfmt(
                        L"آماده — %.2f ثانیه صدا — %d چانک از آستانهٔ کیفیت رد نشد و ممکن است بی‌صدا یا ناقص باشد",
                        L"Ready — %.2f s of audio — %d chunk(s) failed the quality gate and may be silent or short",
                        secs, g.quietChunks));
                else
                    set_status(langfmt(L"آماده — %.2f ثانیه صدا", L"Ready — %.2f s of audio", secs));
                set_text(g.ph, g.lastPhonemes);
                std::string cs;
                for (auto& c : g.chunkList) { cs += c; cs += "\r\n"; }
                set_text(g.chunks, cs);
                if (!g.wav.empty()) PlaySoundW((LPCWSTR)g.wav.data(), nullptr, SND_MEMORY | SND_ASYNC);
            } else if (wp == TASK_PHONEMES) {
                set_text(g.ph, g.phText);
                std::string cs;
                for (auto& c : g.chunkList) { cs += c; cs += "\r\n"; }
                set_text(g.chunks, cs);
                set_status(langfmt(L"آوانگاشت استخراج شد (%d چانک)", L"Phonemes extracted (%d chunks)",
                                   (int)g.chunkList.size()));
                show_page(1);
            } else if (wp == TASK_VOICE) {
                set_status(langfmt(L"صدا پیش‌بارگذاری شد — %d میلی‌ثانیه؛ برای همهٔ ساخت‌های بعدی آماده است",
                                   L"Voice preloaded — %d ms; ready for every later synthesis",
                                   (int)g.voiceMs));
            }
            return 0;
        }
        case WM_APP_TASKFAIL:
            g.busy = false;
            EnableWindow(g.go, g.engineReady);
            set_status(langfmt(L"خطا — جزئیات در farsi-tts-gui.log", L"Error — see farsi-tts-gui.log"));
            return 0;

        case WM_APP_ENGINE: {
            g.loading = false;
            // wParam: 1 = engine ready, 0 = load failed (the worker thread
            // posts it — keep the two sides in sync or a good load reads as
            // "model did not load").
            if (wp != 0) {
                g.engineReady = true;
                scan_voices();
                dict_load_edit();
                fill_model_list();
                wchar_t b[200];
                swprintf(b, 200, L"%ls — %ls %.2f, %ls: %d",
                         g.lang ? L"Ready" : L"آماده",
                         g.lang ? L"default temperature" : L"دمای پیش‌فرض مدل", g.modelTemp,
                         g.lang ? L"threads" : L"رشته‌ها", g.eng ? g.eng->intra_threads() : 0);
                set_status(b);
                swprintf(b, 200, L"%.2f %ls", g.modelTemp, g.lang ? L"(model default)" : L"(پیش‌فرض مدل)");
                SetWindowTextW(g.tempval, b);
                EnableWindow(g.go, TRUE);
            } else {
                logf("engine load failed");
                EnableWindow(g.go, FALSE);
                set_status(langfmt(L"مدل بارگذاری نشد — پوشهٔ model/onnx را بررسی کنید",
                                   L"Model did not load — check the model/onnx folder"));
            }
            return 0;
        }

        case WM_DESTROY:
            // Release the ONNX sessions explicitly on close rather than
            // leaving it to process teardown, then trim the working set so the
            // ~500 MB of weights actually leaves the process footprint. If a
            // synthesis is still running we leave the engine alone (the OS
            // frees it when the process exits) — deleting it under a worker
            // thread would be a use-after-free.
            g.engineReady = false;
            g.dlAbort = true;
            if (g.eng && !g.busy) {
                {
                    std::lock_guard<std::mutex> lk(g.mu);
                    delete g.eng;
                    g.eng = nullptr;
                }
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                logf("model released on close");
            } else if (g.eng) {
                logf("close while busy: engine left to process teardown");
            }
            for (HFONT f : {g_font, g_fontSm, g_fontBig}) if (f) DeleteObject(f);
            if (g_accel) DestroyAcceleratorTable(g_accel);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- engine loading (also used by "Reload model") ---------------------------
static void run_engine_load(HWND hwnd) {
    g.loading = true;
    std::thread([hwnd]() {
        tts::EngineConfig cfg;
        cfg.base_dir = g.base.string();
        cfg.seed_set = false;
        // --threads override from the command line
        int argc2 = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &argc2);
        for (int i = 1; av && i + 1 < argc2; i++)
            if (std::wstring(av[i]) == L"--threads") cfg.intra_threads = _wtoi(av[i + 1]);
        if (av) LocalFree(av);
        try {
            tts::Engine* e = new tts::Engine(cfg);
            {
                std::lock_guard<std::mutex> lk(g.mu);
                g.eng = e;
                g.modelTemp = e->default_temperature();
            }
            logf("engine ready (base=%s, threads=%d)", cfg.base_dir.c_str(), e->intra_threads());
            PostMessageW(hwnd, WM_APP_ENGINE, 1, 0);
        } catch (const std::exception& e) {
            logf("engine load failed: %s (base=%s)", e.what(), cfg.base_dir.c_str());
            PostMessageW(hwnd, WM_APP_ENGINE, 0, 0);
        }
    }).detach();
}

// --- language ---------------------------------------------------------------
// Every visible string is re-applied in the new language; combo item lists
// are rebuilt, the fonts follow (Sahel for Persian, Segoe UI for English) and
// the input box keeps its text (the default sample is re-translated only while
// it is still the sample).
static void retranslate(HWND hwnd) {
    SetWindowTextW(hwnd, T(S_TITLE));
    const wchar_t* tabTxt[5] = {T(S_TAB0), T(S_TAB1), T(S_TAB2), T(S_TAB3), T(S_TAB4)};
    int nTabs = (int)SendMessageW(g.tab, TCM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < 5; i++) {
        TCITEMW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)tabTxt[i];
        // first call (during WM_CREATE) creates the items, later ones relabel
        if (nTabs == 0) SendMessageW(g.tab, TCM_INSERTITEMW, i, (LPARAM)&ti);
        else SendMessageW(g.tab, TCM_SETITEMW, i, (LPARAM)&ti);
    }

    auto setcap = [&](int id, int txt) { HWND h = GetDlgItem(g.pages[3], id + 500); if (h) SetWindowTextW(h, T(txt)); };
    auto btn = [&](HWND h, int txt) { if (h) SetWindowTextW(h, T(txt)); };

    btn(g.go, S_GO);
    btn(g.play, S_PLAY);
    btn(g.save, S_SAVE);
    btn(g.addbtn, S_ADDVOICE);
    btn(g.preload, S_PRELOAD);
    btn(g.extract, S_EXTRACT);
    btn(g.useph, S_USEPH);
    btn(g.dictsave, S_DICT_SAVE);
    btn(g.dictload, S_DICTLOAD);
    btn(g.seedrand, S_SEED_RAND);
    btn(g.znoise, S_ZNOISE);
    btn(g.ezafe, S_EZAFE);
    btn(g.forcetext, S_FORCETEXT);
    btn(g.dlGet, S_DL_GET);
    btn(g.dlGetAll, S_DL_GETALL);
    btn(g.dlCancel, S_DL_CANCEL);
    btn(g.dlReload, S_DL_RELOAD);
    if (g.hints[0]) SetWindowTextW(g.hints[0], T(S_HINT0));
    if (g.hints[1]) SetWindowTextW(g.hints[1], T(S_PH_HINT));
    if (g.hints[2]) SetWindowTextW(g.hints[2], T(S_LASTPH));
    if (g.hints[3]) SetWindowTextW(g.hints[3], T(S_CHUNK_CAP));
    if (g.hints[4]) SetWindowTextW(g.hints[4], T(S_DICT_HINT));
    if (g.hints[5]) SetWindowTextW(g.hints[5], T(S_DL_HINT));
    if (g.paraminfo) SetWindowTextW(g.paraminfo, T(S_PARAMINFO));
    if (g.dlJson) {
        std::wstring p = T(S_DL_JSON);
        p += L" ";
        p += g.modelsJson.wstring();
        SetWindowTextW(g.dlJson, p.c_str());
    }

    // captions of tab 0 (created with ids 1051..1053)
    {
        HWND h;
        h = GetDlgItem(g.pages[0], 1051); if (h) SetWindowTextW(h, T(S_VOICE_CAP));
        h = GetDlgItem(g.pages[0], 1052); if (h) SetWindowTextW(h, T(S_MODE_CAP));
        h = GetDlgItem(g.pages[0], 1053); if (h) SetWindowTextW(h, T(S_PACE_CAP));
    }
    setcap(ID_SEED, S_SEED);
    setcap(ID_TEMP, S_TEMP);
    setcap(ID_RETRIES, S_RETRIES);
    setcap(ID_CHUNKTOK, S_CHUNKTOK);
    setcap(ID_PAUSE, S_PAUSE);
    setcap(ID_THREADS, S_THREADS);

    // mode combo
    int mode = (int)SendMessageW(g.mode, CB_GETCURSEL, 0, 0);
    SendMessageW(g.mode, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.mode, CB_ADDSTRING, 0, (LPARAM)T(S_MODE_SPLIT));
    SendMessageW(g.mode, CB_ADDSTRING, 0, (LPARAM)T(S_MODE_PACK));
    SendMessageW(g.mode, CB_SETCURSEL, mode < 0 ? 0 : mode, 0);
    // threads combo ("auto" + 1..8)
    int thr = (int)SendMessageW(g.threads, CB_GETCURSEL, 0, 0);
    SendMessageW(g.threads, CB_RESETCONTENT, 0, 0);
    SendMessageW(g.threads, CB_ADDSTRING, 0, (LPARAM)T(S_AUTO));
    for (int i = 1; i <= 8; i++) {
        wchar_t b[8];
        swprintf(b, 8, L"%d", i);
        SendMessageW(g.threads, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(g.threads, CB_SETCURSEL, thr < 0 ? 0 : thr, 0);

    // the default sample text follows the language (only while untouched)
    std::string cur = get_text(g.text);
    static std::string shown = "";
    if (cur.empty() || cur == narrow(T(S_DEFAULT_TEXT)) || cur == shown) {
        SetWindowTextW(g.text, T(S_DEFAULT_TEXT));
        shown = get_text(g.text);
    }

    if (g.eng) dict_load_edit();  // its header comment is translated too
    if (!g.models.empty()) fill_model_list();  // list rows carry the description
    rebuild_fonts();
    set_status(langfmt(L"زبان تغییر کرد", L"Language changed"));
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
                                               ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    // base dir = --base override, else the directory of the executable
    // (portable folder layout: the exes sit next to model/ and voices/)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool haveBase = false;
    for (int i = 1; i < argc; i++) {
        if (std::wstring(argv[i]) == L"--base" && i + 1 < argc) {
            g.base = fs::path(argv[++i]);
            haveBase = true;
        } else if (std::wstring(argv[i]) == L"--lang" && i + 1 < argc) {
            std::wstring l(argv[++i]);
            g.lang = (l == L"en" || l == L"english") ? 1 : 0;
        }
    }
    if (argv) LocalFree(argv);
    if (!haveBase) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        g.base = fs::path(exe).parent_path();
    }
    g.voices = g.base / "voices";
    g.uploads = g.base / "uploads";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FarsiTtsWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, T(S_TITLE),
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 690,
                                nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    load_model_list();   // models.json for the download tab
    run_engine_load(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(hwnd, g_accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

#endif  // _WIN32
