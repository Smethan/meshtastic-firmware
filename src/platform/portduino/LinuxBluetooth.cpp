#include "LinuxBluetooth.h"

#ifdef MESHTASTIC_LINUX_BLE

#include "BluetoothCommon.h"
#include "BluetoothStatus.h"
#include "FSCommon.h"
#include "FullPhoneApiLease.h"
#include "PortduinoGlue.h"
#ifdef MESHTASTIC_WDG_API
#include "WdgPolicy.h"
#endif
#include "PowerFSM.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "SdbusCompat.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "main.h"
#include "mesh/NodeDB.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

/*
  BLE peripheral via bluetoothd, over the org.bluez D-Bus APIs.

  Threading model (a simplification of the one documented at length in
  src/nimble/NimbleBluetooth.cpp): the sdbus-c++ event loop runs in its own
  thread and executes every GATT/agent/signal callback, while ALL PhoneAPI calls
  happen on the main thread inside runOnce(), so the rest of the codebase stays
  effectively single-threaded.

  PHONE -> RADIO: WriteValue (event-loop thread) pushes into fromPhoneQueue and
  wakes the main loop; runOnce() pops and calls handleToRadio().

  RADIO -> PHONE: ReadValue (event-loop thread) parks the D-Bus reply in
  readResult and wakes the main loop; runOnce() first drains fromPhoneQueue
  (clients send a write and immediately read its answer, so writes must land
  first), then calls getFromRadio() and completes the parked reply from the main
  thread. Unlike NimBLE there is no busy-wait: a parked D-Bus method reply is
  exactly the deferred-response primitive that NimBLE's
  onReadCallbackIsWaitingForData flag emulates.

  fromNum/logRadio notifications are BlueZ PropertiesChanged("Value") signals,
  emitted from the main thread; sdbus-c++ serializes access to the connection
  internally.
*/

namespace
{

constexpr const char *kBluezService = "org.bluez";
constexpr const char *kBluezRootPath = "/";
constexpr const char *kBluezManagerPath = "/org/bluez";
constexpr const char *kGattAppPath = "/org/meshtastic/gatt";
constexpr const char *kServicePath = "/org/meshtastic/gatt/service0";
constexpr const char *kToRadioPath = "/org/meshtastic/gatt/service0/char0";
constexpr const char *kFromRadioPath = "/org/meshtastic/gatt/service0/char1";
constexpr const char *kFromNumPath = "/org/meshtastic/gatt/service0/char2";
constexpr const char *kLogRadioPath = "/org/meshtastic/gatt/service0/char3";
constexpr const char *kBatteryServicePath = "/org/meshtastic/gatt/service1";
constexpr const char *kBatteryLevelPath = "/org/meshtastic/gatt/service1/char0";
constexpr const char *kAdvertPath = "/org/meshtastic/advertisement0";
constexpr const char *kAgentPath = "/org/meshtastic/agent";
constexpr const char *kDbusService = "org.freedesktop.DBus";
constexpr const char *kDbusPath = "/org/freedesktop/DBus";

constexpr const char *kIfaceAdapter = "org.bluez.Adapter1";
constexpr const char *kIfaceDevice = "org.bluez.Device1";
constexpr const char *kIfaceGattManager = "org.bluez.GattManager1";
constexpr const char *kIfaceGattService = "org.bluez.GattService1";
constexpr const char *kIfaceGattChar = "org.bluez.GattCharacteristic1";
constexpr const char *kIfaceAdvManager = "org.bluez.LEAdvertisingManager1";
constexpr const char *kIfaceAdvert = "org.bluez.LEAdvertisement1";
constexpr const char *kIfaceAgentManager = "org.bluez.AgentManager1";
constexpr const char *kIfaceAgent = "org.bluez.Agent1";
constexpr const char *kIfaceObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kIfaceProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kIfaceDbus = "org.freedesktop.DBus";

constexpr const char *kBatteryServiceUuid = "0000180f-0000-1000-8000-00805f9b34fb";
constexpr const char *kBatteryLevelUuid = "00002a19-0000-1000-8000-00805f9b34fb";

constexpr size_t kFromPhoneQueueMaxMessages = 64;
constexpr size_t kFromPhoneQueueMaxBytes = 32 * 1024;
constexpr size_t kToPhoneQueueMaxMessages = 128;
constexpr size_t kToPhoneQueueMaxBytes = 64 * 1024;
constexpr uint32_t kPairingPolicyPollMsec = 250;
constexpr uint32_t kBluezRecoveryPollMsec = 250;
constexpr uint32_t kBluezRecoveryInitialMsec = 1000;
constexpr uint32_t kBluezRecoveryMaximumMsec = 30000;
constexpr const char *kPhoneIdentityFile = "/prefs/wdg-phone-address";

using PropertyMap = std::map<std::string, sdbus::Variant>;
using InterfaceMap = std::map<std::string, PropertyMap>;
using ManagedObjects = std::map<sdbus::ObjectPath, InterfaceMap>;

uint16_t offsetOption(const PropertyMap &options)
{
    auto it = options.find("offset");
    return it == options.end() ? 0 : it->second.get<uint16_t>();
}

bool pinPairing()
{
    return config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
}

std::string normalizedAddress(std::string value)
{
    if (value.size() != 17)
        return {};
    for (size_t i = 0; i < value.size(); ++i) {
        if (i % 3 == 2) {
            if (value[i] != ':')
                return {};
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return {};
        } else {
            value[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
        }
    }
    return value;
}

std::string addressFromPath(const std::string &path)
{
    const size_t marker = path.rfind("/dev_");
    if (marker == std::string::npos)
        return {};
    std::string address = path.substr(marker + 5);
    std::replace(address.begin(), address.end(), '_', ':');
    return normalizedAddress(address);
}

std::string addressFromProperties(const PropertyMap &properties, const std::string &path)
{
    auto address = properties.find("Address");
    if (address != properties.end())
        return normalizedAddress(address->second.get<std::string>());
    return addressFromPath(path);
}

std::string normalizedUuid(std::string uuid)
{
    std::transform(uuid.begin(), uuid.end(), uuid.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return uuid;
}

bool isPrimaryMeshtasticService(const std::string &uuid)
{
    return normalizedUuid(uuid) == MESH_SERVICE_UUID;
}

bool isSupportedGattService(const std::string &uuid)
{
    const std::string normalized = normalizedUuid(uuid);
    return normalized == MESH_SERVICE_UUID || normalized == TORADIO_UUID || normalized == FROMRADIO_UUID ||
           normalized == FROMNUM_UUID || normalized == LOGRADIO_UUID || normalized == kBatteryServiceUuid ||
           normalized == kBatteryLevelUuid;
}

std::vector<std::string> gattReadFlags(bool authenticated)
{
    return {authenticated ? "encrypt-authenticated-read" : "encrypt-read", "authorize"};
}

std::vector<std::string> gattWriteFlags(bool authenticated)
{
    return {authenticated ? "encrypt-authenticated-write" : "encrypt-write", "authorize"};
}

std::vector<std::string> gattNotifyFlags(bool authenticated)
{
    std::vector<std::string> flags = gattReadFlags(authenticated);
    // BlueZ 5.66 does not infer a writable CCCD from the encrypted notify
    // variants alone.  Export the base capability as well so clients can
    // subscribe, while the encrypted flag continues to enforce link security.
    flags.push_back("notify");
    flags.push_back(authenticated ? "encrypt-authenticated-notify" : "encrypt-notify");
    return flags;
}

std::vector<std::string> batteryGattFlags()
{
    return {"encrypt-read", "notify", "encrypt-notify", "authorize"};
}

bool validGattReadOffset(size_t offset, size_t valueLength)
{
    return offset <= valueLength;
}

bool shouldExposeBatteryService(bool hostBatteryAvailable)
{
    return hostBatteryAvailable;
}

bool identityClearWins(bool, bool clearPending)
{
    return clearPending;
}

void publishStatus(meshtastic::BluetoothStatus::ConnectionState state)
{
    if (!bluetoothStatus)
        return;
    meshtastic::BluetoothStatus newStatus(state);
    bluetoothStatus->updateStatus(&newStatus);
}

} // namespace

struct LinuxBluetooth::Impl final : public PhoneAPI, public concurrency::OSThread {
    explicit Impl(std::string adapter)
        : concurrency::OSThread("LinuxBluetooth"), adapterPath("/org/bluez/" + adapter), adapterId(std::move(adapter))
    {
        api_type = TYPE_BLE;
    }

    std::string adapterPath;
    std::string adapterId;

    // D-Bus plumbing. Objects/proxies are created on the main thread in setup()
    // and only destroyed in doDeinit() after the event loop has been stopped.
    std::unique_ptr<sdbus::IConnection> conn;
    std::unique_ptr<sdbus::IObject> gattRoot, service, toRadioChar, fromRadioChar, fromNumChar, logRadioChar, batteryService,
        batteryLevelChar, advert, agent;
    std::unique_ptr<sdbus::IProxy> adapterProxy, bluezRootProxy, agentManagerProxy, dbusProxy;

    std::atomic<bool> enabled{false};     // setup() finished successfully
    std::atomic<bool> advertising{false}; // advertisement currently registered
    std::atomic<bool> agentRegistered{false};
    std::atomic<bool> appRegistered{false};
    std::atomic<bool> draining{false}; // deinit in progress: fail reads fast
    std::atomic<bool> advertisingUpdatePending{false};
    std::atomic<bool> appRegistrationPending{false};
    std::atomic<bool> advertisingRegistrationPending{false};
    std::atomic<bool> registrationFailurePending{false};
    std::atomic<bool> advertisementReleaseExpected{false};
    std::atomic<bool> agentReleaseExpected{false};
    std::atomic<bool> unexpectedAgentReleasePending{false};
    std::atomic<uint64_t> busGeneration{0};
    std::atomic<uint64_t> advertisingGeneration{0};
    bool fullClientSuspended = false; // cooperative firmware thread only
    bool scanSuspended = false;       // cooperative firmware thread only
    bool powerSuspended = false;

    std::atomic<bool> bluezLostPending{false};
    bool bluezRecoveryPending = false;
    uint32_t bluezRecoveryStartedMsec = 0;
    uint32_t bluezRecoveryDelayMsec = kBluezRecoveryInitialMsec;
    bool recoveryFullClientSuspended = false;
    bool recoveryScanSuspended = false;
    bool recoveryPowerSuspended = false;
    bool recoveryPairingAgentSuspended = false;
    bool recoveryPairingWindowRequested = false;
    uint32_t recoveryPairingWindowStartedMsec = 0;
    uint32_t recoveryPairingWindowDurationMsec = 0;

    // Pairability is opt-in. The event-loop thread may request closure after a
    // successful bond, but only the cooperative firmware thread calls BlueZ.
    bool pairingWindowRequested = false;
    bool pairingAgentSuspended = false;
    uint32_t pairingWindowStartedMsec = 0;
    uint32_t pairingWindowDurationMsec = 0;
    std::atomic<bool> pairingWindowActive{false};
    std::atomic<uint64_t> pairingWindowAuthorization{0};
    std::atomic<bool> pairingClosePending{false};

    // Connected/known devices, mutated on the event-loop thread, read from the
    // main thread.
    std::mutex devMutex;
    std::set<std::string> physicallyConnectedDevices;
    std::set<std::string> connectedDevices;
    std::set<std::string> pairedDevices;
    std::set<std::string> serviceAuthorizedDevices;
    std::string pairingCandidate;
    bool pairingCandidateServiceAuthorized = false;
    std::string bondedPhoneAddress;
    std::string bondedPhonePath;
    std::map<std::string, std::string> deviceAddresses;
    std::map<std::string, std::unique_ptr<sdbus::IProxy>> deviceProxies;
    struct PendingDeviceAdd {
        std::string address;
        bool paired = false;
        bool connected = false;
    };
    std::map<std::string, PendingDeviceAdd> pendingDeviceAdds;
    std::set<std::string> pendingDeviceRemovals;
    std::atomic<bool> deviceStatePending{false};
    std::atomic<bool> pairingFailureStatusPending{false};
    std::mutex identityMutex;
    std::atomic<bool> phoneIdentitySavePending{false};
    std::atomic<bool> phoneIdentityClearPending{false};
    std::string pendingPhoneIdentityAddress;
    bool phoneIdentityLoaded = false;

    std::mutex policyMutex;
    std::set<std::string> unauthorizedDevices;
    std::set<std::string> incompletePairingDevices;

    // PHONE -> RADIO queue (WriteValue -> handleToRadio)
    std::mutex fromPhoneMutex;
    std::atomic<size_t> fromPhoneQueueSize{0};
    std::atomic<size_t> fromPhoneQueueBytes{0};
    std::deque<std::vector<uint8_t>> fromPhoneQueue;
    // Duplicate-write suppression, event-loop thread only (see NimbleBluetooth's
    // lastToRadio)
    uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE] = {0};
    size_t lastToRadioLen = 0;
    std::mutex duplicateWriteMutex;

    // RADIO -> PHONE parked read (ReadValue reply completed from the main thread)
    std::mutex readMutex;
    bool readPending = false;
    sdbus::Result<std::vector<uint8_t>> readResult;
    std::vector<uint8_t> lastFromRadio; // last packet served at offset 0, for blob-read tails
    // Packets prefetched by the main loop during the config phase, so ReadValue
    // can be answered immediately on the event-loop thread instead of paying a
    // main-loop round trip per packet (NimBLE's preloading; see
    // runOnceToPhoneCanPreloadNextPacket there for why this must not happen in
    // STATE_SEND_PACKETS). Guarded by readMutex.
    std::deque<std::vector<uint8_t>> prefetched;
    size_t prefetchedBytes = 0;
    std::atomic<bool> configQueueOverflowPending{false};

    // Snapshot of getDeviceName() taken on the main thread in doSetup(): the
    // advertisement's LocalName getter runs on the event-loop thread, and
    // getDeviceName() returns a static buffer that is not thread-safe.
    std::string deviceName;

    // Notify state for fromNum and logRadio
    std::atomic<bool> fromNumNotifying{false};
    std::atomic<bool> logNotifying{false};
    std::atomic<bool> batteryNotifying{false};
    std::mutex valueMutex;
    std::vector<uint8_t> fromNumValue{0, 0, 0, 0};
    std::vector<uint8_t> logValue;
    std::vector<uint8_t> batteryValue{0};

    std::atomic<bool> fixedPinWarned{false};
    std::atomic<bool> fixedPinUnsupported{false};
    bool batteryAvailable = false; // cooperative firmware thread only
#ifdef PIO_UNIT_TESTING
    std::atomic<size_t> sessionResetCount{0};
    // -1 exercises the real SafeFile path, 0 injects failure, 1 injects
    // success. Tests use this to prove promotion happens strictly after the
    // cooperative identity commit without writing host preferences.
    int identityPersistenceTestResult = -1;
#endif

    // Pairing-code alert. The agent callbacks run on the event-loop thread, but
    // the screen is owned by the main thread, so the passkey is handed over as a
    // pending flag and drawn from runOnce().
    std::atomic<bool> passkeyShowPending{false};
    std::atomic<bool> passkeyHidePending{false};
    std::atomic<uint32_t> pendingPasskey{0};
    std::atomic<uint64_t> passkeyChangeToken{0};
    std::atomic<bool> passkeyAvailable{false};
    bool passkeyShowing = false; // main thread only

    // ---------------------------------------------------------------- PhoneAPI
    // glue

    void rememberRecoveryPolicy()
    {
        recoveryFullClientSuspended = fullClientSuspended;
        recoveryScanSuspended = scanSuspended;
        recoveryPowerSuspended = powerSuspended;
        recoveryPairingAgentSuspended = pairingAgentSuspended;
        recoveryPairingWindowRequested = pairingWindowRequested;
        recoveryPairingWindowStartedMsec = pairingWindowStartedMsec;
        recoveryPairingWindowDurationMsec = pairingWindowDurationMsec;
    }

    void restoreRecoveryPolicy()
    {
        fullClientSuspended = recoveryFullClientSuspended;
        scanSuspended = recoveryScanSuspended;
        powerSuspended = recoveryPowerSuspended;
        pairingAgentSuspended = recoveryPairingAgentSuspended;
        pairingWindowRequested = recoveryPairingWindowRequested;
        pairingWindowStartedMsec = recoveryPairingWindowStartedMsec;
        pairingWindowDurationMsec = recoveryPairingWindowDurationMsec;
        pairingWindowActive = false;
    }

    void armBluezRecovery(bool resetDelay)
    {
        if (resetDelay)
            bluezRecoveryDelayMsec = kBluezRecoveryInitialMsec;
        bluezRecoveryPending = true;
        bluezRecoveryStartedMsec = Time::getMillis();
        setIntervalFromNow(kBluezRecoveryPollMsec);
    }

    void restartBluezTransport(const char *reason, bool increaseBackoff)
    {
        if (!conn)
            return;
        LOG_WARN("BLE transport reset after %s; phone service will re-register automatically", reason);
        rememberRecoveryPolicy();
        draining = true;
        passkeyHidePending = true;
        updatePasskeyAlert();
        // Registration failures can occur while bluetoothd and the adapter are
        // still alive. Withdraw Pairable and our agent before dropping the bus
        // so another system agent cannot accept a phone during recovery. Keep
        // the original window deadline in rememberRecoveryPolicy().
        closePairingWindowPolicy(false);
        unregisterAgent();
        // closePairingWindowPolicy() revokes an unfinished pairing candidate
        // and queues its Device1 path. Attempt the disconnect while the old
        // BlueZ connection and proxies are still usable; teardownBus() clears
        // the queue by design.
        disconnectUnauthorizedDevices();
        publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        // Complete any parked D-Bus read while its connection is still alive,
        // then clear the old PhoneAPI session. WriteValue checks draining again
        // while holding the queue lock, so it cannot repopulate the queue.
        resetTransportSession();
        teardownBus();
        // Hold the global lease until this transport has fully stopped and its
        // PhoneAPI state is cleared. Otherwise a TCP client could acquire the
        // lease and have this delayed reset close the new session.
        meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, this);
        restoreRecoveryPolicy();
        if (increaseBackoff)
            bluezRecoveryDelayMsec = std::min(kBluezRecoveryMaximumMsec, bluezRecoveryDelayMsec * 2U);
        armBluezRecovery(false);
    }

