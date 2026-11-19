/* wsr_relay: rooms for the native Wii Sports Resort's online play, run on a server everyone can reach.

   Without it, the host's PC has to accept connections from the internet, which needs a forwarded port on the
   host's router (and fails behind carrier-grade NAT). With it, the host and every player only connect out to
   this server, and it passes the bytes between them. It never looks inside the game's messages, and keeps
   nothing on disk. Only the server's own port has to be reachable.

   Build on Ubuntu:   gcc -O2 -Wall -o wsr_relay wsr_relay.c
   Run:               ./wsr_relay [port [address]]  (port 51020 and every address if not given; logs to stdout)
   It also builds on Windows with MinGW (-lws2_32), for testing.

   Protocol, version 1. Every connection starts with one line of text, and gets one line back:
     "WSRR1 HOST <room>"   a host opens a room.         -> "OK", or "ERR <reason>" and the connection closes.
                           It stays open as the room's control line: the relay sends "JOIN <ticket>" when a
                           player arrives, and "PING" every 20 seconds. Closing it closes the room.
     "WSRR1 JOIN <room>"   a player asks for a room.    -> the relay tells the host, and once the host has
                           connected for this player: "OK", then the two connections are joined end to end.
                           "ERR <reason>" if there's no such room, or the host doesn't answer in 30 seconds.
     "WSRR1 DATA <ticket>" the host's connection for one player. -> "OK", then joined to that player.
   Rooms are 4 to 12 letters and digits. */
#define _DEFAULT_SOURCE /* POSIX sockets, fcntl, and inet_ntoa with any -std */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define poll WSAPoll
#define close_sock closesocket
#define WOULD_BLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
#define WOULD_BLOCK (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
#endif
#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_PORT 51020
#define MAX_CONNS 256
#define MAX_ROOMS 64
#define BUF_SIZE 65536
#define LINE_MAX_LEN 96
#define HELLO_SECONDS 10   /* to send the first line */
#define ANSWER_SECONDS 30  /* for the host to connect for a waiting player */
#define PING_SECONDS 20

enum { FREE, HELLO, HOST, WAITING, PIPE };

typedef struct {
    sock_t fd;
    int state;
    char line[LINE_MAX_LEN];
    int line_len;
    time_t since;
    int room;          /* HOST and WAITING: the room */
    unsigned ticket;   /* WAITING: the ticket the host answers with */
    int peer;          /* PIPE: the other end */
    int closing;       /* close once out is sent */
    char peer_name[64];
    int out_len;
    char out[BUF_SIZE]; /* waiting to be sent to this connection */
} Conn;

typedef struct {
    int used;
    char name[16];
    int host;
    time_t last_ping;
    unsigned players;
} Room;

static Conn conns[MAX_CONNS];
static Room rooms[MAX_ROOMS];
static unsigned next_ticket = 1;

static void say(const char* format, ...) {
    char when[32];
    time_t now = time(NULL);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("%s ", when);
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void set_nonblocking(sock_t fd) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    int on2 = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&on2, sizeof on2);
}

/* Queues text for a connection. Lines are short, so they always fit while the connection is in its first steps. */
static void queue(int c, const char* text) {
    int n = (int)strlen(text);
    if (conns[c].out_len + n > BUF_SIZE) return;
    memcpy(conns[c].out + conns[c].out_len, text, (size_t)n);
    conns[c].out_len += n;
}

static void drop(int c);

static void close_room(int r, const char* why) {
    if (!rooms[r].used) return;
    say("room %s closed (%s)", rooms[r].name, why);
    rooms[r].used = 0;
    for (int c = 0; c < MAX_CONNS; c++)
        if (conns[c].state == WAITING && conns[c].room == r) {
            queue(c, "ERR the host left\n");
            conns[c].closing = 1;
        }
}

static void drop(int c) {
    Conn* x = &conns[c];
    if (x->state == FREE) return;
    if (x->state == HOST) close_room(x->room, "the host's line closed");
    if (x->state == PIPE && x->peer >= 0 && conns[x->peer].state == PIPE) {
        conns[x->peer].closing = 1; /* the other end gets what's left, then closes */
        conns[x->peer].peer = -1;
    }
    close_sock(x->fd);
    x->state = FREE;
}

