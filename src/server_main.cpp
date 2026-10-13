// farsi-tts-server — llama.cpp-server-style local HTTP API + web UI.
//
//   farsi-tts-server --base . --port 8000 --host 127.0.0.1
//
// Endpoints (also printed by --help, and summarised by GET /api/help):
//   GET  /                     web UI (web/index.html)
//   GET  /api/help             this list, as text/plain
//   GET  /api/health           {"ok":true,"model_loaded":…,"port":…}
//   GET  /api/config           defaults + limits + modes (what the UI renders)
//   GET  /api/stats            request counters / timings
//   GET  /api/voices           JSON list of reference voices
//   POST /api/voice?name=x.wav raw WAV body -> uploads/x.wav
//   POST /api/voice/load       encode a reference voice ONCE (?voice=)
//   GET  /api/voice/state      which voices are already encoded
//   POST /api/phonemes         text -> {"phonemes":…,"chunks":[…],"words":[…]}
//   POST /api/chunks           text|phonemes -> {"chunks":[…]}
//   POST /api/synth            text or phonemes -> audio/wav (all parameters)
//   GET  /api/dict             dictionary entries  (?word= for a single lookup)
//   POST /api/dict             add/update an entry (body: word + phonemes)
//   DELETE /api/dict?word=…    remove an entry
//   POST /api/unload           free the weights (next request reloads, ~1 s)
//
// Works on Windows 7 (Winsock) and POSIX. Requests are handled on a thread per
// connection; synthesis is serialized with a mutex because the engine holds
// shared ONNX sessions and a voice cache.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSESOCK closesocket
#define BADSOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
#define CLOSESOCK close
#define BADSOCK (-1)
#include <fcntl.h>
#include <malloc.h>
#endif

#include "audio.h"
#include "dict.h"
#include "tts.h"

namespace fs = std::filesystem;

#define SERVER_VERSION "1.1.0"
#define SERVER_NAME "farsi-tts-server " SERVER_VERSION

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int v = (int)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            out.push_back((char)v);
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else out.push_back((char)c);
        }
    }
    return out;
}

static std::string json_str(const std::string& s) { return "\"" + json_escape(s) + "\""; }

static std::string json_num(double v) {
    char b[40];
    if (v == (long long)v && std::fabs(v) < 1e15)
        snprintf(b, sizeof(b), "%lld", (long long)v);
    else
        snprintf(b, sizeof(b), "%.6g", v);
    return b;
}