    void retryBluezSetup()
    {
        if (!bluezRecoveryPending || !Throttle::hasElapsed(bluezRecoveryStartedMsec, bluezRecoveryDelayMsec))
            return;

        // A bus exists while RegisterApplication is completing asynchronously.
        // Do not tear it down or claim recovery success until BlueZ confirms the
        // registration in its callback.
        if (enabled && appRegistrationPending)
            return;
        if (enabled && appRegistered) {
            return;
        }

        restoreRecoveryPolicy();
        doSetup();
        if (enabled) {
            // RegisterApplication is asynchronous. Its callback either clears
            // recovery on success or schedules a reset with increased backoff.
            bluezRecoveryStartedMsec = Time::getMillis();
            return;
        }

        restoreRecoveryPolicy();
        bluezRecoveryStartedMsec = Time::getMillis();
        bluezRecoveryDelayMsec = std::min(kBluezRecoveryMaximumMsec, bluezRecoveryDelayMsec * 2U);
    }

    bool checkIsConnected() override
    {
        std::lock_guard<std::mutex> guard(devMutex);
        return !connectedDevices.empty();
    }

    void onNowHasData(uint32_t fromRadioNum) override
    {
        PhoneAPI::onNowHasData(fromRadioNum);

        {
            std::lock_guard<std::mutex> guard(valueMutex);
            fromNumValue = {static_cast<uint8_t>(fromRadioNum & 0xff), static_cast<uint8_t>((fromRadioNum >> 8) & 0xff),
                            static_cast<uint8_t>((fromRadioNum >> 16) & 0xff), static_cast<uint8_t>((fromRadioNum >> 24) & 0xff)};
        }
        if (fromNumNotifying && fromNumChar && hasAuthorizedPhoneConnection())
            sdbuscompat::emitPropertiesChanged(*fromNumChar, kIfaceGattChar, "Value");
    }

    int32_t runOnce() override
    {
        if (bluezLostPending.exchange(false)) {
            // bluetoothd can release the agent while its bus owner disappears.
            // Owner-loss recovery subsumes that callback and must not reset the
            // newly established transport on the next cooperative iteration.
            unexpectedAgentReleasePending = false;
            restartBluezTransport("bluetoothd owner loss", false);
        } else if (unexpectedAgentReleasePending.exchange(false)) {
            LOG_WARN("BlueZ unexpectedly released the Meshtastic pairing agent; closing the pairing window");
            closePairingWindowPolicy(true);
            disconnectUnauthorizedDevices();
            restartBluezTransport("unexpected pairing-agent release", true);
        } else if (registrationFailurePending.exchange(false)) {
            restartBluezTransport("BlueZ registration failure", true);
        }
        retryBluezSetup();

        if (configQueueOverflowPending.exchange(false)) {
            LOG_ERROR("BLE FromRadio configuration queue overflow; disconnecting the phone for a clean resync");
            {
                std::scoped_lock<std::mutex, std::mutex> guard(devMutex, policyMutex);
                unauthorizedDevices.insert(connectedDevices.begin(), connectedDevices.end());
                for (const auto &path : connectedDevices)
                    serviceAuthorizedDevices.erase(path);
                connectedDevices.clear();
            }
            // Already on the cooperative thread: fail the incomplete config
            // session immediately instead of waiting for a Device1 signal.
            publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
            resetTransportSession();
            meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, this);
            advertisingUpdatePending = true;
        }

        processPendingDeviceTopology();
        if (pairingClosePending.exchange(false) || pairingWindowExpired())
            closePairingWindowPolicy(true);
        commitPairingCandidateIfReady();
        updatePasskeyAlert();
        persistPhoneIdentityIfNeeded();

        if (pairingClosePending.exchange(false))
            closePairingWindowPolicy(true);
        if (deviceStatePending.exchange(false))
            reconcilePhoneConnection();
        if (pairingFailureStatusPending.exchange(false) && !checkIsConnected())
            publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        disconnectUnauthorizedDevices();

        if (advertisingUpdatePending.exchange(false)) {
            if (!shouldAdvertise())
                unregisterAdvertisement();
            else
                registerAdvertisement();
        }

        if (bluezRecoveryPending && appRegistered && (!shouldAdvertise() || advertising)) {
            bluezRecoveryPending = false;
            bluezRecoveryDelayMsec = kBluezRecoveryInitialMsec;
            LOG_INFO("BLE re-registered after bluetoothd became available");
        }

        int32_t nextPolicyCheck = pairingWindowRequested ? kPairingPolicyPollMsec : INT32_MAX;
        if (bluezRecoveryPending)
            nextPolicyCheck = std::min<int32_t>(nextPolicyCheck, kBluezRecoveryPollMsec);

        // Only the current full-client lease holder may touch PhoneAPI state.
        // Ownership itself may be changed by the BlueZ event-loop thread, but
        // all core processing below remains on this cooperative firmware thread.
        if (!meshtastic::portduino::fullPhoneApiLease().isHeldBy(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH,
                                                                 this))
            return nextPolicyCheck;

        // Writes before reads: clients send a ToRadio write and immediately read
        // the response, so the parked read must observe the write's effect.
        drainFromPhoneQueue();
        completeParkedRead();
        refillPrefetch();