static int find_room(const char* name) {
    for (int r = 0; r < MAX_ROOMS; r++)
        if (rooms[r].used && !strcmp(rooms[r].name, name)) return r;
    return -1;
}

static int good_room_name(const char* name) {
    size_t n = strlen(name);
    if (n < 4 || n > 12) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)name[i])) return 0;
    return 1;
}

static int refuse(int c, const char* why) {
    char text[LINE_MAX_LEN + 8];
    snprintf(text, sizeof text, "ERR %s\n", why);
    queue(c, text);
    conns[c].closing = 1;
    return 0;
}

/* The first line of a connection. */
static int hello(int c) {
    Conn* x = &conns[c];
    char word[16] = "", arg[32] = "";
    if (sscanf(x->line, "WSRR1 %15s %31s", word, arg) != 2) {
        return refuse(c, "this relay speaks protocol WSRR1; update the game");
    }
    for (char* p = arg; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (!strcmp(word, "HOST")) {
        if (!good_room_name(arg)) return refuse(c, "a room is 4 to 12 letters and digits");
        if (find_room(arg) >= 0) return refuse(c, "that room is already open: pick another code");
        int r = 0;
        while (r < MAX_ROOMS && rooms[r].used) r++;
        if (r == MAX_ROOMS) return refuse(c, "the relay is full; try again later");
        rooms[r].used = 1;
        snprintf(rooms[r].name, sizeof rooms[r].name, "%s", arg);
        rooms[r].host = c;
        rooms[r].last_ping = time(NULL);
        rooms[r].players = 0;
        x->state = HOST;
        x->room = r;
        queue(c, "OK\n");
        say("room %s opened by %s", arg, x->peer_name);
    } else if (!strcmp(word, "JOIN")) {
        int r = find_room(arg);
        if (r < 0) return refuse(c, "there's no room with that code on this relay (is the host's game waiting?)");
        x->state = WAITING;
        x->room = r;
        x->ticket = next_ticket++;
        x->since = time(NULL);
        char text[48];
        snprintf(text, sizeof text, "JOIN %u\n", x->ticket);
        queue(rooms[r].host, text);
        say("room %s: %s asks to join (ticket %u)", rooms[r].name, x->peer_name, x->ticket);
    } else if (!strcmp(word, "DATA")) {
        unsigned ticket = (unsigned)strtoul(arg, NULL, 10);
        int w = -1;
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].state == WAITING && conns[i].ticket == ticket) w = i;
        if (w < 0) return refuse(c, "that player isn't waiting any more");
        x->state = PIPE;
        x->peer = w;
        conns[w].state = PIPE;
        conns[w].peer = c;
        queue(c, "OK\n");
        queue(w, "OK\n");
        int r = conns[w].room;
        if (rooms[r].used) rooms[r].players++;
        say("room %s: %s joined", rooms[r].used ? rooms[r].name : "?", conns[w].peer_name);
    } else {
        refuse(c, "unknown request");
    }
    return 0;
}

/* Reads the first line one byte at a time, so nothing after it is taken by mistake. */
static void read_line(int c) {
    Conn* x = &conns[c];
    for (;;) {
        char ch;
        int got = (int)recv(x->fd, &ch, 1, 0);
        if (got == 0 || (got < 0 && !WOULD_BLOCK)) {
            drop(c);
            return;
        }
        if (got < 0) return;
        if (ch == '\r') continue;
        if (ch == '\n') {
            x->line[x->line_len] = 0;
            x->line_len = 0;
            if (x->state == HELLO) hello(c);
            /* a host's control line may send "PING"; nothing else is expected on it */
            return;
        }
        if (x->line_len >= LINE_MAX_LEN - 1) {
            refuse(c, "line too long");
            return;
        }
        x->line[x->line_len++] = ch;
    }
}

