/* Pairs a Wii Remote with Windows, then checks that it can talk to this PC, with no game running.

   Windows' own pairing screen often fails here. A Wii Remote uses old-style PIN pairing, and its PIN is six
   raw bytes of a Bluetooth address, which can't be typed. After the SYNC button it is the PC's address, and
   after 1+2 it is the remote's own. Both are stored lowest byte first in BLUETOOTH_ADDRESS, the order the
   remote wants.

   Windows keeps a record of a remote it once discovered, and returns it from a search even while the
   remote is asleep. It also stamps that record as seen at the end of every search, so nothing Windows reports
   shows that the remote is listening. The player says so instead, with a key press after SYNC. A remote only
   listens for about 20 seconds, so no pairing is tried before that key.

   Windows can also hold a link to the remote with no pairing behind it (its record says connected, not
   paired), left by a pairing that didn't finish or by a remote reconnecting to a PC that forgot it. A PIN sent
   over that link hung for the whole attempt on 2026-11-08, and the hung call then made every later attempt fail,
   so such a link is dropped first, and again after any attempt that runs out of time. If the PIN still isn't
   taken, the remote is connected without one, the way Dolphin does it: that works until the remote turns off.

   Usage: PairRemote.exe [--seconds 120] [--log analysis/native/pair-remote.txt] [--list 1] [--pipe 1]
   --seconds is how long to wait for the key.
   --list only reports what Windows returns, and can't change anything.
   --pipe: run from the launcher, which shows what this prints and sends a line instead of a key press.
   Only entries named as Wii Remotes are ever changed. Other devices are counted, not recorded. */
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
#include "real_remote.h"

#define INQUIRY_UNITS 2      /* 1.28 seconds each. A remote stays findable for about 20 seconds */
#define HID_WAIT_MS 30000    /* Windows sets up the driver after pairing */
#define CONNECT_WAIT_MS 5000
#define TEST_MS 20000
#define ATTEMPT_MS 6000      /* a pairing call can hang forever; a working one took 0.3 s here */
#define SETTLE_MS 1000       /* after dropping a link */
#define PIN_SIZE 6
#define PAIR_ROUNDS 3
#define SEARCHES_PER_ROUND 4
#define CLASS_REMOTE 0x002504u
#define CLASS_REMOTE_PLUS 0x000508u
#define BUTTON_A 0x08
#define REPORT_STATUS 0x20
#define REPORT_MEMORY 0x21
#define REPORT_BUTTONS 0x30

/* IOCTL_BTH_DISCONNECT_DEVICE from the SDK's bthioctl.h (not in MinGW): drops Windows' link to an address. */
#define IOCTL_DISCONNECT CTL_CODE(0x00000041, 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum { SEARCH_RECORDS, SEARCH_RADIO, SEARCH_LIST };
enum { NOT_FOUND, PAIR_FAILED, PAIR_WORKED };

FILE* report; /* real_remote.c logs here */
static int piped;    /* --pipe: the "key" is a line on standard input, from the launcher's button */
static int without_pin; /* connected the Dolphin way, with no pairing: it lasts until the remote turns off */
static const char* key_word = "press any key in this window";

static void say(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    if (report == NULL) return;
    va_start(args, format);
    vfprintf(report, format, args);
    va_end(args);
    fflush(report);
}

static const char* meaning(DWORD code) {
    switch (code) {
    case ERROR_SUCCESS: return "worked";
    case ERROR_NO_MORE_ITEMS: return "already paired";
    case ERROR_CANCELLED: return "cancelled, the remote may have stopped listening";
    case ERROR_NOT_AUTHENTICATED: return "the remote refused this PIN";
    case ERROR_DEVICE_NOT_CONNECTED: return "the remote isn't reachable";
    case ERROR_TIMEOUT: case WAIT_TIMEOUT: return "timed out";
    case ERROR_INVALID_PARAMETER: return "Windows rejected the request";
    case ERROR_GEN_FAILURE: return "the Bluetooth driver reported a failure";
    case ERROR_SERVICE_DOES_NOT_EXIST: return "Windows found no game device service on it";
    case ERROR_ACCESS_DENIED: return "access denied";
    case ERROR_NOT_FOUND: return "Windows has no record of it";
    case ERROR_INVALID_FUNCTION: return "the adapter's driver can't do this";
    default: return "unknown";
    }
}

