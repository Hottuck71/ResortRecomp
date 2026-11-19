/* Online play: lockstep, like Dolphin's netplay. Every PC runs the same game from the same save, and only input
   travels. Each video field, every PC sends its player's input for a field NET_DELAY ahead, then waits until it
   has every player's input for the current field and hands them all to the stand-in remotes (fake_remote.c). So
   every PC's game sees the same remotes at the same moments, and stays the same as long as the game runs the same
   given the same input. A checksum of guest memory every CHECK_EVERY fields catches the moment it doesn't.

   The host is player 1. Players connect to it over TCP, directly or through a relay server (native/relay), and
   the host passes every message on to the others. Messages are
   a fixed header and a payload: HELLO (protocol and build), WELCOME (player number and count), FILE and START
   (the host's save, before the game starts), INPUT, CHECK.

   Every game must start from the same save, or they differ from the first frame: the host sends its save folder,
   and each player writes it into "<their save folder>-online" and plays from that copy. Their own save isn't
   touched. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "net_status.h"
#include "aurora_glue.h"

#define NET_DELAY 6u     /* fields of room for the network, until the host has measured it */
#define MIN_DELAY 4u
#define MAX_DELAY 15u    /* 250 ms: more than this and the controls feel late */
#define STATS_EVERY 120u /* fields between each PC's speed report: two seconds */
#define RING 64u         /* fields of inputs kept */
#define CHECK_EVERY 120u /* fields between memory checksums: two seconds */
#define MAX_PLAYERS 4u
#define PROTOCOL 4u    /* 3: a host says why it turns a player away; 4: measured delay, speed reports */
#define RELAY_PORT 51020u
#define WAIT_NOTE_MS 2000u

enum { MSG_HELLO = 1, MSG_WELCOME, MSG_INPUT, MSG_CHECK, MSG_FILE, MSG_START, MSG_CALIBRATIONS, MSG_REFUSED, MSG_PING, MSG_PONG, MSG_STATS };
#define MAX_FILE (16u << 20)

typedef struct {
    u32 type, field, player, size;
} Header;

typedef struct {
    u32 protocol;
    char build[32];
    RemoteCalibration cal; /* the player's real remote, if any */
} Hello;

extern FILE* report;

static SOCKET peers[MAX_PLAYERS]; /* the host's: one per player (its own unused); a player's: [0] is the host */
static int is_host, active;
static u32 me, players;
static RemoteInput inputs[RING][MAX_PLAYERS];
static u32 input_field[RING][MAX_PLAYERS]; /* which field each slot holds, plus one */
static u64 checks[RING][MAX_PLAYERS];
static u32 check_field[RING][MAX_PLAYERS];
static u32 gone[MAX_PLAYERS];
static RemoteCalibration cals[MAX_PLAYERS]; /* every player's real remote, from when they joined */
static u64 waited_ms, desyncs, fields_done;
static u32 first_desync;
static u32 delay = NET_DELAY;     /* the same on every PC: the host sends it with START */
static double round_trip_ms[MAX_PLAYERS]; /* the host's measurement of each player, before the game */
/* Speed reports: each PC's time waiting for the others, over its last STATS_EVERY fields, and how long those
   fields took. The PC that waits least while the others wait is the one holding everyone back. */
static u32 stat_wait[MAX_PLAYERS], stat_time[MAX_PLAYERS], stat_have[MAX_PLAYERS];
static u64 stat_total_wait[MAX_PLAYERS], stat_total_time[MAX_PLAYERS];
/* Players must run the same game code. A toolkit build (native/builder/game.mk) is compiled on each player's own PC,
   so it names its toolkit version instead of the time it was compiled. */
#ifdef RR_BUILD_ID
static const char* build_id = RR_BUILD_ID;
#else
static const char* build_id = __DATE__ " " __TIME__;
#endif

static int send_all(SOCKET s, const void* data, int size) {
    const char* p = data;
    while (size > 0) {
        int done = send(s, p, size, 0);
        if (done <= 0) return 0;
        p += done;
        size -= done;
    }
    return 1;
}

static int recv_all(SOCKET s, void* data, int size) {
    char* p = data;
    while (size > 0) {
        int done = recv(s, p, size, 0);
        if (done <= 0) return 0;
        p += done;
        size -= done;
    }
    return 1;
}

static int send_message(SOCKET s, u32 type, u32 field, u32 player, const void* payload, u32 size) {
    Header h = {type, field, player, size};
    return send_all(s, &h, sizeof h) && (!size || send_all(s, payload, (int)size));
}

