/*
 * srt.h — single-header Secure Reliable Transport (SRT) protocol library.
 *
 * Reference: draft-sharabayko-srt (see docs/srt-protocol.md).
 *
 * Usage (stb-style):
 *
 *     // In exactly one translation unit:
 *     #define SRT_IMPLEMENTATION
 *     #include "srt.h"
 *
 *     // In any other TU:
 *     #include "srt.h"
 *
 * Design:
 *   - Sans-I/O. Caller owns the UDP socket and the clock.
 *   - No global state; everything lives behind srt_context / srt_socket.
 *   - Endian-safe, alignment-safe (de)serialization.
 *   - Pluggable crypto via srt_crypto_vtable.
 *   - C99, -Wall -Wextra -Wpedantic clean.
 *
 * Status: v0 — wire format + public API. State machine, ARQ, timers, and
 *         crypto bodies are stubbed. See TODO markers.
 */

#ifndef SRT_H_INCLUDED
#define SRT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                            Protocol constants                              */
/* ========================================================================== */

/* §1 common header is 16 bytes for both data and control packets. */
#define SRT_HEADER_SIZE            16

/* §4 handshake CIF fixed portion (before extensions). */
#define SRT_HS_CIF_SIZE            48

/* §4.3 INDUCTION magic in the Extension Field of Listener's response. */
#define SRT_MAGIC_CODE             0x4A17u

/* §6 KM message signature (HAI vendor id). */
#define SRT_KM_SIGN                0x2029u

/* §4.1 default MTU and flow window. */
#define SRT_DEFAULT_MTU            1500u
#define SRT_DEFAULT_FLOW_WINDOW    8192u

/* §9.4 default timer periods (microseconds). */
#define SRT_ACK_PERIOD_US          10000u   /* 10 ms */
#define SRT_NAK_PERIOD_US          20000u   /* 20 ms */
#define SRT_KEEPALIVE_PERIOD_US    1000000u /* 1 s   */
#define SRT_SYN_COOKIE_GRANULARITY 60       /* seconds */

/* Sequence number masks. */
#define SRT_SEQNO_MASK             0x7FFFFFFFu /* 31-bit */
#define SRT_MSGNO_MASK             0x03FFFFFFu /* 26-bit */

/* ========================================================================== */
/*                                 Enums                                      */
/* ========================================================================== */

/* §3.2 control packet types. */
typedef enum {
    SRT_CTRL_HANDSHAKE    = 0x0000,
    SRT_CTRL_KEEPALIVE    = 0x0001,
    SRT_CTRL_ACK          = 0x0002,
    SRT_CTRL_NAK          = 0x0003,
    SRT_CTRL_CONGESTION   = 0x0004,
    SRT_CTRL_SHUTDOWN     = 0x0005,
    SRT_CTRL_ACKACK       = 0x0006,
    SRT_CTRL_DROPREQ      = 0x0007,
    SRT_CTRL_PEERERROR    = 0x0008,
    SRT_CTRL_USERDEFINED  = 0x7FFF
} srt_ctrl_type;

/* §4.2 handshake types (32-bit sentinels; use #define to stay C99-portable). */
#define SRT_HS_WAVEAHAND   0x00000000u
#define SRT_HS_INDUCTION   0x00000001u
#define SRT_HS_AGREEMENT   0xFFFFFFFEu
#define SRT_HS_DONE        0xFFFFFFFDu
#define SRT_HS_CONCLUSION  0xFFFFFFFFu

/* §4.1 encryption advertisement. */
typedef enum {
    SRT_ENC_NONE    = 0,
    SRT_ENC_AES_128 = 2,
    SRT_ENC_AES_192 = 3,
    SRT_ENC_AES_256 = 4
} srt_enc_advert;

/* §4.5 handshake extension command codes. */
typedef enum {
    SRT_CMD_NONE       = 0,
    SRT_CMD_HSREQ      = 1,
    SRT_CMD_HSRSP      = 2,
    SRT_CMD_KMREQ      = 3,
    SRT_CMD_KMRSP      = 4,
    SRT_CMD_SID        = 5,
    SRT_CMD_CONGESTION = 6,
    SRT_CMD_FILTER     = 7,
    SRT_CMD_GROUP      = 8
} srt_ext_cmd;

/* §4.1 Extension Field flags for HS Version 5. */
enum {
    SRT_EXT_HSREQ  = 0x0001,
    SRT_EXT_KMREQ  = 0x0002,
    SRT_EXT_CONFIG = 0x0004
};

/* §4.6 SRT Flags (HSREQ/HSRSP). */
enum {
    SRT_FLAG_TSBPDSND      = 0x00000001,
    SRT_FLAG_TSBPDRCV      = 0x00000002,
    SRT_FLAG_CRYPT         = 0x00000004,
    SRT_FLAG_TLPKTDROP     = 0x00000008,
    SRT_FLAG_PERIODICNAK   = 0x00000010,
    SRT_FLAG_REXMITFLG     = 0x00000020,
    SRT_FLAG_STREAM        = 0x00000040,
    SRT_FLAG_PACKET_FILTER = 0x00000080
};

/* §2 data packet Packet Position (PP) 2-bit codes. */
typedef enum {
    SRT_PP_MIDDLE = 0x0,
    SRT_PP_LAST   = 0x1,
    SRT_PP_FIRST  = 0x2,
    SRT_PP_SOLO   = 0x3
} srt_pp;

/* §2 data packet KK (key) 2-bit codes. */
typedef enum {
    SRT_KK_NONE = 0x0,
    SRT_KK_EVEN = 0x1,
    SRT_KK_ODD  = 0x2,
    SRT_KK_BOTH = 0x3  /* KM message only */
} srt_kk;

/* §11 cipher selection for KM message. */
typedef enum {
    SRT_CIPHER_NONE    = 0,
    SRT_CIPHER_AES_ECB = 1, /* reserved */
    SRT_CIPHER_AES_CTR = 2,
    SRT_CIPHER_AES_CBC = 3, /* reserved */
    SRT_CIPHER_AES_GCM = 4
} srt_cipher;

/* §12 handshake rejection codes. */
typedef enum {
    SRT_REJ_UNKNOWN    = 1000,
    SRT_REJ_SYSTEM     = 1001,
    SRT_REJ_PEER       = 1002,
    SRT_REJ_RESOURCE   = 1003,
    SRT_REJ_ROGUE      = 1004,
    SRT_REJ_BACKLOG    = 1005,
    SRT_REJ_IPE        = 1006,
    SRT_REJ_CLOSE      = 1007,
    SRT_REJ_VERSION    = 1008,
    SRT_REJ_RDVCOOKIE  = 1009,
    SRT_REJ_BADSECRET  = 1010,
    SRT_REJ_UNSECURE   = 1011,
    SRT_REJ_MESSAGEAPI = 1012,
    SRT_REJ_CONGESTION = 1013,
    SRT_REJ_FILTER     = 1014,
    SRT_REJ_GROUP      = 1015,
    SRT_REJ_TIMEOUT    = 1016,
    SRT_REJ_CRYPTO     = 1017
} srt_reject;

/* §13 socket states. */
typedef enum {
    SRT_STATE_INIT,
    SRT_STATE_OPENED,
    SRT_STATE_LISTENING,
    SRT_STATE_CONNECTING,
    SRT_STATE_CONNECTED,
    SRT_STATE_CLOSING,
    SRT_STATE_CLOSED,
    SRT_STATE_BROKEN
} srt_state;

/* Library error codes. */
typedef enum {
    SRT_OK             =  0,
    SRT_ERR_AGAIN      = -1, /* no data ready / would block */
    SRT_ERR_INVAL      = -2, /* bad argument or malformed packet */
    SRT_ERR_NOMEM      = -3,
    SRT_ERR_TOOSHORT   = -4, /* buffer too small */
    SRT_ERR_PROTO      = -5, /* protocol violation */
    SRT_ERR_CLOSED     = -6,
    SRT_ERR_CRYPTO     = -7,
    SRT_ERR_UNSUPPORTED= -8,
    SRT_ERR_TIMEOUT    = -9
} srt_err;

/* ========================================================================== */
/*                          Wire-level message types                          */
/* ========================================================================== */

/* §1 common header shared by data and control packets. */
typedef struct {
    bool     is_control;     /* F bit */
    uint32_t f1;             /* bytes 0..3 (excluding F); meaning depends on type */
    uint32_t f2;             /* bytes 4..7;               meaning depends on type */
    uint32_t timestamp;      /* µs, connection-relative */
    uint32_t dst_socket_id;
} srt_common_hdr;

/* §2 decoded data packet header (without payload). */
typedef struct {
    uint32_t seqno;      /* 31-bit */
    srt_pp   pp;         /* 2-bit  */
    bool     ordered;    /* O flag */
    srt_kk   kk;         /* 2-bit  */
    bool     retx;       /* R flag */
    uint32_t msgno;      /* 26-bit */
    uint32_t timestamp;
    uint32_t dst_socket_id;
} srt_data_hdr;

/* §3 generic control packet header. */
typedef struct {
    uint16_t ctrl_type;   /* 15 bits used */
    uint16_t subtype;
    uint32_t type_info;   /* Type-specific Information */
    uint32_t timestamp;
    uint32_t dst_socket_id;
} srt_ctrl_hdr;

/* §4.1 handshake CIF fixed portion + pointer to extension blob. */
typedef struct {
    uint32_t hs_version;      /* 4 or 5 */
    uint16_t encryption;      /* srt_enc_advert */
    uint16_t extension_field;
    uint32_t isn;             /* initial packet sequence number (31-bit) */
    uint32_t mtu;
    uint32_t flow_window;
    uint32_t hs_type;         /* srt_hs_type or rejection code */
    uint32_t srt_socket_id;
    uint32_t syn_cookie;
    uint8_t  peer_ip[16];     /* IPv6 or IPv4-mapped */

    /* Extensions: pointer into the caller's buffer. Parsed lazily. */
    const uint8_t *ext_data;
    size_t         ext_len;
} srt_hs_cif;

/* §4.6 HSREQ/HSRSP payload. */
typedef struct {
    uint32_t srt_version;       /* major*0x10000 + minor*0x100 + patch */
    uint32_t srt_flags;         /* SRT_FLAG_* */
    uint16_t rcv_tsbpd_delay;   /* ms */
    uint16_t snd_tsbpd_delay;   /* ms */
} srt_hsreq;

/* §5.1 ACK CIF (full form). */
typedef struct {
    uint32_t ack_number;        /* from type-specific info */
    uint32_t last_ack_seqno;    /* 31-bit */
    uint32_t rtt_us;
    uint32_t rtt_var_us;
    uint32_t avail_buf_pkts;
    uint32_t pkt_recv_rate;     /* pps */
    uint32_t link_capacity;     /* pps */
    uint32_t recv_rate_bps;     /* bytes/sec */
    uint8_t  form;              /* 0=full,1=light,2=small (implementation tag) */
} srt_ack;

#define SRT_ACK_FULL  0
#define SRT_ACK_LIGHT 1
#define SRT_ACK_SMALL 2

/* §5.2 NAK loss entry (as stored in-memory, already decoded). */
typedef struct {
    uint32_t start; /* inclusive */
    uint32_t end;   /* inclusive; equal to start for a single-packet loss */
} srt_loss_range;

/* §5.5 DROPREQ CIF. */
typedef struct {
    uint32_t msgno;
    uint32_t first_seqno;
    uint32_t last_seqno;
} srt_dropreq;

/* §6 KM message (decoded). */
typedef struct {
    uint8_t  version;        /* V, currently 1 */
    uint8_t  pt;             /* Packet Type */
    uint16_t sign;           /* 0x2029 */
    srt_kk   kk;
    uint32_t keki;
    srt_cipher cipher;
    uint8_t  auth;           /* 0 or 1 (AES-GCM) */
    uint8_t  se;             /* stream encapsulation */
    uint8_t  slen;           /* salt length in bytes  */
    uint8_t  klen;           /* key length in bytes   */
    const uint8_t *salt;     /* pointer into input buffer */
    const uint8_t *wrap;     /* wrapped-key blob: 8-byte ICV + n*klen */
    size_t         wrap_len;
} srt_km;

/* ========================================================================== */
/*                      Crypto vtable (pluggable backend)                     */
/* ========================================================================== */

/*
 * Default build ships a minimal AES-CTR/GCM + RFC 3394 key wrap. Callers can
 * override by populating this vtable and passing it to srt_create(). Every
 * function returns 0 on success, nonzero on failure.
 */
typedef struct {
    int (*aes_ctr)(void *ctx,
                   const uint8_t *key, size_t key_len,
                   const uint8_t iv[16],
                   const uint8_t *in, uint8_t *out, size_t n);
    int (*aes_gcm_seal)(void *ctx,
                        const uint8_t *key, size_t key_len,
                        const uint8_t *iv, size_t iv_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *pt, uint8_t *ct, size_t n,
                        uint8_t tag[16]);
    int (*aes_gcm_open)(void *ctx,
                        const uint8_t *key, size_t key_len,
                        const uint8_t *iv, size_t iv_len,
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *ct, uint8_t *pt, size_t n,
                        const uint8_t tag[16]);
    /* RFC 3394 AES key wrap / unwrap.
     * wrap:   pt_len multiple of 8, >=16; produces pt_len + 8 bytes.
     * unwrap: in_len multiple of 8, >=24; produces in_len - 8 bytes. */
    int (*kw_wrap)  (void *ctx, const uint8_t *kek, size_t kek_len,
                     const uint8_t *pt, size_t pt_len, uint8_t *out);
    int (*kw_unwrap)(void *ctx, const uint8_t *kek, size_t kek_len,
                     const uint8_t *in, size_t in_len, uint8_t *out);
    int (*random)(void *ctx, uint8_t *out, size_t n);
    void *ctx;
} srt_crypto_vtable;

/* Fills `v` with an OpenSSL-backed crypto implementation. Returns SRT_OK. */
int srt_crypto_openssl(srt_crypto_vtable *v);

/* ========================================================================== */
/*                               Configuration                                */
/* ========================================================================== */

typedef struct {
    void *(*alloc)(void *ud, size_t n);
    void  (*free) (void *ud, void *p);
    void   *ud;
} srt_allocator;

typedef struct {
    srt_allocator     allocator;    /* optional; NULL fields → use malloc/free */
    srt_crypto_vtable crypto;       /* optional; NULL fields → built-in */
    uint32_t          max_sockets;  /* 0 → default 1024 */
} srt_config;

typedef struct {
    bool     caller;          /* true → caller, false → listener/rendezvous */
    bool     rendezvous;
    bool     stream_mode;     /* true → buffer mode, false → message mode */
    bool     tsbpd;
    bool     tlpktdrop;
    bool     periodic_nak;
    uint16_t rcv_tsbpd_ms;
    uint16_t snd_tsbpd_ms;
    uint32_t flow_window;
    uint32_t mtu;
    srt_enc_advert encryption;
    srt_cipher     cipher;     /* AES-CTR (default) or AES-GCM */
    const char *passphrase;    /* optional; NULL → no encryption */
    const char *stream_id;     /* §4.5 SID extension      — freeform tag */
    const char *congestion;    /* §4.5 CONGESTION ext     — "live"/"file" */
    const char *filter;        /* §4.5 FILTER ext         — e.g. "fec,cols:10,rows:5" */
    const char *group;         /* §4.5 GROUP ext          — bonded-stream member info */
    uint64_t   max_bw_bps;     /* LiveCC cap: bytes/sec (0 = unlimited) */
} srt_sockopts;

/* Message metadata for srt_send / srt_recv. */
typedef struct {
    uint32_t msgno;
    bool     ordered;
    bool     last;            /* in message mode: mark last packet of message */
} srt_msg;

/* Opaque handles. */
typedef struct srt_context_s srt_context;
typedef struct srt_socket_s  srt_socket;

/* ========================================================================== */
/*                                Public API                                  */
/* ========================================================================== */

srt_context *srt_create (const srt_config *cfg);
void         srt_destroy(srt_context *ctx);

srt_socket  *srt_socket_new(srt_context *ctx, const srt_sockopts *opt);
int          srt_shutdown  (srt_socket *s);  /* stage SHUTDOWN, state→CLOSING */
int          srt_close     (srt_socket *s);  /* free; does not guarantee drain */

int  srt_listen   (srt_socket *s, const struct sockaddr *local, socklen_t len);
int  srt_accept   (srt_socket *s, srt_socket **out);
int  srt_connect  (srt_socket *s, const struct sockaddr *remote, socklen_t len);
int  srt_rendezvous(srt_socket *s, const struct sockaddr *local,
                                   const struct sockaddr *remote, socklen_t len);

/* Data path (sans-I/O). */
int  srt_feed_udp(srt_socket *s, const void *pkt, size_t n, uint64_t now_us);

/* Listener variant: the caller supplies the peer address from recvfrom(). */
int  srt_feed_udp_from(srt_socket *s, const void *pkt, size_t n,
                       const struct sockaddr *peer, socklen_t plen,
                       uint64_t now_us);

/* Multiplex: feed every datagram from a shared UDP socket to the right
 * srt_socket based on Destination SRT Socket ID. Packets with dst_id == 0
 * (INDUCTION or WAVEAHAND) go to a matching rendezvous socket if one exists
 * for the peer address, otherwise to any LISTENING socket. */
int  srt_context_feed_udp(srt_context *ctx, const void *pkt, size_t n,
                          const struct sockaddr *peer, socklen_t plen,
                          uint64_t now_us);

/* Drain one staged outbound datagram from any socket in the context. Fills
 * `peer` with the destination address and returns its byte count. */
int  srt_context_pull_udp(srt_context *ctx, void *out, size_t cap,
                          struct sockaddr *peer, socklen_t *plen,
                          uint64_t now_us);

int  srt_pull_udp(srt_socket *s, void *out, size_t cap, uint64_t now_us);
int  srt_send    (srt_socket *s, const void *buf, size_t n, const srt_msg *meta);
int  srt_recv    (srt_socket *s, void *buf, size_t cap, srt_msg *meta);

/* Drive periodic tasks (ACK, KEEPALIVE, EXP retransmit, TSBPD release).
 * Call whenever the wall clock advances; at minimum whenever
 * srt_next_deadline_us(..., now) <= now. Returns SRT_OK. */
int  srt_tick(srt_socket *s, uint64_t now_us);

/* §11.4 KM refresh: generate a new opposite-parity SEK, emit a USER-DEFINED
 * KMREQ on the wire, and start encrypting outbound DATA with the new key.
 * The peer switches to the new key automatically when it sees KK flipped on
 * arriving DATA (both parities are kept live to cover reorder). */
