/* When the program dies, says why in the run report: the exception, where it happened, the guest's program
   counter, and a short host call stack (offsets in the program, for nm or scripts/55_crash_lookup.py). The
   rolling frame log is saved too. Without this a crash left a report that just stops. */
#include <windows.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crash.h"
#include "gx_stream.h"

static FILE* crash_report;
static char frames_path[600];
static volatile LONG crashed;
static const volatile uint32_t *guest_pc, *guest_lr;

#define PREFERRED_BASE 0x140000000ull

static unsigned long long program_offset(DWORD64 address) {
    HMODULE self = GetModuleHandleA(NULL);
    DWORD64 base = (DWORD64)self;
    return (unsigned long long)(address - base + PREFERRED_BASE);
}

static void write_stack(CONTEXT* context) {
    CONTEXT c = *context;
    for (int frame = 0; frame < 16 && c.Rip; frame++) {
        HMODULE module = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)c.Rip, &module);
        if (module) GetModuleFileNameA(module, name, sizeof name);
        const char* base_name = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
        if (module == GetModuleHandleA(NULL))
            fprintf(crash_report, "  at program offset %llX\n", program_offset(c.Rip));
        else
            fprintf(crash_report, "  at %s+%llX\n", base_name, (unsigned long long)(c.Rip - (DWORD64)module));
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(c.Rip, &image_base, NULL);
        if (!entry) { /* a leaf function: the return address is on top of the stack */
            if (IsBadReadPtr((void*)c.Rsp, 8)) break;
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void* handler_data = NULL;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, entry, &c, &handler_data, &establisher, NULL);
    }
}

static void finish(const char* what, CONTEXT* context) {
    if (InterlockedExchange(&crashed, 1)) return; /* only the first thread to die writes */
    if (crash_report) {
        fprintf(crash_report, "Crashed: %s", what);
        if (context) fprintf(crash_report, " at program offset %llX", program_offset(context->Rip));
        if (guest_pc) fprintf(crash_report, ", guest pc %08X lr %08X", *guest_pc, guest_lr ? *guest_lr : 0u);
        fprintf(crash_report, "\n");
        if (context) write_stack(context);
        fflush(crash_report);
    }
    if (frames_path[0]) gx_adapter_save_frames(frames_path);
    if (crash_report) fflush(crash_report);
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    char what[160];
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        snprintf(what, sizeof what, "access violation %s %llX",
                 info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                 (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    else if (code == EXCEPTION_STACK_OVERFLOW)
        snprintf(what, sizeof what, "stack overflow");
    else
        snprintf(what, sizeof what, "exception %08lX", code);
    finish(what, info->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void on_abort(int signal) {
    (void)signal;
    CONTEXT context;
    RtlCaptureContext(&context);
    finish("abort (a library stopped the program; its message is just above)", &context);
}

void crash_install(FILE* report, const char* frames, const volatile uint32_t* pc, const volatile uint32_t* lr) {
    crash_report = report;
    snprintf(frames_path, sizeof frames_path, "%s", frames ? frames : "");
    guest_pc = pc;
    guest_lr = lr;
    SetUnhandledExceptionFilter(on_exception);
    signal(SIGABRT, on_abort);
    /* A stack overflow needs stack to report on. */
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
}

void crash_thread_start(void) {
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
}