static void tune(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

static int winsock(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}


/* ---- The host's save, sent before the game starts ---- */

static u32 files_sent, bytes_sent;
static int wait_readable(SOCKET s, u32 ms);

/* Sends every file under root (path relative to it, with forward slashes) to one player. */
static int send_folder(SOCKET s, const char* root, const char* relative) {
    char pattern[600];
    WIN32_FIND_DATAA found;
    snprintf(pattern, sizeof pattern, "%s/%s*", root, relative);
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        char inner[600];
        snprintf(inner, sizeof inner, "%s%s", relative, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            strncat(inner, "/", sizeof inner - strlen(inner) - 1);
            ok = send_folder(s, root, inner);
            continue;
        }
        char path[700];
        snprintf(path, sizeof path, "%s/%s", root, inner);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 0 || (u32)size > MAX_FILE) {
            fclose(f);
            continue;
        }
        u32 name = (u32)strlen(inner);
        u8* message = malloc(name + (u32)size);
        if (!message) {
            fclose(f);
            return 0;
        }
        memcpy(message, inner, name);
        size_t got = fread(message + name, 1, (size_t)size, f);
        fclose(f);
        Header h = {MSG_FILE, (u32)got, name, name + (u32)got};
        ok = send_all(s, &h, sizeof h) && send_all(s, message, (int)(name + got));
        free(message);
        files_sent++;
        bytes_sent += (u32)got;
    } while (ok && FindNextFileA(find, &found));
    FindClose(find);
    return ok;
}

/* Empties a folder this program made, file by file. */
static void empty_folder(const char* folder) {
    char pattern[600];
    WIN32_FIND_DATAA found;
    snprintf(pattern, sizeof pattern, "%s/*", folder);
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        char path[700];
        snprintf(path, sizeof path, "%s/%s", folder, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            empty_folder(path);
            RemoveDirectoryA(path);
        } else {
            DeleteFileA(path);
        }
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

static void make_parents(char* path) {
    for (char* p = path; *p; p++) {
        if (*p != '/' || p == path) continue;
        *p = 0;
        _mkdir(path);
        *p = '/';
    }
}

/* A player: takes the host's save into folder, until START. Paths can't climb out of it. */
static int take_save(SOCKET s, const char* folder) {
    u32 files = 0, bytes = 0;
    _mkdir(folder);
    empty_folder(folder);
    for (;;) {
        Header h;
        if (wait_readable(s, 0) <= 0 || !recv_all(s, &h, sizeof h)) return 0; /* the host may wait for others */
        if (h.type == MSG_START) {
            if (h.field >= 2 && h.field <= 30) delay = h.field;
            break;
        }
        if (h.type == MSG_PING && h.size == sizeof(u32)) {
            u32 value;
            if (!recv_all(s, &value, sizeof value) || !send_message(s, MSG_PONG, h.field, 0, &value, sizeof value)) return 0;
            continue;
        }
        if (h.type == MSG_CALIBRATIONS && h.player <= MAX_PLAYERS && h.size == h.player * sizeof(RemoteCalibration)) {
            if (!recv_all(s, cals, (int)h.size)) return 0;
            continue;
        }
        if (h.type != MSG_FILE || h.player == 0 || h.player > 400 || h.size != h.player + h.field || h.field > MAX_FILE)
            return 0;
        u8* message = malloc(h.size + 1);
        if (!message || !recv_all(s, message, (int)h.size)) {
            free(message);
            return 0;
        }
        char name[401];
        memcpy(name, message, h.player);
        name[h.player] = 0;
        if (strstr(name, "..") || name[0] == '/' || strchr(name, ':') || strchr(name, '\\')) {
            free(message);
            return 0;
        }
        char path[1000];
        snprintf(path, sizeof path, "%s/%s", folder, name);
        make_parents(path);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(message + h.player, 1, h.field, f);
            fclose(f);
        }
        free(message);
        files++;
        bytes += h.field;
    }
    fprintf(report, "Online: took the host's save, %u files, %u bytes, into %s; input delay %u fields\n", files, bytes,
            folder, delay);
    fflush(report);
    return 1;
}

/* ---- Getting everyone together ----
   Every wait here gives up after a while or when the player presses Cancel in the status window, and every
   failure leaves a sentence for that window (net_last_error) and the report. */

static char error_text[600];