int  srt_rekey(srt_socket *s);

/* §5.7: report a receiver-side processing error (e.g. 4000 = filesystem).
 * Peer transitions to BROKEN and srt_get_peer_error returns `code`. */
int  srt_send_peer_error (srt_socket *s, uint32_t code);
uint32_t srt_get_peer_error(const srt_socket *s);

uint64_t srt_next_deadline_us(const srt_socket *s, uint64_t now_us);

srt_state srt_get_state(const srt_socket *s);
uint32_t  srt_get_local_socket_id(const srt_socket *s);
uint32_t  srt_get_peer_socket_id (const srt_socket *s);

/* Accessors for HS-exchanged config strings. Returned pointer stays valid
 * for the lifetime of the socket. NULL if the peer didn't send one. */
const char *srt_get_peer_stream_id (const srt_socket *s);
const char *srt_get_peer_congestion(const srt_socket *s);
const char *srt_get_peer_filter    (const srt_socket *s);
const char *srt_get_peer_group     (const srt_socket *s);

/* ========================================================================== */
/*                      Low-level wire (de)serialization                      */
/*                                                                            */
/* These are exposed so tests (and advanced callers) can round-trip packets  */
/* without spinning up a full context.                                        */
/* ========================================================================== */

/* Parse the shared 16-byte header. Returns SRT_OK or an error code. */
int srt_parse_common_hdr (const uint8_t *buf, size_t n, srt_common_hdr *out);
int srt_write_common_hdr (uint8_t *buf, size_t cap, const srt_common_hdr *h);

/* §2 data packet header (requires is_control == false). */
int srt_parse_data_hdr   (const uint8_t *buf, size_t n, srt_data_hdr *out);
int srt_write_data_hdr   (uint8_t *buf, size_t cap, const srt_data_hdr *h);

/* §3 generic control packet header (requires is_control == true). */
int srt_parse_ctrl_hdr   (const uint8_t *buf, size_t n, srt_ctrl_hdr *out);
int srt_write_ctrl_hdr   (uint8_t *buf, size_t cap, const srt_ctrl_hdr *h);

/* §4.1 handshake CIF (fixed portion; extensions available via ext_data). */
int srt_parse_hs_cif (const uint8_t *buf, size_t n, srt_hs_cif *out);
int srt_write_hs_cif (uint8_t *buf, size_t cap, const srt_hs_cif *h);

/* §4.6 HSREQ / HSRSP payload. */
int srt_parse_hsreq  (const uint8_t *buf, size_t n, srt_hsreq *out);
int srt_write_hsreq  (uint8_t *buf, size_t cap, const srt_hsreq *h);

/* §5.1 ACK. `form` selects full/light/small; parse auto-detects by length. */
int srt_parse_ack    (const uint8_t *cif, size_t n, uint32_t type_info, srt_ack *out);
int srt_write_ack    (uint8_t *cif, size_t cap, const srt_ack *a, size_t *out_len);

/* §5.2 NAK loss list: parse emits up to `cap` ranges, returns count. */
int srt_parse_nak    (const uint8_t *cif, size_t n,
                      srt_loss_range *out, size_t cap, size_t *out_count);
int srt_write_nak    (uint8_t *cif, size_t cap,
                      const srt_loss_range *ranges, size_t n_ranges,
                      size_t *out_len);

/* §5.5 DROPREQ. */
int srt_parse_dropreq(const uint8_t *cif, size_t n, srt_dropreq *out);
int srt_write_dropreq(uint8_t *cif, size_t cap, const srt_dropreq *d);

/* §6 KM message. Pointers into `buf` are borrowed, not copied. */
int srt_parse_km     (const uint8_t *buf, size_t n, srt_km *out);
int srt_write_km     (uint8_t *buf, size_t cap, const srt_km *k, size_t *out_len);

/* Handshake-extension TLV walker. Returns next cmd and advances cursor. */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} srt_ext_iter;

void srt_ext_iter_init (srt_ext_iter *it, const uint8_t *buf, size_t n);
int  srt_ext_iter_next (srt_ext_iter *it,
                        uint16_t *cmd, const uint8_t **payload, uint16_t *len);

/* RFC 1982 / §9 serial-number arithmetic on 31-bit sequence numbers. */
static inline int32_t srt_seq_cmp(uint32_t a, uint32_t b) {
    /* Sign-extend the 31-bit difference into int32_t. */
    uint32_t d = (a - b) & SRT_SEQNO_MASK;
    if (d & 0x40000000u) d |= 0x80000000u; /* propagate sign */
    return (int32_t)d;
}
static inline uint32_t srt_seq_inc(uint32_t s) {
    return (s + 1u) & SRT_SEQNO_MASK;
}
static inline uint32_t srt_seq_add(uint32_t s, int32_t n) {
    return ((uint32_t)((int32_t)s + n)) & SRT_SEQNO_MASK;
}

#ifdef __cplusplus
}
#endif

/* ============================================================================
 *
 *                            IMPLEMENTATION
 *
 * ========================================================================= */

#ifdef SRT_IMPLEMENTATION

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------- OpenSSL crypto backend ------------------------ */

/* Pick the EVP AES-CTR cipher for a given key length (16/24/32 bytes). */
static const EVP_CIPHER *srt__ossl_ctr(size_t klen) {
    switch (klen) {
        case 16: return EVP_aes_128_ctr();
        case 24: return EVP_aes_192_ctr();
        case 32: return EVP_aes_256_ctr();
        default: return NULL;
    }
}
static const EVP_CIPHER *srt__ossl_gcm(size_t klen) {
    switch (klen) {
        case 16: return EVP_aes_128_gcm();
        case 24: return EVP_aes_192_gcm();
        case 32: return EVP_aes_256_gcm();
        default: return NULL;
    }
}
static const EVP_CIPHER *srt__ossl_wrap(size_t klen) {
    switch (klen) {
        case 16: return EVP_aes_128_wrap();
        case 24: return EVP_aes_192_wrap();
        case 32: return EVP_aes_256_wrap();
        default: return NULL;
    }
}

static int srt__ossl_aes_ctr(void *c,
                             const uint8_t *key, size_t klen,
                             const uint8_t iv[16],
                             const uint8_t *in, uint8_t *out, size_t n) {
    (void)c;
    const EVP_CIPHER *ciph = srt__ossl_ctr(klen);
    if (!ciph) return SRT_ERR_INVAL;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    if (!x) return SRT_ERR_NOMEM;
    int rc = SRT_ERR_CRYPTO, outl = 0;
    if (EVP_EncryptInit_ex(x, ciph, NULL, key, iv) != 1) goto done;
    if (EVP_EncryptUpdate(x, out, &outl, in, (int)n) != 1) goto done;
    /* CTR is a stream cipher; EVP_EncryptFinal_ex is a no-op but required. */
    int tail = 0;
    if (EVP_EncryptFinal_ex(x, out + outl, &tail) != 1) goto done;
    rc = SRT_OK;
done:
    EVP_CIPHER_CTX_free(x);
    return rc;
}

static int srt__ossl_aes_gcm_seal(void *c,
                                  const uint8_t *key, size_t klen,
                                  const uint8_t *iv, size_t ivlen,
                                  const uint8_t *aad, size_t aadlen,
                                  const uint8_t *pt, uint8_t *ct, size_t n,
                                  uint8_t tag[16]) {
    (void)c;
    const EVP_CIPHER *ciph = srt__ossl_gcm(klen);
    if (!ciph) return SRT_ERR_INVAL;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    if (!x) return SRT_ERR_NOMEM;
    int rc = SRT_ERR_CRYPTO, outl = 0;
    if (EVP_EncryptInit_ex(x, ciph, NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_IVLEN, (int)ivlen, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(x, NULL, NULL, key, iv) != 1) goto done;
    if (aad && aadlen) {
        if (EVP_EncryptUpdate(x, NULL, &outl, aad, (int)aadlen) != 1) goto done;
    }
    if (EVP_EncryptUpdate(x, ct, &outl, pt, (int)n) != 1) goto done;
    int tail = 0;
    if (EVP_EncryptFinal_ex(x, ct + outl, &tail) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_GET_TAG, 16, tag) != 1) goto done;
    rc = SRT_OK;
done:
    EVP_CIPHER_CTX_free(x);
    return rc;
}

static int srt__ossl_aes_gcm_open(void *c,
                                  const uint8_t *key, size_t klen,
                                  const uint8_t *iv, size_t ivlen,
                                  const uint8_t *aad, size_t aadlen,
                                  const uint8_t *ct, uint8_t *pt, size_t n,
                                  const uint8_t tag[16]) {
    (void)c;
    const EVP_CIPHER *ciph = srt__ossl_gcm(klen);
    if (!ciph) return SRT_ERR_INVAL;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    if (!x) return SRT_ERR_NOMEM;
    int rc = SRT_ERR_CRYPTO, outl = 0;
    if (EVP_DecryptInit_ex(x, ciph, NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_IVLEN, (int)ivlen, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(x, NULL, NULL, key, iv) != 1) goto done;
    if (aad && aadlen) {
        if (EVP_DecryptUpdate(x, NULL, &outl, aad, (int)aadlen) != 1) goto done;
    }
    if (EVP_DecryptUpdate(x, pt, &outl, ct, (int)n) != 1) goto done;
    /* Tag is input for verification. */
    if (EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) != 1) goto done;
    int tail = 0;
    if (EVP_DecryptFinal_ex(x, pt + outl, &tail) != 1) { rc = SRT_ERR_CRYPTO; goto done; }
    rc = SRT_OK;
done:
    EVP_CIPHER_CTX_free(x);
    return rc;
}

