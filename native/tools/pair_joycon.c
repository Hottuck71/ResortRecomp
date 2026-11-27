/* Pairs Nintendo Switch Joy-Cons with Windows, then checks that each can talk to this PC, with no game running.
   The launcher's Joy-Con tab runs it; it is separate from PairRemote.exe, which pairs Wii Remotes.

   A Joy-Con pairs the modern way (Secure Simple Pairing with nothing to type), so Windows' own Bluetooth screen
   can do it too; this does it in one place and then tests it. The Joy-Con's small sync button is on its rail
   (the side that slides onto a Switch). Held down for a moment, the green player lights sweep back and forth:
   it is listening for about 30 seconds, and found under the name "Joy-Con (L)" or "Joy-Con (R)".

   As with Wii Remotes, Windows keeps a record of a Joy-Con it once knew and returns it from a search even while
   the Joy-Con is off or paired with a Switch since, so the player says when the lights sweep, with a key press,
   and only then is anything tried. A Joy-Con remembered but not connected is removed and paired again: after it
   was paired with a Switch, Windows' old key no longer works. Both Joy-Cons can be paired in one go.

   Then each Joy-Con is opened as joycon.c opens it for the game: its player lights come on, it buzzes, and the
   player presses A (right Joy-Con) or the right arrow (left Joy-Con).

   Usage: PairJoyCon.exe [--seconds 120] [--log pair-joycon.txt] [--list 1] [--pipe 1] [--test 1]
   --seconds is how long to wait for the key.
   --list only reports what Windows returns, and can't change anything.
   --pipe: run from the launcher, which shows what this prints and sends a line instead of a key press.
   --test 1: skip pairing and only test the Joy-Cons that are connected.
   Only entries named as Joy-Cons are ever changed. Other devices are counted, not recorded. */
#include <windows.h>
#include <conio.h>
#include <initguid.h>
#include <bluetoothapis.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "joycon.h"

#define INQUIRY_UNITS 3       /* 1.28 seconds each */
#define SEARCHES_PER_ROUND 4
#define PAIR_ROUNDS 3
#define HID_WAIT_MS 30000     /* Windows sets up the driver after pairing */
#define READY_WAIT_MS 8000
#define TEST_MS 30000
#define ATTEMPT_MS 10000      /* a pairing call can hang forever */
#define SETTLE_MS 1000
#define WII_A 0x0008u

#define IOCTL_DISCONNECT CTL_CODE(0x00000041, 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum { SEARCH_RECORDS, SEARCH_RADIO, SEARCH_LIST };

FILE* report; /* joycon.c logs here */
static int piped;
static const char* key_word = "press any key in this window";
static int attempts_left_behind;
static int paired_now;

static void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    if (report == NULL || report == stdout) return;
    va_start(args, format);
    vfprintf(report, format, args);
    va_end(args);
    fflush(report);
}

static const char* meaning(DWORD code) {
    switch (code) {
    case ERROR_SUCCESS: return "worked";
    case ERROR_NO_MORE_ITEMS: return "already paired";
    case ERROR_CANCELLED: return "cancelled, the Joy-Con may have stopped listening";
    case ERROR_NOT_AUTHENTICATED: return "the Joy-Con refused";
    case ERROR_DEVICE_NOT_CONNECTED: return "the Joy-Con isn't reachable";
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: return "timed out";
    case ERROR_INVALID_PARAMETER: return "Windows rejected the request";
    case ERROR_GEN_FAILURE: return "the Bluetooth driver reported a failure";
    case ERROR_SERVICE_DOES_NOT_EXIST: return "Windows found no game device service on it";
    case ERROR_ACCESS_DENIED: return "access denied";
    case ERROR_NOT_FOUND: return "Windows has no record of it";
    default: return "unknown";
    }
}

static int paired(DWORD code) { return code == ERROR_SUCCESS || code == ERROR_NO_MORE_ITEMS; }

static int is_joycon(const BLUETOOTH_DEVICE_INFO* device) {
    return wcsncmp(device->szName, L"Joy-Con (L)", 11) == 0 || wcsncmp(device->szName, L"Joy-Con (R)", 11) == 0;
}

typedef struct {
    HANDLE radio;
    BLUETOOTH_DEVICE_INFO device;
    DWORD result;
} Attempt;

