#include "CrashHandler.h"

#include <sentry.h>
#include <string>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace st {
namespace crash {

namespace {

std::string g_exeDir;       // directory containing the executable
std::string g_crashDir;     // <exeDir>/crashreports  (sentry database + our summary)
std::string g_reporterPath; // <exeDir>/SimtaryCrashReporter(.exe)
std::string g_summaryPath;  // <crashDir>/last_crash.txt
std::string g_appName = "Simtary"; // shown in the crash summary + reporter GUI

#if defined(_WIN32)
std::string ExeDir()
{
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

// Append one symbolized frame to the human-readable summary file.
void LogSym(FILE* lf, HANDLE proc, const char* tag, DWORD64 addr)
{
    char buf[sizeof(SYMBOL_INFO) + 512] = {0};
    SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen   = 512;

    HMODULE mod = nullptr;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        char full[MAX_PATH] = {0};
        GetModuleFileNameA(mod, full, MAX_PATH);
        const char* slash = strrchr(full, '\\');
        lstrcpynA(modname, slash ? slash + 1 : full, MAX_PATH);
    }
    const DWORD64 rva = mod ? (addr - (DWORD64)mod) : 0;

    DWORD64 disp = 0;
    if (SymFromAddr(proc, addr, &disp, si)) {
        IMAGEHLP_LINE64 line = {0}; line.SizeOfStruct = sizeof(line); DWORD ld = 0;
        if (SymGetLineFromAddr64(proc, addr, &ld, &line))
            fprintf(lf, "%s %s+0x%llX  (%s:%lu)  [%s+0x%llX]\n", tag, si->Name,
                    (unsigned long long)disp, line.FileName, line.LineNumber, modname, (unsigned long long)rva);
        else
            fprintf(lf, "%s %s+0x%llX  [%s+0x%llX]\n", tag, si->Name,
                    (unsigned long long)disp, modname, (unsigned long long)rva);
    } else {
        fprintf(lf, "%s ???  [%s+0x%llX]\n", tag, modname, (unsigned long long)rva);
    }
}

void LaunchReporter()
{
    if (g_reporterPath.empty()) return;
    std::string cmd = "\"" + g_reporterPath + "\" \"" + g_crashDir + "\"";
    STARTUPINFOA si = {0}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Write a human-readable summary of the fault. Runs inside the crashing process,
// so keep it lean and defensive. The full minidump is still produced by Crashpad.
void WriteSummary(EXCEPTION_POINTERS ep)
{
    FILE* lf = fopen(g_summaryPath.c_str(), "w");
    if (!lf) return;

    const EXCEPTION_RECORD* er = ep.ExceptionRecord;
    SYSTEMTIME now; GetLocalTime(&now);
    fprintf(lf, "Milistry crashed.\n");
    fprintf(lf, "time: %04d-%02d-%02d %02d:%02d:%02d\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    if (er) {
        fprintf(lf, "exception code: 0x%08lX   at: %p\n",
                (unsigned long)er->ExceptionCode, er->ExceptionAddress);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            fprintf(lf, "access violation: %s 0x%p\n",
                    er->ExceptionInformation[0] == 1 ? "WRITE to" :
                    er->ExceptionInformation[0] == 8 ? "EXECUTE at" : "READ from",
                    (void*)er->ExceptionInformation[1]);
    }

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    if (er) LogSym(lf, proc, "fault:", (DWORD64)er->ExceptionAddress);

    if (ep.ContextRecord) {
        CONTEXT ctx = *ep.ContextRecord;
        STACKFRAME64 sf = {0};
#if defined(_M_X64)
        DWORD machine = IMAGE_FILE_MACHINE_AMD64;
        sf.AddrPC.Offset    = ctx.Rip; sf.AddrPC.Mode    = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
#else
        DWORD machine = IMAGE_FILE_MACHINE_I386;
        sf.AddrPC.Offset    = ctx.Eip; sf.AddrPC.Mode    = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
#endif
        fprintf(lf, "---- call stack ----\n");
        for (int i = 0; i < 64; ++i) {
            if (!StackWalk64(machine, proc, GetCurrentThread(), &sf, &ctx,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrPC.Offset == 0) break;
            LogSym(lf, proc, "  ", sf.AddrPC.Offset);
        }
    }
    fclose(lf);
}
#endif // _WIN32

// sentry on_crash hook - runs in the crashing process before the dump is finalized.
sentry_value_t OnCrash(const sentry_ucontext_t* uctx, sentry_value_t event, void* /*closure*/)
{
#if defined(_WIN32)
    if (uctx) WriteSummary(uctx->exception_ptrs);
    LaunchReporter();
#else
    (void)uctx;
    if (!g_reporterPath.empty()) {
        std::string cmd = "\"" + g_reporterPath + "\" \"" + g_crashDir + "\" &";
        (void)system(cmd.c_str());
    }
#endif
    return event; // keep the event so Crashpad writes the minidump to disk
}

// No-op transport: guarantees no envelope is ever sent over the network, even if
// the SDK ever decided to. (Belt-and-suspenders with withheld user consent.)
void NoopSend(sentry_envelope_t* envelope, void* /*state*/)
{
    sentry_envelope_free(envelope);
}

} // namespace

void Init(const char* /*exePath*/, const char* appName)
{
    if (appName && *appName) g_appName = appName;

#if defined(_WIN32)
    g_exeDir       = ExeDir();
    g_crashDir     = g_exeDir + "\\crashreports";
    g_reporterPath = g_exeDir + "\\SimtaryCrashReporter.exe";
    g_summaryPath  = g_crashDir + "\\last_crash.txt";
    CreateDirectoryA(g_crashDir.c_str(), nullptr);
#else
    g_exeDir       = ".";
    g_crashDir     = "crashreports";
    g_reporterPath = "./SimtaryCrashReporter";
    g_summaryPath  = g_crashDir + "/last_crash.txt";
#endif

    sentry_options_t* opts = sentry_options_new();
    sentry_options_set_database_path(opts, g_crashDir.c_str());
#if defined(_WIN32)
    const std::string handler = g_exeDir + "\\crashpad_handler.exe";
    sentry_options_set_handler_path(opts, handler.c_str());
#endif
    // A DSN is required for the SDK (and therefore Crashpad) to start, but uploads
    // are fully disabled: user consent is required and never granted, and the
    // transport drops everything. Result: minidumps are saved locally, never sent.
    sentry_options_set_dsn(opts, "https://00000000000000000000000000000000@127.0.0.1/0");
    sentry_options_set_require_user_consent(opts, 1);
    sentry_options_set_auto_session_tracking(opts, 0);
    sentry_options_set_transport(opts, sentry_transport_new(NoopSend));
    sentry_options_set_on_crash(opts, OnCrash, nullptr);

    sentry_init(opts);
}

void CheckPreviousCrash()
{
    // Fallback: if the last run crashed (e.g. a fast-fail crash that bypassed
    // on_crash), make sure the reporter is still shown once.
    if (sentry_get_crashed_last_run() == 1) {
#if defined(_WIN32)
        LaunchReporter();
#else
        if (!g_reporterPath.empty()) {
            std::string cmd = "\"" + g_reporterPath + "\" \"" + g_crashDir + "\" &";
            (void)system(cmd.c_str());
        }
#endif
        sentry_clear_crashed_last_run();
    }
}

void Shutdown()
{
    sentry_close();
}

} // namespace crash
} // namespace st
