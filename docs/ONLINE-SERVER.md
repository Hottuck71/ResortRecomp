# Make your own online server

ResortRecomp plays online through a tiny **relay server** (`wsr_relay`). Every player's game connects *out* to the
relay, and the relay passes the players' input between them. Only the relay's computer ever needs a port opened;
nobody else touches their router.

The relay uses almost no processor or memory, stores nothing, and never sees anything from the game: only the
players' button presses and motion. It runs on:

- a **Windows 10 or 11 PC** (it can be the PC you play on), or
- an **Ubuntu / Debian Linux** computer: a home server, an old laptop, a Raspberry Pi, or a rented VPS.

It uses **one TCP port, 51020** by default. Never UDP.

## Which setup do you need?

| You want to play with… | Do this | Router changes? |
|---|---|---|
| People in the same house (same Wi-Fi / network) | [Local server](#1-local-server-same-home-network) | No |
| Friends in other houses, from a PC you leave on | [Home server on Windows](#2-home-server-on-windows) + [router](#4-forward-the-port-on-your-router) | Yes, one port |
| Friends in other houses, from a Linux box | [Home server on Ubuntu](#3-home-server-on-ubuntu-or-raspberry-pi) + [router](#4-forward-the-port-on-your-router) | Yes, one port |
| Friends, but your internet provider blocks port forwarding | [Ubuntu steps](#3-home-server-on-ubuntu-or-raspberry-pi) on a rented VPS | No (the VPS has its own address) |

Everyone who plays needs the **same ResortRecomp version** and their own copy of the game, set up with the
launcher.

---

## 1. Local server (same home network)

For playing with people on the same network, in the same house or at a LAN party. No router settings.

1. On any PC on the network (the host's PC is fine), open the ResortRecomp folder, then the `relay` folder, and
   double-click **`wsr_relay.exe`**. A black window says:
   ```
   wsr_relay listening on TCP port 51020 at 0.0.0.0
   ```
   Leave it open. The relay runs as long as this window is open.
2. Windows may show "Windows protected your PC": click **More info**, then **Run anyway**. (The program isn't
   signed. Its source is [`native/relay/wsr_relay.c`](../native/relay/wsr_relay.c) if you'd rather build it
   yourself.)
3. When Windows Firewall asks, tick **Private networks** and click **Allow access**.
4. Find that PC's address: press **Win+R**, type `cmd`, press Enter, type `ipconfig`, and look for
   **IPv4 Address**, for example `192.168.1.119`.
5. Every player: in the launcher's **Online** tab, type that address in **Relay server**.
6. The host picks the number of **Players** and presses **Host**, then reads out the 6-letter **Room code**. The
   others type it under **Join a room** and press **Join**.

> **No relay at all?** On the same network you can also leave **Relay server** empty. The host's launcher then
> shows a **Room code on your network**; the others type that code under **Join a room**. The relay is still
> the more reliable choice, since it never depends on the host's firewall.

---

## 2. Home server on Windows

A Windows PC at your home runs the relay, and friends anywhere connect to it over the internet.

1. Do steps 1 to 4 of the [local server](#1-local-server-same-home-network) on the PC that will run the relay.
2. If friends can't connect later, or Windows never asked about the firewall, open PowerShell **as
   administrator** (right-click Start, **Terminal (Admin)**) and run:
   ```powershell
   New-NetFirewallRule -DisplayName "ResortRecomp relay" -Direction Inbound -Protocol TCP -LocalPort 51020 -Action Allow
   ```
3. Give the PC a fixed address on your network, so the port forward keeps pointing at it. In your router this is
   usually called **DHCP reservation**, **static lease** or **address reservation**.
4. [Forward the port on your router](#4-forward-the-port-on-your-router) to this PC's address.
5. [Tell everyone the address](#5-what-everyone-types).

**Start it automatically** (pick one):

- **When you sign in:** press **Win+R**, type `shell:startup`, press Enter. Right-click `wsr_relay.exe`, choose
  **Show more options**, then **Create shortcut**, and move the shortcut into the folder that opened.
- **When the PC starts, even before anyone signs in, with no window:** in PowerShell as administrator, changing
  the path to where your `relay` folder is:
  ```powershell
  $relay = "C:\Games\ResortRecomp\relay\wsr_relay.exe"
  Register-ScheduledTask -TaskName "ResortRecomp relay" -User SYSTEM -RunLevel Highest -Trigger (New-ScheduledTaskTrigger -AtStartup) -Action (New-ScheduledTaskAction -Execute $relay -Argument 51020) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit 0)
  Start-ScheduledTask -TaskName "ResortRecomp relay"
  ```
  To remove it later: `Unregister-ScheduledTask -TaskName "ResortRecomp relay"`.

---

## 3. Home server on Ubuntu (or Raspberry Pi)

Works on Ubuntu, Debian, Raspberry Pi OS, and most other Linux systems. Run these on the server, over SSH or
at its keyboard.

1. **Install a C compiler and curl** (skip if `gcc --version` already prints a version):
   ```bash
   sudo apt update
   sudo apt install build-essential curl
   ```
2. **Download the relay's two files** straight from this repository:
   ```bash
   mkdir -p ~/wsr-relay && cd ~/wsr-relay
   curl -LO https://raw.githubusercontent.com/Hottuck71/ResortRecomp/main/native/relay/wsr_relay.c
   curl -LO https://raw.githubusercontent.com/Hottuck71/ResortRecomp/main/native/relay/wsr-relay.service
   ```
   (Or copy them from the `relay` folder of the download, using `scp`, a USB stick, or anything else.)
3. **Build it.** No output means it worked:
   ```bash
   gcc -O2 -Wall -o wsr_relay wsr_relay.c
   ```
4. **Try it once:**
   ```bash
   ./wsr_relay 51020
   ```
   It should say `wsr_relay listening on TCP port 51020 at 0.0.0.0`. Press **Ctrl+C** to stop it.
5. **Open the port on the server's firewall**, only if it's on:
   ```bash
   sudo ufw status
   ```
   If it says `active`, run `sudo ufw allow 51020/tcp`. If it says `inactive`, skip this step.
6. **Make it run all the time**, starting by itself whenever the server boots:
   ```bash
   sudo mkdir -p /opt/wsr-relay
   sudo cp wsr_relay /opt/wsr-relay/
   sudo cp wsr-relay.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now wsr-relay
   systemctl status wsr-relay
   ```
   It should say **active (running)**. Press **q** to leave. The service runs as an unprivileged throwaway user
   with no access to your files.
7. **Find the server's home address**, the first number this prints (for example `192.168.1.50`):
   ```bash
   hostname -I
   ```
   Give the server a fixed address in your router (DHCP reservation), then
   [forward the port](#4-forward-the-port-on-your-router) to it.
8. **Find your public address** for friends:
   ```bash
   curl -4 ifconfig.me
   ```

Useful afterwards:

| To… | Run |
|---|---|
| Watch players join and leave | `journalctl -u wsr-relay -f` (Ctrl+C stops watching, not the relay) |
| Restart it | `sudo systemctl restart wsr-relay` |
| Stop it for good | `sudo systemctl disable --now wsr-relay` |
| Update it | download `wsr_relay.c` again (step 2), build (step 3), then `sudo cp wsr_relay /opt/wsr-relay/ && sudo systemctl restart wsr-relay` |

**On a rented VPS** (any cheap Linux server from a hosting company): do the same steps. There's no home router, so
skip the router section, but open TCP port 51020 in the hosting company's firewall panel if it has one. Everyone,
including you, types the VPS's address.

---

## 4. Forward the port on your router

Only for a home server that friends outside your home connect to.

1. Open your router's settings page in a browser. It's often `http://192.168.1.1` or `http://192.168.0.1`; the
   address and login are usually on a sticker on the router.
2. Find **Port Forwarding**. It's sometimes under **Advanced**, **NAT**, **Firewall** or **Virtual Server**.
3. Add a rule:

   | Setting | Value |
   |---|---|
   | Protocol | **TCP** |
   | External port | **51020** |
   | Internal port | **51020** |
   | Device / internal IP | the relay computer's home address (from above) |

4. Save. Every router looks different; searching the web for your router's model plus "port forward" usually
   finds a guide.

**If it can't work at all:** some internet providers don't give you a public address of your own
("carrier-grade NAT"). Compare the **WAN / internet address** on your router's status page with what
`curl -4 ifconfig.me` (or a "what is my IP" website) shows. If they differ, often with the router showing
something from `100.64.x.x` to `100.127.x.x`, port forwarding can't reach you. Use a rented VPS instead, or ask
your provider for a public IPv4 address.

---

## 5. What everyone types

In the launcher's **Online** tab, in **Relay server**:

| Who | Types |
|---|---|
| Friends outside your home | your **public** address, for example `73.45.120.18` |
| You, and anyone on the same network as the relay | the relay computer's **home** address, for example `192.168.1.50` (many routers can't loop back to their own public address from inside) |
| Everyone, with a VPS | the VPS's address |

If the relay uses a port other than 51020, add it after a colon: `73.45.120.18:51030`.

Then the host sets **Players** and presses **Host**, and reads out the 6-letter **Room code**; everyone else types it
under **Join a room** and presses **Join**. Each player plays with their own Wii Remote on their own PC.

Your public address can change now and then. If friends suddenly can't connect, check it again.

**A free name instead of a number** (optional): a dynamic-DNS service (DuckDNS, No-IP, or your router's own
DDNS setting) gives your home a name like `myresort.duckdns.org` that follows your address when it changes.
Friends type that name in **Relay server** instead.

---

## Check that it works

From any Windows PC, in PowerShell, using the address your friends would use:

```powershell
Test-NetConnection 192.168.1.50 -Port 51020
```

`TcpTestSucceeded : True` means the relay is reachable from there. Do this once on your network with the home
address, and have a friend do it with your public address to check the port forward.

## Using a different port

Put the port after the program's name:

- **Windows:** make a shortcut to `wsr_relay.exe` and add ` 51030` at the end of its **Target** box (or change
  `-Argument 51020` in the scheduled task).
- **Ubuntu:** in `/etc/systemd/system/wsr-relay.service`, change `51020` on the `ExecStart` line, then
  `sudo systemctl daemon-reload && sudo systemctl restart wsr-relay`.

Open and forward that port instead, and everyone adds it to the address: `73.45.120.18:51030`.

## If something doesn't work

The game opens a small window that says what went wrong:

| Message | Check |
|---|---|
| **Couldn't reach the relay server** | Is the relay running (its window open, or `systemctl status wsr-relay`)? Is the port forwarded and allowed through the firewall? Inside your home, use the relay's **home** address, not the public one. Run the `Test-NetConnection` check above. See the carrier-grade NAT note under [router](#4-forward-the-port-on-your-router). |
| **There's no room with that code** | A typo, or the host closed their waiting window. The host's code changes when they press **New code**. |
| **The host has a different build** | Everyone needs the same ResortRecomp version. Download the same release and set it up again. |
| Lag or stutter | Online play is lockstep: the slowest PC sets the pace. On a slower PC, use **Play > Speed > Start measuring** and the **Graphics** tab's **Low** preset. Where the relay runs matters less, but a relay close to the players helps. |

The relay itself never needs updating unless a release says so.
