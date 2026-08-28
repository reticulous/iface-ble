/**
 * ble_peers — the peer table and the rnsd interface each peer gets.
 *
 * ONE rnsd INTERFACE PER PEER, not one for the medium. That is what upstream
 * does and what iface-tcp does for its inbound connections, and it is what
 * makes `point_to_point = 1` correct (a forwarded announce is
 * never echoed back down the link it arrived on) and announce accounting
 * per-peer. The cost is one of the global RNSD_MAX_IFACES per peer, shared
 * with lora, auto, espnow and every tcp peer.
 *
 * The community radius (`s.ble.rns.community_radius`, default 3) bounds what
 * this node serves through a phone peer: announces within the radius are
 * stored and answered for, while the wider mesh the phone relays stays out of
 * the directory. The direct peer itself is always reachable — path responses
 * this node asked for, claimed and in-use destinations are retained
 * regardless (the "resolved on demand" arm of the retention rule).
 */
#include "ble_iface_priv.h"

#include <cstdio>
#include <cstring>

namespace reticulous_ble { bleif_peer_t s_peers[BLEIF_MAX_PEERS] = {}; }

static void onRnsdRecv(int handle, size_t bytesAvail);
static void onRnsdDisconnect(int ref);

/* ─────────────── the dial gate ───────────────
 *
 * Rate limit and failure back-off, keyed by address and kept outside the peer
 * table: peerDrop wipes the row, so a peer that advertises but keeps refusing
 * would otherwise come back as a fresh row with a clean record and turn the
 * scan into a dial loop. An entry ages out by eviction — once its deadline
 * passed it is the first candidate to be reused. */

struct dial_gate_t {
    bool     used;
    uint8_t  addr[6];
    uint8_t  fails;
    uint32_t nextDialMs;
};
static dial_gate_t s_gates[BLEIF_MAX_PEERS + 1];

static dial_gate_t* gateFind(const uint8_t addr[6]) {
    for (auto& g : s_gates)
        if (g.used && std::memcmp(g.addr, addr, 6) == 0) return &g;
    return nullptr;
}

static dial_gate_t* gateGet(const uint8_t addr[6]) {
    dial_gate_t* g = gateFind(addr);
    if (g) return g;
    dial_gate_t* victim = nullptr;
    for (auto& k : s_gates) {
        if (!k.used) { victim = &k; break; }
        if (!victim || (int32_t)(k.nextDialMs - victim->nextDialMs) < 0) victim = &k;
    }
    *victim = dial_gate_t{};
    victim->used = true;
    std::memcpy(victim->addr, addr, 6);
    return victim;
}

bool dialAllowed(const uint8_t addr[6]) {
    dial_gate_t* g = gateFind(addr);
    return !g || (int32_t)(millis() - g->nextDialMs) >= 0;
}

void dialStamp(const uint8_t addr[6], uint32_t delayMs) {
    gateGet(addr)->nextDialMs = millis() + delayMs;
}

void dialPenalize(const uint8_t addr[6]) {
    dial_gate_t* g = gateGet(addr);
    if (g->fails < 255) g->fails++;
    g->nextDialMs = millis()
                  + (g->fails >= 3 ? BLEIF_BACKOFF_MS : BLEIF_DIAL_GAP_MS);
}

void dialClear(const uint8_t addr[6]) {
    dial_gate_t* g = gateFind(addr);
    if (g) g->used = false;
}

/* ─────────────── lookup ─────────────── */

bool peerAnyReady(const bleif_peer_t* except) {
    for (auto& p : s_peers)
        if (p.used && &p != except && p.stage == BLEIF_ST_READY
                && p.conn != BLE_HS_CONN_HANDLE_NONE) return true;
    return false;
}

bleif_peer_t* peerByConn(uint16_t conn) {
    for (auto& p : s_peers)
        if (p.used && p.conn == conn && conn != BLE_HS_CONN_HANDLE_NONE) return &p;
    return nullptr;
}

bleif_peer_t* peerByAddr(const uint8_t addr[6]) {
    for (auto& p : s_peers)
        if (p.used && std::memcmp(p.addr, addr, 6) == 0) return &p;
    return nullptr;
}

bleif_peer_t* peerByIdent(const uint8_t ident[BLEIF_IDENT_LEN]) {
    for (auto& p : s_peers)
        if (p.used && p.haveIdent && std::memcmp(p.ident, ident, BLEIF_IDENT_LEN) == 0)
            return &p;
    return nullptr;
}

int peerCount(void) {
    int n = 0;
    for (auto& p : s_peers) if (p.used) n++;
    return n;
}

/* Peers actually carrying RNS. A slot is claimed the moment a dial starts and
 * lives through six bring-up stages, so `peerCount` is what the CONNECTION
 * BUDGET has to reserve against — but it is not how many peers this node has.
 * A scan burst with two dials in flight would otherwise report three peers with
 * one attached, which is what an operator reads off the status line. */
