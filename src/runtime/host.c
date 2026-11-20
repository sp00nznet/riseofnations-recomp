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
#include <math.h>
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
static volatile long g_nmaps, g_nupds, g_ndraws, g_nupd1;   /* D3D11 calls seen (RON_LOG_CBUF) */

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
    fprintf(stderr, "[d3d11] Map %ld UpdateSubresource %ld UpdateSubresource1 %ld draws %ld\n", g_nmaps, g_nupds, g_nupd1, g_ndraws);
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
static void* hook_code(uint8_t* f, const char* name, void* to);
static void* hook_export(const char* dll, const char* name, void* to) {
    return hook_code((uint8_t*)GetProcAddress(LoadLibraryA(dll), name), name, to);
}
static void* hook_code(uint8_t* f, const char* name, void* to) {
    static const uint8_t hotpatch[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
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
static const char* where_is(uint32_t a);
/* RON_LOG_CBUF: dump the constant buffers d3dgl fills (camera, lights,
 * tone mapping), a sample per buffer size, to check what the game computes. */
static void* g_map_ptr[64]; static void* g_map_res[64]; static int g_nmap;
static unsigned g_cb_seen[8192];
/* Each context interface has its own method table; keep each one's originals. */
typedef long (WINAPI* map_fn)(void*, void*, unsigned, int, unsigned, void*);
typedef void (WINAPI* unmap_fn)(void*, void*, unsigned);
typedef void (WINAPI* upd_fn)(void*, void*, unsigned, const void*, const void*, unsigned, unsigned);
static struct { void** vt; map_fn map; unmap_fn unmap; upd_fn upd; } g_ctx_vt[32]; static int g_nctx_vt;
static int ctx_vt(void* c) {
    for (int i = g_nctx_vt - 1; i >= 0; i--) if (g_ctx_vt[i].vt == *(void***)c) return i;
    return g_nctx_vt - 1;   /* swapped since: any table's originals are the same code family */
}
static long WINAPI log_Map(void*, void*, unsigned, int, unsigned, void*);
static void WINAPI log_Unmap(void*, void*, unsigned);
static void WINAPI log_UpdateSubresource(void*, void*, unsigned, const void*, const void*, unsigned, unsigned);
static void* g_ctx;
/* d3d11 swaps the immediate context's method table after creation; patch
 * whichever one is current, as often as it changes. */
static DWORD WINAPI ctx_patch_loop(void* unused) {
    (void)unused;
    for (;; Sleep(50)) {
        void** vt = *(void***)g_ctx; DWORD was;
        if (vt[14] == (void*)log_Map || g_nctx_vt == 32) continue;
        g_ctx_vt[g_nctx_vt].vt = vt;
        g_ctx_vt[g_nctx_vt].map = (map_fn)vt[14];
        g_ctx_vt[g_nctx_vt].unmap = (unmap_fn)vt[15];
        g_ctx_vt[g_nctx_vt].upd = (upd_fn)vt[48];
        g_nctx_vt++;
        VirtualProtect(vt, 64 * 4, PAGE_READWRITE, &was);
        vt[14] = log_Map; vt[15] = log_Unmap; vt[48] = log_UpdateSubresource;
        VirtualProtect(vt, 64 * 4, was, &was);
        fprintf(stderr, "[d3d11] context table %p patched (#%d)\n", (void*)vt, g_nctx_vt);
    }
}
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
    long hr = g_ctx_vt[ctx_vt(c)].map(c, res, sub, type, fl, out);
    InterlockedIncrement(&g_nmaps);
    if (hr >= 0 && g_nmap < 64) { g_map_res[g_nmap] = res; g_map_ptr[g_nmap++] = *(void**)out; }
    return hr;
}
static void WINAPI log_Unmap(void* c, void* res, unsigned sub) {
    for (int i = g_nmap - 1; i >= 0; i--) if (g_map_res[i] == res) {
        dump_cb(res, (const float*)g_map_ptr[i], "map");
        g_map_res[i] = g_map_res[--g_nmap]; g_map_ptr[i] = g_map_ptr[g_nmap]; break;
    }
    g_ctx_vt[ctx_vt(c)].unmap(c, res, sub);
}
static void WINAPI log_UpdateSubresource(void* c, void* res, unsigned sub, const void* box, const void* src,
                                         unsigned rp, unsigned dp) {
    InterlockedIncrement(&g_nupds);
    if (src) dump_cb(res, (const float*)src, "upd");
    g_ctx_vt[ctx_vt(c)].upd(c, res, sub, box, src, rp, dp);
}
typedef void (WINAPI* draw_fn)(void*, unsigned, unsigned, int, unsigned, unsigned);
static draw_fn real_draw[4];
#define DRAW_HOOK(i) static void WINAPI log_draw##i(void* c, unsigned a, unsigned b, int d, unsigned e, unsigned f) \
    { InterlockedIncrement(&g_ndraws); real_draw[i](c, a, b, d, e, f); }
DRAW_HOOK(0) DRAW_HOOK(1) DRAW_HOOK(2) DRAW_HOOK(3)
typedef void (WINAPI* upd1_fn)(void*, void*, unsigned, const void*, const void*, unsigned, unsigned, unsigned);
static upd1_fn real_upd1;
static void WINAPI log_upd1(void* c, void* res, unsigned sub, const void* box, const void* src, unsigned rp,
                            unsigned dp, unsigned fl) {
    InterlockedIncrement(&g_nupd1);
    if (src) dump_cb(res, (const float*)src, "upd1");
    real_upd1(c, res, sub, box, src, rp, dp, fl);
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
    /* The context's method table is a per-object copy d3d11 may swap, so the
     * code itself is hooked: the second device's (the first is d3dgl's probe). */
    static int ndev;
    if (hr >= 0 && c && ++ndev == 2) {     /* the first device is d3dgl's probe */
        g_ctx = c;
        CloseHandle(CreateThread(NULL, 0, ctx_patch_loop, NULL, 0, NULL));
    }
    return hr;
}

static void hook_shader_compiler(void) {
    if (getenv("RON_LOG_CBUF"))
        *(void**)&real_D3D11CreateDevice = hook_export("d3d11.dll", "D3D11CreateDevice", log_D3D11CreateDevice);
    *(void**)&real_D3DCompileFromFile = hook_export("D3DCOMPILER_47.dll", "D3DCompileFromFile", log_D3DCompileFromFile);
    *(void**)&real_D3DCompile = hook_export("D3DCOMPILER_47.dll", "D3DCompile", log_D3DCompile);
}

/* The CRT's _libm_sse2_*_precise take and return a double in xmm0, not on
 * the stack, so the native bridge (stack in, eax/st0 out) handed them junk
 * and the game got junk back: the camera's rotation (cos/sin of its pitch)
 * and the tone-mapping exposure came out garbage, and the world drew black.
 * They run here on the guest's own xmm0. */
#define LIBM_SHIM(name, fn) \
    static void shim_##name(void) { g_xmm[0].f64[0] = fn(g_xmm[0].f64[0]); g_esp += 4; }
LIBM_SHIM(libm_sin, sin)
LIBM_SHIM(libm_cos, cos)
LIBM_SHIM(libm_tan, tan)
LIBM_SHIM(libm_sqrt, sqrt)

#define GUEST_SHIMS \
    { "_libm_sse2_sin_precise", shim_libm_sin }, \
    { "_libm_sse2_cos_precise", shim_libm_cos }, \
    { "_libm_sse2_tan_precise", shim_libm_tan }, \
    { "_libm_sse2_sqrt_precise", shim_libm_sqrt }, \
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

static HANDLE g_main_thread;
/* "module+offset" for an address inside an executable section of a DLL, or "". */
static const char* where_is(uint32_t a) {
    static char out[MAX_PATH + 16]; char path[MAX_PATH]; HMODULE m; MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)(uintptr_t)a, &mb, sizeof mb) || !(mb.Protect & 0xF0)) return "";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)a, &m) || !GetModuleFileNameA(m, path, sizeof path)) return "";
    snprintf(out, sizeof out, "%s+%X", strrchr(path, 0x5C) + 1, a - (uint32_t)(uintptr_t)m);
    return out;
}
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count);
    fprintf(stderr, "[d3d11] Map %ld UpdateSubresource %ld UpdateSubresource1 %ld draws %ld\n", g_nmaps, g_nupds, g_nupd1, g_ndraws);
    native32_dump_icalls(8);
    if (g_main_thread && SuspendThread(g_main_thread) != (DWORD)-1) {
        CONTEXT cx = { .ContextFlags = CONTEXT_FULL };
        GetThreadContext(g_main_thread, &cx);
        fprintf(stderr, "main thread: eip %08lX %s, esp %08lX; code addresses on its stack:\n",
                cx.Eip, where_is(cx.Eip), cx.Esp);
        const uint32_t* sp = (const uint32_t*)(uintptr_t)cx.Esp;
        for (int i = 0, shown = 0; i < 2048 && shown < 24 && !IsBadReadPtr(sp + i, 4); i++) {
            const char* m = where_is(sp[i]);
            if (*m) { fprintf(stderr, "  [esp+%04X] %08X %s\n", i * 4, sp[i], m); shown++; }
        }
    }
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

