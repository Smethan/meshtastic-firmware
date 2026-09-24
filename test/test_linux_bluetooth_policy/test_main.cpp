#include "TestUtil.h"
#include "configuration.h"
#include <unity.h>

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

#include "platform/portduino/LinuxBluetooth.h"

static LinuxBluetooth *bluetooth = nullptr;

void setUp()
{
    if (!bluetooth)
        bluetooth = new LinuxBluetooth();
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_policy_defaults_are_closed);
    RUN_TEST(test_policy_controls_fail_closed_without_bluez);
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
