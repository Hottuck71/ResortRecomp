# wsr_relay: an online room server for your Ubuntu machine

> **Setting up a server?** The step-by-step guide for a local (same-house) server, a Windows PC, an Ubuntu home
> server or Raspberry Pi, or a rented VPS is **[docs/ONLINE-SERVER.md](../../docs/ONLINE-SERVER.md)**. This page
> has the technical notes.

Online play normally needs the host's router to forward a port to the host's PC. With this relay running on a
server everyone can reach, the host and the players only connect *out* to the server, so nobody else forwards
anything. The relay passes bytes between them and nothing more: it doesn't read the game's messages, keeps
nothing on disk, and needs no privileges.

It's one C file, `wsr_relay.c`, with no game code in it.

## What it needs

- Ubuntu (any version from the last several years) with a C compiler.
- One TCP port the internet can reach. These steps use **51020**; any free port works if you change it
  everywhere below.

## 1. Copy the file to the server

From this PC (PowerShell, in the repository's folder), replacing `you@server` with your SSH login:

```
scp native\relay\wsr_relay.c native\relay\wsr-relay.service you@server:~
```

Or copy the two files over any way you like.

## 2. Check for a compiler, and install one only if it's missing

```
gcc --version
```

If that prints a version, skip to step 3. If it says `command not found`, install Ubuntu's standard build tools
(the C compiler and the system headers the relay is built against), from Ubuntu's own package archive:

```
sudo apt update
sudo apt install build-essential
```

Check it worked: `gcc --version` now prints a version.

## 3. Build it

```
gcc -O2 -Wall -o wsr_relay wsr_relay.c
```

No output means it built. Check: `ls -l wsr_relay` shows the program.

## 4. Try it by hand

```
./wsr_relay 51020
```

It prints `wsr_relay listening on TCP port 51020 at 0.0.0.0` and waits. Press Ctrl+C to stop it.

## 5. Let the port through

On the server's own firewall, if it uses ufw (`sudo ufw status` says `active`):

```
sudo ufw allow 51020/tcp
```

On your router: forward **TCP** port 51020 to the server's local IP address, as you did for your other
software. Only TCP is used.

## 6. Run it as a service, so it starts with the server

```
sudo mkdir -p /opt/wsr-relay
sudo cp wsr_relay /opt/wsr-relay/
sudo cp wsr-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wsr-relay
```

Check it's running: `systemctl status wsr-relay` says `active (running)`. Its log (rooms opening, players
joining, rooms closing): `journalctl -u wsr-relay -f`.

## 7. Use it

In the launcher's **Online** tab, everyone puts the server's address in **Relay server**: your home's public
IP address or a domain name pointing at it, plus `:51020` if you picked a different port (51020 is assumed).
The host presses **Host** and reads out the 6-character room code; the others type it under **Join a room**.

If the server is at your home, you're on the same network as it: your own PC may not be able to reach your
home's public address from inside ("hairpin" NAT; some routers can't). Then use the server's local address
(for example `192.168.1.50`) on your PC, and the public one on your friends' PCs. Both reach the same relay.

## Updating

Copy the new `wsr_relay.c`, build it again (step 3), then:

```
sudo cp wsr_relay /opt/wsr-relay/ && sudo systemctl restart wsr-relay
```

## How it works

Each connection starts with one line of text: `WSRR1 HOST <room>` opens a room, `WSRR1 JOIN <room>` asks for
one, and the host answers each player with a second connection, `WSRR1 DATA <ticket>`, which the relay joins
end to end with that player's. Details are at the top of `wsr_relay.c`. Limits: 64 rooms and 256 connections
at once; a room closes when the host's game stops waiting for players.