static int srt__ossl_kw_wrap(void *c, const uint8_t *kek, size_t klen,
                             const uint8_t *pt, size_t pt_len, uint8_t *out) {
    (void)c;
    const EVP_CIPHER *ciph = srt__ossl_wrap(klen);
    if (!ciph) return SRT_ERR_INVAL;
    if (pt_len < 16 || (pt_len % 8) != 0) return SRT_ERR_INVAL;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    if (!x) return SRT_ERR_NOMEM;
    int rc = SRT_ERR_CRYPTO, outl = 0;
    EVP_CIPHER_CTX_set_flags(x, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    /* Default IV (RFC 3394 A6A6A6A6A6A6A6A6) is used when iv==NULL. */
    if (EVP_EncryptInit_ex(x, ciph, NULL, kek, NULL) != 1) goto done;
    if (EVP_EncryptUpdate(x, out, &outl, pt, (int)pt_len) != 1) goto done;
    int tail = 0;
    if (EVP_EncryptFinal_ex(x, out + outl, &tail) != 1) goto done;
    rc = SRT_OK;
done:
    EVP_CIPHER_CTX_free(x);
    return rc;
}

static int srt__ossl_kw_unwrap(void *c, const uint8_t *kek, size_t klen,
                               const uint8_t *in, size_t in_len, uint8_t *out) {
    (void)c;
    const EVP_CIPHER *ciph = srt__ossl_wrap(klen);
    if (!ciph) return SRT_ERR_INVAL;
    if (in_len < 24 || (in_len % 8) != 0) return SRT_ERR_INVAL;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    if (!x) return SRT_ERR_NOMEM;
    int rc = SRT_ERR_CRYPTO, outl = 0;
    EVP_CIPHER_CTX_set_flags(x, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    if (EVP_DecryptInit_ex(x, ciph, NULL, kek, NULL) != 1) goto done;
    if (EVP_DecryptUpdate(x, out, &outl, in, (int)in_len) != 1) goto done;
    int tail = 0;
    if (EVP_DecryptFinal_ex(x, out + outl, &tail) != 1) { rc = SRT_ERR_CRYPTO; goto done; }
    rc = SRT_OK;
done:
    EVP_CIPHER_CTX_free(x);
    return rc;
}

static int srt__ossl_random(void *c, uint8_t *out, size_t n) {
    (void)c;
    return (RAND_bytes(out, (int)n) == 1) ? SRT_OK : SRT_ERR_CRYPTO;
}

int srt_crypto_openssl(srt_crypto_vtable *v) {
    if (!v) return SRT_ERR_INVAL;
    memset(v, 0, sizeof(*v));
    v->aes_ctr      = srt__ossl_aes_ctr;
    v->aes_gcm_seal = srt__ossl_aes_gcm_seal;
    v->aes_gcm_open = srt__ossl_aes_gcm_open;
    v->kw_wrap      = srt__ossl_kw_wrap;
    v->kw_unwrap    = srt__ossl_kw_unwrap;
    v->random       = srt__ossl_random;
    v->ctx          = NULL;
    return SRT_OK;
}

/* -------------------------- byte-order helpers ---------------------------- */

static inline uint16_t srt__rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t srt__rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static inline void srt__wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline void srt__wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

/* -------------------------- common header --------------------------------- */

int srt_parse_common_hdr(const uint8_t *buf, size_t n, srt_common_hdr *out) {
    if (!buf || !out || n < SRT_HEADER_SIZE) return SRT_ERR_TOOSHORT;
    uint32_t w0 = srt__rd32(buf);
    uint32_t w1 = srt__rd32(buf + 4);
    out->is_control    = (w0 & 0x80000000u) != 0;
    out->f1            = w0 & 0x7FFFFFFFu;
    out->f2            = w1;
    out->timestamp     = srt__rd32(buf + 8);
    out->dst_socket_id = srt__rd32(buf + 12);
    return SRT_OK;
}

int srt_write_common_hdr(uint8_t *buf, size_t cap, const srt_common_hdr *h) {
    if (!buf || !h || cap < SRT_HEADER_SIZE) return SRT_ERR_TOOSHORT;
    uint32_t w0 = h->f1 & 0x7FFFFFFFu;
    if (h->is_control) w0 |= 0x80000000u;
    srt__wr32(buf,      w0);
    srt__wr32(buf +  4, h->f2);
    srt__wr32(buf +  8, h->timestamp);
    srt__wr32(buf + 12, h->dst_socket_id);
    return SRT_OK;
}

/* -------------------------- data packet ----------------------------------- */

/* §2: seqno (31) | PP(2) O(1) KK(2) R(1) MSGNO(26) | ts | dstid | payload */
int srt_parse_data_hdr(const uint8_t *buf, size_t n, srt_data_hdr *out) {
    srt_common_hdr h;
    int rc = srt_parse_common_hdr(buf, n, &h);
    if (rc != SRT_OK) return rc;
    if (h.is_control) return SRT_ERR_INVAL;
    out->seqno         = h.f1 & SRT_SEQNO_MASK;
    out->pp            = (srt_pp)((h.f2 >> 30) & 0x3u);
    out->ordered       = (h.f2 & 0x20000000u) != 0;
    out->kk            = (srt_kk)((h.f2 >> 27) & 0x3u);
    out->retx          = (h.f2 & 0x04000000u) != 0;
    out->msgno         = h.f2 & SRT_MSGNO_MASK;
    out->timestamp     = h.timestamp;
    out->dst_socket_id = h.dst_socket_id;
    return SRT_OK;
}

int srt_write_data_hdr(uint8_t *buf, size_t cap, const srt_data_hdr *h) {
    srt_common_hdr c;
    c.is_control = false;
    c.f1 = h->seqno & SRT_SEQNO_MASK;
    c.f2 = ((uint32_t)(h->pp & 0x3) << 30)
         | (h->ordered ? 0x20000000u : 0u)
         | ((uint32_t)(h->kk & 0x3) << 27)
         | (h->retx ? 0x04000000u : 0u)
         | (h->msgno & SRT_MSGNO_MASK);
    c.timestamp     = h->timestamp;
    c.dst_socket_id = h->dst_socket_id;
    return srt_write_common_hdr(buf, cap, &c);
}

/* -------------------------- control packet header ------------------------- */

/* §3: F=1 | ctrl_type(15) | subtype(16) | type_info(32) | ts | dstid | CIF */
int srt_parse_ctrl_hdr(const uint8_t *buf, size_t n, srt_ctrl_hdr *out) {
    srt_common_hdr h;
    int rc = srt_parse_common_hdr(buf, n, &h);
    if (rc != SRT_OK) return rc;
    if (!h.is_control) return SRT_ERR_INVAL;
    out->ctrl_type     = (uint16_t)((h.f1 >> 16) & 0x7FFFu);
    out->subtype       = (uint16_t)(h.f1 & 0xFFFFu);
    out->type_info     = h.f2;
    out->timestamp     = h.timestamp;
    out->dst_socket_id = h.dst_socket_id;
    return SRT_OK;
}

int srt_write_ctrl_hdr(uint8_t *buf, size_t cap, const srt_ctrl_hdr *h) {
    srt_common_hdr c;
    c.is_control    = true;
    c.f1            = ((uint32_t)(h->ctrl_type & 0x7FFFu) << 16) | h->subtype;
    c.f2            = h->type_info;
    c.timestamp     = h->timestamp;
    c.dst_socket_id = h->dst_socket_id;
    return srt_write_common_hdr(buf, cap, &c);
}

/* -------------------------- handshake CIF --------------------------------- */

int srt_parse_hs_cif(const uint8_t *buf, size_t n, srt_hs_cif *out) {
    if (!buf || !out || n < SRT_HS_CIF_SIZE) return SRT_ERR_TOOSHORT;
    out->hs_version      = srt__rd32(buf +  0);
    out->encryption      = srt__rd16(buf +  4);
    out->extension_field = srt__rd16(buf +  6);
    out->isn             = srt__rd32(buf +  8) & SRT_SEQNO_MASK;
    out->mtu             = srt__rd32(buf + 12);
    out->flow_window     = srt__rd32(buf + 16);
    out->hs_type         = srt__rd32(buf + 20);
    out->srt_socket_id   = srt__rd32(buf + 24);
    out->syn_cookie      = srt__rd32(buf + 28);
    memcpy(out->peer_ip, buf + 32, 16);
    out->ext_data = (n > SRT_HS_CIF_SIZE) ? buf + SRT_HS_CIF_SIZE : NULL;
    out->ext_len  = n - SRT_HS_CIF_SIZE;
    /* NB: fields 4..6 are stored big-endian with encryption in bytes 4..5
     * and extension field in bytes 6..7 — re-read via srt__rd16 above. */
    return SRT_OK;
}

int srt_write_hs_cif(uint8_t *buf, size_t cap, const srt_hs_cif *h) {
    if (!buf || !h) return SRT_ERR_INVAL;
    size_t need = SRT_HS_CIF_SIZE + h->ext_len;
    if (cap < need) return SRT_ERR_TOOSHORT;
    srt__wr32(buf +  0, h->hs_version);
    srt__wr16(buf +  4, h->encryption);
    srt__wr16(buf +  6, h->extension_field);
    srt__wr32(buf +  8, h->isn & SRT_SEQNO_MASK);
    srt__wr32(buf + 12, h->mtu);
    srt__wr32(buf + 16, h->flow_window);
    srt__wr32(buf + 20, h->hs_type);
    srt__wr32(buf + 24, h->srt_socket_id);
    srt__wr32(buf + 28, h->syn_cookie);
    memcpy(buf + 32, h->peer_ip, 16);
    if (h->ext_len && h->ext_data)
        memcpy(buf + SRT_HS_CIF_SIZE, h->ext_data, h->ext_len);
    return SRT_OK;
}

/* -------------------------- HSREQ / HSRSP --------------------------------- */

int srt_parse_hsreq(const uint8_t *buf, size_t n, srt_hsreq *out) {
    if (n < 12) return SRT_ERR_TOOSHORT;
    out->srt_version     = srt__rd32(buf + 0);
    out->srt_flags       = srt__rd32(buf + 4);
    out->rcv_tsbpd_delay = srt__rd16(buf + 8);
    out->snd_tsbpd_delay = srt__rd16(buf + 10);
    return SRT_OK;
}

int srt_write_hsreq(uint8_t *buf, size_t cap, const srt_hsreq *h) {
    if (cap < 12) return SRT_ERR_TOOSHORT;
    srt__wr32(buf + 0, h->srt_version);
    srt__wr32(buf + 4, h->srt_flags);
    srt__wr16(buf + 8,  h->rcv_tsbpd_delay);
    srt__wr16(buf + 10, h->snd_tsbpd_delay);
    return SRT_OK;
}

/* -------------------------- extension TLV walker -------------------------- */
/*
 * §4.1 extension block: sequence of (Type[16], Length[16 in 32-bit words],
 * Contents[Length*4 bytes]) TLVs until ext_len is consumed.
 */

void srt_ext_iter_init(srt_ext_iter *it, const uint8_t *buf, size_t n) {
    it->p = buf; it->end = buf ? buf + n : NULL;
}

int srt_ext_iter_next(srt_ext_iter *it,
                      uint16_t *cmd, const uint8_t **payload, uint16_t *len) {
    if (!it->p || it->p >= it->end) return SRT_ERR_AGAIN;
    if ((size_t)(it->end - it->p) < 4) return SRT_ERR_TOOSHORT;
    uint16_t t  = srt__rd16(it->p);
    uint16_t lw = srt__rd16(it->p + 2);
    uint32_t lb = (uint32_t)lw * 4u;
    if ((size_t)(it->end - it->p) < 4u + lb) return SRT_ERR_TOOSHORT;
    *cmd     = t;
    *payload = it->p + 4;
    *len     = (uint16_t)lb;
    it->p   += 4 + lb;
    return SRT_OK;
}

/* -------------------------- ACK ------------------------------------------- */
/*
 * §5.1 forms:
 *   full  = 28 bytes (last_ack_seq + rtt + rtt_var + avail + pps + cap + bps)
 *   small = 12 bytes (through avail_buf_pkts)
 *   light =  4 bytes (last_ack_seq only)
 * `type_info` carries the Acknowledgement Number from the control header.
 */

int srt_parse_ack(const uint8_t *cif, size_t n, uint32_t type_info, srt_ack *out) {
    if (n < 4) return SRT_ERR_TOOSHORT;
    memset(out, 0, sizeof(*out));
    out->ack_number     = type_info;
    out->last_ack_seqno = srt__rd32(cif) & SRT_SEQNO_MASK;
    if (n >= 28) {
        out->rtt_us         = srt__rd32(cif +  4);
        out->rtt_var_us     = srt__rd32(cif +  8);
        out->avail_buf_pkts = srt__rd32(cif + 12);
        out->pkt_recv_rate  = srt__rd32(cif + 16);
        out->link_capacity  = srt__rd32(cif + 20);
        out->recv_rate_bps  = srt__rd32(cif + 24);
        out->form           = SRT_ACK_FULL;
    } else if (n >= 12) {
        out->rtt_us         = srt__rd32(cif + 4);
        out->rtt_var_us     = srt__rd32(cif + 8);
        out->form           = SRT_ACK_SMALL;
    } else {
        out->form = SRT_ACK_LIGHT;
    }
    return SRT_OK;
}

int srt_write_ack(uint8_t *cif, size_t cap, const srt_ack *a, size_t *out_len) {
    size_t need = (a->form == SRT_ACK_LIGHT) ? 4
                : (a->form == SRT_ACK_SMALL) ? 12 : 28;
    if (cap < need) return SRT_ERR_TOOSHORT;
    srt__wr32(cif, a->last_ack_seqno & SRT_SEQNO_MASK);
    if (a->form != SRT_ACK_LIGHT) {
        srt__wr32(cif + 4, a->rtt_us);
        srt__wr32(cif + 8, a->rtt_var_us);
    }
    if (a->form == SRT_ACK_SMALL) {
        /* small ACK ends at rtt_var per spec; emit 12 bytes */
    } else if (a->form == SRT_ACK_FULL) {
        srt__wr32(cif + 12, a->avail_buf_pkts);
        srt__wr32(cif + 16, a->pkt_recv_rate);
        srt__wr32(cif + 20, a->link_capacity);
        srt__wr32(cif + 24, a->recv_rate_bps);
    }
    if (out_len) *out_len = need;
    return SRT_OK;
}

/* -------------------------- NAK loss list --------------------------------- */
/*
 * §5.2 encoding (Appendix A):
 *   |0|seq(31)|                        single
 *   |1|start(31)|  |0|end(31)|         inclusive range
 */

int srt_parse_nak(const uint8_t *cif, size_t n,
                  srt_loss_range *out, size_t cap, size_t *out_count) {
    if (n % 4 != 0) return SRT_ERR_INVAL;
    size_t i = 0, c = 0;
    while (i < n) {
        uint32_t w = srt__rd32(cif + i); i += 4;
        if (w & 0x80000000u) {
            /* range start; pair with end */
            if (i >= n) return SRT_ERR_INVAL;
            uint32_t e = srt__rd32(cif + i); i += 4;
            if (e & 0x80000000u) return SRT_ERR_INVAL;
            if (c < cap) { out[c].start = w & SRT_SEQNO_MASK;
                            out[c].end   = e & SRT_SEQNO_MASK; }
            c++;
        } else {
            if (c < cap) { out[c].start = out[c].end = w & SRT_SEQNO_MASK; }
            c++;
        }
    }
    if (out_count) *out_count = c;
    return (c <= cap) ? SRT_OK : SRT_ERR_TOOSHORT;
}

int srt_write_nak(uint8_t *cif, size_t cap,
                  const srt_loss_range *ranges, size_t n_ranges,
                  size_t *out_len) {
    size_t need = 0;
    for (size_t i = 0; i < n_ranges; i++)
        need += (ranges[i].start == ranges[i].end) ? 4u : 8u;
    if (cap < need) return SRT_ERR_TOOSHORT;
    size_t o = 0;
    for (size_t i = 0; i < n_ranges; i++) {
        uint32_t s = ranges[i].start & SRT_SEQNO_MASK;
        uint32_t e = ranges[i].end   & SRT_SEQNO_MASK;
        if (s == e) {
            srt__wr32(cif + o, s); o += 4;
        } else {
            srt__wr32(cif + o, s | 0x80000000u); o += 4;
            srt__wr32(cif + o, e);               o += 4;
        }
    }
    if (out_len) *out_len = need;
    return SRT_OK;
}

/* -------------------------- DROPREQ --------------------------------------- */

int srt_parse_dropreq(const uint8_t *cif, size_t n, srt_dropreq *out) {
    if (n < 8) return SRT_ERR_TOOSHORT;
    out->first_seqno = srt__rd32(cif)     & SRT_SEQNO_MASK;
    out->last_seqno  = srt__rd32(cif + 4) & SRT_SEQNO_MASK;
    /* msgno travels in the type-specific info field, set by caller */
    return SRT_OK;
}

int srt_write_dropreq(uint8_t *cif, size_t cap, const srt_dropreq *d) {
    if (cap < 8) return SRT_ERR_TOOSHORT;
    srt__wr32(cif,     d->first_seqno & SRT_SEQNO_MASK);
    srt__wr32(cif + 4, d->last_seqno  & SRT_SEQNO_MASK);
    return SRT_OK;
}

/* -------------------------- KM message ------------------------------------ */
/*
 * §6 wire layout (big-endian):
 *   byte 0 : S(1) V(3) PT(4)
 *   byte 1-2: Sign (16)
 *   byte 3 : Resv1(6) KK(2)
 *   byte 4-7: KEKI (32)
 *   byte 8 : Cipher
 *   byte 9 : Auth
 *   byte10 : SE
 *   byte11 : Resv2
 *   byte12-13: Resv3
 *   byte14 : SLen/4
 *   byte15 : KLen/4
 *   byte16..: Salt (SLen bytes)
 *   next   : Wrapped Key (8-byte ICV + n*KLen, n = popcount(KK))
 */

static size_t srt__km_nkeys(srt_kk kk) {
    switch (kk) {
        case SRT_KK_EVEN:
        case SRT_KK_ODD:  return 1;
        case SRT_KK_BOTH: return 2;
        default:          return 0;
    }
}

int srt_parse_km(const uint8_t *buf, size_t n, srt_km *out) {
    if (n < 16) return SRT_ERR_TOOSHORT;
    uint8_t b0 = buf[0];
    out->version = (b0 >> 4) & 0x7u; /* skip S bit */
    out->pt      =  b0        & 0xFu;
    out->sign    = srt__rd16(buf + 1);
    out->kk      = (srt_kk)(buf[3] & 0x3u);
    out->keki    = srt__rd32(buf + 4);
    out->cipher  = (srt_cipher)buf[8];
    out->auth    = buf[9];
    out->se      = buf[10];
    out->slen    = (uint8_t)(buf[14] * 4u);
    out->klen    = (uint8_t)(buf[15] * 4u);
    size_t off = 16;
    if (n < off + out->slen) return SRT_ERR_TOOSHORT;
    out->salt = buf + off; off += out->slen;
    size_t nkeys = srt__km_nkeys(out->kk);
    size_t wraplen = 8 + nkeys * out->klen;
    if (n < off + wraplen) return SRT_ERR_TOOSHORT;
    out->wrap     = buf + off;
    out->wrap_len = wraplen;
    if (out->sign != SRT_KM_SIGN) return SRT_ERR_PROTO;
    return SRT_OK;
}

int srt_write_km(uint8_t *buf, size_t cap, const srt_km *k, size_t *out_len) {
    size_t nkeys = srt__km_nkeys(k->kk);
    size_t need  = 16 + k->slen + 8 + nkeys * k->klen;
    if (cap < need) return SRT_ERR_TOOSHORT;
    if (k->slen % 4 || k->klen % 4) return SRT_ERR_INVAL;
    buf[0] = (uint8_t)(((k->version & 0x7u) << 4) | (k->pt & 0xFu));
    srt__wr16(buf + 1, k->sign ? k->sign : SRT_KM_SIGN);
    buf[3] = (uint8_t)(k->kk & 0x3u);
    srt__wr32(buf + 4, k->keki);
    buf[8]  = (uint8_t)k->cipher;
    buf[9]  = k->auth;
    buf[10] = k->se;
    buf[11] = 0;
    srt__wr16(buf + 12, 0);
    buf[14] = (uint8_t)(k->slen / 4u);
    buf[15] = (uint8_t)(k->klen / 4u);
    size_t off = 16;
    if (k->salt && k->slen) memcpy(buf + off, k->salt, k->slen);
    off += k->slen;
    if (k->wrap && k->wrap_len) memcpy(buf + off, k->wrap, k->wrap_len);
    off += 8 + nkeys * k->klen;
    if (out_len) *out_len = off;
    return SRT_OK;
}

/* ========================================================================== */
/*                       Context / socket internals                           */
/* ========================================================================== */

#include <netinet/in.h>
#include <arpa/inet.h>

#define SRT_MAX_PENDING  8      /* listener-side half-open slots */
#define SRT_VERSION_U32  0x00010500u  /* advertise SRT 1.5.0 (compatible with libsrt 1.5.x) */
#define SRT_TX_RING      32     /* outbound UDP packets staged */
#define SRT_WINDOW       256    /* send/recv window in packets (power of 2) */
#define SRT_WINDOW_MASK  (SRT_WINDOW - 1u)
#define SRT_MAX_NAK_RANGES 64
#define SRT_ACK_LOG_LEN    16    /* recent full-ACK sends remembered for RTT */

struct srt_context_s {
    srt_config cfg;
    uint8_t    cookie_secret[16];   /* per-context SYN-cookie secret */
    /* Socket table for multiplexing on one shared UDP socket. Each entry
     * is either NULL or a live socket; indexed arbitrarily. */
    srt_socket **sockets;
    int          sockets_cap;
    int          pull_cursor;       /* round-robin for srt_context_pull_udp */
};

/* Listener-side half-open connection record. */
typedef struct {
    bool                   used;
    struct sockaddr_storage peer;
    socklen_t              peer_len;
    uint32_t               peer_socket_id;
    uint32_t               peer_isn;
    uint32_t               syn_cookie;  /* cookie we issued them */
    uint32_t               local_socket_id;
    uint32_t               local_isn;
    srt_hsreq              peer_hsreq;
    bool                   conclusion_seen;
    /* Received HS extension strings. */
    char                   peer_sid        [512];
    char                   peer_congestion [16];
    char                   peer_filter     [128];
    char                   peer_group      [64];

    /* Encryption state derived from KMREQ, propagated to the child socket. */
    bool                   encrypted;
    srt_cipher             cipher;
    uint8_t                klen;
    uint8_t                salt[16];
    uint8_t                sek[32];
} srt_pending;

/* Window slot used by both send (retx buffer) and recv (reorder buffer).
 * release_us carries dual meaning depending on side:
 *   - send: time the packet was staged (TLPKTDROP deadline reference)
 *   - recv: earliest delivery time set by §8 TSBPD                    */
typedef struct {
    uint8_t  pkt[1600];   /* full UDP payload incl. 16-byte header */
    size_t   len;
    uint32_t seqno;
    uint64_t release_us;
    bool     valid;
} srt_pkt_slot;

typedef struct {
    uint8_t                 buf[1600];
    size_t                  len;
    struct sockaddr_storage dest;
    socklen_t               dest_len;
} srt_tx_slot;

struct srt_socket_s {
    srt_context *ctx;
    srt_sockopts opt;
    srt_state    state;

    uint32_t     local_socket_id;
    uint32_t     peer_socket_id;
    uint32_t     isn_local;
    uint32_t     isn_peer;

    struct sockaddr_storage local_addr;
    socklen_t               local_len;
    struct sockaddr_storage peer_addr;
    socklen_t               peer_len;

    uint64_t     t_create_us;    /* pre-connection timestamp base */
    uint64_t     t0_us;          /* connection-establishment reference */

    uint32_t     rcv_syn_cookie; /* cookie caller echoes in CONCLUSION */
    srt_hsreq    peer_hsreq;

    /* Rendezvous state (§4.4). */
    uint32_t     rdv_own_cookie;
    bool         rdv_contest_done;
    bool         rdv_is_initiator;
    bool         rdv_sent_conclusion;

    /* tx ring: outbound UDP datagrams staged for srt_pull_udp() */
    srt_tx_slot  txq[SRT_TX_RING];
    int          txq_head;
    int          txq_count;

    /* send window (retransmission buffer). Indexed by seqno & WINDOW_MASK. */
    srt_pkt_slot snd[SRT_WINDOW];
    uint32_t     snd_next;          /* next seqno to assign */
    uint32_t     snd_first_unacked; /* oldest unacked seqno */
    uint32_t     snd_msgno;

    /* receive window (reorder buffer). */
    srt_pkt_slot rcv[SRT_WINDOW];
    uint32_t     rcv_next;    /* next expected in-order seqno (gap head)     */
    uint32_t     rcv_high;    /* highest seqno seen                          */
    uint32_t     rcv_read;    /* next seqno to hand to srt_recv              */
    uint32_t     rcv_ready;   /* TSBPD-released watermark: rcv_read..rcv_ready delivers */
    uint16_t     rcv_offset;  /* stream mode: bytes already consumed from the slot at rcv_read */

    /* ACK bookkeeping. */
    uint32_t     ack_counter; /* monotonic ack_number for full ACKs */
    uint32_t     rtt_us;       /* EWMA RTT (our own sample or peer-reported)  */
    uint32_t     rtt_var_us;
    bool         rtt_have_sample;
    /* Log of outgoing full-ACKs used to match ACKACKs for RTT sampling. */
    struct { uint32_t ack_number; uint64_t sent_us; } ack_log[SRT_ACK_LOG_LEN];
    int          ack_log_head;

    /* Timers (all µs, wall-clock supplied by caller). */
    uint64_t     t_last_ack_us;
    uint64_t     t_last_tx_us;
    uint64_t     t_last_rx_us;
    uint64_t     t_last_nak_us;
    uint64_t     t_last_release_us; /* LiveCC: time of last wire release */
    uint64_t     last_clock_us; /* most recent now_us seen by any entry */
    uint32_t     exp_count;

    /* FileCC / flow control. */
    uint32_t     peer_flow_window;   /* latest avail_buf_pkts from peer ACK */
    uint32_t     peer_error;         /* last PEERERROR code received (0=none) */

    /* Received HS extensions (§4.5). Terminated strings. */
    char         peer_sid        [512];
    char         peer_congestion [16];
    char         peer_filter     [128];
    char         peer_group      [64];

    /* Encryption state. Populated during the handshake when a passphrase is
     * configured. §11. */
    bool         encrypted;
    srt_cipher   cipher;      /* AES-CTR or AES-GCM (agreed via KM message) */
    uint8_t      klen;        /* 16/24/32 */
    uint8_t      salt[16];
    uint8_t      seks[2][32]; /* [0]=even, [1]=odd */
    bool         has_sek[2];
    uint8_t      active_sek;  /* 0 or 1: which parity we encrypt outbound with */

    /* Listener accept queue. */
    srt_pending  pending[SRT_MAX_PENDING];
    int          pending_head;   /* next to deliver via accept() */
    int          pending_count;
};

static void *srt__alloc(srt_context *c, size_t n) {
    if (c->cfg.allocator.alloc)
        return c->cfg.allocator.alloc(c->cfg.allocator.ud, n);
    return malloc(n);
}
static void srt__free(srt_context *c, void *p) {
    if (!p) return;
    if (c->cfg.allocator.free) c->cfg.allocator.free(c->cfg.allocator.ud, p);
    else free(p);
}

static uint32_t srt__rand32(srt_context *c) {
    uint8_t b[4];
    c->cfg.crypto.random(c->cfg.crypto.ctx, b, 4);
    return srt__rd32(b);
}

/* Forward declarations for impls that live lower in the file. */
static int  srt__tx_push      (srt_socket *s, const uint8_t *buf, size_t n);
static int  srt__emit_shutdown(srt_socket *s, uint64_t now_us);
static void srt__advance_ready(srt_socket *s, uint64_t now_us);

srt_context *srt_create(const srt_config *cfg) {
    srt_context *c = (srt_context *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (cfg) c->cfg = *cfg;
    if (c->cfg.max_sockets == 0) c->cfg.max_sockets = 1024;
    if (c->cfg.crypto.aes_ctr == NULL) srt_crypto_openssl(&c->cfg.crypto);
    c->cfg.crypto.random(c->cfg.crypto.ctx, c->cookie_secret,
                         sizeof c->cookie_secret);
    c->sockets_cap = (int)c->cfg.max_sockets;
    c->sockets = (srt_socket **)calloc((size_t)c->sockets_cap,
                                       sizeof(srt_socket *));
    if (!c->sockets) { free(c); return NULL; }
    return c;
}

void srt_destroy(srt_context *ctx) {
    if (!ctx) return;
    free(ctx->sockets);
    free(ctx);
}

static int srt__register(srt_context *ctx, srt_socket *s) {
    for (int i = 0; i < ctx->sockets_cap; i++)
        if (!ctx->sockets[i]) { ctx->sockets[i] = s; return SRT_OK; }
    return SRT_ERR_NOMEM;
}

static void srt__unregister(srt_context *ctx, srt_socket *s) {
    for (int i = 0; i < ctx->sockets_cap; i++)
        if (ctx->sockets[i] == s) { ctx->sockets[i] = NULL; return; }
}

srt_socket *srt_socket_new(srt_context *ctx, const srt_sockopts *opt) {
    if (!ctx) return NULL;
    srt_socket *s = (srt_socket *)srt__alloc(ctx, sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->ctx   = ctx;
    s->state = SRT_STATE_INIT;
    if (opt) s->opt = *opt;
    if (s->opt.mtu == 0)         s->opt.mtu = SRT_DEFAULT_MTU;
    if (s->opt.flow_window == 0) s->opt.flow_window = SRT_DEFAULT_FLOW_WINDOW;
    /* Allocate a nonzero local socket id. */
    do { s->local_socket_id = srt__rand32(ctx); } while (s->local_socket_id == 0);
    s->isn_local = srt__rand32(ctx) & SRT_SEQNO_MASK;
    if (srt__register(ctx, s) != SRT_OK) { srt__free(ctx, s); return NULL; }
    return s;
}

int srt_shutdown(srt_socket *s) {
    if (!s) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_CONNECTED) return SRT_OK;
    int rc = srt__emit_shutdown(s, 0);
    s->state = SRT_STATE_CLOSING;
    return rc;
}

int srt_close(srt_socket *s) {
    if (!s) return SRT_ERR_INVAL;
    /* Best-effort SHUTDOWN if user skipped srt_shutdown; may be lost if the
     * caller doesn't drain the tx ring before this call. */
    if (s->state == SRT_STATE_CONNECTED && s->peer_socket_id != 0)
        srt__emit_shutdown(s, 0);
    s->state = SRT_STATE_CLOSED;
    srt__unregister(s->ctx, s);
    srt__free(s->ctx, s);
    return SRT_OK;
}

srt_state srt_get_state(const srt_socket *s) {
    return s ? s->state : SRT_STATE_BROKEN;
}

/* -------------------------- sockaddr helpers ------------------------------ */

static void srt__store_addr(struct sockaddr_storage *dst, socklen_t *dlen,
                            const struct sockaddr *src, socklen_t slen) {
    if (slen > (socklen_t)sizeof *dst) slen = sizeof *dst;
    memcpy(dst, src, slen);
    *dlen = slen;
}

/* §4.1 peer IP: 128 bits. Reference libsrt encodes IPv4 in the **first 4
 * bytes byte-reversed from network order** — i.e. little-endian when read
 * off the wire on a BE host — and zero-pads the rest. IPv6 goes in
 * network order. We follow libsrt's convention for interop. */
static void srt__addr_to_peer_ip(uint8_t ip[16], const struct sockaddr *sa) {
    memset(ip, 0, 16);
    if (!sa) return;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        /* libsrt writes IPv4 byte-reversed from network order — i.e. for
         * 127.0.0.1 the peer_ip bytes are [0x01, 0, 0, 0x7F]. */
        const uint8_t *p = (const uint8_t *)&s4->sin_addr.s_addr;
        ip[0] = p[3]; ip[1] = p[2]; ip[2] = p[1]; ip[3] = p[0];
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        memcpy(ip, &s6->sin6_addr, 16);
    }
}

/* -------------------------- SYN cookie ------------------------------------ */
/* §4.3: time-based cookie with ~1-minute accuracy. We XOR-mix a per-context
 * secret with the peer address and a 60 s epoch. Accept current OR previous
 * epoch on verify to tolerate the boundary. TODO: upgrade to HMAC-SHA256. */

static uint32_t srt__mix32(const uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t h = seed;
    for (size_t i = 0; i < n; i++) {
        h ^= buf[i];
        h *= 0x01000193u;       /* FNV-1a prime */
    }
    return h ? (h & 0x7FFFFFFFu) : 1u; /* never zero */
}

static uint32_t srt__cookie_at(srt_context *c,
                               const struct sockaddr *peer, uint32_t epoch) {
    uint8_t buf[16 + 2 + 4];
    srt__addr_to_peer_ip(buf, peer);
    uint16_t port = 0;
    if (peer) {
        if (peer->sa_family == AF_INET)
            port = ((const struct sockaddr_in  *)peer)->sin_port;
        else if (peer->sa_family == AF_INET6)
            port = ((const struct sockaddr_in6 *)peer)->sin6_port;
    }
    srt__wr16(buf + 16, port);
    srt__wr32(buf + 18, epoch);
    uint32_t seed = srt__rd32(c->cookie_secret)
                  ^ srt__rd32(c->cookie_secret + 4)
                  ^ srt__rd32(c->cookie_secret + 8)
                  ^ srt__rd32(c->cookie_secret + 12);
    return srt__mix32(buf, sizeof buf, seed);
}

static uint32_t srt__cookie_gen(srt_context *c,
                                const struct sockaddr *peer, uint64_t now_us) {
    uint32_t epoch = (uint32_t)((now_us / 1000000u) / SRT_SYN_COOKIE_GRANULARITY);
    return srt__cookie_at(c, peer, epoch);
}

static bool srt__cookie_verify(srt_context *c, const struct sockaddr *peer,
                               uint32_t cookie, uint64_t now_us) {
    uint32_t epoch = (uint32_t)((now_us / 1000000u) / SRT_SYN_COOKIE_GRANULARITY);
    if (srt__cookie_at(c, peer, epoch)     == cookie) return true;
    if (epoch && srt__cookie_at(c, peer, epoch - 1) == cookie) return true;
    return false;
}

/* -------------------------- HS packet build/parse ------------------------- */

/*
 * Build a complete HS UDP packet into `buf`. Returns total bytes written
 * (>=64) or a negative srt_err. Extensions (if any) are appended as a
 * contiguous blob built by srt__build_hsreq_tlv / KMREQ / etc.
 */
static int srt__build_hs_pkt(uint8_t *buf, size_t cap,
                             uint32_t dst_socket_id, uint32_t ts_us,
                             uint32_t hs_version, srt_enc_advert enc,
                             uint16_t ext_field, uint32_t hs_type,
                             uint32_t isn, uint32_t mtu, uint32_t fw,
                             uint32_t my_socket_id, uint32_t syn_cookie,
                             const struct sockaddr *peer,
                             const uint8_t *ext_tlv, size_t ext_len) {
    size_t need = SRT_HEADER_SIZE + SRT_HS_CIF_SIZE + ext_len;
    if (cap < need) return SRT_ERR_TOOSHORT;

    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_HANDSHAKE;
    ch.subtype       = 0;
    ch.type_info     = 0;
    ch.timestamp     = ts_us;
    ch.dst_socket_id = dst_socket_id;
    int rc = srt_write_ctrl_hdr(buf, cap, &ch);
    if (rc) return rc;

    srt_hs_cif hs = {0};
    hs.hs_version      = hs_version;
    hs.encryption      = (uint16_t)enc;
    hs.extension_field = ext_field;
    hs.isn             = isn & SRT_SEQNO_MASK;
    hs.mtu             = mtu;
    hs.flow_window     = fw;
    hs.hs_type         = hs_type;
    hs.srt_socket_id   = my_socket_id;
    hs.syn_cookie      = syn_cookie;
    srt__addr_to_peer_ip(hs.peer_ip, peer);
    hs.ext_data = ext_tlv;
    hs.ext_len  = ext_len;
    rc = srt_write_hs_cif(buf + SRT_HEADER_SIZE, cap - SRT_HEADER_SIZE, &hs);
    if (rc) return rc;

    return (int)need;
}

/* §4.5 SID/CONGESTION/FILTER/GROUP: each is a CONFIG TLV carrying an ASCII
 * string, zero-padded to a 4-byte boundary. Returns bytes written or 0. */
static size_t srt__build_str_tlv(uint8_t *out, size_t cap,
                                 uint16_t cmd, const char *str) {
    if (!str || !*str) return 0;
    size_t slen = strlen(str);
    size_t words = (slen + 3u) / 4u;
    size_t byt   = words * 4u;
    if (cap < 4u + byt) return 0;
    srt__wr16(out,     cmd);
    srt__wr16(out + 2, (uint16_t)words);
    memcpy(out + 4, str, slen);
    memset(out + 4 + slen, 0, byt - slen);
    return 4u + byt;
}

/* Copy a zero-padded TLV string payload into a caller-sized char buffer. */
static void srt__copy_str_payload(char *dst, size_t cap,
                                  const uint8_t *p, uint16_t len) {
    if (cap == 0) return;
    size_t n = len < cap - 1 ? len : cap - 1;
    /* Trim the trailing zero padding added at build time. */
    while (n > 0 && p[n - 1] == '\0') n--;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* §4.6: Encode HSREQ/HSRSP as a single TLV (cmd, lenw=3, 12-byte payload). */
static size_t srt__build_hsreq_tlv(uint8_t out[16], uint16_t cmd,
                                   const srt_hsreq *h) {
    srt__wr16(out, cmd);
    srt__wr16(out + 2, 3);  /* length in 32-bit words */
    srt_write_hsreq(out + 4, 12, h);
    return 16;
}

static int srt__parse_hsreq_from_ext(const srt_hs_cif *hs,
                                     uint16_t want, srt_hsreq *out) {
    srt_ext_iter it;
    srt_ext_iter_init(&it, hs->ext_data, hs->ext_len);
    uint16_t cmd, len;
    const uint8_t *p;
    while (srt_ext_iter_next(&it, &cmd, &p, &len) == SRT_OK) {
        if (cmd == want) return srt_parse_hsreq(p, len, out);
    }
    return SRT_ERR_PROTO;
}

static uint32_t srt__ts_us(const srt_socket *s, uint64_t now_us) {
    uint64_t base = s->t0_us ? s->t0_us : s->t_create_us;
    return (uint32_t)(now_us - base);
}

/* -------------------------- encryption helpers ---------------------------- */

#include <openssl/hmac.h>

/* Map advertised cipher strength to key length in bytes. */
static size_t srt__klen_for(srt_enc_advert e) {
    switch (e) {
        case SRT_ENC_AES_128: return 16;
        case SRT_ENC_AES_192: return 24;
        case SRT_ENC_AES_256: return 32;
        default:              return 0;
    }
}

/*
 * Derive the KEK from a passphrase. The SRT spec uses PBKDF2-HMAC-SHA1 with
 * 2048 iterations over the **last 8 bytes** of the KM salt.
 * TODO: hide this behind the crypto vtable for non-OpenSSL builds.
 */
static int srt__derive_kek(const char *pass, const uint8_t *salt, size_t slen,
                           uint8_t *kek, size_t klen) {
    if (!pass || !salt || slen < 8) return SRT_ERR_INVAL;
    const uint8_t *usalt = salt + slen - 8;
    int rc = PKCS5_PBKDF2_HMAC_SHA1(pass, (int)strlen(pass), usalt, 8,
                                    2048, (int)klen, kek);
    return rc == 1 ? SRT_OK : SRT_ERR_CRYPTO;
}

/*
 * §11.3 AES-CTR IV layout (libsrt/haicrypt compatible):
 *
 *   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 * +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 * |                   0s                  |      pki      |  ctr  |
 * +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *                              XOR
 * +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 * |                         nonce (salt[16])                      |
 * +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
 *
 * pki = packet sequence number (big-endian 32-bit at bytes 10..13);
 * ctr = 16-bit block counter that OpenSSL AES-CTR increments internally,
 *       so we leave bytes 14..15 at salt[14..15].
 */
static void srt__build_iv_ctr(const uint8_t salt[16], uint32_t seqno,
                              uint8_t iv[16]) {
    memcpy(iv, salt, 16);
    iv[10] ^= (uint8_t)(seqno >> 24);
    iv[11] ^= (uint8_t)(seqno >> 16);
    iv[12] ^= (uint8_t)(seqno >>  8);
    iv[13] ^= (uint8_t)(seqno);
}

/* GCM uses a 12-byte IV. We reuse the low-12-byte salt prefix and mix the
 * packet sequence number into bytes 8..11 so each packet gets a unique nonce
 * under a given SEK. */
static void srt__build_iv_gcm(const uint8_t salt[16], uint32_t seqno,
                              uint8_t iv[12]) {
    memcpy(iv, salt, 12);
    iv[8]  ^= (uint8_t)(seqno >> 24);
    iv[9]  ^= (uint8_t)(seqno >> 16);
    iv[10] ^= (uint8_t)(seqno >>  8);
    iv[11] ^= (uint8_t)(seqno);
}

/* AES-CTR is self-inverse; same helper encrypts and decrypts, `parity`
 * picks even (0) or odd (1) SEK. */
static int srt__xcrypt(srt_socket *s, uint32_t seqno,
                       uint8_t *buf, size_t n, int parity) {
    if (!s->encrypted || s->klen == 0) return SRT_OK;
    if (parity < 0 || parity > 1 || !s->has_sek[parity]) return SRT_ERR_CRYPTO;
    uint8_t iv[16];
    srt__build_iv_ctr(s->salt, seqno, iv);
    return s->ctx->cfg.crypto.aes_ctr(s->ctx->cfg.crypto.ctx,
                                      s->seks[parity], s->klen, iv, buf, buf, n);
}

/* Normalize the 16-byte packet header for use as GCM AAD: clear the R bit
 * (byte 4, bit 2) so retransmissions produce the same tag even though the
 * sender flips R=1 on retx. */
static void srt__aad_from_hdr(uint8_t aad[16], const uint8_t *hdr) {
    memcpy(aad, hdr, 16);
    aad[4] &= (uint8_t)~0x04u;
}

static int srt__seal(srt_socket *s, uint32_t seqno, const uint8_t *hdr,
                     uint8_t *payload, size_t n, uint8_t tag[16], int parity) {
    if (parity < 0 || parity > 1 || !s->has_sek[parity]) return SRT_ERR_CRYPTO;
    uint8_t iv[12], aad[16];
    srt__build_iv_gcm(s->salt, seqno, iv);
    srt__aad_from_hdr(aad, hdr);
    return s->ctx->cfg.crypto.aes_gcm_seal(
        s->ctx->cfg.crypto.ctx, s->seks[parity], s->klen,
        iv, 12, aad, 16, payload, payload, n, tag);
}

static int srt__open(srt_socket *s, uint32_t seqno, const uint8_t *hdr,
                     uint8_t *payload, size_t n, const uint8_t tag[16],
                     int parity) {
    if (parity < 0 || parity > 1 || !s->has_sek[parity]) return SRT_ERR_CRYPTO;
    uint8_t iv[12], aad[16];
    srt__build_iv_gcm(s->salt, seqno, iv);
    srt__aad_from_hdr(aad, hdr);
    return s->ctx->cfg.crypto.aes_gcm_open(
        s->ctx->cfg.crypto.ctx, s->seks[parity], s->klen,
        iv, 12, aad, 16, payload, payload, n, tag);
}

/* Caller-side: build a KMREQ TLV carrying a freshly-wrapped SEK. Stashes the
 * cleartext SEK + salt in `s` so the data path can encrypt outbound payloads. */
static int srt__build_kmreq_tlv(srt_socket *s, uint8_t *out, size_t cap,
                                size_t *out_len) {
    size_t klen = srt__klen_for(s->opt.encryption);
    if (klen == 0 || !s->opt.passphrase) return SRT_ERR_INVAL;

    s->klen = (uint8_t)klen;
    s->ctx->cfg.crypto.random(s->ctx->cfg.crypto.ctx, s->salt, 16);
    s->ctx->cfg.crypto.random(s->ctx->cfg.crypto.ctx, s->seks[0], klen);
    s->has_sek[0] = true;
    s->active_sek = 0;

    uint8_t kek[32];
    int rc = srt__derive_kek(s->opt.passphrase, s->salt, 16, kek, klen);
    if (rc) return rc;

    uint8_t wrapped[8 + 32];
    rc = s->ctx->cfg.crypto.kw_wrap(s->ctx->cfg.crypto.ctx,
                                    kek, klen, s->seks[0], klen, wrapped);
    if (rc) return rc;

    srt_km km = {0};
    srt_cipher cipher = s->opt.cipher ? s->opt.cipher : SRT_CIPHER_AES_CTR;
    s->cipher = cipher;
    km.version = 1;
    km.pt      = 2;
    km.sign    = SRT_KM_SIGN;
    km.kk      = SRT_KK_EVEN;
    km.keki    = 0;
    km.cipher  = cipher;
    km.auth    = (cipher == SRT_CIPHER_AES_GCM) ? 1 : 0;
    km.se      = 0;
    km.slen    = 16;
    km.klen    = (uint8_t)klen;
    km.salt    = s->salt;
    km.wrap    = wrapped;
    km.wrap_len = 8 + klen;

    uint8_t km_buf[128];
    size_t km_len = 0;
    rc = srt_write_km(km_buf, sizeof km_buf, &km, &km_len);
    if (rc) return rc;
    /* TLV length is in 32-bit words, so pad to a 4-byte boundary. */
    while (km_len & 3u) km_buf[km_len++] = 0;
    if (cap < 4 + km_len) return SRT_ERR_TOOSHORT;

    srt__wr16(out,     SRT_CMD_KMREQ);
    srt__wr16(out + 2, (uint16_t)(km_len / 4u));
    memcpy(out + 4, km_buf, km_len);
    *out_len = 4 + km_len;
    s->encrypted = true;
    return SRT_OK;
}

/* Listener-side: find KMREQ in the ext blob, unwrap SEK into `p`. Returns
 * SRT_OK whether or not a KMREQ is present; sets p->encrypted only if it is. */
static int srt__consume_kmreq(const srt_hs_cif *hs, const char *pass,
                              const srt_crypto_vtable *cv, srt_pending *p) {
    srt_ext_iter it;
    srt_ext_iter_init(&it, hs->ext_data, hs->ext_len);
    uint16_t cmd, len;
    const uint8_t *payload;
    while (srt_ext_iter_next(&it, &cmd, &payload, &len) == SRT_OK) {
        if (cmd != SRT_CMD_KMREQ) continue;
        if (!pass) return SRT_ERR_CRYPTO;  /* peer wants encryption; we lack passphrase */
        srt_km km;
        if (srt_parse_km(payload, len, &km) != SRT_OK) return SRT_ERR_PROTO;
        if (km.slen != 16 || km.klen == 0 || km.klen > 32) return SRT_ERR_PROTO;
        if (km.kk != SRT_KK_EVEN) return SRT_ERR_UNSUPPORTED;  /* TODO: ODD/BOTH */
        if (km.wrap_len != 8u + km.klen) return SRT_ERR_PROTO;

        uint8_t kek[32];
        int rc = srt__derive_kek(pass, km.salt, km.slen, kek, km.klen);
        if (rc) return rc;
        rc = cv->kw_unwrap(cv->ctx, kek, km.klen, km.wrap, km.wrap_len, p->sek);
        if (rc) return SRT_ERR_CRYPTO;

        memcpy(p->salt, km.salt, 16);
        p->klen      = km.klen;
        p->cipher    = km.cipher;
        p->encrypted = true;
        return SRT_OK;
    }
    return SRT_OK;
}

/* -------------------------- tx ring --------------------------------------- */

/* Push a staged datagram. If dest is NULL, the socket's own peer_addr is used
 * — which is the right default for CONNECTED sockets and for carriers that
 * emit their own stream. Listener-side HS replies pass the peer explicitly so
 * several pending handshakes to different callers don't collide. */
static int srt__tx_push_to(srt_socket *s, const uint8_t *buf, size_t n,
                           const struct sockaddr *dest, socklen_t dlen) {
    if (n > sizeof s->txq[0].buf) return SRT_ERR_TOOSHORT;
    if (s->txq_count >= SRT_TX_RING) return SRT_ERR_AGAIN;
    int idx = (s->txq_head + s->txq_count) % SRT_TX_RING;
    memcpy(s->txq[idx].buf, buf, n);
    s->txq[idx].len = n;
    if (dest && dlen) {
        memcpy(&s->txq[idx].dest, dest, (size_t)dlen);
        s->txq[idx].dest_len = dlen;
    } else {
        memcpy(&s->txq[idx].dest, &s->peer_addr, (size_t)s->peer_len);
        s->txq[idx].dest_len = s->peer_len;
    }
    s->txq_count++;
    return SRT_OK;
}

static int srt__tx_push(srt_socket *s, const uint8_t *buf, size_t n) {
    return srt__tx_push_to(s, buf, n, NULL, 0);
}

/* Legacy alias used by the HS builders below. */
static int srt__tx_stage(srt_socket *s, const uint8_t *buf, size_t n) {
    return srt__tx_push(s, buf, n);
}

/* LiveCC §12.1: min inter-release gap in µs for a packet of `bytes` bytes. */
static uint64_t srt__pace_interval(const srt_socket *s, size_t bytes) {
    if (!s->opt.max_bw_bps) return 0;
    return (uint64_t)bytes * 1000000u / s->opt.max_bw_bps;
}

static bool srt__slot_is_data(const srt_tx_slot *slot) {
    return slot->len >= 1 && (slot->buf[0] & 0x80) == 0;
}

/* When is the next staged packet eligible to go out? 0 = no pacing needed. */
static uint64_t srt__next_release_us(const srt_socket *s) {
    if (s->txq_count == 0) return 0;
    const srt_tx_slot *head = &s->txq[s->txq_head];
    if (!srt__slot_is_data(head)) return 0;
    return s->t_last_release_us + srt__pace_interval(s, head->len);
}

int srt_pull_udp(srt_socket *s, void *out, size_t cap, uint64_t now_us) {
    if (!s || !out) return SRT_ERR_INVAL;
    s->last_clock_us = now_us;
    if (s->txq_count == 0) return SRT_ERR_AGAIN;
    srt_tx_slot *slot = &s->txq[s->txq_head];
    /* Only DATA packets are paced. Control must flow freely — HS, ACK, NAK,
     * KEEPALIVE cannot afford to queue behind bandwidth throttling. */
    if (s->opt.max_bw_bps && srt__slot_is_data(slot)) {
        uint64_t due = s->t_last_release_us
                     + srt__pace_interval(s, slot->len);
        if (now_us < due) return SRT_ERR_AGAIN;
    }
    if (cap < slot->len) return SRT_ERR_TOOSHORT;
    memcpy(out, slot->buf, slot->len);
    int n = (int)slot->len;
    s->txq_head = (s->txq_head + 1) % SRT_TX_RING;
    s->txq_count--;
    s->t_last_tx_us = now_us;
    if (srt__slot_is_data(slot)) s->t_last_release_us = now_us;
    return n;
}

/* -------------------------- state-machine: caller ------------------------- */

static int srt__emit_induction(srt_socket *s, uint64_t now_us) {
    /* §4.3 step 1: HS version = 4, Ext = 2, dst sock id = 0, cookie = 0. */
    uint8_t pkt[64];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        /*dst*/ 0, srt__ts_us(s, now_us),
        /*hsv*/ 4, SRT_ENC_NONE, /*ext*/ 2, SRT_HS_INDUCTION,
        s->isn_local, s->opt.mtu, s->opt.flow_window,
        s->local_socket_id, /*cookie*/ 0,
        (const struct sockaddr *)&s->peer_addr,
        NULL, 0);
    if (len < 0) return len;
    return srt__tx_stage(s, pkt, (size_t)len);
}

static int srt__emit_conclusion_caller(srt_socket *s, uint64_t now_us) {
    /* §4.3 step 3: HS version = 5, echo SYN cookie, include HSREQ (+ optional
     * KMREQ, SID, ...). For v0 we emit only HSREQ. */
    srt_hsreq req = {0};
    req.srt_version = SRT_VERSION_U32;
    req.srt_flags   = SRT_FLAG_CRYPT | SRT_FLAG_REXMITFLG;
    if (s->opt.tsbpd)        req.srt_flags |= SRT_FLAG_TSBPDSND | SRT_FLAG_TSBPDRCV;
    if (s->opt.tlpktdrop)    req.srt_flags |= SRT_FLAG_TLPKTDROP;
    if (s->opt.periodic_nak) req.srt_flags |= SRT_FLAG_PERIODICNAK;
    if (s->opt.stream_mode)  req.srt_flags |= SRT_FLAG_STREAM;
    req.rcv_tsbpd_delay = s->opt.rcv_tsbpd_ms;
    req.snd_tsbpd_delay = s->opt.snd_tsbpd_ms;

    uint8_t  ext[1024];
    size_t   ext_len = srt__build_hsreq_tlv(ext, SRT_CMD_HSREQ, &req);
    uint16_t ext_field = SRT_EXT_HSREQ;

    /* Attach KMREQ if the caller configured encryption + passphrase. */
    if (s->opt.encryption != SRT_ENC_NONE && s->opt.passphrase) {
        size_t km_tlv_len = 0;
        int rc = srt__build_kmreq_tlv(s, ext + ext_len,
                                      sizeof ext - ext_len, &km_tlv_len);
        if (rc) return rc;
        ext_len   += km_tlv_len;
        ext_field |= SRT_EXT_KMREQ;
    }

    /* §4.5 CONFIG TLVs. */
    size_t added = 0;
    added += srt__build_str_tlv(ext + ext_len + added,
                                sizeof ext - ext_len - added,
                                SRT_CMD_SID,        s->opt.stream_id);
    added += srt__build_str_tlv(ext + ext_len + added,
                                sizeof ext - ext_len - added,
                                SRT_CMD_CONGESTION, s->opt.congestion);
    added += srt__build_str_tlv(ext + ext_len + added,
                                sizeof ext - ext_len - added,
                                SRT_CMD_FILTER,     s->opt.filter);
    added += srt__build_str_tlv(ext + ext_len + added,
                                sizeof ext - ext_len - added,
                                SRT_CMD_GROUP,      s->opt.group);
    if (added) { ext_len += added; ext_field |= SRT_EXT_CONFIG; }

    uint8_t pkt[512];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        s->peer_socket_id, srt__ts_us(s, now_us),
        /*hsv*/ 5, s->opt.encryption, ext_field,
        SRT_HS_CONCLUSION,
        s->isn_local, s->opt.mtu, s->opt.flow_window,
        s->local_socket_id, s->rcv_syn_cookie,
        (const struct sockaddr *)&s->peer_addr,
        ext, ext_len);
    if (len < 0) return len;
    return srt__tx_stage(s, pkt, (size_t)len);
}

int srt_connect(srt_socket *s, const struct sockaddr *remote, socklen_t len) {
    if (!s || !remote) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_INIT) return SRT_ERR_INVAL;
    srt__store_addr(&s->peer_addr, &s->peer_len, remote, len);
    s->opt.caller = true;
    s->t_create_us = 0;  /* set to real clock on first feed/connect if desired */
    s->state = SRT_STATE_CONNECTING;
    return srt__emit_induction(s, /*now_us*/ 0);
}

