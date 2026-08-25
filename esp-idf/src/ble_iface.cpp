/**
 * ble_iface — RNS over the ble-reticulum v2.2 protocol.
 *
 *   lower BLE address (central)              higher BLE address (peripheral)
 *     |  scan, filter 37145b00-…-c5da28e3           advertising that service
 *     |  ---- connect ------------------------>
 *     |  <--- READ Identity char …28e6 (16 B) -
 *     |  ---- write CCCD on TX char …28e4 ---->
 *     |  ---- WRITE 16 B to RX char …28e5 ---->     identity handshake
 *     |  ---- WRITE fragments to RX ---------->     [type|seq16|total16|payload]
 *     |  <--- NOTIFY fragments on TX ---------
 *     |  ---- WRITE 1 B 0x00 every 15 s ------>     keepalive, on transmit idle
 *
 * BOTH ROLES AT ONCE. The device advertises the service and scans for it. Who
 * dials is decided by comparing the two adapter addresses as integers — the
 * lower one becomes the central. There is no negotiation and no retry from the
 * other side. Our own address rotates (spangap-ble: every host start, every
 * disconnect, 15-minute idle floor) but is stable in between, so the election
 * stays deterministic; the copy in s_ownAddr is re-read on every BLE_EV_UP,
 * which fires again after each rotation.
 *
 * PEERS ARE NAMED BY IDENTITY, REACHED BY ADDRESS. Android rotates its address
 * about every fifteen minutes. The 16-byte identity hash is the peer's name and
 * the key of the peer table; an address is only how to get there this quarter
 * of an hour. That is what keeps a peer's rnsd interface alive across a
 * rotation instead of churning a global interface slot.
 *
 * ONE rnsd INTERFACE PER PEER — see ble_peers.cpp for why.
 *
 * Lifecycle is the RNS orchestrator's, not the legacy `rns.ready` barrier:
 * rnsStart() calls bleIfaceStart(), which spawns this task the first time and
 * un-parks it afterwards; bleIfaceStop() parks it. The task never deletes
 * itself — deleting leaks ITS state.
 */
#include "ble_iface_priv.h"

#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* TAG = "ble_if";

static TaskHandle_t s_task = nullptr;

namespace reticulous_ble {

/* Service 37145b00-442d-4a94-917f-8f42c5da28e3 and its three characteristics,
 * least-significant byte first as NimBLE carries them. */
const ble_uuid128_t BLEIF_SVC_UUID = BLE_UUID128_INIT(
    0xe3, 0x28, 0xda, 0xc5, 0x42, 0x8f, 0x7f, 0x91,
    0x94, 0x4a, 0x2d, 0x44, 0x00, 0x5b, 0x14, 0x37);
const ble_uuid128_t BLEIF_TX_UUID = BLE_UUID128_INIT(   /* …28e4, read + notify */
    0xe4, 0x28, 0xda, 0xc5, 0x42, 0x8f, 0x7f, 0x91,
    0x94, 0x4a, 0x2d, 0x44, 0x00, 0x5b, 0x14, 0x37);
const ble_uuid128_t BLEIF_RX_UUID = BLE_UUID128_INIT(   /* …28e5, write */
    0xe5, 0x28, 0xda, 0xc5, 0x42, 0x8f, 0x7f, 0x91,
    0x94, 0x4a, 0x2d, 0x44, 0x00, 0x5b, 0x14, 0x37);
const ble_uuid128_t BLEIF_ID_UUID = BLE_UUID128_INIT(   /* …28e6, read, 16 B */
    0xe6, 0x28, 0xda, 0xc5, 0x42, 0x8f, 0x7f, 0x91,
    0x94, 0x4a, 0x2d, 0x44, 0x00, 0x5b, 0x14, 0x37);

uint16_t s_rxHandle = 0;
uint16_t s_txHandle = 0;
uint16_t s_idHandle = 0;

uint8_t s_ownIdent[BLEIF_IDENT_LEN] = {};

int  s_maxPeers        = 3;
int  s_minRssi         = -85;
int  s_scanIntervalMs  = 5000;
int  s_communityRadius = 3;
uint8_t s_ifaceMode    = RNS_IFACE_MODE_ACCESS_POINT;
char s_ifacNetname[32] = "";
char s_ifacNetkey[64]  = "";
int  s_ifacSize        = 0;
/* An estimate until somebody measures it: ~500 B of payload per 30 ms
 * connection interval at MTU 247. It is not cosmetic — rnsd feeds it to the
 * first-hop link timeout and to the announce throttle's airtime maths. */
int  s_bitrate         = 100000;

}   /* namespace reticulous_ble */

