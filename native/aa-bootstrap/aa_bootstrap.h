#ifndef AA_BOOTSTRAP_H
#define AA_BOOTSTRAP_H
/* Pure wire codec, no Bluetooth access, allocation, libc, or global state. */
#define AA_RX_LIMIT 1024u
struct aa_bytes {
    const unsigned char *data;
    unsigned int size;
};
struct aa_wifi {
    struct aa_bytes ssid, key, bssid;
    unsigned int security, ap_type;
};
/* Return total frame bytes; return 0 on invalid input or insufficient capacity.
 * Output may be partially written on failure and must then be discarded. */
unsigned int aa_start_frame(unsigned char *out, unsigned int capacity, struct aa_bytes ip,
                            unsigned int port);
unsigned int aa_wifi_frame(unsigned char *out, unsigned int capacity, const struct aa_wifi *wifi);
unsigned int aa_raw_frame(unsigned char *out, unsigned int capacity, unsigned int id,
                          const unsigned char *payload, unsigned int size);
struct aa_rx {
    unsigned char header[4], payload[AA_RX_LIMIT];
    unsigned int header_used, payload_used, length, id, failed;
};
typedef int (*aa_on_frame)(void *, unsigned int, const unsigned char *, unsigned int);
/* Callback payload valid only during call; callback must not re-enter this parser.
 * Returns 0 on success, -1 on malformed/oversized input or callback failure.
 * Any error latches until reset. Payload protobuf interpretation belongs to caller. */
void aa_rx_reset(struct aa_rx *rx);
int aa_rx_feed(struct aa_rx *rx, const unsigned char *data, unsigned int size, aa_on_frame callback,
               void *context);
#endif
