#include "spp.h"
/* Head-unit wireless AA bootstrap. See aa_boot_sm.h for the completion contract
 * and THIRD_PARTY_NOTICES.md for protocol provenance. */
#include "aa_boot_sm.h"
#include "../runtime/hif.h"

/* AA message IDs from the protocol reference listed in THIRD_PARTY_NOTICES.md. */
#define AA_ID_INFO_REQ 2u
#define AA_ID_VER_REQ 4u
#define AA_ID_VER_RESP 5u
#define AA_ID_CONN_STAT 6u
#define AA_ID_START_RESP 7u
/* Keepalive: WifiPingRequest / WifiPingResponse. */
#define AA_ID_PING_REQ 8u
#define AA_ID_PING_RESP 9u
#define AA_PING_MAX 32u /* a PingRequest is one varint timestamp */

static void boot_fail(struct aa_boot *b, int st) { b->state = st; }

/* u32 monotonic subtraction that tolerates wraparound: returns 1 iff now has reached deadline. */
static int reached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

/* Flush any pending TX bounded-pump so the frame we just queued is fully emitted this cycle. */
static void boot_flush_tx(struct aa_boot *b) {
    uint32_t guard = 0;
    while ((waa_spp_status(b->inst) & SPP_F_TXPEND) &&
           !(waa_spp_status(b->inst) & (SPP_F_ERROR | SPP_F_FLOW)) && guard++ < 4096u)
        waa_spp_tick(b->hl, b->inst);
}

static int send_frame(struct aa_boot *b, const uint8_t *frame, uint32_t n) {
    int rc;
    if (!n) {
        boot_fail(b, AA_BOOT_ERROR);
        return -1;
    }
    rc = waa_spp_send(b->hl, b->inst, frame, n);
    if (rc != 0) {
        boot_fail(b, AA_BOOT_ERROR);
        return -1;
    }
    boot_flush_tx(b);
    return 0;
}

int aa_boot_init(struct aa_boot *b, void *hl, uint32_t inst, const struct aa_wifi_cfg *cfg,
                 uint32_t bt_timeout_ms, uint32_t tcp_timeout_ms) {
    uint32_t i;
    uint8_t *p = (uint8_t *)b;
    if (!b || !cfg)
        return -1;
    if (!cfg->ip_len || cfg->ip_len > AA_CFG_MAX)
        return -3;
    if (!cfg->ssid_len || cfg->ssid_len > 32u)
        return -3;
    if (cfg->key_len > AA_CFG_MAX || cfg->bssid_len > AA_CFG_MAX)
        return -3;
    for (i = 0; i < sizeof(*b); i++)
        p[i] = 0;
    b->hl = hl;
    b->inst = inst;
    b->cfg = cfg;
    b->bt_timeout_ms = bt_timeout_ms ? bt_timeout_ms : 1;
    b->tcp_timeout_ms = tcp_timeout_ms ? tcp_timeout_ms : 1;
    return 0;
}

int aa_boot_start(struct aa_boot *b, uint32_t now_ms, uint16_t listener_port) {
    uint8_t frame[64 + AA_CFG_MAX];
    struct aa_bytes ip;
    uint32_t n;
    if (!b)
        return AA_BOOT_ERROR;
    if (b->state != AA_BOOT_IDLE) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    if (!listener_port) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    if (!waa_spp_ready(b->inst)) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    ip.data = b->cfg->ip;
    ip.size = b->cfg->ip_len;
    n = aa_start_frame(frame, sizeof(frame), ip, listener_port);
    if (!n) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    Log(b->hl, 1, "AA_BOOT: -> WifiStartRequest\n");
    if (send_frame(b, frame, n) != 0)
        return b->state;
    b->state = AA_BOOT_START_SENT;
    b->deadline_ms = now_ms + b->bt_timeout_ms;
    return b->state;
}

static int send_wifi_info(struct aa_boot *b) {
    uint8_t frame[64 + 4 * AA_CFG_MAX];
    struct aa_wifi w;
    uint32_t n;
    w.ssid.data = b->cfg->ssid;
    w.ssid.size = b->cfg->ssid_len;
    w.key.data = b->cfg->key;
    w.key.size = b->cfg->key_len;
    w.bssid.data = b->cfg->bssid;
    w.bssid.size = b->cfg->bssid_len;
    w.security = b->cfg->security;
    w.ap_type = b->cfg->ap_type;
    n = aa_wifi_frame(frame, sizeof(frame), &w);
    if (!n) {
        boot_fail(b, AA_BOOT_ERROR);
        return -1;
    }
    /* key is intentionally NOT a log argument */
    Log(b->hl, 1, "AA_BOOT: -> WifiInfoResponse ssid_bytes=%u sec=%u ap=%u (key redacted)\n",
        b->cfg->ssid_len, b->cfg->security, b->cfg->ap_type);
    return send_frame(b, frame, n);
}

/* Echo WifiPingRequest as WifiPingResponse. Unlike send_frame, a failure is only logged. */
static void answer_ping(struct aa_boot *b, const unsigned char *payload, unsigned int len) {
    uint8_t frame[4 + AA_PING_MAX];
    uint32_t n = 0;
    if (len <= AA_PING_MAX)
        n = aa_raw_frame(frame, sizeof(frame), AA_ID_PING_RESP, payload, len);
    if (!n || waa_spp_send(b->hl, b->inst, frame, n) != 0) {
        Log(b->hl, 1, "AA_BOOT: WifiPingResponse not sent (%u B)\n", len);
        return;
    }
    boot_flush_tx(b);
}

