/**
 * ble_iface_priv.h — the peer table and the pieces the three source files
 * share. Everything here is owned by the `ble_if` task unless a comment says
 * otherwise; the two exceptions are the host-context callbacks, which only
 * enqueue, and the GATT handles NimBLE fills in at registration.
 */
#pragma once

#include "ble_iface.h"

#include "ble.h"
#include "mem.h"
#include "ports.h"
#include "rnsd.h"
#include "spangap.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ─────────────── protocol constants (ble-reticulum v2.2) ───────────────
 *
 * Service 37145b00-442d-4a94-917f-8f42c5da28e3
 *   RX     …28e5  write / write-without-response, central → peripheral
 *   TX     …28e4  read + notify,                  peripheral → central
 *   Ident  …28e6  read, exactly 16 bytes
 */
#define BLEIF_IDENT_LEN     16      /* an RNS identity hash, truncated */
#define BLEIF_FRAG_HDR      5       /* !BHH: type | seq16 | total16 */
#define BLEIF_FRAG_START    0x01
#define BLEIF_FRAG_CONTINUE 0x02
#define BLEIF_FRAG_END      0x03
#define BLEIF_REASM_MAX     512     /* the largest inbound fragment we accept */
#define BLEIF_REASM_MS      30000   /* DEFAULT_TIMEOUT */
#define BLEIF_KEEPALIVE_MS  15000   /* one 0x00 byte per idle interval */
#define BLEIF_RNS_MTU       500     /* rnsd's _FIXED_MTU; the ATT MTU never
                                     * leaks upward */

#define BLEIF_MAX_PEERS     7       /* the protocol's own ceiling */
#define BLEIF_DIAL_GAP_MS   5000    /* per-peer connection-attempt rate limit */
#define BLEIF_BACKOFF_MS    30000   /* after repeated failures */

/* The cross-file state lives in a namespace and is pulled in unqualified
 * below. Plain `s_`-prefixed globals would be external symbols in a flat link
 * of forty straddles, where `s_peers` and `s_enabled` are names half of them
 * use for something file-local; a namespace keeps the familiar names without
 * putting them in that shared pool. */
namespace reticulous_ble {

extern const ble_uuid128_t BLEIF_SVC_UUID;
extern const ble_uuid128_t BLEIF_RX_UUID;
extern const ble_uuid128_t BLEIF_TX_UUID;
extern const ble_uuid128_t BLEIF_ID_UUID;

/* Our own GATT server's value handles, filled by NimBLE at registration. */
extern uint16_t s_rxHandle;
extern uint16_t s_txHandle;
extern uint16_t s_idHandle;

/* ─────────────── the peer table ───────────────
 *
 * KEYED BY IDENTITY, NOT BY ADDRESS. Android rotates its adapter address about
 * every fifteen minutes, so an address is a way to reach a peer and never a way
 * to name one. The address → identity map the protocol calls for is this table
 * itself: a peer we have only just met is address-keyed with haveIdent = 0
 * until its 16-byte handshake arrives, and if that identity already has a row
 * the two are merged — the surviving row keeps its rnsd interface, so a
 * rotation costs nothing above the reconnection.
 */
enum {
    BLEIF_ST_IDLE = 0,
    BLEIF_ST_DIALLING,     /* bleConnect issued */
    BLEIF_ST_DISC_SVC,     /* discovering the service */
    BLEIF_ST_DISC_CHR,     /* discovering its characteristics */
    BLEIF_ST_SUBSCRIBE,    /* writing the CCCD on TX */
    BLEIF_ST_READ_ID,      /* reading the Identity characteristic */
    BLEIF_ST_HANDSHAKE,    /* writing our own 16 bytes to RX */
    BLEIF_ST_READY,
};

struct bleif_peer_t {
    bool     used;
    uint8_t  ident[BLEIF_IDENT_LEN];
    bool     haveIdent;
    uint8_t  addr[6];
    uint8_t  addrType;
    bool     central;             /* we dialled them */
    uint16_t conn;
    uint16_t mtu;
    uint8_t  stage;

    /* The peer's handles, learned by discovery. Central role only — as a
     * peripheral we write nothing to them and notify on our own s_txHandle. */
    uint16_t svcStart, svcEnd;
    uint16_t peerRx, peerTx, peerId, peerCccd;

    /* Fragmentation. One reassembly buffer per peer, in PSRAM. */
    uint16_t txSeq;
    uint8_t* reasm;
    uint16_t reasmLen, reasmTotal, reasmNext;
    uint32_t reasmStartMs;

    int      rnsd;                /* ITS handle of this peer's rnsd interface */
    char     name[24];            /* "ble/<8 hex of the identity>" */