int peerReadyCount(void) {
    int n = 0;
    for (auto& p : s_peers) if (p.used && p.stage == BLEIF_ST_READY) n++;
    return n;
}

bleif_peer_t* peerAlloc(const uint8_t addr[6], uint8_t addrType) {
    bleif_peer_t* p = peerByAddr(addr);
    if (p) return p;
    if (peerCount() >= s_maxPeers) return nullptr;
    for (auto& k : s_peers) {
        if (k.used) continue;
        uint8_t* keep = k.reasm;     /* the buffer outlives the peer that used it */
        k = bleif_peer_t{};
        k.reasm    = keep;
        k.used     = true;
        k.conn     = BLE_HS_CONN_HANDLE_NONE;
        k.rnsd     = -1;
        k.addrType = addrType;
        std::memcpy(k.addr, addr, 6);
        safeStrncpy(k.name, "ble/?", sizeof(k.name));
        return &k;
    }
    return nullptr;
}

/* ─────────────── rnsd registration ─────────────── */

static void registerWithRnsd(bleif_peer_t* p) {
    if (p->rnsd >= 0 || !p->haveIdent) return;

    rnsd_iface_t reg = {};
    snprintf(reg.name, sizeof(reg.name), "ble/%02x%02x%02x%02x",
             p->ident[0], p->ident[1], p->ident[2], p->ident[3]);
    safeStrncpy(p->name, reg.name, sizeof(p->name));
    /* rnsd's MTU is fixed at the RNS protocol's 500; the ATT MTU governs what
     * one fragment carries and never leaves this straddle. */
    reg.mtu     = BLEIF_RNS_MTU;
    reg.bitrate = (uint32_t)s_bitrate;
    reg.mode    = s_ifaceMode;
    reg.in = reg.out = 1;
    reg.fwd = (s_ifaceMode == RNS_IFACE_MODE_GATEWAY || s_ifaceMode == RNS_IFACE_MODE_FULL) ? 1 : 0;
    reg.rpt = 0;
    reg.point_to_point   = 1;
    /* Who is at the far end, for the neighbourhood listing to show until this
     * peer's first announce names it — a peer that has attached and said
     * nothing is still a row rather than a silence. The adapter address is what
     * an operator has to compare against the other device. */
    snprintf(reg.peer_label, sizeof(reg.peer_label), "%02X:%02X:%02X:%02X:%02X:%02X",
             p->addr[5], p->addr[4], p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
    reg.community_radius = (uint8_t)s_communityRadius;
    reg.announce_cap     = 0;        /* rnsd applies the RNS default of 2% */
    reg.ifac_size        = (uint8_t)s_ifacSize;
    safeStrncpy(reg.ifac_netname, s_ifacNetname, sizeof(reg.ifac_netname));
    safeStrncpy(reg.ifac_netkey,  s_ifacNetkey,  sizeof(reg.ifac_netkey));

    /* Slot index = ITS ref = array index, so both disconnect callbacks resolve
     * the same peer — the iface-tcp inbound shape. */
    int slot = (int)(p - s_peers);
    p->rnsd = itsConnect("rnsd", RNSD_PORT_IFACE, &reg, sizeof reg,
                         pdMS_TO_TICKS(500), slot, onRnsdRecv, onRnsdDisconnect);
    if (p->rnsd < 0) { warn("%s: rnsd register failed", reg.name); return; }
    info("%s: registered (mtu %u, %u bit/s)", reg.name,
         (unsigned)reg.mtu, (unsigned)reg.bitrate);
}

/* ─────────────── identity adoption ───────────────
 *
 * The one place two rows become one. A peer whose address rotated arrives as a
 * fresh address-keyed row and hands over an identity we already know; the OLD
 * row survives — it holds the rnsd interface, and re-registering it would churn
 * a global interface slot and a storage batch that lxmf subscribes to for
 * nothing. */
void peerAdoptIdent(bleif_peer_t* p, const uint8_t ident[BLEIF_IDENT_LEN]) {
    if (p->haveIdent) return;

    bleif_peer_t* known = peerByIdent(ident);
    if (known && known != p) {
        char a[20];
        snprintf(a, sizeof(a), "%02X:%02X:%02X:%02X:%02X:%02X",
                 p->addr[5], p->addr[4], p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
        /* Liveness gate: a peer whose address changed under a live link is
         * rediscovered as a stranger and dialled, so it arrives here twice.
         * A READY session with inbound traffic inside a keepalive-plus-margin
         * window outranks any newcomer (adopting one trades a working session
         * for one the peer's own duplicate check kills seconds later); only a
         * silent session — possibly a corpse awaiting its supervision timeout
         * — cedes. */
        if (known->conn != BLE_HS_CONN_HANDLE_NONE && known->conn != p->conn &&
            known->stage == BLEIF_ST_READY &&
            millis() - known->lastSeenMs < BLEIF_KEEPALIVE_MS + 10000) {
            info("%s: duplicate session from %s dropped (existing link is live)",
                 known->name, a);
            uint16_t dupConn = p->conn;
            /* A dial that ended here never reached READY, so it owes the gate
             * a failure exactly as peerDrop would charge one — this path wipes
             * the row itself and never goes through peerDrop. */
            if (p->central) dialPenalize(p->addr);
            uint8_t* keep = p->reasm;
            *p = bleif_peer_t{};
            p->reasm = keep;
            p->conn  = BLE_HS_CONN_HANDLE_NONE;
            p->rnsd  = -1;
            if (dupConn != BLE_HS_CONN_HANDLE_NONE) {
                /* Hold the address across this teardown. Rotating here would
                 * put a stranger on air for the very peer on the other end of
                 * the link we just kept, whose scanner would dial it and
                 * arrive as the next duplicate; holding it lets that peer
                 * match us by address and never dial at all. */
                bleConnKeepAddr(dupConn, true);
                bleDisconnect(dupConn);
            }
            peerPublish();
            return;
        }
        info("%s: address moved to %s", known->name, a);
        /* Move the live connection onto the row that owns the interface. */
        if (known->conn != BLE_HS_CONN_HANDLE_NONE && known->conn != p->conn)
            bleDisconnect(known->conn);
        known->conn     = p->conn;
        known->central  = p->central;
        known->mtu      = p->mtu;
        known->stage    = p->stage;
        known->addrType = p->addrType;
        known->peerRx   = p->peerRx;
        known->peerTx   = p->peerTx;
        known->peerId   = p->peerId;
        known->peerCccd = p->peerCccd;
        known->rssi     = p->rssi;
        known->lastSeenMs = millis();
        std::memcpy(known->addr, p->addr, 6);
        uint8_t* keep = p->reasm;
        *p = bleif_peer_t{};
        p->reasm = keep;
        p = known;
    } else {
        std::memcpy(p->ident, ident, BLEIF_IDENT_LEN);
        p->haveIdent = true;
    }
    p->lastSeenMs = millis();
    registerWithRnsd(p);
    peerPublish();
}

/* ─────────────── teardown ─────────────── */

void peerDrop(bleif_peer_t* p, const char* why) {
    if (!p || !p->used) return;
    /* A dial we made that never reached READY is a failure the gate must
     * remember — the row itself is wiped below. */
    if (p->central && p->stage != BLEIF_ST_IDLE && p->stage != BLEIF_ST_READY)
        dialPenalize(p->addr);
    /* rnsd first, exactly as iface-tcp tears an inbound peer down: closing the
     * interface before the transport means no frame can arrive for a peer that
     * is half gone. */
    if (p->rnsd >= 0) { itsDisconnect(p->rnsd); p->rnsd = -1; }
    if (p->conn != BLE_HS_CONN_HANDLE_NONE) bleDisconnect(p->conn);
    info("%s: dropped (%s)", p->name, why);
    uint8_t* keep = p->reasm;
    *p = bleif_peer_t{};
    p->reasm = keep;          /* the buffer is recycled by the next peer */
    p->conn  = BLE_HS_CONN_HANDLE_NONE;
    p->rnsd  = -1;
    peerPublish();
}

static void onRnsdDisconnect(int ref) {
    if (ref < 0 || ref >= BLEIF_MAX_PEERS) return;
    bleif_peer_t& p = s_peers[ref];
    if (!p.used) return;
    p.rnsd = -1;
    peerDrop(&p, "rnsd closed the interface");
}

/* rnsd → peer. Declared here and defined in ble_iface.cpp, where the transmit
 * path (fragment, then write or notify) lives. */
void bleifSendToPeer(bleif_peer_t* p);

static void onRnsdRecv(int handle, size_t /*bytesAvail*/) {
    for (auto& p : s_peers)
        if (p.used && p.rnsd == handle) { bleifSendToPeer(&p); return; }
}

/* ─────────────── readout ─────────────── */

void peerPublish(void) {
    char buf[160];
    int n = peerCount(), ready = 0;
    uint32_t in = 0, out = 0;
    for (auto& p : s_peers) {
        if (!p.used) continue;
        if (p.stage == BLEIF_ST_READY) ready++;
        in  += p.pktsIn;
        out += p.pktsOut;
    }
    storageBegin();
    if (!n) safeStrncpy(buf, "no peers", sizeof(buf));
    else {
        int o = snprintf(buf, sizeof(buf), "%d peer%s, %d linked", n, n == 1 ? "" : "s", ready);
        for (auto& p : s_peers) {
            if (!p.used || !p.haveIdent || o >= (int)sizeof(buf) - 24) continue;
            o += snprintf(buf + o, sizeof(buf) - o, " \xC2\xB7 %s %d dBm", p.name, (int)p.rssi);
        }
    }
    storageSet("ble.rns.peers", buf);
    snprintf(buf, sizeof(buf), "in %u \xC2\xB7 out %u packets", (unsigned)in, (unsigned)out);
    storageSet("ble.rns.traffic", buf);
    storageEnd();
    bleifPublishPill();
}