static std::string base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out.push_back(T[(v >> 18) & 63]); out.push_back(T[(v >> 12) & 63]);
        out.push_back(T[(v >> 6) & 63]); out.push_back(T[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out.push_back(T[(v >> 18) & 63]); out.push_back(T[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out.push_back(T[(v >> 18) & 63]); out.push_back(T[(v >> 12) & 63]);
        out.push_back(T[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

// --- request ----------------------------------------------------------------
struct Request {
    std::string method, path, body;
    std::vector<std::pair<std::string, std::string>> query, headers;
    // The client is willing to send another request on this connection
    // (HTTP/1.1 without "Connection: close", or HTTP/1.0 with an explicit
    // "Connection: keep-alive"). Computed by read_request().
    bool keepalive = false;
    std::string q(const std::string& k, const std::string& def = "") const {
        for (auto& kv : query) if (kv.first == k) return kv.second;
        return def;
    }
    bool has(const std::string& k) const {
        for (auto& kv : query) if (kv.first == k) return true;
        return false;
    }
};

static std::string qstr(const Request& r, const char* k, const std::string& def) {
    std::string v = r.q(k, "");
    return v.empty() ? def : v;
}
static double qnum(const Request& r, const char* k, double def) {
    std::string v = r.q(k, "");
    if (v.empty()) return def;
    try { return std::stod(v); } catch (...) { return def; }
}
static int qint(const Request& r, const char* k, int def) {
    std::string v = r.q(k, "");
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}
static bool qbool(const Request& r, const char* k, bool def) {
    std::string v = r.q(k, "");
    if (v.empty()) return def;
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

// Minimal JSON readers for the small POST bodies this API accepts
// ({"word":"…","phonemes":"…"} / {"text":"…"}). Not a general parser.
static bool json_get(const std::string& body, const std::string& key, std::string& out) {
    size_t p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = body.find(':', p);
    if (p == std::string::npos) return false;
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size() || body[p] != '"') return false;
    p++;
    std::string v;
    for (; p < body.size(); p++) {
        char c = body[p];
        if (c == '\\' && p + 1 < body.size()) {
            char n = body[p + 1];
            if (n == 'n') v += '\n';
            else if (n == 't') v += '\t';
            else if (n == 'r') v += '\r';
            else v += n;
            p++;
        } else if (c == '"') {
            out = v;
            return true;
        } else v += c;
    }
    return false;
}
static bool json_get_num(const std::string& body, const std::string& key, double& out) {
    size_t p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = body.find(':', p);
    if (p == std::string::npos) return false;
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    size_t e = p;
    while (e < body.size() && (std::isdigit((unsigned char)body[e]) || body[e] == '-' || body[e] == '+' ||
                               body[e] == '.' || body[e] == 'e' || body[e] == 'E'))
        e++;
    if (e == p) return false;
    try { out = std::stod(body.substr(p, e - p)); } catch (...) { return false; }
    return true;
}

static std::string status_text(int c) {
    switch (c) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
    }
}

// A browser that navigates away (or aborts a fetch) closes its socket while
// we are still writing; on Unix that raises SIGPIPE, whose default action is
// to kill the process. MSG_NOSIGNAL suppresses it per-send where available,
// and main() ignores the signal outright for every other path.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void send_all(socket_t fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)send(fd, data.data() + sent, (int)(data.size() - sent), MSG_NOSIGNAL);
        if (n <= 0) break;  // peer hung up: stop writing, never die over it
        sent += n;
    }
}

// Is a byte (or an EOF) waiting on this socket? `ms == 0` polls and returns.
static bool readable(socket_t fd, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select((int)fd + 1, &rf, nullptr, nullptr, &tv) > 0;
}

// End a connection without provoking a reset.
//
// Releasing a socket handle discards its buffers on both platforms: bytes
// still queued in the SEND buffer (i.e. the reply we just wrote) and bytes
// left unread in the RECEIVE buffer both turn an ordinary close into RST,
// which the caller then reports as "connection forcibly closed"
// (WinError 10054 / ECONNRESET) even though the response was written
// successfully. Windows is the stricter of the two, which is where this was
// seen. So: shut the write side down first (that flushes the reply and queues
// FIN behind it), swallow whatever the peer still has in flight, wait for its
// EOF, and only then drop the handle.
static void close_gracefully(socket_t fd) {
    if (fd == BADSOCK) return;
#ifdef _WIN32
    shutdown(fd, SD_SEND);
#else
    shutdown(fd, SHUT_WR);
#endif
    char tmp[4096];
    int64_t drained = 0;
    const int64_t cap = (int64_t)96 << 20;  // never chase a runaway upload
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (drained < cap) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (!readable(fd, ms)) break;   // nothing more coming: the peer has it
        int n = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;              // EOF: reply delivered, peer closed
        drained += n;
    }
    CLOSESOCK(fd);
}

// Set while a request that asked for keep-alive is being served: respond()
// then advertises "Connection: keep-alive" and handle() reads another request
// off the same socket instead of closing it. Browsers reuse these connections,
// which removes the per-request connect/FIN round trip (and with it most of
// the churn that made a busy browser report dropped connections).
static thread_local bool t_keepalive = false;

// Keep-alive bounds: a connection may carry at most KEEPALIVE_MAX_REQ
// requests, and the gap between two of them may not exceed KEEPALIVE_IDLE_S
// seconds (browsers release an idle pooled connection after ~6 s anyway, so
// this only ever fires for a client that went away silently).
static constexpr int KEEPALIVE_IDLE_S = 15;
static constexpr int KEEPALIVE_MAX_REQ = 200;

// `extra` is appended after the standard headers (already CRLF-terminated).
static void respond(socket_t fd, int code, const std::string& ctype, const std::string& body,
                    const std::string& extra = "") {
    std::ostringstream h;
    h << "HTTP/1.1 " << code << " " << status_text(code) << "\r\n";
    h << "Content-Type: " << ctype << "\r\n";
    h << "Content-Length: " << body.size() << "\r\n";
    h << "X-Api-Version: " SERVER_VERSION "\r\n";
    h << (t_keepalive ? "Connection: keep-alive\r\nKeep-Alive: timeout=" +
                            std::to_string(KEEPALIVE_IDLE_S) + ", max=" +
                            std::to_string(KEEPALIVE_MAX_REQ) + "\r\n"
                      : "Connection: close\r\n");
    h << "Access-Control-Allow-Origin: *\r\n";
    h << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    h << "Access-Control-Allow-Headers: Content-Type\r\n";
    h << extra << "\r\n";
    send_all(fd, h.str());
    send_all(fd, body);
}

static void respond_json(socket_t fd, int code, const std::string& json, const std::string& extra = "") {
    respond(fd, code, "application/json; charset=utf-8", json, extra);
}

static void error_json(socket_t fd, int code, const std::string& msg) {
    respond_json(fd, code, "{\"error\":" + json_str(msg) + "}");
}

// Why a request could not be read. Only REQ_OK carries a request; REQ_PEER
// means the client hung up or stalled (nothing to answer). REQ_BAD and
// REQ_LARGE are answered (400 / 413) before the socket is released — bailing
// out silently while the client is still sending is what left bytes unread
// in the receive buffer and produced resets on the caller's side.
enum ReqStatus { REQ_OK = 0, REQ_PEER = 1, REQ_BAD = 2, REQ_LARGE = 3, REQ_NOLEN = 4 };

// One stalled client must never pin a thread for good.
static constexpr int READ_TIMEOUT_MS = 120000;

// Reads one request. `carry` holds bytes that arrived past the previous
// request's Content-Length (a pipelining client): they start this request and
// anything left over at the end is kept for the next one — never dropped.
static int read_request(socket_t fd, Request& r, std::string& carry) {
    std::string buf = carry;
    carry.clear();
    char tmp[4096];
    // `buf` may already hold a whole request (bytes carried over from the
    // previous one on this connection) — scan it before waiting for more.
    size_t header_end = buf.find("\r\n\r\n");
    // headers are bounded at 64 KB; the body is bounded by Content-Length up
    // to MAX_BODY (a 5 s reference voice is ~1 MB, text bodies are tiny).
    const size_t MAX_HEADER = 64 * 1024;
    const size_t MAX_BODY = 32u << 20;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(READ_TIMEOUT_MS);
    // wait for bytes, but never past the overall deadline
    auto wait_read = [&]() -> bool {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        return readable(fd, ms);
    };
    while (header_end == std::string::npos) {
        if (!wait_read()) return REQ_PEER;
        int n = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return REQ_PEER;
        buf.append(tmp, n);
        header_end = buf.find("\r\n\r\n");
        if (header_end == std::string::npos && buf.size() > MAX_HEADER) return REQ_LARGE;
    }
    std::string headers = buf.substr(0, header_end);
    std::string body = buf.substr(header_end + 4);
    std::istringstream hs(headers);
    std::string line;
    std::getline(hs, line);
    bool http11 = false;
    {
        size_t sp1 = line.find(' ');
        size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos) return REQ_BAD;
        r.method = line.substr(0, sp1);
        std::string target = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos
                                                                           : sp2 - sp1 - 1);
        http11 = line.find("HTTP/1.1") != std::string::npos;
        size_t qp = target.find('?');
        r.path = (qp == std::string::npos) ? target : target.substr(0, qp);
        if (qp != std::string::npos) {
            std::string qs = target.substr(qp + 1);
            std::istringstream qss(qs);
            std::string kv;
            while (std::getline(qss, kv, '&')) {
                size_t eq = kv.find('=');
                if (eq == std::string::npos) r.query.push_back({url_decode(kv), ""});
                else r.query.push_back({url_decode(kv.substr(0, eq)), url_decode(kv.substr(eq + 1))});
            }
        }
    }
    size_t content_length = 0;
    bool conn_close = false, conn_keep = false;
    while (std::getline(hs, line) && line != "\r" && !line.empty()) {
        std::string lower = line;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.rfind("content-length:", 0) == 0) {
            try {
                content_length = (size_t)std::stoull(line.substr(15));
            } catch (...) {
                return REQ_BAD;  // malformed header — never let it escape as an exception
            }
        }
        if (lower.rfind("connection:", 0) == 0) {
            conn_close = conn_close || lower.find("close") != std::string::npos;
            conn_keep = conn_keep || lower.find("keep-alive") != std::string::npos;
        }
        // bodies are read by Content-Length only — a chunked body would be
        // interpreted as the payload itself (it used to be synthesised as
        // "text"), so say so instead of guessing
        if (lower.rfind("transfer-encoding:", 0) == 0 &&
            lower.find("chunked") != std::string::npos) {
            return REQ_NOLEN;
        }
    }
    // HTTP/1.1 is persistent by default; HTTP/1.0 only on an explicit ask
    r.keepalive = !conn_close && (http11 || conn_keep);
    if (content_length > MAX_BODY) return REQ_LARGE;
    // anything past Content-Length is not part of this request — hand it to
    // the next request on this connection (or drop it when we close)
    if (body.size() > content_length) {
        carry = body.substr(content_length);
        body.resize(content_length);
    }
    while (body.size() < content_length) {
        if (!wait_read()) return REQ_PEER;
        int n = (int)recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return REQ_PEER;   // truncated body: ignore the request
        body.append(tmp, n);
        if (body.size() > MAX_BODY) return REQ_LARGE;
    }
    r.body = body;
    return REQ_OK;
}

