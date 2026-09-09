# iface-ble

```
s.ble.rns.enable = 0?    yes → nothing advertises, nothing scans. Stop.
 ↓ no
bleUp() + reserve max_peers connections
 ↓
advertise 37145b00-…-c5da28e3 (no name)  AND  scan for the same UUID
 ↓ a peer answers above the signal floor
our address sorts lower?  no  → they dial us; wait
 ↓ yes
connect → discover → subscribe → read their identity → write ours
 ↓
one rnsd interface per peer, named ble/<8 hex of their identity>
 ↓
fragments both ways; 0x00 to their RX every 15 s of transmit idle
 ↓ the peer's address rotates (Android, ~15 min)
the identity is the key, so the peer and its interface survive it
```

**iface-ble** is an RNS **interface** for [rns](../rns): it carries Reticulum
packets over Bluetooth Low Energy, speaking the **ble-reticulum v2.2** protocol.
Phones running Columba and Linux nodes running ble-reticulum join the mesh
directly — no gateway, no RNode emulation, no TCP in between.

Each peer becomes its **own** rnsd interface rather than everyone sharing one,
which is what upstream does and what keeps point-to-point echo suppression and
per-peer announce accounting correct.

## Origins

The wire protocol is `torlando-tech/ble-reticulum`'s **BLE_PROTOCOL_v2.2**, also
spoken by `torlando-tech/columba` on Android (which runs the reference Python
stack under Chaquopy with a Kotlin radio driver, so Android and Linux are the
same protocol). No upstream code is reused — this is an independent
implementation of that protocol on NimBLE.

## What it does

One FreeRTOS task at priority 1, started and stopped by the RNS lifecycle
orchestrator along with the rest of the ecosystem. The Bluetooth radio itself is
owned by [spangap-ble](../spangap-ble) — a hard dependency — and this straddle
is one of its consumers.

**Both roles at once.** The device advertises the service *and* scans for it.
When two peers see each other, who dials is decided by comparing their adapter
addresses as integers: **the lower address becomes the central**. There is no
negotiation and no retry from the other side. Our own address is a
resolvable private one that rotates at Bluetooth start and after every
disconnect — the per-connection freshness phones get from their radios — so
the election is deterministic while a pairing of addresses stands. The one
teardown that **holds** the address is a duplicate session: a second link to
a peer we are already linked to. Rotating there would hand that same peer's
scanner a stranger to dial, whose session is the next duplicate, at the scan
interval, indefinitely; holding the address instead lets the peer recognise
us by it and stop dialling.
Resolvable is [spangap-ble](../spangap-ble)'s choice, made so *bonded* peers
recognise the device across rotations; Columba does not bond, so this
straddle only pays its price: the address's top bits (01) match every
phone's, and against a phone the dialling side is a per-rotation coin flip.
The windows where the phone dials are the fragile direction when its own
per-address bookkeeping goes stale; the identity handshake and the
liveness-gated merge absorb the recovery.

**Peers are named by identity, reached by address.** Android rotates its
Bluetooth address about every fifteen minutes. The 16-byte RNS identity hash is
the peer's name and the key of the peer table; an address is only how to get
there this quarter of an hour. A rotation therefore costs a reconnection and
nothing else — the peer keeps its rnsd interface, its counters and its place.

The identity is exchanged as a handshake: the central reads the peer's Identity
characteristic, then writes its own 16 bytes to the peer's RX. A peripheral
cannot read from a central, which is why the handshake is a bare 16-byte write
in that direction.

**Fragmentation.** Every packet is fragmented, including one that would fit
whole, with a 5-byte header carrying type, sequence and total. Reassembly gives
up after 30 s. An inbound value that is neither a 16-byte handshake nor a
parseable fragment is ignored in silence — that is where the keepalive byte
goes.

### What you will see in `rns ifaces`

One entry per peer, named `ble/<first four bytes of their identity, hex>`,
registered with the configured interface mode (default **access point**), MTU
**500** and `point_to_point` (so a forwarded announce is never echoed back
down the link it arrived on). The service radius (default 3) bounds what is
served through a peer: announces relayed from a phone's wider mesh arrive
deeper than the radius and are looked up on demand rather than filling this
node's directory. The Bluetooth ATT MTU governs what one fragment carries and
never reaches Reticulum.

### Starts automatically

When `iface-ble` is in the build it starts on its own, with the rest of the RNS
ecosystem — there is no init call for a consumer to make.

## Setting it up

1. Put both straddles in the build:
   `spangap build --with spangap/spangap-ble --with reticulous/iface-ble`.
2. Turn this interface on (below) — that is what turns the Bluetooth radio on.
   The radio has no switch of its own; it runs while something asks for it.
3. Turn the interface on: `set s.ble.rns.enable=1` (Reticulum → Bluetooth).
4. `ble_if` and `ble_if peers` show what it finds.

No pairing and no bonding: the v2.2 service demands neither, so a peer never
raises a pairing dialog. Access control on this link is **IFAC** — a network
name and passphrase both ends must match — applied by rnsd to the traffic, not
by the Bluetooth stack to the attribute.

## Storage variables

### Settings (persisted, `s.`)

