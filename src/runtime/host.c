/*
 * Rise of Nations: Extended Edition - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header), copied from tw (Tiberium Wars). What is
 * here is only what is specific to this game: where the image goes, the
 * guest's identity and command line, and the fault report. docs/bringup.md has the reasoning.
 *
 * Linked at /BASE:0x20000000 (CMakeLists.txt) so 0x00400000..0x00FB3000 is
 * free when main() maps the image. riseofnations.exe imports little of Win32
 * directly and finds the rest with GetProcAddress, so that is shimmed too:
 * a name in the shim table answers with the shim.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"

extern const uint32_t ron_entry_va;    /* recomp_dispatch.c */

#define RON_IMAGE_BASE 0x00400000u
#define RON_EXE_STAMP 0x6674863Fu     /* riseofnations.exe's PE timestamp (2024-06-20): the build lifted (run_lift.py) */
#define RON_EXE "riseofnations.exe"

static DWORD g_watchdog_s;
static int   g_headless;

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ---- headless ------------------------------------------------------------
 * Nothing reaches the screen (REPO_RULES section 13): message boxes print and
 * answer, and a top-level window is layered at alpha 0, click-through and
 * never activated, as tiberiansun-recomp's headless mode does. */
static uint32_t mb_answer(uint32_t type) {
    uint32_t buttons = type & MB_TYPEMASK;
    return (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) ? IDNO : IDOK;
}

static void shim_MessageBoxA(void) {
    g_eax = mb_answer(ARG(3));
    fprintf(stderr, "[messagebox] type 0x%X -> %u: %s: %s\n", ARG(3), g_eax, gstr(ARG(2)), gstr(ARG(1)));
    g_esp += 4 + 4 * 4;
}

static void shim_MessageBoxW(void) {
    g_eax = mb_answer(ARG(3));
    fprintf(stderr, "[messagebox] type 0x%X -> %u: %ls: %ls\n", ARG(3), g_eax,
            ARG(2) ? (const wchar_t*)(uintptr_t)ARG(2) : L"(null)",
            ARG(1) ? (const wchar_t*)(uintptr_t)ARG(1) : L"(null)");
    g_esp += 4 + 4 * 4;
}

#define HL_EXSTYLE (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)