/* Says yes to whatever Windows asks: a Joy-Con has no screen or keys, so there is nothing to compare. */
static BOOL CALLBACK answer(void* param, BLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS* request) {
    Attempt* attempt = param;
    BLUETOOTH_AUTHENTICATE_RESPONSE response;
    memset(&response, 0, sizeof response);
    response.bthAddressRemote = request->deviceInfo.Address;
    response.authMethod = request->authenticationMethod;
    switch (request->authenticationMethod) {
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        memcpy(response.pinInfo.pin, "0000", 4);
        response.pinInfo.pinLength = 4;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        response.passkeyInfo.passkey = 0;
        break;
    default: /* numeric comparison or passkey notification: accept the number */
        response.numericCompInfo.NumericValue = request->Numeric_Value;
        break;
    }
    DWORD sent = BluetoothSendAuthenticationResponseEx(attempt->radio, &response);
    say("    Windows asked to confirm the pairing (method %d); answered yes: result %lu, %s\n",
        (int)request->authenticationMethod, sent, meaning(sent));
    return TRUE;
}

static DWORD WINAPI run_attempt(void* param) {
    Attempt* attempt = param;
    HBLUETOOTH_AUTHENTICATION_REGISTRATION registration = NULL;
    DWORD result = BluetoothRegisterForAuthenticationEx(&attempt->device, &registration, answer, attempt);
    if (result != ERROR_SUCCESS) {
        attempt->result = result;
        return 0;
    }
    attempt->result = BluetoothAuthenticateDeviceEx(NULL, attempt->radio, &attempt->device, NULL, MITMProtectionNotRequired);
    BluetoothUnregisterAuthentication(registration);
    return 0;
}

/* The pairing call has no timeout of its own: one that overruns is left behind (see PairRemote.exe). */
static DWORD limited_attempt(HANDLE radio, BLUETOOTH_DEVICE_INFO* device) {
    Attempt* attempt = calloc(1, sizeof *attempt);
    attempt->radio = radio;
    attempt->device = *device;
    HANDLE thread = CreateThread(NULL, 0, run_attempt, attempt, 0, NULL);
    if (WaitForSingleObject(thread, ATTEMPT_MS) != WAIT_OBJECT_0) {
        attempts_left_behind++;
        return WAIT_TIMEOUT;
    }
    CloseHandle(thread);
    DWORD result = attempt->result;
    *device = attempt->device;
    free(attempt);
    return result;
}

static void drop_link(HANDLE radio, const BLUETOOTH_ADDRESS* address) {
    ULONGLONG to = address->ullLong;
    DWORD returned = 0;
    DeviceIoControl(radio, IOCTL_DISCONNECT, &to, sizeof to, NULL, 0, &returned, NULL);
    Sleep(SETTLE_MS);
}

static int pair(HANDLE radio, BLUETOOTH_DEVICE_INFO* device) {
    if (device->fConnected && device->fAuthenticated) {
        say("  It's already paired and connected.\n");
        return 1;
    }
    if (device->fRemembered || device->fAuthenticated) {
        DWORD removed = BluetoothRemoveDevice(&device->Address);
        say("  Removed Windows' old entry for it (it may have been paired with a Switch since): result %lu\n", removed);
        device->fRemembered = device->fAuthenticated = FALSE;
        Sleep(SETTLE_MS);
    }
    if (device->fConnected) {
        drop_link(radio, &device->Address);
        device->fConnected = FALSE;
    }
    DWORD started = GetTickCount();
    DWORD result = limited_attempt(radio, device);
    say("  Pairing: result %lu, %s, %lu ms\n", result, meaning(result), GetTickCount() - started);
    if (result == WAIT_TIMEOUT) drop_link(radio, &device->Address);
    if (!paired(result)) return 0;
    result = BluetoothSetServiceState(radio, device, &HumanInterfaceDeviceServiceClass_UUID, BLUETOOTH_SERVICE_ENABLE);
    say("  Turning on the game device service: result %lu, %s\n", result, meaning(result));
    return 1;
}