/* --mute (implied by --headless): mute this process's audio session on every
 * output device, so test runs are silent. On its own MTA thread so the
 * game's own CoInitialize on the main thread is untouched. */
#define COBJMACROS
#define CINTERFACE
#include <mmdeviceapi.h>
#include <audiopolicy.h>
static const CLSID CLSID_MMDev = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const IID IID_MMDevEnum = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const IID IID_SessMgr = {0xBFA971F1,0x4D5E,0x40BB,{0x93,0x5E,0x96,0x70,0x39,0xBF,0xBE,0xE6}};
static DWORD WINAPI mute_thread(void* quiet) {
    IMMDeviceEnumerator* en = NULL; IMMDeviceCollection* all = NULL; UINT n = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&CLSID_MMDev, NULL, CLSCTX_ALL, &IID_MMDevEnum, (void**)&en)))
        return 1;
    IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &all);
    if (all) IMMDeviceCollection_GetCount(all, &n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = NULL; IAudioSessionManager* m = NULL; ISimpleAudioVolume* v = NULL;
        IMMDeviceCollection_Item(all, i, &d);
        if (d && SUCCEEDED(IMMDevice_Activate(d, &IID_SessMgr, CLSCTX_ALL, NULL, (void**)&m))) {
            if (SUCCEEDED(IAudioSessionManager_GetSimpleAudioVolume(m, NULL, FALSE, &v))) {
                ISimpleAudioVolume_SetMute(v, TRUE, NULL);
                ISimpleAudioVolume_Release(v);
            }
            IAudioSessionManager_Release(m);
        }
        if (d) IMMDevice_Release(d);
    }
    if (!quiet || !*(int*)quiet) fprintf(stderr, "[mute] muted on %u output device(s)\n", n);
    IMMDeviceCollection_Release(all); IMMDeviceEnumerator_Release(en);
    return 0;
}
/* Again every second: a device or session the game opens later is muted too. */
static DWORD WINAPI mute_loop(void* unused) {
    for (int quiet = 0;; quiet = 1) { mute_thread(&quiet); Sleep(1000); }
}
static void mute_audio(void) {
    HANDLE t = CreateThread(NULL, 0, mute_thread, NULL, 0, NULL);
    WaitForSingleObject(t, 5000); CloseHandle(t);
    CloseHandle(CreateThread(NULL, 0, mute_loop, NULL, 0, NULL));
}

