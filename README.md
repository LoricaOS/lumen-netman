# lumen-netman

The network status panel for **AspisOS**, a capability-based,
no-ambient-authority x86-64 operating system built on the from-scratch
[Aegis](https://github.com/AspisOS/Aegis) kernel. netman shows the live `eth0`
configuration — link state, IPv4 address, subnet, gateway, DNS, and MAC — and
refreshes automatically. It is a standalone client of the
[lumen](https://github.com/AspisOS/lumen) compositor, distributed as a
[herald](https://github.com/AspisOS/AspisOS) system package and installed into
the `/apps` bundle tree.

## Where netman fits

AspisOS is decomposed into independent repositories. netman is a leaf: a GUI
application that talks to the compositor and queries the kernel for network
state.

| Repo | Role |
|------|------|
| [`AspisOS/Aegis`](https://github.com/AspisOS/Aegis) | The kernel. Provides `sys_netcfg` (the network-config query), `AF_UNIX` sockets, `memfd`, the filesystem, and the capability model. |
| [`AspisOS/lumen`](https://github.com/AspisOS/lumen) | The compositor / display server. netman's window is a proxy surface owned by lumen. |
| [`AspisOS/glyph`](https://github.com/AspisOS/glyph) | The GUI toolkit. Supplies the software renderer (`draw_*`, `draw_rounded_rect`, themed `THEME_*` colors) and the **client side of lumen's window protocol** (`lumen_client.h`). |
| `AspisOS/lumen-netman` | **This repo.** An external Lumen client, the same pattern as settings and the calculator. |

netman does not touch the framebuffer or input devices. It connects to lumen
over `/run/lumen.sock`, receives a shared `memfd` to draw into, and gets input
events back. In herald terms it declares `depends=lumen`.

## What it does

`main()` (`src/main.c`) connects to lumen, creates a fixed 460×388 window titled
"Network Manager", and runs a 16 ms-timeout event loop. The UI is two themed
cards — a status card (a colored dot plus "Connected" / "Not connected" for
`eth0`) and a details card — with a Refresh button.

- **Network state.** Read from the kernel via `sys_netcfg` op=1 (syscall 500),
  whose result mirrors the kernel `netcfg_info_t` (MAC, IP, mask, gateway). The
  subnet mask is also shown as a prefix length (`/24`). DNS is read from the
  first `nameserver` line of `/etc/resolv.conf`. IPv4 fields are formatted from
  network-byte-order `u32`s.
- **Refresh.** Auto-refreshes about once a second (the 16 ms loop refreshes every
  60th tick), and on demand via the Refresh button or Enter / Space. A redraw is
  only triggered when the state actually changes. Esc or a close request quits.
- **Disconnected state.** When `sys_netcfg` fails or the IP is zero, the card
  reads "Not connected" and the detail rows show em dashes, with a hint to check
  the cable or DHCP server.

## Status

netman is **early-stage** and deliberately **read-only**. It can *display* the
interface configuration but cannot change it: active control — DHCP renew, static
addressing, bringing a link up or down — requires the `NET_ADMIN` capability,
which is intentionally **not** in herald's safe-package allowlist (see
Capabilities). A follow-up will add real network configuration through a small
privileged helper once a vetted privileged path exists; until then netman is a
status panel only. The "Managed by DHCP" hint reflects that configuration is
currently automatic and not user-editable here.

## Capabilities

AspisOS has **no ambient authority**: a process can do nothing except through
capabilities granted by kernel policy at exec time. netman's policy
(`pkg/etc/aegis/caps.d/netman`, installed to `/etc/aegis/caps.d/netman`) is:

```
service NET_SOCKET
```

- **`service`** — the baseline profile; lets netman connect to the compositor
  over the Lumen socket.
- **`NET_SOCKET`** — the one capability beyond baseline. It authorizes the
  `sys_netcfg` op=1 query that reads the live `eth0` state netman displays.

netman holds no `AUTH`, `SETUID`, or `FB` capability, and crucially it does
**not** hold `NET_ADMIN` — so it cannot reconfigure the interface. That omission
is intentional: it is what keeps netman a safe, signature-trusted package and is
the reason this version is read-only (see Status).

## Building

netman builds with a musl cross-compiler against the prebuilt glyph toolkit. The
`Makefile` fetches a pinned toolkit artifact, compiles `src/*.c` against it, and
packs a signed herald package.

```sh
make MUSL_CC=/path/to/musl-gcc HERALD_KEY=/path/to/signing.key
```

- `GLYPH_VERSION` pins the [glyph](https://github.com/AspisOS/glyph) toolkit
  release fetched by `tools/fetch-glyph.sh` (it unpacks `include/` and `lib/`
  into `toolkit/`).
- `MUSL_CC` is the musl cross-compiler (defaults to `musl-gcc` on `PATH`; the
  only toolchain assumption — point it at an Aegis-native `cc` to build on-device
  in the future).
- `HERALD_KEY` is the ECDSA P-256 signing key for the package.
- The link line pulls all four toolkit archives (`-lcitadel -laudio -lauth
  -lglyph`); only the objects actually referenced are contributed.

Output: `lumen-netman.hpkg` (a `class=system` herald package) +
`lumen-netman.hpkg.sig`.

## Package payload

`lumen-netman.hpkg` is a manifest-first, uncompressed POSIX `ustar` archive with
a detached ECDSA-P256/SHA-256 signature (`tools/pack.sh`). The herald package
**id** (`lumen-netman`) differs from the bundle/exec name (`netman`), and the
package installs an `/apps` binary alongside a cap policy under `/etc` — so it is
a `class=system` package: first-party, signature-trusted, installed verbatim by
herald.

```
manifest                          id=lumen-netman, class=system, depends=lumen
apps/netman/netman                the network panel binary (stripped)
apps/netman/app.ini               the Lumen app-bundle manifest (name=Network Manager, exec=netman)
etc/aegis/caps.d/netman           its capability policy (service NET_SOCKET)
```

## Repository layout

```
.
├── Makefile          fetch toolkit → build component.elf → pack the .hpkg
├── VERSION           this component's version
├── GLYPH_VERSION     pinned glyph toolkit artifact version
├── src/
│   └── main.c        sys_netcfg query, status/details cards, refresh, event loop
├── pkg/              install-tree skeleton shipped verbatim
│   ├── apps/netman/app.ini      the /apps bundle manifest
│   └── etc/aegis/caps.d/netman  the capability policy
└── tools/
    ├── fetch-glyph.sh   fetch + unpack the pinned glyph toolkit artifact
    └── pack.sh          build + sign lumen-netman.hpkg
```

Build outputs (`component.elf`, `*.hpkg`, `*.hpkg.sig`) and the fetched
`toolkit/` are git-ignored.

## Dependencies

`depends=lumen` — netman is a client of the
[lumen](https://github.com/AspisOS/lumen) compositor, so installing it pulls
lumen, which in turn ships the desktop fonts every Lumen client inherits. There
is no separate font package.

## Sibling components

- [bastion](https://github.com/AspisOS/bastion) — display manager / login greeter
- [lumen-imageviewer](https://github.com/AspisOS/lumen-imageviewer) — image viewer
- [lumen-sysmon](https://github.com/AspisOS/lumen-sysmon) — system monitor