        return nextPolicyCheck;
    }

    /// Put the pairing code on screen, or take it back down. Main thread only.
    /// Hide is applied before show so a code that arrives in the same pass as a
    /// stale dismissal still ends up visible.
    void updatePasskeyAlert()
    {
        const bool hide = passkeyHidePending.exchange(false);
        const bool show = passkeyShowPending.exchange(false);
        if (!hide && !show)
            return;
        if (show) {
            const uint32_t passkey = pendingPasskey.load();
            char formatted[8];
            snprintf(formatted, sizeof(formatted), "%06u", passkey);
            LOG_INFO("BLE pairing request: enter passkey %s", formatted);
            powerFSM.trigger(EVENT_BLUETOOTH_PAIR);
            if (bluetoothStatus) {
                meshtastic::BluetoothStatus newStatus{std::string(formatted)};
                bluetoothStatus->updateStatus(&newStatus);
            }
        }
#if HAS_SCREEN
        if (!screen) {
            passkeyShowing = false;
            return;
        }
        if (hide && passkeyShowing && !show) {
            screen->endAlert();
            passkeyShowing = false;
        }
        if (show) {
            const uint32_t passkey = pendingPasskey.load();
            screen->startAlert([passkey](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
                char btPIN[16] = "888888";
                snprintf(btPIN, sizeof(btPIN), "%06u", passkey);
                int x_offset = display->width() / 2;
                int y_offset = display->height() <= 80 ? 0 : 12;
                display->setTextAlignment(TEXT_ALIGN_CENTER);
                display->setFont(FONT_MEDIUM);
                display->drawString(x_offset + x, y_offset + y, "Bluetooth");

                display->setFont(FONT_SMALL);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
                display->drawString(x_offset + x, y_offset + y, "Enter this code");

                display->setFont(FONT_LARGE);
                char pin[8];
                snprintf(pin, sizeof(pin), "%.3s %.3s", btPIN, btPIN + 3);
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
                display->drawString(x_offset + x, y_offset + y, pin);

                display->setFont(FONT_SMALL);
                char deviceName[64];
                snprintf(deviceName, sizeof(deviceName), "Name: %s", getDeviceName());
                y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
                display->drawString(x_offset + x, y_offset + y, deviceName);
            });
            passkeyShowing = true;
        }
#endif
    }

    /// Called from the event-loop thread when the pairing code should come down
    /// (pairing finished, canceled, or the peer went away).
    void dismissPasskey()
    {
        passkeyHidePending = true;
        wakeMainLoop();
    }

    void refillPrefetch()
    {
        // Only outside STATE_SEND_PACKETS: during config the client will definitely
        // read every packet (and re-reads nothing on reconnect), while in
        // STATE_SEND_PACKETS a packet fetched early would be lost if the phone
        // disconnects before reading it.
        while (PhoneAPI::isConnected() && !isSendingPackets()) {
            {
                std::lock_guard<std::mutex> guard(readMutex);
                if (prefetched.size() >= kToPhoneQueueMaxMessages ||
                    prefetchedBytes + MAX_TO_FROM_RADIO_SIZE > kToPhoneQueueMaxBytes)
                    return;
            }
            // LOCK ORDER: getFromRadio() may emit a fromNum notify (a D-Bus call), so
            // it must not run under readMutex (see completeParkedRead).
            uint8_t buf[meshtastic_FromRadio_size] = {0};
            size_t numBytes = getFromRadio(buf);
            if (numBytes == 0)
                return;
            std::lock_guard<std::mutex> guard(readMutex);
            if (prefetched.size() >= kToPhoneQueueMaxMessages || prefetchedBytes + numBytes > kToPhoneQueueMaxBytes) {
                // getFromRadio() destructively advances the configuration stream.
                // Continuing after this point would give the phone an incomplete
                // configuration, so fail the session and require a fresh sync.
                configQueueOverflowPending = true;
                wakeMainLoop();
                return;
            }
            prefetched.emplace_back(buf, buf + numBytes);
            prefetchedBytes += numBytes;
        }
    }

    void drainFromPhoneQueue()
    {
        while (fromPhoneQueueSize > 0) {
            std::vector<uint8_t> packet;
            {
                std::lock_guard<std::mutex> guard(fromPhoneMutex);
                if (fromPhoneQueue.empty()) {
                    fromPhoneQueueSize = 0;
                    fromPhoneQueueBytes = 0;
                    break;
                }
                packet = std::move(fromPhoneQueue.front());
                fromPhoneQueue.pop_front();
                fromPhoneQueueSize = fromPhoneQueue.size();
                fromPhoneQueueBytes -= packet.size();
            }
            handleToRadio(packet.data(), packet.size());
        }
    }

    void completeParkedRead()
    {
        {
            // A prefetched packet must be served before anything newly fetched, or
            // the stream reorders.
            std::unique_lock<std::mutex> lk(readMutex);
            if (!readPending)
                return;
            if (!prefetched.empty()) {
                std::vector<uint8_t> packet = std::move(prefetched.front());
                prefetched.pop_front();
                prefetchedBytes -= packet.size();
                lastFromRadio = packet;
                auto result = std::move(readResult);
                readPending = false;
                lk.unlock();
                result.returnResults(packet);
                return;
            }
        }

        // LOCK ORDER: getFromRadio() can emit a fromNum notify (a D-Bus call), and
        // the event-loop thread takes readMutex inside its dispatch lock, so
        // calling into sdbus while holding readMutex would deadlock. Fetch the
        // packet unlocked, then re-take the lock only to hand it to the parked
        // reply.
        uint8_t buf[meshtastic_FromRadio_size] = {0};
        size_t numBytes = getFromRadio(buf);

        std::unique_lock<std::mutex> lk(readMutex);
        if (!readPending) {
            // A disconnect raced us and already failed the read; the packet is lost,
            // which a disconnect implies anyway (PhoneAPI session state gets reset).
            return;
        }
        lastFromRadio.assign(buf, buf + numBytes);
        auto result = std::move(readResult);
        readPending = false;
        lk.unlock();

        // A zero-length reply is correct here: any pending write has already been
        // handled, and in STATE_SEND_PACKETS clients poll-read until they get 0
        // bytes.
        result.returnResults(std::vector<uint8_t>(buf, buf + numBytes));
    }

    // ------------------------------------------------- event-loop thread
    // callbacks

    bool isBondedPhoneLocked(const std::string &path) const
    {
        const auto knownAddress = deviceAddresses.find(path);
        const std::string address =
            knownAddress == deviceAddresses.end() || knownAddress->second.empty() ? addressFromPath(path) : knownAddress->second;
        return belongsToAdapter(path) && !bondedPhoneAddress.empty() && address == bondedPhoneAddress &&
               pairedDevices.count(path) != 0;
    }

    bool authorizedPhonePath(const std::string &path)
    {
        std::lock_guard<std::mutex> guard(devMutex);
        return isBondedPhoneLocked(path) && path == bondedPhonePath && connectedDevices.count(path) != 0;
    }

    bool authorizedPhoneRequest(const PropertyMap &options)
    {
        auto device = options.find("device");
        if (device == options.end())
            return false;
        const std::string path = device->second.get<sdbus::ObjectPath>();
        return authorizedPhonePath(path);
    }

    bool hasAuthorizedPhoneConnection()
    {
        std::set<std::string> foreignConnections;
        bool authorized = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            authorized = connectedDevices.size() == 1 && !bondedPhonePath.empty() &&
                         connectedDevices.count(bondedPhonePath) != 0 && isBondedPhoneLocked(bondedPhonePath);
            for (const auto &path : physicallyConnectedDevices) {
                const bool storedPhone = path == bondedPhonePath && isBondedPhoneLocked(path);
                const bool activeCandidate = path == pairingCandidate && pairingWindowAcceptingCallbacks();
                if (!storedPhone && !activeCandidate)
                    foreignConnections.insert(path);
            }
            // InterfacesAdded and StartNotify can be delivered back-to-back on
            // the D-Bus thread before runOnce() promotes the topology record.
            // Include connected additions here so that callback ordering cannot
            // create a brief subscription window for an unknown peer.
            for (const auto &[path, device] : pendingDeviceAdds) {
                if (!device.connected)
                    continue;
                const bool storedPhone = path == bondedPhonePath && device.paired && !bondedPhoneAddress.empty() &&
                                         device.address == bondedPhoneAddress;
                const bool activeCandidate = path == pairingCandidate && pairingWindowAcceptingCallbacks();
                if (!storedPhone && !activeCandidate)
                    foreignConnections.insert(path);
            }
        }

        // BlueZ 5.66 does not identify the device behind StartNotify and
        // broadcasts Value changes to every remote with its CCC enabled. Fail
        // closed while any unrelated peer is physically connected, and let the
        // cooperative thread perform the D-Bus disconnect outside this callback.
        if (!foreignConnections.empty()) {
            {
                std::lock_guard<std::mutex> guard(policyMutex);
                unauthorizedDevices.insert(foreignConnections.begin(), foreignConnections.end());
            }
            wakeMainLoop();
            return false;
        }
        return authorized;
    }

    enum class NotifyKind { FROM_NUM, LOG, BATTERY };

    bool setNotifyState(NotifyKind kind, bool notifying)
    {
        if (notifying && !hasAuthorizedPhoneConnection())
            return false;
        switch (kind) {
        case NotifyKind::FROM_NUM:
            fromNumNotifying = notifying;
            break;
        case NotifyKind::LOG:
            logNotifying = notifying;
            break;
        case NotifyKind::BATTERY:
            batteryNotifying = notifying;
            break;
        }
        return true;
    }

    void requireNotifyState(NotifyKind kind, bool notifying)
    {
        if (!setNotifyState(kind, notifying))
            throw sdbuscompat::dbusError("org.bluez.Error.NotAuthorized",
                                         "notification subscription is not from the Meshtastic phone");
    }

    enum class QueueCommitResult { ACCEPTED, DUPLICATE, FULL, DRAINING };

    QueueCommitResult commitToRadioWrite(const std::vector<uint8_t> &value)
    {
        // This lock is the commit boundary shared with resetTransportSession().
        // Once teardown sets draining, a callback that was already in flight
        // can never refill the queue after it has been cleared.
        std::lock_guard<std::mutex> queueGuard(fromPhoneMutex);
        if (draining)
            return QueueCommitResult::DRAINING;
        if (fromPhoneQueueSize >= kFromPhoneQueueMaxMessages || fromPhoneQueueBytes + value.size() > kFromPhoneQueueMaxBytes)
            return QueueCommitResult::FULL;

        std::lock_guard<std::mutex> duplicateGuard(duplicateWriteMutex);
        if (value.size() == lastToRadioLen && memcmp(lastToRadio, value.data(), value.size()) == 0)
            return QueueCommitResult::DUPLICATE;

        fromPhoneQueue.push_back(value);
        fromPhoneQueueSize = fromPhoneQueue.size();
        fromPhoneQueueBytes += value.size();
        memcpy(lastToRadio, value.data(), value.size());
        lastToRadioLen = value.size();
        return QueueCommitResult::ACCEPTED;
    }

    void onToRadioWrite(std::vector<uint8_t> value, const PropertyMap &options)
    {
        if (draining)
            throw sdbuscompat::dbusError("org.bluez.Error.NotPermitted", "BLE transport is restarting");
        if (!authorizedPhoneRequest(options))
            throw sdbuscompat::dbusError("org.bluez.Error.NotAuthorized", "request is not from the Meshtastic phone");
        if (!meshtastic::portduino::fullPhoneApiLease().isHeldBy(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH,
                                                                 this))
            throw sdbuscompat::dbusError("org.bluez.Error.NotPermitted", "BLE does not own the PhoneAPI lease");
        if (offsetOption(options) != 0)
            throw sdbuscompat::dbusError("org.bluez.Error.NotSupported", "offset writes not supported");
        if (value.empty() || value.size() > MAX_TO_FROM_RADIO_SIZE)
            throw sdbuscompat::dbusError("org.bluez.Error.InvalidValueLength", "bad ToRadio length");

        switch (commitToRadioWrite(value)) {
        case QueueCommitResult::DUPLICATE:
            LOG_DEBUG("BLE drop duplicate ToRadio packet (%u bytes)", (unsigned)value.size());
            return;
        case QueueCommitResult::FULL:
            // Push back on the client rather than dropping silently; it will retry.
            throw sdbuscompat::dbusError("org.bluez.Error.InProgress", "ToRadio queue full");
        case QueueCommitResult::DRAINING:
            throw sdbuscompat::dbusError("org.bluez.Error.NotPermitted", "BLE transport is restarting");
        case QueueCommitResult::ACCEPTED:
            break;
        }
        wakeMainLoop();
    }

    void onFromRadioRead(sdbus::Result<std::vector<uint8_t>> &&result, const PropertyMap &options)
    {
        if (!authorizedPhoneRequest(options)) {
            result.returnError(
                sdbuscompat::dbusError("org.bluez.Error.NotAuthorized", "request is not from the Meshtastic phone"));
            return;
        }
        if (!meshtastic::portduino::fullPhoneApiLease().isHeldBy(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH,
                                                                 this)) {
            result.returnError(sdbuscompat::dbusError("org.bluez.Error.NotPermitted", "BLE does not own the PhoneAPI lease"));
            return;
        }
        uint16_t offset = offsetOption(options);

        if (draining) {
            result.returnResults(std::vector<uint8_t>());
            return;
        }

        std::unique_lock<std::mutex> lk(readMutex);
        if (offset > 0) {
            // Blob-read continuation of the packet we served at offset 0; do not
            // consume a new packet from PhoneAPI.
            if (!validGattReadOffset(offset, lastFromRadio.size())) {
                lk.unlock();
                result.returnError(sdbuscompat::dbusError("org.bluez.Error.InvalidOffset", "read offset exceeds value"));
                return;
            }
            std::vector<uint8_t> tail;
            if (offset < lastFromRadio.size())
                tail.assign(lastFromRadio.begin() + offset, lastFromRadio.end());
            lk.unlock();
            result.returnResults(tail);
            return;
        }
        if (readPending) {
            // Shouldn't happen (ATT serializes reads), but never strand a reply.
            auto stale = std::move(readResult);
            readPending = false;
            stale.returnError(sdbuscompat::dbusError("org.bluez.Error.Failed", "superseded by a newer read"));
        }
        if (!prefetched.empty() && fromPhoneQueueSize == 0) {
            // Answer straight from the config-phase prefetch queue - no main-loop
            // round trip. Skipped when a write is still queued, so a
            // write-then-read client never reads past its own write.
            std::vector<uint8_t> packet = std::move(prefetched.front());
            prefetched.pop_front();
            prefetchedBytes -= packet.size();
            lastFromRadio = packet;
            lk.unlock();
            result.returnResults(packet);
            wakeMainLoop(); // top the prefetch queue back up
            return;
        }
        readResult = std::move(result);
        readPending = true;
        lk.unlock();
        wakeMainLoop();
    }

    std::vector<uint8_t> onFromNumRead(const PropertyMap &options)
    {
        if (!authorizedPhoneRequest(options))
            throw sdbuscompat::dbusError("org.bluez.Error.NotAuthorized", "request is not from the Meshtastic phone");
        std::lock_guard<std::mutex> guard(valueMutex);
        return fromNumValue;
    }

    std::vector<uint8_t> onLogRadioRead(const PropertyMap &options)
    {
        if (!authorizedPhoneRequest(options))
            throw sdbuscompat::dbusError("org.bluez.Error.NotAuthorized", "request is not from the Meshtastic phone");
        std::lock_guard<std::mutex> guard(valueMutex);
        return logValue;
    }

    std::vector<uint8_t> onBatteryLevelRead(const PropertyMap &options)
    {
        if (!batteryAvailable)
            throw sdbuscompat::dbusError("org.bluez.Error.NotSupported", "host battery is unavailable");
        if (!authorizedPhoneRequest(options))
            throw sdbuscompat::dbusError("org.bluez.Error.NotAuthorized", "request is not from the Meshtastic phone");
        std::lock_guard<std::mutex> guard(valueMutex);
        return batteryValue;
    }

    void wakeMainLoop()
    {
        setIntervalFromNow(0);
        concurrency::mainDelay.interrupt();
    }

    void loadPhoneIdentity()
    {
        if (phoneIdentityLoaded)
            return;
        phoneIdentityLoaded = true;

        std::string stored;
        {
            concurrency::LockGuard guard(spiLock);
            auto file = FSCom.open(kPhoneIdentityFile, FILE_O_READ);
            if (file) {
                int value;
                while ((value = file.read()) >= 0 && stored.size() < 32)
                    stored.push_back(static_cast<char>(value));
                file.close();
            }
        }
        while (!stored.empty() && std::isspace(static_cast<unsigned char>(stored.back())))
            stored.pop_back();
        stored = normalizedAddress(stored);
        if (!stored.empty()) {
            std::lock_guard<std::mutex> guard(devMutex);
            bondedPhoneAddress = stored;
            LOG_INFO("BLE loaded Meshtastic phone identity %s", stored.c_str());
        }
    }

    bool writePhoneIdentity(const std::string &address)
    {
#ifdef PIO_UNIT_TESTING
        if (identityPersistenceTestResult >= 0)
            return identityPersistenceTestResult != 0;
#endif
        {
            concurrency::LockGuard guard(spiLock);
            FSCom.mkdir("/prefs");
        }
        SafeFile file(kPhoneIdentityFile, true);
        const size_t expected = address.size() + 1;
        const size_t written = file.write(reinterpret_cast<const uint8_t *>(address.c_str()), address.size()) + file.write('\n');
        return written == expected && file.close();
    }

    bool clearPhoneIdentityFile()
    {
#ifdef PIO_UNIT_TESTING
        if (identityPersistenceTestResult >= 0)
            return identityPersistenceTestResult != 0;
#endif
        concurrency::LockGuard guard(spiLock);
        return !FSCom.exists(kPhoneIdentityFile) || FSCom.remove(kPhoneIdentityFile);
    }

    bool persistPhoneIdentityIfNeeded()
    {
        bool saveRequested;
        bool clearRequested;
        std::string address;
        {
            std::lock_guard<std::mutex> guard(identityMutex);
            saveRequested = phoneIdentitySavePending.exchange(false);
            clearRequested = phoneIdentityClearPending.exchange(false);
            if (identityClearWins(saveRequested, clearRequested)) {
                phoneIdentitySavePending = false;
                pendingPhoneIdentityAddress.clear();
            } else if (saveRequested) {
                address = std::move(pendingPhoneIdentityAddress);
                pendingPhoneIdentityAddress.clear();
            }
        }
        if (!saveRequested && !clearRequested)
            return true;

        if (identityClearWins(saveRequested, clearRequested)) {
            // Forget is authoritative. A bond-complete callback can race the
            // explicit clear request, but it must never recreate the identity
            // file after the user asked to remove it.
            const bool cleared = clearPhoneIdentityFile();
            if (!cleared) {
                LOG_ERROR("BLE could not clear the Meshtastic phone identity; will retry");
                std::lock_guard<std::mutex> guard(identityMutex);
                phoneIdentitySavePending = false;
                phoneIdentityClearPending = true;
                pendingPhoneIdentityAddress.clear();
                return false;
            }
            {
                std::lock_guard<std::mutex> guard(devMutex);
                serviceAuthorizedDevices.erase(bondedPhonePath);
                bondedPhoneAddress.clear();
                bondedPhonePath.clear();
            }
            deviceStatePending = true;
            return true;
        }

        if (address.empty())
            return true;

        if (!writePhoneIdentity(address)) {
            LOG_ERROR("BLE could not persist the Meshtastic phone identity; will retry");
            {
                std::lock_guard<std::mutex> guard(identityMutex);
                if (!phoneIdentityClearPending.load()) {
                    pendingPhoneIdentityAddress = address;
                    phoneIdentitySavePending = true;
                }
            }
            return false;
        }
        LOG_INFO("BLE saved Meshtastic phone identity %s", address.c_str());
        return true;
    }

    bool requestPhoneIdentitySave(const std::string &address)
    {
        std::lock_guard<std::mutex> guard(identityMutex);
        if (phoneIdentityClearPending.load() || address.empty())
            return false;
        pendingPhoneIdentityAddress = address;
        phoneIdentitySavePending = true;
        return true;
    }

    void requestPhoneIdentityClear()
    {
        std::lock_guard<std::mutex> guard(identityMutex);
        phoneIdentitySavePending = false;
        phoneIdentityClearPending = true;
        pendingPhoneIdentityAddress.clear();
    }

    void failPairingCandidate(const std::string &path, bool removeBond)
    {
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (pairingCandidate == path) {
                pairingCandidate.clear();
                pairingCandidateServiceAuthorized = false;
            }
            serviceAuthorizedDevices.erase(path);
        }
        rejectDevice(path, removeBond);
        pairingFailureStatusPending = true;
        pairingClosePending = true;
        passkeyAvailable = false;
        dismissPasskey();
        deviceStatePending = true;
    }

    void commitPairingCandidateIfReady()
    {
        std::string path;
        std::string address;
        std::string expiredPath;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (!pairingWindowAcceptingCallbacks()) {
                if (pairingCandidate.empty())
                    return;
                expiredPath = pairingCandidate;
            } else if (pairingCandidate.empty() || !pairingCandidateServiceAuthorized ||
                       pairedDevices.count(pairingCandidate) == 0 || phoneIdentityClearPending.load()) {
                return;
            }
            path = expiredPath.empty() ? pairingCandidate : expiredPath;
            const auto knownAddress = deviceAddresses.find(path);
            address = knownAddress == deviceAddresses.end() || knownAddress->second.empty() ? addressFromPath(path)
                                                                                            : knownAddress->second;
        }

        if (!expiredPath.empty()) {
            failPairingCandidate(expiredPath, true);
            return;
        }

        if (address.empty() || !writePhoneIdentity(address)) {
            LOG_ERROR("BLE could not commit the new phone identity; rejecting the incomplete bond");
            failPairingCandidate(path, true);
            return;
        }

        bool promoted = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            const auto knownAddress = deviceAddresses.find(path);
            const std::string currentAddress = knownAddress == deviceAddresses.end() || knownAddress->second.empty()
                                                   ? addressFromPath(path)
                                                   : knownAddress->second;
            if (pairingWindowAcceptingCallbacks() && pairingCandidate == path && pairingCandidateServiceAuthorized &&
                pairedDevices.count(path) != 0 && currentAddress == address && !phoneIdentityClearPending.load()) {
                bondedPhoneAddress = address;
                bondedPhonePath = path;
                serviceAuthorizedDevices.insert(path);
                pairingCandidate.clear();
                pairingCandidateServiceAuthorized = false;
                promoted = true;
            }
        }

        if (!promoted) {
            // A Paired=false/removal event raced the verified write. The new
            // identity must not survive a candidate that was never promoted.
            if (!clearPhoneIdentityFile()) {
                LOG_ERROR("BLE could not roll back a canceled phone identity; retrying the clear before any phone is authorized");
                requestPhoneIdentityClear();
            }
            failPairingCandidate(path, true);
            return;
        }

        LOG_INFO("BLE committed Meshtastic phone identity %s", address.c_str());
        passkeyAvailable = false;
        dismissPasskey();
        pairingClosePending = true;
        deviceStatePending = true;
    }

    void reconcilePhoneConnection()
    {
        bool wasConnected;
        bool nowConnected;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            wasConnected = !connectedDevices.empty();
            connectedDevices.clear();
            if (!bondedPhonePath.empty() && isBondedPhoneLocked(bondedPhonePath) &&
                physicallyConnectedDevices.count(bondedPhonePath) != 0 && serviceAuthorizedDevices.count(bondedPhonePath) != 0)
                connectedDevices.insert(bondedPhonePath);
            nowConnected = !connectedDevices.empty();
        }

        if (!wasConnected && nowConnected) {
            // The identity is already durably committed at this point. Lease
            // ownership and firmware observers are cooperative-thread work.
            activatePhoneConnection();
        } else if (wasConnected && !nowConnected) {
            resetTransportSession();
            meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, this);
            publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
            advertisingUpdatePending = true;
        }
    }

    // ------------------------------------------------------------ device
    // tracking

    void activatePhoneConnection()
    {
        meshtastic::portduino::fullPhoneApiLease().acquireBluetooth(this);
        advertisingUpdatePending = true;
        publishStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        wakeMainLoop();
    }

    bool claimPairingDevice(const std::string &path)
    {
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (!pairingWindowAcceptingCallbacks() || !belongsToAdapter(path) || !bondedPhoneAddress.empty() ||
                phoneIdentityClearPending.load())
                return false;
            if (!pairingCandidate.empty() && pairingCandidate != path)
                return false;
            auto address = deviceAddresses.find(path);
            if ((address == deviceAddresses.end() || address->second.empty()) && addressFromPath(path).empty())
                return false;
            pairingCandidate = path;
        }
        return true;
    }

    void requirePairingDevice(const std::string &path)
    {
        if (!claimPairingDevice(path))
            throw sdbuscompat::dbusError("org.bluez.Error.Rejected", "pairing window is not open for this device");
    }

    void requirePairingOrBondedDevice(const std::string &path)
    {
        bool bonded = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            bonded = isBondedPhoneLocked(path);
            if (bonded)
                bondedPhonePath = path;
        }
        if (!bonded)
            requirePairingDevice(path);
    }

    void rejectDevice(const std::string &path, bool removeBond)
    {
        {
            std::lock_guard<std::mutex> guard(policyMutex);
            unauthorizedDevices.insert(path);
            if (removeBond)
                incompletePairingDevices.insert(path);
        }
        wakeMainLoop();
    }

    bool authorizeMeshtasticService(const std::string &path, const std::string &uuid)
    {
        if (!isSupportedGattService(uuid)) {
            bool candidate = false;
            {
                std::lock_guard<std::mutex> guard(devMutex);
                candidate = pairingCandidate == path;
            }
            if (candidate)
                failPairingCandidate(path, true);
            else
                rejectDevice(path, false);
            return false;
        }

        bool accepted = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            const bool paired = pairedDevices.count(path) != 0;
            const bool storedPhone = paired && isBondedPhoneLocked(path);
            const bool pendingPhone = pairingWindowAcceptingCallbacks() && pairingCandidate == path;
            if (storedPhone) {
                // Auxiliary services, including Battery, may only follow an
                // identity that was already committed by the cooperative
                // firmware thread. They can never establish a phone identity.
                serviceAuthorizedDevices.insert(path);
                accepted = true;
            } else if (pendingPhone && isPrimaryMeshtasticService(uuid)) {
                // A new phone is merely a candidate here. AuthorizeService is a
                // D-Bus callback, so it records the primary-service fact and
                // returns. runOnce() persists the identity before promotion.
                pairingCandidateServiceAuthorized = true;
                accepted = true;
            }
        }

        if (!accepted) {
            bool candidate = false;
            {
                std::lock_guard<std::mutex> guard(devMutex);
                candidate = pairingCandidate == path;
            }
            if (candidate)
                failPairingCandidate(path, true);
            else
                rejectDevice(path, false);
            return false;
        }
        deviceStatePending = true;
        wakeMainLoop();
        return true;
    }

    void onDevicePairedChanged(const std::string &path, bool pairedNow)
    {
        bool clearIdentity = false;
        bool rejectIncomplete = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (pairedNow) {
                pairedDevices.insert(path);
                const auto knownAddress = deviceAddresses.find(path);
                const std::string address = knownAddress == deviceAddresses.end() ? addressFromPath(path) : knownAddress->second;
                if (!bondedPhoneAddress.empty() && address == bondedPhoneAddress) {
                    bondedPhonePath = path;
                    serviceAuthorizedDevices.insert(path);
                }
            } else {
                pairedDevices.erase(path);
                serviceAuthorizedDevices.erase(path);
                if (path == bondedPhonePath) {
                    clearIdentity = true;
                }
                if (pairingCandidate == path)
                    rejectIncomplete = true;
            }
        }
        if (clearIdentity)
            requestPhoneIdentityClear();
        if (rejectIncomplete)
            failPairingCandidate(path, true);
        deviceStatePending = true;
        wakeMainLoop();
    }

    void trackDevice(const std::string &path, const std::string &address, bool pairedNow)
    {
        // LOCK ORDER: devMutex must stay a leaf on the main thread (the event-loop
        // thread takes it inside its dispatch lock), so the proxy - a D-Bus
        // operation - is created outside the lock.
        {
            std::lock_guard<std::mutex> guard(devMutex);
            deviceAddresses[path] = address;
            if (pairedNow)
                pairedDevices.insert(path);
            else
                pairedDevices.erase(path);
            if (pairedNow && !bondedPhoneAddress.empty() && address == bondedPhoneAddress) {
                bondedPhonePath = path;
                // The durable identity and BlueZ bond are the reconnect
                // authorization. No default Agent1 registration is needed (or
                // permitted) outside an explicit pairing window.
                serviceAuthorizedDevices.insert(path);
            }
            if (deviceProxies.count(path) != 0)
                return;
        }
        auto proxy = sdbuscompat::makeProxy(*conn, kBluezService, path);
        proxy->uponSignal("PropertiesChanged")
            .onInterface(kIfaceProperties)
            .call([this, path](const std::string &iface, const PropertyMap &changed, const std::vector<std::string> &) {
                if (iface != kIfaceDevice)
                    return;
                auto paired = changed.find("Paired");
                if (paired != changed.end())
                    onDevicePairedChanged(path, paired->second.get<bool>());
                auto it = changed.find("Connected");
                if (it != changed.end())
                    onDeviceConnectedChanged(path, it->second.get<bool>());
            });
        sdbuscompat::finishProxy(*proxy);
        std::lock_guard<std::mutex> guard(devMutex);
        if (deviceProxies.count(path) == 0)
            deviceProxies[path] = std::move(proxy);
    }

    void processPendingDeviceTopology()
    {
        std::map<std::string, PendingDeviceAdd> additions;
        std::set<std::string> removals;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            additions.swap(pendingDeviceAdds);
            removals.swap(pendingDeviceRemovals);
        }

        // New proxies are D-Bus objects. Construct them only on the
        // cooperative firmware thread, never from InterfacesAdded.
        for (const auto &[path, device] : additions) {
            if (removals.count(path) != 0)
                continue;
            trackDevice(path, device.address, device.paired);
            onDeviceConnectedChanged(path, device.connected);
        }

        std::vector<std::unique_ptr<sdbus::IProxy>> retired;
        for (const auto &path : removals) {
            bool abandonedCandidate = false;
            {
                std::lock_guard<std::mutex> guard(devMutex);
                auto proxy = deviceProxies.find(path);
                if (proxy != deviceProxies.end()) {
                    retired.push_back(std::move(proxy->second));
                    deviceProxies.erase(proxy);
                }
                pendingDeviceAdds.erase(path);
                physicallyConnectedDevices.erase(path);
                pairedDevices.erase(path);
                serviceAuthorizedDevices.erase(path);
                deviceAddresses.erase(path);
                // InterfacesRemoved also occurs during bluetoothd/controller
                // teardown and cache churn.  Retire this transient object path
                // without forgetting the durable bonded address; Paired=false
                // and clearBonds() are the explicit identity-removal paths.
                if (bondedPhonePath == path)
                    bondedPhonePath.clear();
                if (pairingCandidate == path)
                    abandonedCandidate = true;
            }
            if (abandonedCandidate)
                failPairingCandidate(path, true);
        }
        // Destructors can call into sdbus-c++. They run here, outside devMutex
        // and outside BlueZ's event-loop callback stack.
        retired.clear();
    }

    void onDeviceConnectedChanged(const std::string &path, bool connected)
    {
        bool abandonedCandidate = false;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (connected) {
                physicallyConnectedDevices.insert(path);
            } else {
                physicallyConnectedDevices.erase(path);
                if (pairingCandidate == path)
                    abandonedCandidate = bondedPhonePath != path;
            }
        }
        if (abandonedCandidate)
            failPairingCandidate(path, true);
        deviceStatePending = true;
        wakeMainLoop();
    }

    void failParkedRead()
    {
        std::unique_lock<std::mutex> lk(readMutex);
        if (!readPending)
            return;
        auto result = std::move(readResult);
        readPending = false;
        lk.unlock();
        result.returnResults(std::vector<uint8_t>());
    }

    void resetTransportSession()
    {
        failParkedRead();
        close();
#ifdef PIO_UNIT_TESTING
        sessionResetCount++;
#endif
        {
            std::lock_guard<std::mutex> guard(fromPhoneMutex);
            fromPhoneQueue.clear();
            fromPhoneQueueSize = 0;
            fromPhoneQueueBytes = 0;
        }
        {
            std::lock_guard<std::mutex> guard(readMutex);
            prefetched.clear();
            prefetchedBytes = 0;
            lastFromRadio.clear();
        }
        {
            std::lock_guard<std::mutex> guard(duplicateWriteMutex);
            lastToRadioLen = 0;
            std::memset(lastToRadio, 0, sizeof(lastToRadio));
        }
        {
            std::lock_guard<std::mutex> guard(valueMutex);
            fromNumValue = {0, 0, 0, 0};
            logValue.clear();
        }
        fromNumNotifying = false;
        logNotifying = false;
        batteryNotifying = false;
        configQueueOverflowPending = false;
    }

    void onInterfacesAdded(const sdbus::ObjectPath &path, const InterfaceMap &interfaces)
    {
        auto it = interfaces.find(kIfaceDevice);
        if (it == interfaces.end() || !belongsToAdapter(path))
            return;
        bool connectedNow = false;
        bool pairedNow = false;
        auto prop = it->second.find("Connected");
        if (prop != it->second.end())
            connectedNow = prop->second.get<bool>();
        prop = it->second.find("Paired");
        if (prop != it->second.end())
            pairedNow = prop->second.get<bool>();
        const std::string address = addressFromProperties(it->second, path);
        {
            std::lock_guard<std::mutex> guard(devMutex);
            pendingDeviceAdds[path] = PendingDeviceAdd{address, pairedNow, connectedNow};
        }
        deviceStatePending = true;
        wakeMainLoop();
    }

    void onBluezOwnerChanged(const std::string &oldOwner, const std::string &newOwner)
    {
        if (!oldOwner.empty() && oldOwner != newOwner) {
            bluezLostPending = true;
            wakeMainLoop();
        }
    }

    void onInterfacesRemoved(const sdbus::ObjectPath &path, const std::vector<std::string> &interfaces)
    {
        if (path == adapterPath && std::find(interfaces.begin(), interfaces.end(), kIfaceAdapter) != interfaces.end()) {
            LOG_WARN("BLE adapter %s was removed", adapterId.c_str());
            bluezLostPending = true;
            wakeMainLoop();
            return;
        }
        if (std::find(interfaces.begin(), interfaces.end(), kIfaceDevice) == interfaces.end())
            return;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            pendingDeviceRemovals.insert(path);
        }
        deviceStatePending = true;
        wakeMainLoop();
    }

    bool belongsToAdapter(const std::string &path) const { return path.rfind(adapterPath + "/", 0) == 0; }

    // --------------------------------------------------------------------
    // pairing

    void onDisplayPasskey(const sdbus::ObjectPath &device, uint32_t passkey, uint16_t entered)
    {
        requirePairingDevice(device);
        if (entered > 0)
            return; // progress updates while the peer types; the code is already
                    // showing
        pendingPasskey = passkey;
        passkeyChangeToken.fetch_add(1, std::memory_order_release);
        passkeyAvailable.store(true, std::memory_order_release);
        passkeyShowPending = true;
        wakeMainLoop();
    }

    void cancelPairingFromAgent()
    {
        std::string candidate;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            candidate = pairingCandidate;
            pairingCandidate.clear();
            pairingCandidateServiceAuthorized = false;
        }
        if (!candidate.empty()) {
            std::lock_guard<std::mutex> guard(policyMutex);
            unauthorizedDevices.insert(candidate);
            incompletePairingDevices.insert(candidate);
        }
        pairingClosePending = true;
        pairingFailureStatusPending = true;
        deviceStatePending = true;
        passkeyAvailable = false;
        dismissPasskey();
        wakeMainLoop();
    }

    void onAgentReleased()
    {
        agentRegistered = false;
        if (agentReleaseExpected.exchange(false) || draining)
            return;

        // Release means BlueZ no longer recognizes this process as its agent.
        // Revoke the in-progress candidate immediately on the event-loop thread;
        // the cooperative thread will turn Pairable off, disconnect the device
        // best-effort, and rebuild the registration.
        disarmPairingWindowAuthorization();
        pairingWindowActive = false;
        cancelPairingFromAgent();
        unexpectedAgentReleasePending = true;
        wakeMainLoop();
    }

    bool hasBondedPhone()
    {
        std::lock_guard<std::mutex> guard(devMutex);
        return !bondedPhoneAddress.empty();
    }

    bool pairingWindowExpired() const
    {
        return pairingWindowRequested && Throttle::hasElapsed(pairingWindowStartedMsec, pairingWindowDurationMsec);
    }

    static constexpr uint64_t kPairingWindowAuthorizationArmed = uint64_t{1} << 32;

    void armPairingWindowAuthorization()
    {
        const uint32_t deadline = pairingWindowStartedMsec + pairingWindowDurationMsec;
        pairingWindowAuthorization.store(kPairingWindowAuthorizationArmed | deadline, std::memory_order_release);
    }

    void disarmPairingWindowAuthorization() { pairingWindowAuthorization.store(0, std::memory_order_release); }

    bool pairingWindowAcceptingCallbacks() const
    {
        const uint64_t authorization = pairingWindowAuthorization.load(std::memory_order_acquire);
        if ((authorization & kPairingWindowAuthorizationArmed) == 0)
            return false;
        return !Throttle::deadlinePassedAt(Time::getMillis(), static_cast<uint32_t>(authorization));
    }

    bool pairingAgentNeeded()
    {
        return pairingWindowRequested && !pairingAgentSuspended && !pairingWindowExpired() && !hasBondedPhone();
    }

    bool openPairingWindowPolicy(uint32_t seconds)
    {
        if (!enabled || pairingAgentSuspended || seconds == 0 || hasBondedPhone())
            return false;

        seconds = std::min(seconds, LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS);
        passkeyAvailable.store(false, std::memory_order_release);
        pairingWindowRequested = true;
        pairingWindowStartedMsec = Time::getMillis();
        pairingWindowDurationMsec = seconds * 1000U;

        if (!registerAgent() || !setAdapterPairable(true)) {
            closePairingWindowPolicy(true);
            return false;
        }

        armPairingWindowAuthorization();
        pairingWindowActive = true;
        setIntervalFromNow(kPairingPolicyPollMsec);
        LOG_INFO("BLE pairing window open for %u seconds", seconds);
        return true;
    }

    void closePairingWindowPolicy(bool clearRequest)
    {
        disarmPairingWindowAuthorization();
        pairingWindowActive = false;
        passkeyAvailable.store(false, std::memory_order_release);
        if (clearRequest) {
            pairingWindowRequested = false;
            pairingWindowDurationMsec = 0;
        }
        setAdapterPairable(false);
        unregisterAgent();
        dismissPasskey();

        std::string unpairedCandidate;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            if (!pairingCandidate.empty() && pairingCandidate != bondedPhonePath) {
                unpairedCandidate = pairingCandidate;
                pairingCandidate.clear();
                pairingCandidateServiceAuthorized = false;
            }
        }
        if (!unpairedCandidate.empty()) {
            {
                std::lock_guard<std::mutex> guard(policyMutex);
                unauthorizedDevices.insert(unpairedCandidate);
                incompletePairingDevices.insert(unpairedCandidate);
            }
            pairingFailureStatusPending = true;
            deviceStatePending = true;
            wakeMainLoop();
        }
    }

    void setPairingAgentSuspendedPolicy(bool suspended)
    {
        if (pairingAgentSuspended == suspended)
            return;
        pairingAgentSuspended = suspended;
        if (suspended) {
            // Preserve the requested window and its original deadline while an
            // external local pairing agent owns BlueZ.
            closePairingWindowPolicy(false);
        } else if (pairingWindowRequested && !pairingWindowExpired() && !hasBondedPhone()) {
            if (registerAgent() && setAdapterPairable(true)) {
                armPairingWindowAuthorization();
                pairingWindowActive = true;
            } else
                closePairingWindowPolicy(true);
        } else {
            closePairingWindowPolicy(true);
        }
    }

    void setScanSuspendedPolicy(bool suspended)
    {
        scanSuspended = suspended;
        if (suspended)
            unregisterAdvertisement();
        else
            registerAdvertisement();
    }

    void setPowerSuspended(bool suspended)
    {
        powerSuspended = suspended;
        if (suspended)
            unregisterAdvertisement();
        else
            registerAdvertisement();
    }

    bool getLatestPasskey(uint32_t &passkey, uint64_t &changeToken) const
    {
        passkey = 0;
        changeToken = 0;
        if (!passkeyAvailable.load(std::memory_order_acquire))
            return false;
        uint64_t before;
        uint64_t after;
        do {
            before = passkeyChangeToken.load(std::memory_order_acquire);
            passkey = pendingPasskey.load();
            after = passkeyChangeToken.load(std::memory_order_acquire);
        } while (before != after);
        if (!passkeyAvailable.load(std::memory_order_acquire)) {
            passkey = 0;
            return false;
        }
        changeToken = after;
        return after != 0;
    }

    void disconnectUnauthorizedDevices()
    {
        // Keep the work queued while BlueZ is unavailable. Recovery teardown
        // may later discard it with the entire dead bus, while unit policy
        // tests can still inspect the fail-closed decision.
        if (!conn)
            return;
        std::set<std::string> pending;
        std::set<std::string> removeBonds;
        {
            std::lock_guard<std::mutex> guard(policyMutex);
            pending.swap(unauthorizedDevices);
            removeBonds.swap(incompletePairingDevices);
        }
        for (const auto &path : pending) {
            try {
                if (removeBonds.count(path) != 0 && adapterProxy) {
                    adapterProxy->callMethod("RemoveDevice").onInterface(kIfaceAdapter).withArguments(sdbus::ObjectPath{path});
                } else {
                    auto proxy = sdbuscompat::makeProxy(*conn, kBluezService, path);
                    sdbuscompat::finishProxy(*proxy);
                    proxy->callMethod("Disconnect").onInterface(kIfaceDevice);
                }
            } catch (const sdbus::Error &e) {
                LOG_DEBUG("BLE rejected-device disconnect ended: %s", e.getMessage().c_str());
            }
        }
    }

    // ----------------------------------------------------------------- lifecycle

    void cleanupFailedSetup()
    {
        draining = true;
        if (conn) {
            closePairingWindowPolicy(false);
            unregisterAgent();
            disconnectUnauthorizedDevices();
        }
        // A retry may fail after the prior transport had already claimed the
        // process-wide PhoneAPI lease. Always clear the session before releasing
        // BLUETOOTH or BLUETOOTH_PENDING, even when no D-Bus connection was
        // created during this attempt.
        resetTransportSession();
        teardownBus();
        meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, this);
        publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    }

    void doSetup()
    {
        if (enabled)
            return;
#ifdef MESHTASTIC_WDG_API
        // The policy stores a stable controller address. Resolve it again on
        // every recovery so USB hot-plug and hci renumbering are handled without
        // restarting meshtasticd.
        if (!meshtastic::portduino::refreshWdgBluetoothAdapter()) {
            LOG_WARN("BLE adapter %s is unavailable; retrying", meshtastic::portduino::wdgPolicy().adapterAddress.c_str());
            cleanupFailedSetup();
            return;
        }
        adapterId = portduino_config.bluetooth_adapter;
        adapterPath = "/org/bluez/" + adapterId;
#endif
        if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN) {
            fixedPinUnsupported = true;
            if (!fixedPinWarned.exchange(true))
                LOG_ERROR("BLE FIXED_PIN is unsupported by BlueZ Agent1 for a DisplayOnly LE peripheral; "
                          "select RANDOM_PIN or NO_PIN");
            cleanupFailedSetup();
            return;
        }
        fixedPinUnsupported = false;
        busGeneration.fetch_add(1);
        appRegistrationPending = false;
        advertisingRegistrationPending = false;
        registrationFailurePending = false;
        loadPhoneIdentity();
        try {
            conn = sdbus::createSystemBusConnection();
            conn->enterEventLoopAsync();

            dbusProxy = sdbuscompat::makeProxy(*conn, kDbusService, kDbusPath);
            dbusProxy->uponSignal("NameOwnerChanged")
                .onInterface(kIfaceDbus)
                .call([this](const std::string &name, const std::string &oldOwner, const std::string &newOwner) {
                    if (name == kBluezService)
                        onBluezOwnerChanged(oldOwner, newOwner);
                });
            sdbuscompat::finishProxy(*dbusProxy);

            bluezRootProxy = sdbuscompat::makeProxy(*conn, kBluezService, kBluezRootPath);
            bluezRootProxy->uponSignal("InterfacesAdded")
                .onInterface(kIfaceObjectManager)
                .call([this](const sdbus::ObjectPath &path, const InterfaceMap &ifaces) { onInterfacesAdded(path, ifaces); });
            bluezRootProxy->uponSignal("InterfacesRemoved")
                .onInterface(kIfaceObjectManager)
                .call([this](const sdbus::ObjectPath &path, const std::vector<std::string> &ifaces) {
                    onInterfacesRemoved(path, ifaces);
                });
            sdbuscompat::finishProxy(*bluezRootProxy);

            ManagedObjects objects;
            bluezRootProxy->callMethod("GetManagedObjects").onInterface(kIfaceObjectManager).storeResultsTo(objects);
            if (objects.find(sdbus::ObjectPath{adapterPath}) == objects.end()) {
                LOG_ERROR("BLE adapter %s not found in BlueZ; Bluetooth stays off", adapterId.c_str());
                cleanupFailedSetup();
                return;
            }

            deviceName = getDeviceName();
            batteryAvailable = shouldExposeBatteryService(powerStatus && powerStatus->getHasBattery());
            if (batteryAvailable) {
                std::lock_guard<std::mutex> guard(valueMutex);
                batteryValue = {std::min<uint8_t>(100, powerStatus->getBatteryChargePercent())};
            }

            adapterProxy = sdbuscompat::makeProxy(*conn, kBluezService, adapterPath);
            sdbuscompat::finishProxy(*adapterProxy);
            adapterProxy->setProperty("Powered").onInterface(kIfaceAdapter).toValue(true);
            try {
                adapterProxy->setProperty("Alias").onInterface(kIfaceAdapter).toValue(deviceName);
                // Advertising remains available for bonded reconnects, but an
                // unbonded phone may pair only during an explicit WDG window.
                adapterProxy->setProperty("Pairable").onInterface(kIfaceAdapter).toValue(false);
            } catch (const sdbus::Error &e) {
                LOG_WARN("BLE could not set adapter alias/pairable: %s", e.what());
            }

            exportGattTree();
            exportAgent();
            exportAdvertisement();

            registerApplication();

            // Track already-known devices (and any live connection) before
            // advertising.
            for (const auto &entry : objects) {
                auto dev = entry.second.find(kIfaceDevice);
                if (dev == entry.second.end() || !belongsToAdapter(entry.first))
                    continue;
                bool connectedNow = false;
                auto prop = dev->second.find("Connected");
                if (prop != dev->second.end())
                    connectedNow = prop->second.get<bool>();
                bool pairedNow = false;
                prop = dev->second.find("Paired");
                if (prop != dev->second.end())
                    pairedNow = prop->second.get<bool>();
                const std::string address = addressFromProperties(dev->second, entry.first);
                trackDevice(entry.first, address, pairedNow);
                if (connectedNow)
                    onDeviceConnectedChanged(entry.first, true);
            }

            enabled = true;
            if (pairingWindowRequested) {
                if (pairingWindowExpired()) {
                    closePairingWindowPolicy(true);
                } else if (!pairingAgentSuspended) {
                    if (registerAgent() && setAdapterPairable(true)) {
                        armPairingWindowAuthorization();
                        pairingWindowActive = true;
                    } else
                        closePairingWindowPolicy(true);
                }
            }
            registerAdvertisement();
            LOG_INFO("BLE ready on %s as '%s' (%s pairing)", adapterId.c_str(), deviceName.c_str(),
                     pinPairing() ? "passkey" : "just-works");
        } catch (const sdbus::Error &e) {
            LOG_ERROR("BLE setup failed (%s: %s); Bluetooth stays off", e.getName().c_str(), e.getMessage().c_str());
            cleanupFailedSetup();
        }
    }

    void exportGattTree()
    {
        gattRoot = sdbuscompat::makeObject(*conn, kGattAppPath);
        gattRoot->addObjectManager();

        service = sdbuscompat::makeObject(*conn, kServicePath);
        sdbuscompat::addVTable(*service, kIfaceGattService,
                               sdbuscompat::property("UUID", [] { return std::string(MESH_SERVICE_UUID); }),
                               sdbuscompat::property("Primary", [] { return true; }));

        const bool pin = pinPairing();
        const std::vector<std::string> readFlags = gattReadFlags(pin);
        const std::vector<std::string> writeFlags = gattWriteFlags(pin);
        const std::vector<std::string> notifyFlags = gattNotifyFlags(pin);

        toRadioChar = sdbuscompat::makeObject(*conn, kToRadioPath);
        sdbuscompat::addVTable(
            *toRadioChar, kIfaceGattChar,
            sdbuscompat::method("WriteValue", [this](std::vector<uint8_t> value,
                                                     PropertyMap options) { onToRadioWrite(std::move(value), options); }),
            sdbuscompat::property("UUID", [] { return std::string(TORADIO_UUID); }),
            sdbuscompat::property("Service", [] { return sdbus::ObjectPath{kServicePath}; }),
            sdbuscompat::property("Flags", [writeFlags] { return writeFlags; }));

        fromRadioChar = sdbuscompat::makeObject(*conn, kFromRadioPath);
        sdbuscompat::addVTable(
            *fromRadioChar, kIfaceGattChar,
            sdbuscompat::method("ReadValue", [this](sdbus::Result<std::vector<uint8_t>> result,
                                                    PropertyMap options) { onFromRadioRead(std::move(result), options); }),
            sdbuscompat::property("UUID", [] { return std::string(FROMRADIO_UUID); }),
            sdbuscompat::property("Service", [] { return sdbus::ObjectPath{kServicePath}; }),
            sdbuscompat::property("Flags", [readFlags] { return readFlags; }));

        fromNumChar = sdbuscompat::makeObject(*conn, kFromNumPath);
        sdbuscompat::addVTable(*fromNumChar, kIfaceGattChar,
                               sdbuscompat::method("ReadValue", [this](PropertyMap options) { return onFromNumRead(options); }),
                               sdbuscompat::method("StartNotify", [this] { requireNotifyState(NotifyKind::FROM_NUM, true); }),
                               sdbuscompat::method("StopNotify", [this] { requireNotifyState(NotifyKind::FROM_NUM, false); }),
                               sdbuscompat::property("UUID", [] { return std::string(FROMNUM_UUID); }),
                               sdbuscompat::property("Service", [] { return sdbus::ObjectPath{kServicePath}; }),
                               sdbuscompat::property("Flags", [notifyFlags] { return notifyFlags; }),
                               sdbuscompat::property("Notifying", [this] { return fromNumNotifying.load(); }),
                               sdbuscompat::property("Value", [this] {
                                   std::lock_guard<std::mutex> guard(valueMutex);
                                   return fromNumValue;
                               }));

        logRadioChar = sdbuscompat::makeObject(*conn, kLogRadioPath);
        sdbuscompat::addVTable(*logRadioChar, kIfaceGattChar,
                               sdbuscompat::method("ReadValue", [this](PropertyMap options) { return onLogRadioRead(options); }),
                               sdbuscompat::method("StartNotify", [this] { requireNotifyState(NotifyKind::LOG, true); }),
                               sdbuscompat::method("StopNotify", [this] { requireNotifyState(NotifyKind::LOG, false); }),
                               sdbuscompat::property("UUID", [] { return std::string(LOGRADIO_UUID); }),
                               sdbuscompat::property("Service", [] { return sdbus::ObjectPath{kServicePath}; }),
                               sdbuscompat::property("Flags", [notifyFlags] { return notifyFlags; }),
                               sdbuscompat::property("Notifying", [this] { return logNotifying.load(); }),
                               sdbuscompat::property("Value", [this] {
                                   std::lock_guard<std::mutex> guard(valueMutex);
                                   return logValue;
                               }));

        if (shouldExposeBatteryService(batteryAvailable)) {
            batteryService = sdbuscompat::makeObject(*conn, kBatteryServicePath);
            sdbuscompat::addVTable(*batteryService, kIfaceGattService,
                                   sdbuscompat::property("UUID", [] { return std::string(kBatteryServiceUuid); }),
                                   sdbuscompat::property("Primary", [] { return true; }));

            const std::vector<std::string> batteryFlags = batteryGattFlags();
            batteryLevelChar = sdbuscompat::makeObject(*conn, kBatteryLevelPath);
            sdbuscompat::addVTable(
                *batteryLevelChar, kIfaceGattChar,
                sdbuscompat::method("ReadValue", [this](PropertyMap options) { return onBatteryLevelRead(options); }),
                sdbuscompat::method("StartNotify", [this] { requireNotifyState(NotifyKind::BATTERY, true); }),
                sdbuscompat::method("StopNotify", [this] { requireNotifyState(NotifyKind::BATTERY, false); }),
                sdbuscompat::property("UUID", [] { return std::string(kBatteryLevelUuid); }),
                sdbuscompat::property("Service", [] { return sdbus::ObjectPath{kBatteryServicePath}; }),
                sdbuscompat::property("Flags", [batteryFlags] { return batteryFlags; }),
                sdbuscompat::property("Notifying", [this] { return batteryNotifying.load(); }),
                sdbuscompat::property("Value", [this] {
                    std::lock_guard<std::mutex> guard(valueMutex);
                    return batteryValue;
                }));
        }
    }

    void exportAgent()
    {
        agent = sdbuscompat::makeObject(*conn, kAgentPath);
        sdbuscompat::addVTable(
            *agent, kIfaceAgent, sdbuscompat::method("Release", [this] { onAgentReleased(); }),
            sdbuscompat::method("RequestPinCode",
                                [](sdbus::ObjectPath) -> std::string {
                                    throw sdbuscompat::dbusError("org.bluez.Error.Rejected", "display-only device");
                                }),
            sdbuscompat::method("DisplayPinCode", [](sdbus::ObjectPath, std::string) {}),
            sdbuscompat::method("RequestPasskey",
                                [](sdbus::ObjectPath) -> uint32_t {
                                    throw sdbuscompat::dbusError("org.bluez.Error.Rejected", "display-only device");
                                }),
            sdbuscompat::method("DisplayPasskey", [this](sdbus::ObjectPath device, uint32_t passkey,
                                                         uint16_t entered) { onDisplayPasskey(device, passkey, entered); }),
            sdbuscompat::method("RequestConfirmation",
                                [this](sdbus::ObjectPath device, uint32_t) {
                                    // Never expected with our capabilities; with a
                                    // PIN mode configured, silently confirming would
                                    // bypass MITM.
                                    if (pinPairing())
                                        throw sdbuscompat::dbusError("org.bluez.Error.Rejected", "passkey required");
                                    requirePairingOrBondedDevice(device);
                                }),
            sdbuscompat::method("RequestAuthorization",
                                [this](sdbus::ObjectPath device) { requirePairingOrBondedDevice(device); }),
            sdbuscompat::method("AuthorizeService",
                                [this](sdbus::ObjectPath device, std::string uuid) {
                                    if (!authorizeMeshtasticService(device, uuid))
                                        throw sdbuscompat::dbusError("org.bluez.Error.Rejected",
                                                                     "device is not authorized for the Meshtastic service");
                                }),
            sdbuscompat::method("Cancel", [this] {
                LOG_INFO("BLE pairing canceled");
                cancelPairingFromAgent();
            }));
    }

    void exportAdvertisement()
    {
        advert = sdbuscompat::makeObject(*conn, kAdvertPath);
        sdbuscompat::addVTable(*advert, kIfaceAdvert,
                               sdbuscompat::method("Release",
                                                   [this] {
                                                       advertising = false;
                                                       if (!advertisementReleaseExpected.exchange(false) && enabled &&
                                                           !draining) {
                                                           registrationFailurePending = true;
                                                           wakeMainLoop();
                                                       }
                                                   }),
                               sdbuscompat::property("Type", [] { return std::string("peripheral"); }),
                               sdbuscompat::property("ServiceUUIDs", [] { return std::vector<std::string>{MESH_SERVICE_UUID}; }),
                               sdbuscompat::property("LocalName", [this] { return deviceName; }),
                               sdbuscompat::property("Discoverable", [] { return true; }),
                               // Without these, the kernel default advertising interval of
                               // 1.28s applies and a central can take many seconds just to
                               // establish a link. Milliseconds. 20ms matches NimBLE's floor
                               // on ESP32: Android's background connects listen in sparse
                               // scan windows, and only an aggressive advertiser lands in
                               // them quickly. Power cost is irrelevant on a mains-powered
                               // host. Ignored by BlueZ < 5.71.
                               sdbuscompat::property("MinInterval", [] { return static_cast<uint32_t>(20); }),
                               sdbuscompat::property("MaxInterval", [] { return static_cast<uint32_t>(100); }));
    }

    void registerApplication()
    {
        if (appRegistered || appRegistrationPending || !adapterProxy)
            return;
        // Must be async: before replying, bluetoothd calls GetManagedObjects back
        // on our connection, and a synchronous call would sit on the connection
        // until timeout.
        const uint64_t generation = busGeneration.load();
        appRegistrationPending = true;
        adapterProxy->callMethodAsync("RegisterApplication")
            .onInterface(kIfaceGattManager)
            .withArguments(sdbus::ObjectPath{kGattAppPath}, PropertyMap{})
            .uponReplyInvoke([this, generation](sdbuscompat::AsyncError error) {
                onApplicationRegistrationComplete(generation, sdbuscompat::asyncFailed(error),
                                                  sdbuscompat::asyncErrorMessage(error));
            });
    }

    void onApplicationRegistrationComplete(uint64_t generation, bool failed, const std::string &error)
    {
        if (generation != busGeneration.load())
            return;
        appRegistrationPending = false;
        if (failed) {
            LOG_ERROR("BLE GATT registration failed: %s", error.c_str());
            registrationFailurePending = true;
        } else {
            appRegistered = true;
            LOG_INFO("BLE GATT service registered");
            advertisingUpdatePending = true;
        }
        wakeMainLoop();
    }

    bool registerAgent()
    {
        if (agentRegistered)
            return true;
        // The daemon must never be BlueZ's default agent outside a live,
        // explicit pairing window. Bonded reconnects use the persisted identity
        // and GATT request checks; they do not need an Agent1 lease.
        if (!conn || !pairingAgentNeeded())
            return false;
        try {
            if (!agentManagerProxy) {
                agentManagerProxy = sdbuscompat::makeProxy(*conn, kBluezService, kBluezManagerPath);
                sdbuscompat::finishProxy(*agentManagerProxy);
            }
            const std::string capability = pinPairing() ? "DisplayOnly" : "NoInputNoOutput";
            // An expected Release belongs to the previous registration. D-Bus
            // method ordering guarantees it precedes this synchronous
            // RegisterAgent reply on the same connection.
            agentReleaseExpected = false;
            agentManagerProxy->callMethod("RegisterAgent")
                .onInterface(kIfaceAgentManager)
                .withArguments(sdbus::ObjectPath{kAgentPath}, capability);
            agentRegistered = true;
            agentManagerProxy->callMethod("RequestDefaultAgent")
                .onInterface(kIfaceAgentManager)
                .withArguments(sdbus::ObjectPath{kAgentPath});
            return true;
        } catch (const sdbus::Error &e) {
            LOG_WARN("BLE could not register pairing agent: %s", e.getMessage().c_str());
            unregisterAgent();
            return false;
        }
    }

    void unregisterAgent()
    {
        if (!agentRegistered.exchange(false) || !agentManagerProxy)
            return;
        agentReleaseExpected = true;
        try {
            agentManagerProxy->callMethod("UnregisterAgent")
                .onInterface(kIfaceAgentManager)
                .withArguments(sdbus::ObjectPath{kAgentPath});
        } catch (const sdbus::Error &e) {
            agentReleaseExpected = false;
            LOG_DEBUG("BLE pairing agent was already unavailable: %s", e.getMessage().c_str());
        }
    }

    bool setAdapterPairable(bool pairable)
    {
        if (!adapterProxy)
            return false;
        try {
            adapterProxy->setProperty("Pairable").onInterface(kIfaceAdapter).toValue(pairable);
            return true;
        } catch (const sdbus::Error &e) {
            LOG_WARN("BLE could not set Pairable=%s: %s", pairable ? "true" : "false", e.getMessage().c_str());
            return false;
        }
    }

    bool shouldAdvertise()
    {
        return enabled && appRegistered && !fullClientSuspended && !scanSuspended && !powerSuspended && !checkIsConnected() &&
               meshtastic::portduino::fullPhoneApiLease().owner() == meshtastic::portduino::FullPhoneApiLease::Owner::NONE;
    }

    void registerAdvertisement()
    {
        if (!shouldAdvertise() || advertising || advertisingRegistrationPending || !adapterProxy)
            return;
        // Async for the same reason as RegisterApplication: bluetoothd reads our
        // advertisement object's properties before replying.
        const uint64_t currentBusGeneration = busGeneration.load();
        const uint64_t attempt = advertisingGeneration.fetch_add(1) + 1;
        advertisementReleaseExpected = false;
        advertisingRegistrationPending = true;
        adapterProxy->callMethodAsync("RegisterAdvertisement")
            .onInterface(kIfaceAdvManager)
            .withArguments(sdbus::ObjectPath{kAdvertPath}, PropertyMap{})
            .uponReplyInvoke([this, currentBusGeneration, attempt](sdbuscompat::AsyncError error) {
                if (currentBusGeneration != busGeneration.load())
                    return;
                advertisingRegistrationPending = false;
                if (sdbuscompat::asyncFailed(error)) {
                    advertising = false;
                    LOG_ERROR("BLE could not start advertising: %s", sdbuscompat::asyncErrorMessage(error).c_str());
                    if (attempt == advertisingGeneration.load())
                        registrationFailurePending = true;
                } else {
                    advertising = true;
                    LOG_INFO("BLE advertising as '%s'", deviceName.c_str());
                    // Reconcile desired state on the cooperative thread. This
                    // also unregisters a late success from an invalidated attempt.
                    advertisingUpdatePending = true;
                }
                wakeMainLoop();
            });
    }

    void unregisterAdvertisement()
    {
        advertisingGeneration.fetch_add(1);
        if (!enabled || !advertising || !adapterProxy)
            return;
        advertisementReleaseExpected = true;
        try {
            adapterProxy->callMethod("UnregisterAdvertisement")
                .onInterface(kIfaceAdvManager)
                .withArguments(sdbus::ObjectPath{kAdvertPath});
        } catch (const sdbus::Error &e) {
            advertisementReleaseExpected = false;
            LOG_WARN("BLE could not stop advertising: %s", e.getMessage().c_str());
        }
        advertising = false;
        LOG_INFO("BLE advertising stopped");
    }

    void setFullClientSuspended(bool suspended)
    {
        if (fullClientSuspended == suspended)
            return;
        fullClientSuspended = suspended;
        if (suspended)
            unregisterAdvertisement();
        else
            registerAdvertisement();
    }

    void doDeinit()
    {
        bluezRecoveryPending = false;
        bluezLostPending = false;
        if (!conn) {
            cleanupFailedSetup();
            return;
        }
        draining = true;
        // Runs on the main thread (setBluetoothEnable / AdminModule), so the alert
        // can be torn down inline -- runOnce() may never be scheduled again after
        // this.
        passkeyHidePending = true;
        updatePasskeyAlert();
        closePairingWindowPolicy(true);
        unregisterAgent();
        // The pairing window may have held a logically authorized but unbonded
        // candidate. Disconnect it while BlueZ is still live; teardown clears
        // the pending set and cannot perform this operation later.
        disconnectUnauthorizedDevices();
        unregisterAdvertisement();
        if (appRegistered.exchange(false)) {
            try {
                adapterProxy->callMethod("UnregisterApplication")
                    .onInterface(kIfaceGattManager)
                    .withArguments(sdbus::ObjectPath{kGattAppPath});
            } catch (const sdbus::Error &) {
            }
        }
        enabled = false;
        // Complete any parked ReadValue before destroying its D-Bus connection.
        // The draining checks at WriteValue's queue commit point prevent an
        // event-loop callback from refilling the cleared queue.
        resetTransportSession();
        teardownBus();
        meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, this);
        publishStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        LOG_INFO("BLE disabled");
    }

    void teardownBus()
    {
        busGeneration.fetch_add(1);
        if (conn)
            conn->leaveEventLoop();
        std::map<std::string, std::unique_ptr<sdbus::IProxy>> doomed;
        {
            // Move the proxies out so their (D-Bus) destruction happens without
            // holding devMutex - the main thread must never call into sdbus under
            // that lock.
            std::lock_guard<std::mutex> guard(devMutex);
            doomed.swap(deviceProxies);
            physicallyConnectedDevices.clear();
            connectedDevices.clear();
            pairedDevices.clear();
            serviceAuthorizedDevices.clear();
            deviceAddresses.clear();
            pendingDeviceAdds.clear();
            pendingDeviceRemovals.clear();
            bondedPhonePath.clear();
            pairingCandidate.clear();
            pairingCandidateServiceAuthorized = false;
        }
        doomed.clear();
        {
            std::lock_guard<std::mutex> guard(policyMutex);
            unauthorizedDevices.clear();
            incompletePairingDevices.clear();
        }
        agentManagerProxy.reset();
        adapterProxy.reset();
        bluezRootProxy.reset();
        dbusProxy.reset();
        gattRoot.reset();
        service.reset();
        toRadioChar.reset();
        fromRadioChar.reset();
        fromNumChar.reset();
        logRadioChar.reset();
        batteryService.reset();
        batteryLevelChar.reset();
        advert.reset();
        agent.reset();
        conn.reset();
        draining = false;
        enabled = false;
        advertising = false;
        advertisingUpdatePending = false;
        appRegistrationPending = false;
        advertisingRegistrationPending = false;
        registrationFailurePending = false;
        advertisementReleaseExpected = false;
        agentReleaseExpected = false;
        unexpectedAgentReleasePending = false;
        advertisingGeneration.fetch_add(1);
        scanSuspended = false;
        powerSuspended = false;
        pairingWindowRequested = false;
        disarmPairingWindowAuthorization();
        pairingWindowActive = false;
        pairingClosePending = false;
        pairingAgentSuspended = false;
        pairingWindowDurationMsec = 0;
        deviceStatePending = false;
        pairingFailureStatusPending = false;
        agentRegistered = false;
        appRegistered = false;
        batteryNotifying = false;
        batteryAvailable = false;
        bluezLostPending = false;
    }

    bool doClearBonds()
    {
        if (!conn) {
            LOG_WARN("BLE clearBonds: Bluetooth is not running, nothing to clear");
            return false;
        }
        closePairingWindowPolicy(true);
        std::string phoneAddress;
        std::string phonePath;
        {
            std::lock_guard<std::mutex> guard(devMutex);
            phoneAddress = bondedPhoneAddress;
            phonePath = bondedPhonePath;
        }
        if (phoneAddress.empty())
            return true;

        bool success = true;
        try {
            ManagedObjects objects;
            bluezRootProxy->callMethod("GetManagedObjects").onInterface(kIfaceObjectManager).storeResultsTo(objects);
            for (const auto &entry : objects) {
                auto dev = entry.second.find(kIfaceDevice);
                if (dev == entry.second.end() || !belongsToAdapter(entry.first))
                    continue;
                if (addressFromProperties(dev->second, entry.first) != phoneAddress)
                    continue;
                phonePath = entry.first;
                auto paired = dev->second.find("Paired");
                if (paired != dev->second.end() && paired->second.get<bool>()) {
                    LOG_INFO("BLE removing Meshtastic phone bond %s", entry.first.c_str());
                    adapterProxy->callMethod("RemoveDevice").onInterface(kIfaceAdapter).withArguments(entry.first);
                }
                success = true;
                break;
            }
        } catch (const sdbus::Error &e) {
            LOG_ERROR("BLE clearBonds failed: %s", e.getMessage().c_str());
            success = false;
        }
        if (!success)
            return false;

        requestPhoneIdentityClear();
        if (!persistPhoneIdentityIfNeeded())
            return false;

        {
            std::lock_guard<std::mutex> guard(devMutex);
            bondedPhoneAddress.clear();
            bondedPhonePath.clear();
            pairingCandidate.clear();
            connectedDevices.erase(phonePath);
            serviceAuthorizedDevices.erase(phonePath);
        }
        return !hasBondedPhone();
    }

    void doSendLog(const uint8_t *logMessage, size_t length)
    {
        // CAUTION: called from the logger; never LOG_* in here (infinite
        // recursion).
        if (!enabled || !logNotifying || !logRadioChar || length == 0 || !hasAuthorizedPhoneConnection())
            return;
        if (length > MAX_TO_FROM_RADIO_SIZE)
            length = MAX_TO_FROM_RADIO_SIZE;
        {
            std::lock_guard<std::mutex> guard(valueMutex);
            logValue.assign(logMessage, logMessage + length);
        }
        sdbuscompat::emitPropertiesChanged(*logRadioChar, kIfaceGattChar, "Value");
    }

    void doUpdateBatteryLevel(uint8_t level)
    {
        if (!batteryAvailable)
            return;
        level = std::min<uint8_t>(100, level);
        {
            std::lock_guard<std::mutex> guard(valueMutex);
            if (!batteryValue.empty() && batteryValue.front() == level)
                return;
            batteryValue = {level};
        }
        if (enabled && batteryNotifying && batteryLevelChar && hasAuthorizedPhoneConnection())
            sdbuscompat::emitPropertiesChanged(*batteryLevelChar, kIfaceGattChar, "Value");
    }
};

