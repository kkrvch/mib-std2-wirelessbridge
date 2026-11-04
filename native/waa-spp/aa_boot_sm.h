#ifndef AA_BOOT_SM_H
#define AA_BOOT_SM_H
/* Head-unit wireless AA bootstrap over the SPP byte transport. Protocol sources
 * and their MIT notice are recorded in THIRD_PARTY_NOTICES.md. Framing is
 * [payload length u16 BE][message ID u16 BE][protobuf payload].
 * Send WifiStartRequest, receive WifiInfoRequest, then send WifiInfoResponse.
 * Other message payloads are opaque; only TCP acceptance completes bootstrap.
 * WifiPingRequest is answered by echoing its payload as WifiPingResponse.
 * Bind the head-unit TCP listener before starting. Deadlines use caller-supplied
 * monotonic milliseconds, not the number of polling calls. */
#include <stdint.h>
#include "../aa-bootstrap/aa_bootstrap.h"

enum aa_boot_state {
    AA_BOOT_IDLE = 0,   /* not started                                                         */
    AA_BOOT_START_SENT, /* WifiStartRequest sent; awaiting WifiInfoRequest                      */
    AA_BOOT_WAIT_TCP,   /* WifiInfoResponse sent; BT role done; awaiting TCP accept (NOT a BT ev)*/
    AA_BOOT_DONE,       /* TCP accepted & handed to SAL (success) — set ONLY by the adapter     */
    AA_BOOT_ERROR,      /* transport error, malformed/oversized/unexpected BT frame, bad config */
    AA_BOOT_TIMEOUT     /* a WifiInfoRequest or the TCP accept did not arrive before the deadline*/
};

#define AA_CFG_MAX 64u /* bounded fixed buffers; no secrets are logged by this module */

/* Wi-Fi parameters supplied by the integration via an explicit adapter (not read from globals).
 * Scalars first (aligned), then byte arrays, so the layout has no interior padding:
 * ip_len@0 ssid_len@4 key_len@8 bssid_len@12 security@16 ap_type@20 port@24 _pad@26
 * ip@28 ssid@92 key@156 bssid@220 ; sizeof==284. port here is a default/fallback; the adapter
 * overrides it with the confirmed listener port at aa_boot_start(). */
struct aa_wifi_cfg {
    uint32_t ip_len, ssid_len, key_len, bssid_len;
    uint32_t security; /* AA SecurityMode enum (validated by codec)*/
    uint32_t ap_type;  /* AA AccessPointType enum (0 static/1 dyn) */
    uint16_t port;     /* fallback HU TCP listen port              */
    uint8_t _pad[2];
    uint8_t ip[AA_CFG_MAX]; /* HU TCP listen IP, ASCII dotted-quad      */
    uint8_t ssid[AA_CFG_MAX];
    uint8_t key[AA_CFG_MAX]; /* AP passphrase — never logged             */
    uint8_t bssid[AA_CFG_MAX];
};

struct aa_boot {
    void *hl;
    uint32_t inst;
    const struct aa_wifi_cfg *cfg;
    int state;
    uint32_t bt_timeout_ms;  /* deadline budget for WifiInfoRequest                      */
    uint32_t tcp_timeout_ms; /* deadline budget for the TCP accept                       */
    uint32_t deadline_ms;    /* absolute deadline for the current waiting state          */
    uint32_t listen_only;    /* reconnected channel: log frames, never answer            */
    struct aa_rx rx;
};

/* Returns 0 on success, non-zero on bad args / config. bt_timeout_ms / tcp_timeout_ms are budgets
 * applied against the caller's monotonic clock (now_ms). */
int aa_boot_init(struct aa_boot *b, void *hl, uint32_t inst, const struct aa_wifi_cfg *cfg,
                 uint32_t bt_timeout_ms, uint32_t tcp_timeout_ms);
/* Begin the exchange. Preconditions: transport ready AND the HU listener already bound on
 * listener_port. Sends WifiStartRequest(ip, listener_port). now_ms seeds the BT deadline. */
int aa_boot_start(struct aa_boot *b, uint32_t now_ms, uint16_t listener_port);
/* Drive once with an explicit monotonic time: pull transport RX, parse frames, advance, apply the
 * deadline. Does NOT itself complete on any BT frame. Returns the state. */
int aa_boot_poll(struct aa_boot *b, uint32_t now_ms);
/* Adapter -> SM: the phone's TCP connection was accepted on the HU listener and handed to SAL.
 * Valid only in AA_BOOT_WAIT_TCP; returns the new state (AA_BOOT_DONE) or leaves an error. */
int aa_boot_tcp_accepted(struct aa_boot *b);
int aa_boot_state(const struct aa_boot *b);
/* After bootstrap: drain the SPP channel, log frame ids and answer WifiPingRequest. Never changes
 * the state; a framing fault only stops parsing until aa_boot_rechannel. */
void aa_boot_serve(struct aa_boot *b);
/* A new SPP channel was opened: restart framing and only log its frames until aa_boot_init. */
void aa_boot_rechannel(struct aa_boot *b);

#endif