static std::string sandbox_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') out.push_back(c);
    }
    if (out.empty()) out = "voice.wav";
    return out;
}

// --- graceful stop -----------------------------------------------------------
static volatile sig_atomic_t g_stop = 0;

static void on_stop_signal(int) { g_stop = 1; }

static void install_stop_handler() {
    signal(SIGINT, on_stop_signal);
    signal(SIGTERM, on_stop_signal);
#ifndef _WIN32
    // A client that hangs up mid-response is normal (the browser aborting a
    // request); without this the first one takes the whole server down.
    signal(SIGPIPE, SIG_IGN);
#endif
#ifdef _WIN32
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
            type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
            type == CTRL_SHUTDOWN_EVENT) {
            g_stop = 1;
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#endif
}

static void ioctlsocket_compat(socket_t s, u_long nonblock) {
#ifdef _WIN32
    ioctlsocket(s, FIONBIO, &nonblock);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void trim_memory() {
#ifdef _WIN32
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
#else
    malloc_trim(0);
#endif
}

static int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct App {
    // The model is loaded ONCE and kept for the life of the process (or until
    // an idle-unload fires), so a request never pays the ~1 s load again.
    std::unique_ptr<tts::Engine> eng;
    tts::EngineConfig cfg;
    fs::path base, web, voices, uploads;
    std::mutex mu;
    int idle_unload = 0;             // seconds; 0 = never unload
    int64_t last_use = 0;            // unix seconds, guarded by mu
    std::atomic<int> busy{0};        // in-flight requests, for clean shutdown

    std::string default_voice = "voices/male_hello.wav";
    std::vector<std::string> dict_files;   // extra dictionaries merged at load
    int port = 0;                          // the port actually bound

    // stats (guarded by mu)
    int64_t requests = 0, synths = 0, last_ms = 0, total_audio_ms = 0;

    // Loads on first use. Callers must hold mu, so the load and the use that
    // follows cannot be split by a concurrent /api/unload.
    tts::Engine* ensure_loaded_locked() {
        last_use = (int64_t)std::time(nullptr);
        if (!eng) {
            try {
                eng.reset(new tts::Engine(cfg));
                for (auto& f : dict_files) eng->dict().load_file(f);
                fprintf(stderr, "model loaded (%s)\n", cfg.base_dir.c_str());
            } catch (const std::exception& e) {
                fprintf(stderr, "model load failed: %s\n", e.what());
                return nullptr;
            }
        }
        return eng.get();
    }

    tts::Engine* engine() {
        std::lock_guard<std::mutex> lk(mu);
        return ensure_loaded_locked();
    }

    bool unload() {
        std::lock_guard<std::mutex> lk(mu);
        if (!eng) return false;
        eng.reset();
        trim_memory();
        return true;
    }

    bool loaded() {
        std::lock_guard<std::mutex> lk(mu);
        return (bool)eng;
    }

    bool idle_tick() {
        if (idle_unload <= 0) return false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!eng) return false;
            int64_t idle = (int64_t)std::time(nullptr) - last_use;
            if (idle < idle_unload) return false;
        }
        bool freed = unload();
        if (freed) fprintf(stderr, "idle -> model unloaded\n");
        return freed;
    }
};

// --- shared helpers ---------------------------------------------------------
static bool valid_voice_path(const std::string& v) { return v.find("..") == std::string::npos; }

static std::string wav_bytes(const std::vector<float>& audio, int sr) {
    std::vector<int16_t> pcm(audio.size());
    for (size_t i = 0; i < audio.size(); i++) {
        float v = std::max(-1.0f, std::min(1.0f, audio[i]));
        pcm[i] = (int16_t)std::lround(v * 32767.0f);
    }
    uint32_t data_len = (uint32_t)(pcm.size() * 2);
    std::string wav;
    wav.reserve(44 + data_len);
    auto put = [&](uint32_t v, int n) { for (int i = 0; i < n; i++) wav.push_back((char)((v >> (8 * i)) & 0xFF)); };
    auto put16 = [&](uint16_t v, int n) { for (int i = 0; i < n; i++) wav.push_back((char)((v >> (8 * i)) & 0xFF)); };
    wav += "RIFF"; put(36 + data_len, 4); wav += "WAVEfmt "; put(16, 4);
    put16(1, 2); put16(1, 2); put((uint32_t)sr, 4); put((uint32_t)sr * 2, 4); put16(2, 2); put16(16, 2);
    wav += "data"; put(data_len, 4);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_len);
    return wav;
}

// Builds the GenOptions shared by /api/synth, /api/phonemes and /api/chunks.
static std::string dict_lines(const std::string& spec);
static tts::GenOptions options_from(const Request& r) {
    tts::GenOptions o;
    o.mode = qstr(r, "mode", "split");
    if (o.mode != "pack") o.mode = "split";
    o.pace = qnum(r, "pace", 1.0);
    o.temperature = qnum(r, "temperature", qnum(r, "temp", -1.0));
    o.retries = qint(r, "retries", 3);
    o.chunk_tokens = qint(r, "chunk-tokens", qint(r, "chunk_tokens", 18));
    o.pause_scale = qnum(r, "pause-scale", qnum(r, "pause_scale", 1.0));
    o.keep_ezafe = qbool(r, "keep-ezafe", qbool(r, "keep_ezafe", false));
    o.zero_noise = qbool(r, "zero-noise", qbool(r, "zero_noise", false));
    std::string input = qstr(r, "input", "auto");
    // exactly one of the two: "phonemes" never runs the G2P, "text" always
    // does (numeric/symbol-only input), anything else auto-detects.
    o.input_phonemes = (input == "phonemes");
    o.input_text = (input == "text");
    std::string seed = r.q("seed", "");
    if (!seed.empty()) {
        try {
            o.seed = std::stoull(seed);
            o.seed_set = true;
        } catch (...) {}
    } else {
        // no seed given: derive one from the clock so repeated calls differ
        o.seed = (uint64_t)std::time(nullptr) * 2654435761u + (uint64_t)now_ms();
        o.seed_set = true;
    }
    o.dict = dict_lines(r.q("dict", ""));
    return o;
}