static int paired(DWORD code) { return code == ERROR_SUCCESS || code == ERROR_NO_MORE_ITEMS; }

static int is_remote(const BLUETOOTH_DEVICE_INFO* device) {
    uint32_t kind = device->ulClassofDevice & 0xFFFFFFu;
    if (wcsncmp(device->szName, L"Nintendo RVL-CNT-01", 19) == 0) return 1;
    return device->szName[0] == 0 && (kind == CLASS_REMOTE || kind == CLASS_REMOTE_PLUS);
}

typedef struct {
    HANDLE radio;
    BLUETOOTH_DEVICE_INFO device;
    BYTE pin[PIN_SIZE];
    int raw;
    DWORD result;
} Attempt;

/* Sends the PIN as raw bytes, for the case where Windows mangles it in the plain call below. Says what Windows
   asked for, which tells a remote that never answered apart from one that refused. */
static BOOL CALLBACK answer_pin(void* param, BLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS* request) {
    Attempt* attempt = param;
    BLUETOOTH_AUTHENTICATE_RESPONSE response;
    memset(&response, 0, sizeof response);
    response.bthAddressRemote = request->deviceInfo.Address;
    response.authMethod = request->authenticationMethod;
    int legacy = request->authenticationMethod == BLUETOOTH_AUTHENTICATION_METHOD_LEGACY;
    if (legacy) {
        memcpy(response.pinInfo.pin, attempt->pin, PIN_SIZE);
        response.pinInfo.pinLength = PIN_SIZE;
    } else {
        response.negativeResponse = 1;
    }
    DWORD sent = BluetoothSendAuthenticationResponseEx(attempt->radio, &response);
    say("    Windows asked for %s (method %d); answer sent: result %lu, %s\n", legacy ? "the PIN" : "a passkey, refused",
        (int)request->authenticationMethod, sent, meaning(sent));
    return TRUE;
}

/* Drops Windows' link to the remote, which also ends a pairing call stuck on it. */
static void drop_link(HANDLE radio, const BLUETOOTH_ADDRESS* address, const char* why) {
    ULONGLONG to = address->ullLong;
    DWORD returned = 0;
    DWORD result = DeviceIoControl(radio, IOCTL_DISCONNECT, &to, sizeof to, NULL, 0, &returned, NULL) ? ERROR_SUCCESS : GetLastError();
    say("  Dropped Windows' link to it (%s): result %lu, %s\n", why, result,
        result == ERROR_DEVICE_NOT_CONNECTED ? "there was none" : meaning(result));
    Sleep(SETTLE_MS);
}

static DWORD send_raw_pin(Attempt* attempt) {
    HBLUETOOTH_AUTHENTICATION_REGISTRATION registration = NULL;
    DWORD result = BluetoothRegisterForAuthenticationEx(&attempt->device, &registration, answer_pin, attempt);
    if (result != ERROR_SUCCESS) return result;
    result = BluetoothAuthenticateDeviceEx(NULL, attempt->radio, &attempt->device, NULL, MITMProtectionNotRequired);
    BluetoothUnregisterAuthentication(registration);
    return result;
}

static DWORD WINAPI run_attempt(void* param) {
    Attempt* attempt = param;
    WCHAR wide[PIN_SIZE];
    if (attempt->raw) {
        attempt->result = send_raw_pin(attempt);
        return 0;
    }
    for (int i = 0; i < PIN_SIZE; i++) wide[i] = attempt->pin[i];
    attempt->result = BluetoothAuthenticateDevice(NULL, attempt->radio, &attempt->device, wide, PIN_SIZE);
    return 0;
}