static int fail(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(error_text, sizeof error_text, format, args);
    va_end(args);
    fprintf(report, "Online: %s\n", error_text);
    fflush(report);
    return -1;
}

const char* net_last_error(void) { return error_text; }

static int cancelled(void) {
    if (!net_status_cancelled()) return 0;
    fail("Cancelled.");
    return 1;
}

/* Waits until a socket can be read, up to ms (0: no limit). 1 ready, 0 timed out, -1 cancelled. */
static int wait_readable(SOCKET s, u32 ms) {
    DWORD started = GetTickCount();
    for (;;) {
        if (net_status_cancelled()) return -1;
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(s, &ready);
        struct timeval step = {0, 250000};
        if (select(0, &ready, NULL, NULL, &step) > 0) return 1;
        if (ms && GetTickCount() - started >= ms) return 0;
    }
}

/* Connects within ms, or gives up. */
static SOCKET connect_to(const struct addrinfo* where, u32 ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
    connect(s, where->ai_addr, (int)where->ai_addrlen);
    DWORD started = GetTickCount();
    for (;;) {
        fd_set done, failed;
        FD_ZERO(&done);
        FD_ZERO(&failed);
        FD_SET(s, &done);
        FD_SET(s, &failed);
        struct timeval step = {0, 250000};
        int got = select(0, NULL, &done, &failed, &step);
        if (got > 0 && FD_ISSET(s, &done)) break;
        if ((got > 0 && FD_ISSET(s, &failed)) || net_status_cancelled() || GetTickCount() - started >= ms) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    on = 0;
    ioctlsocket(s, FIONBIO, &on);
    tune(s);
    return s;
}

/* "name:port" (or "name", with fallback_port) to an address. */
static int resolve(const char* text, u32 fallback_port, struct addrinfo** found, char* shown, size_t shown_size) {
    char host[128], port[16];
    const char* colon = strrchr(text, ':');
    size_t length = colon ? (size_t)(colon - text) : strlen(text);
    if (!length || length >= sizeof host) return 0;
    memcpy(host, text, length);
    host[length] = 0;
    snprintf(port, sizeof port, "%s", colon ? colon + 1 : "");
    if (!colon) snprintf(port, sizeof port, "%u", fallback_port);
    snprintf(shown, shown_size, "%s:%s", host, port);
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    return getaddrinfo(host, port, &hints, found) == 0 && *found;
}

/* Reads one line of the relay's (up to size - 1 characters), within ms. */
static int read_line(SOCKET s, char* out, u32 size, u32 ms) {
    u32 n = 0;
    out[0] = 0;
    while (n + 1 < size) {
        if (wait_readable(s, ms) <= 0) return 0;
        char ch;
        if (recv(s, &ch, 1, 0) != 1) return 0;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        out[n++] = ch;
    }
    out[n] = 0;
    return 1;
}

static int send_text(SOCKET s, const char* text) { return send_all(s, text, (int)strlen(text)); }

/* The host lets one player in: their HELLO, then WELCOME, or a refusal they can show. 1 if they're in. */
static int admit(SOCKET s, u32 player, u32 count) {
    Header h;
    Hello hello;
    DWORD limit = 15000; /* a connection that doesn't say hello in time isn't a player */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&limit, sizeof limit);
    int ok = recv_all(s, &h, sizeof h) && h.type == MSG_HELLO && h.size == sizeof hello && recv_all(s, &hello, sizeof hello);
    limit = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&limit, sizeof limit);
    if (!ok) {
        fprintf(report, "Online: a connection that wasn't a player of this game was closed\n");
        return 0;
    }
    if (hello.protocol != PROTOCOL || strncmp(hello.build, build_id, sizeof hello.build)) {
        char why[220];
        snprintf(why, sizeof why, "the host has a different version of the game build (theirs: %.32s, "
                 "yours: %.32s). Everyone needs the same copy.", build_id, hello.build);
        send_message(s, MSG_REFUSED, 0, 0, why, (u32)strlen(why));
        fprintf(report, "Online: turned away a player with a different build (%.32s)\n", hello.build);
        return 0;
    }
    u32 welcome[2] = {player, count};
    if (!send_message(s, MSG_WELCOME, 0, player, welcome, sizeof welcome)) return 0;
    peers[player] = s;
    cals[player] = hello.cal;
    fprintf(report, "Online: player %u joined, %s\n", player + 1, hello.cal.present ? "with a Wii Remote" : "on the keyboard");
    fflush(report);
    return 1;
}

