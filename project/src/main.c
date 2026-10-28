// Digimon Rumble Arena 2 (USA): host executable for the recompiled game.
//
// XBE: title ID 0x42410001, base 0x00010000, entry 0x00127020, 13 sections,
// 158 kernel imports.

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <xbox/xboxrecomp.h>

// RECOMP_TLS is required: a plain extern to a __declspec(thread) variable
// resolves to the TLS template, not the calling thread's copy.
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
// Global because one guest routine can lift to several C functions.
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

#define DRA2_ENTRY_POINT        0x00127020
#define DRA2_GAME_DIR            "game"

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

extern void xbe_entry_point(void);

// RECOMP_SAMPLE_PROFILE=<secs>: every 1 ms record each thread's RIP; every
// <secs> print the 30 hottest symbols. Needs the PDB beside the exe.
#define PROF_SLOTS (1u << 16)
static struct { DWORD64 rip; unsigned n; } s_prof[PROF_SLOTS];
static unsigned s_prof_total;

static void prof_report(void)
{
    enum { TOP = 30 };
    static struct { char name[96]; unsigned n; } agg[4096];
    unsigned i, j, nagg = 0;
    char buf[sizeof(SYMBOL_INFO) + 96];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;

    for (i = 0; i < PROF_SLOTS; i++) {
        DWORD64 disp = 0;
        const char *name = "?";
        if (!s_prof[i].n)
            continue;
        memset(buf, 0, sizeof buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 95;
        if (SymFromAddr(GetCurrentProcess(), s_prof[i].rip, &disp, sym))
            name = sym->Name;
        for (j = 0; j < nagg && strcmp(agg[j].name, name); j++)
            ;
        if (j == nagg) {
            if (nagg == 4096)
                continue;
            strncpy(agg[nagg].name, name, 95);
            agg[nagg++].n = 0;
        }
        agg[j].n += s_prof[i].n;
    }
    fprintf(stderr, "[SAMPLE] %u samples, hottest symbols:\n", s_prof_total);
    for (i = 0; i < TOP && i < nagg; i++) {
        unsigned best = i;
        for (j = i + 1; j < nagg; j++)
            if (agg[j].n > agg[best].n)
                best = j;
        if (best != i) {
            char t[96]; unsigned tn = agg[i].n;
            memcpy(t, agg[i].name, 96); memcpy(agg[i].name, agg[best].name, 96);
            memcpy(agg[best].name, t, 96); agg[i].n = agg[best].n; agg[best].n = tn;
        }
        fprintf(stderr, "  %5.1f%%  %s\n",
                100.0 * agg[i].n / (s_prof_total ? s_prof_total : 1), agg[i].name);
    }
    fflush(stderr);
}

// RECOMP_SAMPLE_STACKS=1: every thread's native stack with each report; a
// deadlock between host locks shows only here.
static void prof_stacks(HANDLE *th, DWORD *tid, int n)
{
    HANDLE proc = GetCurrentProcess();
    char buf[sizeof(SYMBOL_INFO) + 96];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    int k, f;

    for (k = 0; k < n; k++) {
        CONTEXT c;
        STACKFRAME64 sf;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(th[k]) == (DWORD)-1)
            continue;
        if (GetThreadContext(th[k], &c)) {
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            fprintf(stderr, "[STACK] thread %lu:", (unsigned long)tid[k]);
            for (f = 0; f < 14 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th[k], &sf, &c,
                                              NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL); f++) {
                DWORD64 disp = 0;
                memset(buf, 0, sizeof buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 95;
                fprintf(stderr, " %s", SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?");
            }
            fputc('\n', stderr);
        }
        ResumeThread(th[k]);
    }
    fflush(stderr);
}

// RECOMP_RAM_SNAPSHOT=t1,t2,...: at each second t, write guest 0x10000-0x400000
// (.data and BSS) to ram-<t>.bin; diffing screens finds screen-state variables.
extern ptrdiff_t xbox_GetMemoryOffset(void);
static DWORD WINAPI ram_snapshots(LPVOID arg)
{
    const char *p = (const char *)arg;
    DWORD t0 = GetTickCount();
    while (*p) {
        int t = atoi(p);
        char name[32];
        FILE *f;
        while (GetTickCount() - t0 < (DWORD)t * 1000u)
            Sleep(50);
        snprintf(name, sizeof name, "ram-%d.bin", t);
        if ((f = fopen(name, "wb")) != NULL) {
            fwrite((const uint8_t *)xbox_GetMemoryOffset() + 0x10000, 1, 0x400000 - 0x10000, f);
            fclose(f);
            fprintf(stderr, "[RAM] snapshot %s\n", name);
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

static DWORD WINAPI prof_sampler(LPVOID arg)
{
    DWORD every_ms = (DWORD)(uintptr_t)arg * 1000u, last = GetTickCount();
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();

    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = { sizeof te };
        HANDLE th[64];
        DWORD tid[64];
        int n = 0, k, tick;

        if (snap != INVALID_HANDLE_VALUE) {
            for (BOOL ok = Thread32First(snap, &te); ok && n < 64; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                          FALSE, te.th32ThreadID);
                    if (h) { tid[n] = te.th32ThreadID; th[n++] = h; }
                }
            CloseHandle(snap);
        }
        for (tick = 0; tick < 1000; tick++) {     // refresh the thread list each second
            for (k = 0; k < n; k++) {
                CONTEXT c = { 0 };
                c.ContextFlags = CONTEXT_CONTROL;
                if (SuspendThread(th[k]) == (DWORD)-1)
                    continue;
                if (GetThreadContext(th[k], &c)) {
                    unsigned h = (unsigned)((c.Rip * 0x9E3779B97F4A7C15ull) >> 48) & (PROF_SLOTS - 1);
                    unsigned p;
                    for (p = 0; p < PROF_SLOTS; p++, h = (h + 1) & (PROF_SLOTS - 1))
                        if (!s_prof[h].n || s_prof[h].rip == c.Rip) {
                            s_prof[h].rip = c.Rip; s_prof[h].n++; s_prof_total++;
                            break;
                        }
                }
                ResumeThread(th[k]);
            }
            Sleep(1);
            if (GetTickCount() - last >= every_ms) {
                prof_report();
                if (getenv("RECOMP_SAMPLE_STACKS"))
                    prof_stacks(th, tid, n);
                memset(s_prof, 0, sizeof s_prof);
                s_prof_total = 0;
                last = GetTickCount();
            }
        }
        for (k = 0; k < n; k++)
            CloseHandle(th[k]);
    }
    return 0;
}

// Names the guest function at a fault (generated functions are PDB symbols)
// and scans the guest stack for return addresses: lifted code keeps no ebp
// chain, so the scan over-reports.
static void print_guest_context(void *rip)
{
    // SYMBOL_INFO is variable-length: over-allocate by MaxNameLen.
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        // An overrun copy can destroy the frame; then show what survives.
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        if (!shown) {
            uint32_t k;

            // The indirect-call history survives a smashed stack.
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            // Unfiltered: the faulting call's pointers and lengths are here.
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

// XAPI drives the OHCI registers directly; they trap to the runtime's model.
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

// The APU's own 512 KB; AC'97 above it stays plain memory for the codec-ready bit.
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Trapped OHCI and APU registers: a handled access is decoded and
        // stepped over, so execution resumes.
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
                return EXCEPTION_CONTINUE_EXECUTION;

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        // Native stack as module+offset: a fault in a driver thread has no
        // frame of ours.
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            char m[MAX_PATH];
            HMODULE hm;
            fprintf(stderr, "  thread %lu, RIP in %s\n", GetCurrentThreadId(),
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)ep->ContextRecord->Rip, &hm)
                    && GetModuleFileNameA(hm, m, sizeof m) ? m : "(no module)");
            fflush(stderr);
            fprintf(stderr, "  Native stack (code addresses in the first 64 slots):\n");
            for (int i = 0, shown = 0; i < 64 && shown < 16; i++) {
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)sp[i], &hm) && GetModuleFileNameA(hm, m, sizeof m)) {
                    const char *base = strrchr(m, 0x5C);
                    fprintf(stderr, "    [%d] %s+0x%llX\n", i, base ? base + 1 : m,
                            (unsigned long long)(sp[i] - (uintptr_t)hm));
                    shown++;
                }
            }
        }
        fflush(stderr);
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_OVERFLOW) {
        fprintf(stderr, "[CRASH] Integer divide fault 0x%08lX at RIP=0x%llX\n",
            ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)ep->ContextRecord->Rip);
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        print_guest_context((void *)ep->ContextRecord->Rip);
        // The divisor usually sits in the frame g_seh_ebp points at.
        {
            int off;
            fprintf(stderr, "  guest frame at g_seh_ebp=0x%08X:\n", g_seh_ebp);
            for (off = -48; off <= 12; off += 4)
                fprintf(stderr, "    [ebp%+d] 0x%08X\n", off,
                    *(uint32_t *)((uint8_t *)g_xbox_mem_offset + g_seh_ebp + off));
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// An installed copy runs with no RECOMP_* variables, so the product defaults
// apply; development runs set RECOMP_PB_EXEC and keep exactly what they set.
static char s_game_dir[MAX_PATH], s_xbe_path[MAX_PATH], s_save_dir[MAX_PATH];

static void setting_env(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

static void display_settings(void)
{
    char ini[MAX_PATH], rel[MAX_PATH], v[16];
    int res;
    FILE *f;

    snprintf(rel, sizeof rel, "%s\\settings.ini", s_save_dir);
    // GetPrivateProfile* reads a relative name from the Windows directory.
    if (!GetFullPathNameA(rel, sizeof ini, ini, NULL))
        return;
    CreateDirectoryA(s_save_dir, NULL);   // first start: the kernel makes it later
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES && (f = fopen(ini, "w")) != NULL) {
        fputs("; Digimon Rumble Arena 2 - display settings. Edit, then restart the game.\n"
              "[display]\n"
              "; Internal resolution: 1080, 1440 or 2160.\n"
              "resolution=1080\n"
              "; 1 fills 16:9 screens by widening the view; 0 keeps the original 4:3.\n"
              "widescreen=1\n"
              "; 1 borderless fullscreen, 0 window (F11 or Alt+Enter switches while playing).\n"
              "fullscreen=1\n"
              "; 0 primary monitor, or a display number from Windows Settings > Display.\n"
              "monitor=0\n", f);
        fclose(f);
    }
    setting_env("RECOMP_SETTINGS_INI", ini);   // the port menu writes here
    res = (int)GetPrivateProfileIntA("display", "resolution", 1080, ini);
    if (res != 1440 && res != 2160)
        res = 1080;
    snprintf(v, sizeof v, "%.4f", res / 480.0);
    setting_env("RECOMP_GPU_SCALE", v);
    setting_env("RECOMP_WIDESCREEN", GetPrivateProfileIntA("display", "widescreen", 1, ini) ? "1" : "0");
    setting_env("RECOMP_FULLSCREEN", GetPrivateProfileIntA("display", "fullscreen", 1, ini) ? "1" : "0");
    snprintf(v, sizeof v, "%u", GetPrivateProfileIntA("display", "monitor", 0, ini));
    setting_env("RECOMP_MONITOR", v);
}

static void product_setup(void)
{
    static const char *const defaults[][2] = {
        { "RECOMP_PB_EXEC", "1" }, { "RECOMP_ASYNC_IO", "1" }, { "RECOMP_USB", "1" },
        { "RECOMP_USB_PADS", "4" }, { "RECOMP_IRQL_LOCK", "1" }, { "RECOMP_AC97_READY", "1" },
        { "RECOMP_APU_DSP_ACK", "0x805F0810" }, { "RECOMP_GPU", "1" }, { "RECOMP_FB_WINDOW", "1" },
        // Front-panel wiring: OHCI0 ports 3,4,1,2 are players 1-4.
        { "RECOMP_USB_PORT", "2" },
        { "RECOMP_WINDOW_TITLE", "Digimon Rumble Arena 2" },
        // Title screen (Esc quits there): loaded menu resource size 2188 and
        // screen id 3, both .data statics.
        { "RECOMP_TITLE_STATE", "0x2F9698=2188,0x2F88C4=3" },
    };
    char exe[MAX_PATH], *slash;
    size_t i;
    DWORD attr;
    int product = !getenv("RECOMP_PB_EXEC");

    if (product) {
        for (i = 0; i < sizeof defaults / sizeof defaults[0]; i++)
            if (!getenv(defaults[i][0]))
                _putenv_s(defaults[i][0], defaults[i][1]);
    }

    GetModuleFileNameA(NULL, exe, sizeof exe);
    if ((slash = strrchr(exe, '\\')) != NULL)
        *slash = 0;
    if (getenv("RECOMP_GAME_DIR"))
        snprintf(s_game_dir, sizeof s_game_dir, "%s", getenv("RECOMP_GAME_DIR"));
    else {
        snprintf(s_game_dir, sizeof s_game_dir, "%s\\game", exe);
        attr = GetFileAttributesA(s_game_dir);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            snprintf(s_game_dir, sizeof s_game_dir, "%s", DRA2_GAME_DIR);
    }
    snprintf(s_xbe_path, sizeof s_xbe_path, "%s\\default.xbe", s_game_dir);
    if (getenv("RECOMP_SAVE_DIR"))
        snprintf(s_save_dir, sizeof s_save_dir, "%s", getenv("RECOMP_SAVE_DIR"));
    else if (strcmp(s_game_dir, DRA2_GAME_DIR) && getenv("LOCALAPPDATA")) {
        snprintf(s_save_dir, sizeof s_save_dir, "%s\\DigimonRumbleArena2", getenv("LOCALAPPDATA"));
        CreateDirectoryA(s_save_dir, NULL);
    } else
        snprintf(s_save_dir, sizeof s_save_dir, "save");
    if (product)
        display_settings();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    product_setup();

    printf("=== DRA2 - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    // Symbols are loaded now: SymInitialize allocates, which a fault
    // handler should not.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    if (getenv("RECOMP_RAM_SNAPSHOT"))
        CloseHandle(CreateThread(NULL, 0, ram_snapshots, (LPVOID)getenv("RECOMP_RAM_SNAPSHOT"), 0, NULL));
    if (getenv("RECOMP_SAMPLE_PROFILE"))
        CloseHandle(CreateThread(NULL, 0, prof_sampler,
            (LPVOID)(uintptr_t)atoi(getenv("RECOMP_SAMPLE_PROFILE")), 0, NULL));

    if (!load_xbe(s_xbe_path, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Place the game files in the 'game' subdirectory.",
                    "Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    // Same gate as the register trap: one is useless without the other.
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(s_game_dir, s_save_dir);
    }

    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    xbox_OhciInit();    // no-op unless RECOMP_USB is set

    g_esp = XBOX_STACK_TOP;

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", DRA2_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    // Without the flat table every indirect call binary-searches the
    // function list; failure falls back to that search.
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    // Must run on the guest thread: the registers it samples are thread-local.
    // Inert unless RECOMP_WATCHDOG_SECS is set.
    xbox_WatchdogStart();

    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

// Console entry point: a console build shows printf output.
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
