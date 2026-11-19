/* The online status window. See net_status.c. */
#ifndef NET_STATUS_H
#define NET_STATUS_H

void net_status_open(int show);        /* show 0: tests, nothing on screen; the calls below do nothing */
void net_status(const char* format, ...); /* replaces the text */
int net_status_cancelled(void);        /* the player pressed Cancel or closed it */
void net_status_fail(const char* message); /* shows why it couldn't start, and waits until the player closes it */
void net_status_close(void);           /* everyone's in: the game's window takes over */
int net_status_self_test(const char* folder); /* draws it off-screen into two BMPs; 0 if it worked */

#endif