static double now_ms(void) {
    static LARGE_INTEGER rate;
    LARGE_INTEGER t;
    if (!rate.QuadPart) QueryPerformanceFrequency(&rate);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)rate.QuadPart;
}

/* Five pings to each player; the middle one counts. The input delay has to cover the slowest path an input
   takes (host to player, or player to host to another player) with room for the network's ups and downs. */
static void choose_delay(u32 count) {
    net_status("Everyone's here. Measuring the connection...");
    double half[MAX_PLAYERS] = {0};
    for (u32 p = 1; p < count; p++) {
        double rtt[5];
        for (u32 i = 0; i < 5; i++) {
            double started = now_ms();
            u32 value = i;
            Header h;
            u32 back;
            rtt[i] = 500.0; /* no answer counts as slow */
            if (send_message(peers[p], MSG_PING, i, 0, &value, sizeof value) && wait_readable(peers[p], 3000) > 0 &&
                recv_all(peers[p], &h, sizeof h) && h.type == MSG_PONG && h.size == sizeof back &&
                recv_all(peers[p], &back, sizeof back))
                rtt[i] = now_ms() - started;
        }
        for (u32 a = 0; a < 5; a++) /* sort, for the middle one */
            for (u32 b = a + 1; b < 5; b++)
                if (rtt[b] < rtt[a]) { double t = rtt[a]; rtt[a] = rtt[b]; rtt[b] = t; }
        round_trip_ms[p] = rtt[2];
        half[p] = rtt[2] / 2.0;
        fprintf(report, "Online: player %u's round trip: %.0f ms (fastest %.0f, slowest %.0f)\n", p + 1, rtt[2], rtt[0], rtt[4]);
    }
    double first = 0, second = 0;
    for (u32 p = 1; p < count; p++) {
        if (half[p] > first) { second = first; first = half[p]; }
        else if (half[p] > second) second = half[p];
    }
    double path = count > 2 ? first + second : first;
    u32 fields = (u32)((path + 25.0) / 16.683) + 2; /* 25 ms for the network's ups and downs, and a field to work in */
    delay = fields < MIN_DELAY ? MIN_DELAY : fields > MAX_DELAY ? MAX_DELAY : fields;
    fprintf(report, "Online: input delay %u fields (%.0f ms) for a %.0f ms path\n", delay, delay * 16.683, path);
    fflush(report);
}

/* Once everyone's in: everyone's remotes, then this PC's save for all. */
static int host_send_save(u32 count, const char* save) {
    choose_delay(count);
    net_status("Everyone's here. Sending your save to the other players...");
    for (u32 player = 1; player < count; player++) {
        if (!send_message(peers[player], MSG_CALIBRATIONS, 0, count, cals, count * (u32)sizeof(RemoteCalibration)) ||
            !send_folder(peers[player], save, "") || !send_message(peers[player], MSG_START, delay, 0, NULL, 0))
            return fail("Player %u's connection dropped while your save was being sent to them.", player + 1);
    }
    fprintf(report, "Online: sent the save, %u files, %u bytes, to each player\n", files_sent / (count - 1),
            bytes_sent / (count - 1));
    is_host = 1;
    me = 0;
    players = count;
    active = 1;
    return 0;
}

static const char* who_joined(u32 joined, u32 wanted) {
    static char text[64];
    snprintf(text, sizeof text, "%u of %u other player%s %s joined", joined, wanted, wanted == 1 ? "" : "s",
             joined == 1 ? "has" : "have");
    return text;
}

int net_start_host(u32 port, u32 count, const char* save, const RemoteCalibration* mine) {
    if (count < 2 || count > MAX_PLAYERS) return fail("A room is for 2 to 4 players.");
    if (!winsock()) return fail("Windows networking didn't start.");
    cals[0] = *mine;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in at = {0};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons((u16)port);
    if (bind(listener, (struct sockaddr*)&at, sizeof at) || listen(listener, 4))
        return fail("This PC couldn't take connections on TCP port %u (error %d). Another program, or another "
                    "copy of the game, may be using it: close it or pick another port.", port, WSAGetLastError());
    fprintf(report, "Online: hosting on port %u, waiting for %u players\n", port, count - 1);
    fflush(report);
    for (u32 player = 1; player < count;) {
        net_status("Hosting on TCP port %u. %s.\n\nGive the others your room code from the launcher. Players outside "
                   "your home network need the internet code, and TCP port %u forwarded on your router to this PC, "
                   "with the game allowed through Windows Firewall. A relay server avoids all of that.\n\nCancel "
                   "stops hosting.", port, who_joined(player - 1, count - 1), port);
        int ready = wait_readable(listener, 0);
        if (ready < 0) {
            closesocket(listener);
            return cancelled() ? -1 : fail("Cancelled.");
        }
        SOCKET s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        tune(s);
        if (admit(s, player, count)) player++;
        else closesocket(s);
    }
    closesocket(listener);
    return host_send_save(count, save);
}

