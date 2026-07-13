/* user/bin/netman/main.c — Aegis Network Manager (external Lumen client)
 *
 * A live network status panel: shows the eth0 link state, IPv4 address,
 * subnet, gateway, DNS and MAC, refreshing automatically. Speaks the Lumen
 * external window protocol (same pattern as settings / calculator).
 *
 * Network state is read from the kernel via sys_netcfg op=1 (needs the
 * NET_SOCKET capability) plus /etc/resolv.conf for DNS. This v1 is read-only;
 * active control (DHCP renew / static config) needs the NET_ADMIN capability,
 * which is intentionally NOT in herald's safe-package allowlist — a follow-up
 * will add it via a small privileged helper.
 *
 * Shipped built-in under /apps/netman AND packaged as a herald .hpkg so it can
 * be updated from the repo.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>

#include <glyph.h>
#include <lumen_client.h>
#include "font.h"

#define SYS_NETCFG 500

/* Mirrors kernel netcfg_info_t (kernel/syscall/sys_socket.c). */
typedef struct {
    uint8_t  mac[6];
    uint8_t  pad[2];
    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
} netcfg_info_t;

/* Mirrors kernel wifi_net_pub_t (sys_socket.c / iwl_ax200.h). */
typedef struct {
    char    ssid[33];
    uint8_t channel;
    uint8_t sec;         /* 0 = open, 1 = secured */
    uint8_t connected;
    uint8_t pad;
} wifi_net_t;
#define WIFI_MAX 24

/* ── Layout ───────────────────────────────────────────────────────────── */

#define WIN_W       460
#define WIN_H       600
#define PAD         SP_4
#define CARD_R      R_MD
#define CARD_PAD    SP_4
#define ROW_H       (TYPE_BODY + SP_3)

/* ── State ────────────────────────────────────────────────────────────── */

static struct {
    int           lfd;
    lumen_window_t *lwin;
    surface_t     surf;
    int           fb_w, fb_h;
    int           dirty, done;

    netcfg_info_t net;
    int           have_net;
    char          dns[64];

    wifi_net_t    wifi[WIFI_MAX];
    int           wifi_n;         /* -1 = no adapter, >=0 = count */

    int           refresh_x, refresh_y, refresh_w, refresh_h;
} g_st;

static volatile sig_atomic_t s_term = 0;
static void sigterm_handler(int s) { (void)s; s_term = 1; }

/* ── Top-bar menu ─────────────────────────────────────────────────────── */

static int refresh_state(void);

enum { CMD_RESCAN = 1, CMD_CLOSE };

static void publish_menu(void)
{
    lumen_set_menu_t m;
    glyph_menu_reset(&m, g_st.lwin->id);
    int net = glyph_menu_add_col(&m, "Network");
    glyph_menu_add_item(&m, net, "Rescan", CMD_RESCAN);
    int file = glyph_menu_add_col(&m, "File");
    glyph_menu_add_item(&m, file, "Close", CMD_CLOSE);
    lumen_window_set_menu(g_st.lwin, &m);
}

static void menu_invoke(uint32_t cmd)
{
    switch (cmd) {
    case CMD_RESCAN: if (refresh_state()) g_st.dirty = 1; break;
    case CMD_CLOSE:  g_st.done = 1; break;
    }
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static int text_w(int sz, const char *s)
{
    if (g_font_ui) return font_text_width(g_font_ui, sz, s);
    return glyph_text_width(s);
}

static void draw_text_sz(int sz, int x, int y, const char *s, uint32_t color)
{
    if (g_font_ui) font_draw_text(&g_st.surf, g_font_ui, sz, x, y, s, color);
    else           draw_text_t(&g_st.surf, x, y, s, color);
}

/* network-byte-order u32 -> dotted quad (bytes are MSB-first in memory) */
static void fmt_ip(uint32_t addr, char *out, size_t n)
{
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static int mask_to_prefix(uint32_t mask)
{
    int bits = 0;
    const uint8_t *b = (const uint8_t *)&mask;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            if (b[i] & (1u << k)) bits++;
    return bits;
}

/* Read the first nameserver from /etc/resolv.conf into out. */
static void read_dns(char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10) == 0) {
            char *p = line + 10;
            while (*p == ' ' || *p == '\t') p++;
            char *e = p;
            while (*e && *e != '\n' && *e != '\r' && *e != ' ') e++;
            *e = '\0';
            strncpy(out, p, n - 1);
            out[n - 1] = '\0';
            break;
        }
    }
    fclose(f);
}

