# lumen-netman

The network manager for **AspisOS**, a capability-based, no-ambient-authority
operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel.

netman is a standalone client of the [lumen](https://github.com/AspisOS/lumen)
compositor, speaking the Lumen external window protocol (the same pattern as
settings and calculator). It is a component of the Lumen desktop, distributed as
a [herald](https://github.com/AspisOS/AspisOS) package and installed into the
`/apps` bundle tree.

## Role in the system

- A live network status panel for `eth0`: link state, IPv4 address, subnet mask
  (with prefix length), gateway, DNS and MAC, presented in a status card and a
  details card with a Refresh button.
- Network state is read from the kernel via `sys_netcfg` op=1 (syscall 500),
  whose result mirrors the kernel `netcfg_info_t` (MAC, IP, mask, gateway). DNS
  is read from the first `nameserver` line of `/etc/resolv.conf`.
- This version is read-only. Active control (DHCP renew / static configuration)
  needs the `NET_ADMIN` capability, which is intentionally **not** in herald's
  safe-package allowlist; a follow-up will add it via a small privileged helper.
- Auto-refreshes about once a second (a 16 ms event loop tick, every 60th tick),
  and on demand via the Refresh button or Enter / Space. Esc or the close request
  quits. The window is a fixed 460x388.

## Capabilities

netman's cap policy (`pkg/etc/aegis/caps.d/netman`) is:

```
service NET_SOCKET
```

- **NET_SOCKET** — required to query kernel network configuration via
  `sys_netcfg` op=1. This is the one capability beyond the baseline service
  profile, and it is what lets netman read the live `eth0` state it displays.

It holds no AUTH, SETUID or FB capability, and it does **not** hold `NET_ADMIN`
(so it cannot reconfigure the interface — see "Role in the system" above).

Although its herald package id (`lumen-netman`) differs from the bundle/exec name
(`netman`) and it installs an `/apps` binary plus a cap policy, that naming and
cross-tree install make it a `class=system` package: first-party and
signature-trusted, installed verbatim by herald.

## Building

netman fetches a pinned [glyph](https://github.com/AspisOS/glyph) toolkit
artifact (the GUI libraries it links: glyph + libaudio + libauth) and builds
against it, then packs a signed herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the toolkit release fetched by `tools/fetch-glyph.sh`.
- `MUSL_CC` is the musl cross-compiler (the only toolchain assumption — point it
  at an Aegis-native `cc` to build on-device in the future).
- `HERALD_KEY` signs the `.hpkg`.

Output: `lumen-netman.hpkg` (a `class=system` herald package) +
`lumen-netman.hpkg.sig`.

## Package payload

```
/apps/netman/netman                  the network manager binary
/apps/netman/app.ini                 the Lumen app-bundle manifest (name=Network Manager, exec=netman)
/etc/aegis/caps.d/netman             its capability policy (service NET_SOCKET)
```

## Repository layout

```
src/        netman source
pkg/        install-tree skeleton shipped verbatim (apps/ bundle + caps.d)
tools/      fetch-glyph.sh (toolkit fetch) + pack.sh (build the signed .hpkg)
Makefile    fetch toolkit -> build -> pack
VERSION         this component's version
GLYPH_VERSION   the pinned glyph toolkit version it builds against
```

## Dependencies

`depends=lumen` — netman is a client of the
[lumen](https://github.com/AspisOS/lumen) compositor, so installing it pulls
lumen (which in turn provides the desktop fonts).

## Sibling components

- [bastion](https://github.com/AspisOS/bastion) — display manager / login greeter
- [lumen-imageviewer](https://github.com/AspisOS/lumen-imageviewer) — image viewer
- [lumen-sysmon](https://github.com/AspisOS/lumen-sysmon) — system monitor
