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
which is what upstream does and what gives correct split horizon and per-peer
announce accounting.

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
non-resolvable private one that rotates at Bluetooth start and after every
disconnect — the per-connection freshness phones get from their radios — so
the election is deterministic while a pairing of addresses stands; and its
top bits sort it below every phone's address, so against a phone this device
is always the one that dials, which is the direction that stays healthy when
the phone's own per-address bookkeeping goes stale.

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
registered in **gateway mode** with MTU **500** and `point_to_point` (so a
forwarded announce is never echoed back down the link it arrived on).
Announces relayed through a phone peer's wider mesh are **not remembered by
default** — the phone is a gateway, and this node looks those destinations up
on demand rather than letting them fill its directory; the switch below flips
that for an isolated Bluetooth-only mesh. The Bluetooth ATT MTU governs what
one fragment carries and never reaches Reticulum.

### Starts automatically

When `iface-ble` is in the build it starts on its own, with the rest of the RNS
ecosystem — there is no init call for a consumer to make.

## Setting it up

1. Put both straddles in the build:
   `spangap build --with spangap/spangap-ble --with reticulous/iface-ble`.
2. Turn Bluetooth on: `set s.ble.enable=1` (or the Bluetooth pane's switch).
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
| `s.ble.rns.max_peers` | `3` | Maximum simultaneous peers, 1–4. Three leaves one of the device's four Bluetooth connections for the RNode door; four is the ceiling with the door off. The protocol itself allows seven, but the ESP32-S3's internal RAM does not carry a larger controller beside Wi-Fi. Live; lowering it does not evict peers already linked. |
| `s.ble.rns.min_rssi` | `-85` | Signal floor in dBm. An advertiser weaker than this is not dialled. Live. |
| `s.ble.rns.scan_interval` | `5` | Discovery interval in seconds; sets the scan duty cycle. Live. |
| `s.ble.rns.retain_announces` | `0` | Remember every announce heard on this interface. Off, destinations relayed through a phone's mesh are resolved on demand (the peer's own announces, path responses, claimed and in-use destinations are always kept); on is for an isolated Bluetooth-only mesh. Changing it drops every peer so they re-register. |
| `s.ble.rns.ifac_netname` | `""` | IFAC network name. Blank at both ends = an open interface. Changing it drops every peer so they re-register. |
| `s.ble.rns.ifac_size` | `0` | IFAC access-code length in bytes; `0` = rnsd's default. |

The keys live in [spangap-ble](../spangap-ble)'s `s.ble.*` namespace so an
operator sees one Bluetooth namespace; the radio itself, the transmit power and
the pairing window belong to that straddle.

### Secrets (persisted, never sent to the browser)

| Key | Meaning |
|---|---|
| `secrets.ble.rns.ifac_netkey` | IFAC passphrase. One code for the whole radio — peers here are discovered, not configured, so there is nowhere to put a per-peer one. |

The device's own RNS identity is read from `secrets.rnsd.identity`, owned by
[rns](../rns); this straddle only takes its 16-byte hash.

### Runtime readouts (ephemeral)

| Key | Meaning |
|---|---|
| `ble.rns.state_text` | `disabled`, `stopped`, `waiting for Bluetooth`, `listening`, `linked` |
| `ble.rns.peers` | Peer count, how many are linked, and each one's name and RSSI, as one finished line |
| `ble.rns.traffic` | Packets in and out, as one finished line |

### Command sentinels

None.

## CLI

| Command | What it does |
|---|---|
| `ble_if` | Status: state, our identity and adapter address, peers against the cap, whether we are advertising and scanning, the signal floor and scan interval, and the bitrate registered with rnsd |
| `ble_if peers` | One line per peer: name, role, stage, MTU, RSSI, fragments in and out |

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