/* -------------------------- state-machine: rendezvous --------------------- */

/* §4.5 cookie contest. Returns 1 = initiator, 0 = responder, -1 = draw. */
static int srt__cookie_contest(uint32_t host, uint32_t peer) {
    int64_t contest = (int64_t)(int32_t)host - (int64_t)(int32_t)peer;
    if ((contest & 0xFFFFFFFFLL) == 0) return -1;
    if (contest & 0x80000000LL) return 0;
    return 1;
}

static int srt__emit_waveahand(srt_socket *s, uint64_t now_us) {
    /* §4.4 step 1: HSv5, ext=0, carries our time-based cookie. */
    uint8_t pkt[64];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        /*dst*/ 0, srt__ts_us(s, now_us),
        /*hsv*/ 5, s->opt.encryption, /*ext*/ 0, SRT_HS_WAVEAHAND,
        s->isn_local, s->opt.mtu, s->opt.flow_window,
        s->local_socket_id, s->rdv_own_cookie,
        (const struct sockaddr *)&s->peer_addr, NULL, 0);
    if (len < 0) return len;
    return srt__tx_push(s, pkt, (size_t)len);
}

/* Build a CONCLUSION packet carrying HSREQ or HSRSP and (optionally) KMREQ. */
static int srt__emit_rdv_conclusion(srt_socket *s, uint16_t cmd,
                                    uint64_t now_us) {
    srt_hsreq req = {0};
    req.srt_version = SRT_VERSION_U32;
    req.srt_flags   = SRT_FLAG_CRYPT | SRT_FLAG_REXMITFLG;
    if (s->opt.tsbpd)        req.srt_flags |= SRT_FLAG_TSBPDSND | SRT_FLAG_TSBPDRCV;
    if (s->opt.tlpktdrop)    req.srt_flags |= SRT_FLAG_TLPKTDROP;
    if (s->opt.periodic_nak) req.srt_flags |= SRT_FLAG_PERIODICNAK;
    if (s->opt.stream_mode)  req.srt_flags |= SRT_FLAG_STREAM;
    req.rcv_tsbpd_delay = s->opt.rcv_tsbpd_ms;
    req.snd_tsbpd_delay = s->opt.snd_tsbpd_ms;

    uint8_t   ext[1024];
    size_t    ext_len = srt__build_hsreq_tlv(ext, cmd, &req);
    uint16_t  ext_field = SRT_EXT_HSREQ;
    if (cmd == SRT_CMD_HSREQ
        && s->opt.encryption != SRT_ENC_NONE && s->opt.passphrase) {
        size_t km_len = 0;
        int rc = srt__build_kmreq_tlv(s, ext + ext_len,
                                      sizeof ext - ext_len, &km_len);
        if (rc) return rc;
        ext_len   += km_len;
        ext_field |= SRT_EXT_KMREQ;
    }
    if (cmd == SRT_CMD_HSREQ) {
        size_t cfg = 0;
        cfg += srt__build_str_tlv(ext + ext_len + cfg,
                                  sizeof ext - ext_len - cfg,
                                  SRT_CMD_SID,        s->opt.stream_id);
        cfg += srt__build_str_tlv(ext + ext_len + cfg,
                                  sizeof ext - ext_len - cfg,
                                  SRT_CMD_CONGESTION, s->opt.congestion);
        cfg += srt__build_str_tlv(ext + ext_len + cfg,
                                  sizeof ext - ext_len - cfg,
                                  SRT_CMD_FILTER,     s->opt.filter);
        cfg += srt__build_str_tlv(ext + ext_len + cfg,
                                  sizeof ext - ext_len - cfg,
                                  SRT_CMD_GROUP,      s->opt.group);
        if (cfg) { ext_len += cfg; ext_field |= SRT_EXT_CONFIG; }
    }

    uint8_t pkt[512];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        s->peer_socket_id, srt__ts_us(s, now_us),
        /*hsv*/ 5, s->opt.encryption, ext_field, SRT_HS_CONCLUSION,
        s->isn_local, s->opt.mtu, s->opt.flow_window,
        s->local_socket_id, s->rdv_own_cookie,
        (const struct sockaddr *)&s->peer_addr, ext, ext_len);
    if (len < 0) return len;
    return srt__tx_push(s, pkt, (size_t)len);
}

