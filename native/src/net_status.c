/* A small window that says what online play is doing while players connect: the game's own window only opens
   once everyone is in, and before this there was nothing on screen at all. It runs on its own thread, so the
   network code can wait without it freezing, and its Cancel button ends the wait. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "net_status.h"

#define ID_BUTTON 1
#define WM_NEW_TEXT (WM_APP + 1)
#define WM_FAILED (WM_APP + 2)

static HANDLE thread, ready;
static HWND window, label, button;
static CRITICAL_SECTION lock;
static char text[2048];
static volatile LONG cancelled;
static int shown, off_screen; /* off_screen: the self-test, never in sight or focus */

static LRESULT CALLBACK window_proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BUTTON) DestroyWindow(w);
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&cancelled, 1);
        PostQuitMessage(0);
        return 0;
    case WM_NEW_TEXT: {
        char copy[sizeof text];
        EnterCriticalSection(&lock);
        memcpy(copy, text, sizeof copy);
        LeaveCriticalSection(&lock);
        SetWindowTextA(label, copy);
        return 0;
    }
    case WM_FAILED:
        SetWindowTextA(button, "Close");
        SetWindowTextA(w, "Wii Sports Resort online: couldn't start");
        FlashWindow(w, TRUE);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcA(w, message, wp, lp);
}

static DWORD WINAPI run(LPVOID unused) {
    (void)unused;
    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSA kind = {0};
    kind.lpfnWndProc = window_proc;
    kind.hInstance = instance;
    kind.hCursor = LoadCursor(NULL, IDC_ARROW);
    kind.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    kind.lpszClassName = "WsrOnlineStatus";
    RegisterClassA(&kind);
    int width = 560, height = 260;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    if (off_screen) x = y = -6000;
    window = CreateWindowExA(off_screen ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, kind.lpszClassName, "Wii Sports Resort online", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             x, y, width, height, NULL, NULL, instance, NULL);
    RECT inside;
    GetClientRect(window, &inside);
    label = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 14, inside.right - 32,
                            inside.bottom - 70, window, NULL, instance, NULL);
    button = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, inside.right - 116,
                             inside.bottom - 44, 100, 30, window, (HMENU)ID_BUTTON, instance, NULL);
    NONCLIENTMETRICSA metrics = {sizeof metrics};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    HFONT font = CreateFontIndirectA(&metrics.lfMessageFont);
    SendMessageA(label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(button, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(window, WM_NEW_TEXT, 0, 0);
    ShowWindow(window, off_screen ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    SetEvent(ready);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageA(window, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    DeleteObject(font);
    window = NULL;
    return 0;
}

void net_status_open(int show) {
    InitializeCriticalSection(&lock);
    shown = show;
    if (!show) return;
    ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, run, NULL, 0, NULL);
    if (thread) WaitForSingleObject(ready, 5000);
}

void net_status(const char* format, ...) {
    if (!shown) return;
    va_list args;
    va_start(args, format);
    EnterCriticalSection(&lock);
    vsnprintf(text, sizeof text, format, args);
    LeaveCriticalSection(&lock);
    va_end(args);
    if (window) PostMessageA(window, WM_NEW_TEXT, 0, 0);
}

int net_status_cancelled(void) { return cancelled != 0; }

void net_status_fail(const char* message) {
    if (!shown || !thread) return;
    if (!cancelled) {
        net_status("%s", message);
        if (window) PostMessageA(window, WM_FAILED, 0, 0);
    }
    WaitForSingleObject(thread, INFINITE); /* until the player closes it */
}

/* Draws the window into a BMP with PrintWindow, which copies only this window's own pixels. */
static int save_window(const char* path) {
    RECT r;
    GetWindowRect(window, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    int ok = PrintWindow(window, dc, PW_RENDERFULLCONTENT) != 0;
    FILE* f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        BITMAPFILEHEADER head = {0};
        head.bfType = 0x4D42;
        head.bfOffBits = sizeof head + sizeof info.bmiHeader;
        head.bfSize = head.bfOffBits + (DWORD)(w * h * 4);
        fwrite(&head, sizeof head, 1, f);
        fwrite(&info.bmiHeader, sizeof info.bmiHeader, 1, f);
        fwrite(bits, 4, (size_t)w * (size_t)h, f);
        fclose(f);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
    return f != NULL;
}

int net_status_self_test(const char* folder) {
    char path[600];
    off_screen = 1;
    net_status_open(1);
    if (!window) return 1;
    net_status("Room ABC123 is open on the relay relay.example.net:51020. 0 of 1 other player has joined.\n\n"
               "Give the others the room code ABC123. They need the same relay server address in their launcher."
               "\n\nCancel closes the room.");
    Sleep(400);
    snprintf(path, sizeof path, "%s/status-waiting.bmp", folder);
    int ok = save_window(path);
    EnterCriticalSection(&lock);
    snprintf(text, sizeof text, "Couldn't reach the host at 203.0.113.7:51010 in a minute. Usually this means the "
             "host's router doesn't pass TCP port 51010 to their PC, Windows Firewall on their PC blocks the game, or "
             "the code is their home-network code. A relay server avoids port forwarding.");
    LeaveCriticalSection(&lock);
    PostMessageA(window, WM_NEW_TEXT, 0, 0);
    PostMessageA(window, WM_FAILED, 0, 0);
    Sleep(400);
    snprintf(path, sizeof path, "%s/status-failed.bmp", folder);
    ok = save_window(path) && ok;
    net_status_close();
    return ok ? 0 : 1;
}

void net_status_close(void) {
    if (!shown || !thread) return;
    if (window) PostMessageA(window, WM_CLOSE, 0, 0);
    WaitForSingleObject(thread, 5000);
    cancelled = 0; /* closing it ourselves isn't a cancel */
}
