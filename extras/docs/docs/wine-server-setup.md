# Echo VR Dedicated Server on Wine (Linux)

Guide for deploying Echo VR dedicated game servers under Wine on a Linux VPS.
Tested on Ubuntu 24.04 LTS (kernel 6.8) with Wine 9.0. Supports running
multiple server instances simultaneously.

## Prerequisites

- Ubuntu 24.04 LTS (or similar Debian-based distro)
- 2+ CPU cores and 1GB RAM per server instance (~400MB typical)
- Wine 64-bit (`apt install wine wine64`)
- 7zip (`apt install p7zip-full`) for extracting game files
- zstd (`apt install zstd`) for extracting NEVR runtime DLLs

## 1. Install Packages

```bash
apt update
apt install -y wine wine64 p7zip-full zstd
```

Wine 32-bit (`wine32`) is not required — Echo VR is a 64-bit binary. The
`wine32 missing` warning during prefix creation is harmless.

## 2. Deploy Game Files

Download and extract the game archive:

```bash
cd /opt
curl -L -o ready-at-dawn-echo-arena.7z "https://sprock.io/e/ready-at-dawn-echo-arena.7z"
7z x -y ready-at-dawn-echo-arena.7z
rm ready-at-dawn-echo-arena.7z
```

This creates `/opt/ready-at-dawn-echo-arena/` with the game binary at
`bin/win10/echovr.exe`.

## 3. Deploy NEVR Runtime DLLs

Build the runtime (on your dev machine) and copy the dist archive to the server:

```bash
# On dev machine
make dist
scp /tmp/nevr-runtime-*.tar.zst root@server:/tmp/
```

On the server, extract and deploy:

```bash
cd /tmp
tar --zstd -xf nevr-runtime-*.tar.zst
cp nevr-runtime-*/dbgcore.dll /opt/ready-at-dawn-echo-arena/bin/win10/
cp nevr-runtime-*/pnsradgameserver.dll /opt/ready-at-dawn-echo-arena/bin/win10/
cp nevr-runtime-*/telemetryagent.dll /opt/ready-at-dawn-echo-arena/bin/win10/
rm -rf /tmp/nevr-runtime-*
```

The key DLLs:

| File | Purpose |
|------|---------|
| `dbgcore.dll` | Gamepatches — runtime hooks, CLI flags |
| `pnsradgameserver.dll` | Multiplayer networking, session management |
| `telemetryagent.dll` | Game state monitoring, telemetry |

## 4. Configure the Game

### Server config

Place your config at `/opt/ready-at-dawn-echo-arena/_local/g.config.json`.
This points the server at the Nakama backend. Example structure:

```json
{
  "configservice_host": "ws://g.echovrce.com:80/spr",
  "loginservice_host": "ws://g.echovrce.com:80/spr?format=evr&discord_id=YOUR_ID&password=YOUR_PASS",
  "serverdb_host": "ws://g.echovrce.com:80/spr?format=evr&regions=REGION&discord_id=YOUR_ID&password=YOUR_PASS&guilds=GUILD_IDS",
  "matchingservice_host": "ws://g.echovrce.com:80",
  "transactionservice_host": "ws://g.echovrce.com:80/spr",
  "publisher_lock": "echovrce"
}
```

### Network retry tolerance

The default netconfig files have `"retries": 2`, which is too aggressive for
a remote server where network hiccups are expected. Increase to 50:

```bash
sed -i 's/"retries": 2/"retries": 50/g' \
  /opt/ready-at-dawn-echo-arena/sourcedb/rad15/json/r14/config/netconfig_*.json
```

This affects `netconfig_dedicatedserver.json`, `netconfig_lanserver.json`,
`netconfig_localserver.json`, and `netconfig_client.json`.

## 5. Firewall

Open UDP ports 6791-6820 for game traffic:

```bash
iptables -I INPUT -p udp --dport 6791:6820 -j ACCEPT
apt install -y iptables-persistent
netfilter-persistent save
```