LinuxBluetooth::LinuxBluetooth() : impl(new Impl(portduino_config.bluetooth_adapter)) {}

LinuxBluetooth::~LinuxBluetooth()
{
    if (impl)
        impl->doDeinit();
}

void LinuxBluetooth::setup()
{
    impl->rememberRecoveryPolicy();
    impl->doSetup();
    if (impl->enabled && impl->appRegistered) {
        impl->bluezRecoveryPending = false;
    } else if (!impl->fixedPinUnsupported) {
        impl->restoreRecoveryPolicy();
        impl->armBluezRecovery(true);
    }
}

void LinuxBluetooth::shutdown()
{
    impl->setPowerSuspended(true);
}

void LinuxBluetooth::resumeAdvertising()
{
    impl->setPowerSuspended(false);
}

void LinuxBluetooth::setFullClientSuspended(bool suspended)
{
    impl->setFullClientSuspended(suspended);
}

void LinuxBluetooth::setScanSuspended(bool suspended)
{
    impl->setScanSuspendedPolicy(suspended);
}

void LinuxBluetooth::retrySharedAdapter()
{
    impl->setScanSuspendedPolicy(false);
}

void LinuxBluetooth::setPairingAgentSuspended(bool suspended)
{
    impl->setPairingAgentSuspendedPolicy(suspended);
}

