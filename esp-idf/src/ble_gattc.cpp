/**
 * ble_gattc — the central half of a v2.2 link.
 *
 *   scan sees the service UUID
 *     ↓ our address sorts lower than the peer's?   no  → wait; they dial us
 *   bleConnect
 *     ↓ BLE_EV_CONNECT
 *   discover 37145b00-…-c5da28e3                  → svcStart..svcEnd
 *     ↓
 *   discover its characteristics                  → RX …28e5, TX …28e4, Ident …28e6
 *     ↓
 *   discover TX's descriptors                     → the 0x2902 CCCD
 *     ↓
 *   write 0x0001 to the CCCD                      (subscribe)
 *     ↓
 *   read Ident                                    → the peer's 16-byte identity
 *     ↓
 *   write our own 16 bytes to RX                  (the handshake)
 *     ↓
 *   READY — fragments both ways, 0x00 to RX every 15 s of transmit idle
 *
 * EVERY CALLBACK HERE RUNS IN NIMBLE HOST CONTEXT and does one thing: it
 * allocates a result block, copies what it needs into it, and posts the pointer
 * to the bleif task. centralDrain() then advances the machine on that task,
 * where the peer table is safe to touch.
 *
 * The CCCD is discovered rather than assumed to be val_handle + 1: that
 * relation holds for a service definition WE registered, not for a peer's
 * layout, and the three characteristics here can be discovered in any order.
 */
#include "ble_iface_priv.h"

#include <cstring>

/* ─────────────── host → task ─────────────── */

enum {
    HEV_SVC = 1,     /* h1 = start, h2 = end */
    HEV_SVC_DONE,    /* service discovery finished; status 0 = clean end */
    HEV_CHR,         /* h1 = val handle, h2 = which (1 RX, 2 TX, 3 Ident) */
    HEV_CHR_DONE,
    HEV_CCCD,        /* h1 = CCCD handle */
    HEV_CCCD_DONE,
    HEV_WROTE,       /* an op finished; status says how */
    HEV_IDENT,       /* the peer's identity, in data */
    HEV_DATA,        /* a notification arrived (central role), in data */
    HEV_WRITE,       /* a write landed on OUR RX (peripheral role), in data */
};

struct bleif_hev_t {
    uint8_t  kind;
    uint16_t conn;
    uint16_t h1, h2;
    int16_t  status;
    uint16_t len;
    uint8_t  data[];
};

#define HEV_QDEPTH 16
static QueueHandle_t s_hq = nullptr;

bool centralQueueInit(void) {
    if (!s_hq) s_hq = xQueueCreate(HEV_QDEPTH, sizeof(bleif_hev_t*));
    return s_hq != nullptr;
}

/* Allocate + post. A refused post frees the block and loses the step; the
 * dialling peer then stalls in its stage and is dropped by the stage timeout,
 * which is the same recovery a lost packet gets. */
static void hevPost(uint8_t kind, uint16_t conn, uint16_t h1, uint16_t h2,
                    int16_t status, const uint8_t* data, uint16_t len) {
    if (!s_hq) return;
    auto* e = (bleif_hev_t*)gp_alloc(sizeof(bleif_hev_t) + len);
    if (!e) return;
    e->kind = kind; e->conn = conn; e->h1 = h1; e->h2 = h2;
    e->status = status; e->len = len;
    if (len) std::memcpy(e->data, data, len);
    if (xQueueSend(s_hq, &e, 0) != pdTRUE) { gp_free(e); return; }
    bleifWake();
}

/* ─────────────── host-context callbacks ─────────────── */

static int onDiscSvc(uint16_t conn, const struct ble_gatt_error* error,
                     const struct ble_gatt_svc* svc, void* /*arg*/) {
    /* Each found service arrives as its own callback, and the procedure ENDS
     * with one whose status is BLE_HS_EDONE — a completion marker, never a
     * failure. Fold it (and any real error) into HEV_SVC_DONE; the drain
     * decides by whether a service was reported before it. */
    if (error && error->status != 0) {
        int16_t st = (error->status == BLE_HS_EDONE) ? 0 : (int16_t)error->status;
        hevPost(HEV_SVC_DONE, conn, 0, 0, st, nullptr, 0);
        return 0;
    }
    if (svc) hevPost(HEV_SVC, conn, svc->start_handle, svc->end_handle, 0, nullptr, 0);
    return 0;
}