static volatile bool s_stop        = false;
static volatile bool s_parked      = false;
static volatile bool s_configDirty = true;
static bool          s_enabled     = false;
static int           s_advSlot     = -1;
static bool          s_scanning    = false;
static uint8_t       s_ownAddr[6]  = {};

void bleifWake(void) { if (s_task) xTaskNotifyGive(s_task); }

void bleifInstallNotifyRx(void);
void bleifPostInbound(uint16_t conn, const uint8_t* data, uint16_t len);

/* ─────────────── our GATT server (the peripheral half) ─────────────── */

static int gattAccess(uint16_t conn, uint16_t attr,
                      struct ble_gatt_access_ctxt* ctxt, void* /*arg*/) {
    /* HOST CONTEXT. Copy and post, and answer a read from a buffer that is
     * already there — nothing else may happen on this task. */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr != s_idHandle) return 0;
        return os_mbuf_append(ctxt->om, s_ownIdent, BLEIF_IDENT_LEN) == 0
                 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr != s_rxHandle) return 0;

    uint8_t  buf[BLEIF_REASM_MAX];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0) return 0;
    if (n) bleifPostInbound(conn, buf, n);
    return 0;
}

static const struct ble_gatt_chr_def BLEIF_CHRS[] = {
    {   /* TX: we notify on this, a central subscribes to it and may read it */
        .uuid        = &BLEIF_TX_UUID.u,
        .access_cb   = gattAccess,
        .arg         = nullptr,
        .descriptors = nullptr,
        .flags       = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle  = &s_txHandle,
        .cpfd        = nullptr,
    },
    {   /* RX: the central writes here */
        .uuid        = &BLEIF_RX_UUID.u,
        .access_cb   = gattAccess,
        .arg         = nullptr,
        .descriptors = nullptr,
        .flags       = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle  = &s_rxHandle,
        .cpfd        = nullptr,
    },
    {   /* Identity: 16 bytes, readable without pairing */
        .uuid        = &BLEIF_ID_UUID.u,
        .access_cb   = gattAccess,
        .arg         = nullptr,
        .descriptors = nullptr,
        .flags       = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle  = &s_idHandle,
        .cpfd        = nullptr,
    },
    {},
};

/* No _ENC or _AUTHEN flag anywhere above, deliberately: this service must not
 * trigger a pairing dialog. Access control on this link is IFAC, applied by
 * rnsd to the traffic, not by the Bluetooth stack to the attribute. */
static const struct ble_gatt_svc_def BLEIF_SVCS[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &BLEIF_SVC_UUID.u,
        .includes        = nullptr,
        .characteristics = BLEIF_CHRS,
    },
    {},
};

/* ─────────────── transmit ─────────────── */

/* One fragment onto whichever half of the link this peer is. False means the
 * radio had no buffer; fragSend then abandons the packet, which is right — a
 * half-sent packet is not a packet, and RNS is a datagram network. */
static bool emitFragment(bleif_peer_t* p, const uint8_t* frag, size_t len) {
    bool ok;
    if (p->central) {
        ok = centralWrite(p, frag, len);
    } else {
        struct os_mbuf* om = ble_hs_mbuf_from_flat(frag, len);
        ok = om && ble_gatts_notify_custom(p->conn, s_txHandle, om) == 0;
    }
    if (ok) p->lastTxMs = millis();
    return ok;
}

/* rnsd → this peer. */
void bleifSendToPeer(bleif_peer_t* p) {
    if (p->rnsd < 0 || p->stage != BLEIF_ST_READY) return;
    uint8_t pkt[BLEIF_RNS_MTU + 16];
    while (itsBytesAvailable(p->rnsd) > 0) {
        size_t n = itsRecv(p->rnsd, pkt, sizeof(pkt), 0);
        if (n == 0) break;
        if (n > BLEIF_RNS_MTU) continue;
        if (!fragSend(p, pkt, n, emitFragment)) {
            warn("%s: radio full, packet dropped", p->name);
            break;
        }
    }
}