bool LinuxBluetooth::openPairingWindow(uint32_t seconds)
{
    return impl->openPairingWindowPolicy(seconds);
}

void LinuxBluetooth::closePairingWindow()
{
    impl->closePairingWindowPolicy(true);
}

bool LinuxBluetooth::isPairingWindowOpen() const
{
    return impl->pairingWindowActive;
}

bool LinuxBluetooth::hasBondedPhone() const
{
    return impl->hasBondedPhone();
}

bool LinuxBluetooth::getLatestPasskey(uint32_t &passkey, uint64_t &changeToken) const
{
    return impl->getLatestPasskey(passkey, changeToken);
}

bool LinuxBluetooth::isAdvertising() const
{
    return impl->advertising;
}

bool LinuxBluetooth::isFixedPinUnsupported() const
{
    return impl->fixedPinUnsupported;
}

void LinuxBluetooth::deinit()
{
    impl->doDeinit();
}

bool LinuxBluetooth::clearBonds()
{
    return impl->doClearBonds();
}

bool LinuxBluetooth::isConnected()
{
    return impl->checkIsConnected();
}

int LinuxBluetooth::getRssi()
{
    return 0; // not exposed by BlueZ for connected peers; same answer as NRF52
}

bool LinuxBluetooth::isEnabled()
{
    return impl->enabled;
}

