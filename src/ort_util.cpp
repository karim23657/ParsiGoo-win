#include "ort_util.h"

#include <stdexcept>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tts {

static const OrtApi* g_api = nullptr;
static OrtEnv* g_env = nullptr;
static OrtMemoryInfo* g_mem = nullptr;

#ifdef _WIN32
static HMODULE g_ort_dll = nullptr;
static const OrtApiBase* g_base = nullptr;

// onnxruntime.dll is loaded explicitly instead of being a static import.
//
// The official 1.14.1 binary links against Windows-8+ API-set contracts that
// Windows 7 does not have; a *static* import makes the loader touch them while
// it is still creating the process, which killed every exe at startup with
// "The application was unable to start correctly (0xc0000005)" before main()
// could run.  Loading the DLL here (after startup, inside our own error path)
// keeps the failure visible and reportable instead of fatal to the process.
// scripts/patch_ort_win7.py + w7shim.dll make the load itself succeed on
// Windows 7.
static void load_ort() {
    if (g_ort_dll) return;
    // Breadcrumb written *before* LoadLibrary so that, in the worst case, the
    // last line on the console says exactly which DLL was being loaded.
    std::fprintf(stderr, "farsi-tts: loading onnxruntime.dll ...\n");
    std::fflush(stderr);
    OutputDebugStringA("farsi-tts: loading onnxruntime.dll ...\n");
    g_ort_dll = LoadLibraryW(L"onnxruntime.dll");
    if (!g_ort_dll) {
        DWORD e = GetLastError();
        const char* hint = "";
        if (e == 126) hint = " (missing dependency: install vc_redist.x64 + KB2999226, "
                             "and keep onnxruntime.dll + w7shim.dll next to the exe)";
        else if (e == 127) hint = " (a required function was not found)";
        else if (e == 193) hint = " (not a valid Win32 image - wrong architecture)";
        char buf[512];
        std::snprintf(buf, sizeof buf,
                      "cannot load onnxruntime.dll (Windows error %lu)%s",
                      (unsigned long)e, hint);
        std::fprintf(stderr, "farsi-tts: %s\n", buf);
        std::fflush(stderr);
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        throw std::runtime_error(buf);
    }
    typedef const OrtApiBase* (ORT_API_CALL* GetBaseFn)(void);
    GetBaseFn fn = (GetBaseFn)(void*)GetProcAddress(g_ort_dll, "OrtGetApiBase");
    if (!fn) throw std::runtime_error("onnxruntime.dll: OrtGetApiBase export not found");
    g_base = fn();
    if (!g_base) throw std::runtime_error("onnxruntime.dll: OrtGetApiBase() returned null");
    std::fprintf(stderr, "farsi-tts: onnxruntime.dll loaded, ORT %s\n",
                 g_base->GetVersionString());
    std::fflush(stderr);
}
#endif  // _WIN32

static void fail(const char* what, OrtStatus* st) {
    std::string msg = what;
    if (st) {
        msg += ": ";
        msg += ort()->GetErrorMessage(st);
        ort()->ReleaseStatus(st);
    }
    throw std::runtime_error(msg);
}

const OrtApi* ort() {
    if (!g_api) {
#ifdef _WIN32
        load_ort();
        g_api = g_base->GetApi(ORT_API_VERSION);
#else
        g_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
#endif
    }
    if (!g_api) throw std::runtime_error("onnxruntime: GetApi failed");
    return g_api;
}

OrtEnv* ort_env() {
    if (!g_env) {
        const OrtApi* a = ort();
        OrtStatus* st = a->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "farsi-tts", &g_env);
        if (st) fail("CreateEnv", st);
        if (!g_mem) {
            st = a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_mem);
            if (st) fail("CreateCpuMemoryInfo", st);
        }
    }
    return g_env;
}

OrtValue* make_tensor(const std::vector<int64_t>& shape, void* data, ONNXTensorElementDataType type) {
    const OrtApi* a = ort();
    ort_env();
    OrtValue* v = nullptr;
    size_t bytes = 1;
    for (auto d : shape) bytes *= (size_t)(d < 0 ? 0 : d);
    size_t elem = 4;
    if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) elem = 8;
    else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL || type == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) elem = 1;
    else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) elem = 4;
    OrtStatus* st = a->CreateTensorWithDataAsOrtValue(
        g_mem, data, bytes * elem, shape.data(), shape.size(), type, &v);
    if (st) fail("CreateTensorWithDataAsOrtValue", st);
    return v;
}

