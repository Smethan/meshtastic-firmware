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
#ifdef PIO_UNIT_TESTING
#include <string>
#include <vector>
#endif

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

    /// Connect to BlueZ and export the GATT application and advertisement. The
    /// default pairing agent is registered only while an explicit pairing
    /// window is open. Failure (no adapter, D-Bus policy denial, no
    /// bluetoothd) logs and leaves BLE off.
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
    /// Return the active pairing passkey and its monotonic change token.
    bool getLatestPasskey(uint32_t &passkey, uint64_t &changeToken) const;

    bool isAdvertising() const;
    /// FIXED_PIN cannot be expressed by BlueZ's Agent1 API for this
    /// DisplayOnly LE peripheral. The backend fails closed instead of silently
    /// substituting a random passkey.
    bool isFixedPinUnsupported() const;
    /// Full teardown: unregister everything and drop the bus connection.
    void deinit();

    bool clearBonds();
    bool isConnected();
    int getRssi();
    /// setup() succeeded and deinit() has not run.
    bool isEnabled();
    /// Update the standard Battery Service when host power telemetry provides
    /// a charge percentage.
    void updateBatteryLevel(uint8_t level);
    void sendLog(const uint8_t *logMessage, size_t length);

#ifdef PIO_UNIT_TESTING
    // Test-only seam around the production callback/state-transition helpers.
    // This keeps native tests independent of a live system bus while ensuring
    // they exercise the same authorization and failure paths used by BlueZ.
    struct TestSnapshot {
        bool ownerLossPending = false;
        bool registrationFailurePending = false;
        bool applicationRegistrationPending = false;
        bool applicationRegistered = false;
        bool unexpectedAgentReleasePending = false;
        bool agentRegistered = false;
        bool agentReleaseExpected = false;
        bool pairingWindowActive = false;
        bool batteryAvailable = false;
        bool draining = false;
        size_t queuedWrites = 0;
        size_t queuedWriteBytes = 0;
        size_t queuedReads = 0;
        size_t queuedReadBytes = 0;
        size_t queuedDisconnects = 0;
        size_t queuedBondRemovals = 0;
        size_t pendingDeviceRemovals = 0;
        size_t serviceAuthorizedDevices = 0;
        size_t connectedDevices = 0;
        bool pairingCandidateServiceAuthorized = false;
        bool pairingWindowRequested = false;
        bool pairingAgentNeeded = false;
        size_t sessionResetCount = 0;
        bool fromNumNotifying = false;
        bool logNotifying = false;
        bool batteryNotifying = false;
        bool identitySavePending = false;
        bool identityClearPending = false;
        std::string adapterId;
        std::string bondedPhoneAddress;
        std::string bondedPhonePath;
        std::string pairingCandidate;
        std::string pendingIdentityAddress;
    };

    enum class TestGattKind { READ, WRITE, NOTIFY, BATTERY };
    enum class TestNotifyKind { FROM_NUM, LOG, BATTERY };

    void testResetState();
    TestSnapshot testSnapshot() const;
    void testSetEnabled(bool enabled);
    void testSetAdapter(const std::string &adapter);
    void testInjectBluezOwnerChange(const std::string &oldOwner, const std::string &newOwner);
    void testBeginApplicationRegistration(uint64_t generation);
    void testCompleteApplicationRegistration(uint64_t generation, bool failed);
    void testSetAgentRegistration(bool registered, bool releaseExpected);
    void testInjectAgentRelease();
    void testInjectAdapterRemoval();
    void testInjectDeviceRemoval(const std::string &path);
    void testSetBondIdentity(const std::string &address, const std::string &path, bool paired, bool connected);
    void testAddDevice(const std::string &address, const std::string &path, bool paired, bool connected);
    void testSetPairingCandidate(const std::string &address, const std::string &path, bool connected);
    bool testClaimPairingDevice(const std::string &path);
    void testSetPaired(const std::string &path, bool paired);
    bool testAuthorizeService(const std::string &path, const std::string &uuid);
    bool testAuthorizedPhonePath(const std::string &path);
    void testSetIdentityPersistenceResult(int result);
    void testSetPairingWindowState(bool requested, bool active, uint32_t durationMsec);
    bool testCommitWrite(const std::vector<uint8_t> &value, bool draining);
    bool testQueueWrite(const std::vector<uint8_t> &value);
    bool testQueueRead(const std::vector<uint8_t> &value);
    bool testSetNotify(TestNotifyKind kind, bool enabled);
    void testAcquirePhoneLease();
    void testCleanupFailedSetup();
    void testProcessOnce();
    static bool testIdentityClearWins(bool savePending, bool clearPending);
    bool testRequestIdentitySave(const std::string &address);
    void testRequestIdentityClear();
    void testSetBatteryAvailable(bool available);
    bool testBatteryServiceWouldBeExported() const;
    static bool testValidReadOffset(size_t offset, size_t valueLength);
    static std::vector<std::string> testGattFlags(TestGattKind kind, bool authenticated);
#endif

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