void LinuxBluetooth::updateBatteryLevel(uint8_t level)
{
    impl->doUpdateBatteryLevel(level);
}

void LinuxBluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    impl->doSendLog(logMessage, length);
}

#ifdef PIO_UNIT_TESTING
void LinuxBluetooth::testResetState()
{
    meshtastic::portduino::fullPhoneApiLease().release(meshtastic::portduino::FullPhoneApiLease::Owner::BLUETOOTH, impl.get());
    impl->bluezLostPending = false;
    impl->bluezRecoveryPending = false;
    impl->registrationFailurePending = false;
    impl->unexpectedAgentReleasePending = false;
    impl->agentReleaseExpected = false;
    impl->agentRegistered = false;
    impl->appRegistered = false;
    impl->appRegistrationPending = false;
    impl->advertisingUpdatePending = false;
    impl->disarmPairingWindowAuthorization();
    impl->pairingWindowActive = false;
    impl->pairingWindowRequested = false;
    impl->pairingClosePending = false;
    impl->deviceStatePending = false;
    impl->pairingFailureStatusPending = false;
    impl->batteryAvailable = false;
    impl->draining = false;
    impl->enabled = false;
    {
        std::lock_guard<std::mutex> guard(impl->identityMutex);
        impl->phoneIdentitySavePending = false;
        impl->phoneIdentityClearPending = false;
        impl->pendingPhoneIdentityAddress.clear();
    }
    impl->sessionResetCount = 0;
    impl->fromNumNotifying = false;
    impl->logNotifying = false;
    impl->batteryNotifying = false;
    impl->configQueueOverflowPending = false;
    impl->identityPersistenceTestResult = -1;
    {
        std::lock_guard<std::mutex> guard(impl->devMutex);
        impl->physicallyConnectedDevices.clear();
        impl->connectedDevices.clear();
        impl->pairedDevices.clear();
        impl->serviceAuthorizedDevices.clear();
        impl->deviceAddresses.clear();
        impl->pendingDeviceAdds.clear();
        impl->pendingDeviceRemovals.clear();
        impl->bondedPhoneAddress.clear();
        impl->bondedPhonePath.clear();
        impl->pairingCandidate.clear();
        impl->pairingCandidateServiceAuthorized = false;
    }
    {
        std::lock_guard<std::mutex> guard(impl->policyMutex);
        impl->unauthorizedDevices.clear();
        impl->incompletePairingDevices.clear();
    }
    {
        std::lock_guard<std::mutex> guard(impl->fromPhoneMutex);
        impl->fromPhoneQueue.clear();
        impl->fromPhoneQueueSize = 0;
        impl->fromPhoneQueueBytes = 0;
    }
    {
        std::lock_guard<std::mutex> guard(impl->duplicateWriteMutex);
        impl->lastToRadioLen = 0;
        std::memset(impl->lastToRadio, 0, sizeof(impl->lastToRadio));
    }
    {
        std::lock_guard<std::mutex> guard(impl->readMutex);
        impl->prefetched.clear();
        impl->prefetchedBytes = 0;
    }
}