| Key | Default | Meaning |
|---|---|---|
| `s.ble.rns.enable` | `0` | Master switch for this interface. Live. |
| `s.ble.rns.txpower` | `9` | Transmit power in dBm for THIS interface's links. spangap-ble takes the loudest of its consumers for advertising and scanning, and puts each claimed connection on its own owner's level. Live. |
| `s.ble.rns.max_peers` | `3` | Maximum simultaneous peers, 1–4. Three leaves one of the device's four Bluetooth connections for the RNode endpoint; four is the ceiling with that off. The protocol itself allows seven, but the ESP32-S3's internal RAM does not carry a larger controller beside Wi-Fi. Live; lowering it does not evict peers already linked. |
| `s.ble.rns.min_rssi` | `-85` | Signal floor in dBm. An advertiser weaker than this is not dialled. Live. |
| `s.ble.rns.scan_interval` | `5` | Discovery interval in seconds; sets the scan duty cycle. Live. |
| `s.ble.rns.announce_interval` | `30` | How often this node says who it is: rnsd replays every hosted destination's announce onto every Bluetooth peer on this beat, jittered ±10 %. Minutes. `0` = never on this interface's own account — the node then says who it is only when an application changes what it advertises, when a peer arrives, or on the pane's **Announce now**. A peer that connects mid-interval is told at once regardless: it registers as its own interface and rnsd replays onto a fresh registration. Live. See [rns/README.md](../rns/README.md), "The announce tick". |
| `s.ble.rns.mode` | `"access_point"` | interface mode applied to every peer's interface: `full`, `gateway`, `access_point`, `roaming`, `boundary`. Changing it drops every peer so they re-register. |
| `s.ble.rns.community_radius` | `3` | Service radius: nodes within this many hops via a Bluetooth peer are served — their announces kept and answered for, searches run on their behalf. The wider mesh a phone relays arrives deeper than the radius, stays out of this node's directory, and is resolved on demand (the peer's own announces, path responses, claimed and in-use destinations are always kept). Changing it drops every peer so they re-register. See `rns/README.md`. |
| `s.ble.rns.ifac_netname` | `""` | IFAC network name. Blank at both ends = an open interface. Changing it drops every peer so they re-register. |
| `s.ble.rns.ifac_size` | `0` | IFAC access-code length in bytes; `0` = rnsd's default. |

The keys live in [spangap-ble](../spangap-ble)'s `s.ble.*` namespace so an
operator sees one Bluetooth namespace; the radio itself, the transmit power and
the pairing window belong to that straddle.

### Secrets (persisted, never sent to the browser)

| Key | Meaning |
|---|---|
| `s.ble.rns.ifac_netkey` | IFAC passphrase. One code for the whole radio — peers here are discovered, not configured, so there is nowhere to put a per-peer one. |

The device's own RNS identity is read from `secrets.rnsd.identity`, owned by
[rns](../rns); this straddle only takes its 16-byte hash.

### Runtime readouts (ephemeral)

| Key | Meaning |
|---|---|
| `ble.rns.state_text` | `disabled`, `stopped`, `waiting for Bluetooth`, `listening`, `linked` |
| `ble.rns.peers` | Peer count, how many are linked, and each one's name and RSSI, as one finished line |
| `ble.rns.traffic` | Packets in and out, as one finished line |
| `rns.pill.ble.*` | The top status line's Bluetooth pill (blue `4d90ff`, order 6, titled "ble (Bluetooth)"), written through rnsd: `B` and how many peers are **attached and carrying RNS** — peers at `BLEIF_ST_READY`, not slots. A slot is claimed the moment a dial starts and lives through six bring-up stages, which is what the connection budget has to reserve against but is not what an operator means by "peers"; counting slots reported three during a scan burst with one attached. Shown while the mesh switch is on and at 0 as readily as at 2 — "advertising and nobody has connected" is the state an operator most needs to see. Published every second regardless of `uiTelemetryWanted()`, unlike the readout lines: it is two deduped keys, and a peer count that lags a disconnect is worse on a status line than no count at all. See [rns/README](../rns/README.md#status-line-pills) |

### Command keys

| Key | Value | Effect |
|---|---|---|
| `ble.rns.announce_now` | any | Ask rnsd to replay every hosted destination's announce onto every Bluetooth peer now — the pane's **Announce now** button. |

## CLI

| Command | What it does |
|---|---|
| `ble_if` | Status: state, our identity and adapter address, peers against the cap, whether we are advertising and scanning, the signal floor and scan interval, and the bitrate registered with rnsd |
| `ble_if peers` | One line per peer: name, role, stage, MTU, RSSI, fragments in and out |
| `ble_if n[eighbors] [-v]` | Direct RNS peers heard over them — which destinations are one hop away, as against `peers`' question of which radios are attached. rnsd's shared neighbourhood table, the format every interface prints; see [rns/README](../rns/README.md#the-neighbourhood--who-is-one-hop-away). |

Each Bluetooth peer is its own point-to-point interface, so the interface **is**
the node: `ble_if n` shows one numbered block per peer with its destinations
under it, not one row per destination. The registration carries the adapter
address as `peer_label`, so a peer that has attached and not yet announced is a
row under its MAC rather than a silence — and the interface registrations
(`ble/<hex>`) stay out of the listing, being this straddle's plumbing rather than
the neighbourhood.

## Interoperating

- **Columba** (Android) — the reference client. Up to 7 peers, an RSSI floor of
  −85 dBm and a 5 s discovery interval, matching the defaults here.
- **ble-reticulum** (Linux, e.g. a Raspberry Pi) — the same protocol through
  BlueZ. Note that its fragmenter sizes payloads from the raw ATT MTU rather
  than from what one attribute value can hold, making its *full* fragments three
  bytes larger than the peer can accept. We emit the safe size and accept up to
  512 inbound, so the asymmetry is its problem and not ours.
- **Another reticulous node** — both ends run this straddle; the address
  comparison decides who dials.

## Read next

- [INTERNALS.md](INTERNALS.md) — protocol v2.2 in full: the fragment header,
  the address-sort election, the identity handshake, the keepalive byte, the
  peer table's merge on rotation, and the payload-sizing discrepancy above.