static void pass_on(int c) {
    Conn* x = &conns[c];
    Conn* to = &conns[x->peer];
    int room = BUF_SIZE - to->out_len;
    if (room <= 0) return;
    int got = (int)recv(x->fd, to->out + to->out_len, room, 0);
    if (got > 0) {
        to->out_len += got;
    } else if (got == 0 || !WOULD_BLOCK) {
        drop(c);
    }
}

static void send_out(int c) {
    Conn* x = &conns[c];
    int sent = (int)send(x->fd, x->out, x->out_len, 0);
    if (sent < 0) {
        if (!WOULD_BLOCK) drop(c);
        return;
    }
    memmove(x->out, x->out + sent, (size_t)(x->out_len - sent));
    x->out_len -= sent;
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    const char* address = argc > 2 ? argv[2] : "0.0.0.0";
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "usage: %s [port [address to listen on]]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    sock_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);
    struct sockaddr_in at;
    memset(&at, 0, sizeof at);
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = inet_addr(address);
    at.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr*)&at, sizeof at) || listen(listener, 16)) {
        fprintf(stderr, "can't listen on TCP port %d at %s\n", port, address);
        return 1;
    }
    set_nonblocking(listener);
    say("wsr_relay listening on TCP port %d at %s", port, address);

    static struct pollfd fds[MAX_CONNS + 1];
    static int index_of[MAX_CONNS + 1];
    for (;;) {
        int n = 0;
        fds[n].fd = listener;
        fds[n].events = POLLIN;
        index_of[n++] = -1;
        for (int c = 0; c < MAX_CONNS; c++) {
            Conn* x = &conns[c];
            if (x->state == FREE) continue;
            short events = 0;
            if (!x->closing) {
                if (x->state != PIPE) events |= POLLIN;
                else if (x->peer >= 0 && conns[x->peer].out_len < BUF_SIZE) events |= POLLIN;
            }
            if (x->out_len) events |= POLLOUT;
            fds[n].fd = x->fd;
            fds[n].events = events;
            fds[n].revents = 0;
            index_of[n++] = c;
        }
        poll(fds, (unsigned)n, 1000);
        time_t now = time(NULL);
        for (int i = 0; i < n; i++) {
            if (index_of[i] < 0) {
                if (!(fds[i].revents & POLLIN)) continue;
                struct sockaddr_in from;
                socklen_t size = sizeof from;
                sock_t fd = accept(listener, (struct sockaddr*)&from, &size);
                if (fd == INVALID_SOCKET) continue;
                int c = 0;
                while (c < MAX_CONNS && conns[c].state != FREE) c++;
                if (c == MAX_CONNS) {
                    close_sock(fd);
                    continue;
                }
                set_nonblocking(fd);
                Conn* x = &conns[c];
                memset(x, 0, offsetof(Conn, out));
                x->fd = fd;
                x->state = HELLO;
                x->since = now;
                x->peer = -1;
                snprintf(x->peer_name, sizeof x->peer_name, "%s:%u", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                continue;
            }
            int c = index_of[i];
            if (conns[c].state == FREE) continue;
            short got = fds[i].revents;
            if (got & POLLOUT) send_out(c);
            if (conns[c].state == FREE) continue;
            if (got & (POLLIN | POLLHUP | POLLERR)) {
                if (conns[c].state == PIPE && conns[c].peer >= 0) pass_on(c);
                else if (conns[c].state == PIPE) drop(c); /* the other end is gone */
                else read_line(c);
            }
        }
        for (int c = 0; c < MAX_CONNS; c++) {
            Conn* x = &conns[c];
            if (x->state == FREE) continue;
            if (x->closing && !x->out_len) drop(c);
            else if (x->state == HELLO && now - x->since > HELLO_SECONDS) drop(c);
            else if (x->state == WAITING && !x->closing && now - x->since > ANSWER_SECONDS) {
                say("ticket %u: the host didn't answer", x->ticket);
                refuse(c, "the host's game didn't answer");
            }
        }
        for (int r = 0; r < MAX_ROOMS; r++)
            if (rooms[r].used && now - rooms[r].last_ping >= PING_SECONDS) {
                rooms[r].last_ping = now;
                queue(rooms[r].host, "PING\n");
            }
    }
}
