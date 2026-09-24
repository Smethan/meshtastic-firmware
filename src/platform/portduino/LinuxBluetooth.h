#pragma once

#include "configuration.h"

// BLE peripheral support for meshtasticd, implemented against BlueZ's D-Bus
// GATT, advertising and agent APIs via sdbus-c++. Compiled only on Linux hosts
// in the native-wdg build. Other Portduino targets stay unchanged even when
// sdbus-c++ happens to be installed on the build host.
#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

#if !__has_include(<sdbus-c++/sdbus-c++.h>)
#error "native-wdg requires the sdbus-c++ development headers"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>

/**
 * Runs the standard Meshtastic BLE service (toRadio/fromRadio/fromNum/logRadio)
 * through bluetoothd. Method names deliberately match
 * NimbleBluetooth/NRF52Bluetooth so the cross-platform call sites
 * (setBluetoothEnable, AdminModule, RedirectablePrint, ...) read the same on
 * every architecture.
 *
 * Note this intentionally does not derive from BluetoothApi: no call site uses
 * that base polymorphically (each platform is reached through its own concrete
 * global pointer), and the base's declared-but-undefined virtuals make its
 * vtable/typeinfo unlinkable in unoptimized RTTI builds such as the native
 * coverage env.
 */
class LinuxBluetooth
{
  public:
    static constexpr uint32_t MAX_PAIRING_WINDOW_SECONDS = 120;

    LinuxBluetooth();
    ~LinuxBluetooth();

    /// Connect to BlueZ, export the GATT application, register agent +
    /// advertisement. Failure (no adapter, D-Bus policy denial, no bluetoothd)
    /// logs and leaves BLE off.
    void setup();
    /// Stop advertising only; an established connection stays up (PowerFSM dark
    /// states).
    void shutdown();
    /// Re-register the advertisement after shutdown().
    void resumeAdvertising();
    /// Suspend advertising while the unrestricted TCP PhoneAPI owns the
    /// process-wide full-client lease. Called only by the cooperative firmware
    /// thread; a connected BLE phone remains connected.
    void setFullClientSuspended(bool suspended);
    /// Suppress advertising for a bounded host-controller scan without
    /// disconnecting an established phone session.
    void setScanSuspended(bool suspended);
    /// Clear the session-only scan suppression and try advertising again.
    void retrySharedAdapter();
    /// Temporarily yield BlueZ's default pairing-agent role to another local
    /// process. An unexpired explicit pairing window is restored on release.
    void setPairingAgentSuspended(bool suspended);

    /// Open an explicit unbonded-phone pairing window. The duration is capped
    /// at MAX_PAIRING_WINDOW_SECONDS and the window also expires internally.
    bool openPairingWindow(uint32_t seconds);
    void closePairingWindow();
    bool isPairingWindowOpen() const;
    bool hasBondedPhone() const;
    /// Return the most recent BlueZ passkey and its monotonic change token.
    /// A false return means BlueZ has not supplied a passkey this process run.
    bool getLatestPasskey(uint32_t &passkey, uint64_t &changeToken) const;

    bool isAdvertising() const;
    /// Full teardown: unregister everything and drop the bus connection.
    void deinit();

    void clearBonds();
    bool isConnected();
    int getRssi();
    /// setup() succeeded and deinit() has not run.
    bool isEnabled();
    void sendLog(const uint8_t *logMessage, size_t length);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
