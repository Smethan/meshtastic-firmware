#include "MeshService.h"
#include "TestUtil.h"
#include "configuration.h"
#include "main.h"
#include <unity.h>

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

#include "BluetoothCommon.h"
#include "UptimeClock.h"
#include "platform/portduino/FullPhoneApiLease.h"
#include "platform/portduino/LinuxBluetooth.h"

#include <algorithm>
#include <atomic>
#include <thread>

static LinuxBluetooth *bluetooth = nullptr;

void setUp()
{
    Time::useRealClock();
    if (!bluetooth)
        bluetooth = new LinuxBluetooth();
    bluetooth->testResetState();
}

// PhoneAPI's process globals are intentionally minimal in this policy-only
// suite. Let the short-lived test process reclaim the backend rather than run
// PhoneAPI's production shutdown path without a MeshService fixture.
void tearDown() {}

void test_policy_defaults_are_closed()
{
    uint32_t passkey = 123456;
    uint64_t token = 99;

    TEST_ASSERT_EQUAL_UINT32(120, LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS);
    TEST_ASSERT_FALSE(bluetooth->isAdvertising());
    TEST_ASSERT_FALSE(bluetooth->isPairingWindowOpen());
    TEST_ASSERT_FALSE(bluetooth->hasBondedPhone());
    TEST_ASSERT_FALSE(bluetooth->getLatestPasskey(passkey, token));
    TEST_ASSERT_EQUAL_UINT32(0, passkey);
    TEST_ASSERT_EQUAL_UINT64(0, token);
}

void test_policy_controls_fail_closed_without_bluez()
{
    TEST_ASSERT_FALSE(bluetooth->openPairingWindow(0));
    TEST_ASSERT_FALSE(bluetooth->openPairingWindow(LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS + 1));

    bluetooth->setScanSuspended(true);
    bluetooth->retrySharedAdapter();
    bluetooth->setPairingAgentSuspended(true);
    bluetooth->setPairingAgentSuspended(false);
    bluetooth->closePairingWindow();

    TEST_ASSERT_FALSE(bluetooth->isAdvertising());
    TEST_ASSERT_FALSE(bluetooth->isPairingWindowOpen());
}

void test_bluez_owner_loss_requires_a_real_owner_transition()
{
    bluetooth->testInjectBluezOwnerChange("", "");
    TEST_ASSERT_FALSE(bluetooth->testSnapshot().ownerLossPending);

    bluetooth->testInjectBluezOwnerChange(":1.17", ":1.17");
    TEST_ASSERT_FALSE(bluetooth->testSnapshot().ownerLossPending);

    bluetooth->testInjectBluezOwnerChange(":1.17", "");
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().ownerLossPending);
}

void test_async_application_registration_ignores_stale_replies_and_fails_closed()
{
    bluetooth->testBeginApplicationRegistration(12);
    bluetooth->testCompleteApplicationRegistration(11, true);
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.applicationRegistrationPending);
    TEST_ASSERT_FALSE(state.registrationFailurePending);

    bluetooth->testCompleteApplicationRegistration(12, true);
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.applicationRegistrationPending);
    TEST_ASSERT_FALSE(state.applicationRegistered);
    TEST_ASSERT_TRUE(state.registrationFailurePending);

    bluetooth->testResetState();
    bluetooth->testBeginApplicationRegistration(13);
    bluetooth->testCompleteApplicationRegistration(13, false);
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.applicationRegistrationPending);
    TEST_ASSERT_TRUE(state.applicationRegistered);
    TEST_ASSERT_FALSE(state.registrationFailurePending);
}

void test_agent_release_distinguishes_expected_shutdown_from_policy_loss()
{
    bluetooth->testSetAdapter("hci3");
    bluetooth->testSetAgentRegistration(true, true);
    bluetooth->testInjectAgentRelease();
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_FALSE(state.agentReleaseExpected);
    TEST_ASSERT_FALSE(state.unexpectedAgentReleasePending);

    bluetooth->testResetState();
    bluetooth->testSetAdapter("hci3");
    const std::string candidate = "/org/bluez/hci3/dev_AA_BB_CC_DD_EE_FF";
    bluetooth->testSetBondIdentity("AA:BB:CC:DD:EE:FF", candidate, false, true);
    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->testInjectAgentRelease();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_FALSE(state.pairingWindowActive);
    TEST_ASSERT_TRUE(state.unexpectedAgentReleasePending);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedDisconnects);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(candidate));
}