/* ─────────────── receive ─────────────── */

static void deliverToRnsd(bleif_peer_t* p, const uint8_t* pkt, size_t len) {
    if (p->rnsd < 0) {
        /* A packet before the handshake finished. Upstream warns and drops it,
         * and so do we: there is no interface to hand it to yet. */
        warn("%s: packet before the identity handshake, dropped", p->name);
        return;
    }
    if (itsSend(p->rnsd, pkt, len, pdMS_TO_TICKS(50)) == 0)
        warn("%s: rnsd would not take %u B", p->name, (unsigned)len);
}

/* The shared inbound path — a write we received as peripheral, or a
 * notification we received as central, are the same bytes on the same wire. */
void bleifOnInbound(bleif_peer_t* p, const uint8_t* data, size_t len) {
    p->lastSeenMs = millis();

    /* Wire-level ingress trace (`log tag bleif debug`): the raw ATT value as
     * received, before any interpretation. A v2.2 fragment reads
     * [01|02|03] [seq16] [total16]; anything else here is the peer speaking
     * a different framing than the fragment layer expects. */
    if (len >= 1)
        dbg("rx %3uB: %02x %02x%02x %02x%02x | %02x %02x %02x %02x %02x %02x %02x",
            (unsigned)len,
            data[0],
            len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
            len > 3 ? data[3] : 0, len > 4 ? data[4] : 0,
            len > 5 ? data[5] : 0, len > 6 ? data[6] : 0,
            len > 7 ? data[7] : 0, len > 8 ? data[8] : 0,
            len > 9 ? data[9] : 0, len > 10 ? data[10] : 0,
            len > 11 ? data[11] : 0);

    /* A keepalive (one 0x00 byte). Tolerating it is the spec's minimum;
     * MIRRORING it back — peripheral side only, so two of us can never
     * ping-pong — gives the central's liveness watchdog an inbound byte even
     * on a link with no traffic. A peer whose announce timers were satisfied
     * on another interface sends nothing here, and if its watchdog counts
     * inbound bytes a silent-but-healthy link reads as dead. The echo is
     * ignored by the far side under the same too-short-for-a-fragment rule
     * this function applies. */
    if (len == 1 && data[0] == 0x00) {
        if (!p->central && p->conn != BLE_HS_CONN_HANDLE_NONE) {
            struct os_mbuf* om = ble_hs_mbuf_from_flat(data, 1);
            if (om) ble_gatts_notify_custom(p->conn, s_txHandle, om);
        }
        return;
    }

    /* A 16-byte write is the handshake ONLY while no identity is known for this
     * peer; afterwards it is just a fragment-sized value, and a fragment header
     * is five bytes so the two can never be confused once we know who this is. */
    if (len == BLEIF_IDENT_LEN && !p->haveIdent) {
        /* Capture the handle first: adoption may merge this row into an older
         * one for the same identity and clear this one. */
        uint16_t conn = p->conn;
        peerAdoptIdent(p, data);
        bleif_peer_t* q = peerByConn(conn);
        if (q) q->stage = BLEIF_ST_READY;
        return;
    }
    fragRecv(p, data, len, deliverToRnsd);
}

/* ─────────────── role election ───────────────
 *
 * The two adapter addresses compared as integers; the lower one dials. Six
 * bytes, most significant first — NimBLE stores them the other way round, so
 * the walk runs from index 5 down. */
static bool weDial(const uint8_t peer[6]) {
    for (int i = 5; i >= 0; i--) {
        if (s_ownAddr[i] < peer[i]) return true;
        if (s_ownAddr[i] > peer[i]) return false;
    }
    return false;      /* our own advertisement */
}

/* ─────────────── BLE events (bleif task) ─────────────── */

static void onBleUp(const ble_event_t*) { s_configDirty = true; bleifWake(); }

static void onBleDown(const ble_event_t*) {
    s_advSlot  = -1;
    s_scanning = false;
    for (auto& p : s_peers) if (p.used) peerDrop(&p, "Bluetooth stopped");
}