    int8_t   rssi;
    uint32_t lastTxMs, lastSeenMs;
    uint32_t fragsIn, fragsOut, pktsIn, pktsOut;
};

extern bleif_peer_t s_peers[BLEIF_MAX_PEERS];
extern uint8_t      s_ownIdent[BLEIF_IDENT_LEN];

/* Live configuration, read by every file. */
extern int  s_maxPeers;
extern int  s_minRssi;
extern int  s_scanIntervalMs;
extern int  s_communityRadius;
extern uint8_t s_ifaceMode;
extern char s_ifacNetname[32];
extern char s_ifacNetkey[64];
extern int  s_ifacSize;
extern int  s_bitrate;

}   /* namespace reticulous_ble */
using namespace reticulous_ble;

void bleifWake(void);

/** Claim an inbound connection as a v2.2 peer. Called on the ble_if task, at a
 *  connection's first PROOF of the protocol — its subscribe to our TX, or a
 *  bare identity handshake written to our RX. A bare connect is never proof:
 *  it may belong to another consumer (the RNode door shares the radio).
 *  Returns the row, or nullptr (disabled, or table full — full disconnects,
 *  which is safe exactly because the connection has proven itself ours). */
bleif_peer_t* bleifClaimInbound(uint16_t conn, const uint8_t addr[6],
                                uint8_t addrType, uint16_t mtu);

/* ── ble_peers.cpp ── */
/** The dial gate: rate limit and failure back-off, keyed by ADDRESS and kept
 *  OUTSIDE the peer table — peerDrop wipes the row, so the memory that a peer
 *  keeps failing has to live somewhere the drop cannot reach. */
bool dialAllowed(const uint8_t addr[6]);
void dialStamp(const uint8_t addr[6], uint32_t delayMs);
void dialPenalize(const uint8_t addr[6]);
void dialClear(const uint8_t addr[6]);

/** True while some row other than `except` holds a live READY session. A new
 *  session opened in that state may be a duplicate of the peer already linked,
 *  which is not knowable until its identity arrives — see bleConnKeepAddr. */
bool peerAnyReady(const bleif_peer_t* except);

bleif_peer_t* peerByConn(uint16_t conn);
bleif_peer_t* peerByAddr(const uint8_t addr[6]);
bleif_peer_t* peerByIdent(const uint8_t ident[BLEIF_IDENT_LEN]);
bleif_peer_t* peerAlloc(const uint8_t addr[6], uint8_t addrType);
void          peerAdoptIdent(bleif_peer_t* p, const uint8_t ident[BLEIF_IDENT_LEN]);
void          peerDrop(bleif_peer_t* p, const char* why);
int           peerCount(void);
/* Peers at BLEIF_ST_READY — attached and carrying RNS. peerCount() includes
 * every slot a dial has claimed, which is what the connection budget reserves
 * against but not what an operator means by "peers". */
int           peerReadyCount(void);
void          peerPublish(void);
/* The `B<n>` status-bar pill — the mesh switch and the attached-peer count.
 * Lives in ble_iface.cpp, where the switch does. */
void          bleifPublishPill(void);

/* ── ble_frag.cpp ── */
/** Fragment `data` and hand each fragment to `emit`. Returns false the moment
 *  `emit` refuses one, leaving the packet incomplete — the caller drops it,
 *  which is correct: a half-sent packet is not a packet. */
typedef bool (*bleif_emit_fn)(bleif_peer_t* p, const uint8_t* frag, size_t len);
bool fragSend(bleif_peer_t* p, const uint8_t* data, size_t len, bleif_emit_fn emit);

/** Feed one inbound ATT value. Anything that is neither a 16-byte handshake nor
 *  a parseable fragment is ignored in silence — that is the keepalive path.
 *  On a completed packet, calls `deliver`. */
typedef void (*bleif_deliver_fn)(bleif_peer_t* p, const uint8_t* pkt, size_t len);
void fragRecv(bleif_peer_t* p, const uint8_t* data, size_t len, bleif_deliver_fn deliver);

/** Expire a reassembly that stalled. Called once per task pass. */
void fragTick(bleif_peer_t* p);

/** The largest payload one fragment may carry to this peer. */
size_t fragPayloadMax(const bleif_peer_t* p);

/* ── ble_gattc.cpp (the central role) ── */
void centralOnConnect(bleif_peer_t* p);
void centralStep(bleif_peer_t* p);
bool centralWrite(bleif_peer_t* p, const uint8_t* data, size_t len);
void centralKeepalive(bleif_peer_t* p);
void centralDrain(void);         /* run queued host-context results */
bool centralQueueInit(void);