void test_adapter_removal_targets_only_the_selected_controller()
{
    bluetooth->testSetAdapter("hci7");
    TEST_ASSERT_EQUAL_STRING("hci7", bluetooth->testSnapshot().adapterId.c_str());
    bluetooth->testInjectAdapterRemoval();
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().ownerLossPending);

    // Recovery resolves the stable selector again after a USB hotplug. The
    // callback state accepts the newly resolved hci number instead of retaining
    // the removed controller name.
    bluetooth->testResetState();
    bluetooth->testSetAdapter("hci9");
    TEST_ASSERT_EQUAL_STRING("hci9", bluetooth->testSnapshot().adapterId.c_str());
    TEST_ASSERT_FALSE(bluetooth->testSnapshot().ownerLossPending);
}

static bool hasFlag(const std::vector<std::string> &flags, const char *value)
{
    return std::find(flags.begin(), flags.end(), value) != flags.end();
}

void test_gatt_security_flags_and_blob_offsets_match_the_bluez_contract()
{
    const auto authenticatedRead = LinuxBluetooth::testGattFlags(LinuxBluetooth::TestGattKind::READ, true);
    const auto authenticatedWrite = LinuxBluetooth::testGattFlags(LinuxBluetooth::TestGattKind::WRITE, true);
    const auto authenticatedNotify = LinuxBluetooth::testGattFlags(LinuxBluetooth::TestGattKind::NOTIFY, true);
    TEST_ASSERT_TRUE(hasFlag(authenticatedRead, "encrypt-authenticated-read"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedRead, "authorize"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedWrite, "encrypt-authenticated-write"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedWrite, "authorize"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedNotify, "encrypt-authenticated-read"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedNotify, "notify"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedNotify, "encrypt-authenticated-notify"));
    TEST_ASSERT_TRUE(hasFlag(authenticatedNotify, "authorize"));

    const auto justWorksNotify = LinuxBluetooth::testGattFlags(LinuxBluetooth::TestGattKind::NOTIFY, false);
    TEST_ASSERT_TRUE(hasFlag(justWorksNotify, "encrypt-read"));
    TEST_ASSERT_TRUE(hasFlag(justWorksNotify, "notify"));
    TEST_ASSERT_TRUE(hasFlag(justWorksNotify, "encrypt-notify"));
    TEST_ASSERT_FALSE(hasFlag(justWorksNotify, "encrypt-authenticated-notify"));

    const auto battery = LinuxBluetooth::testGattFlags(LinuxBluetooth::TestGattKind::BATTERY, true);
    TEST_ASSERT_TRUE(hasFlag(battery, "encrypt-read"));
    TEST_ASSERT_TRUE(hasFlag(battery, "notify"));
    TEST_ASSERT_TRUE(hasFlag(battery, "encrypt-notify"));
    TEST_ASSERT_TRUE(hasFlag(battery, "authorize"));

    TEST_ASSERT_TRUE(LinuxBluetooth::testValidReadOffset(0, 8));
    TEST_ASSERT_TRUE(LinuxBluetooth::testValidReadOffset(8, 8));
    TEST_ASSERT_FALSE(LinuxBluetooth::testValidReadOffset(9, 8));
}

void test_pairing_and_service_authorization_are_separate_requirements()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;

    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(1);
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetPairingCandidate("11:22:33:44:55:66", phone, true);

    bluetooth->testSetPaired(phone, true);
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.bondedPhoneAddress.empty());
    TEST_ASSERT_EQUAL_STRING(phone.c_str(), state.pairingCandidate.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(phone));

    TEST_ASSERT_TRUE(bluetooth->testAuthorizeService(phone, MESH_SERVICE_UUID));
    state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.bondedPhoneAddress.empty());
    TEST_ASSERT_EQUAL_STRING(phone.c_str(), state.pairingCandidate.c_str());
    TEST_ASSERT_TRUE(state.pairingCandidateServiceAuthorized);
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(phone));
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());

    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", state.bondedPhoneAddress.c_str());
    TEST_ASSERT_TRUE(state.pairingCandidate.empty());
    TEST_ASSERT_FALSE(state.pairingCandidateServiceAuthorized);
    TEST_ASSERT_EQUAL_UINT32(1, state.serviceAuthorizedDevices);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::BLUETOOTH, fullPhoneApiLease().owner());
    TEST_ASSERT_FALSE(state.agentRegistered);

    bluetooth->testResetState();
    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(1);
    bluetooth->testSetPairingCandidate("11:22:33:44:55:66", phone, true);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizeService(phone, MESH_SERVICE_UUID));
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().bondedPhoneAddress.empty());
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(phone));
    bluetooth->testSetPaired(phone, true);
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().bondedPhoneAddress.empty());
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
    bluetooth->testProcessOnce();
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", bluetooth->testSnapshot().bondedPhoneAddress.c_str());
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
}