static void shim_CreateWindowExA(void) {
    uint32_t style = ARG(3);
    HWND parent = (HWND)(uintptr_t)ARG(8);
    HWND h = CreateWindowExA(parent ? ARG(0) : ARG(0) | HL_EXSTYLE, (LPCSTR)(uintptr_t)ARG(1),
                             (LPCSTR)(uintptr_t)ARG(2), style, (int)ARG(4), (int)ARG(5), (int)ARG(6),
                             (int)ARG(7), parent, (HMENU)(uintptr_t)ARG(9),
                             (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    if (h && !parent) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    fprintf(stderr, "[headless] CreateWindowExA(%s) style 0x%08X -> %p\n", gstr(ARG(2)), style, (void*)h);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_CreateWindowExW(void) {
    uint32_t style = ARG(3);
    HWND parent = (HWND)(uintptr_t)ARG(8);
    HWND h = CreateWindowExW(parent ? ARG(0) : ARG(0) | HL_EXSTYLE, (LPCWSTR)(uintptr_t)ARG(1),
                             (LPCWSTR)(uintptr_t)ARG(2), style, (int)ARG(4), (int)ARG(5), (int)ARG(6),
                             (int)ARG(7), parent, (HMENU)(uintptr_t)ARG(9),
                             (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    if (h && !parent) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    fprintf(stderr, "[headless] CreateWindowExW(%ls) style 0x%08X -> %p\n",
            ARG(2) ? (const wchar_t*)(uintptr_t)ARG(2) : L"", style, (void*)h);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

/* ---- the guest's identity ------------------------------------------------
 * The guest is <install>\riseofnations.exe, not this host. Its hInstance
 * comes from GetModuleHandle(NULL), and it finds its files relative to
 * GetModuleFileName and the working directory (the install). */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[2 * MAX_PATH + 64];
static wchar_t g_guest_exe_w[MAX_PATH], g_guest_cmdline_w[2 * MAX_PATH + 64];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0)) : RON_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

/* The game looks up its own exports at startup (sub_00402E21): from the
 * process exe (GetModuleHandleExA with no name) and from the module holding
 * its own code (FROM_ADDRESS 0x00402E21), then GetProcAddress on each. Both
 * mean the guest image, which Windows does not know as a module, so they are
 * answered here from its export table (docs/bringup.md, 3). */
static void shim_GetModuleHandleExA(void) {
    uint32_t flags = ARG(0), name = ARG(1), *out = (uint32_t*)(uintptr_t)ARG(2);
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) ? native32_in_guest(name) : !name) {
        *out = RON_IMAGE_BASE;
        g_eax = 1;
    } else {
        g_eax = GetModuleHandleExA(flags, (LPCSTR)(uintptr_t)name, (HMODULE*)out);
    }
    g_esp += 4 + 3 * 4;
}

static native32_shim_t* g_active; static int g_nactive;   /* the table bound */
static void shim_GetProcAddress(void) {
    uint32_t h = ARG(0), name = ARG(1);
    g_eax = 0;
    if (getenv("RON_LOG_GPA") && name > 0xFFFF) fprintf(stderr, "[gpa] %s\n", (const char*)(uintptr_t)name);
    if (name > 0xFFFF && h != RON_IMAGE_BASE)          /* by name, not ordinal */
        for (int i = 0; i < g_nactive; i++)
            if (!strcmp(g_active[i].name, (const char*)(uintptr_t)name)) { g_eax = g_active[i].va; break; }
    if (!g_eax)
        g_eax = h == RON_IMAGE_BASE ? native32_export(h, (const char*)(uintptr_t)name)
                                    : (uint32_t)(uintptr_t)GetProcAddress((HMODULE)(uintptr_t)h, (LPCSTR)(uintptr_t)name);
    g_esp += 4 + 2 * 4;
}

static void shim_FreeLibrary(void) {
    g_eax = ARG(0) == RON_IMAGE_BASE ? 1 : FreeLibrary((HMODULE)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

/* The game's own exits (a /GS failure, a fatal error) end in TerminateProcess:
 * say so, and flush --calltrace, whose newest entries are the ones that matter. */
static void shim_TerminateProcess(void) {
    fprintf(stderr, "\n[exit] TerminateProcess(0x%X) from sub_%08X\n", ARG(1), g_cur_func);
    native32_dump_icalls(8);
    recomp_trace_flush();
    TerminateProcess((HANDLE)(uintptr_t)ARG(0), ARG(1));
    g_eax = 1;
    g_esp += 4 + 2 * 4;
}

static uint32_t copy_out(void* out, const void* src, uint32_t n, uint32_t size, int wide) {
    if (!size) return n;
    uint32_t k = n < size ? n : size - 1;
    memcpy(out, src, (size_t)k << wide);
    if (wide) ((wchar_t*)out)[k] = 0; else ((char*)out)[k] = 0;
    return k;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0);
    if (h == 0 || h == RON_IMAGE_BASE)
        g_eax = copy_out((void*)(uintptr_t)ARG(1), g_guest_exe, (uint32_t)strlen(g_guest_exe), ARG(2), 0);
    else
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, (char*)(uintptr_t)ARG(1), ARG(2));
    g_esp += 4 + 3 * 4;
}

static void shim_GetModuleFileNameW(void) {
    uint32_t h = ARG(0);
    if (h == 0 || h == RON_IMAGE_BASE)
        g_eax = copy_out((void*)(uintptr_t)ARG(1), g_guest_exe_w, (uint32_t)wcslen(g_guest_exe_w), ARG(2), 1);
    else
        g_eax = GetModuleFileNameW((HMODULE)(uintptr_t)h, (wchar_t*)(uintptr_t)ARG(1), ARG(2));
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline; g_esp += 4; }
static void shim_GetCommandLineW(void) { g_eax = (uint32_t)(uintptr_t)g_guest_cmdline_w; g_esp += 4; }
/* The UCRT's WinMain startup (cdecl): the command line after the program name. */
static void shim_get_narrow_winmain_command_line(void) {
    const char* p = g_guest_cmdline;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    g_eax = (uint32_t)(uintptr_t)p;
    g_esp += 4;
}

/* The native DLLs the game loads ask Windows, not the guest, for the exe's
 * path, and something among them changes into its folder, which for this host
 * is the build dir: msxml then can't find data\*.xml ("translated_string.xml
 * not loaded. File missing?"). So the loader's own record of the main module
 * names the install's riseofnations.exe, as it does for the real game. */
/* Every game popup goes through MessageBoxW, which the game resolves at run
 * time (not through its imports). Log each one with the lifted function that
 * raised it, then show it as usual through MessageBoxExW. */
static int WINAPI log_MessageBoxW(HWND w, LPCWSTR text, LPCWSTR cap, UINT type) {
    fprintf(stderr, "[msgbox] from sub_%08X: %ls | %ls\n", g_cur_func, cap ? cap : L"", text ? text : L"");
    fflush(stderr);
    return MessageBoxExW(w, text, cap, type, 0);
}

static void hook_message_box(void) {
    uint8_t* f = (uint8_t*)GetProcAddress(LoadLibraryA("user32.dll"), "MessageBoxW");
    DWORD was;
    if (!f || !VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &was)) return;
    f[0] = 0xE9;                                    /* jmp log_MessageBoxW */
    *(int32_t*)(f + 1) = (int32_t)((uint8_t*)log_MessageBoxW - (f + 5));
    VirtualProtect(f, 5, was, &was);
}

/* d3dgl.dll sets fullscreen modes through ChangeDisplaySettingsW; log each. */
static LONG WINAPI log_ChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags) {
    if (dm) fprintf(stderr, "[cds] from sub_%08X: %lux%lu %lubpp %luHz flags=%lx fields=%lx\n", g_cur_func,
                    dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel, dm->dmDisplayFrequency, flags, dm->dmFields);
    else fprintf(stderr, "[cds] from sub_%08X: reset flags=%lx\n", g_cur_func, flags);
    LONG r = ChangeDisplaySettingsExW(NULL, dm, NULL, flags, NULL);
    fprintf(stderr, "[cds]   -> %ld\n", r); fflush(stderr);
    return r;
}

