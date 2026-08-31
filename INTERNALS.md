# iface-ble — internals

Maintainer reference. The [README](README.md) is the operator guide; this is for
changing the code without breaking it. It is self-authoritative.

```
lower BLE address (central)              higher BLE address (peripheral)
  |  scan, filter 37145b00-…-c5da28e3           advertising that service
  |  ---- connect ------------------------>
  |  <--- READ Identity char …28e6 (16 B) -
  |  ---- write CCCD on TX char …28e4 ---->
  |  ---- WRITE 16 B to RX char …28e5 ---->     identity handshake
  |  ---- WRITE fragments to RX ---------->     [type|seq16|total16|payload]
  |  <--- NOTIFY fragments on TX ---------
  |  ---- WRITE 1 B 0x00 every 15 s ------>     keepalive, on transmit idle
```

```
BleIfaceService::onInit()    main task   bleGattAdd, CLI, rnsServiceRegister
 ↓ rnsStart()
bleIfaceStart()                          spawn or un-park
bleifTaskMain                ble_if task  itsClientInit, the host→task queue,
 ↓                                       our identity hash, bleRegister, subs
applyConfig()                ble_if task  enabled? → reserve, bleUp, advertise,
 ↓                                       scan.  disabled? → drop all, park
BLE_EV_SCAN                  ble_if task  floor → election → dial gate → dial
BLE_EV_CONNECT               ble_if task  our dial completed → centralOnConnect
subscribe / bare handshake   ble_if task  an inbound central claims its peer row
centralDrain()               ble_if task  advance the bring-up machine
BLE_EV_* / notify / write    ble_if task bleifOnInbound → handshake or fragment
 ↓ 30 s reassembly timeout, 15 s keepalive, 15 s stage timeout
peerDrop()                   ble_if task  rnsd first, then the link
```

## 1. What this straddle adds

Four source files plus a private header. Nothing is forked or vendored; the
protocol is implemented from its specification.

- **`ble_iface.cpp`** — the service, the task, our GATT server (the peripheral
  half), the transmit path, the shared inbound path, the role election, the
  spangap-ble event handlers, the settings reconcile and the two `ble_if` verbs.
- **`ble_gattc.cpp`** — the central half: the connect → discover → subscribe →
  read-identity → handshake machine, its host-context callbacks, and the
  host→task result queue.
- **`ble_peers.cpp`** — the peer table, the per-peer rnsd registration, the
  identity adoption that merges two rows after an address rotation, and the
  readout.
- **`ble_frag.cpp`** — the v2.2 fragment layer, pure logic over a peer.
- **`ble_iface_priv.h`** — the cross-file state, in a `reticulous_ble` namespace
  pulled in unqualified, so `s_peers` and `s_enabled` are not external symbols
  in a link where half the straddles have something file-local by those names.

## 2. Lifecycle and tasks

`rnsServiceRegister(TAG, bleIfaceStart, bleIfaceStop, RNS_PHASE_IFACE)` from
`onInit()`. The legacy `rns.ready` barrier still exists but no interface
straddle waits on it: the orchestrator calls `bleIfaceStart()` once rnsd is up
and past its boot window. `start` spawns the task the first time and un-parks it
(`xTaskNotifyGive`) thereafter; `stop` sets a flag and waits for the park.

