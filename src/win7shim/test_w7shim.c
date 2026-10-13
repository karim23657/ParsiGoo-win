/* w7shim-test.exe - environment self-test + unit test for w7shim.dll.
 *
 * Deliberately imports nothing but KERNEL32/msvcrt so it starts even on a
 * machine where the api-set chain is broken (the state it exists to diagnose).
 *
 *   x86_64-w64-mingw32-gcc -O2 -I build-win/shim \
 *       src/win7shim/test_w7shim.c build-win/shim/win7shim_gen.S \
 *       -o build-win/w7shim-test.exe
 *
 * Sections:
 *   1. required files next to the exe
 *   2. LoadLibrary probes - the first failure is the module that produced the
 *      "procedure entry point ... could not be located" dialog
 *   3. where GetACP lives (the reported symptom)
 *   4. our Windows-8+ implementations vs the real OS ones (only compared when
 *      the OS actually has them; on Windows 7 they are skipped)
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "win7shim.c"   /* the implementations under test (+ win7shim_slots) */

static int g_fail = 0;
#define CHECK(cond, msg) do { \
        if (cond) printf("  ok   %s\n", msg); \
        else { printf("  FAIL %s (err=%lu)\n", msg, (unsigned long)GetLastError()); g_fail = 1; } \
    } while (0)

/* --------------------------------------------------------------- helpers */
static void path_beside_exe(wchar_t* out, size_t n, const wchar_t* file) {
    wchar_t mod[MAX_PATH];
    out[0] = 0;
    if (GetModuleFileNameW(NULL, mod, MAX_PATH)) {
        wchar_t* sl = wcsrchr(mod, L'\\');
        if (sl) { *sl = 0; wcsncpy(out, mod, n - 1); wcsncat(out, L"\\", n - 1); }
        wcsncat(out, file, n - 1);
    }
}

static int file_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void probe_file(const wchar_t* name) {
    wchar_t p[MAX_PATH];
    path_beside_exe(p, MAX_PATH, name);
    if (file_exists(p)) {
        printf("  ok   present: %ls\n", name);
    } else {
        printf("  FAIL missing next to the exe: %ls\n", name);
        g_fail = 1;
    }
}

/* Load a DLL and report error 126 (missing dependency) / 127 (missing
   procedure) / 193 (bad image) in words - that is exactly what the Windows
   startup dialog is telling us. */
static void probe_dll(const char* name) {
    HMODULE h = LoadLibraryA(name);
    char path[MAX_PATH * 3] = "";
    if (!h) {
        DWORD e = GetLastError();
        const char* why = "";
        if (e == 126) why = " (dependency not found)";
        else if (e == 127) why = " (procedure not found - api-set!)";
        else if (e == 193) why = " (not a valid Win32 image)";
        printf("  FAIL LoadLibrary(%-42s) error %lu%s\n", name,
               (unsigned long)e, why);
        g_fail = 1;
        return;
    }
    GetModuleFileNameA(h, path, sizeof path);
    printf("  ok   %-42s -> %s\n", name, path);
    if (!_stricmp(name, "onnxruntime.dll")) {
        FARPROC f = GetProcAddress(h, "OrtGetApiBase");
        printf(f ? "  ok   OrtGetApiBase export present\n"
                 : "  FAIL OrtGetApiBase export missing\n");
        if (!f) g_fail = 1;
    }
}

typedef struct {
    DWORD dwSize, dwFileFlags, dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} EXT;

/* resolved at run time so this exe never imports them statically */
typedef HRESULT (WINAPI *PFN_PathCch)(wchar_t*, size_t);
typedef HANDLE (WINAPI *PFN_CreateFile2)(LPCWSTR, DWORD, DWORD, DWORD,
                                         const void*);
typedef void (WINAPI *PFN_Precise)(LPFILETIME);
static PFN_PathCch pRemoveBackslash, pRemoveFileSpec;
static PFN_CreateFile2 pCreateFile2;
static PFN_Precise pPrecise;