static void hook_change_display(void) {
    uint8_t* f = (uint8_t*)GetProcAddress(LoadLibraryA("user32.dll"), "ChangeDisplaySettingsW");
    DWORD was;
    if (!f || !VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &was)) return;
    f[0] = 0xE9;
    *(int32_t*)(f + 1) = (int32_t)((uint8_t*)log_ChangeDisplaySettingsW - (f + 5));
    VirtualProtect(f, 5, was, &was);
}

static void claim_guest_path(void) {
    PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
    LDR_DATA_TABLE_ENTRY* me = CONTAINING_RECORD(peb->Ldr->InMemoryOrderModuleList.Flink,
                                                 LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
    UNICODE_STRING* names[2] = { &me->FullDllName, &peb->ProcessParameters->ImagePathName };
    for (int i = 0; i < 2; i++) {
        names[i]->Buffer = g_guest_exe_w;   /* static: lives as long as the process */
        names[i]->Length = (USHORT)(wcslen(g_guest_exe_w) * 2);
        names[i]->MaximumLength = (USHORT)sizeof g_guest_exe_w;
    }
}

/* The renderer (d3dgl.dll, native) refuses Remote Desktop: GetSystemMetrics
 * (SM_REMOTESESSION) set, and the game says "can not be run in a remote
 * desktop" and quits (8008), a rule from when RDP had no 3D. It runs fine
 * there now, so user32's GetSystemMetrics is hooked for the whole process to
 * answer 0 for it. The hook replaces the hot-patch prologue (mov edi, edi;
 * push ebp; mov ebp, esp: five bytes), which the trampoline replays. */
static int (WINAPI* real_GetSystemMetrics)(int);
static int WINAPI hook_GetSystemMetrics(int i) {
    return i == SM_REMOTESESSION ? 0 : real_GetSystemMetrics(i);
}

/* Jump from dll!name to `to`; returns a trampoline that calls the original. */
static void* hook_export(const char* dll, const char* name, void* to) {
    static const uint8_t hotpatch[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    uint8_t* f = (uint8_t*)GetProcAddress(LoadLibraryA(dll), name);
    DWORD was;
    /* Whole instructions to move, none relative to where they sit: the
     * hot-patch prologue, or the SEH one (push imm8; push imm32) that Windows
     * 11's WOW64 user32 has. */
    int n = !f ? 0 : !memcmp(f, hotpatch, 5) ? 5 : f[0] == 0x6A && f[2] == 0x68 ? 7 : 0;
    if (!n) {
        if (f) fprintf(stderr, "[hook] %s starts %02X %02X %02X %02X %02X: not hooked\n",
                       name, f[0], f[1], f[2], f[3], f[4]);
        return NULL;
    }
    uint8_t* t = (uint8_t*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(t, f, n);
    t[n] = 0xE9;                                    /* jmp f + n */
    *(int32_t*)(t + n + 1) = (int32_t)((f + n) - (t + n + 5));
    if (!VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &was)) return NULL;
    f[0] = 0xE9;
    *(int32_t*)(f + 1) = (int32_t)((uint8_t*)to - (f + 5));
    VirtualProtect(f, 5, was, &was);
    return t;
}

static void hook_remote_session(void) {
    real_GetSystemMetrics = (int (WINAPI*)(int))hook_export("user32.dll", "GetSystemMetrics", hook_GetSystemMetrics);
}

/* d3dgl compiles its HLSL at run time; log every compile that fails. */
typedef struct { void** vt; } Blob;   /* ID3DBlob: GetBufferPointer is slot 3 */
static long (WINAPI* real_D3DCompileFromFile)(const wchar_t*, const void*, void*, const char*, const char*,
                                              unsigned, unsigned, Blob**, Blob**);
static long (WINAPI* real_D3DCompile)(const void*, size_t, const char*, const void*, void*, const char*,
                                      const char*, unsigned, unsigned, Blob**, Blob**);
static void log_compile(long hr, const char* what, const char* entry, const char* target, Blob** err) {
    const char* msg = err && *err ? ((const char* (WINAPI*)(Blob*))(*err)->vt[3])(*err) : "";
    if (getenv("RON_LOG_SHADERS") || hr < 0)
        fprintf(stderr, "[shader] %s %s %s -> %08lX %s\n", what, entry ? entry : "", target ? target : "", hr, msg);
    fflush(stderr);
}
static long WINAPI log_D3DCompileFromFile(const wchar_t* file, const void* defs, void* inc, const char* entry,
                                          const char* target, unsigned f1, unsigned f2, Blob** code, Blob** err) {
    long hr = real_D3DCompileFromFile(file, defs, inc, entry, target, f1, f2, code, err);
    char n[MAX_PATH]; snprintf(n, sizeof n, "%ls", file);
    log_compile(hr, n, entry, target, err);
    return hr;
}
static long WINAPI log_D3DCompile(const void* src, size_t len, const char* name, const void* defs, void* inc,
                                  const char* entry, const char* target, unsigned f1, unsigned f2,
                                  Blob** code, Blob** err) {
    long hr = real_D3DCompile(src, len, name, defs, inc, entry, target, f1, f2, code, err);
    log_compile(hr, name ? name : "(memory)", entry, target, err);
    return hr;
}
/* RON_LOG_CBUF: dump the constant buffers d3dgl fills (camera, lights,
 * tone mapping), a sample per buffer size, to check what the game computes. */
static void* g_map_ptr[64]; static void* g_map_res[64]; static int g_nmap;
static unsigned g_cb_seen[8192];
static long (WINAPI* real_Map)(void*, void*, unsigned, int, unsigned, void*);
static void (WINAPI* real_Unmap)(void*, void*, unsigned);
static void (WINAPI* real_UpdateSubresource)(void*, void*, unsigned, const void*, const void*, unsigned, unsigned);
static void dump_cb(void* res, const float* p, const char* how) {
    struct { unsigned w, usage, bind, cpu, misc, stride; } d;
    int type; ((void (WINAPI*)(void*, int*))(*(void***)res)[7])(res, &type);
    if (type != 1) return;                          /* buffers only */
    ((void (WINAPI*)(void*, void*))(*(void***)res)[10])(res, &d);
    if (!(d.bind & 4) || d.w >= 8192 || g_cb_seen[d.w]++ % 600) return;
    fprintf(stderr, "[cb] %s size=%u n=%u:", how, d.w, g_cb_seen[d.w]);
    for (unsigned i = 0; i < d.w / 4 && i < 48; i++) fprintf(stderr, "%s%g", i % 4 ? " " : " | ", p[i]);
    fprintf(stderr, "\n"); fflush(stderr);
}
static long WINAPI log_Map(void* c, void* res, unsigned sub, int type, unsigned fl, void* out) {
    long hr = real_Map(c, res, sub, type, fl, out);
    static int calls; if (calls++ < 3) fprintf(stderr, "[d3d11] Map type=%d hr=%08lX\n", type, hr);
    if (hr >= 0 && g_nmap < 64) { g_map_res[g_nmap] = res; g_map_ptr[g_nmap++] = *(void**)out; }
    return hr;
}
static void WINAPI log_Unmap(void* c, void* res, unsigned sub) {
    for (int i = g_nmap - 1; i >= 0; i--) if (g_map_res[i] == res) {
        dump_cb(res, (const float*)g_map_ptr[i], "map");
        g_map_res[i] = g_map_res[--g_nmap]; g_map_ptr[i] = g_map_ptr[g_nmap]; break;
    }
    real_Unmap(c, res, sub);
}
static void WINAPI log_UpdateSubresource(void* c, void* res, unsigned sub, const void* box, const void* src,
                                         unsigned rp, unsigned dp) {
    if (src) dump_cb(res, (const float*)src, "upd");
    real_UpdateSubresource(c, res, sub, box, src, rp, dp);
}
static long (WINAPI* real_D3D11CreateDevice)(void*, int, void*, unsigned, const void*, unsigned, unsigned,
                                             void**, void*, void**);
static long WINAPI log_D3D11CreateDevice(void* a, int t, void* sw, unsigned fl, const void* lv, unsigned n,
                                         unsigned sdk, void** dev, void* got, void** ctx) {
    long hr = real_D3D11CreateDevice(a, t, sw, fl, lv, n, sdk, dev, got, ctx);
    void* c = ctx ? *ctx : NULL;
    if (hr >= 0 && !c && dev && *dev) {             /* ID3D11Device::GetImmediateContext */
        ((void (WINAPI*)(void*, void**))(*(void***)*dev)[40])(*dev, &c);
        if (c) ((unsigned long (WINAPI*)(void*))(*(void***)c)[2])(c);
    }
    fprintf(stderr, "[d3d11] CreateDevice hr=%08lX ctx=%p\n", hr, c);
    static const GUID ctx_iids[5] = {
        {0xc0bfa96c,0xe089,0x44fb,{0x8e,0xaf,0x26,0xf8,0x79,0x61,0x90,0xda}},
        {0xbb2c6faa,0xb5fb,0x4082,{0x8e,0x6b,0x38,0x8b,0x8c,0xfa,0x90,0xe1}},
        {0x420d5b32,0xb90c,0x4da4,{0xbe,0xf0,0x35,0x9f,0x6a,0x24,0xa8,0x3a}},
        {0xb4e3c01d,0xe79e,0x4637,{0x91,0xb2,0x51,0x0e,0x9f,0x4c,0x9b,0x8f}},
        {0x917600da,0xf58c,0x4c33,{0x98,0xd8,0x3e,0x15,0xb3,0x90,0xfa,0x24}} };
    for (int q = 0; hr >= 0 && c && q < 5; q++) {
        void* ci = NULL;
        ((long (WINAPI*)(void*, const GUID*, void**))(*(void***)c)[0])(c, &ctx_iids[q], &ci);
        if (!ci) continue;
        ((unsigned long (WINAPI*)(void*))(*(void***)ci)[2])(ci);
        if ((*(void***)ci)[14] == (void*)log_Map) continue;
        void** vt = *(void***)ci; DWORD was;
        if (real_Map && (vt[14] != (void*)real_Map || vt[15] != (void*)real_Unmap)) {
            fprintf(stderr, "[d3d11] iface %d has other Map %p: skipped\n", q, vt[14]); continue;
        }
        VirtualProtect(vt, 64 * 4, PAGE_READWRITE, &was);
        *(void**)&real_Map = vt[14]; vt[14] = log_Map;
        *(void**)&real_Unmap = vt[15]; vt[15] = log_Unmap;
        *(void**)&real_UpdateSubresource = vt[48]; vt[48] = log_UpdateSubresource;
        VirtualProtect(vt, 64 * 4, was, &was);
        fprintf(stderr, "[d3d11] patched vt=%p map now %p (log %p)\n", (void*)vt, vt[14], (void*)log_Map);
    }
    return hr;
}

static void hook_shader_compiler(void) {
    if (getenv("RON_LOG_CBUF"))
        *(void**)&real_D3D11CreateDevice = hook_export("d3d11.dll", "D3D11CreateDevice", log_D3D11CreateDevice);
    *(void**)&real_D3DCompileFromFile = hook_export("D3DCOMPILER_47.dll", "D3DCompileFromFile", log_D3DCompileFromFile);
    *(void**)&real_D3DCompile = hook_export("D3DCOMPILER_47.dll", "D3DCompile", log_D3DCompile);
}

#define GUEST_SHIMS \
    { "GetModuleHandleA", shim_GetModuleHandleA }, \
    { "GetModuleHandleExA", shim_GetModuleHandleExA }, \
    { "GetProcAddress", shim_GetProcAddress }, \
    { "FreeLibrary", shim_FreeLibrary }, \
    { "TerminateProcess", shim_TerminateProcess }, \
    { "GetModuleFileNameA", shim_GetModuleFileNameA }, \
    { "GetModuleFileNameW", shim_GetModuleFileNameW }, \
    { "GetCommandLineA", shim_GetCommandLineA }, \
    { "GetCommandLineW", shim_GetCommandLineW },     { "_get_narrow_winmain_command_line", shim_get_narrow_winmain_command_line }, \
    { "_CxxThrowException", native32_shim_CxxThrowException }, \
    { "?__ExceptionPtrRethrow@@YAXPBX@Z", native32_shim_ExceptionPtrRethrow }

/* A guest throw, or a rethrow of a task's stored exception, goes to eh32,
 * which finds the guest's own catch; the real ones walk the host's SEH chain,
 * find nothing, and abort (0xC0000409 on a cpprest worker). */
extern void native32_shim_CxxThrowException(void);
extern void native32_shim_ExceptionPtrRethrow(void);
static native32_shim_t g_shims[] = { GUEST_SHIMS };
static native32_shim_t g_headless_shims[] = {
    GUEST_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },
    { "MessageBoxW", shim_MessageBoxW },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "CreateWindowExW", shim_CreateWindowExW },
};