static int srt__emit_agreement(srt_socket *s, uint64_t now_us) {
    uint8_t pkt[64];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        s->peer_socket_id, srt__ts_us(s, now_us),
        /*hsv*/ 5, s->opt.encryption, /*ext*/ 0, SRT_HS_AGREEMENT,
        s->isn_local, s->opt.mtu, s->opt.flow_window,
        s->local_socket_id, s->rdv_own_cookie,
        (const struct sockaddr *)&s->peer_addr, NULL, 0);
    if (len < 0) return len;
    return srt__tx_push(s, pkt, (size_t)len);
}

/* Finish the rendezvous by anchoring cursors + state. */
static void srt__rdv_mark_connected(srt_socket *s, uint64_t now_us) {
    s->snd_next = s->snd_first_unacked = s->isn_local;
    s->rcv_next = s->rcv_read = s->rcv_ready = s->isn_peer;
    s->rcv_high = srt_seq_add(s->isn_peer, -1);
    s->snd_msgno = 1;
    s->state = SRT_STATE_CONNECTED;
    s->t0_us = now_us;
    s->t_last_ack_us = s->t_last_tx_us = s->t_last_rx_us = now_us;
}

static int srt__feed_rendezvous_hs(srt_socket *s, const srt_ctrl_hdr *ch,
                                   const srt_hs_cif *hs, uint64_t now_us) {
    (void)ch;
    if (hs->hs_version != 5) return SRT_ERR_PROTO;

    if (hs->hs_type == SRT_HS_WAVEAHAND) {
        /* Cookie contest on first WAVEAHAND received. */
        int role = srt__cookie_contest(s->rdv_own_cookie, hs->syn_cookie);
        if (role == -1) {
            /* Draw: regenerate our cookie and re-send WAVEAHAND. */
            s->rdv_own_cookie ^= 0x55555555u;
            return srt__emit_waveahand(s, now_us);
        }
        s->rdv_contest_done = true;
        s->rdv_is_initiator = (role == 1);
        s->peer_socket_id   = hs->srt_socket_id;
        s->isn_peer         = hs->isn;
        if (s->rdv_is_initiator && !s->rdv_sent_conclusion) {
            int rc = srt__emit_rdv_conclusion(s, SRT_CMD_HSREQ, now_us);
            if (rc) return rc;
            s->rdv_sent_conclusion = true;
        }
        return SRT_OK;
    }

    if (hs->hs_type == SRT_HS_CONCLUSION) {
        /* Late first packet: if we haven't run the contest yet, infer role. */
        if (!s->rdv_contest_done) {
            int role = srt__cookie_contest(s->rdv_own_cookie, hs->syn_cookie);
            s->rdv_contest_done = true;
            s->rdv_is_initiator = (role == 1);
            s->peer_socket_id   = hs->srt_socket_id;
            s->isn_peer         = hs->isn;
        }
        if (s->rdv_is_initiator) {
            /* Peer's response with HSRSP → we finalize. */
            srt_hsreq rsp;
            if (srt__parse_hsreq_from_ext(hs, SRT_CMD_HSRSP, &rsp) != SRT_OK)
                return SRT_ERR_PROTO;
            s->peer_hsreq = rsp;
            int rc = srt__emit_agreement(s, now_us);
            if (rc) return rc;
            srt__rdv_mark_connected(s, now_us);
            return SRT_OK;
        } else {
            /* Peer (initiator) sent CONCLUSION+HSREQ — we reply with HSRSP.
             * Also unwrap their KMREQ if encryption was negotiated. */
            srt_hsreq req;
            if (srt__parse_hsreq_from_ext(hs, SRT_CMD_HSREQ, &req) != SRT_OK)
                return SRT_ERR_PROTO;
            s->peer_hsreq = req;
            /* CONFIG TLVs from the initiator's CONCLUSION. */
            {
                srt_ext_iter it; uint16_t cmd, clen; const uint8_t *cp;
                srt_ext_iter_init(&it, hs->ext_data, hs->ext_len);
                while (srt_ext_iter_next(&it, &cmd, &cp, &clen) == SRT_OK) {
                    switch (cmd) {
                    case SRT_CMD_SID:
                        srt__copy_str_payload(s->peer_sid,
                                              sizeof s->peer_sid, cp, clen); break;
                    case SRT_CMD_CONGESTION:
                        srt__copy_str_payload(s->peer_congestion,
                                              sizeof s->peer_congestion, cp, clen); break;
                    case SRT_CMD_FILTER:
                        srt__copy_str_payload(s->peer_filter,
                                              sizeof s->peer_filter, cp, clen); break;
                    case SRT_CMD_GROUP:
                        srt__copy_str_payload(s->peer_group,
                                              sizeof s->peer_group, cp, clen); break;
                    default: break;
                    }
                }
            }
            srt_pending tmp = {0};
            int krc = srt__consume_kmreq(hs, s->opt.passphrase,
                                         &s->ctx->cfg.crypto, &tmp);
            if (krc) return krc;
            if (tmp.encrypted) {
                s->encrypted = true;
                s->cipher    = tmp.cipher;
                s->klen      = tmp.klen;
                memcpy(s->salt, tmp.salt, 16);
                memcpy(s->seks[0], tmp.sek, sizeof tmp.sek);
                s->has_sek[0] = true;
                s->active_sek = 0;
            }
            int rc = srt__emit_rdv_conclusion(s, SRT_CMD_HSRSP, now_us);
            if (rc) return rc;
            s->rdv_sent_conclusion = true;
            return SRT_OK;
        }
    }

    if (hs->hs_type == SRT_HS_AGREEMENT) {
        /* Responder finalizes on receiving AGREEMENT from the initiator. */
        srt__rdv_mark_connected(s, now_us);
        return SRT_OK;
    }

    if (hs->hs_type >= 1000) { s->state = SRT_STATE_BROKEN; return SRT_ERR_PROTO; }
    return SRT_ERR_PROTO;
}

