/* w7shim.dll - lets the official (Windows 8+) onnxruntime.dll load on Windows 7.
 *
 * scripts/patch_ort_win7.py rewires every `api-ms-win-*` import descriptor of
 * onnxruntime.dll to this DLL.  For every affected function we export a
 * trampoline (win7shim_gen.S) whose target is looked up lazily, on first use,
 * from the DLLs that really exist on Windows 7:
 *
 *     core contracts   kernel32 -> kernelbase -> ntdll
 *     crt  contracts   ucrtbase -> msvcrt
 *     rtlsupport       ntdll
 *     eventing         advapi32
 *
 * and the four Windows-8-only functions are implemented here.  Because
 * nothing is resolved during DllMain, a symbol that happens to be missing on
 * some machine can never fail the process startup again: the trampoline falls
 * back to a stub that logs the name instead of crashing the loader.
 *
 * Build: generated win7shim_gen.S / win7shim_names.inc / win7shim.def
 * (scripts/patch_ort_win7.py) are compiled together with this file.
 */
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "win7shim_names.inc"  /* GEN[], GEN_N */

/* defined in the generated win7shim_gen.S */
extern unsigned long long win7shim_slots[];

/* ------------------------------------------------------------------ */
/* Windows-8+ implementations                                          */
/* ------------------------------------------------------------------ */

static void WINAPI impl_GetSystemTimePreciseAsFileTime(LPFILETIME t) {
    /* Win8+ high-resolution variant; the Win7 file time (100 ns ticks from
       1601) is the same structure - just less precise. */
    GetSystemTimeAsFileTime(t);
}

typedef struct {
    DWORD dwSize;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} W7_CREATEFILE2_EXTENDED_PARAMETERS;

static HANDLE WINAPI impl_CreateFile2(LPCWSTR name, DWORD access, DWORD share,
                                      DWORD disposition,
                                      const W7_CREATEFILE2_EXTENDED_PARAMETERS* p) {
    /* NB: CreateFile2 really takes *five* parameters - dwFlagsAndAttributes
       and lpSecurityAttributes only exist inside the extended struct (the
       SDK header, the Windows 7-incompatible caller's disassembly and MinGW
       all agree).  Passing the 7-argument shape of CreateFileW here used to
       pick up garbage and fail every file open. */
    DWORD fa = FILE_ATTRIBUTE_NORMAL;
    LPSECURITY_ATTRIBUTES sa = NULL;
    HANDLE tpl = NULL;
    if (p && p->dwSize >= 4) {
        if (p->dwFileFlags) fa = p->dwFileFlags;
        if (p->dwSize >= 24) sa = p->lpSecurityAttributes;
        if (p->dwSize >= 32) tpl = p->hTemplateFile;
    }
    return CreateFileW(name, access, share, sa, disposition, fa, tpl);
}

#define W7_E_INVALIDARG            ((HRESULT)0x80070057L)
#define W7_STRSAFE_E_INSUFFICIENT  ((HRESULT)0x8007007AL)