// "word=phon;word2=phon2" (already URL-decoded) -> dict lines.
static std::string dict_lines(const std::string& spec) {
    std::string out, cur;
    for (char c : spec) {
        if (c == ';' || c == '\n') {
            size_t eq = cur.find('=');
            if (eq != std::string::npos && eq > 0 && eq + 1 < cur.size())
                out += cur.substr(0, eq) + "\t" + cur.substr(eq + 1) + "\n";
            cur.clear();
        } else cur += c;
    }
    size_t eq = cur.find('=');
    if (eq != std::string::npos && eq > 0 && eq + 1 < cur.size())
        out += cur.substr(0, eq) + "\t" + cur.substr(eq + 1) + "\n";
    return out;
}

static const char* HELP_TEXT =
    SERVER_NAME " — Persian TTS HTTP API + web UI\n"
    "\n"
    "usage: farsi-tts-server [--base DIR] [--host H] [--port P] [--scan N]\n"
    "                        [--voice PATH] [--preload] [--dict FILE]\n"
    "                        [--seed N] [--threads N] [--idle-unload SEC]\n"
    "\n"
    "start-up\n"
    "  --base DIR        package dir with model/, voices/, web/, dicts/\n"
    "  --host H          bind address (default 127.0.0.1)\n"
    "  --port P          preferred port (default 8000). 0 = pick a free port\n"
    "  --scan N          if the port is busy, try P+1 .. P+N (default 32)\n"
    "  --no-scan         fail instead of moving to the next port\n"
    "  --threads N       ONNX Runtime threads (default: all cores, max 8)\n"
    "  --seed N          fixed seed (default: random per request)\n"
    "  --idle-unload S   free the weights after S idle seconds (0 = never)\n"
    "  --dict FILE       merge an extra dictionary file at model load\n"
    "\n"
    "voice\n"
    "  --voice PATH      default reference voice when ?voice= is omitted\n"
    "  --preload         load the model + encode the default voice at start-up\n"
    "                    (prints the cost; every later request reuses both)\n"
    "\n"
    "endpoints\n"
    "  GET  /                     web UI\n"
    "  GET  /api/help             this text\n"
    "  GET  /api/health           {\"ok\":true,\"model_loaded\":bool,\"port\":int}\n"
    "  GET  /api/config           defaults, limits, modes, voices, dict path\n"
    "  GET  /api/stats            request counters and timings\n"
    "  GET  /api/voices           [\"voices/x.wav\", ...]\n"
    "  POST /api/voice?name=x.wav body = raw WAV (RIFF/WAVE) -> {\"path\":..}\n"
    "                            (other formats are refused - convert first:\n"
    "                             ffmpeg -i in.mp3 -ar 24000 -ac 1 out.wav)\n"
    "  POST /api/voice/load       ?voice=  encode the reference voice once;\n"
    "                            {..,\"ms\":N,\"rms\":R,\"gain\":G,\"silent\":bool}\n"
    "                            (gain>1 = quiet reference amplified, silent =\n"
    "                             the file has no speech, the clone will be)\n"
    "                             -> {\"voice\":..,\"loaded\":true,\"ms\":int}\n"
    "  GET  /api/voice/state      {\"loaded\":[..],\"default\":\"..\"}\n"
    "  POST /api/phonemes         body = text -> {\"phonemes\":..,\"chunks\":[..],\n"
    "                                             \"words\":[..]}   (no audio)\n"
    "  POST /api/chunks           same input -> {\"chunks\":[..]}\n"
    "  POST /api/synth            body = text or phonemes -> audio/wav\n"
    "  GET  /api/dict             {\"entries\":[{\"word\":..,\"phonemes\":..}]}\n"
    "  GET  /api/dict?word=X      one lookup -> {\"found\":bool,\"phonemes\":..}\n"
    "  POST /api/dict             body {\"word\":\"..\",\"phonemes\":\"..\"} (or\n"
    "                             ?word= + raw phoneme body) -> adds, saves\n"
    "  DELETE /api/dict?word=X    removes and saves\n"
    "  POST /api/unload           free the weights (next request reloads, ~1 s)\n"
    "\n"
    "/api/synth parameters (query string; body = the text)\n"
    "  input=text|phonemes|auto  auto (default) = Persian script -> G2P, a\n"
    "                            romanised string is used as phonemes as-is;\n"
    "                            text = always run the G2P (numeric/symbol-only\n"
    "                            input), phonemes = never run it\n"
    "  voice=voices/x.wav        reference voice (see /api/voices)\n"
    "  mode=split|pack           split = pause at punctuation (default),\n"
    "                            pack = merge comma phrases into one breath\n"
    "  pace=0.6..1.5             speech rate (1.0 = unchanged)\n"
    "  seed=N                    reproducible output (default: random)\n"
    "  temperature=0.05..2.0     flow-LM noise temperature (default 0.3)\n"
    "  retries=1..4              quality-gate attempts per chunk (default 3)\n"
    "  chunk-tokens=8..32        SentencePiece budget per chunk (default 18)\n"
    "  pause-scale=0..3          multiply every pause (1.0 = normal)\n"
    "  keep-ezafe=1              keep the \"1\" ezafe marker in the phonemes\n"
    "  zero-noise=1              deterministic noise (verification only - it\n"
    "                            can silence output with some voices)\n"
    "  dict=\"word=phon;w2=ph2\"    extra dictionary entries for this request\n"
    "  format=json               instead of audio/wav, return\n"
    "                            {\"phonemes\":..,\"chunks\":[..],\"seconds\":..,\n"
    "                             \"ms\":int,\"total_chunks\":int,\"quiet_chunks\":int,\n"
    "                             \"seed\":\"..\",\"wav\":\"<base64>\"}\n"
    "                            (seed is a string so uint64 survives JSON;\n"
    "                             quiet_chunks>0 adds a \"warning\" and means part\n"
    "                             of the audio may be silent - try another seed)\n"
    "\n"
    "The WAV response also carries X-Phonemes (percent-encoded), X-Chunks,\n"
    "X-Quiet-Chunks, X-Audio-Seconds and X-Synth-Ms headers. Request bodies are\n"
    "capped at 32 MB (a 5 s reference voice is ~1 MB); oversized, malformed or\n"
    "chunked requests are answered with 413 / 400 / 411 rather than dropped.\n"
    "\n"
    "connections\n"
    "  HTTP/1.1 keep-alive is honoured: one connection carries up to 200\n"
    "  requests with at most 15 idle seconds between them (a browser reuses\n"
    "  its pooled connections, which removes the per-request connect/FIN\n"
    "  churn). \"Connection: close\" (or HTTP/1.0) is always respected, and a\n"
    "  connection is always ended gracefully — shutdown + drain + close, never\n"
    "  a reset — so a finished request is never reported as a dropped\n"
    "  connection (WinError 10054 / ECONNRESET) by the caller.\n"
    "  The web UI also POSTs /api/unload when its tab or window is closed, so\n"
    "  the ~500 MB of weights leave the process with the page.\n"
    "\n"
    "env: PARSIGO_HOST / PARSIGO_PORT override --host/--port.\n"
    "The model is loaded once and reused. Ctrl+C (or closing the console\n"
    "window) stops the listener and frees the port.\n";