**The task parks; it never deletes itself.** A deleted task leaks its ITS state.
On stop it drops every peer (which closes each peer's rnsd interface), releases
the advertisement, the scan and the connection reservation, calls `bleDown()`,
and then blocks in `itsPoll(portMAX_DELAY)` until the next start.

Priority 1, `CORE_PRIMARY`, PSRAM stack — one level for the whole RNS ecosystem,
because a producer above its consumer never lets back-pressure clear.
`itsClientInit(BLEIF_MAX_PEERS + 2)`: only the rnsd side needs client slots,
with headroom for the churn of a peer coming and going.

| Task | What it does here |
|---|---|
| NimBLE host | `gattAccess` (our RX writes, our Identity reads), the `ble_gattc_*` completions, `onNotifyRx`. Each one copies into a heap block and posts it. |
| `ble_if` (prio 1) | Everything else. |
| `ble` | spangap-ble's own task; delivers our events by ITS aux. |

`centralDrain()` is the single point where a host-context result becomes a state
change. The block is PSRAM (`gp_alloc`), the queue carries pointers, and the
receiving side frees.

## 3. The protocol, in full

**Service** `37145b00-442d-4a94-917f-8f42c5da28e3`

| Characteristic | UUID tail | Properties | Direction |
|---|---|---|---|
| RX | `…28e5` | write, write-without-response | central → peripheral |
| TX | `…28e4` | read, notify (+ standard CCCD) | peripheral → central |
| Identity | `…28e6` | read, exactly 16 bytes | peripheral → central |

No characteristic carries an encryption or authentication flag, deliberately:
this service must never raise a pairing dialog. Access control is IFAC, applied
by rnsd to the traffic.

**The role election** compares the two adapter addresses as integers; the lower
one dials. Six bytes, most significant first — NimBLE stores them the other way
round, so `weDial()` walks from index 5 down. No negotiation, no retry from the
other side: if both ends got the same answer they would both dial, and if both
got the opposite neither would.

**The identity handshake.** The peripheral cannot read from a central, so the
central's first write to RX is a bare 16-byte identity hash, sent after
subscribing. A 16-byte write is a handshake **only while no identity is yet
known for that peer**; afterwards it is just a value of that length, and since a
fragment header is five bytes the two can never be confused once we know who the
peer is.

**The fragment header** is 5 bytes, big-endian: type (`0x01` START, `0x02`
CONTINUE, `0x03` END), 16-bit sequence, 16-bit total. Every packet is
fragmented, including one that fits whole — a single-fragment packet is START
with total 1, which is what the reference emitter produces and what a
total-driven reassembler completes on. Columba also defines a `0x00` LONE type
that the Python side never emits.

**Reassembly is driven by `seq` and `total`, not by type.** The two
implementations that exist disagree about which type a lone fragment carries,
and a type says nothing the sequence and total do not. START (seq 0) resets the
buffer, a fragment whose seq is the expected one appends, and `seq == total - 1`
completes. Anything else resets — a peer that lost a fragment gets a clean start
rather than a spliced packet. `DEFAULT_TIMEOUT` is 30 s, enforced by `fragTick`.

**Payload sizing follows Columba, not the Linux side.** Columba passes
`usableValueLength(attMtu) = attMtu - 3` into the fragmenter, which subtracts
its own 5-byte header, so a full fragment is exactly `attMtu - 3` bytes on the
wire — what one writable attribute value holds. The Linux implementation passes
bleak's raw ATT MTU, making its full fragments three bytes larger than the peer
can accept. We emit `attMtu - 8` of payload and accept anything up to 512
inbound, so we interoperate with both and provoke neither.

**The keepalive** is a single `0x00` byte written to RX after 15 s of transmit
idle. Columba's peripheral disconnects after three missed intervals, so a link
that is merely quiet has to say something. Sending it is the central's job; the
peripheral tolerates the byte AND MIRRORS it back as a notify — peripheral
side only, so two implementations of this rule can never ping-pong. The mirror
exists because a central whose announce timers were satisfied on another
interface sends nothing else here, and a liveness watchdog that counts inbound
bytes would read a silent-but-healthy link as dead. Any other inbound value
that is neither a 16-byte handshake nor a parseable ≥5-byte fragment is
**ignored in silence**: feeding it to the reassembler would corrupt a packet in
flight, and warning about it would fill the log every fifteen seconds.

## 4. The peer table

Keyed by the 16-byte identity, with the address → identity map being the table
itself: a peer we have only just met is address-keyed with `haveIdent = 0` until
its handshake arrives.

`peerAdoptIdent()` is the one place two rows become one. A peer whose address
rotated arrives as a fresh row and hands over an identity we already know; the
**old row survives** — it holds the rnsd interface, and re-registering would
churn one of the global `RNSD_MAX_IFACES` and a storage batch that lxmf
subscribes to, for nothing. The live connection, MTU, stage, discovered handles
and new address are moved onto it and the temporary row is cleared.

The merge is **liveness-gated**: a peer whose address changed under a live link
is rediscovered as a stranger and dialled, so it arrives here twice. If the
row's existing connection is READY and carried anything within a
keepalive-plus-margin window it is demonstrably alive, and the **newcomer** is
dropped — adopting it would trade a working session for one the peer's own
duplicate-identity check kills seconds later. Only a silent existing connection
(a corpse whose supervision timeout has not expired) cedes to the new address.

Dropping the newcomer **holds the adapter address** (`bleConnKeepAddr()`) and
charges the dial gate a failure. Both matter, and between two nodes running
this straddle both are load-bearing: each side rotates per disconnect, so
killing a duplicate would put a fresh address on air, the peer on the other end
of the link we just kept would rediscover us as a stranger and dial, and its
session would be the next duplicate — a loop at the scan interval that runs
until something else breaks it. The dial gate cannot: it is address-keyed, and
a rotation gives every attempt a fresh address and therefore a clean record.
Holding the address makes the peer's `peerByAddr()` lookup hit its existing row
and skip the dial entirely, and makes the gate's rate limit and back-off apply
if it dials anyway. The side that is *terminated on* must hold it too — it
learns nothing about the identity behind a session that dies before its
handshake, so it holds the address for any session opened while another is
already READY (`peerAnyReady()`), and releases it when that session reaches
READY itself.

Each peer's reassembly buffer is allocated once from PSRAM and **kept across
`peerDrop`**, so a peer table that churns does not churn the heap with it.

Registration is `itsConnect("rnsd", RNSD_PORT_IFACE, &reg, sizeof reg, …,
slot, …)` with **slot index = ITS ref = array index**, so both disconnect
callbacks resolve the same peer — the iface-tcp inbound shape. Teardown closes
rnsd **first**: no frame can then arrive for a peer that is half gone.

The registration fields worth stating:

| Field | Value | Why |
|---|---|---|
| `name` | `ble/<8 hex>` | first four bytes of the peer's identity; fits `name[24]` |
| `mtu` | 500 | rnsd's `_FIXED_MTU`. The ATT MTU never leaks upward |
| `mode` | `s.ble.rns.mode`, default `access_point` | |
| `point_to_point` | 1 | one peer per link ⇒ nothing is echoed back to the peer that sent it |
| `community_radius` | `s.ble.rns.community_radius`, default 3 | a phone peer relays the whole wider mesh — those announces arrive deeper than the radius and must not colonise the directory, while nodes within it are served. On-demand, claimed and in-use destinations are retained regardless |
| `announce_cap` | 0 | rnsd applies the RNS default of 2 % |
| `bitrate` | 100000 | **an estimate**, ~500 B per 30 ms interval at MTU 247. Not cosmetic: it feeds the first-hop link timeout and the announce throttle's airtime maths. Measure it and put the real number here. |

## 5. Discovery, dialling and back-off

`bleScanStart` with the service UUID and the configured RSSI floor;
[spangap-ble](../spangap-ble) does the 128-bit-UUID parse over the raw
advertisement bytes once, for every consumer, because ESP-IDF offers no
BlueZ-style service filter.

A scan result is dropped below the floor, dropped if the election says the peer
dials us, and otherwise allocates or finds a peer row. Dialling is rate-limited
per peer (`BLEIF_DIAL_GAP_MS`, 5 s) and backs off to `BLEIF_BACKOFF_MS` (30 s)
after three failures, so a peer that advertises but refuses cannot turn the scan
into a dial loop. The gate is **address-keyed and lives outside the peer
table** (`ble_peers.cpp`): `peerDrop` wipes the row, so the row cannot be where
the failure count survives. `peerDrop` charges a failure for any dialled peer
that never reached READY, as does the duplicate-session drop in
`peerAdoptIdent()`, which wipes its row without going through `peerDrop`;
reaching READY clears the record. The gate only bites while the peer's address
holds still — a peer that rotates per attempt presents a clean record every
time, which is why a duplicate teardown holds our address rather than
rotating it.

**An inbound central is claimed at its first PROOF of protocol, never at
connect.** The radio is shared with the RNode door, and at `BLE_EV_CONNECT`
time an inbound connection is anonymous — claiming it there would put a phone
that dialled the RNode service into the peer table, and the stage timeout
would then cut its session at 15 s. Two things are proof, and either claims
the row (`bleifClaimInbound`): a subscribe to *our* TX characteristic, or a
bare 16-byte identity handshake written to *our* RX — Columba in the field
writes its handshake without subscribing first, whatever order the sketch
draws, so the subscribe alone would drop every one of its writes in silence.
`BLE_EV_CONNECT` therefore only touches rows that already exist — the dials we
made ourselves.

**A proven inbound peer outranks a row still in bring-up.** With the table
full, `bleifClaimInbound` first has `peerEvictForInbound()` drop the stalest
row that never reached READY: a dial can hold a slot for the whole 15 s stage
timeout and a scan can hold one at IDLE behind the dial gate, and neither has
proven anything, while the peer in front of us just spoke v2.2. A row at
DIALLING is spared — it has no connection handle yet, so dropping it would
leave the dial to land on a row that is gone and a connection nobody owns.
Only when every slot is READY or dialling is the connection refused — and a
refusal **holds our address** (`bleConnKeepAddr`) before disconnecting. Rotating there
would present the refused peer a stranger within the second, its address-keyed
dial gate would never see the same address twice, and the refusal would repeat
for as long as the table stayed full, at an advertising and scan restart per
turn. The refusal is logged, rate-limited to one line per 10 s with the count,
because the connect/disconnect pair is otherwise the only trace it leaves.

Advertising carries the service UUID and **no name**: 31 bytes will not hold a
128-bit UUID and a name, which is exactly why v2.2 made the name optional.

**Why our address rotates, and what its type costs us.** Peers key
per-address state by our address, and address-rotating peers clean that state
unreliably — a device that presents the same address twice reconnects into
whatever a peer failed to sweep, a bug class phones never exercise on each
other because their radios mint a fresh address per connection attempt.
spangap-ble gives us the same protection sequentially: a fresh resolvable
private address at every host start, after every disconnect, and at latest
every 15 minutes idle. Resolvable is spangap-ble's choice, made so its bonded
consumers survive rotation; Columba does not bond, so this straddle only pays
the price: an RPA's top bits (01) match every phone's, and the who-dials
election against a phone is a per-rotation coin flip. The windows the phone
dials are the direction that suffers when its identity-keyed bookkeeping is
stale (see the appendix); the handshake and the liveness-gated merge carry
the recovery. Only spangap-ble's privacy-failure fallback (a non-resolvable
address, top bits 00, below every phone) makes the election always ours. This
straddle's obligations are to re-read `bleOwnAddr()` on `BLE_EV_UP` (the
election input), to tolerate being seen as a stranger each session — the
identity handshake and the liveness-gated merge absorb that on both sides —
and to mark a duplicate session `bleConnKeepAddr()` so its teardown does not
rotate. Rotation protects against a peer's stale per-address state; a peer we
still hold a working link to has none to escape, and rotating for it only
manufactures the next duplicate.