/* --headless runs on a hidden desktop (tools/hidden_run.py), where the game
 * is never the foreground window and so stops rendering. Tell it it is. */
static DWORD g_main_tid; static HWND g_game_wnd;
static BOOL CALLBACK find_game_wnd(HWND h, LPARAM unused) {
    RECT r; (void)unused;
    if (GetWindow(h, GW_OWNER) || !GetWindowRect(h, &r) || r.right - r.left < 320) return TRUE;
    g_game_wnd = h; return FALSE;
}
static HWND WINAPI fake_foreground(void) { return g_game_wnd; }
/* RON_CLICKS="SECONDS:X,Y;SECONDS:X,Y...": left clicks at client X,Y, the
 * cursor reported there too (GetCursorPos), to drive menus headlessly. */
static POINT g_cursor;
static BOOL WINAPI fake_GetCursorPos(POINT* p) { *p = g_cursor; ClientToScreen(g_game_wnd, p); return TRUE; }
static void click_script(void) {
    const char* p = getenv("RON_CLICKS");
    DWORD t0 = GetTickCount();
    while (p && *p) {
        DWORD at = strtoul(p, (char**)&p, 0) * 1000; p += *p == ':';
        int x = strtol(p, (char**)&p, 0); p += *p == ',';
        int y = strtol(p, (char**)&p, 0); p += *p == ';';
        DWORD now = GetTickCount() - t0;
        if (at > now) Sleep(at - now);
        g_cursor.x = x; g_cursor.y = y;
        LPARAM l = MAKELPARAM(x, y);
        PostMessageW(g_game_wnd, WM_MOUSEMOVE, 0, l); Sleep(150);
        PostMessageW(g_game_wnd, WM_LBUTTONDOWN, MK_LBUTTON, l); Sleep(100);
        PostMessageW(g_game_wnd, WM_LBUTTONUP, 0, l);
        fprintf(stderr, "[headless] click %d,%d at %lus\n", x, y, at / 1000);
    }
}
static DWORD WINAPI activate_loop(void* unused) {
    (void)unused;
    while (!g_game_wnd) { Sleep(200); EnumThreadWindows(g_main_tid, find_game_wnd, 0); }
    fprintf(stderr, "[headless] game window %p: telling it it is active\n", (void*)g_game_wnd);
    PostMessageW(g_game_wnd, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_game_wnd, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_game_wnd, WM_SETFOCUS, 0, 0);
    click_script();
    return 0;
}
/* RON_SNAP=SECONDS[,SECONDS...] (fractions ok, up to 160): at each time, the next presented frame goes
 * to build-farm-style RON_SNAP_FILE (default snap.bmp; -N appended per shot).
 * Present is hooked in dxgi's code, found from a throwaway swap chain. */