/* These calls have no timeout of their own. One that overruns is left behind with its memory, because
   Windows may still write to it, and the process ends soon after anyway. While one is left behind, closing
   the radio blocks until it returns, which can be never, so the radio is then left open and the process
   ends itself (finish). */
static int attempts_left_behind;

static DWORD limited_attempt(HANDLE radio, BLUETOOTH_DEVICE_INFO* device, const BYTE* pin, int raw) {
    Attempt* attempt = calloc(1, sizeof *attempt);
    attempt->radio = radio;
    attempt->device = *device;
    attempt->raw = raw;
    memcpy(attempt->pin, pin, PIN_SIZE);
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

static int try_pin(HANDLE radio, BLUETOOTH_DEVICE_INFO* device, const BYTE* pin, const char* label) {
    DWORD started = GetTickCount();
    DWORD result = limited_attempt(radio, device, pin, 0);
    say("  %s PIN: result %lu, %s, %lu ms\n", label, result, meaning(result), GetTickCount() - started);
    if (paired(result)) return 1;
    if (result == WAIT_TIMEOUT) drop_link(radio, &device->Address, "the call was stuck");
    else Sleep(500);
    started = GetTickCount();
    result = limited_attempt(radio, device, pin, 1);
    say("  %s PIN as raw bytes: result %lu, %s, %lu ms\n", label, result, meaning(result), GetTickCount() - started);
    if (result == WAIT_TIMEOUT) drop_link(radio, &device->Address, "the call was stuck");
    return paired(result);
}

static int pair(HANDLE radio, const BLUETOOTH_RADIO_INFO* info, BLUETOOTH_DEVICE_INFO* device) {
    if (device->fRemembered || device->fAuthenticated) {
        /* It isn't working as a game device, or the HID check would have found it. Start clean. */
        DWORD removed = BluetoothRemoveDevice(&device->Address);
        say("Removed the old Wii Remote entry: result %lu\n", removed);
        device->fRemembered = device->fAuthenticated = FALSE;
    }
    if (device->fConnected) { /* a link with nothing behind it; a PIN sent over it hangs */
        drop_link(radio, &device->Address, "it was connected but not paired");
        device->fConnected = FALSE;
    }
    if (try_pin(radio, device, info->address.rgBytes, "SYNC button")) {
        DWORD result = BluetoothSetServiceState(radio, device, &HumanInterfaceDeviceServiceClass_UUID,
                                                BLUETOOTH_SERVICE_ENABLE);
        say("Turning on the game device service: result %lu, %s\n", result, meaning(result));
        return 1;
    }
    /* The PIN wasn't taken. Connect without pairing, as Dolphin does: the remote is still listening. */
    DWORD result = BluetoothSetServiceState(radio, device, &HumanInterfaceDeviceServiceClass_UUID,
                                            BLUETOOTH_SERVICE_ENABLE);
    say("  Connecting without a PIN: result %lu, %s\n", result, meaning(result));
    if (result != ERROR_SUCCESS) return 0;
    without_pin = 1;
    return 1;
}

/* SEARCH_RECORDS reads what Windows already knows, which is quickest. The others search over the air. */
static int search_once(HANDLE radio, const BLUETOOTH_RADIO_INFO* info, uint32_t* others, int mode) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search;
    BLUETOOTH_DEVICE_INFO device;
    int outcome = NOT_FOUND;
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
    if (find == NULL) return NOT_FOUND;
    *others = 0;
    do {
        if (!is_remote(&device)) {
            (*others)++;
            continue;
        }
        say("%s %ls (class %06lX, paired %d, remembered %d, connected %d)\n", mode == SEARCH_LIST ? "Windows lists" : "Found",
            device.szName[0] ? device.szName : L"a Wii Remote", device.ulClassofDevice & 0xFFFFFFul,
            device.fAuthenticated != 0, device.fRemembered != 0, device.fConnected != 0);
        if (mode != SEARCH_LIST) outcome = pair(radio, info, &device) ? PAIR_WORKED : PAIR_FAILED;
    } while (outcome == NOT_FOUND && BluetoothFindNextDevice(find, &device));
    BluetoothFindDeviceClose(find);
    return outcome;
}