int net_start_host_relay(const char* relay, const char* room, u32 count, const char* save, const RemoteCalibration* mine) {
    if (count < 2 || count > MAX_PLAYERS) return fail("A room is for 2 to 4 players.");
    if (!winsock()) return fail("Windows networking didn't start.");
    cals[0] = *mine;
    struct addrinfo* where = NULL;
    char shown[160], line[160];
    if (!resolve(relay, RELAY_PORT, &where, shown, sizeof shown))
        return fail("The relay server's address \"%s\" couldn't be looked up.", relay);
    net_status("Opening room %s on the relay %s...", room, shown);
    SOCKET control = connect_to(where, 10000);
    if (control == INVALID_SOCKET) {
        freeaddrinfo(where);
        return cancelled() ? -1 : fail("Couldn't reach the relay server at %s. Check its address in the launcher, "
                                       "that wsr_relay is running there, and that its TCP port is forwarded.", shown);
    }
    snprintf(line, sizeof line, "WSRR1 HOST %s\n", room);
    if (!send_text(control, line) || !read_line(control, line, sizeof line, 10000) || strcmp(line, "OK")) {
        closesocket(control);
        freeaddrinfo(where);
        return cancelled() ? -1 : fail("The relay didn't open the room: %s", line[0] == 'E' ? line + 4 : "no answer.");
    }
    fprintf(report, "Online: room %s open on the relay %s, waiting for %u players\n", room, shown, count - 1);
    fflush(report);
    for (u32 player = 1; player < count;) {
        net_status("Room %s is open on the relay %s. %s.\n\nGive the others the room code %s. They need the same "
                   "relay server address in their launcher.\n\nCancel closes the room.", room, shown,
                   who_joined(player - 1, count - 1), room);
        int ready = wait_readable(control, 0);
        if (ready < 0 || !read_line(control, line, sizeof line, 5000)) {
            closesocket(control);
            freeaddrinfo(where);
            return cancelled() ? -1 : fail("The relay server closed the room's connection.");
        }
        unsigned ticket;
        if (sscanf(line, "JOIN %u", &ticket) != 1) continue; /* PING */
        SOCKET s = connect_to(where, 10000);
        char request[64];
        snprintf(request, sizeof request, "WSRR1 DATA %u\n", ticket);
        if (s == INVALID_SOCKET || !send_text(s, request) || !read_line(s, line, sizeof line, 10000) || strcmp(line, "OK")) {
            if (s != INVALID_SOCKET) closesocket(s);
            fprintf(report, "Online: a player's connection through the relay didn't open (%s)\n", line);
            continue;
        }
        if (admit(s, player, count)) player++;
        else closesocket(s);
    }
    closesocket(control); /* the room closes; the players' connections stay */
    freeaddrinfo(where);
    return host_send_save(count, save);
}

/* A player, once connected: HELLO, WELCOME, the host's save. */
static int join_on(SOCKET s, const char* shown, const char* save, const RemoteCalibration* mine) {
    Hello hello;
    memset(&hello, 0, sizeof hello);
    hello.protocol = PROTOCOL;
    snprintf(hello.build, sizeof hello.build, "%s", build_id);
    hello.cal = *mine;
    Header h;
    u32 welcome[2];
    net_status("Connected to %s. Waiting for the host's game to let you in...", shown);
    if (!send_message(s, MSG_HELLO, 0, 0, &hello, sizeof hello) || wait_readable(s, 20000) <= 0 || !recv_all(s, &h, sizeof h)) {
        closesocket(s);
        return cancelled() ? -1 : fail("The host's game didn't answer.");
    }
    if (h.type == MSG_REFUSED && h.size < 400) {
        char why[401] = "";
        recv_all(s, why, (int)h.size);
        why[h.size] = 0;
        closesocket(s);
        return fail("The host turned this PC away: %s", why);
    }
    if (h.type != MSG_WELCOME || h.size != sizeof welcome || !recv_all(s, welcome, sizeof welcome)) {
        closesocket(s);
        return fail("The host's game didn't let this PC in. Is everyone using the same build?");
    }
    net_status("You're player %u. Waiting for the others, then taking the host's save...", welcome[0] + 1);
    if (!take_save(s, save)) {
        closesocket(s);
        return cancelled() ? -1 : fail("The connection closed before the host's save arrived.");
    }
    peers[0] = s;
    me = welcome[0];
    players = welcome[1];
    active = 1;
    fprintf(report, "Online: joined %s as player %u of %u\n", shown, me + 1, players);
    fflush(report);
    return (int)me;
}