#include <d3d11.h>
static long (WINAPI* real_Present)(IDXGISwapChain*, UINT, UINT);
static DWORD g_snap_at[160]; static int g_nsnap, g_snapped;
static void snap_frame(IDXGISwapChain* sc) {
    ID3D11Texture2D *bb = NULL, *st = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* cx = NULL;
    D3D11_TEXTURE2D_DESC d; D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&bb))) return;
    ID3D11Texture2D_GetDevice(bb, &dev); ID3D11Device_GetImmediateContext(dev, &cx);
    ID3D11Texture2D_GetDesc(bb, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(cx, (ID3D11Resource*)st, (ID3D11Resource*)bb);
        if (SUCCEEDED(ID3D11DeviceContext_Map(cx, (ID3D11Resource*)st, 0, D3D11_MAP_READ, 0, &m))) {
            char name[MAX_PATH]; const char* base = getenv("RON_SNAP_FILE");
            snprintf(name, sizeof name, "%s-%d.bmp", base ? base : "snap", g_snapped);
            BITMAPINFOHEADER bh = { sizeof bh, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB };
            BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof bh + d.Width * d.Height * 4), 0, 0, sizeof fh + sizeof bh };
            FILE* f = fopen(name, "wb");
            if (f) {
                fwrite(&fh, sizeof fh, 1, f); fwrite(&bh, sizeof bh, 1, f);
                for (UINT y = 0; y < d.Height; y++) fwrite((char*)m.pData + y * m.RowPitch, 4, d.Width, f);
                fclose(f);
            }
            fprintf(stderr, "[snap] %s %ux%u format %u\n", name, d.Width, d.Height, d.Format);
            ID3D11DeviceContext_Unmap(cx, (ID3D11Resource*)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11DeviceContext_Release(cx); ID3D11Device_Release(dev); ID3D11Texture2D_Release(bb);
}
/* Swap chain methods whose code can't be hooked (a push imm32 prologue) are
 * swapped in the method table, which dxgi shares between its swap chains. */