/* ---- diagnostics ---------------------------------------------------------
 * --probe VA (repeatable): report indirect calls to VA -- a virtual method or
 * callback -- with `this` and the first arguments, the first five calls.
 * RECOMP_ICALL asks this hook before the dispatch table. */
#define MAX_PROBES 8
static uint32_t g_probe[MAX_PROBES];
static int g_nprobe;
static volatile LONG g_probe_hits[MAX_PROBES];

recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_nprobe; i++)
        if (g_probe[i] == va && InterlockedIncrement(&g_probe_hits[i]) <= 5)
            fprintf(stderr, "[probe] sub_%08X from sub_%08X  ecx=%08X edx=%08X  args %08X %08X %08X %08X\n",
                    va, g_cur_func, g_ecx, g_edx, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8),
                    MEM32(g_esp + 12));
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* After native32's own handler, so callbacks are resolved first and only real
 * faults get here. Written with WriteFile from a static buffer, not stdio: a
 * fault while another thread holds stderr's lock otherwise reports nothing. */
static char g_crash_buf[16384];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u &&
        !(r->ExceptionCode == EXCEPTION_BREAKPOINT && getenv("RON_TRAP_EXIT"))) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    {
        HMODULE m = NULL;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)r->ExceptionAddress, &m) && GetModuleFileNameA(m, path, sizeof path))
            crash_emit("  in %s at +0x%lX\n", path, (unsigned long)((uintptr_t)r->ExceptionAddress - (uintptr_t)m));
        else
            crash_emit("  in no module (heap or generated code)\n");
    }
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    {   CONTEXT* c = ep->ContextRecord;   /* the faulting code's own, when it is native */
        crash_emit("  native: eax=%08lX ecx=%08lX edx=%08lX ebx=%08lX esp=%08lX ebp=%08lX esi=%08lX edi=%08lX\n",
                   c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esp, c->Ebp, c->Esi, c->Edi);
        crash_emit("  native stack:");
        for (int k = 0; k < 24; k++) crash_emit("%s%08X", k % 8 ? " " : "\n    ", ((uint32_t*)(uintptr_t)c->Esp)[k]);
        crash_emit("\n");
    }
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 40 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    crash_emit("guest stack at %08X:", g_esp);
    for (int k = 0; k < 64; k++) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)(g_esp + 4 * k), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
        crash_emit("%s%08X", k % 8 ? " " : "\n  ", MEM32(g_esp + 4 * k));
    }
    crash_emit("\n");
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A process exit nobody reports: ExitProcess called by native code (the CRT's
 * exit, a DLL), which no shim sees. Windows runs this exe's TLS callbacks with
 * DLL_PROCESS_DETACH on the thread that called it, after the other threads are
 * gone, so its native stack still says who. */