static int onDiscChr(uint16_t conn, const struct ble_gatt_error* error,
                     const struct ble_gatt_chr* chr, void* /*arg*/) {
    if (!chr || (error && error->status != 0)) {
        hevPost(HEV_CHR_DONE, conn, 0, 0, error ? (int16_t)error->status : 0, nullptr, 0);
        return 0;
    }
    /* The UUID compare is three memcmps and belongs here — posting the whole
     * UUID upward would triple the message for no gain. */
    uint16_t which = 0;
    if      (ble_uuid_cmp(&chr->uuid.u, &BLEIF_RX_UUID.u) == 0) which = 1;
    else if (ble_uuid_cmp(&chr->uuid.u, &BLEIF_TX_UUID.u) == 0) which = 2;
    else if (ble_uuid_cmp(&chr->uuid.u, &BLEIF_ID_UUID.u) == 0) which = 3;
    if (which) hevPost(HEV_CHR, conn, chr->val_handle, which, 0, nullptr, 0);
    return 0;
}

static int onDiscDsc(uint16_t conn, const struct ble_gatt_error* error,
                     uint16_t /*chrValHandle*/, const struct ble_gatt_dsc* dsc,
                     void* /*arg*/) {
    if (!dsc || (error && error->status != 0)) {
        hevPost(HEV_CCCD_DONE, conn, 0, 0, error ? (int16_t)error->status : 0, nullptr, 0);
        return 0;
    }
    ble_uuid16_t cccd = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);
    if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0)
        hevPost(HEV_CCCD, conn, dsc->handle, 0, 0, nullptr, 0);
    return 0;
}

static int onWrote(uint16_t conn, const struct ble_gatt_error* error,
                   struct ble_gatt_attr* /*attr*/, void* /*arg*/) {
    hevPost(HEV_WROTE, conn, 0, 0, error ? (int16_t)error->status : 0, nullptr, 0);
    return 0;
}

static int onReadIdent(uint16_t conn, const struct ble_gatt_error* error,
                       struct ble_gatt_attr* attr, void* /*arg*/) {
    if ((error && error->status != 0) || !attr || !attr->om) {
        hevPost(HEV_IDENT, conn, 0, 0, error ? (int16_t)error->status : -1, nullptr, 0);
        return 0;
    }
    uint8_t  buf[BLEIF_IDENT_LEN];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &n) != 0 || n != BLEIF_IDENT_LEN) {
        hevPost(HEV_IDENT, conn, 0, 0, -1, nullptr, 0);
        return 0;
    }
    hevPost(HEV_IDENT, conn, 0, 0, 0, buf, n);
    return 0;
}

/* Notifications from a peer we dialled. spangap-ble owns the connection's GAP
 * callback, so this is where they surface — host context, copy and post. */
static void onNotifyRx(uint16_t conn, uint16_t /*attr*/,
                       const uint8_t* data, uint16_t len) {
    hevPost(HEV_DATA, conn, 0, 0, 0, data, len);
}

/** Inbound ATT writes on OUR server, from the peripheral half. Host context.
 *  A distinct kind from HEV_DATA: a write to our RX may CLAIM a connection
 *  (see centralDrain), which a notification we subscribed to never does. */
void bleifPostInbound(uint16_t conn, const uint8_t* data, uint16_t len) {
    hevPost(HEV_WRITE, conn, 0, 0, 0, data, len);
}

/* ─────────────── the machine (bleif task) ─────────────── */

void centralOnConnect(bleif_peer_t* p) {
    p->stage = BLEIF_ST_DISC_SVC;
    p->lastSeenMs = millis();
    int rc = ble_gattc_disc_svc_by_uuid(p->conn, &BLEIF_SVC_UUID.u, onDiscSvc, nullptr);
    if (rc != 0) { warn("%s: disc_svc rc=%d", p->name, rc); peerDrop(p, "discovery failed"); }
}

/* Nothing to poll: each stage is entered by the previous one's result. This
 * exists so the task can time a stage out — a peer that stops answering
 * mid-discovery would otherwise hold a slot forever. */
void centralStep(bleif_peer_t* p) {
    if (p->stage == BLEIF_ST_IDLE || p->stage == BLEIF_ST_READY) return;
    if (millis() - p->lastSeenMs < 15000) return;
    peerDrop(p, "stalled during bring-up");
}

bool centralWrite(bleif_peer_t* p, const uint8_t* data, size_t len) {
    /* Write-without-response: an acknowledged write would cost a round trip per
     * fragment, and the ATT layer's own credit is the back-pressure. ENOMEM
     * here means the controller's buffers are full — the caller stops and
     * retries, it never drops. */
    return ble_gattc_write_no_rsp_flat(p->conn, p->peerRx, data, (uint16_t)len) == 0;
}

void centralKeepalive(bleif_peer_t* p) {
    /* Columba's peripheral disconnects after three missed intervals, so a link
     * that is simply quiet has to say something. One byte, which the far side
     * ignores because it is neither a handshake nor a fragment. */
    uint8_t z = 0;
    if (ble_gattc_write_no_rsp_flat(p->conn, p->peerRx, &z, 1) == 0)
        p->lastTxMs = millis();
}

