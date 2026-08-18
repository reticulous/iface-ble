/**
 * ble_frag — the v2.2 fragment layer.
 *
 *   [ type | seq16 | total16 | payload ]      5-byte header, big-endian
 *     0x01 START      seq == 0
 *     0x02 CONTINUE   anything between
 *     0x03 END        seq == total - 1
 *
 * EVERY PACKET IS FRAGMENTED, including one that fits in a single fragment —
 * there is no bare-payload form on this wire. A single-fragment packet is
 * therefore START with total = 1, which is what the reference emitter produces
 * (its first branch wins) and what a total-driven reassembler completes on.
 *
 * REASSEMBLY IS DRIVEN BY seq AND total, NOT BY type. The two implementations
 * that exist disagree about which type a lone fragment carries, and there is
 * nothing in a type that seq and total do not already say. START (seq 0) resets
 * the buffer, a fragment whose seq is the one expected appends, and seq ==
 * total - 1 completes. Anything else is dropped and the buffer reset — a peer
 * that lost a fragment gets a clean start rather than a spliced packet.
 *
 * PAYLOAD SIZING FOLLOWS COLUMBA. It passes attMtu - 3 (what fits in one
 * writable attribute value) into the fragmenter, which then subtracts this
 * 5-byte header — so a full fragment is exactly attMtu - 3 bytes on the wire.
 * The Linux implementation passes the raw ATT MTU instead, making its full
 * fragments three bytes larger than the attribute can hold. We emit the safe
 * size and accept up to 512 inbound, so we interoperate with both.
 */
#include "ble_iface_priv.h"

#include <cstring>

size_t fragPayloadMax(const bleif_peer_t* p) {
    uint16_t mtu = p->mtu ? p->mtu : 23;
    if (mtu < BLEIF_FRAG_HDR + 4) return 4;
    size_t n = (size_t)mtu - 3 - BLEIF_FRAG_HDR;
    return n > BLEIF_REASM_MAX - BLEIF_FRAG_HDR ? BLEIF_REASM_MAX - BLEIF_FRAG_HDR : n;
}

bool fragSend(bleif_peer_t* p, const uint8_t* data, size_t len, bleif_emit_fn emit) {
    if (len == 0) return true;
    size_t cap = fragPayloadMax(p);
    size_t total = (len + cap - 1) / cap;
    if (total > 0xFFFF) return false;

    uint8_t frag[BLEIF_REASM_MAX];
    for (size_t i = 0; i < total; i++) {
        size_t off = i * cap;
        size_t n   = (len - off) < cap ? (len - off) : cap;
        frag[0] = (i == 0)           ? BLEIF_FRAG_START
                : (i == total - 1)   ? BLEIF_FRAG_END
                                     : BLEIF_FRAG_CONTINUE;
        frag[1] = (uint8_t)(i >> 8);
        frag[2] = (uint8_t)i;
        frag[3] = (uint8_t)(total >> 8);
        frag[4] = (uint8_t)total;
        std::memcpy(frag + BLEIF_FRAG_HDR, data + off, n);
        if (!emit(p, frag, BLEIF_FRAG_HDR + n)) return false;
        p->fragsOut++;
    }
    p->pktsOut++;
    return true;
}

static void reasmReset(bleif_peer_t* p) {
    p->reasmLen = p->reasmTotal = p->reasmNext = 0;
    p->reasmStartMs = 0;
}

void fragTick(bleif_peer_t* p) {
    if (!p->reasmStartMs) return;
    if (millis() - p->reasmStartMs < BLEIF_REASM_MS) return;
    warn("%s: reassembly timed out with %u B of %u fragments",
         p->name, (unsigned)p->reasmLen, (unsigned)p->reasmTotal);
    reasmReset(p);
}

void fragRecv(bleif_peer_t* p, const uint8_t* data, size_t len,
              bleif_deliver_fn deliver) {
    /* The keepalive is a single 0x00 byte, and anything else too short to be a
     * fragment is somebody else's idea we have no opinion about. Both are
     * ignored in silence: feeding either to the reassembler would corrupt a
     * packet in flight, and warning about them would fill the log every 15 s. */
    if (len < BLEIF_FRAG_HDR) return;

    uint16_t seq   = (uint16_t)((data[1] << 8) | data[2]);
    uint16_t total = (uint16_t)((data[3] << 8) | data[4]);
    const uint8_t* pay = data + BLEIF_FRAG_HDR;
    size_t         n   = len - BLEIF_FRAG_HDR;
    if (total == 0 || seq >= total) return;

    if (!p->reasm) {
        p->reasm = (uint8_t*)gp_alloc(BLEIF_REASM_MAX);
        if (!p->reasm) return;
    }

    if (seq == 0) { reasmReset(p); p->reasmTotal = total; p->reasmStartMs = millis(); }
    else if (seq != p->reasmNext || total != p->reasmTotal) {
        /* A gap, or a fragment from a packet we never saw the start of. */
        reasmReset(p);
        return;
    }

    /* The buffer holds one whole RNS packet; a peer claiming more is broken or
     * hostile, and either way the packet is not one rnsd would take. */
    if (p->reasmLen + n > BLEIF_RNS_MTU) { reasmReset(p); return; }
    std::memcpy(p->reasm + p->reasmLen, pay, n);
    p->reasmLen += n;
    p->reasmNext = seq + 1;
    p->fragsIn++;

    if (seq + 1 == total) {
        p->pktsIn++;
        deliver(p, p->reasm, p->reasmLen);
        reasmReset(p);
    }
}