int net_start_join(const char* address, const char* save, const RemoteCalibration* mine) {
    if (!winsock()) return fail("Windows networking didn't start.");
    struct addrinfo* where = NULL;
    char shown[160];
    if (!resolve(address, 0, &where, shown, sizeof shown) || !strchr(address, ':'))
        return fail("\"%s\" isn't an address and port.", address);
    SOCKET s = INVALID_SOCKET;
    DWORD started = GetTickCount();
    while (s == INVALID_SOCKET) { /* the host may still be starting */
        u32 waited = (GetTickCount() - started) / 1000u;
        if (waited >= 60) break;
        net_status("Connecting to the host at %s... (%u s)\n\nIf this doesn't connect: check the room code (the "
                   "internet code if you're not on the host's network), and that the host is waiting with TCP port "
                   "%s forwarded on their router to their PC.", shown, waited, strrchr(shown, ':') + 1);
        s = connect_to(where, 5000);
        if (s == INVALID_SOCKET && net_status_cancelled()) break;
        if (s == INVALID_SOCKET) Sleep(500);
    }
    freeaddrinfo(where);
    if (s == INVALID_SOCKET)
        return cancelled() ? -1 : fail("Couldn't reach the host at %s in a minute. Usually this means the host's "
                                       "router doesn't pass TCP port %s to their PC, Windows Firewall on their PC "
                                       "blocks the game, or the code is their home-network code. A relay server "
                                       "avoids port forwarding.", shown, strrchr(shown, ':') + 1);
    return join_on(s, shown, save, mine);
}

int net_start_join_relay(const char* relay, const char* room, const char* save, const RemoteCalibration* mine) {
    if (!winsock()) return fail("Windows networking didn't start.");
    struct addrinfo* where = NULL;
    char shown[160], line[160] = "";
    if (!resolve(relay, RELAY_PORT, &where, shown, sizeof shown))
        return fail("The relay server's address \"%s\" couldn't be looked up.", relay);
    net_status("Connecting to the relay %s for room %s...", shown, room);
    SOCKET s = connect_to(where, 10000);
    freeaddrinfo(where);
    if (s == INVALID_SOCKET)
        return cancelled() ? -1 : fail("Couldn't reach the relay server at %s. Check its address in the launcher.", shown);
    snprintf(line, sizeof line, "WSRR1 JOIN %s\n", room);
    net_status("Asking the relay %s for room %s...", shown, room);
    if (!send_text(s, line) || !read_line(s, line, sizeof line, 40000) || strcmp(line, "OK")) {
        closesocket(s);
        return cancelled() ? -1 : fail("The relay couldn't put you in room %s: %s", room,
                                       !strncmp(line, "ERR ", 4) ? line + 4 : "no answer.");
    }
    char label[200];
    snprintf(label, sizeof label, "room %s on %s", room, shown);
    return join_on(s, label, save, mine);
}

int net_active(void) { return active; }

void net_calibration(u32 player, RemoteCalibration* out) {
    if (player < MAX_PLAYERS) *out = cals[player];
    else memset(out, 0, sizeof *out);
}
u32 net_players(void) { return players; }

static void keep_input(u32 field, u32 player, const RemoteInput* in) {
    if (player >= MAX_PLAYERS) return;
    inputs[field % RING][player] = *in;
    input_field[field % RING][player] = field + 1;
}

static void keep_check(u32 field, u32 player, u64 value) {
    if (player >= MAX_PLAYERS) return;
    checks[(field / CHECK_EVERY) % RING][player] = value;
    check_field[(field / CHECK_EVERY) % RING][player] = field + 1;
}

/* The host passes a player's message on to the other players. */
static void relay(u32 from, u32 type, u32 field, const void* payload, u32 size) {
    if (!is_host) return;
    for (u32 p = 1; p < players; p++)
        if (p != from && !gone[p]) send_message(peers[p], type, field, from, payload, size);
}