/* With --pipe: anything waiting on standard input counts as the key. Returns how many bytes it took. */
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
        take_piped_input(); /* a click from before doesn't count */
        while ((LONG)(until - GetTickCount()) > 0) {
            if (take_piped_input()) return 1;
            Sleep(20);
        }
        return 0;
    }
    while (_kbhit()) _getch(); /* a key pressed earlier doesn't count */
    while ((LONG)(until - GetTickCount()) > 0) {
        if (_kbhit()) {
            _getch();
            return 1;
        }
        Sleep(20);
    }
    return 0;
}

static int wait_for_hid(void) {
    uint32_t devices = 0;
    for (int waited = 0; waited < HID_WAIT_MS; waited += 1000) {
        if (real_remote_scan_once(&devices)) return 1;
        Sleep(1000);
    }
    return 0;
}

/* LED 1, a short rumble, then reports. Proves writing and reading both work over this adapter. */
static int test_remote(void) {
    static const uint8_t led[] = {0x11, 0x10};
    static const uint8_t rumble_on[] = {0x10, 0x01};
    static const uint8_t rumble_off[] = {0x10, 0x00};
    static const uint8_t status[] = {0x15, 0x00};
    static const uint8_t buttons[] = {0x12, 0x00, REPORT_BUTTONS};
    static const uint8_t motion_plus[] = {0x17, 0x04, 0xA6, 0x00, 0xFA, 0x00, 0x06};
    uint8_t in[REMOTE_REPORT_SIZE];
    uint32_t count = 0;
    int heard_status = 0, heard_memory = 0, pressed_a = 0;

    real_remote_start();
    for (int waited = 0; !real_remote_connected() && waited < CONNECT_WAIT_MS; waited += 50) Sleep(50);
    if (!real_remote_connected()) {
        say("The remote is listed, but it couldn't be opened.\n");
        return 0;
    }
    say("\nTesting. LED 1 should light and the remote should buzz briefly.\n");
    real_remote_write(led, sizeof led);
    real_remote_write(rumble_on, sizeof rumble_on);
    Sleep(300);
    real_remote_write(rumble_off, sizeof rumble_off);
    real_remote_write(buttons, sizeof buttons);
    real_remote_write(status, sizeof status);
    real_remote_write(motion_plus, sizeof motion_plus);
    say("Now press A on the remote.\n");

    for (int waited = 0; !pressed_a && waited < TEST_MS;) {
        if (!real_remote_read(in, NULL)) {
            Sleep(5);
            waited += 5;
            continue;
        }
        count++;
        if (in[0] == REPORT_STATUS && !heard_status) {
            heard_status = 1;
            say("Remote status: battery %u of 200, extension plugged in: %s\n", in[6], in[3] & 2 ? "yes" : "no");
        } else if (in[0] == REPORT_MEMORY && !heard_memory) {
            heard_memory = 1;
            say("MotionPlus: %s\n", (in[3] & 0x0F) == 0 && in[11] == 0x05 ? "found" : "not found");
        } else if (in[0] == REPORT_BUTTONS && (in[2] & BUTTON_A)) {
            pressed_a = 1;
        }
    }
    say("Reports heard: %u, A pressed: %s\n", count, pressed_a ? "yes" : "no");
    real_remote_write_report();
    fflush(report);
    return count != 0 && pressed_a;
}

static void close_radio(HANDLE radio) {
    if (!attempts_left_behind) CloseHandle(radio);
}