LinuxBluetooth::TestSnapshot LinuxBluetooth::testSnapshot() const
{
    TestSnapshot snapshot;
    snapshot.ownerLossPending = impl->bluezLostPending;
    snapshot.registrationFailurePending = impl->registrationFailurePending;
    snapshot.applicationRegistrationPending = impl->appRegistrationPending;
    snapshot.applicationRegistered = impl->appRegistered;
    snapshot.unexpectedAgentReleasePending = impl->unexpectedAgentReleasePending;
    snapshot.agentRegistered = impl->agentRegistered;
    snapshot.agentReleaseExpected = impl->agentReleaseExpected;
    snapshot.pairingWindowActive = impl->pairingWindowActive;
    snapshot.pairingWindowRequested = impl->pairingWindowRequested;
    snapshot.pairingAgentNeeded = impl->pairingAgentNeeded();
    snapshot.batteryAvailable = impl->batteryAvailable;
    snapshot.draining = impl->draining;
    snapshot.queuedWrites = impl->fromPhoneQueueSize.load();
    snapshot.queuedWriteBytes = impl->fromPhoneQueueBytes.load();
    snapshot.sessionResetCount = impl->sessionResetCount.load();
    snapshot.fromNumNotifying = impl->fromNumNotifying;
    snapshot.logNotifying = impl->logNotifying;
    snapshot.batteryNotifying = impl->batteryNotifying;
    snapshot.adapterId = impl->adapterId;
    {
        std::lock_guard<std::mutex> guard(impl->identityMutex);
        snapshot.identitySavePending = impl->phoneIdentitySavePending;
        snapshot.identityClearPending = impl->phoneIdentityClearPending;
        snapshot.pendingIdentityAddress = impl->pendingPhoneIdentityAddress;
    }
    {
        std::lock_guard<std::mutex> guard(impl->devMutex);
        snapshot.bondedPhoneAddress = impl->bondedPhoneAddress;
        snapshot.bondedPhonePath = impl->bondedPhonePath;
        snapshot.pairingCandidate = impl->pairingCandidate;
        snapshot.pairingCandidateServiceAuthorized = impl->pairingCandidateServiceAuthorized;
        snapshot.serviceAuthorizedDevices = impl->serviceAuthorizedDevices.size();
        snapshot.connectedDevices = impl->connectedDevices.size();
        snapshot.pendingDeviceRemovals = impl->pendingDeviceRemovals.size();
    }
    {
        std::lock_guard<std::mutex> guard(impl->readMutex);
        snapshot.queuedReads = impl->prefetched.size();
        snapshot.queuedReadBytes = impl->prefetchedBytes;
    }
    {
        std::lock_guard<std::mutex> guard(impl->policyMutex);
        snapshot.queuedDisconnects = impl->unauthorizedDevices.size();
        snapshot.queuedBondRemovals = impl->incompletePairingDevices.size();
    }
    return snapshot;
}