// --- request handling -------------------------------------------------------
// Serves ONE request on this connection and reports whether another one may
// follow (keep-alive). It never closes the socket itself — `handle` below owns
// that — so an early `error_json(...) return` cannot leak a fd.
static bool handle_request(App& app, socket_t fd, std::string& carry) {
    Request r;
    t_keepalive = false;
    int rc = read_request(fd, r, carry);
    // Answer the rejections instead of just dropping the socket: a connection
    // released while the client is still sending gets reset, and the caller
    // sees "connection forcibly closed" instead of a useful status. Every
    // rejection ends the connection: the client's stream position is unknown.
    if (rc == REQ_BAD) {
        error_json(fd, 400, "malformed request");
        return false;
    }
    if (rc == REQ_LARGE) {
        error_json(fd, 413, "request too large (limit: 64 KB headers, 32 MB body)");
        return false;
    }
    if (rc == REQ_NOLEN) {
        error_json(fd, 411, "Length Required: bodies are read by Content-Length; "
                            "Transfer-Encoding: chunked is not supported");
        return false;
    }
    if (rc != REQ_OK) return false;  // peer hung up or stalled — nothing to answer
    {
        std::lock_guard<std::mutex> lk(app.mu);
        app.requests++;
    }
    // From here on the reply may say keep-alive; a handler that throws resets
    // the flag in handle() before it writes its 500.
    t_keepalive = r.keepalive;

    if (r.method == "OPTIONS") {
        respond(fd, 204, "text/plain", "");
    } else if (r.method == "GET" && (r.path == "/api/help" || r.path == "/help")) {
        respond(fd, 200, "text/plain; charset=utf-8", HELP_TEXT);
    } else if (r.method == "GET" && r.path == "/api/health") {
        bool l = app.loaded();
        std::string j = "{\"ok\":true,\"model_loaded\":" + std::string(l ? "true" : "false") +
                        ",\"version\":" + json_str(SERVER_VERSION) +
                        ",\"port\":" + std::to_string(app.port) +
                        ",\"base\":" + json_str(app.base.string()) + "}";
        respond_json(fd, 200, j);
    } else if ((r.method == "POST" || r.method == "GET") && r.path == "/api/unload") {
        respond_json(fd, 200, app.unload() ? "{\"unloaded\":true}" : "{\"unloaded\":false}");
    } else if (r.method == "GET" && r.path == "/api/stats") {
        std::lock_guard<std::mutex> lk(app.mu);
        std::string j = "{\"requests\":" + std::to_string(app.requests) +
                        ",\"synths\":" + std::to_string(app.synths) +
                        ",\"in_flight\":" + std::to_string(app.busy.load()) +
                        ",\"last_ms\":" + std::to_string(app.last_ms) +
                        ",\"total_audio_ms\":" + std::to_string(app.total_audio_ms) +
                        ",\"model_loaded\":" + (app.eng ? "true" : "false") + "}";
        respond_json(fd, 200, j);
    } else if (r.method == "GET" && r.path == "/api/config") {
        bool l = app.loaded();
        double temp_def = 0.3;
        int threads = 0, sr = 24000;
        std::string dict_path = (app.base / "dicts" / "user.dict").string();
        size_t dict_entries = 0;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            if (app.eng) {
                temp_def = app.eng->default_temperature();
                threads = app.eng->intra_threads();
                sr = app.eng->sample_rate();
                dict_entries = app.eng->dict().size();
            }
        }
        tts::Dictionary probe;
        if (!l) {
            probe.load_file(dict_path);
            dict_entries = probe.size();
        }
        std::ostringstream j;
        j << "{\"version\":" << json_str(SERVER_VERSION)
          << ",\"port\":" << app.port
          << ",\"base\":" << json_str(app.base.string())
          << ",\"model_loaded\":" << (l ? "true" : "false")
          << ",\"sample_rate\":" << sr
          << ",\"threads\":" << threads
          << ",\"dict_path\":" << json_str(dict_path)
          << ",\"dict_entries\":" << dict_entries
          << ",\"default_voice\":" << json_str(app.default_voice)
          << ",\"defaults\":{\"mode\":\"split\",\"pace\":1.0,\"temperature\":" << json_num(temp_def)
          << ",\"retries\":3,\"chunk_tokens\":18,\"pause_scale\":1.0}"
          << ",\"limits\":{\"pace\":[0.6,1.5],\"temperature\":[0.05,2.0],\"retries\":[1,4],"
             "\"chunk_tokens\":[8,32],\"pause_scale\":[0,3]}"
          << ",\"modes\":[{\"value\":\"split\",\"label\":\"با مکث ویرگول‌ها\","
             "\"desc\":\"pause at every comma/semicolon (reading aloud)\"},"
             "{\"value\":\"pack\",\"label\":\"یکپارچه و روان\","
             "\"desc\":\"merge comma phrases into longer breaths\"}]}";
        respond_json(fd, 200, j.str());
    } else if (r.method == "GET" && r.path == "/api/voices") {
        std::string json = "[";
        bool first = true;
        auto add = [&](const fs::path& dir, const std::string& prefix) {
            std::error_code ec;
            if (!fs::exists(dir, ec)) return;
            std::vector<std::string> names;
            for (auto& e : fs::directory_iterator(dir, ec)) {
                std::string ext = e.path().extension().string();
                for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext == ".wav")  // the only format we can decode
                    names.push_back(e.path().filename().string());
            }
            std::sort(names.begin(), names.end());
            for (auto& n : names) {
                if (!first) json += ",";
                first = false;
                json += json_str(prefix + n);
            }
        };
        add(app.voices, "voices/");
        add(app.uploads, "uploads/");
        json += "]";
        respond_json(fd, 200, json);
    } else if (r.method == "POST" && r.path == "/api/voice") {
        std::string name = sandbox_name(r.q("name", "upload.wav"));
        // Only RIFF/WAVE can be decoded here. This has to run on an offline
        // Win7 box, so there is deliberately no mp3/ogg/flac decoder — reject
        // them here with a fix instead of crashing on the first load.
        if (r.body.size() < 12 || r.body.compare(0, 4, "RIFF") != 0 ||
            r.body.compare(8, 4, "WAVE") != 0) {
            error_json(fd, 400,
                       "reference voices must be WAV (RIFF/WAVE) - convert first: "
                       "ffmpeg -i in.mp3 -ar 24000 -ac 1 out.wav");
            return false;
        }
        std::error_code ec;
        fs::create_directories(app.uploads, ec);
        fs::path out = app.uploads / name;
        std::ofstream f(out, std::ios::binary);
        f.write(r.body.data(), (std::streamsize)r.body.size());
        f.close();
        respond_json(fd, 200, "{\"path\":" + json_str("uploads/" + name) + "}");
    } else if (r.method == "POST" && r.path == "/api/voice/load") {
        std::string voice = qstr(r, "voice", app.default_voice);
        if (!valid_voice_path(voice)) { error_json(fd, 400, "bad voice path"); return false; }
        fs::path vp = app.base / voice;
        if (!fs::exists(vp)) { error_json(fd, 400, "voice not found"); return false; }
        int64_t t0 = now_ms();
        std::lock_guard<std::mutex> lk(app.mu);
        tts::Engine* e = app.ensure_loaded_locked();
        if (!e) { error_json(fd, 503, "model load failed"); return false; }
        bool cached = e->voice_loaded(vp.string());
        e->preload_voice(vp.string());
        int64_t ms = now_ms() - t0;
        // how loud the reference was, so the UI can explain a quiet clone:
        // rms = original level, gain = correction applied (>1 = amplified),
        // silent = no usable speech in the file at all.
        float rms = 0, gain = 1;
        e->voice_stats(vp.string(), &rms, &gain);
        char lvl[96];
        snprintf(lvl, sizeof(lvl), ",\"rms\":%.4f,\"gain\":%.1f,\"silent\":%s", (double)rms, (double)gain,
                 (rms < 1e-4) ? "true" : "false");
        respond_json(fd, 200, "{\"voice\":" + json_str(voice) + ",\"loaded\":true,\"cached\":" +
                                  (cached ? "true" : "false") + ",\"ms\":" + std::to_string(ms) + lvl + "}");
    } else if (r.method == "GET" && r.path == "/api/voice/state") {
        std::lock_guard<std::mutex> lk(app.mu);
        std::string j = "{\"default\":" + json_str(app.default_voice) + ",\"loaded\":[";
        if (app.eng) {
            auto v = app.eng->loaded_voices();
            for (size_t i = 0; i < v.size(); i++) {
                if (i) j += ",";
                // report package-relative paths where possible
                fs::path p(v[i]);
                std::string rel;
                std::error_code ec;
                fs::path rr = fs::relative(p, app.base, ec);
                rel = ec ? p.string() : rr.generic_string();
                j += json_str(rel);
            }
        }
        j += std::string("],\"model_loaded\":") + (app.eng ? "true" : "false") + "}";
        respond_json(fd, 200, j);
    } else if (r.method == "GET" && r.path == "/api/dict") {
        std::string word = r.q("word", "");
        std::string path = (app.base / "dicts" / "user.dict").string();
        tts::Dictionary d;
        {
            std::lock_guard<std::mutex> lk(app.mu);
            if (app.eng) d = app.eng->dict();
        }
        if (d.empty()) d.load_file(path);
        if (!word.empty()) {
            std::string ph = d.lookup(word);
            if (ph.empty())
                respond_json(fd, 200, "{\"word\":" + json_str(word) + ",\"found\":false}");
            else
                respond_json(fd, 200,
                             "{\"word\":" + json_str(word) + ",\"found\":true,\"phonemes\":" +
                                 json_str(ph) + "}");
            return false;
        }
        std::ostringstream j;
        j << "{\"path\":" << json_str(path) << ",\"entries\":[";
        auto es = d.entries();
        for (size_t i = 0; i < es.size(); i++) {
            if (i) j << ",";
            j << "{\"word\":" << json_str(es[i].word) << ",\"phonemes\":" << json_str(es[i].phonemes) << "}";
        }
        j << "]}";
        respond_json(fd, 200, j.str());
    } else if (r.method == "POST" && r.path == "/api/dict") {
        std::string word = r.q("word", ""), ph = "";
        if (!json_get(r.body, "word", word)) word = r.q("word", "");
        if (!json_get(r.body, "phonemes", ph)) ph = r.body;
        if (word.empty() || ph.empty()) {
            error_json(fd, 400, "need a word and phonemes (JSON {\"word\":..,\"phonemes\":..} "
                                "or ?word= with the phonemes as the body)");
            return false;
        }
        std::string path = (app.base / "dicts" / "user.dict").string();
        std::lock_guard<std::mutex> lk(app.mu);
        tts::Dictionary d;
        if (app.eng) d = app.eng->dict();
        if (d.empty()) d.load_file(path);
        if (!d.set(word, ph)) { error_json(fd, 400, "empty word or phonemes"); return false; }
        if (!d.save_file(path)) { error_json(fd, 500, "cannot write " + path); return false; }
        if (app.eng) app.eng->set_dict(d);
        respond_json(fd, 200, "{\"ok\":true,\"word\":" + json_str(word) +
                                  ",\"entries\":" + std::to_string(d.size()) +
                                  ",\"path\":" + json_str(path) + "}");
    } else if (r.method == "DELETE" && r.path == "/api/dict") {
        std::string word = r.q("word", "");
        if (word.empty()) { error_json(fd, 400, "need ?word="); return false; }
        std::string path = (app.base / "dicts" / "user.dict").string();
        std::lock_guard<std::mutex> lk(app.mu);
        tts::Dictionary d;
        if (app.eng) d = app.eng->dict();
        if (d.empty()) d.load_file(path);
        bool removed = d.erase(word);
        if (removed && !d.save_file(path)) { error_json(fd, 500, "cannot write " + path); return false; }
        if (app.eng) app.eng->set_dict(d);
        respond_json(fd, 200, std::string("{\"ok\":true,\"removed\":") + (removed ? "true" : "false") +
                                  ",\"entries\":" + std::to_string(d.size()) + "}");
    } else if ((r.method == "POST" || r.method == "GET") && r.path == "/api/phonemes") {
        std::string text = r.body.empty() ? r.q("text") : r.body;
        if (text.empty()) { error_json(fd, 400, "empty text"); return false; }
        tts::GenOptions o = options_from(r);
        try {
            std::lock_guard<std::mutex> lk(app.mu);
            tts::Engine* e = app.ensure_loaded_locked();
            if (!e) { error_json(fd, 503, "model load failed"); return false; }
            std::string ph = e->phonemise_text(text, o);
            auto chunks = e->plan_chunks(text, o);
            std::ostringstream j;
            j << "{\"phonemes\":" << json_str(ph) << ",\"input\":" << json_str(text) << ",\"words\":[";
            bool first = true;
            std::istringstream ss(ph);
            std::string w;
            while (ss >> w) {
                if (!first) j << ",";
                first = false;
                j << json_str(w);
            }
            j << "],\"chunks\":[";
            for (size_t i = 0; i < chunks.size(); i++) {
                if (i) j << ",";
                j << json_str(chunks[i]);
            }
            j << "]}";
            respond_json(fd, 200, j.str());
        } catch (const std::exception& e) {
            error_json(fd, 500, e.what());
        }
    } else if ((r.method == "POST" || r.method == "GET") && r.path == "/api/chunks") {
        std::string text = r.body.empty() ? r.q("text") : r.body;
        if (text.empty()) { error_json(fd, 400, "empty text"); return false; }
        tts::GenOptions o = options_from(r);
        try {
            std::lock_guard<std::mutex> lk(app.mu);
            tts::Engine* e = app.ensure_loaded_locked();
            if (!e) { error_json(fd, 503, "model load failed"); return false; }
            auto chunks = e->plan_chunks(text, o);
            std::ostringstream j;
            j << "{\"chunks\":[";
            for (size_t i = 0; i < chunks.size(); i++) {
                if (i) j << ",";
                j << json_str(chunks[i]);
            }
            j << "],\"count\":" << chunks.size() << "}";
            respond_json(fd, 200, j.str());
        } catch (const std::exception& e) {
            error_json(fd, 500, e.what());
        }
    } else if ((r.method == "POST" || r.method == "GET") && r.path == "/api/synth") {
        std::string text = r.body.empty() ? r.q("text") : r.body;
        std::string voice = qstr(r, "voice", app.default_voice);
        tts::GenOptions o = options_from(r);
        if (text.empty() || text.find_first_not_of(" \t\r\n") == std::string::npos) {
            error_json(fd, 400, "empty text");
            return false;
        }
        if (!valid_voice_path(voice)) { error_json(fd, 400, "bad voice path"); return false; }
        fs::path vp = app.base / voice;
        if (!fs::exists(vp)) { error_json(fd, 400, "voice not found: " + voice); return false; }
        try {
            tts::SynthResult res;
            int sr = 24000;
            int64_t t0 = now_ms();
            {
                std::lock_guard<std::mutex> lk(app.mu);
                tts::Engine* e = app.ensure_loaded_locked();
                if (!e) { error_json(fd, 503, "model load failed"); return false; }
                res = e->synthesize(text, vp.string(), o);
                sr = e->sample_rate();
                // idle is measured from the END of the request: a long
                // synthesis must not look idle while it runs
                app.last_use = (int64_t)std::time(nullptr);
                app.synths++;
                app.last_ms = now_ms() - t0;
                app.total_audio_ms += (int64_t)(res.audio.size() * 1000.0 / sr);
            }
            int64_t ms = now_ms() - t0;
            if (res.audio.empty()) { error_json(fd, 400, "nothing to synthesise (empty text?)"); return false; }
            double secs = res.audio.size() / (double)sr;
            std::ostringstream h;
            h << "X-Phonemes: " << url_encode(res.phonemes) << "\r\n"
              << "X-Chunks: " << res.chunks.size() << "\r\n"
              << "X-Quiet-Chunks: " << res.quiet_chunks << "\r\n"
              << "X-Audio-Seconds: " << json_num(secs) << "\r\n"
              << "X-Synth-Ms: " << ms << "\r\n";
            if (qstr(r, "format", "") == "json") {
                std::ostringstream j;
                j << "{\"phonemes\":" << json_str(res.phonemes)
                  << ",\"from_phonemes\":" << (res.from_phonemes ? "true" : "false")
                  << ",\"chunks\":[";
                for (size_t i = 0; i < res.chunks.size(); i++) {
                    if (i) j << ",";
                    j << json_str(res.chunks[i]);
                }
                j << "],\"seconds\":" << json_num(secs)
                  << ",\"sample_rate\":" << sr
                  << ",\"ms\":" << ms
                  << ",\"total_chunks\":" << res.total_chunks
                  << ",\"quiet_chunks\":" << res.quiet_chunks;
                // a chunk that failed every retry is silent audio: say so in
                // the payload instead of shipping the silence unannounced
                if (res.quiet_chunks > 0)
                    j << ",\"warning\":" << json_str(
                          std::to_string(res.quiet_chunks) + " of " +
                          std::to_string(res.total_chunks) +
                          " chunk(s) failed the quality gate after every retry; that part of the audio may be silent or truncated");
                j << ",\"seed\":" << (o.seed_set ? json_str(std::to_string(o.seed)) : "null")
                  << ",\"wav\":" << json_str(base64(wav_bytes(res.audio, sr))) << "}";
                respond_json(fd, 200, j.str(), h.str());
            } else {
                respond(fd, 200, "audio/wav", wav_bytes(res.audio, sr), h.str());
            }
        } catch (const std::exception& e) {
            error_json(fd, 500, e.what());
        }
    } else if (r.method == "GET") {
        std::string rel = r.path == "/" ? "/index.html" : r.path;
        if (rel.find("..") != std::string::npos) {
            respond(fd, 400, "text/plain", "bad path");
        } else {
            fs::path fp = app.web / rel.substr(1);
            std::ifstream f(fp, std::ios::binary);
            if (!f) {
                respond(fd, 404, "text/plain", "not found");
            } else {
                std::ostringstream ss;
                ss << f.rdbuf();
                std::string ext = fp.extension().string();
                std::string ct = (ext == ".css") ? "text/css" : (ext == ".js") ? "application/javascript"
                                    : (ext == ".ttf") ? "font/ttf"
                                    : "text/html; charset=utf-8";
                respond(fd, 200, ct, ss.str());
            }
        }
    } else {
        respond(fd, 404, "text/plain", "not found");
    }
    return r.keepalive;
}