/* Defined in ble_iface.cpp — the shared inbound path for both roles. */
void bleifOnInbound(bleif_peer_t* p, const uint8_t* data, size_t len);

void centralDrain(void) {
    if (!s_hq) return;
    bleif_hev_t* e = nullptr;
    while (xQueueReceive(s_hq, &e, 0) == pdTRUE) {
        bleif_peer_t* p = peerByConn(e->conn);
        if (!p && e->kind == HEV_WRITE && e->len == BLEIF_IDENT_LEN) {
            /* A bare identity handshake from a connection nothing claimed:
             * a central that writes before (or instead of) subscribing —
             * Columba does, whatever order the sketch draws. The write to
             * OUR RX characteristic is proof enough of v2.2, so this is a
             * claim moment beside the subscribe. */
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(e->conn, &d) == 0)
                p = bleifClaimInbound(e->conn, d.peer_id_addr.val,
                                      d.peer_id_addr.type, bleConnMtu(e->conn));
        }
        if (!p) {
            if (e->kind == HEV_WRITE)
                dbg("write from unclaimed conn %u dropped (%u B)",
                    (unsigned)e->conn, (unsigned)e->len);
            gp_free(e);
            continue;
        }
        p->lastSeenMs = millis();

        switch (e->kind) {

        case HEV_SVC:
            if (e->h1 == 0) break;
            p->svcStart = e->h1;
            p->svcEnd   = e->h2;
            p->stage    = BLEIF_ST_DISC_CHR;
            ble_gattc_disc_all_chrs(p->conn, p->svcStart, p->svcEnd, onDiscChr, nullptr);
            break;

        case HEV_SVC_DONE:
            /* Meaningful only if no service was reported before it — a clean
             * completion after HEV_SVC already moved the stage on. */
            if (p->stage == BLEIF_ST_DISC_SVC) peerDrop(p, "service not found");
            break;

        case HEV_CHR:
            if      (e->h2 == 1) p->peerRx = e->h1;
            else if (e->h2 == 2) p->peerTx = e->h1;
            else if (e->h2 == 3) p->peerId = e->h1;
            break;

        case HEV_CHR_DONE:
            if (!p->peerRx || !p->peerTx || !p->peerId) {
                peerDrop(p, "the service is missing a characteristic");
                break;
            }
            p->stage = BLEIF_ST_SUBSCRIBE;
            ble_gattc_disc_all_dscs(p->conn, p->peerTx, p->svcEnd, onDiscDsc, nullptr);
            break;

        case HEV_CCCD:
            if (!p->peerCccd) p->peerCccd = e->h1;   /* the first, i.e. TX's */
            break;

        case HEV_CCCD_DONE: {
            if (!p->peerCccd) { peerDrop(p, "no CCCD on the TX characteristic"); break; }
            static const uint8_t on[2] = { 0x01, 0x00 };
            ble_gattc_write_flat(p->conn, p->peerCccd, on, sizeof(on), onWrote, nullptr);
            break;
        }

        case HEV_WROTE:
            if (e->status != 0) { peerDrop(p, "a setup write was refused"); break; }
            if (p->stage == BLEIF_ST_SUBSCRIBE) {
                p->stage = BLEIF_ST_READ_ID;
                ble_gattc_read(p->conn, p->peerId, onReadIdent, nullptr);
            } else if (p->stage == BLEIF_ST_HANDSHAKE) {
                p->stage = BLEIF_ST_READY;
                bleConnKeepAddr(p->conn, false);   /* a session of its own: its
                                                    * end rotates as any
                                                    * session's does */
                p->lastTxMs = millis();
                dialClear(p->addr);      /* a working peer starts with a clean record */
                info("%s: link up (mtu %u)", p->name, (unsigned)p->mtu);
                peerPublish();
            }
            break;

        case HEV_IDENT:
            if (e->status != 0 || e->len != BLEIF_IDENT_LEN) {
                peerDrop(p, "the peer would not give an identity");
                break;
            }
            peerAdoptIdent(p, e->data);
            p = peerByConn(e->conn);     /* adoption may have merged the rows */
            if (!p) break;
            p->stage = BLEIF_ST_HANDSHAKE;
            /* The peripheral cannot read from us, so our identity goes over as
             * a bare 16-byte write — a length that means "handshake" only while
             * the far side knows no identity for this address. */
            ble_gattc_write_flat(p->conn, p->peerRx, s_ownIdent, BLEIF_IDENT_LEN,
                                 onWrote, nullptr);
            break;

        case HEV_DATA:
        case HEV_WRITE:
            bleifOnInbound(p, e->data, e->len);
            break;

        default:
            break;
        }
        gp_free(e);
    }
}

/* Installed once, from the task. */
void bleifInstallNotifyRx(void) { bleOnNotifyRx(onNotifyRx); }