static void lost(u32 player) {
    if (gone[player]) return;
    gone[player] = 1;
    fprintf(report, "Online: player %u left at field %llu; their remote stays still\n", player + 1,
            (unsigned long long)fields_done);
    fflush(report);
}

/* Reads one message from a socket that has one waiting. */
static void take_message(u32 index) {
    SOCKET s = peers[index];
    Header h;
    u8 payload[1024];
    if (!recv_all(s, &h, sizeof h) || h.size > sizeof payload || (h.size && !recv_all(s, payload, (int)h.size))) {
        if (is_host) lost(index);
        else for (u32 p = 0; p < players; p++) if (p != me) lost(p);
        return;
    }
    if (h.type == MSG_INPUT && h.size == sizeof(RemoteInput)) {
        keep_input(h.field, h.player, (const RemoteInput*)payload);
        relay(h.player, h.type, h.field, payload, h.size);
    } else if (h.type == MSG_STATS && h.size == 2 * sizeof(u32)) {
        u32 values[2];
        memcpy(values, payload, sizeof values);
        if (h.player < MAX_PLAYERS) {
            stat_wait[h.player] = values[0];
            stat_time[h.player] = values[1];
            stat_have[h.player] = 1;
            stat_total_wait[h.player] += values[0];
            stat_total_time[h.player] += values[1];
        }
        relay(h.player, h.type, h.field, payload, h.size);
    } else if (h.type == MSG_CHECK && h.size == sizeof(u64)) {
        u64 value;
        memcpy(&value, payload, sizeof value);
        keep_check(h.field, h.player, value);
        relay(h.player, h.type, h.field, payload, h.size);
    }
}

/* Waits up to ms for messages and takes them. */
static void pump(u32 ms) {
    fd_set ready;
    FD_ZERO(&ready);
    int any = 0;
    for (u32 p = 0; p < MAX_PLAYERS; p++)
        if (peers[p] && peers[p] != INVALID_SOCKET && !gone[is_host ? p : 0]) {
            FD_SET(peers[p], &ready);
            any = 1;
        }
    if (!any) return;
    struct timeval wait = {(long)(ms / 1000u), (long)((ms % 1000u) * 1000u)};
    if (select(0, &ready, NULL, NULL, &wait) <= 0) return;
    for (u32 p = 0; p < MAX_PLAYERS; p++)
        if (peers[p] && peers[p] != INVALID_SOCKET && FD_ISSET(peers[p], &ready)) take_message(p);
}

static int have_all(u32 field) {
    for (u32 p = 0; p < players; p++)
        if (!gone[p] && input_field[field % RING][p] != field + 1) return 0;
    return 1;
}

static void send_to_others(u32 type, u32 field, const void* payload, u32 size) {
    if (is_host) {
        for (u32 p = 1; p < players; p++)
            if (!gone[p]) send_message(peers[p], type, field, me, payload, size);
    } else if (!gone[0]) {
        send_message(peers[0], type, field, me, payload, size);
    }
}

static void compare_checks(u32 field) {
    u32 slot = (field / CHECK_EVERY) % RING;
    for (u32 p = 0; p < players; p++) {
        if (p == me || gone[p] || check_field[slot][p] != field + 1) continue;
        if (checks[slot][p] != checks[slot][me]) {
            desyncs++;
            if (!first_desync) first_desync = field;
            if (desyncs <= 10)
                fprintf(report, "Online: DESYNC at field %u: player %u's game differs from this one's\n", field, p + 1);
        }
    }
}

/* Every STATS_EVERY fields: this PC's speed report to the others, and a line in the window's title saying who,
   if anyone, everyone is waiting for. */