Or with ufw:

```bash
ufw allow 6791:6820/udp
```

## 6. Kernel and System Tuning

### GRUB kernel parameters

Edit `/etc/default/grub` and add to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
preempt=full threadirqs skew_tick=1
```

Then apply:

```bash
update-grub    # or grub-mkconfig -o /boot/grub/grub.cfg
reboot
```

What each does:

| Parameter | Purpose |
|-----------|---------|
| `preempt=full` | Full kernel preemption — reduces worst-case scheduling latency at ~2-5% throughput cost. Valid on kernels with `CONFIG_PREEMPT_DYNAMIC` (Ubuntu 24.04 ships this). |
| `threadirqs` | Runs interrupt handlers as schedulable kernel threads instead of in hard IRQ context. Closest thing to RT behavior on a non-RT kernel. |
| `skew_tick=1` | Offsets per-CPU timer ticks so they don't all fire simultaneously, reducing lock contention jitter on multi-core systems. Standard Red Hat low-latency recommendation. |

**Note on `clocksource=tsc`:** Often recommended, but on KVM/QEMU virtual
machines the kernel will ignore it and correctly select `kvm-clock` instead.
TSC is unreliable in virtualized environments. Only add this on bare metal.
You can check what's active with:

```bash
cat /sys/devices/system/clocksource/clocksource0/current_clocksource
```

### Sysctl — network buffers

Create `/etc/sysctl.d/99-echovr.conf`:

```ini
# UDP buffer sizes for game traffic (default max is ~212KB)
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576
net.core.netdev_max_backlog = 5000
```

Apply: `sysctl --system`

The `rmem_max`/`wmem_max` values set the ceiling applications can request via
`setsockopt(SO_RCVBUF)`. 16MB is standard for latency-sensitive UDP. The
`rmem_default`/`wmem_default` values ensure decent buffers without requiring
the application to explicitly request them.

### Transparent Huge Pages

Set THP to `madvise` to prevent `khugepaged` from causing latency spikes:

```bash
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
```

To persist across reboots, add a systemd tmpfiles rule or a line in
`/etc/rc.local`.

### File descriptor limits (required for esync)

Wine's esync uses one file descriptor per synchronization object. The default
limit of 1024 is far too low.

Create `/etc/security/limits.d/99-echovr.conf`:

```
*    soft    nofile    524288
*    hard    nofile    524288
root soft    nofile    524288
root hard    nofile    524288
```

Also set the systemd default in `/etc/systemd/system.conf.d/echovr.conf`:

```ini
[Manager]
DefaultLimitNOFILE=524288
```

And the same in `/etc/systemd/user.conf.d/echovr.conf`. Then:

```bash
systemctl daemon-reexec
```

### Timer slack (per-process)

The kernel's default timer slack of 50,000ns allows coalescing timer wakeups.
For a physics server running at 120Hz (~8.3ms frames), 50us slack is
measurable. Reduce it to 1ns per-process in the launch script:

```bash
echo 1 > /proc/self/timerslack_ns
```

**This is NOT a global sysctl.** `/proc/sys/kernel/timer_slack_ns` does not
exist. It is per-process at `/proc/<pid>/timerslack_ns`, or set via
`prctl(PR_SET_TIMERSLACK)`. The launch scripts handle this automatically.

## 7. Wine Setup

### Create Wine prefixes

Each server instance needs its own Wine prefix to avoid shared state conflicts:

```bash
export WINEDEBUG=-all