The bring-up machine has a **15 s stage timeout** (`centralStep`). Each stage is
entered by the previous stage's result, so nothing polls; the timeout exists
because a peer that stops answering mid-discovery would otherwise hold a
connection slot forever.

The CCCD is **discovered**, not assumed to be `val_handle + 1`. That relation
holds for a service definition we registered, not for a peer's layout, and the
three characteristics can be discovered in any order.

## 6. Pitfalls

- **`BLEIF_SVCS` must stay `static const`.** `bleGattAdd` records the pointer
  and dereferences it at host bring-up, arbitrarily later.
- **Do not give the v2.2 characteristics an `_ENC` flag.** It would raise a
  pairing dialog on a phone that is trying to join a mesh, and Columba does not
  bond.
- **Re-read our address on every `BLE_EV_UP`.** spangap-ble generates a fresh
  resolvable private address at every host start, after every disconnect
  and at latest every 15 minutes idle, and fires `BLE_EV_UP` again each time.
  The election is deterministic between rotations because the address is
  stable between rotations; a stale cached copy would have both ends dial at
  once, or neither.
- **A 16-byte inbound write is only a handshake while the peer is unknown.**
  Treating every 16-byte value that way would eat a fragment-sized packet.
- **Reassemble by `seq`/`total`, never by type.** See §3.
- **`ble_gatts_notify_custom` consumes its mbuf on failure too.** A fragment it
  refuses is gone, and `fragSend` then abandons the rest of the packet — which
  is correct, because a half-sent packet is not a packet and RNS is a datagram
  network. Do not "improve" this into a retry that emits the tail of a packet
  whose head never left.