void test_auxiliary_service_cannot_establish_a_phone_identity()
{
    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(1);
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetPairingCandidate("11:22:33:44:55:66", phone, true);
    bluetooth->testSetPairingWindowState(true, true, 120000);
    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->testSetPaired(phone, true);

    TEST_ASSERT_FALSE(bluetooth->testAuthorizeService(phone, "0000180f-0000-1000-8000-00805f9b34fb"));
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.bondedPhoneAddress.empty());
    TEST_ASSERT_TRUE(state.pairingCandidate.empty());
    TEST_ASSERT_FALSE(state.pairingCandidateServiceAuthorized);
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedBondRemovals);
    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_FALSE(state.pairingWindowRequested);

    bluetooth->testResetState();
    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, true);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizeService(phone, "0000180f-0000-1000-8000-00805f9b34fb"));
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
}

void test_identity_commit_failure_revokes_candidate_and_agent_before_activation()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;

    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(0);
    bluetooth->testSetPairingWindowState(true, true, 120000);
    bluetooth->testSetAgentRegistration(true, false);
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetPairingCandidate("11:22:33:44:55:66", phone, true);
    bluetooth->testSetPaired(phone, true);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizeService(phone, MESH_SERVICE_UUID));

    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.agentRegistered);
    TEST_ASSERT_TRUE(state.pairingCandidateServiceAuthorized);
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());

    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_FALSE(state.pairingWindowActive);
    TEST_ASSERT_TRUE(state.bondedPhoneAddress.empty());
    TEST_ASSERT_TRUE(state.pairingCandidate.empty());
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_EQUAL_UINT32(0, state.connectedDevices);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedDisconnects);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedBondRemovals);
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
}

void test_default_agent_is_scoped_to_an_unexpired_pairing_window()
{
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.pairingWindowRequested);
    TEST_ASSERT_FALSE(state.pairingAgentNeeded);
    TEST_ASSERT_FALSE(state.agentRegistered);

    bluetooth->testSetPairingWindowState(true, true, 120000);
    state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.pairingAgentNeeded);
    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->closePairingWindow();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.pairingWindowRequested);
    TEST_ASSERT_FALSE(state.pairingAgentNeeded);
    TEST_ASSERT_FALSE(state.agentRegistered);

    bluetooth->testSetPairingWindowState(true, true, 0);
    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.pairingWindowRequested);
    TEST_ASSERT_FALSE(state.agentRegistered);
}

void test_callback_pairing_deadline_is_enforced_across_millis_wrap()
{
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    Time::setTestMillis(UINT32_MAX - 15U);
    bluetooth->testSetAdapter("hci2");
    bluetooth->testAddDevice("11:22:33:44:55:66", phone, false, false);
    bluetooth->testSetPairingWindowState(true, true, 32);
    Time::advanceTestMillis(31);
    TEST_ASSERT_TRUE(bluetooth->testClaimPairingDevice(phone));
    Time::advanceTestMillis(1);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizeService(phone, MESH_SERVICE_UUID));
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().pairingCandidate.empty());

    bluetooth->testResetState();
    Time::setTestMillis(UINT32_MAX - 15U);
    bluetooth->testSetAdapter("hci2");
    bluetooth->testAddDevice("11:22:33:44:55:66", phone, false, false);
    bluetooth->testSetPairingWindowState(true, true, 32);
    Time::advanceTestMillis(32);
    TEST_ASSERT_FALSE(bluetooth->testClaimPairingDevice(phone));
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().pairingCandidate.empty());
}