void LinuxBluetooth::testSetEnabled(bool enabled)
{
    impl->enabled = enabled;
}

void LinuxBluetooth::testSetAdapter(const std::string &adapter)
{
    impl->adapterId = adapter;
    impl->adapterPath = "/org/bluez/" + adapter;
}

void LinuxBluetooth::testInjectBluezOwnerChange(const std::string &oldOwner, const std::string &newOwner)
{
    impl->onBluezOwnerChanged(oldOwner, newOwner);
}

void LinuxBluetooth::testBeginApplicationRegistration(uint64_t generation)
{
    impl->busGeneration = generation;
    impl->appRegistered = false;
    impl->appRegistrationPending = true;
    impl->registrationFailurePending = false;
}

void LinuxBluetooth::testCompleteApplicationRegistration(uint64_t generation, bool failed)
{
    impl->onApplicationRegistrationComplete(generation, failed, failed ? "injected failure" : "");
}

void LinuxBluetooth::testSetAgentRegistration(bool registered, bool releaseExpected)
{
    impl->agentRegistered = registered;
    impl->agentReleaseExpected = releaseExpected;
}

void LinuxBluetooth::testInjectAgentRelease()
{
    impl->onAgentReleased();
}

void LinuxBluetooth::testInjectAdapterRemoval()
{
    impl->onInterfacesRemoved(sdbus::ObjectPath{impl->adapterPath}, {kIfaceAdapter});
}

void LinuxBluetooth::testInjectDeviceRemoval(const std::string &path)
{
    impl->onInterfacesRemoved(sdbus::ObjectPath{path}, {kIfaceDevice});
}

void LinuxBluetooth::testSetBondIdentity(const std::string &address, const std::string &path, bool paired, bool connected)
{
    const std::string normalized = normalizedAddress(address);
    std::lock_guard<std::mutex> guard(impl->devMutex);
    impl->deviceAddresses[path] = normalized;
    if (paired) {
        impl->bondedPhoneAddress = normalized;
        impl->bondedPhonePath = path;
        impl->pairedDevices.insert(path);
        impl->serviceAuthorizedDevices.insert(path);
        impl->pairingCandidate.clear();
        impl->pairingCandidateServiceAuthorized = false;
    } else {
        impl->bondedPhoneAddress.clear();
        impl->bondedPhonePath.clear();
        impl->pairedDevices.erase(path);
        impl->serviceAuthorizedDevices.erase(path);
        impl->pairingCandidate = path;
        impl->pairingCandidateServiceAuthorized = false;
    }
    if (connected) {
        impl->physicallyConnectedDevices.insert(path);
        impl->connectedDevices.insert(path);
    } else {
        impl->physicallyConnectedDevices.erase(path);
        impl->connectedDevices.erase(path);
    }
    impl->pairingWindowActive = !paired;
    if (paired) {
        impl->disarmPairingWindowAuthorization();
    } else {
        impl->pairingWindowStartedMsec = Time::getMillis();
        impl->pairingWindowDurationMsec = LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS * 1000U;
        impl->armPairingWindowAuthorization();
    }
}

void LinuxBluetooth::testSetPairingCandidate(const std::string &address, const std::string &path, bool connected)
{
    const std::string normalized = normalizedAddress(address);
    std::lock_guard<std::mutex> guard(impl->devMutex);
    impl->deviceAddresses[path] = normalized;
    impl->bondedPhoneAddress.clear();
    impl->bondedPhonePath.clear();
    impl->pairedDevices.erase(path);
    impl->serviceAuthorizedDevices.erase(path);
    impl->connectedDevices.erase(path);
    impl->pairingCandidate = path;
    impl->pairingCandidateServiceAuthorized = false;
    impl->pairingWindowStartedMsec = Time::getMillis();
    impl->pairingWindowDurationMsec = LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS * 1000U;
    impl->armPairingWindowAuthorization();
    impl->pairingWindowActive = true;
    if (connected)
        impl->physicallyConnectedDevices.insert(path);
    else
        impl->physicallyConnectedDevices.erase(path);
}

bool LinuxBluetooth::testClaimPairingDevice(const std::string &path)
{
    return impl->claimPairingDevice(path);
}

void LinuxBluetooth::testAddDevice(const std::string &address, const std::string &path, bool paired, bool connected)
{
    const std::string normalized = normalizedAddress(address);
    std::lock_guard<std::mutex> guard(impl->devMutex);
    impl->deviceAddresses[path] = normalized;
    if (paired) {
        impl->pairedDevices.insert(path);
        if (!impl->bondedPhoneAddress.empty() && normalized == impl->bondedPhoneAddress) {
            impl->bondedPhonePath = path;
            impl->serviceAuthorizedDevices.insert(path);
        }
    } else {
        impl->pairedDevices.erase(path);
    }
    if (connected)
        impl->physicallyConnectedDevices.insert(path);
    else
        impl->physicallyConnectedDevices.erase(path);
    impl->deviceStatePending = true;
}

void LinuxBluetooth::testSetPaired(const std::string &path, bool paired)
{
    impl->onDevicePairedChanged(path, paired);
}

bool LinuxBluetooth::testAuthorizeService(const std::string &path, const std::string &uuid)
{
    return impl->authorizeMeshtasticService(path, uuid);
}

bool LinuxBluetooth::testAuthorizedPhonePath(const std::string &path)
{
    return impl->authorizedPhonePath(path);
}

void LinuxBluetooth::testSetIdentityPersistenceResult(int result)
{
    impl->identityPersistenceTestResult = result;
}

void LinuxBluetooth::testSetPairingWindowState(bool requested, bool active, uint32_t durationMsec)
{
    impl->pairingWindowRequested = requested;
    impl->pairingWindowActive = active;
    impl->pairingWindowStartedMsec = Time::getMillis();
    impl->pairingWindowDurationMsec = durationMsec;
    if (requested && active)
        impl->armPairingWindowAuthorization();
    else
        impl->disarmPairingWindowAuthorization();
}

bool LinuxBluetooth::testCommitWrite(const std::vector<uint8_t> &value, bool draining)
{
    {
        std::lock_guard<std::mutex> guard(impl->fromPhoneMutex);
        impl->fromPhoneQueue.clear();
        impl->fromPhoneQueueSize = 0;
        impl->fromPhoneQueueBytes = 0;
    }
    {
        std::lock_guard<std::mutex> guard(impl->duplicateWriteMutex);
        impl->lastToRadioLen = 0;
        std::memset(impl->lastToRadio, 0, sizeof(impl->lastToRadio));
    }
    impl->draining = draining;
    const bool accepted = impl->commitToRadioWrite(value) == Impl::QueueCommitResult::ACCEPTED;
    impl->draining = false;
    return accepted;
}

bool LinuxBluetooth::testQueueWrite(const std::vector<uint8_t> &value)
{
    impl->draining = false;
    return impl->commitToRadioWrite(value) == Impl::QueueCommitResult::ACCEPTED;
}

bool LinuxBluetooth::testQueueRead(const std::vector<uint8_t> &value)
{
    std::lock_guard<std::mutex> guard(impl->readMutex);
    if (impl->prefetched.size() >= kToPhoneQueueMaxMessages || impl->prefetchedBytes + value.size() > kToPhoneQueueMaxBytes) {
        impl->configQueueOverflowPending = true;
        return false;
    }
    impl->prefetched.push_back(value);
    impl->prefetchedBytes += value.size();
    return true;
}

bool LinuxBluetooth::testSetNotify(TestNotifyKind kind, bool enabled)
{
    switch (kind) {
    case TestNotifyKind::FROM_NUM:
        return impl->setNotifyState(Impl::NotifyKind::FROM_NUM, enabled);
    case TestNotifyKind::LOG:
        return impl->setNotifyState(Impl::NotifyKind::LOG, enabled);
    case TestNotifyKind::BATTERY:
        return impl->setNotifyState(Impl::NotifyKind::BATTERY, enabled);
    }
    return false;
}

void LinuxBluetooth::testAcquirePhoneLease()
{
    impl->activatePhoneConnection();
}

void LinuxBluetooth::testCleanupFailedSetup()
{
    impl->cleanupFailedSetup();
}

void LinuxBluetooth::testProcessOnce()
{
    impl->runOnce();
}

bool LinuxBluetooth::testIdentityClearWins(bool savePending, bool clearPending)
{
    return identityClearWins(savePending, clearPending);
}

bool LinuxBluetooth::testRequestIdentitySave(const std::string &address)
{
    return impl->requestPhoneIdentitySave(address);
}

void LinuxBluetooth::testRequestIdentityClear()
{
    impl->requestPhoneIdentityClear();
}

void LinuxBluetooth::testSetBatteryAvailable(bool available)
{
    impl->batteryAvailable = available;
}

bool LinuxBluetooth::testBatteryServiceWouldBeExported() const
{
    return shouldExposeBatteryService(impl->batteryAvailable);
}

bool LinuxBluetooth::testValidReadOffset(size_t offset, size_t valueLength)
{
    return validGattReadOffset(offset, valueLength);
}

std::vector<std::string> LinuxBluetooth::testGattFlags(TestGattKind kind, bool authenticated)
{
    switch (kind) {
    case TestGattKind::READ:
        return gattReadFlags(authenticated);
    case TestGattKind::WRITE:
        return gattWriteFlags(authenticated);
    case TestGattKind::NOTIFY:
        return gattNotifyFlags(authenticated);
    case TestGattKind::BATTERY:
        return batteryGattFlags();
    }
    return {};
}
#endif

#endif // MESHTASTIC_LINUX_BLE