/* Ends the program. With a pairing call still hanging, ending normally could wait on it forever. */
static int finish(int code) {
    fflush(stdout);
    if (report) fflush(report);
    if (attempts_left_behind) TerminateProcess(GetCurrentProcess(), (UINT)code);
    return code;
}

int main(int argc, char** argv) {
    const char* log_path = "analysis/native/pair-remote.txt";
    int seconds = 120, list_only = 0;
    uint32_t others = 0, devices = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--log")) log_path = argv[i + 1];
        else if (!strcmp(argv[i], "--list")) list_only = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--pipe")) piped = atoi(argv[i + 1]);
    }
    if (piped) key_word = "click \"I pressed SYNC\" in the launcher";
    report = fopen(log_path, "w");
    if (report == NULL) report = stderr;

    int have_hid = real_remote_scan_once(&devices);
    if (have_hid) say("A Wii Remote is already connected as a game device.\n");
    if (have_hid && !list_only) {
        say("Skipping pairing.\n");
    } else {
        BLUETOOTH_FIND_RADIO_PARAMS radio_search = {sizeof radio_search};
        BLUETOOTH_RADIO_INFO info;
        HANDLE radio = NULL;
        HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&radio_search, &radio);
        if (find == NULL) {
            say("No Bluetooth adapter found. Check that Bluetooth is turned on in Windows.\n");
            return 2;
        }
        BluetoothFindRadioClose(find);
        info.dwSize = sizeof info;
        DWORD result = BluetoothGetRadioInfo(radio, &info);
        if (result != ERROR_SUCCESS) {
            say("Couldn't read the Bluetooth adapter: result %lu\n", result);
            return 2;
        }
        say("Bluetooth adapter: %ls\n", info.szName);
        if (list_only) {
            DWORD until = GetTickCount() + (DWORD)seconds * 1000u;
            while ((LONG)(until - GetTickCount()) > 0) search_once(radio, &info, &others, SEARCH_LIST);
            say("Other Bluetooth devices seen: %u\n", others);
            close_radio(radio);
            return finish(0);
        }
        say("\n1. Open the battery cover on the back of the remote.\n");
        say("2. Press the small red SYNC button once. The four lights should start blinking.\n");
        say("3. Straight away, %s.\n\n", key_word);
        int done = 0;
        for (int round = 0; !done && round < PAIR_ROUNDS; round++) {
            if (round) say("\nThat didn't work. If Windows' Bluetooth settings are open, close them. Then press SYNC again, and %s.\n", key_word);
            if (!wait_for_key(seconds)) {
                say("Nothing was pressed in time, so no pairing was tried.\n");
                break;
            }
            say("Pairing...\n");
            int outcome = search_once(radio, &info, &others, SEARCH_RECORDS);
            for (int i = 0; outcome == NOT_FOUND && i < SEARCHES_PER_ROUND; i++) {
                outcome = search_once(radio, &info, &others, SEARCH_RADIO);
            }
            if (outcome == NOT_FOUND) say("The remote wasn't found.\n");
            done = outcome == PAIR_WORKED;
        }
        close_radio(radio);
        if (!done) {
            say("\nNo Wii Remote was paired. The details are in %s\n", log_path);
            return finish(1);
        }
        say(without_pin ? "Connected. Waiting for Windows to set it up as a game device...\n"
                        : "Paired. Waiting for Windows to set it up as a game device...\n");
        if (!wait_for_hid()) {
            say("Paired, but Windows didn't add it as a game device within %d seconds.\n", HID_WAIT_MS / 1000);
            return finish(1);
        }
    }
    if (!test_remote()) {
        say("\nThe test didn't finish. The details are in %s\n", log_path);
        return finish(1);
    }
    say("\nThe Wii Remote works. %s\n", piped ? "Start the game from the Play tab." : "Start the game with Run Native.cmd.");
    if (without_pin)
        say("It's connected without pairing, so after it turns off, connect it here again the same way.\n");
    return finish(0);
}