static int g_entered;                 /* the guest's entry point has been called */
static void NTAPI exit_report(PVOID h, DWORD reason, PVOID r) {
    (void)h; (void)r;
    if (reason != DLL_PROCESS_DETACH || !g_entered) return;
    void* frames[24];
    USHORT n = CaptureStackBackTrace(0, 24, frames, NULL);
    g_crash_len = 0;
    crash_emit("\n[exit] process exiting on thread %lu, last lifted sub_%08X, last native call %s\n",
               GetCurrentThreadId(), g_cur_func, g_cur_import);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m))
            GetModuleFileNameA(m, path, sizeof path);
        crash_emit("  %p  %s+0x%lX\n", frames[i], strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path,
                   m ? (unsigned long)((uintptr_t)frames[i] - (uintptr_t)m) : 0ul);
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
}
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_ron_exit_report")
#pragma const_seg(".CRT$XLB")
const PIMAGE_TLS_CALLBACK ron_exit_report = exit_report;
#pragma const_seg()

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    native32_dump_icalls(8);
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* game = "game";
    const char* extra = "";             /* --gameargs "...": appended to the guest's command line */
    char exe_full[MAX_PATH], game_full[MAX_PATH], exe_dir[MAX_PATH];
    int run = 0;
    /* Unbuffered: the log is the evidence, and ExitProcess flushes nothing
     * with the C runtime linked in (/MT). */
    setvbuf(stderr, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--gameargs") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: ron [--run] [--headless] [--game INSTALL_DIR] [--gameargs \"...\"]\n"
                   "          [--watchdog S] [--probe VA] [--native-trace] [--callbacks]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    _snprintf(exe_full, sizeof exe_full - 1, "%s\\" RON_EXE, game_full);
    strcpy_s(g_guest_exe, sizeof g_guest_exe, exe_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s%s", g_guest_exe,
              *extra ? " " : "", extra);
    MultiByteToWideChar(CP_ACP, 0, g_guest_exe, -1, g_guest_exe_w, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, g_guest_cmdline, -1, g_guest_cmdline_w,
                        sizeof g_guest_cmdline_w / sizeof g_guest_cmdline_w[0]);
    /* steam_api.dll, d3dgl.dll and the Crossplay DLLs sit beside the game exe. */
    strcpy_s(exe_dir, sizeof exe_dir, exe_full);
    *strrchr(exe_dir, '\\') = 0;
    SetDllDirectoryA(exe_dir);

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    /* RON_TRAP_EXIT=1: int3 on ucrtbase!abort, so the fault report says who
     * aborts (0xC0000409 in SteamAPI_Init's neighbourhood). */
    if (getenv("RON_TRAP_EXIT")) {
        BYTE* f = (BYTE*)GetProcAddress(LoadLibraryA("ucrtbase.dll"), "abort");
        DWORD old;
        VirtualProtect(f, 1, PAGE_EXECUTE_READWRITE, &old);
        *f = 0xCC;
        VirtualProtect(f, 1, old, &old);
    }
    printf("Rise of Nations recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    {   /* The lifted C is one build's: another crashes in game. */
        uint8_t head[4096] = { 0 };
        FILE* f = fopen(exe_full, "rb");
        if (f) { fread(head, 1, sizeof head, f); fclose(f); }
        uint32_t nt = *(const uint32_t*)(head + 0x3C);
        uint32_t stamp = nt < sizeof head - 12 ? *(const uint32_t*)(head + nt + 8) : 0;
        if (stamp != RON_EXE_STAMP) {
            fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, "
                    "wanted 0x%08X). Lift and build from the Steam release (README, Getting Started).\n",
                    exe_full, stamp, RON_EXE_STAMP);
            return 1;
        }
    }
    uint32_t span = native32_map(exe_full, RON_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, RON_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", RON_EXE, RON_IMAGE_BASE, RON_IMAGE_BASE + span);
    native32_shim_t* shims = g_headless ? g_headless_shims : g_shims;
    int nshims = g_headless ? (int)(sizeof g_headless_shims / sizeof g_headless_shims[0])
                            : (int)(sizeof g_shims / sizeof g_shims[0]);
    g_active = shims; g_nactive = nshims;
    if (native32_bind(RON_IMAGE_BASE, shims, nshims)) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", ron_entry_va);
        return 0;
    }
    /* The game reads its data relative to the working directory. */
    claim_guest_path();
    hook_message_box();
    hook_change_display();
    hook_remote_session();
    hook_shader_compiler();
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", ron_entry_va);
    fflush(stdout);
    g_entered = 1;
    native32_call_guest(ron_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