/* Pairs every Joy-Con it finds listening. Returns how many were paired. */
static int search_once(HANDLE radio, uint32_t* others, int mode) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search;
    BLUETOOTH_DEVICE_INFO device;
    int done = 0;
    memset(&search, 0, sizeof search);
    search.dwSize = sizeof search;
    search.fReturnAuthenticated = search.fReturnRemembered = TRUE;
    search.fReturnUnknown = search.fReturnConnected = TRUE;
    search.fIssueInquiry = mode != SEARCH_RECORDS;
    search.cTimeoutMultiplier = mode != SEARCH_RECORDS ? INQUIRY_UNITS : 0;
    search.hRadio = radio;
    memset(&device, 0, sizeof device);
    device.dwSize = sizeof device;
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &device);
    if (find == NULL) return 0;
    *others = 0;
    do {
        if (!is_joycon(&device)) {
            (*others)++;
            continue;
        }
        say("%s %ls (paired %d, remembered %d, connected %d)\n", mode == SEARCH_LIST ? "Windows lists" : "Found",
            device.szName, device.fAuthenticated != 0, device.fRemembered != 0, device.fConnected != 0);
        if (mode == SEARCH_LIST) continue;
        /* Records alone can't show a Joy-Con is listening: only one connected now counts from them. */
        if (mode == SEARCH_RECORDS && !device.fConnected) continue;
        if (pair(radio, &device)) done++;
    } while (BluetoothFindNextDevice(find, &device));
    BluetoothFindDeviceClose(find);
    return done;
}

static DWORD take_piped_input(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0, got = 0, total = 0;
    char buffer[64];
    while (PeekNamedPipe(in, NULL, 0, NULL, &available, NULL) && available) {
        if (!ReadFile(in, buffer, available < sizeof buffer ? available : sizeof buffer, &got, NULL) || !got) break;
        total += got;
    }
    return total;
}

static int wait_for_key(int seconds) {
    DWORD until = GetTickCount() + (DWORD)seconds * 1000u;
    if (piped) {
        take_piped_input();
        while ((LONG)(until - GetTickCount()) > 0) {
            if (take_piped_input()) return 1;
            Sleep(20);
        }
        return 0;
    }
    while (_kbhit()) _getch();
    while ((LONG)(until - GetTickCount()) > 0) {
        if (_kbhit()) {
            _getch();
            return 1;
        }
        Sleep(20);
    }
    return 0;
}

static int wait_for_hid(u32 want) {
    for (int waited = 0; waited < HID_WAIT_MS; waited += 1000) {
        if ((u32)joycon_scan_once(NULL, NULL) >= want) return 1;
        Sleep(1000);
    }
    return joycon_scan_once(NULL, NULL) > 0;
}

/* Opens every connected Joy-Con as the game does: lights, a buzz, then A (or the left one's right arrow). */
static int test_joycons(void) {
    u32 left = 0, right = 0;
    joycon_scan_once(&left, &right);
    if (!left && !right) {
        say("No Joy-Con is connected as a game device.\n");
        return 0;
    }
    say("\nConnected: %u left, %u right. Testing...\n", left, right);
    joycon_start(0); /* each Joy-Con on its own, as in the Party games */
    u32 want = left + right, ready = 0;
    for (int waited = 0; waited < READY_WAIT_MS; waited += 100) {
        ready = 0;
        for (int s = 0; s < JOYCON_SLOTS; s++) ready += (u32)joycon_ready_at(s);
        if (ready >= want) break;
        Sleep(100);
    }
    if (!ready) {
        say("The Joy-Con is listed, but it didn't answer. Press a button on it to wake it, then try again.\n");
        return 0;
    }
    say("%u Joy-Con%s answered. Each shows its player light and buzzes now.\n", ready, ready == 1 ? "" : "s");
    for (int s = 0; s < JOYCON_SLOTS; s++) joycon_output_at(s, 1u << s, 1);
    Sleep(400);
    for (int s = 0; s < JOYCON_SLOTS; s++) joycon_output_at(s, 1u << s, 0);
    say("Now press A on the right Joy-Con, or the right arrow on the left one (each one you want to check).\n");
    int pressed[JOYCON_SLOTS] = {0}, motion[JOYCON_SLOTS] = {0};
    u32 presses = 0;
    for (int waited = 0; presses < ready && waited < TEST_MS; waited += 20) {
        for (int s = 0; s < JOYCON_SLOTS; s++) {
            RemoteSample got[64];
            s32 plug;
            u32 n = joycon_take_at(s, got, NULL, 64, &plug, NULL);
            for (u32 i = 0; i < n; i++) {
                /* Lying still or held, the accelerometer feels about 1 g: 104 counts from 512 on some axis. */
                int dx = got[i].accel[0] - 512, dy = got[i].accel[1] - 512, dz = got[i].accel[2] - 512;
                int sq = dx * dx + dy * dy + dz * dz;
                if (sq > 70 * 70 && sq < 140 * 140) motion[s] = 1;
                if ((got[i].buttons & WII_A) && !pressed[s]) {
                    pressed[s] = 1;
                    presses++;
                    say("Player %d's Joy-Con: A pressed. Motion sensor: %s.\n", s + 1, motion[s] ? "working" : "no reading yet");
                }
            }
        }
        Sleep(20);
    }
    joycon_write_report(report);
    if (!presses) say("No A press was seen.\n");
    return presses > 0;
}