static void onBleScan(const ble_event_t* ev) {
    if (!s_enabled || s_stop) return;
    if (ev->rssi < s_minRssi) return;
    if (!weDial(ev->addr)) return;             /* they dial us; just wait */

    bleif_peer_t* p = peerByAddr(ev->addr);
    if (p) {
        p->rssi = ev->rssi;
        if (p->stage != BLEIF_ST_IDLE) return;
    } else {
        if (peerCount() >= s_maxPeers) return;
        p = peerAlloc(ev->addr, ev->addrType);
        if (!p) return;
        p->rssi = ev->rssi;
    }
    /* Rate-limited per peer, and backed off after repeated failures, so a peer
     * that is advertising but refusing cannot turn the scan into a dial loop.
     * The gate is address-keyed and outlives the row — see ble_peers.cpp. */
    if (!dialAllowed(p->addr)) return;
    dialStamp(p->addr, BLEIF_DIAL_GAP_MS);
    p->central    = true;                /* ours is the dialling side */
    p->lastSeenMs = millis();            /* the stage timeout runs from here */
    p->stage      = BLEIF_ST_DIALLING;
    if (!bleConnect(p->addr, p->addrType, 7000)) p->stage = BLEIF_ST_IDLE;
}

static void onBleConnect(const ble_event_t* ev) {
    /* Only a connection we have a row for — i.e. one we dialled. An unknown
     * inbound connection may belong to another consumer entirely (the RNode
     * door shares the radio), so it is left alone; a v2.2 central proves
     * itself by SUBSCRIBING to our TX characteristic and is claimed there. */
    bleif_peer_t* p = peerByAddr(ev->addr);
    if (!p) return;
    p->conn    = ev->conn;
    p->central = ev->central;
    /* Ours: on this interface's own transmit power, not the radio's shared one. */
    bleConnOwner(TAG, ev->conn);
    p->mtu     = ev->mtu ? ev->mtu : 23;
    p->lastSeenMs = millis();
    /* 15-30 ms. Fragmentation makes throughput a function of the connection
     * interval, and Android's supervision timeout is what the keepalive fights. */
    bleConnParams(ev->conn, 12, 24, 0, 4000);
    if (ev->central) centralOnConnect(p);
    else             p->stage = BLEIF_ST_SUBSCRIBE;   /* wait for their handshake */
}

static void onBleDisconnect(const ble_event_t* ev) {
    bleif_peer_t* p = peerByConn(ev->conn);
    if (!p && ev->conn == BLE_HS_CONN_HANDLE_NONE) {
        /* A dial that failed: no handle was ever assigned, the address names
         * it. peerDrop records the failure with the dial gate. */
        p = peerByAddr(ev->addr);
        if (p && p->stage != BLEIF_ST_DIALLING) p = nullptr;
    }
    if (!p) return;
    p->conn = BLE_HS_CONN_HANDLE_NONE;
    peerDrop(p, "link down");
}

static void onBleMtu(const ble_event_t* ev) {
    bleif_peer_t* p = peerByConn(ev->conn);
    if (p) p->mtu = ev->mtu;
}

bleif_peer_t* bleifClaimInbound(uint16_t conn, const uint8_t addr[6],
                                uint8_t addrType, uint16_t mtu) {
    if (!s_enabled || s_stop) return nullptr;
    bleif_peer_t* p = peerAlloc(addr, addrType);
    if (!p) { bleDisconnect(conn); return nullptr; }    /* table full */
    p->conn    = conn;
    p->central = false;
    p->mtu     = mtu ? mtu : 23;
    p->stage   = BLEIF_ST_SUBSCRIBE;       /* wait for their handshake */
    p->lastSeenMs = millis();
    bleConnParams(conn, 12, 24, 0, 4000);
    return p;
}

static void onBleSubscribe(const ble_event_t* ev) {
    if (!s_txHandle || ev->attr != s_txHandle || !ev->notify) return;
    dbg("peer subscribed to our TX (conn %u)", (unsigned)ev->conn);
    bleif_peer_t* p = peerByConn(ev->conn);
    if (!p) p = bleifClaimInbound(ev->conn, ev->addr, ev->addrType, ev->mtu);
    if (p) p->lastSeenMs = millis();   /* their handshake write is what readies us */
}

/* ─────────────── reconcile ─────────────── */