static void speed_report(u32 field) {
    static DWORD since;
    static u64 waited_before;
    DWORD now = GetTickCount();
    if (!since) {
        since = now;
        waited_before = waited_ms;
        return;
    }
    u32 values[2] = {(u32)(waited_ms - waited_before), now - since};
    since = now;
    waited_before = waited_ms;
    stat_wait[me] = values[0];
    stat_time[me] = values[1];
    stat_have[me] = 1;
    stat_total_wait[me] += values[0];
    stat_total_time[me] += values[1];
    send_to_others(MSG_STATS, field, values, sizeof values);

    u32 fps = values[1] ? (u32)(STATS_EVERY * 1000u / values[1]) : 0;
    u32 slowest = me, waits[MAX_PLAYERS] = {0}, everyone_waits = 1, someone_waits = 0;
    for (u32 p = 0; p < players; p++) {
        if (gone[p] || !stat_have[p] || !stat_time[p]) continue;
        waits[p] = stat_wait[p] * 100u / stat_time[p];
        if (waits[p] < waits[slowest]) slowest = p;
        if (waits[p] < 15) everyone_waits = 0;
        if (waits[p] >= 15) someone_waits = 1;
    }
    char title[256], who[64];
    if (slowest == me) snprintf(who, sizeof who, "this PC");
    else snprintf(who, sizeof who, "player %u's PC", slowest + 1);
    if (fps >= 57 || !someone_waits)
        snprintf(title, sizeof title, "Wii Sports Resort - online, player %u of %u - %u fps - running smoothly - input delay %u ms",
                 me + 1, players, fps, (u32)(delay * 16.683 + 0.5));
    else if (everyone_waits)
        snprintf(title, sizeof title, "Wii Sports Resort - online, player %u of %u - %u fps - every PC waits: the connection "
                 "is the slow part - input delay %u ms", me + 1, players, fps, (u32)(delay * 16.683 + 0.5));
    else
        snprintf(title, sizeof title, "Wii Sports Resort - online, player %u of %u - %u fps - waiting for %s (it waits %u%%, "
                 "this PC %u%%)", me + 1, players, fps, who, waits[slowest], waits[me]);
    glue_set_title(title);
    if ((field / STATS_EVERY) % 5 == 0) { /* every 10 seconds in the report */
        fprintf(report, "Online: field %u, %u fps, waiting:", field, fps);
        for (u32 p = 0; p < players; p++)
            if (stat_have[p]) fprintf(report, " player %u %u%%", p + 1, waits[p]);
        fprintf(report, "\n");
    }
}

void net_field(u32 field) {
    static u32 next_send; /* the first field this PC hasn't sent its input for yet */
    if (!active) return;
    RemoteInput local;
    fake_remote_local_input(&local);
    /* Normally one field. The first time, every field up to the delay, as fields count from 1 and the game may
       have run a few before online play started: every player sends the same span, so all agree on it. */
    for (; next_send <= field + delay; next_send++) {
        keep_input(next_send, me, &local);
        send_to_others(MSG_INPUT, next_send, &local, sizeof local);
        local.plug = -1; /* a plug happens once */
    }
    DWORD started = GetTickCount(), noted = started;
    while (!have_all(field)) {
        pump(50);
        DWORD now = GetTickCount();
        if (now - noted > WAIT_NOTE_MS) {
            noted = now;
            fprintf(report, "Online: waiting for players at field %u\n", field);
            fflush(report);
        }
    }
    waited_ms += GetTickCount() - started;
    pump(0); /* take whatever else has arrived, without waiting */
    RemoteInput still;
    memset(&still, 0, sizeof still);
    still.plug = -1;
    for (u32 p = 0; p < players; p++)
        fake_remote_apply_input(p, gone[p] ? &still : &inputs[field % RING][p]);
    /* Checks are compared a few fields after they're made, once the others' have had time to arrive. */
    if (field % CHECK_EVERY == 0 && field) {
        u64 value = host_memory_hash();
        keep_check(field, me, value);
        send_to_others(MSG_CHECK, field, &value, sizeof value);
    }
    if (field % CHECK_EVERY == delay && field > CHECK_EVERY) compare_checks(field - delay);
    if (field % STATS_EVERY == 0) speed_report(field);
    fields_done++;
}

void net_write_report(void) {
    if (!active) return;
    fprintf(report, "online: player %u of %u, %llu fields, waited %llu ms for others, %llu desyncs", me + 1, players,
            (unsigned long long)fields_done, (unsigned long long)waited_ms, (unsigned long long)desyncs);
    if (desyncs) fprintf(report, ", the first at field %u", first_desync);
    fprintf(report, ", input delay %u fields\n", delay);
    for (u32 p = 0; p < players; p++) {
        if (!stat_total_time[p]) continue;
        fprintf(report, "online: player %u waited for the others %llu%% of the time", p + 1,
                (unsigned long long)(stat_total_wait[p] * 100u / stat_total_time[p]));
        if (round_trip_ms[p] > 0) fprintf(report, ", round trip to the host %.0f ms", round_trip_ms[p]);
        fprintf(report, "%s\n", p == me ? " (this PC)" : "");
    }
}