void test_expired_window_closes_before_candidate_persistence()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;

    Time::setTestMillis(5000);
    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(1);
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetPairingCandidate("11:22:33:44:55:66", phone, true);
    bluetooth->testSetPairingWindowState(true, true, 100);
    bluetooth->testSetAgentRegistration(true, false);
    bluetooth->testSetPaired(phone, true);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizeService(phone, MESH_SERVICE_UUID));

    Time::advanceTestMillis(100);
    bluetooth->testProcessOnce();
    const LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.pairingWindowRequested);
    TEST_ASSERT_FALSE(state.pairingWindowActive);
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_TRUE(state.bondedPhoneAddress.empty());
    TEST_ASSERT_TRUE(state.pairingCandidate.empty());
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
}

void test_persisted_bond_reconnects_without_registering_a_default_agent()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;

    bluetooth->testSetAdapter("hci2");
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, false);
    bluetooth->testAddDevice("11:22:33:44:55:66", phone, true, true);
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_FALSE(state.pairingAgentNeeded);
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());

    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.agentRegistered);
    TEST_ASSERT_EQUAL_UINT32(1, state.connectedDevices);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::BLUETOOTH, fullPhoneApiLease().owner());
}

void test_device_removal_preserves_bond_identity_and_allows_path_rebind()
{
    bluetooth->testSetAdapter("hci2");
    bluetooth->testSetIdentityPersistenceResult(1);
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, true);

    bluetooth->testInjectDeviceRemoval(phone);
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_UINT32(1, state.pendingDeviceRemovals);
    TEST_ASSERT_EQUAL_UINT32(1, state.connectedDevices);
    TEST_ASSERT_EQUAL_UINT32(1, state.serviceAuthorizedDevices);

    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_UINT32(0, state.pendingDeviceRemovals);
    TEST_ASSERT_EQUAL_UINT32(0, state.connectedDevices);
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", state.bondedPhoneAddress.c_str());
    TEST_ASSERT_TRUE(state.bondedPhonePath.empty());
    TEST_ASSERT_FALSE(state.identityClearPending);

    bluetooth->testAddDevice("11:22:33:44:55:66", phone, true, true);
    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", state.bondedPhoneAddress.c_str());
    TEST_ASSERT_EQUAL_STRING(phone.c_str(), state.bondedPhonePath.c_str());
    TEST_ASSERT_EQUAL_UINT32(1, state.connectedDevices);
    TEST_ASSERT_EQUAL_UINT32(1, state.serviceAuthorizedDevices);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
}

void test_foreign_bond_is_rejected_before_any_notification_subscription()
{
    bluetooth->testSetAdapter("hci4");
    const std::string phone = "/org/bluez/hci4/dev_11_22_33_44_55_66";
    const std::string foreign = "/org/bluez/hci4/dev_AA_BB_CC_DD_EE_FF";
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, true);
    bluetooth->testAddDevice("AA:BB:CC:DD:EE:FF", foreign, true, true);

    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(foreign));
    TEST_ASSERT_FALSE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::FROM_NUM, true));
    TEST_ASSERT_FALSE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::LOG, true));
    TEST_ASSERT_FALSE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::BATTERY, true));
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_FALSE(state.fromNumNotifying);
    TEST_ASSERT_FALSE(state.logNotifying);
    TEST_ASSERT_FALSE(state.batteryNotifying);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedDisconnects);

    // Once BlueZ reports the foreign peer gone, the stored phone is again the
    // sole possible CCC subscriber and notification setup may proceed.
    bluetooth->testAddDevice("AA:BB:CC:DD:EE:FF", foreign, true, false);
    TEST_ASSERT_TRUE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::FROM_NUM, true));
    TEST_ASSERT_TRUE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::LOG, true));
    TEST_ASSERT_TRUE(bluetooth->testSetNotify(LinuxBluetooth::TestNotifyKind::BATTERY, true));
}

void test_incomplete_pairing_is_removed_when_the_window_closes()
{
    bluetooth->testSetAdapter("hci5");
    const std::string candidate = "/org/bluez/hci5/dev_AA_BB_CC_DD_EE_FF";
    bluetooth->testSetPairingCandidate("AA:BB:CC:DD:EE:FF", candidate, true);
    bluetooth->testSetPaired(candidate, true);
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().bondedPhoneAddress.empty());

    bluetooth->closePairingWindow();
    const LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.pairingCandidate.empty());
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedDisconnects);
    TEST_ASSERT_EQUAL_UINT32(1, state.queuedBondRemovals);
}