static void applyConfig(void) {
    bool en = storageGetInt("s.ble.rns.enable", 0) != 0;

    /* The ceiling is whichever is lower of the protocol's 7 and the
     * controller's connection table; the default leaves one connection of
     * that table for the RNode door. */
    int cap = CONFIG_BT_NIMBLE_MAX_CONNECTIONS < BLEIF_MAX_PEERS
                ? CONFIG_BT_NIMBLE_MAX_CONNECTIONS : BLEIF_MAX_PEERS;
    int mp = storageGetInt("s.ble.rns.max_peers", cap > 1 ? cap - 1 : 1);
    if (mp < 1) mp = 1;
    if (mp > cap) mp = cap;
    int rssi = storageGetInt("s.ble.rns.min_rssi", -85);
    int si   = storageGetInt("s.ble.rns.scan_interval", 5) * 1000;
    if (si < 1000)  si = 1000;
    if (si > 60000) si = 60000;

    char ifn[sizeof(s_ifacNetname)] = "";
    storageGetStr("s.ble.rns.ifac_netname", ifn, sizeof(ifn), "");
    char ifk[sizeof(s_ifacNetkey)] = "";
    storageGetStr("s.ble.rns.ifac_netkey", ifk, sizeof(ifk), "");
    int ifs = storageGetInt("s.ble.rns.ifac_size", 0);
    int radius = storageGetInt("s.ble.rns.community_radius", 3);
    char modeStr[24] = "access_point";
    storageGetStr("s.ble.rns.mode", modeStr, sizeof(modeStr), "access_point");
    uint8_t m;
    if      (strcmp(modeStr, "full")     == 0) m = RNS_IFACE_MODE_FULL;
    else if (strcmp(modeStr, "gateway")  == 0) m = RNS_IFACE_MODE_GATEWAY;
    else if (strcmp(modeStr, "roaming")  == 0) m = RNS_IFACE_MODE_ROAMING;
    else if (strcmp(modeStr, "boundary") == 0) m = RNS_IFACE_MODE_BOUNDARY;
    else                                       m = RNS_IFACE_MODE_ACCESS_POINT;

    /* IFAC, max_peers, mode and the community radius are baked into a peer's
     * registration at handshake time, so a change to any drops the live peers
     * and lets them come back with the new settings — the same thing
     * iface-tcp does to its inbound. */
    bool churn = strcmp(ifn, s_ifacNetname) != 0 || strcmp(ifk, s_ifacNetkey) != 0
              || ifs != s_ifacSize || radius != s_communityRadius || m != s_ifaceMode;

    s_maxPeers        = mp;
    s_minRssi         = rssi;
    s_scanIntervalMs  = si;
    s_ifacSize        = ifs;
    s_communityRadius = radius;
    s_ifaceMode       = m;
    safeStrncpy(s_ifacNetname, ifn, sizeof(s_ifacNetname));
    safeStrncpy(s_ifacNetkey,  ifk, sizeof(s_ifacNetkey));
    s_enabled = en;

    if (churn) for (auto& p : s_peers) if (p.used) peerDrop(&p, "registration settings changed");

    if (!en || s_stop) {
        for (auto& p : s_peers) if (p.used) peerDrop(&p, en ? "stopping" : "disabled");
        if (s_advSlot >= 0) { bleAdvRelease(s_advSlot); s_advSlot = -1; }
        if (s_scanning)     { bleScanStop(); s_scanning = false; }
        bleSlotRelease(TAG);
        bleDown(TAG);
        storageSet("ble.rns.state_text", en ? "stopped" : "disabled");
        return;
    }

    /* Reserve our share of the connection budget and leave the rest — the RNode
     * door needs one slot, and a reservation is what stops seven mesh peers
     * from taking it. */
    bleSlotReserve(TAG, s_maxPeers);
    bleTxPower(TAG, storageGetInt("s.ble.rns.txpower", 9));
    bleUp(TAG);
    if (!bleIsUp()) { storageSet("ble.rns.state_text", "waiting for Bluetooth"); return; }

    if (!bleOwnAddr(s_ownAddr, nullptr)) return;

    /* Advertise with the service UUID and NO name: a 31-byte advertisement will
     * not hold a 128-bit UUID and a name, and v2.2 made the name optional for
     * exactly this reason. */
    if (s_advSlot < 0) {
        ble_adv_req_t r = {};
        std::memcpy(r.uuid128, BLEIF_SVC_UUID.value, 16);
        r.connectable = 1;
        r.includeName = 0;
        r.priority    = 1;
        s_advSlot = bleAdvRequest(&r);
    }
    if (!s_scanning) {
        ble_scan_req_t r = {};
        std::memcpy(r.uuid128, BLEIF_SVC_UUID.value, 16);
        r.minRssi    = (int8_t)s_minRssi;
        r.intervalMs = 100;
        /* Duty cycle from the discovery interval: a 5 s interval buys a 10 %
         * window, and spangap-ble halves it again while a link is attached. */
        r.windowMs   = (uint16_t)(s_scanIntervalMs >= 5000 ? 10 : 30);
        r.activeScan = 0;      /* we filter on the UUID; a name buys nothing */
        s_scanning = bleScanStart(&r);
    }
    storageSet("ble.rns.state_text", peerCount() ? "linked" : "listening");
}