/* aa_rx_feed callback: one complete AA frame (id + opaque payload). NEVER completes the SM. */
static int on_frame(void *ctx, unsigned int id, const unsigned char *payload, unsigned int len) {
    struct aa_boot *b = (struct aa_boot *)ctx;
    switch (b->state) {
    case AA_BOOT_START_SENT:
        if (id != AA_ID_INFO_REQ) {
            boot_fail(b, AA_BOOT_ERROR);
            return -1;
        }
        Log(b->hl, 1, "AA_BOOT: <- WifiInfoRequest (%u B, opaque)\n", len);
        if (send_wifi_info(b) != 0)
            return -1;
        b->state = AA_BOOT_WAIT_TCP; /* BT role done; now wait for the TCP accept */
        return 0;
    case AA_BOOT_WAIT_TCP:
        /* Post-WifiInfoResponse frames per the enum. Their payload schemas are NOT recovered, so
         * they are logged OPAQUE and do NOT mean success (success == TCP accept). An id outside the
         * known enum is a protocol fault. */
        if (id == AA_ID_PING_REQ) {
            Log(b->hl, 1, "AA_BOOT: <- WifiPingRequest (%u B) -> WifiPingResponse\n", len);
            answer_ping(b, payload, len);
            return 0; /* keepalive; stay in WAIT_TCP */
        }
        if (id == AA_ID_VER_REQ || id == AA_ID_VER_RESP || id == AA_ID_CONN_STAT ||
            id == AA_ID_START_RESP) {
            Log(b->hl, 1, "AA_BOOT: <- id=%u (%u B, OPAQUE/UNKNOWN; not a success)\n", id, len);
            return 0; /* stay in WAIT_TCP */
        }
        boot_fail(b, AA_BOOT_ERROR);
        return -1;
    default:
        boot_fail(b, AA_BOOT_ERROR);
        return -1;
    }
}

int aa_boot_poll(struct aa_boot *b, uint32_t now_ms) {
    uint8_t buf[256];
    uint32_t n, st;
    int prev;
    if (!b)
        return AA_BOOT_ERROR;
    if (b->state == AA_BOOT_DONE || b->state == AA_BOOT_ERROR || b->state == AA_BOOT_TIMEOUT)
        return b->state;
    st = waa_spp_status(b->inst);
    if (st & SPP_F_ERROR) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    prev = b->state;
    for (;;) {
        n = waa_spp_read(b->inst, buf, sizeof(buf));
        if (!n)
            break;
        if (aa_rx_feed(&b->rx, buf, n, on_frame, b) != 0) {
            if (b->state != AA_BOOT_ERROR)
                boot_fail(b, AA_BOOT_ERROR);
            return b->state;
        }
        if (b->state == AA_BOOT_ERROR)
            return b->state;
    }
    /* on the START_SENT -> WAIT_TCP transition, rebase the deadline to the TCP-accept budget */
    if (prev == AA_BOOT_START_SENT && b->state == AA_BOOT_WAIT_TCP)
        b->deadline_ms = now_ms + b->tcp_timeout_ms;
    /* explicit-time deadline while waiting for the peer / the TCP accept */
    if ((b->state == AA_BOOT_START_SENT || b->state == AA_BOOT_WAIT_TCP) &&
        reached(now_ms, b->deadline_ms))
        boot_fail(b, AA_BOOT_TIMEOUT);
    return b->state;
}

int aa_boot_tcp_accepted(struct aa_boot *b) {
    if (!b)
        return AA_BOOT_ERROR;
    if (b->state != AA_BOOT_WAIT_TCP) {
        boot_fail(b, AA_BOOT_ERROR);
        return b->state;
    }
    b->state = AA_BOOT_DONE;
    Log(b->hl, 1, "AA_BOOT: TCP accepted -> handed to SAL (DONE)\n");
    return b->state;
}

int aa_boot_state(const struct aa_boot *b) { return b ? b->state : AA_BOOT_ERROR; }

/* aa_rx_feed callback after bootstrap: logs id/length only and never fails the parser. */
static int on_late_frame(void *ctx, unsigned int id, const unsigned char *payload,
                         unsigned int len) {
    struct aa_boot *b = (struct aa_boot *)ctx;
    if (b->listen_only) {
        Log(b->hl, 1, "AA_BOOT: <- id=%u (%u B) on reconnected channel; not answered\n", id, len);
        return 0;
    }
    if (id == AA_ID_PING_REQ) {
        Log(b->hl, 1, "AA_BOOT: <- WifiPingRequest (%u B) after bootstrap -> WifiPingResponse\n",
            len);
        answer_ping(b, payload, len);
        return 0;
    }
    Log(b->hl, 1, "AA_BOOT: <- id=%u (%u B) after bootstrap; ignored\n", id, len);
    return 0;
}

void aa_boot_serve(struct aa_boot *b) {
    uint8_t buf[256];
    uint32_t n, guard = 0;
    if (!b || (b->state != AA_BOOT_WAIT_TCP && b->state != AA_BOOT_DONE) || b->rx.failed ||
        !waa_spp_ready(b->inst))
        return;
    /* bounded per pump: the SPP RX buffer holds at most 4096 bytes */
    while (guard++ < 16u && (n = waa_spp_read(b->inst, buf, sizeof(buf))) != 0) {
        if (aa_rx_feed(&b->rx, buf, n, on_late_frame, b) != 0) {
            Log(b->hl, 1, "AA_BOOT: malformed frame after bootstrap; channel no longer parsed\n");
            return;
        }
    }
}

void aa_boot_rechannel(struct aa_boot *b) {
    if (!b)
        return;
    aa_rx_reset(&b->rx);
    b->listen_only = 1;
    if (b->state == AA_BOOT_WAIT_TCP || b->state == AA_BOOT_DONE)
        Log(b->hl, 1, "AA_BOOT: SPP channel reopened after bootstrap; logging frames only\n");
}