- **One rnsd interface per peer costs one of the global `RNSD_MAX_IFACES`,**
  shared with lora, auto, espnow and every tcp peer. `max_peers` is capped at
  whichever is lower of the protocol's ceiling of 7 and the controller's
  connection table (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS`, 4 on the S3 — internal
  DRAM, not the protocol, is the binding constraint), and defaults to one
  under the table so the RNode door keeps a connection.
- **Rate-limit registration churn.** Every peer come-and-go is a
  register/deregister pair plus a storage batch that lxmf subscribes to. The
  identity merge on address rotation exists to keep that from happening every
  fifteen minutes per Android peer.
- **Nothing but a copy runs in NimBLE host context** — `gattAccess`, the
  `ble_gattc_*` completions and `onNotifyRx` allocate a block, copy, post, and
  return.

## 7. Open, and worth resolving with hardware

- Whether BlueZ truncates or rejects the three-bytes-too-large fragments the
  Linux implementation emits at a high MTU, or whether its own accounting hides
  the bug. It decides whether we must clamp the negotiated MTU when talking to a
  Linux peer.
- The measured bitrate, and whether rnsd's default 2 % announce cap is sane on a
  link this fast.
- Whether to set `rx_signal = 1` and prefix inbound frames with per-packet RSSI.
  The mechanism exists in rnsd, but NimBLE has no per-packet RSSI without
  polling `ble_gap_conn_rssi`, so it would be a connection-level figure wearing
  a packet-level label.

## Appendix: the reconnect flap, and how it was killed

The field symptom, against Columba on Android: connect, register, our replayed
announces go out, **zero inbound**, deliberate hangup (reason 0x13) five
seconds in, reconnect, repeat — sometimes for a bounded minute after a config
change on the phone, sometimes indefinitely until a reboot. The investigation
that ended it surfaced four independent mechanisms, each hiding behind the
previous one. They are recorded here because every one of them is invisible
in isolation, and the next protocol partner will exercise the same seams.

**1. Address-keyed state on the peer, and why phones never see it.** Columba's
bridge keys role flags, MTU maps and dedupe sets by the peer's address, and
merges every new connection into an existing row for that address
(`connectedPeers.getOrPut`). Its disconnect handling has lossy paths — a
"partially disconnected" branch that skips cleanup, a stop path that clears
maps without firing per-peer callbacks, and Android's own `close()` which
drops connections callback-free. Between two phones none of this matters:
every connection arrives from a fresh resolvable private address, so a stale
row is orphaned the moment it forms and no new session ever merges into it.
A fixed adapter address rendezvouses with the same stale rows
forever — the unbounded flap. The cure is to be what a phone is: a fresh
address per connection (spangap-ble rotates per disconnect), which makes
every stale row on the far side unreachable the moment it forms.

The duplicate-session exemption (`bleConnKeepAddr()`) does not reopen this.
The hazard here is a session that *established* and left state behind on a
peer that then lost it; the exemption covers only a session torn down as a
duplicate of a link that is still up, which is a different peer state
entirely — that peer is not lost, it is being talked to. Every session that
ends on its own account still rotates, which is the case this appendix is
about.

**2. Identity-keyed state on the peer — the bounded flap that remains.**
Above the bridge, Columba's Python `BLEInterface` keys by RNS identity, which
never rotates. After a teardown whose disconnect callbacks were lost, its
`_check_duplicate_identity` still maps our identity to the dead session and
judges it alive (a stale entry in its own `peers` dict suffices), so every
new session is killed seconds after the identity handshake — until the entry
ages past the 30 s zombie threshold or its periodic sweeper runs. This is
address-independent: rotation cannot touch it, and it bounds a post-teardown
outage at roughly half a minute. It is Columba's bug to fix; what contains it
on our side is our own dialling pressure plus whichever rotation windows the
election hands us (below).

**3. The dialling direction is not symmetric.** In every observed episode,
sessions the phone dialled died and sessions we dialled came up and carried
traffic — their outbound path is where the stale bookkeeping bites. The
who-dials election compares addresses, and the spec welds semantics to the
top two bits of a random address: non-resolvable 00, phone RPA 01, random
static 11. A non-resolvable address wins the election against every phone,
every rotation; a random static address loses it every time (briefly
shipped, and it removed our dials from the board entirely). The resolvable
address we run sits at 01 like the phones' — chosen by spangap-ble so bonds
survive rotation — so the election is a per-rotation coin flip: half the
windows start with the phone dialling, and recovery leans on the handshake
and the liveness-gated merge.

**4. Our own machinery can be its own denial of service.** A dial's outcome
arrives as a host event, and host events can vanish — a queue overflow in a
scan burst, a connection dead before its conn lookup, a connect whose timeout
event NimBLE never delivers. `s_dialInFlight` gates the scan reconcile, all
dialling and the address rotation, so one lost outcome silently paralysed
all three for hours while the logs showed only the peer misbehaving. The
dial watchdog (spangap-ble) force-fails any dial unresolved past its own
timeout plus margin; nothing gates on a single event arriving anymore.

**5. Rotation's own tax, and the merge policy.** A peer that rotates under a
live link is rediscovered as a stranger and dialled, so a throwaway duplicate
session arrives per rotation window. Both ends must resolve it the same way,
and both must resolve it without rotating — otherwise each teardown mints the
next stranger and the tax becomes a flap at the scan interval rather than one
connection per rotation. The liveness gate in `peerAdoptIdent()` settles which
session wins: a READY session with inbound traffic inside a
keepalive-plus-margin window outranks any newcomer; only a silent session
cedes to a new address. Losing that gate to a newest-wins merge lets the
peer's doomed duplicate evict a healthy link, metronomically.

Steady state after all five: one rnsd registration per peer surviving address
handovers on both sides, duplicate dials bounced within a second, a peer
config change costing ~20 s of bounded churn, and the radio's only waste one
throwaway connection per rotation. Two false leads are worth remembering: a
45 s "go dark" breaker (the peer's cleanup does not need our absence — it
needs its own bug fixed), and mirroring the phones' "15-minute" rotation
interval (their protection is per-connection freshness; 15 minutes is merely
an address's maximum lifetime, and mirroring it left wedges 15 minutes wide).