WINEPREFIX=/opt/echovr-server1/.wine WINEARCH=win64 wineboot --init
WINEPREFIX=/opt/echovr-server2/.wine WINEARCH=win64 wineboot --init
```

### Wine environment variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `WINEPREFIX` | `/opt/echovr-serverN/.wine` | Isolates each instance |
| `WINEARCH` | `win64` | 64-bit prefix (echovr.exe is x64) |
| `WINEDEBUG` | `-all` | Suppresses Wine debug output |
| `WINEESYNC` | `1` | Uses Linux eventfd for synchronization — reduces wineserver round-trips |

**On fsync:** `WINEFSYNC=1` uses futex2 (kernel 5.16+) for even better
synchronization performance than esync. However, it requires Wine-Staging or
Wine 9.4+. Ubuntu 24.04's stock Wine 9.0 does not support it. If you install
Wine-Staging, enable fsync instead of esync.

## 8. Systemd Services

### Per-instance launch scripts

Create `/opt/echovr-server1/run.sh` (and similarly for server2):

```bash
#!/bin/bash
set -euo pipefail

export WINEPREFIX=/opt/echovr-server1/.wine
export WINEARCH=win64
export WINEDEBUG=-all
export WINEESYNC=1

echo 1 > /proc/self/timerslack_ns 2>/dev/null || true

cd /opt/ready-at-dawn-echo-arena

exec wine ./bin/win10/echovr.exe \
    -noovr \
    -server \
    -headless \
    -exitonerror \
    -timestep 120 \
    -config-path ./_local/g.config.json
```

```bash
chmod +x /opt/echovr-server1/run.sh
chmod +x /opt/echovr-server2/run.sh
```

The only difference between instance scripts is the `WINEPREFIX` path.

### Templated systemd unit

Create `/etc/systemd/system/echovr-server@.service`:

```ini
[Unit]
Description=Echo VR Dedicated Server (Instance %i)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/echovr-server%i/run.sh
WorkingDirectory=/opt/ready-at-dawn-echo-arena
Restart=on-failure
RestartSec=10

Nice=-10
IOSchedulingClass=best-effort
IOSchedulingPriority=0

LimitNOFILE=524288

StandardOutput=journal
StandardError=journal
SyslogIdentifier=echovr-server%i

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
systemctl daemon-reload
systemctl enable --now echovr-server@1.service
systemctl enable --now echovr-server@2.service
```

## 9. Management

```bash
# Status
systemctl status echovr-server@1
systemctl status echovr-server@2

# Follow logs
journalctl -fu echovr-server@1
journalctl -fu echovr-server@2

# Restart
systemctl restart echovr-server@1

# Stop both
systemctl stop echovr-server@{1,2}

# Deploy new DLLs (stop first)
systemctl stop echovr-server@{1,2}
cp new-dlls/*.dll /opt/ready-at-dawn-echo-arena/bin/win10/
systemctl start echovr-server@{1,2}
```

## 10. Resource Usage

Each server instance uses approximately:

| Resource | Per Instance |
|----------|-------------|
| RAM | ~400MB |
| CPU | 1 core under load |
| Processes | ~10 (wine + game + wineserver + services) |
| File descriptors | ~500 (with esync) |

Two instances fit comfortably on a 4-core / 16GB VPS.

## 11. Troubleshooting

**Server exits immediately:** Check `journalctl -u echovr-server@1`. Common
causes: missing DLLs, bad config JSON, Nakama backend unreachable.

**"too many open files":** esync fd limits not applied. Verify with
`cat /proc/$(pgrep -f echovr)/limits | grep "Max open files"`. Must show
524288. May require a reboot after changing limits.conf.

**Wine GUI dialogs pop up:** Ensure `WINEDEBUG=-all` is set. The `-headless`
flag prevents the game from creating any windows, but Wine itself may try to
show crash dialogs. Setting `WINEDEBUG=-all` suppresses these.

**"kvm-clock" instead of TSC:** Expected on virtual machines. `kvm-clock` is
the correct and optimal clocksource for KVM guests — do not fight it.

**zstd decompression errors in logs:** These are harmless game-side warnings
about config documents that aren't zstd-compressed. They do not affect server
operation.

**Both instances bind the same port:** The game server gets its port assignment
from the Nakama backend via the serverdb connection, so port conflicts should
not occur in normal operation. If running without a backend, pass different
`-port` flags.