static size_t w7_len(const wchar_t* s, size_t n) {
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

static int w7_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static HRESULT WINAPI impl_PathCchRemoveBackslash(wchar_t* path, size_t cch) {
    size_t len;
    if (!path || cch == 0) return W7_E_INVALIDARG;
    len = w7_len(path, cch);
    if (len == cch) return W7_STRSAFE_E_INSUFFICIENT;
    if (len > 0 && w7_sep(path[len - 1])) {
        /* roots are left alone: "\", "\\", "C:\" */
        if (len == 1) return S_FALSE;
        if (len == 2 && path[0] == L'\\') return S_FALSE;
        if (len == 3 && path[1] == L':') return S_FALSE;
        path[len - 1] = L'\0';
        return S_OK;
    }
    return S_FALSE;   /* nothing to remove - the real function does this too */
}

static HRESULT WINAPI impl_PathCchRemoveFileSpec(wchar_t* path, size_t cch) {
    size_t len, i, s, newlen;
    if (!path || cch == 0) return W7_E_INVALIDARG;
    len = w7_len(path, cch);
    if (len == cch) return W7_STRSAFE_E_INSUFFICIENT;
    if (len == 0) return S_FALSE;

    i = len;
    while (i > 0 && !w7_sep(path[i - 1])) i--;
    if (i > 0) {
        s = i - 1;                                     /* index of the separator */
        if (s == 2 && path[1] == L':') return S_FALSE; /* "C:\" - nothing to do */
        newlen = s;                                    /* drop sep and file name */
    } else {
        /* no separator at all: keep a drive prefix ("C:file.txt" -> "C:"),
           otherwise empty the string ("justname" -> "") */
        newlen = (len >= 2 && path[1] == L':') ? 2 : 0;
    }
    if (newlen == len) return S_FALSE;                 /* unchanged */
    path[newlen] = L'\0';
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* Lazy resolution                                                     */
/* ------------------------------------------------------------------ */

static const wchar_t* CHAIN[5][3] = {
    { L"kernel32.dll", L"kernelbase.dll", L"ntdll.dll" },  /* HOST_K32    */
    { L"ucrtbase.dll", L"msvcrt.dll",      NULL        },  /* HOST_CRT    */
    { L"ntdll.dll",    NULL,               NULL        },  /* HOST_NTDLL  */
    { L"advapi32.dll", NULL,               NULL        },  /* HOST_ADVAPI */
    { NULL,            NULL,               NULL        },  /* HOST_CUSTOM */
};

static HMODULE g_h[5][3];

static void* custom_impl(const char* name) {
    if (!strcmp(name, "GetSystemTimePreciseAsFileTime"))
        return (void*)impl_GetSystemTimePreciseAsFileTime;
    if (!strcmp(name, "CreateFile2"))
        return (void*)impl_CreateFile2;
    if (!strcmp(name, "PathCchRemoveBackslash"))
        return (void*)impl_PathCchRemoveBackslash;
    if (!strcmp(name, "PathCchRemoveFileSpec"))
        return (void*)impl_PathCchRemoveFileSpec;
    return NULL;
}

static unsigned long long missing_impl(void) {
    static int once;
    if (!once) {
        once = 1;
        OutputDebugStringA("w7shim: an onnxruntime import could not be resolved "
                           "on this system\n");
    }
    return 0;
}

static void* resolve(const char* name, unsigned char host) {
    void* p = NULL;
    int j;
    /* W7SHIM_FORCE_OURS[=name,name] forces our own implementations even when
       the OS has the function - used by the test harness to exercise the
       Windows-7 code paths on a modern OS (and on Wine). */
    int force_ours = 0;
    {
        char list[256];
        DWORD n = GetEnvironmentVariableA("W7SHIM_FORCE_OURS", list, sizeof list);
        if (n > 0 && n < sizeof list) {
            if (!strcmp(list, "ALL") || strstr(list, name)) force_ours = 1;
        }
    }
    if (force_ours) p = custom_impl(name);
    /* 1. the real system implementation, if this OS has one (Windows 10 and
     *    Wine do have the Win8+ functions; Windows 7 does not) */
    if (!p) for (j = 0; host < 5 && j < 3 && CHAIN[host][j]; j++) {
        HMODULE h = g_h[host][j];
        FARPROC f;
        if (!h) {
            h = GetModuleHandleW(CHAIN[host][j]);
            if (!h) h = LoadLibraryW(CHAIN[host][j]);
            g_h[host][j] = h;
        }
        if (!h) continue;
        f = GetProcAddress(h, name);
        if (f) { p = (void*)f; break; }
    }
    /* 2. our own implementation of the Windows-8+ functions */
    if (!p) p = custom_impl(name);
    {
        char line[192];
        const char* mod = p ? (force_ours && custom_impl(name) == p ? "ours" : "ok")
                            : "MISSING";
        if (GetEnvironmentVariableA("W7SHIM_LOG", line, sizeof line) > 0) {
            FILE* f = fopen("w7shim.log", "a");
            if (f) {
                snprintf(line, sizeof line, "%-36s host=%u %s\n", name, host, mod);
                fputs(line, f);
                fclose(f);
            }
        }
    }
    return p;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        unsigned i;
        DisableThreadLibraryCalls(inst);
        for (i = 0; i < GEN_N; i++) {
            void* p = resolve(GEN[i].name, GEN[i].host);
            win7shim_slots[i] = (unsigned long long)(p ? (size_t)p
                                                       : (size_t)missing_impl);
        }
    }
    return TRUE;
}
