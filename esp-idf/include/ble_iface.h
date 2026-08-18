/**
 * ble_iface — RNS over the ble-reticulum v2.2 protocol.
 *
 * Discovers other implementations of the protocol over Bluetooth Low Energy —
 * Columba on Android, ble-reticulum on Linux, another reticulous node — and
 * gives each of them its own rnsd interface, so Reticulum sees a point-to-point
 * link per peer rather than one shared medium.
 *
 * Both roles at once: the device advertises the service and scans for it, and
 * which of two peers dials is decided by comparing their adapter addresses.
 */
#pragma once

#include "service.h"

/** Boot-registered service: onInit() queues the v2.2 GATT service with
 *  spangap-ble, registers the `ble_if` CLI verbs, and registers with the RNS
 *  lifecycle orchestrator. The task is spawned by rnsStart(), not here. */
class BleIfaceService : public Service {
public:
    void onInit() override;
};