static void* patch_slot(void** vt, int i, void* to) {
    DWORD was; void* old = vt[i];
    if (old == to || !VirtualProtect(&vt[i], sizeof *vt, PAGE_READWRITE, &was)) return old;
    vt[i] = to; VirtualProtect(&vt[i], sizeof *vt, was, &was);
    return old;
}
static long (WINAPI* real_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
static long WINAPI log_SetFullscreenState(IDXGISwapChain* sc, BOOL on, IDXGIOutput* o) {
    long hr = real_SetFullscreenState(sc, on, o);
    fprintf(stderr, "[dxgi] SetFullscreenState(%d) -> %08lX\n", on, hr); fflush(stderr);
    return hr;
}
static long (WINAPI* real_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
static long WINAPI log_ResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* m) {
    fprintf(stderr, "[dxgi] ResizeTarget %ux%u\n", m ? m->Width : 0, m ? m->Height : 0); fflush(stderr);
    return real_ResizeTarget(sc, m);
}
static void** g_sc_vt;
static void patch_swap_chain(IDXGISwapChain* sc) {
    void** vt = *(void***)sc;
    if (vt == g_sc_vt) return;
    fprintf(stderr, "[dxgi] patching swap chain table %p (was %p)\n", (void*)vt, (void*)g_sc_vt);
    g_sc_vt = vt;
    *(void**)&real_SetFullscreenState = patch_slot(vt, 10, log_SetFullscreenState);
    *(void**)&real_ResizeTarget = patch_slot(vt, 14, log_ResizeTarget);
}
static int g_show_at_present;
static HWND (WINAPI* real_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HWND WINAPI log_CreateWindowExW(DWORD ex, LPCWSTR c, LPCWSTR n, DWORD st, int x, int y, int cx, int cy, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    /* The game's window is born a visible screen-sized popup, black until the
     * game resizes it to its real mode: on screen, the whole display blanks.
     * It is made hidden instead, and shown at the first Present. */
    if ((ex & WS_EX_APPWINDOW) && (st & WS_POPUP) && (st & WS_VISIBLE)) { st &= ~WS_VISIBLE; g_show_at_present = 1; }
    HWND w = real_CreateWindowExW(ex, c, n, st, x, y, cx, cy, p, m, i, l);
    if (getenv("RON_LOG_WIN")) fprintf(stderr, "[win] CreateWindowExW %p ex=%lX style=%08lX %d,%d %dx%d t=%lu\n", (void*)w, ex, st, x, y, cx, cy, GetTickCount()); fflush(stderr);
    return w;
}
static long WINAPI log_Present(IDXGISwapChain* sc, UINT sync, UINT fl) {
    static IDXGISwapChain* seen;
    if (sc != seen) {
        DXGI_SWAP_CHAIN_DESC d; seen = sc;
        patch_swap_chain(sc);
        if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)) && g_show_at_present && !IsWindowVisible(d.OutputWindow))
            ShowWindow(d.OutputWindow, SW_SHOW);
        if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)))
            fprintf(stderr, "[dxgi] swap chain %ux%u windowed=%d effect=%d style=%08lX\n", d.BufferDesc.Width,
                    d.BufferDesc.Height, d.Windowed, d.SwapEffect, GetWindowLongW(d.OutputWindow, GWL_STYLE));
    }
    if (g_snapped < g_nsnap && GetTickCount() >= g_snap_at[g_snapped]) { snap_frame(sc); g_snapped++; }
    return real_Present(sc, sync, fl);
}
static void hook_present(void) {
    const char* p = getenv("RON_SNAP");
    DWORD t0 = GetTickCount();
    for (; p && *p && g_nsnap < 160; p += strspn(p, ", "))
        g_snap_at[g_nsnap++] = t0 + (DWORD)(1000 * strtod(p, (char**)&p));
    HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 64, 64, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC sd = { { 64, 64, { 60, 1 }, DXGI_FORMAT_R8G8B8A8_UNORM }, { 1, 0 },
                                DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, w, TRUE, DXGI_SWAP_EFFECT_FLIP_DISCARD };
    IDXGISwapChain* sc = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* cx = NULL;
    if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                             &sd, &sc, &dev, NULL, &cx))) { fprintf(stderr, "[snap] no swap chain\n"); return; }
    *(void**)&real_Present = hook_code((uint8_t*)sc->lpVtbl->Present, "Present", log_Present);
    patch_swap_chain(sc);
    IDXGISwapChain_Release(sc); ID3D11DeviceContext_Release(cx); ID3D11Device_Release(dev); DestroyWindow(w);
}

static void fake_active(void) {
    hook_present();
    g_main_tid = GetCurrentThreadId();
    hook_export("user32.dll", "GetForegroundWindow", fake_foreground);
    hook_export("user32.dll", "GetActiveWindow", fake_foreground);
    if (getenv("RON_CLICKS")) hook_export("user32.dll", "GetCursorPos", fake_GetCursorPos);
    CloseHandle(CreateThread(NULL, 0, activate_loop, NULL, 0, NULL));
}

int main(int argc, char** argv) {
    int mute = 0;
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
        else if (!strcmp(argv[i], "--headless")) g_headless = mute = 1;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--gameargs") && i + 1 < argc) extra = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: ron [--run] [--headless] [--mute] [--game INSTALL_DIR] [--gameargs \"...\"]\n"
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
    if (mute) mute_audio();
    if (g_headless) fake_active(); else hook_present();
    claim_guest_path();
    hook_message_box();
    hook_change_display();
    *(void**)&real_CreateWindowExW = hook_export("user32.dll", "CreateWindowExW", log_CreateWindowExW);
    hook_remote_session();
    hook_shader_compiler();
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", ron_entry_va);
    fflush(stdout);
    g_entered = 1;
    native32_call_guest(ron_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