void ort_release(OrtValue* v) { if (v) ort()->ReleaseValue(v); }

void* tensor_data(const OrtValue* v) {
    void* p = nullptr;
    OrtStatus* st = ort()->GetTensorMutableData(const_cast<OrtValue*>(v), &p);
    if (st) fail("GetTensorMutableData", st);
    return p;
}

std::vector<int64_t> tensor_shape(const OrtValue* v) {
    const OrtApi* a = ort();
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (a->GetTensorTypeAndShape(v, &info)) throw std::runtime_error("GetTensorTypeAndShape failed");
    size_t rank = 0;
    a->GetDimensionsCount(info, &rank);
    std::vector<int64_t> shape(rank);
    if (rank) a->GetDimensions(info, shape.data(), rank);
    a->ReleaseTensorTypeAndShapeInfo(info);
    return shape;
}

ONNXTensorElementDataType tensor_type(const OrtValue* v) {
    const OrtApi* a = ort();
    OrtTensorTypeAndShapeInfo* info = nullptr;
    if (a->GetTensorTypeAndShape(v, &info)) throw std::runtime_error("GetTensorTypeAndShape failed");
    ONNXTensorElementDataType t;
    a->GetTensorElementType(info, &t);
    a->ReleaseTensorTypeAndShapeInfo(info);
    return t;
}

// ORTCHAR_T is wchar_t on Windows: session paths must be widened.
static std::basic_string<ORTCHAR_T> to_ort_path(const std::string& s) {
#ifdef _WIN32
    bool ascii = true;
    for (unsigned char c : s) if (c >= 0x80) { ascii = false; break; }
    if (ascii) return std::wstring(s.begin(), s.end());
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    return s;
#endif
}

Session::Session(const std::string& path, int intra_threads, int inter_threads) {
    const OrtApi* a = ort();
    OrtSessionOptions* opts = nullptr;
    if (a->CreateSessionOptions(&opts)) fail("CreateSessionOptions", nullptr);
    a->SetIntraOpNumThreads(opts, intra_threads);
    a->SetInterOpNumThreads(opts, inter_threads);
    a->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    std::basic_string<ORTCHAR_T> wpath = to_ort_path(path);
    OrtStatus* st = a->CreateSession(ort_env(), wpath.c_str(), opts, &s_);
    a->ReleaseSessionOptions(opts);
    if (st) fail(("CreateSession " + path).c_str(), st);

    size_t n = 0;
    a->SessionGetInputCount(s_, &n);
    for (size_t i = 0; i < n; i++) {
        OrtAllocator* tmp = nullptr;
        a->GetAllocatorWithDefaultOptions(&tmp);
        char* name = nullptr;
        if (!a->SessionGetInputName(s_, i, tmp, &name)) {
            in_.push_back(name);
            tmp->Free(tmp, name);
        }
    }
    a->SessionGetOutputCount(s_, &n);
    for (size_t i = 0; i < n; i++) {
        OrtAllocator* tmp = nullptr;
        a->GetAllocatorWithDefaultOptions(&tmp);
        char* name = nullptr;
        if (!a->SessionGetOutputName(s_, i, tmp, &name)) {
            out_.push_back(name);
            tmp->Free(tmp, name);
        }
    }
}

Session::~Session() {
    if (s_) ort()->ReleaseSession(s_);
}

std::vector<OrtValue*> Session::run_ordered(const std::vector<OrtValue*>& values) {
    std::vector<const char*> names;
    for (auto& s : in_) names.push_back(s.c_str());
    return run(names, values);
}

std::vector<OrtValue*> Session::run(const std::vector<const char*>& names,
                                    const std::vector<OrtValue*>& values) {
    const OrtApi* a = ort();
    std::vector<const char*> out_names;
    out_names.reserve(out_.size());
    for (auto& s : out_) out_names.push_back(s.c_str());
    std::vector<OrtValue*> outs(out_.size(), nullptr);
    OrtStatus* st = a->Run(s_, nullptr, names.data(), values.data(), names.size(),
                           out_names.data(), outs.size(), outs.data());
    if (st) {
        std::string m = "Run failed";
        if (st) { m += ": "; m += a->GetErrorMessage(st); a->ReleaseStatus(st); }
        throw std::runtime_error(m);
    }
    return outs;
}

}  // namespace tts