/* Re-read all network state. Returns 1 if anything changed since last read. */
static int refresh_state(void)
{
    netcfg_info_t prev = g_st.net;
    int prev_have = g_st.have_net;
    char prev_dns[64];
    memcpy(prev_dns, g_st.dns, sizeof(prev_dns));

    int prev_wifi_n = g_st.wifi_n;

    memset(&g_st.net, 0, sizeof(g_st.net));
    g_st.have_net = (syscall(SYS_NETCFG, 1, (long)&g_st.net, 0, 0) == 0);
    read_dns(g_st.dns, sizeof(g_st.dns));

    /* op=2: read the Wi-Fi scan list (needs NET_SOCKET; ENODEV if no adapter). */
    long n = syscall(SYS_NETCFG, 2, (long)g_st.wifi, WIFI_MAX, 0);
    g_st.wifi_n = (n >= 0) ? (int)n : -1;

    return prev_have != g_st.have_net ||
           memcmp(&prev, &g_st.net, sizeof(prev)) != 0 ||
           strcmp(prev_dns, g_st.dns) != 0 ||
           prev_wifi_n != g_st.wifi_n;
}

/* ── Render ───────────────────────────────────────────────────────────── */

static int draw_card(int x, int y, int w, int h)
{
    draw_rounded_rect(&g_st.surf, x, y, w, h, CARD_R, THEME_SURFACE_2);
    return y + CARD_PAD;
}

/* label/value row inside a card */
static int kv_row(int x, int w, int y, const char *label, const char *value,
                  uint32_t vcolor)
{
    draw_text_sz(TYPE_BODY, x + CARD_PAD, y, label, THEME_TEXT_DIM);
    int vw = text_w(TYPE_BODY, value);
    draw_text_sz(TYPE_BODY, x + w - CARD_PAD - vw, y, value, vcolor);
    return y + ROW_H;
}

/* small padlock glyph: a body rect + a shackle outline above it */
static void draw_lock(int x, int y, uint32_t col)
{
    surface_t *s = &g_st.surf;
    draw_rounded_outline(s, x + 2, y, 6, 7, 2, 1, col);   /* shackle */
    draw_rounded_rect(s, x, y + 4, 10, 7, 2, col);        /* body */
}

static void draw_button(int x, int y, int w, int h, const char *label,
                        uint32_t bg)
{
    surface_t *s = &g_st.surf;
    draw_rounded_rect(s, x, y, w, h, R_SM, bg);
    draw_rounded_outline(s, x, y, w, h, R_SM, 1, THEME_BORDER);
    int tw = text_w(TYPE_BODY, label);
    draw_text_sz(TYPE_BODY, x + (w - tw) / 2, y + (h - TYPE_BODY) / 2 - 1,
                 label, THEME_TEXT_ON_ACCENT);
}