void test_setup_failure_and_null_deinit_clear_session_and_phone_lease()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;

    bluetooth->testSetAdapter("hci6");
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", "/org/bluez/hci6/dev_11_22_33_44_55_66", true, true);
    bluetooth->testAcquirePhoneLease();
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::BLUETOOTH, fullPhoneApiLease().owner());
    TEST_ASSERT_TRUE(bluetooth->testQueueWrite(std::vector<uint8_t>{1, 2, 3}));
    bluetooth->testCleanupFailedSetup();
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
    TEST_ASSERT_EQUAL_UINT32(0, state.queuedWrites);
    TEST_ASSERT_EQUAL_UINT32(0, state.queuedWriteBytes);
    TEST_ASSERT_EQUAL_UINT32(1, state.sessionResetCount);
    TEST_ASSERT_TRUE(state.bondedPhonePath.empty());
    TEST_ASSERT_EQUAL_UINT32(0, state.serviceAuthorizedDevices);

    static int tcpHolder;
    TEST_ASSERT_TRUE(fullPhoneApiLease().tryAcquireTcp(&tcpHolder));
    bluetooth->testAcquirePhoneLease();
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::BLUETOOTH_PENDING, fullPhoneApiLease().owner());
    bluetooth->testCleanupFailedSetup();
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
    TEST_ASSERT_EQUAL_UINT32(2, bluetooth->testSnapshot().sessionResetCount);

    // deinit() follows the same cleanup path when setup never acquired a bus.
    bluetooth->testAcquirePhoneLease();
    bluetooth->deinit();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL(FullPhoneApiLease::Owner::NONE, fullPhoneApiLease().owner());
    TEST_ASSERT_EQUAL_UINT32(3, state.sessionResetCount);
}

void test_ble_transport_queues_accept_documented_bursts_and_fail_bounded()
{
    for (size_t i = 0; i < 64; ++i) {
        std::vector<uint8_t> packet(512, static_cast<uint8_t>(i));
        packet[0] = static_cast<uint8_t>(i);
        TEST_ASSERT_TRUE(bluetooth->testQueueWrite(packet));
    }
    LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_UINT32(64, state.queuedWrites);
    TEST_ASSERT_EQUAL_UINT32(32 * 1024, state.queuedWriteBytes);
    TEST_ASSERT_FALSE(bluetooth->testQueueWrite(std::vector<uint8_t>(1, 0xee)));

    for (size_t i = 0; i < 128; ++i)
        TEST_ASSERT_TRUE(bluetooth->testQueueRead(std::vector<uint8_t>(512, static_cast<uint8_t>(i))));
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_UINT32(128, state.queuedReads);
    TEST_ASSERT_EQUAL_UINT32(64 * 1024, state.queuedReadBytes);
    TEST_ASSERT_FALSE(bluetooth->testQueueRead(std::vector<uint8_t>(1, 0xee)));

    bluetooth->testAcquirePhoneLease();
    bluetooth->testProcessOnce();
    state = bluetooth->testSnapshot();
    TEST_ASSERT_EQUAL_UINT32(0, state.queuedReads);
    TEST_ASSERT_EQUAL_UINT32(0, state.queuedReadBytes);
    TEST_ASSERT_EQUAL_UINT32(0, state.queuedWrites);
    TEST_ASSERT_EQUAL_UINT32(1, state.sessionResetCount);
    TEST_ASSERT_EQUAL(meshtastic::portduino::FullPhoneApiLease::Owner::NONE, meshtastic::portduino::fullPhoneApiLease().owner());
}

void test_identity_clear_dominates_a_simultaneous_save_request()
{
    TEST_ASSERT_FALSE(LinuxBluetooth::testIdentityClearWins(false, false));
    TEST_ASSERT_FALSE(LinuxBluetooth::testIdentityClearWins(true, false));
    TEST_ASSERT_TRUE(LinuxBluetooth::testIdentityClearWins(false, true));
    TEST_ASSERT_TRUE(LinuxBluetooth::testIdentityClearWins(true, true));

    std::atomic<bool> start{false};
    std::thread save([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        bluetooth->testRequestIdentitySave("11:22:33:44:55:66");
    });
    std::thread clear([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        bluetooth->testRequestIdentityClear();
    });
    start.store(true, std::memory_order_release);
    save.join();
    clear.join();

    const LinuxBluetooth::TestSnapshot state = bluetooth->testSnapshot();
    TEST_ASSERT_TRUE(state.identityClearPending);
    TEST_ASSERT_FALSE(state.identitySavePending);
    TEST_ASSERT_TRUE(state.pendingIdentityAddress.empty());
}