static void onCfgChange(const char*, const char*) { s_configDirty = true; bleifWake(); }

/* ─────────────── CLI ─────────────── */

static const char* stageWord(uint8_t s) {
    switch (s) {
    case BLEIF_ST_IDLE:      return "idle";
    case BLEIF_ST_DIALLING:  return "dialling";
    case BLEIF_ST_DISC_SVC:  return "discovering";
    case BLEIF_ST_DISC_CHR:  return "discovering";
    case BLEIF_ST_SUBSCRIBE: return "subscribing";
    case BLEIF_ST_READ_ID:   return "reading id";
    case BLEIF_ST_HANDSHAKE: return "handshaking";
    default:                 return "ready";
    }
}

static void cliBleif(const char* args) {
    if (args && strcmp(args, "help") == 0) {
        cliPrintf("%-*s RNS-over-Bluetooth interface status\n", CLI_HELP_COL, "ble_if");
        return;
    }
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s RNS-over-Bluetooth interface status\n", CLI_HELP_COL, "ble_if");
        cliPrintf("%-*s one line per peer\n", CLI_HELP_COL, "ble_if peers");
        return;
    }
    cliPrintf("state:    %s\n", !s_enabled ? "disabled"
                              : !bleIsUp() ? "waiting for Bluetooth"
                              : peerCount() ? "linked" : "listening");
    cliPrintf("identity: ");
    for (int i = 0; i < BLEIF_IDENT_LEN; i++) cliPrintf("%02x", s_ownIdent[i]);
    cliPrintf("\n");
    cliPrintf("address:  %02X:%02X:%02X:%02X:%02X:%02X (lower address dials)\n",
              s_ownAddr[5], s_ownAddr[4], s_ownAddr[3],
              s_ownAddr[2], s_ownAddr[1], s_ownAddr[0]);
    cliPrintf("peers:    %d of %d\n", peerCount(), s_maxPeers);
    cliPrintf("radio:    %s, %s\n", s_advSlot >= 0 ? "advertising" : "not advertising",
              s_scanning ? "scanning" : "not scanning");
    cliPrintf("floor:    %d dBm, scan every %d s\n", s_minRssi, s_scanIntervalMs / 1000);
    cliPrintf("bitrate:  %d bit/s (as registered with rnsd)\n", s_bitrate);
}

static void cliBleifPeers(const char* args) {
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s one line per peer\n", CLI_HELP_COL, "ble_if peers");
        return;
    }
    int n = 0;
    for (auto& p : s_peers) {
        if (!p.used) continue;
        cliPrintf("%-14s %-10s %-11s mtu %-4u %4d dBm  frag in %u / out %u\n",
                  p.haveIdent ? p.name : "(no identity)",
                  p.central ? "central" : "peripheral",
                  stageWord(p.stage), (unsigned)p.mtu, (int)p.rssi,
                  (unsigned)p.fragsIn, (unsigned)p.fragsOut);
        n++;
    }
    if (!n) cliPrintf("no peers\n");
}

/* ─────────────── task ─────────────── */