int srt_rendezvous(srt_socket *s, const struct sockaddr *local,
                   const struct sockaddr *remote, socklen_t len) {
    if (!s || !local || !remote) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_INIT) return SRT_ERR_INVAL;
    srt__store_addr(&s->local_addr, &s->local_len, local,  len);
    srt__store_addr(&s->peer_addr,  &s->peer_len,  remote, len);
    s->opt.rendezvous = true;
    /* Unique cookie from context secret + peer addr; §4.4 time-based. */
    s->rdv_own_cookie = srt__cookie_gen(s->ctx, remote, 0);
    s->state = SRT_STATE_CONNECTING;
    return srt__emit_waveahand(s, 0);
}

/* -------------------------- state-machine: listener ----------------------- */

int srt_listen(srt_socket *s, const struct sockaddr *local, socklen_t len) {
    if (!s) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_INIT) return SRT_ERR_INVAL;
    if (local) srt__store_addr(&s->local_addr, &s->local_len, local, len);
    s->state = SRT_STATE_LISTENING;
    return SRT_OK;
}

/* Allocate or look up a pending record for a given peer. */
static srt_pending *srt__pending_find(srt_socket *lis,
                                      const struct sockaddr *peer,
                                      socklen_t peer_len) {
    for (int i = 0; i < SRT_MAX_PENDING; i++) {
        srt_pending *p = &lis->pending[i];
        if (p->used && p->peer_len == peer_len
            && memcmp(&p->peer, peer, peer_len) == 0) return p;
    }
    return NULL;
}
static srt_pending *srt__pending_alloc(srt_socket *lis) {
    for (int i = 0; i < SRT_MAX_PENDING; i++)
        if (!lis->pending[i].used) return &lis->pending[i];
    return NULL;
}

/* Listener: respond to INDUCTION with HSv5 + SRT magic + SYN cookie. */
static int srt__emit_induction_reply(srt_socket *lis,
                                     const srt_hs_cif *in,
                                     const struct sockaddr *peer,
                                     uint64_t now_us) {
    uint32_t cookie = srt__cookie_gen(lis->ctx, peer, now_us);
    /* libsrt convention: in the INDUCTION response the listener echoes the
     * caller's socket_id rather than exposing its own — the real per-
     * connection child id is only revealed in the CONCLUSION reply. */
    uint8_t pkt[64];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        in->srt_socket_id, srt__ts_us(lis, now_us),
        /*hsv*/ 5, lis->opt.encryption, SRT_MAGIC_CODE, SRT_HS_INDUCTION,
        in->isn, lis->opt.mtu, lis->opt.flow_window,
        in->srt_socket_id, cookie,
        peer, NULL, 0);
    if (len < 0) return len;
    return srt__tx_push_to(lis, pkt, (size_t)len, peer, lis->peer_len);
}

/* Listener: respond to a validated CONCLUSION with our own CONCLUSION+HSRSP. */
static int srt__emit_conclusion_reply(srt_socket *lis, srt_pending *p,
                                      uint64_t now_us) {
    srt_hsreq rsp = {0};
    rsp.srt_version = SRT_VERSION_U32;
    /* §4.6: advertise only features we actually implement, then intersect
     * with what the peer requested. CRYPT and REXMITFLG are mandatory. */
    uint32_t mine = SRT_FLAG_CRYPT | SRT_FLAG_REXMITFLG;
    if (lis->opt.tsbpd)        mine |= SRT_FLAG_TSBPDSND | SRT_FLAG_TSBPDRCV;
    if (lis->opt.tlpktdrop)    mine |= SRT_FLAG_TLPKTDROP;
    if (lis->opt.periodic_nak) mine |= SRT_FLAG_PERIODICNAK;
    if (lis->opt.stream_mode)  mine |= SRT_FLAG_STREAM;
    rsp.srt_flags = (mine & p->peer_hsreq.srt_flags)
                  | SRT_FLAG_CRYPT | SRT_FLAG_REXMITFLG;
    rsp.rcv_tsbpd_delay = p->peer_hsreq.rcv_tsbpd_delay
                          ? p->peer_hsreq.rcv_tsbpd_delay : lis->opt.rcv_tsbpd_ms;
    rsp.snd_tsbpd_delay = lis->opt.snd_tsbpd_ms
                          ? lis->opt.snd_tsbpd_ms : p->peer_hsreq.snd_tsbpd_delay;

    uint8_t ext[16];
    size_t ext_len = srt__build_hsreq_tlv(ext, SRT_CMD_HSRSP, &rsp);

    uint8_t pkt[128];
    int len = srt__build_hs_pkt(pkt, sizeof pkt,
        p->peer_socket_id, srt__ts_us(lis, now_us),
        /*hsv*/ 5, lis->opt.encryption, SRT_EXT_HSREQ, SRT_HS_CONCLUSION,
        p->local_isn, lis->opt.mtu, lis->opt.flow_window,
        p->local_socket_id, p->syn_cookie,
        (const struct sockaddr *)&p->peer, ext, ext_len);
    if (len < 0) return len;
    return srt__tx_push_to(lis, pkt, (size_t)len,
                           (struct sockaddr *)&p->peer, p->peer_len);
}

/* -------------------------- feed_udp dispatch ----------------------------- */

static int srt__feed_caller_hs(srt_socket *s, const srt_ctrl_hdr *ch,
                               const srt_hs_cif *hs, uint64_t now_us) {
    (void)ch;
    /* Expect INDUCTION response from listener, then CONCLUSION response. */
    if (hs->hs_type == SRT_HS_INDUCTION && hs->hs_version == 5) {
        /* §4.3 step 2: libsrt's listener echoes the caller's socket_id here
         * rather than exposing its own. The real peer socket_id only shows
         * up in the CONCLUSION reply, so we don't update peer_socket_id yet
         * — CONCLUSION goes out with dst_socket_id=0. */
        s->rcv_syn_cookie = hs->syn_cookie;
        s->isn_peer       = hs->isn;
        return srt__emit_conclusion_caller(s, now_us);
    }
    if (hs->hs_type == SRT_HS_CONCLUSION && hs->hs_version == 5) {
        srt_hsreq rsp;
        int rc = srt__parse_hsreq_from_ext(hs, SRT_CMD_HSRSP, &rsp);
        if (rc) return rc;
        s->peer_hsreq = rsp;
        s->peer_socket_id = hs->srt_socket_id;
        s->isn_peer = hs->isn;
        s->snd_next = s->snd_first_unacked = s->isn_local;
        s->rcv_next = s->rcv_read = s->rcv_ready = s->isn_peer;
        s->rcv_high = srt_seq_add(s->isn_peer, -1); /* nothing received yet */
        s->snd_msgno = 1;
        s->state = SRT_STATE_CONNECTED;
        s->t0_us = now_us;
        s->t_last_ack_us = s->t_last_tx_us = s->t_last_rx_us = now_us;
        return SRT_OK;
    }
    /* Rejection codes ≥ 1000 arrive as hs_type with the error value. */
    if (hs->hs_type >= 1000) {
        s->state = SRT_STATE_BROKEN;
        return SRT_ERR_PROTO;
    }
    return SRT_ERR_PROTO;
}

static int srt__feed_listener_hs(srt_socket *s, const srt_ctrl_hdr *ch,
                                 const srt_hs_cif *hs,
                                 const struct sockaddr *peer, socklen_t plen,
                                 uint64_t now_us) {
    (void)ch;
    /* Park the current peer on the listener so any staged reply packet
     * reports the right destination when drained via srt_context_pull_udp. */
    srt__store_addr(&s->peer_addr, &s->peer_len, peer, plen);
    if (hs->hs_type == SRT_HS_INDUCTION) {
        /* HSv4 probe or HSv5 probe — reply with HSv5 + SRT magic + cookie. */
        return srt__emit_induction_reply(s, hs, peer, now_us);
    }
    if (hs->hs_type == SRT_HS_CONCLUSION && hs->hs_version == 5) {
        /* §4.3 step 3: validate SYN cookie (stateless) before allocating. */
        if (!srt__cookie_verify(s->ctx, peer, hs->syn_cookie, now_us))
            return SRT_ERR_PROTO;

        srt_hsreq req;
        if (srt__parse_hsreq_from_ext(hs, SRT_CMD_HSREQ, &req) != SRT_OK)
            return SRT_ERR_PROTO;

        srt_pending *p = srt__pending_find(s, peer, plen);
        if (!p) {
            p = srt__pending_alloc(s);
            if (!p) return SRT_ERR_NOMEM;
            memset(p, 0, sizeof *p);
            p->used = true;
            srt__store_addr(&p->peer, &p->peer_len, peer, plen);
            p->peer_socket_id = hs->srt_socket_id;
            p->peer_isn       = hs->isn;
            p->syn_cookie     = hs->syn_cookie;
            do { p->local_socket_id = srt__rand32(s->ctx); }
            while (p->local_socket_id == 0);
            /* libsrt enforces that the listener echoes the caller's ISN in
             * the CONCLUSION reply (security check: m_ConnRes.m_iISN must
             * equal m_iISN). Share the caller's ISN for our SND side too. */
            p->local_isn = hs->isn & SRT_SEQNO_MASK;
            s->pending_count++;
        }
        p->peer_hsreq       = req;
        p->conclusion_seen  = true;
        int krc = srt__consume_kmreq(hs, s->opt.passphrase,
                                     &s->ctx->cfg.crypto, p);
        if (krc) return krc;
        /* §4.5 CONFIG TLVs: capture SID/CONGESTION/FILTER/GROUP. */
        srt_ext_iter it; uint16_t cmd, clen; const uint8_t *cp;
        srt_ext_iter_init(&it, hs->ext_data, hs->ext_len);
        while (srt_ext_iter_next(&it, &cmd, &cp, &clen) == SRT_OK) {
            switch (cmd) {
            case SRT_CMD_SID:
                srt__copy_str_payload(p->peer_sid, sizeof p->peer_sid, cp, clen); break;
            case SRT_CMD_CONGESTION:
                srt__copy_str_payload(p->peer_congestion,
                                      sizeof p->peer_congestion, cp, clen); break;
            case SRT_CMD_FILTER:
                srt__copy_str_payload(p->peer_filter,
                                      sizeof p->peer_filter, cp, clen); break;
            case SRT_CMD_GROUP:
                srt__copy_str_payload(p->peer_group,
                                      sizeof p->peer_group, cp, clen); break;
            default: break;
            }
        }
        return srt__emit_conclusion_reply(s, p, now_us);
    }
    return SRT_ERR_PROTO;
}

/* -------------------------- data path ------------------------------------- */

/* Record (ack_number, sent_time) so a matching ACKACK can yield an RTT. */
static void srt__ack_log_put(srt_socket *s, uint32_t ackno, uint64_t now_us) {
    s->ack_log[s->ack_log_head].ack_number = ackno;
    s->ack_log[s->ack_log_head].sent_us    = now_us;
    s->ack_log_head = (s->ack_log_head + 1) % SRT_ACK_LOG_LEN;
}

/* Look up a recorded ACK by number. Returns 0 when unknown. */
static uint64_t srt__ack_log_lookup(const srt_socket *s, uint32_t ackno) {
    for (int i = 0; i < SRT_ACK_LOG_LEN; i++)
        if (s->ack_log[i].ack_number == ackno && s->ack_log[i].sent_us)
            return s->ack_log[i].sent_us;
    return 0;
}

/* §5.1 build + stage a Full ACK. */
static int srt__emit_ack(srt_socket *s, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE + 28];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_ACK;
    ch.subtype       = 0;
    ch.type_info     = ++s->ack_counter;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    /* §12.2 FileCC: advertise genuine receive-buffer headroom so the sender
     * can cap its inflight window. */
    uint32_t held   = (s->rcv_next - s->rcv_read) & SRT_SEQNO_MASK;
    uint32_t avail  = (held < SRT_WINDOW) ? SRT_WINDOW - held : 0;
    srt_ack a = {0};
    a.form           = SRT_ACK_FULL;
    a.ack_number     = ch.type_info;
    a.last_ack_seqno = s->rcv_next;
    a.rtt_us         = s->rtt_us;
    a.rtt_var_us     = s->rtt_var_us;
    a.avail_buf_pkts = avail;
    a.pkt_recv_rate  = 0;
    a.link_capacity  = 0;
    a.recv_rate_bps  = 0;
    size_t alen = 0;
    rc = srt_write_ack(pkt + SRT_HEADER_SIZE, 28, &a, &alen);
    if (rc) return rc;
    rc = srt__tx_push(s, pkt, SRT_HEADER_SIZE + alen);
    if (rc == SRT_OK) srt__ack_log_put(s, a.ack_number, now_us);
    return rc;
}

/* §6 (§5.6 ACKACK in our numbering). */
static int srt__emit_ackack(srt_socket *s, uint32_t ack_number, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_ACKACK;
    ch.type_info     = ack_number;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE);
}

/* §5.2 NAK with loss ranges. */
static int srt__emit_nak(srt_socket *s, const srt_loss_range *r, size_t nr,
                         uint64_t now_us) {
    if (nr == 0) return SRT_OK;
    if (getenv("SRT_TRACE_NAK")) {
        for (size_t i = 0; i < nr; i++)
            fprintf(stderr, "[nak sid=%u start=%u end=%u rcv_next=%u rcv_high=%u]\n",
                    s->local_socket_id, r[i].start, r[i].end, s->rcv_next, s->rcv_high);
    }
    uint8_t pkt[SRT_HEADER_SIZE + 8 * SRT_MAX_NAK_RANGES];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_NAK;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    size_t nlen = 0;
    rc = srt_write_nak(pkt + SRT_HEADER_SIZE, sizeof pkt - SRT_HEADER_SIZE,
                       r, nr, &nlen);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE + nlen);
}

static int srt__emit_shutdown(srt_socket *s, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_SHUTDOWN;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE);
}

static int srt__emit_peererror(srt_socket *s, uint32_t code, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_PEERERROR;
    ch.type_info     = code;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE);
}

int srt_send_peer_error(srt_socket *s, uint32_t code) {
    if (!s) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_CONNECTED) return SRT_ERR_CLOSED;
    return srt__emit_peererror(s, code, s->last_clock_us);
}

uint32_t srt_get_peer_error(const srt_socket *s) {
    return s ? s->peer_error : 0;
}

/* §5.6: emit a DROPREQ carrying msgno (type-specific info) + first/last seq. */
static int srt__emit_dropreq(srt_socket *s, uint32_t msgno,
                             uint32_t first, uint32_t last, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE + 8];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_DROPREQ;
    ch.type_info     = msgno;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    srt_dropreq d = { msgno, first, last };
    rc = srt_write_dropreq(pkt + SRT_HEADER_SIZE, 8, &d);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE + 8);
}

/* Build one data packet into the send slot, encrypt if configured, stage on
 * the tx ring. Advances snd_next but not snd_msgno — the caller handles it. */
static int srt__stage_data(srt_socket *s, const uint8_t *payload, size_t n,
                           uint32_t msgno, srt_pp pp, bool ordered) {
    uint32_t inflight =
        (uint32_t)((s->snd_next - s->snd_first_unacked) & SRT_SEQNO_MASK);
    if (inflight >= SRT_WINDOW) return SRT_ERR_AGAIN;

    srt_data_hdr dh = {0};
    dh.seqno         = s->snd_next;
    dh.pp            = pp;
    dh.ordered       = ordered;
    dh.kk            = !s->encrypted ? SRT_KK_NONE
                     : (s->active_sek == 0 ? SRT_KK_EVEN : SRT_KK_ODD);
    dh.retx          = false;
    dh.msgno         = msgno;
    dh.timestamp     = 0;
    dh.dst_socket_id = s->peer_socket_id;

    int idx = s->snd_next & SRT_WINDOW_MASK;
    srt_pkt_slot *slot = &s->snd[idx];
    slot->seqno = s->snd_next;
    slot->valid = true;
    int rc = srt_write_data_hdr(slot->pkt, sizeof slot->pkt, &dh);
    if (rc) return rc;
    if (SRT_HEADER_SIZE + n + (s->cipher == SRT_CIPHER_AES_GCM ? 16u : 0u)
        > sizeof slot->pkt) return SRT_ERR_INVAL;
    memcpy(slot->pkt + SRT_HEADER_SIZE, payload, n);

    size_t wire = n;
    if (s->encrypted) {
        if (s->cipher == SRT_CIPHER_AES_GCM) {
            uint8_t tag[16];
            rc = srt__seal(s, dh.seqno, slot->pkt,
                           slot->pkt + SRT_HEADER_SIZE, n, tag,
                           s->active_sek);
            if (rc) return rc;
            memcpy(slot->pkt + SRT_HEADER_SIZE + n, tag, 16);
            wire = n + 16;
        } else {
            rc = srt__xcrypt(s, dh.seqno, slot->pkt + SRT_HEADER_SIZE, n,
                             s->active_sek);
            if (rc) return rc;
        }
    }
    slot->len        = SRT_HEADER_SIZE + wire;
    slot->release_us = s->last_clock_us;

    rc = srt__tx_push(s, slot->pkt, slot->len);
    if (rc) { slot->valid = false; return rc; }
    s->snd_next = srt_seq_inc(s->snd_next);
    return SRT_OK;
}

