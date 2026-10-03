# Fixture: DHCP-dead AP on a shared SSID (Surabaya Suites, 2026-10-03)

Live capture of the failure the AP auto-steering feature targets. No secrets:
BSSIDs/SSIDs only, profile dumps redacted to non-secret fields.

## Environment

- Hotel "SURABAYA SUITES", Surabaya. Two AP systems share the SSID:
  - `24:A4:3C:8F:8E:5D` (ch 6, WPA2-only, strongest, ~74): accepts WPA2
    (4-way handshake completes) but never answers DHCP.
  - `06:2B:A6:5F:C0:6B` family (WPA1+WPA2, hotel managed system): healthy,
    DHCP `192.168.8.0/22` via `192.168.10.254`.
- Laptop: `wlp55s0` (rtw89_8852ce), NetworkManager 1.52, Ubuntu 25.10.
- Forcing: profile `bssid` pinned to the dead AP + `nmcli connection up`.

## D-Bus signature (the matcher input)

Device `/org/freedesktop/NetworkManager/Devices/2` signal sequence per cycle:

```
StateChanged (110, 100, 60)   # deactivating; 60 = new-activation (our reactivation)
StateChanged (30, 110, 60)    # disconnected
StateChanged (40, 30, 0)      # prepare
StateChanged (50, 40, 0)      # config
StateChanged (60, 50, 0)      # need-auth (secrets exist, passes through)
StateChanged (70, 50, 0)      # ip-config (after 4-way handshake)
StateChanged (120, 70, 5)     # FAILED from IP_CONFIG, reason 5 = IP_CONFIG_UNAVAILABLE
StateChanged (30, 120, 0)     # back to disconnected; autoconnect retries immediately
```

ActiveConnection layer for the same failure (note the reason differs):

```
ActiveConnection/N: StateChanged (1, 0)   # activating
ActiveConnection/N: StateChanged (4, 3)   # deactivated, reason 3 = DEVICE_DISCONNECTED
```

Design consequence: the AC signal carries only the generic reason 3; the
specific L3 reason (5) exists only on the *device* signal. The monitor must
match the device triple `(120, 70, 5)`, not the AC outcome.

## D-Bus state during IP_CONFIG (attribution input, 5 s poll)

```
DevState: u 70 | Reason: (uu) 70 0 | AC: o "/org/.../ActiveConnection/20"
  ACState: u 1
  SpecificObject: o "/org/.../AccessPoint/77"
  AP-Hw: s "24:A4:3C:8F:8E:5D"
  AP-Str: y 100
  AP-Seen: i 2648
```

5 s polling never observed state 120 (FAILED→disconnected is sub-second);
signal subscription is required, polling is not a fallback.

## Journal signature

```
16:40:20 state change: config -> ip-config (reason 'none')
16:40:20 dhcp4 (wlp55s0): activation: beginning transaction (timeout in 45 seconds)
16:41:06 state change: ip-config -> failed (reason 'ip-config-unavailable')
16:41:06 Activation: failed for connection 'SURABAYA SUITES'
16:41:06 state change: failed -> disconnected (reason 'none')
16:41:06 Activation: starting connection 'SURABAYA SUITES'   # autoconnect retry, ~0.5 s later
```

Timing: assoc+auth took ~25 s this cycle (includes a scan); DHCP timeout is
exactly 45 s; NM retried with no visible backoff. One forced cycle spans
~70 s from `connection up` to FAILED.

## Raw captures (not committed, on the capture machine)

- `/tmp/step0-gdbus.log`: full `gdbus monitor` of NM signals (~160 s).
- `/tmp/step0-poll.log`: 5 s D-Bus property poll through the cycle.
- `/tmp/surabaya-*-backup-*.txt`: both SURABAYA SUITES profiles pre-test.
- `busctl monitor` was unusable unprivileged (`BecomeMonitor: Access denied`);
  `gdbus monitor --system --dest org.freedesktop.NetworkManager` worked.
