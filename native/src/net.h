/* Online play. See net.c. */
#ifndef NET_H
#define NET_H
#include "fake_remote.h"

/* --net-host <port> --net-players <n>: waits for n - 1 players, then sends each this PC's save folder.
   --net-join <ip:port>: joins one, and writes the host's save into the folder given, which it empties first.
   Return this PC's player number (0 for the host), or -1. Block until everyone is there. */
int net_start_host(u32 port, u32 players, const char* save, const RemoteCalibration* mine);
int net_start_join(const char* address, const char* save, const RemoteCalibration* mine);
/* The same through a relay server ("name[:port]", port 51020 if left out) and a room code. */
int net_start_host_relay(const char* relay, const char* room, u32 players, const char* save, const RemoteCalibration* mine);
int net_start_join_relay(const char* relay, const char* room, const char* save, const RemoteCalibration* mine);
/* Why the last net_start_* failed, in a sentence for the player. */
const char* net_last_error(void);
/* Every player's real remote calibration, as gathered when they joined (present 0 for a keyboard player). */
void net_calibration(u32 player, RemoteCalibration* out);
int net_active(void);
u32 net_players(void);
/* Once per video field, on the CPU thread: sends this PC's input for a later field, waits for every player's
   input for this one, and hands them to the stand-in remotes. Every so often it also compares a checksum of
   guest memory with the others, to catch a desync. */
void net_field(u32 field);
void net_write_report(void);

u64 host_memory_hash(void); /* main.c */

#endif