int srt_send(srt_socket *s, const void *buf, size_t n, const srt_msg *meta) {
    if (!s || !buf) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_CONNECTED) return SRT_ERR_CLOSED;
    if (n == 0) return SRT_ERR_INVAL;

    size_t per_pkt = (s->opt.mtu ? s->opt.mtu : SRT_DEFAULT_MTU) - SRT_HEADER_SIZE;
    if (s->encrypted && s->cipher == SRT_CIPHER_AES_GCM) per_pkt -= 16;
    if (per_pkt == 0) return SRT_ERR_INVAL;

    size_t need = (n + per_pkt - 1) / per_pkt;
    /* §9 local window must hold the whole message — fragments can't be
     * interleaved with retransmissions of a prior stream in message mode. */
    uint32_t inflight =
        (uint32_t)((s->snd_next - s->snd_first_unacked) & SRT_SEQNO_MASK);
    if ((size_t)inflight + need > SRT_WINDOW) return SRT_ERR_AGAIN;
    /* §12.2 FileCC: also respect peer's advertised receive-buffer headroom. */
    if (s->peer_flow_window
        && (size_t)inflight + need > s->peer_flow_window)
        return SRT_ERR_AGAIN;

    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = n;
    uint32_t msgno = s->snd_msgno;
    bool ordered = !meta || meta->ordered;
    for (size_t k = 0; k < need; k++) {
        size_t chunk = remaining > per_pkt ? per_pkt : remaining;
        srt_pp pp;
        if (s->opt.stream_mode)   pp = SRT_PP_SOLO;
        else if (need == 1)       pp = SRT_PP_SOLO;
        else if (k == 0)          pp = SRT_PP_FIRST;
        else if (k == need - 1)   pp = SRT_PP_LAST;
        else                      pp = SRT_PP_MIDDLE;
        int rc = srt__stage_data(s, p, chunk, msgno, pp, ordered);
        if (rc) return rc;
        p         += chunk;
        remaining -= chunk;
    }
    s->snd_msgno = (s->snd_msgno + 1) & SRT_MSGNO_MASK;
    return (int)n;
}

int srt_recv(srt_socket *s, void *buf, size_t cap, srt_msg *meta) {
    if (!s || !buf) return SRT_ERR_INVAL;
    /* Skip slots invalidated by a DROPREQ so the consumer doesn't stall. */
    while (srt_seq_cmp(s->rcv_read, s->rcv_ready) < 0) {
        int i = s->rcv_read & SRT_WINDOW_MASK;
        if (s->rcv[i].valid && s->rcv[i].seqno == s->rcv_read) break;
        s->rcv_read = srt_seq_inc(s->rcv_read);
        s->rcv_offset = 0;
    }

    /* §7.2 buffer (stream) mode: copy as much contiguous payload as fits
     * in `cap`, spanning slots and supporting partial consumption. Ignores
     * msgno / PP flags. */
    if (s->opt.stream_mode) {
        uint8_t *dst = (uint8_t *)buf;
        size_t   w   = 0;
        while (w < cap && srt_seq_cmp(s->rcv_read, s->rcv_ready) < 0) {
            int i = s->rcv_read & SRT_WINDOW_MASK;
            srt_pkt_slot *sl = &s->rcv[i];
            if (!sl->valid || sl->seqno != s->rcv_read) break;
            size_t avail = (sl->len - SRT_HEADER_SIZE) - s->rcv_offset;
            size_t copy  = (cap - w < avail) ? (cap - w) : avail;
            memcpy(dst + w, sl->pkt + SRT_HEADER_SIZE + s->rcv_offset, copy);
            w             += copy;
            s->rcv_offset += (uint16_t)copy;
            if (s->rcv_offset == sl->len - SRT_HEADER_SIZE) {
                sl->valid = false;
                s->rcv_read = srt_seq_inc(s->rcv_read);
                s->rcv_offset = 0;
            }
        }
        if (w == 0) return SRT_ERR_AGAIN;
        if (meta) { meta->msgno = 0; meta->ordered = true; meta->last = true; }
        return (int)w;
    }

    if (srt_seq_cmp(s->rcv_read, s->rcv_ready) >= 0) return SRT_ERR_AGAIN;
    int idx = s->rcv_read & SRT_WINDOW_MASK;
    srt_pkt_slot *head = &s->rcv[idx];
    if (!head->valid || head->seqno != s->rcv_read) return SRT_ERR_AGAIN;

    srt_data_hdr hd;
    if (srt_parse_data_hdr(head->pkt, head->len, &hd) != SRT_OK)
        return SRT_ERR_PROTO;

    /* Scan forward until PP_LAST (or PP_SOLO on the head slot itself).
     * If any fragment is missing within rcv_ready, the message is still
     * incomplete — caller retries later. */
    uint32_t end_seq = s->rcv_read;
    size_t   total   = head->len - SRT_HEADER_SIZE;
    srt_pp   head_pp = hd.pp;

    if (head_pp == SRT_PP_FIRST || head_pp == SRT_PP_MIDDLE) {
        bool found_last = false;
        uint32_t seq = srt_seq_inc(s->rcv_read);
        while (srt_seq_cmp(seq, s->rcv_ready) < 0) {
            int i = seq & SRT_WINDOW_MASK;
            srt_pkt_slot *sl = &s->rcv[i];
            if (!sl->valid || sl->seqno != seq) return SRT_ERR_AGAIN;
            srt_data_hdr dh;
            if (srt_parse_data_hdr(sl->pkt, sl->len, &dh) != SRT_OK)
                return SRT_ERR_PROTO;
            total += sl->len - SRT_HEADER_SIZE;
            if (dh.pp == SRT_PP_LAST) { end_seq = seq; found_last = true; break; }
            seq = srt_seq_inc(seq);
        }
        if (!found_last) return SRT_ERR_AGAIN;
    } else if (head_pp != SRT_PP_SOLO) {
        return SRT_ERR_PROTO;  /* orphan LAST with no FIRST before it */
    }

    if (cap < total) return SRT_ERR_TOOSHORT;

    /* Concat and retire. */
    uint8_t *dst = (uint8_t *)buf;
    size_t   w   = 0;
    uint32_t cur = s->rcv_read;
    while (1) {
        int i = cur & SRT_WINDOW_MASK;
        srt_pkt_slot *sl = &s->rcv[i];
        size_t plen = sl->len - SRT_HEADER_SIZE;
        memcpy(dst + w, sl->pkt + SRT_HEADER_SIZE, plen);
        w += plen;
        sl->valid = false;
        if (cur == end_seq) break;
        cur = srt_seq_inc(cur);
    }
    if (meta) {
        meta->msgno   = hd.msgno;
        meta->ordered = hd.ordered;
        meta->last    = true;
    }
    s->rcv_read = srt_seq_inc(end_seq);
    return (int)w;
}

/* Receive-side DATA handler. §2 + §9. */
static int srt__handle_data(srt_socket *s, const uint8_t *buf, size_t n,
                            uint64_t now_us) {
    srt_data_hdr dh;
    if (srt_parse_data_hdr(buf, n, &dh) != SRT_OK) return SRT_ERR_PROTO;
    if (dh.dst_socket_id != s->local_socket_id) return SRT_ERR_PROTO;

    int32_t delta = srt_seq_cmp(dh.seqno, s->rcv_next);
    if (delta < 0) {
        /* Duplicate or already delivered. Still ACK so the sender unsticks. */
        return srt__emit_ack(s, now_us);
    }
    if (delta >= (int32_t)SRT_WINDOW) return SRT_ERR_PROTO;

    int idx = dh.seqno & SRT_WINDOW_MASK;
    srt_pkt_slot *slot = &s->rcv[idx];
    if (!(slot->valid && slot->seqno == dh.seqno)) {
        slot->seqno = dh.seqno;
        slot->len   = n;
        slot->valid = true;
        memcpy(slot->pkt, buf, n);
        if (dh.kk != SRT_KK_NONE && s->encrypted) {
            int parity = (dh.kk == SRT_KK_ODD) ? 1 : 0;
            if (s->cipher == SRT_CIPHER_AES_GCM) {
                if (slot->len < SRT_HEADER_SIZE + 16) {
                    slot->valid = false; return SRT_ERR_PROTO;
                }
                size_t ct_len = slot->len - SRT_HEADER_SIZE - 16;
                uint8_t tag[16];
                memcpy(tag, slot->pkt + SRT_HEADER_SIZE + ct_len, 16);
                int rc = srt__open(s, dh.seqno, slot->pkt,
                                   slot->pkt + SRT_HEADER_SIZE, ct_len, tag,
                                   parity);
                if (rc) { slot->valid = false; return SRT_ERR_CRYPTO; }
                slot->len -= 16;
            } else {
                int rc = srt__xcrypt(s, dh.seqno,
                                     slot->pkt + SRT_HEADER_SIZE,
                                     slot->len - SRT_HEADER_SIZE, parity);
                if (rc) { slot->valid = false; return rc; }
            }
        }
        /* §8 TSBPD: hold the packet until now + rcv_tsbpd_ms. Without TSBPD
         * the watermark releases as soon as a slot becomes in-order. */
        if (s->opt.tsbpd && s->opt.rcv_tsbpd_ms)
            slot->release_us = now_us + (uint64_t)s->opt.rcv_tsbpd_ms * 1000u;
        else
            slot->release_us = 0;
    }

    if (srt_seq_cmp(dh.seqno, s->rcv_high) > 0) s->rcv_high = dh.seqno;

    /* Gap detected → emit NAK for the missing range. */
    if (delta > 0) {
        srt_loss_range r = {
            s->rcv_next,
            srt_seq_add(dh.seqno, -1)
        };
        srt__emit_nak(s, &r, 1, now_us);
    }

    /* Advance rcv_next over contiguous valid packets. */
    while (1) {
        int i2 = s->rcv_next & SRT_WINDOW_MASK;
        srt_pkt_slot *sl = &s->rcv[i2];
        if (!sl->valid || sl->seqno != s->rcv_next) break;
        s->rcv_next = srt_seq_inc(s->rcv_next);
    }
    /* Release anything whose TSBPD deadline has already passed. */
    srt__advance_ready(s, now_us);

    int rc = srt__emit_ack(s, now_us);
    if (rc == SRT_OK) s->t_last_ack_us = now_us;
    return rc;
}

/* §5.1 incoming ACK. Slides the send window, triggers ACKACK for full ACKs. */
static int srt__handle_ack(srt_socket *s, const srt_ctrl_hdr *cc,
                           const uint8_t *cif, size_t clen, uint64_t now_us) {
    srt_ack a;
    if (srt_parse_ack(cif, clen, cc->type_info, &a) != SRT_OK)
        return SRT_ERR_PROTO;
    /* Advance snd_first_unacked up to (but excluding) last_ack_seqno. */
    while (srt_seq_cmp(s->snd_first_unacked, a.last_ack_seqno) < 0) {
        int idx = s->snd_first_unacked & SRT_WINDOW_MASK;
        s->snd[idx].valid = false;
        s->snd_first_unacked = srt_seq_inc(s->snd_first_unacked);
    }
    /* Prefer our own ACKACK-derived sample; fall back to peer's estimate. */
    if (!s->rtt_have_sample && a.rtt_us) {
        s->rtt_us     = a.rtt_us;
        s->rtt_var_us = a.rtt_var_us;
    }
    /* FileCC: track peer's free buffer budget (0 = don't know, keep prev). */
    if (a.avail_buf_pkts) s->peer_flow_window = a.avail_buf_pkts;
    s->exp_count = 0;  /* peer is alive — reset the EXP backoff */
    if (a.form == SRT_ACK_FULL)
        return srt__emit_ackack(s, a.ack_number, now_us);
    return SRT_OK;
}

/* §5.2 incoming NAK: retransmit each listed packet still in our window. */
static int srt__handle_nak(const uint8_t *cif, size_t clen, srt_socket *s) {
    srt_loss_range r[SRT_MAX_NAK_RANGES];
    size_t nr = 0;
    if (srt_parse_nak(cif, clen, r, SRT_MAX_NAK_RANGES, &nr) != SRT_OK)
        return SRT_ERR_PROTO;
    for (size_t i = 0; i < nr; i++) {
        uint32_t cur = r[i].start;
        while (1) {
            int idx = cur & SRT_WINDOW_MASK;
            srt_pkt_slot *sl = &s->snd[idx];
            if (sl->valid && sl->seqno == cur) {
                /* Set R (retransmitted) flag: byte 4, bit 2. §2. */
                sl->pkt[4] |= 0x04;
                int rc = srt__tx_push(s, sl->pkt, sl->len);
                if (rc == SRT_ERR_AGAIN) return SRT_OK; /* backpressure; retry later */
                if (rc) return rc;
            }
            if (cur == r[i].end) break;
            cur = srt_seq_inc(cur);
        }
    }
    return SRT_OK;
}

/* Walk the recv ring between rcv_next and rcv_high, gathering gap ranges. */
static size_t srt__compute_loss(const srt_socket *s,
                                srt_loss_range *out, size_t cap) {
    /* Nothing past rcv_next has arrived — no gap to report. */
    if (srt_seq_cmp(s->rcv_high, s->rcv_next) < 0 || cap == 0) return 0;
    size_t n = 0;
    uint32_t cur = s->rcv_next;
    while (srt_seq_cmp(cur, s->rcv_high) <= 0 && n < cap) {
        int idx = cur & SRT_WINDOW_MASK;
        if (s->rcv[idx].valid && s->rcv[idx].seqno == cur) {
            cur = srt_seq_inc(cur);
            continue;
        }
        uint32_t start = cur;
        while (srt_seq_cmp(cur, s->rcv_high) <= 0) {
            int i2 = cur & SRT_WINDOW_MASK;
            if (s->rcv[i2].valid && s->rcv[i2].seqno == cur) break;
            cur = srt_seq_inc(cur);
        }
        out[n].start = start;
        out[n].end   = srt_seq_add(cur, -1);
        n++;
    }
    return n;
}

/* Advance the TSBPD release watermark based on current time. */
static void srt__advance_ready(srt_socket *s, uint64_t now_us) {
    while (srt_seq_cmp(s->rcv_ready, s->rcv_next) < 0) {
        int idx = s->rcv_ready & SRT_WINDOW_MASK;
        const srt_pkt_slot *sl = &s->rcv[idx];
        if (!sl->valid || sl->seqno != s->rcv_ready) break;
        if (sl->release_us != 0 && sl->release_us > now_us) break;
        s->rcv_ready = srt_seq_inc(s->rcv_ready);
    }
}

/* §5.3 stage a KEEPALIVE. */
static int srt__emit_keepalive(srt_socket *s, uint64_t now_us) {
    uint8_t pkt[SRT_HEADER_SIZE];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_KEEPALIVE;
    ch.timestamp     = srt__ts_us(s, now_us);
    ch.dst_socket_id = s->peer_socket_id;
    int rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    return srt__tx_push(s, pkt, SRT_HEADER_SIZE);
}

int srt_rekey(srt_socket *s) {
    if (!s) return SRT_ERR_INVAL;
    if (!s->encrypted || s->klen == 0) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_CONNECTED) return SRT_ERR_CLOSED;

    int new_parity = 1 - s->active_sek;
    int rc = s->ctx->cfg.crypto.random(s->ctx->cfg.crypto.ctx,
                                       s->seks[new_parity], s->klen);
    if (rc) return rc;
    s->has_sek[new_parity] = true;

    uint8_t kek[32];
    rc = srt__derive_kek(s->opt.passphrase, s->salt, 16, kek, s->klen);
    if (rc) return rc;
    uint8_t wrapped[8 + 32];
    rc = s->ctx->cfg.crypto.kw_wrap(s->ctx->cfg.crypto.ctx,
                                    kek, s->klen, s->seks[new_parity],
                                    s->klen, wrapped);
    if (rc) return rc;

    srt_km km = {0};
    km.version  = 1;
    km.pt       = 2;
    km.sign     = SRT_KM_SIGN;
    km.kk       = (new_parity == 0) ? SRT_KK_EVEN : SRT_KK_ODD;
    km.keki     = 0;
    km.cipher   = s->cipher;
    km.auth     = (s->cipher == SRT_CIPHER_AES_GCM) ? 1 : 0;
    km.se       = 0;
    km.slen     = 16;
    km.klen     = s->klen;
    km.salt     = s->salt;
    km.wrap     = wrapped;
    km.wrap_len = 8u + s->klen;

    uint8_t km_buf[128];
    size_t  km_len = 0;
    rc = srt_write_km(km_buf, sizeof km_buf, &km, &km_len);
    if (rc) return rc;

    /* Wrap in a USER-DEFINED control packet (type 0x7FFF, subtype = cmd). */
    uint8_t pkt[SRT_HEADER_SIZE + 128];
    srt_ctrl_hdr ch = {0};
    ch.ctrl_type     = SRT_CTRL_USERDEFINED;
    ch.subtype       = SRT_CMD_KMREQ;
    ch.timestamp     = srt__ts_us(s, s->last_clock_us);
    ch.dst_socket_id = s->peer_socket_id;
    rc = srt_write_ctrl_hdr(pkt, sizeof pkt, &ch);
    if (rc) return rc;
    memcpy(pkt + SRT_HEADER_SIZE, km_buf, km_len);
    rc = srt__tx_push(s, pkt, SRT_HEADER_SIZE + km_len);
    if (rc) return rc;

    /* Switch outbound encryption to the new key immediately. The receiver
     * still has the old key for in-flight packets. */
    s->active_sek = (uint8_t)new_parity;
    return SRT_OK;
}