static void resolve_real(void) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb = LoadLibraryW(L"kernelbase.dll");
    pPrecise = (PFN_Precise)GetProcAddress(k, "GetSystemTimePreciseAsFileTime");
    pCreateFile2 = (PFN_CreateFile2)GetProcAddress(k, "CreateFile2");
    pRemoveBackslash = (PFN_PathCch)GetProcAddress(k, "PathCchRemoveBackslash");
    if (!pRemoveBackslash && kb)
        pRemoveBackslash = (PFN_PathCch)GetProcAddress(kb, "PathCchRemoveBackslash");
    pRemoveFileSpec = (PFN_PathCch)GetProcAddress(k, "PathCchRemoveFileSpec");
    if (!pRemoveFileSpec && kb)
        pRemoveFileSpec = (PFN_PathCch)GetProcAddress(kb, "PathCchRemoveFileSpec");
    if (!pRemoveBackslash || !pRemoveFileSpec) {
        /* Win8.1+/Win10 expose them through the path contract */
        HMODULE p = LoadLibraryW(L"api-ms-win-core-path-l1-1-0.dll");
        if (p) {
            if (!pRemoveBackslash)
                pRemoveBackslash = (PFN_PathCch)GetProcAddress(p, "PathCchRemoveBackslash");
            if (!pRemoveFileSpec)
                pRemoveFileSpec = (PFN_PathCch)GetProcAddress(p, "PathCchRemoveFileSpec");
        }
    }
}

static const wchar_t* SRC = L"README.md";
static const wchar_t* DST = L"output\\shim_copy.tmp";
static const wchar_t* NOPE = L"does_not_exist_xyz.dat";

static int compare_open(const char* what, LPCWSTR path, DWORD acc, DWORD share,
                        DWORD disp, int with_ext) {
    W7_CREATEFILE2_EXTENDED_PARAMETERS e;
    EXT r;
    HANDLE h;
    DWORD err1 = 0, err2 = 0;
    int ok;
    memset(&e, 0, sizeof e);
    e.dwSize = sizeof e;
    memset(&r, 0, sizeof r);
    r.dwSize = sizeof r;

    SetLastError(0);
    h = impl_CreateFile2(path, acc, share, disp, with_ext ? &e : NULL);
    err1 = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    SetLastError(0);
    h = pCreateFile2(path, acc, share, disp, with_ext ? &r : NULL);
    err2 = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    ok = (err1 == err2);
    printf("  %-44s ours err=%-5lu real err=%-5lu %s\n", what,
           (unsigned long)err1, (unsigned long)err2, ok ? "" : "  <-- MISMATCH");
    if (!ok) g_fail = 1;
    return ok;
}

static void compare_path(const wchar_t* what, HRESULT (*ours)(wchar_t*, size_t),
                         PFN_PathCch real, const wchar_t* input) {
    wchar_t a[128], b[128];
    HRESULT h1, h2;
    wcscpy(a, input);
    wcscpy(b, input);
    h1 = ours(a, 128);
    h2 = real(b, 128);
    printf("  %-44ls ours=\"%ls\" hr=%08lx | real=\"%ls\" hr=%08lx%s\n",
           (input[0] ? input : L"\"\""), a, (unsigned long)h1, b,
           (unsigned long)h2, wcscmp(a, b) || h1 != h2 ? "  <-- MISMATCH" : "");
    if (wcscmp(a, b) || h1 != h2) g_fail = 1;
    (void)what;
}