static int finish(int code) {
    fflush(stdout);
    if (report) fflush(report);
    if (attempts_left_behind) TerminateProcess(GetCurrentProcess(), (UINT)code);
    return code;
}

int main(int argc, char** argv) {
    const char* log_path = "pair-joycon.txt";
    int seconds = 120, list_only = 0, test_only = 0;
    uint32_t others = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--log")) log_path = argv[i + 1];
        else if (!strcmp(argv[i], "--list")) list_only = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--pipe")) piped = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--test")) test_only = atoi(argv[i + 1]);
    }
    if (piped) key_word = "click \"The lights are moving\" in the launcher";
    report = fopen(log_path, "w");
    if (report == NULL) report = stdout;

    if (!test_only) {
        BLUETOOTH_FIND_RADIO_PARAMS radio_search = {sizeof radio_search};
        BLUETOOTH_RADIO_INFO info;
        HANDLE radio = NULL;
        HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&radio_search, &radio);
        if (find == NULL) {
            say("No Bluetooth adapter found. Check that Bluetooth is turned on in Windows.\n");
            return finish(2);
        }
        BluetoothFindRadioClose(find);
        info.dwSize = sizeof info;
        if (BluetoothGetRadioInfo(radio, &info) != ERROR_SUCCESS) {
            say("Couldn't read the Bluetooth adapter.\n");
            return finish(2);
        }
        say("Bluetooth adapter: %ls\n", info.szName);
        if (list_only) {
            search_once(radio, &others, SEARCH_LIST);
            say("Other Bluetooth devices seen: %u\n", others);
            return finish(0);
        }
        u32 before = (u32)joycon_scan_once(NULL, NULL);
        if (before) say("%u Joy-Con%s already connected. To add another, pair it now too.\n", before, before == 1 ? " is" : "s are");
        say("\n1. Find the small round sync button on the Joy-Con's rail (the side that slides onto a Switch).\n");
        say("2. Hold it until the green lights on the rail move back and forth. Do both Joy-Cons if you have two.\n");
        say("3. Straight away, %s.\n\n", key_word);
        for (int round = 0; !paired_now && round < PAIR_ROUNDS; round++) {
            if (round) say("\nNone was found. If Windows' Bluetooth settings are open, close them. Hold the sync button "
                           "again until the lights move, and %s.\n", key_word);
            if (!wait_for_key(seconds)) {
                say("Nothing was pressed in time, so no pairing was tried.\n");
                break;
            }
            say("Looking for Joy-Cons...\n");
            for (int i = 0; i < SEARCHES_PER_ROUND; i++) {
                int got = search_once(radio, &others, SEARCH_RADIO);
                paired_now += got;
                if (got && i >= 1) break; /* one more search catches the second Joy-Con */
            }
            if (!paired_now) say("No listening Joy-Con was found.\n");
        }
        if (!attempts_left_behind) CloseHandle(radio);
        if (!paired_now && !before) {
            say("\nNo Joy-Con was paired. The details are in %s\n", log_path);
            return finish(1);
        }
        if (paired_now) {
            say("Paired %d. Waiting for Windows to set %s up as a game device...\n", paired_now, paired_now == 1 ? "it" : "them");
            if (!wait_for_hid(before + (u32)paired_now)) {
                say("Paired, but Windows didn't add it as a game device within %d seconds.\n", HID_WAIT_MS / 1000);
                return finish(1);
            }
        }
    }
    if (!test_joycons()) {
        say("\nThe test didn't finish. The details are in %s\n", log_path);
        return finish(1);
    }
    say("\nThe Joy-Cons work. Start the game from the Play tab: they play as Wii Remotes.\n");
    say("Next time, just press a button on a Joy-Con to connect it again (no pairing needed).\n");
    return finish(0);
}