static void render(void)
{
    if (!g_st.dirty) return;
    g_st.dirty = 0;

    surface_t *s = &g_st.surf;
    int cw = WIN_W - 2 * PAD;
    draw_fill_rect(s, 0, 0, g_st.fb_w, g_st.fb_h, THEME_SURFACE);

    int connected = g_st.have_net && g_st.net.ip != 0;

    /* ── Status card ── */
    int sc_h = 72;
    draw_card(PAD, PAD, cw, sc_h);
    int dot_cx = PAD + CARD_PAD + 8;
    int dot_cy = PAD + sc_h / 2;
    draw_circle_filled(s, dot_cx, dot_cy, 8,
                       connected ? THEME_OK : THEME_TEXT_FAINT);
    int tx = dot_cx + 8 + SP_4;
    draw_text_sz(TYPE_TITLE, tx, PAD + SP_4,
                 connected ? "Connected" : "Not connected", THEME_TEXT);
    draw_text_sz(TYPE_CAPTION, tx, PAD + SP_4 + TYPE_TITLE + SP_1,
                 "eth0  \xc2\xb7  Ethernet", THEME_TEXT_DIM);

    /* ── Details card ── */
    int dc_y = PAD + sc_h + SP_4;
    int dc_h = CARD_PAD + 5 * ROW_H + CARD_PAD - SP_1;
    int y = draw_card(PAD, dc_y, cw, dc_h);

    char buf[64], buf2[80];
    if (connected) {
        fmt_ip(g_st.net.ip, buf, sizeof(buf));
        y = kv_row(PAD, cw, y, "IPv4 Address", buf, THEME_TEXT);
        fmt_ip(g_st.net.mask, buf, sizeof(buf));
        snprintf(buf2, sizeof(buf2), "%s  (/%d)", buf,
                 mask_to_prefix(g_st.net.mask));
        y = kv_row(PAD, cw, y, "Subnet Mask", buf2, THEME_TEXT);
        fmt_ip(g_st.net.gateway, buf, sizeof(buf));
        y = kv_row(PAD, cw, y, "Gateway", buf, THEME_TEXT);
        y = kv_row(PAD, cw, y, "DNS", g_st.dns[0] ? g_st.dns : "—", THEME_TEXT);
    } else {
        y = kv_row(PAD, cw, y, "IPv4 Address", "—", THEME_TEXT_DIM);
        y = kv_row(PAD, cw, y, "Subnet Mask", "—", THEME_TEXT_DIM);
        y = kv_row(PAD, cw, y, "Gateway", "—", THEME_TEXT_DIM);
        y = kv_row(PAD, cw, y, "DNS", "—", THEME_TEXT_DIM);
    }
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             g_st.net.mac[0], g_st.net.mac[1], g_st.net.mac[2],
             g_st.net.mac[3], g_st.net.mac[4], g_st.net.mac[5]);
    (void)kv_row(PAD, cw, y, "MAC Address", buf, THEME_TEXT);

    /* ── Refresh button (position first so the Wi-Fi list can size to fit) ── */
    int bw = 120, bh = 36;
    int bx = PAD;
    int by = WIN_H - PAD - bh;

    /* ── Wi-Fi card ── */
    int wc_y = dc_y + dc_h + SP_4;
    int wc_h = by - SP_4 - wc_y;
    draw_card(PAD, wc_y, cw, wc_h);
    draw_text_sz(TYPE_CAPTION, PAD + CARD_PAD, wc_y + CARD_PAD,
                 "WI-FI NETWORKS", THEME_TEXT_DIM);
    int list_y = wc_y + CARD_PAD + TYPE_CAPTION + SP_3;
    int row_step = ROW_H + 2;
    int avail_rows = (wc_y + wc_h - CARD_PAD - list_y) / row_step;

    if (g_st.wifi_n < 0) {
        draw_text_sz(TYPE_BODY, PAD + CARD_PAD, list_y,
                     "No Wi-Fi adapter detected", THEME_TEXT_FAINT);
    } else if (g_st.wifi_n == 0) {
        draw_text_sz(TYPE_BODY, PAD + CARD_PAD, list_y,
                     "No networks found", THEME_TEXT_FAINT);
    } else {
        int rows = g_st.wifi_n < avail_rows ? g_st.wifi_n : avail_rows;
        for (int i = 0; i < rows; i++) {
            wifi_net_t *w = &g_st.wifi[i];
            int ry = list_y + i * row_step;
            if (w->connected)
                draw_circle_filled(s, PAD + CARD_PAD + 4, ry + TYPE_BODY / 2, 4, THEME_OK);
            draw_text_sz(TYPE_BODY, PAD + CARD_PAD + 16, ry, w->ssid,
                         w->connected ? THEME_OK : THEME_TEXT);
            /* right side: optional "ch N", then a lock for secured networks */
            int rx = PAD + cw - CARD_PAD;
            if (w->channel > 0) {
                char chb[16];
                snprintf(chb, sizeof(chb), "ch %d", w->channel);
                int chw = text_w(TYPE_CAPTION, chb);
                rx -= chw;
                draw_text_sz(TYPE_CAPTION, rx, ry + 1, chb, THEME_TEXT_FAINT);
                rx -= SP_3;
            }
            if (w->sec)
                draw_lock(rx - 12, ry, THEME_TEXT_DIM);
        }
        if (g_st.wifi_n > rows) {
            char more[24];
            snprintf(more, sizeof(more), "+%d more", g_st.wifi_n - rows);
            draw_text_sz(TYPE_CAPTION, PAD + CARD_PAD, list_y + rows * row_step,
                         more, THEME_TEXT_FAINT);
        }
    }

    g_st.refresh_x = bx; g_st.refresh_y = by;
    g_st.refresh_w = bw; g_st.refresh_h = bh;
    draw_button(bx, by, bw, bh, "Refresh", THEME_ACCENT);

    const char *hint = connected
        ? "Managed by DHCP. Configuration is automatic."
        : "No lease. Check the cable or DHCP server.";
    draw_text_sz(TYPE_CAPTION, bx + bw + SP_4,
                 by + (bh - TYPE_CAPTION) / 2 - 1, hint, THEME_TEXT_DIM);

    lumen_window_present(g_st.lwin);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    g_st.lfd = lumen_connect_retry();
    if (g_st.lfd < 0) {
        dprintf(2, "[NETMAN] lumen_connect failed (%d)\n", g_st.lfd);
        return 1;
    }

    g_st.lwin = lumen_window_create(g_st.lfd, "Network Manager", WIN_W, WIN_H);
    if (!g_st.lwin) {
        dprintf(2, "[NETMAN] lumen_window_create failed\n");
        close(g_st.lfd);
        return 1;
    }
    g_st.fb_w = g_st.lwin->w;
    g_st.fb_h = g_st.lwin->h;
    g_st.surf = (surface_t){
        .buf = (uint32_t *)g_st.lwin->backbuf,
        .w = g_st.fb_w, .h = g_st.fb_h, .pitch = g_st.lwin->stride,
    };

    font_init();
    publish_menu();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    refresh_state();
    g_st.dirty = 1;
    render();

    dprintf(2, "[NETMAN] connected %dx%d\n", g_st.lwin->w, g_st.lwin->h);

    int tick = 0;
    while (!s_term && !g_st.done) {
        lumen_event_t ev;
        int r = lumen_wait_event(g_st.lfd, &ev, 16);
        if (r < 0) break;

        if (r == 1) {
            if (ev.type == LUMEN_EV_CLOSE_REQUEST) break;
            if (ev.type == LUMEN_EV_MENU_INVOKE) menu_invoke(ev.menu.command);
            if (ev.type == LUMEN_EV_KEY && ev.key.pressed) {
                char k = (char)ev.key.keycode;
                if (k == '\x1b') break;
                if (k == '\r' || k == '\n' || k == ' ') {
                    if (refresh_state()) g_st.dirty = 1;
                }
            }
            if (ev.type == LUMEN_EV_MOUSE &&
                ev.mouse.evtype == LUMEN_MOUSE_DOWN &&
                (ev.mouse.buttons & 1) &&
                hit(ev.mouse.x, ev.mouse.y, g_st.refresh_x, g_st.refresh_y,
                    g_st.refresh_w, g_st.refresh_h)) {
                if (refresh_state()) g_st.dirty = 1;
            }
        } else {
            /* Timeout tick (~16ms). Auto-refresh once a second. */
            if (++tick >= 60) {
                tick = 0;
                if (refresh_state()) g_st.dirty = 1;
            }
        }
        render();
    }

    lumen_window_destroy(g_st.lwin);
    close(g_st.lfd);
    dprintf(2, "[NETMAN] exit\n");
    return 0;
}