int main(void) {
    FILETIME ft1, ft2;
    unsigned long long t1, t2;

    printf("== w7shim self-test / Windows 7 environment report ==\n\n");

    printf("[1] files that must sit next to the exes:\n");
    probe_file(L"farsi-tts.exe");
    probe_file(L"farsi-tts-server.exe");
    probe_file(L"farsi-tts-gui.exe");
    probe_file(L"onnxruntime.dll");
    probe_file(L"w7shim.dll");
    probe_file(L"msvcp140.dll");
    probe_file(L"vcruntime140.dll");
    probe_file(L"vcruntime140_1.dll");
    probe_file(L"api-ms-win-core-localization-l1-2-0.dll");

    printf("\n[2] LoadLibrary probes (first FAIL = the module behind the dialog):\n");
    probe_dll("kernel32.dll");
    probe_dll("ucrtbase.dll");
    probe_dll("api-ms-win-crt-runtime-l1-1-0.dll");
    probe_dll("MSVCP140.dll");
    probe_dll("VCRUNTIME140.dll");
    probe_dll("VCRUNTIME140_1.dll");
    probe_dll("w7shim.dll");
    probe_dll("api-ms-win-core-localization-l1-2-0.dll");
    probe_dll("onnxruntime.dll");

    printf("\n[3] GetACP (the reported missing procedure):\n");
    printf("      kernel32 : %s\n",
           GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetACP")
               ? "exported" : "NOT exported");
    {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        printf("      kernelbase: %s%s\n",
               (kb && GetProcAddress(kb, "GetACP")) ? "exported" : "NOT exported",
               kb ? "" : " (not loaded)");
    }

    printf("\n[4] Windows-8+ implementations, ours vs this OS:\n");
    printf("  sizeof(CREATEFILE2_EXTENDED_PARAMETERS)=%u (expect 32)\n",
           (unsigned)sizeof(W7_CREATEFILE2_EXTENDED_PARAMETERS));
    CHECK(sizeof(W7_CREATEFILE2_EXTENDED_PARAMETERS) == 32, "struct layout");

    resolve_real();
    if (pCreateFile2) {
        printf("  CreateFile2:\n");
        compare_open("read existing, no ext", SRC, GENERIC_READ, FILE_SHARE_READ,
                     OPEN_EXISTING, 0);
        compare_open("read existing, ext", SRC, GENERIC_READ, FILE_SHARE_READ,
                     OPEN_EXISTING, 1);
        compare_open("read missing, no ext", NOPE, GENERIC_READ, FILE_SHARE_READ,
                     OPEN_EXISTING, 0);
        compare_open("create new", DST, GENERIC_WRITE, 0, CREATE_ALWAYS, 0);
        compare_open("create new, ext", DST, GENERIC_WRITE, 0, CREATE_ALWAYS, 1);
        compare_open("attr query (FILE_READ_ATTRIBUTES)", SRC,
                     FILE_READ_ATTRIBUTES,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     OPEN_EXISTING, 0);
        DeleteFileW(DST);
    } else {
        printf("  CreateFile2: not provided by this OS (Windows 7) - "
               "our implementation is used, comparison skipped\n");
    }

    if (pPrecise) {
        impl_GetSystemTimePreciseAsFileTime(&ft1);
        pPrecise(&ft2);
        memcpy(&t1, &ft1, 8);
        memcpy(&t2, &ft2, 8);
        printf("  GetSystemTimePreciseAsFileTime: ours=%llu real=%llu\n", t1, t2);
        CHECK(t1 != 0 && (t1 > t2 ? t1 - t2 : t2 - t1) < 200000ULL,
              "within 20 ms of the real clock");
    } else {
        printf("  GetSystemTimePreciseAsFileTime: not provided by this OS - "
               "our implementation is used, comparison skipped\n");
    }

    if (pRemoveBackslash && pRemoveFileSpec) {
        printf("  PathCchRemoveBackslash:\n");
        compare_path(L"trailing", impl_PathCchRemoveBackslash, pRemoveBackslash,
                     L"C:\\dir\\sub\\");
        compare_path(L"drive root", impl_PathCchRemoveBackslash, pRemoveBackslash,
                     L"C:\\");
        compare_path(L"relative", impl_PathCchRemoveBackslash, pRemoveBackslash,
                     L"relative\\");
        compare_path(L"no trailing", impl_PathCchRemoveBackslash, pRemoveBackslash,
                     L"no\\trailing");
        compare_path(L"unc root", impl_PathCchRemoveBackslash, pRemoveBackslash,
                     L"\\\\server\\share\\");
        printf("  PathCchRemoveFileSpec:\n");
        compare_path(L"file path", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L"C:\\dir\\sub\\file.txt");
        compare_path(L"relative file", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L".\\model\\onnx\\x.onnx");
        compare_path(L"bare name", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L"justname");
        compare_path(L"trailing sep", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L"C:\\dir\\sub\\");
        compare_path(L"drive root", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L"C:\\");
        compare_path(L"drive relative", impl_PathCchRemoveFileSpec, pRemoveFileSpec,
                     L"C:file.txt");
        compare_path(L"empty", impl_PathCchRemoveFileSpec, pRemoveFileSpec, L"");
    } else {
        printf("  PathCch*: not provided by this OS - our implementations are "
               "used, comparison skipped\n");
    }

    printf("\n");
    printf(g_fail ? "RESULT: FAIL (send this whole output)\n"
                  : "RESULT: PASS\n");
    return g_fail;
}