int srt_tick(srt_socket *s, uint64_t now_us) {
    if (!s) return SRT_ERR_INVAL;
    s->last_clock_us = now_us;
    if (s->state != SRT_STATE_CONNECTED) return SRT_OK;

    /* TSBPD release cursor. */
    srt__advance_ready(s, now_us);

    /* §9.3 TLPKTDROP: once a packet has been unacked longer than
     * snd_tsbpd + 2·RTT, give up and tell the peer to skip it. Only applies
     * in message mode where whole messages can be dropped atomically. */
    if (s->opt.tlpktdrop && s->snd_first_unacked != s->snd_next) {
        uint64_t thr = (uint64_t)s->opt.snd_tsbpd_ms * 1000u
                     + 2u * (uint64_t)(s->rtt_us ? s->rtt_us : 10000u);
        if (thr < 50000u) thr = 50000u;

        uint32_t first = 0, last = 0, msgno = 0;
        bool     in_drop = false;
        while (s->snd_first_unacked != s->snd_next) {
            int idx = s->snd_first_unacked & SRT_WINDOW_MASK;
            srt_pkt_slot *sl = &s->snd[idx];
            if (sl->valid && sl->seqno == s->snd_first_unacked) {
                if (now_us - sl->release_us < thr) break;
                srt_data_hdr dh;
                srt_parse_data_hdr(sl->pkt, sl->len, &dh);
                if (!in_drop) {
                    first = sl->seqno; msgno = dh.msgno; in_drop = true;
                }
                last = sl->seqno;
                sl->valid = false;
            }
            s->snd_first_unacked = srt_seq_inc(s->snd_first_unacked);
        }
        if (in_drop) srt__emit_dropreq(s, msgno, first, last, now_us);
    }

    /* §9.4 periodic Full ACK (10 ms). Emit only once the window has opened. */
    if (srt_seq_cmp(s->rcv_next, s->rcv_read) > 0 &&
        now_us - s->t_last_ack_us >= SRT_ACK_PERIOD_US) {
        int rc = srt__emit_ack(s, now_us);
        if (rc == SRT_OK) s->t_last_ack_us = now_us;
    }

    /* §9.3 periodic NAK (20 ms): re-report outstanding losses when
     * SRT_FLAG_PERIODICNAK is set, so a dropped NAK doesn't stall recovery. */
    if (s->opt.periodic_nak &&
        now_us - s->t_last_nak_us >= SRT_NAK_PERIOD_US) {
        srt_loss_range r[SRT_MAX_NAK_RANGES];
        size_t nr = srt__compute_loss(s, r, SRT_MAX_NAK_RANGES);
        if (nr) srt__emit_nak(s, r, nr, now_us);
        s->t_last_nak_us = now_us;
    }

    /* KEEPALIVE when nothing else went out recently. */
    if (now_us - s->t_last_tx_us >= SRT_KEEPALIVE_PERIOD_US) {
        int rc = srt__emit_keepalive(s, now_us);
        if (rc == SRT_OK) s->t_last_tx_us = now_us;
    }

    /* §9.3 EXP retransmit fallback. Backoff off the EXP count so a dead peer
     * doesn't trigger a retransmit storm. Threshold = max(4·RTT, 50 ms). */
    if (s->snd_first_unacked != s->snd_next) {
        uint64_t thr = 4u * (uint64_t)(s->rtt_us ? s->rtt_us : 10000u);
        if (thr < 50000u) thr = 50000u;
        thr <<= (s->exp_count > 4 ? 4 : s->exp_count); /* cap shift */
        if (now_us - s->t_last_rx_us >= thr) {
            /* Retransmit the head of the unacked window. */
            int idx = s->snd_first_unacked & SRT_WINDOW_MASK;
            srt_pkt_slot *sl = &s->snd[idx];
            if (sl->valid && sl->seqno == s->snd_first_unacked) {
                sl->pkt[4] |= 0x04;  /* set R bit */
                srt__tx_push(s, sl->pkt, sl->len);
            }
            s->t_last_rx_us = now_us;  /* reset so we back off */
            if (s->exp_count < 16) s->exp_count++;
            else { s->state = SRT_STATE_BROKEN; return SRT_ERR_TIMEOUT; }
        }
    }
    return SRT_OK;
}

uint64_t srt_next_deadline_us(const srt_socket *s, uint64_t now_us) {
    if (!s || s->state != SRT_STATE_CONNECTED) return now_us + SRT_KEEPALIVE_PERIOD_US;
    uint64_t d = s->t_last_tx_us + SRT_KEEPALIVE_PERIOD_US;
    /* LiveCC release deadline for the head tx slot. */
    if (s->opt.max_bw_bps) {
        uint64_t rel = srt__next_release_us(s);
        if (rel && rel < d) d = rel;
    }
    if (srt_seq_cmp(s->rcv_next, s->rcv_read) > 0) {
        uint64_t ack_due = s->t_last_ack_us + SRT_ACK_PERIOD_US;
        if (ack_due < d) d = ack_due;
    }
    /* Next TSBPD release, if any. */
    if (srt_seq_cmp(s->rcv_ready, s->rcv_next) < 0) {
        int idx = s->rcv_ready & SRT_WINDOW_MASK;
        const srt_pkt_slot *sl = &s->rcv[idx];
        if (sl->valid && sl->release_us && sl->release_us < d)
            d = sl->release_us;
    }
    if (s->snd_first_unacked != s->snd_next) {
        uint64_t thr = 4u * (uint64_t)(s->rtt_us ? s->rtt_us : 10000u);
        if (thr < 50000u) thr = 50000u;
        uint64_t exp_due = s->t_last_rx_us + thr;
        if (exp_due < d) d = exp_due;
    }
    return d;
}

int srt_feed_udp(srt_socket *s, const void *pkt, size_t n, uint64_t now_us) {
    if (!s || !pkt || n < SRT_HEADER_SIZE) return SRT_ERR_INVAL;
    const uint8_t *buf = (const uint8_t *)pkt;

    s->last_clock_us = now_us;
    s->t_last_rx_us = now_us;

    srt_common_hdr ch;
    int rc = srt_parse_common_hdr(buf, n, &ch);
    if (rc) return rc;

    if (!ch.is_control) {
        if (s->state != SRT_STATE_CONNECTED) return SRT_ERR_PROTO;
        return srt__handle_data(s, buf, n, now_us);
    }

    srt_ctrl_hdr cc;
    if (srt_parse_ctrl_hdr(buf, n, &cc) != SRT_OK) return SRT_ERR_PROTO;

    switch (cc.ctrl_type) {
    case SRT_CTRL_HANDSHAKE: {
        srt_hs_cif hs;
        if (srt_parse_hs_cif(buf + SRT_HEADER_SIZE, n - SRT_HEADER_SIZE, &hs)
            != SRT_OK) return SRT_ERR_PROTO;
        if (s->state == SRT_STATE_CONNECTING)
            return s->opt.rendezvous
                ? srt__feed_rendezvous_hs(s, &cc, &hs, now_us)
                : srt__feed_caller_hs    (s, &cc, &hs, now_us);
        if (s->state == SRT_STATE_LISTENING)
            return srt__feed_listener_hs(s, &cc, &hs,
                (const struct sockaddr *)&s->peer_addr, s->peer_len, now_us);
        return SRT_ERR_PROTO;
    }
    case SRT_CTRL_ACK:
        return srt__handle_ack(s, &cc, buf + SRT_HEADER_SIZE,
                               n - SRT_HEADER_SIZE, now_us);
    case SRT_CTRL_NAK:
        return srt__handle_nak(buf + SRT_HEADER_SIZE, n - SRT_HEADER_SIZE, s);
    case SRT_CTRL_ACKACK: {
        /* §5.5: the ACKACK's type-specific info echoes the ACK number. Look
         * it up, EWMA the RTT sample (Stevens constants: 1/8, 1/4). */
        uint64_t sent = srt__ack_log_lookup(s, cc.type_info);
        if (sent && now_us > sent) {
            uint32_t sample = (uint32_t)(now_us - sent);
            if (!s->rtt_have_sample) {
                s->rtt_us     = sample;
                s->rtt_var_us = sample / 2u;
                s->rtt_have_sample = true;
            } else {
                uint32_t diff = (sample > s->rtt_us)
                              ? sample - s->rtt_us : s->rtt_us - sample;
                s->rtt_var_us = (s->rtt_var_us * 3u + diff) / 4u;
                s->rtt_us     = (s->rtt_us     * 7u + sample) / 8u;
            }
        }
        return SRT_OK;
    }
    case SRT_CTRL_KEEPALIVE:
        return SRT_OK; /* §5.3: one-way, no reply required */
    case SRT_CTRL_SHUTDOWN:
        s->state = SRT_STATE_CLOSED;
        return SRT_OK;
    case SRT_CTRL_DROPREQ: {
        srt_dropreq d = {0};
        if (srt_parse_dropreq(buf + SRT_HEADER_SIZE,
                              n - SRT_HEADER_SIZE, &d) != SRT_OK)
            return SRT_ERR_PROTO;
        d.msgno = cc.type_info;
        /* §9.3 advance rcv_next past the dropped range, then extend through
         * any already-arrived packets that were blocked by the gap. */
        if (srt_seq_cmp(d.last_seqno, s->rcv_next) >= 0) {
            uint32_t new_rcv = srt_seq_inc(d.last_seqno);
            while (srt_seq_cmp(s->rcv_next, new_rcv) < 0) {
                int i = s->rcv_next & SRT_WINDOW_MASK;
                if (!(s->rcv[i].valid && s->rcv[i].seqno == s->rcv_next))
                    s->rcv[i].valid = false;
                s->rcv_next = srt_seq_inc(s->rcv_next);
            }
            while (1) {
                int i = s->rcv_next & SRT_WINDOW_MASK;
                if (!s->rcv[i].valid || s->rcv[i].seqno != s->rcv_next) break;
                s->rcv_next = srt_seq_inc(s->rcv_next);
            }
            /* Nothing to hold for TSBPD past a sender-ordered drop. */
            if (srt_seq_cmp(s->rcv_ready, s->rcv_next) < 0)
                s->rcv_ready = s->rcv_next;
        }
        return SRT_OK;
    }
    case SRT_CTRL_USERDEFINED: {
        /* §11.4 KM refresh: subtype carries the SRT_CMD_* code. */
        if (cc.subtype == SRT_CMD_KMREQ && s->encrypted) {
            srt_km km;
            if (srt_parse_km(buf + SRT_HEADER_SIZE,
                             n - SRT_HEADER_SIZE, &km) != SRT_OK)
                return SRT_ERR_PROTO;
            if (km.klen != s->klen || km.slen != 16) return SRT_ERR_PROTO;
            if (km.kk != SRT_KK_EVEN && km.kk != SRT_KK_ODD)
                return SRT_ERR_PROTO;
            if (km.wrap_len != 8u + km.klen) return SRT_ERR_PROTO;
            uint8_t kek[32];
            int rc = srt__derive_kek(s->opt.passphrase, km.salt, km.slen,
                                     kek, km.klen);
            if (rc) return rc;
            int parity = (km.kk == SRT_KK_ODD) ? 1 : 0;
            rc = s->ctx->cfg.crypto.kw_unwrap(s->ctx->cfg.crypto.ctx,
                                              kek, km.klen,
                                              km.wrap, km.wrap_len,
                                              s->seks[parity]);
            if (rc) return SRT_ERR_CRYPTO;
            s->has_sek[parity] = true;
        }
        return SRT_OK;
    }
    case SRT_CTRL_PEERERROR:
        /* §5.7 receiver-side fatal error. Remember the code and break the
         * connection so subsequent srt_send/srt_recv fail fast. */
        s->peer_error = cc.type_info;
        s->state      = SRT_STATE_BROKEN;
        return SRT_OK;
    case SRT_CTRL_CONGESTION:
        /* TODO: congestion window adjust from receiver signal. */
        return SRT_OK;
    default:
        return SRT_ERR_PROTO;
    }
}

/* Variant for listener sockets that lets the caller supply the peer address
 * alongside the datagram (UDP recvfrom). Exposed as a separate helper below. */
int srt_feed_udp_from(srt_socket *s, const void *pkt, size_t n,
                      const struct sockaddr *peer, socklen_t plen,
                      uint64_t now_us);

int srt_feed_udp_from(srt_socket *s, const void *pkt, size_t n,
                      const struct sockaddr *peer, socklen_t plen,
                      uint64_t now_us) {
    if (!s) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_LISTENING)
        return srt_feed_udp(s, pkt, n, now_us);
    /* Listener path: parse HS and respond using the supplied peer address. */
    if (n < SRT_HEADER_SIZE) return SRT_ERR_INVAL;
    srt_ctrl_hdr cc;
    if (srt_parse_ctrl_hdr((const uint8_t *)pkt, n, &cc) != SRT_OK)
        return SRT_ERR_PROTO;
    if (cc.ctrl_type != SRT_CTRL_HANDSHAKE) return SRT_ERR_UNSUPPORTED;
    srt_hs_cif hs;
    if (srt_parse_hs_cif((const uint8_t *)pkt + SRT_HEADER_SIZE,
                         n - SRT_HEADER_SIZE, &hs) != SRT_OK)
        return SRT_ERR_PROTO;
    return srt__feed_listener_hs(s, &cc, &hs, peer, plen, now_us);
}

/* -------------------------- accept ---------------------------------------- */

int srt_accept(srt_socket *s, srt_socket **out) {
    if (!s || !out) return SRT_ERR_INVAL;
    if (s->state != SRT_STATE_LISTENING) return SRT_ERR_INVAL;
    for (int i = 0; i < SRT_MAX_PENDING; i++) {
        int idx = (s->pending_head + i) % SRT_MAX_PENDING;
        srt_pending *p = &s->pending[idx];
        if (!p->used || !p->conclusion_seen) continue;

        srt_socket *child = (srt_socket *)srt__alloc(s->ctx, sizeof *child);
        if (!child) return SRT_ERR_NOMEM;
        memset(child, 0, sizeof *child);
        child->ctx             = s->ctx;
        child->opt             = s->opt;
        child->state           = SRT_STATE_CONNECTED;
        child->local_socket_id = p->local_socket_id;
        child->peer_socket_id  = p->peer_socket_id;
        child->isn_local       = p->local_isn;
        child->isn_peer        = p->peer_isn;
        child->peer_hsreq      = p->peer_hsreq;
        child->snd_next        = child->snd_first_unacked = child->isn_local;
        child->rcv_next        = child->rcv_read = child->rcv_ready = child->isn_peer;
        child->rcv_high        = srt_seq_add(child->isn_peer, -1);
        child->snd_msgno       = 1;
        child->t_last_ack_us   = child->t_last_tx_us
                               = child->t_last_rx_us = s->last_clock_us;
        child->encrypted       = p->encrypted;
        child->cipher          = p->cipher;
        child->klen            = p->klen;
        memcpy(child->salt, p->salt, 16);
        if (p->encrypted) {
            memcpy(child->seks[0], p->sek, sizeof p->sek);
            child->has_sek[0] = true;
            child->active_sek = 0;
        }
        memcpy(child->peer_sid,        p->peer_sid,        sizeof p->peer_sid);
        memcpy(child->peer_congestion, p->peer_congestion, sizeof p->peer_congestion);
        memcpy(child->peer_filter,     p->peer_filter,     sizeof p->peer_filter);
        memcpy(child->peer_group,      p->peer_group,      sizeof p->peer_group);
        memcpy(&child->peer_addr, &p->peer, p->peer_len);
        child->peer_len        = p->peer_len;
        child->t0_us           = 0;  /* caller can set via clock later */

        p->used = false;
        p->conclusion_seen = false;
        s->pending_count--;
        s->pending_head = (idx + 1) % SRT_MAX_PENDING;
        if (srt__register(s->ctx, child) != SRT_OK) {
            srt__free(s->ctx, child);
            return SRT_ERR_NOMEM;
        }
        *out = child;
        return SRT_OK;
    }
    return SRT_ERR_AGAIN;
}

/* -------------------------- remaining stubs ------------------------------- */


/* -------------------------- context-level UDP mux ------------------------- */

int srt_context_feed_udp(srt_context *ctx, const void *pkt, size_t n,
                         const struct sockaddr *peer, socklen_t plen,
                         uint64_t now_us) {
    if (!ctx || !pkt || n < SRT_HEADER_SIZE) return SRT_ERR_INVAL;
    const uint8_t *b = (const uint8_t *)pkt;
    uint32_t dst_id = srt__rd32(b + 12);

    if (dst_id != 0) {
        for (int i = 0; i < ctx->sockets_cap; i++) {
            srt_socket *s = ctx->sockets[i];
            if (s && s->local_socket_id == dst_id)
                return srt_feed_udp_from(s, pkt, n, peer, plen, now_us);
        }
        return SRT_ERR_PROTO;
    }

    /* dst_id == 0: HS INDUCTION or WAVEAHAND. Prefer a rendezvous socket
     * whose peer address matches, then any LISTENING socket. */
    for (int i = 0; i < ctx->sockets_cap; i++) {
        srt_socket *s = ctx->sockets[i];
        if (s && s->opt.rendezvous
            && s->state == SRT_STATE_CONNECTING
            && s->peer_len == plen
            && memcmp(&s->peer_addr, peer, (size_t)plen) == 0)
            return srt_feed_udp_from(s, pkt, n, peer, plen, now_us);
    }
    for (int i = 0; i < ctx->sockets_cap; i++) {
        srt_socket *s = ctx->sockets[i];
        if (s && s->state == SRT_STATE_LISTENING)
            return srt_feed_udp_from(s, pkt, n, peer, plen, now_us);
    }
    return SRT_ERR_PROTO;
}

int srt_context_pull_udp(srt_context *ctx, void *out, size_t cap,
                         struct sockaddr *peer, socklen_t *plen,
                         uint64_t now_us) {
    if (!ctx || !out) return SRT_ERR_INVAL;
    int cap_n = ctx->sockets_cap;
    for (int k = 0; k < cap_n; k++) {
        int i = (ctx->pull_cursor + k) % cap_n;
        srt_socket *s = ctx->sockets[i];
        if (!s || s->txq_count == 0) continue;
        if (s->opt.max_bw_bps && now_us < srt__next_release_us(s)) continue;
        srt_tx_slot *slot = &s->txq[s->txq_head];
        if (peer && plen) {
            socklen_t want = slot->dest_len ? slot->dest_len : s->peer_len;
            if (*plen < want) return SRT_ERR_TOOSHORT;
            const void *src = slot->dest_len ? (const void *)&slot->dest
                                             : (const void *)&s->peer_addr;
            memcpy(peer, src, (size_t)want);
            *plen = want;
        }
        int rc = srt_pull_udp(s, out, cap, now_us);
        ctx->pull_cursor = (i + 1) % cap_n;
        return rc;
    }
    return SRT_ERR_AGAIN;
}

uint32_t srt_get_local_socket_id(const srt_socket *s) {
    return s ? s->local_socket_id : 0;
}
uint32_t srt_get_peer_socket_id(const srt_socket *s) {
    return s ? s->peer_socket_id : 0;
}

const char *srt_get_peer_stream_id (const srt_socket *s) {
    return (s && s->peer_sid[0])        ? s->peer_sid        : NULL;
}
const char *srt_get_peer_congestion(const srt_socket *s) {
    return (s && s->peer_congestion[0]) ? s->peer_congestion : NULL;
}
const char *srt_get_peer_filter    (const srt_socket *s) {
    return (s && s->peer_filter[0])     ? s->peer_filter     : NULL;
}
const char *srt_get_peer_group     (const srt_socket *s) {
    return (s && s->peer_group[0])      ? s->peer_group      : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* SRT_IMPLEMENTATION */
#endif /* SRT_H_INCLUDED */