// One connection = one `handle`. Three things leave this function: the normal
// fall-through, an early `error_json(...) return`, and an escaped exception.
// The guard closes the socket exactly once for all three, and an exception
// becomes a 500 instead of killing the server — one unreadable voice file
// used to take the whole process down (std::terminate).
//
// A connection may carry up to KEEPALIVE_MAX_REQ requests; between two of
// them the peer gets KEEPALIVE_IDLE_S seconds to show up, otherwise the
// connection is closed the graceful way (FIN + drain, never a reset).
static void handle(App& app, socket_t fd) {
    struct CloseGuard {
        socket_t fd;
        ~CloseGuard() { close_gracefully(fd); }  // flush + FIN + drain, no RST
    } guard{fd};
    std::string carry;  // bytes past the previous request's Content-Length
    for (int served = 0; served < KEEPALIVE_MAX_REQ; served++) {
        bool again = false;
        try {
            again = handle_request(app, fd, carry);
        } catch (const std::exception& e) {
            fprintf(stderr, "farsi-tts-server: request failed: %s\n", e.what());
            t_keepalive = false;  // the 500 below must say close, then we do
            try {
                error_json(fd, 500, std::string("internal error: ") + e.what());
            } catch (...) {
            }
        } catch (...) {
            fprintf(stderr, "farsi-tts-server: request failed: unknown error\n");
            t_keepalive = false;
            try {
                error_json(fd, 500, "internal error");
            } catch (...) {
            }
        }
        if (!again) return;
        t_keepalive = false;
        // wait for the next request, but never longer than the idle bound
        // (bytes already in `carry` are the start of that request: no wait)
        if (carry.empty() && !readable(fd, KEEPALIVE_IDLE_S * 1000)) return;
    }
}