void test_gatt_authorization_requires_the_persisted_paired_identity()
{
    bluetooth->testSetAdapter("hci2");
    const std::string phone = "/org/bluez/hci2/dev_11_22_33_44_55_66";
    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, true);
    TEST_ASSERT_TRUE(bluetooth->testAuthorizedPhonePath(phone));
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", bluetooth->testSnapshot().bondedPhoneAddress.c_str());

    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, false, true);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(phone));

    bluetooth->testSetBondIdentity("11:22:33:44:55:66", phone, true, false);
    TEST_ASSERT_FALSE(bluetooth->testAuthorizedPhonePath(phone));
}

void test_write_commit_rechecks_draining_at_the_queue_boundary()
{
    const std::vector<uint8_t> packet{0x94, 0xc3, 0x01};
    TEST_ASSERT_FALSE(bluetooth->testCommitWrite(packet, true));
    TEST_ASSERT_EQUAL_UINT32(0, bluetooth->testSnapshot().queuedWrites);

    TEST_ASSERT_TRUE(bluetooth->testCommitWrite(packet, false));
    TEST_ASSERT_EQUAL_UINT32(1, bluetooth->testSnapshot().queuedWrites);
}

void test_battery_service_is_gated_by_host_telemetry()
{
    bluetooth->testSetBatteryAvailable(false);
    TEST_ASSERT_FALSE(bluetooth->testSnapshot().batteryAvailable);
    TEST_ASSERT_FALSE(bluetooth->testBatteryServiceWouldBeExported());
    bluetooth->updateBatteryLevel(81);
    TEST_ASSERT_FALSE(bluetooth->testSnapshot().batteryAvailable);

    bluetooth->testSetBatteryAvailable(true);
    TEST_ASSERT_TRUE(bluetooth->testBatteryServiceWouldBeExported());
    bluetooth->updateBatteryLevel(81);
    TEST_ASSERT_TRUE(bluetooth->testSnapshot().batteryAvailable);
}

void setup()
{
    initializeTestEnvironment();
    static MeshService policyService;
    service = &policyService;
    UNITY_BEGIN();
    RUN_TEST(test_policy_defaults_are_closed);
    RUN_TEST(test_policy_controls_fail_closed_without_bluez);
    RUN_TEST(test_bluez_owner_loss_requires_a_real_owner_transition);
    RUN_TEST(test_async_application_registration_ignores_stale_replies_and_fails_closed);
    RUN_TEST(test_agent_release_distinguishes_expected_shutdown_from_policy_loss);
    RUN_TEST(test_adapter_removal_targets_only_the_selected_controller);
    RUN_TEST(test_gatt_security_flags_and_blob_offsets_match_the_bluez_contract);
    RUN_TEST(test_gatt_authorization_requires_the_persisted_paired_identity);
    RUN_TEST(test_pairing_and_service_authorization_are_separate_requirements);
    RUN_TEST(test_auxiliary_service_cannot_establish_a_phone_identity);
    RUN_TEST(test_identity_commit_failure_revokes_candidate_and_agent_before_activation);
    RUN_TEST(test_default_agent_is_scoped_to_an_unexpired_pairing_window);
    RUN_TEST(test_callback_pairing_deadline_is_enforced_across_millis_wrap);
    RUN_TEST(test_expired_window_closes_before_candidate_persistence);
    RUN_TEST(test_persisted_bond_reconnects_without_registering_a_default_agent);
    RUN_TEST(test_device_removal_preserves_bond_identity_and_allows_path_rebind);
    RUN_TEST(test_foreign_bond_is_rejected_before_any_notification_subscription);
    RUN_TEST(test_incomplete_pairing_is_removed_when_the_window_closes);
    RUN_TEST(test_setup_failure_and_null_deinit_clear_session_and_phone_lease);
    RUN_TEST(test_ble_transport_queues_accept_documented_bursts_and_fail_bounded);
    RUN_TEST(test_identity_clear_dominates_a_simultaneous_save_request);
    RUN_TEST(test_write_commit_rechecks_draining_at_the_queue_boundary);
    RUN_TEST(test_battery_service_is_gated_by_host_telemetry);
    exit(UNITY_END());
}

void loop() {}

#else

void setUp() {}
void tearDown() {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}

void loop() {}

#endif