static void bleifTaskMain(void*) {
    info("[%s] task up", TAG);

    /* Only the rnsd side needs client slots — there is no net-side server
     * here — plus headroom for the churn of a peer coming and going. */
    itsClientInit(BLEIF_MAX_PEERS + 2);
    centralQueueInit();

    /* Our own identity hash, the 16 bytes the Identity characteristic answers
     * with and the handshake carries. Pure crypto, inline on this task. */
    if (!rnsdIdentityHash("secrets.rnsd.identity", s_ownIdent))
        warn("no rnsd identity yet — peers cannot name us");

    bleRegister(BLE_EV_UP,         onBleUp);
    bleRegister(BLE_EV_DOWN,       onBleDown);
    bleRegister(BLE_EV_CONNECT,    onBleConnect);
    bleRegister(BLE_EV_DISCONNECT, onBleDisconnect);
    bleRegister(BLE_EV_MTU,        onBleMtu);
    bleRegister(BLE_EV_SUBSCRIBE,  onBleSubscribe);
    bleRegister(BLE_EV_SCAN,       onBleScan);
    bleifInstallNotifyRx();

    storageSubscribeChanges("s.ble.rns",       onCfgChange);
    

    for (;;) {
        s_configDirty = true;
        while (!s_stop) {
            while (itsPoll(0)) {}
            if (s_configDirty) { s_configDirty = false; applyConfig(); }
            centralDrain();

            uint32_t now = millis();
            for (auto& p : s_peers) {
                if (!p.used) continue;
                fragTick(&p);
                centralStep(&p);
                /* The keepalive is the central's job: the peripheral has
                 * nothing to write and only has to tolerate the byte. */
                if (p.central && p.stage == BLEIF_ST_READY &&
                    now - p.lastTxMs >= BLEIF_KEEPALIVE_MS) centralKeepalive(&p);
            }
            /* At most 1 Hz: a pass runs per inbound fragment, and a storage
             * batch per fragment would churn the subscribers for nothing. */
            static uint32_t lastPubMs = 0;
            if (uiTelemetryWanted() && now - lastPubMs >= 1000) {
                lastPubMs = now;
                peerPublish();
            }

            /* One second while enabled, so the keepalive and the reassembly
             * timeout have a tick; parked forever while disabled. */
            itsPoll(s_enabled ? pdMS_TO_TICKS(1000) : portMAX_DELAY);
        }

        /* rns stop: drop every peer (which closes its rnsd interface), hand the
         * radio back, then PARK on the inbox. The task is never deleted — a
         * deleted task leaks its ITS state. */
        for (auto& p : s_peers) if (p.used) peerDrop(&p, "rns stopped");
        if (s_advSlot >= 0) { bleAdvRelease(s_advSlot); s_advSlot = -1; }
        if (s_scanning)     { bleScanStop(); s_scanning = false; }
        bleSlotRelease(TAG);
        bleDown(TAG);
        storageSet("ble.rns.state_text", "stopped");
        s_parked = true;
        info("[%s] stopped", TAG);
        while (s_stop) itsPoll(portMAX_DELAY);
        s_parked = false;
    }
}

/* ── RNS lifecycle hooks ── */

static void bleIfaceStart(void) {
    s_stop = false;
    if (!s_task)
        s_task = spawnTask(bleifTaskMain, TAG, 6144, nullptr, 1, CORE_PRIMARY, STACK_PSRAM);
    else
        xTaskNotifyGive(s_task);        /* un-park the resident task */
}

static void bleIfaceStop(void) {
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 300 && !s_parked; i++) delay(10);
    if (!s_parked) warn("[%s] stop timed out", TAG);
}

void BleIfaceService::onInit() {
    /* Queued, not registered: the Bluetooth host starts lazily, and NimBLE's
     * GATT registry only exists between nimble_port_init() and the host start.
     * BLEIF_SVCS is static const and outlives the process. */
    bleGattAdd(BLEIF_SVCS);

    cliRegisterCmd("ble_if",       cliBleif);
    cliRegisterCmd("ble_if peers", cliBleifPeers);

    storageBegin();
    storageSet("ble.rns.state_text", "disabled");
    storageSet("ble.rns.peers", "no peers");
    storageSet("ble.rns.traffic", "in 0 \xC2\xB7 out 0 packets");
    storageEnd();

    /* The orchestrator, not the legacy rns.ready barrier: rnsStart() calls
     * bleIfaceStart() once rnsd is up and past its boot window. */
    rnsServiceRegister(TAG, bleIfaceStart, bleIfaceStop, RNS_PHASE_IFACE);
}