// --- listening socket -------------------------------------------------------
// Bind `host:port`; if the port is taken, walk up to `scan` more ports so two
// copies (or another app) never stop the server from starting.
static socket_t bind_listener(const std::string& host, int port, int scan, int& chosen) {
    for (int i = 0; i <= scan; i++) {
        int p = port + i;
        if (p <= 0 || p > 65535) break;
        socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv == BADSOCK) { fprintf(stderr, "socket failed\n"); return BADSOCK; }
        int one = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)p);
        addr.sin_addr.s_addr = inet_addr(host.c_str());
        if (bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) {
            CLOSESOCK(srv);
            if (i == 0 && scan > 0)
                fprintf(stderr, "port %d busy, looking for a free one...\n", port);
            continue;
        }
        if (listen(srv, 16) != 0) { CLOSESOCK(srv); continue; }
        chosen = p;
        return srv;
    }
    return BADSOCK;
}

static void print_usage() { fputs(HELP_TEXT, stdout); }

int main(int argc, char** argv) {
    std::string base = ".", host = "127.0.0.1";
    int port = 8000, scan = 32;
    bool seeded = false, preload = false, no_scan = false;
    unsigned long long seed = 0;
    int threads = 0;   // 0 = auto
    int idle_unload = 0;  // 0 = never free the model automatically
    std::string default_voice = "voices/male_hello.wav";
    std::vector<std::string> dict_files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "error: %s needs a value\n", what); exit(1); }
            return argv[++i];
        };
        if (a == "--base") base = next("--base");
        else if (a == "--host") host = next("--host");
        else if (a == "--port") port = std::stoi(next("--port"));
        else if (a == "--scan") scan = std::stoi(next("--scan"));
        else if (a == "--no-scan") no_scan = true;
        else if (a == "--seed") { seed = std::stoull(next("--seed")); seeded = true; }
        else if (a == "--threads") threads = std::stoi(next("--threads"));
        else if (a == "--idle-unload") idle_unload = std::stoi(next("--idle-unload"));
        else if (a == "--voice") default_voice = next("--voice");
        else if (a == "--dict") dict_files.push_back(next("--dict"));
        else if (a == "--preload") preload = true;
        else if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else {
            fprintf(stderr, "error: unknown option \"%s\" (try --help)\n", a.c_str());
            return 1;
        }
    }
    if (const char* p = std::getenv("PARSIGO_PORT")) port = std::stoi(p);
    if (const char* h = std::getenv("PARSIGO_HOST")) host = h;
    if (no_scan) scan = 0;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    App app;
    app.base = base;
    app.web = fs::path(base) / "web";
    app.voices = fs::path(base) / "voices";
    app.uploads = fs::path(base) / "uploads";
    app.idle_unload = idle_unload;
    app.default_voice = default_voice;
    app.dict_files = dict_files;
    std::error_code ec;
    fs::create_directories(app.uploads, ec);

    tts::EngineConfig cfg;
    cfg.base_dir = base;
    cfg.seed_set = seeded;
    cfg.seed = seed;
    cfg.intra_threads = threads;
    app.cfg = cfg;

    int chosen = port;
    socket_t srv = bind_listener(host, port, scan, chosen);
    if (srv == BADSOCK) {
        fprintf(stderr, "bind failed on %s:%d (tried %d ports)\n", host.c_str(), port, scan + 1);
        return 1;
    }
    app.port = chosen;
    std::string port_note;
    if (chosen != port) port_note = "  (port " + std::to_string(port) + " was busy)";
    printf(SERVER_NAME "\n");
    printf("farsi-tts server: http://%s:%d%s\n", host.c_str(), chosen, port_note.c_str());
    printf("help:   curl http://%s:%d/api/help\n", host.c_str(), chosen);
    if (app.idle_unload > 0)
        printf("model idle-unload: %ds (POST /api/unload to free it now)\n", app.idle_unload);
    fflush(stdout);

    if (preload) {
        int64_t t0 = now_ms();
        std::lock_guard<std::mutex> lk(app.mu);
        tts::Engine* e = app.ensure_loaded_locked();
        if (e) {
            std::string v = default_voice;
            bool abs_v = !v.empty() && (v[0] == '/' || v[0] == '\\' || v.find(':') != std::string::npos);
            fs::path vp = abs_v ? fs::path(v) : app.base / v;
            try {
                if (fs::exists(vp)) e->preload_voice(vp.string());
            } catch (const std::exception& ex) {
                // a bad --preload-voice file must not stop the server from starting
                fprintf(stderr, "preload: %s (%s)\n", ex.what(), v.c_str());
            }
            printf("preloaded in %lld ms (model + voice)\n", (long long)(now_ms() - t0));
        } else {
            fprintf(stderr, "preload failed (model not loaded)\n");
        }
        fflush(stdout);
    }

    g_stop = 0;
    install_stop_handler();

    u_long nonblock = 1;
    ioctlsocket_compat(srv, nonblock);

    while (!g_stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv, &rd);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int n = select((int)srv + 1, &rd, nullptr, nullptr, &tv);
        if (n > 0) {
            socket_t fd = accept(srv, nullptr, nullptr);
            if (fd != BADSOCK) {
                app.busy++;
                try {
                    std::thread([&app, fd] {
                        try {
                            handle(app, fd);
                        } catch (...) {
                            // handle() answers its own errors; this is a belt
                        }
                        app.busy--;
                    }).detach();
                } catch (...) {
                    // out of threads: drop this one connection cleanly instead of
                    // letting std::thread's exception terminate the server
                    app.busy--;
                    close_gracefully(fd);
                }
            }
        }
        app.idle_tick();
    }

    CLOSESOCK(srv);
    printf("farsi-tts server: stopping");
    for (int i = 0; i < 300 && app.busy > 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("%s\n", app.busy > 0 ? " (requests still running, forcing exit)"
                                : " (model released, port closed)");
    fflush(stdout);
    return 0;
}
