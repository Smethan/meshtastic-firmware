#include "TestUtil.h"
#include "configuration.h"
#include <unity.h>

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

#include "platform/portduino/FullPhoneApiLease.h"

using meshtastic::portduino::FullPhoneApiLease;

void setUp() {}
void tearDown() {}

void test_tcp_is_exclusive_and_holder_checked()
{
    FullPhoneApiLease lease;
    int first = 0;
    int second = 0;

    TEST_ASSERT_TRUE(lease.tryAcquireTcp(&first));
    TEST_ASSERT_TRUE(lease.tryAcquireTcp(&first));
    TEST_ASSERT_FALSE(lease.tryAcquireTcp(&second));
    TEST_ASSERT_TRUE(lease.isHeldBy(FullPhoneApiLease::Owner::TCP, &first));
    TEST_ASSERT_FALSE(lease.release(FullPhoneApiLease::Owner::TCP, &second));
    TEST_ASSERT_TRUE(lease.release(FullPhoneApiLease::Owner::TCP, &first));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FullPhoneApiLease::Owner::NONE), static_cast<int>(lease.owner()));
}

void test_bluetooth_preemption_waits_for_cooperative_tcp_cleanup()
{
    FullPhoneApiLease lease;
    int tcp = 0;
    int bluetooth = 0;

    TEST_ASSERT_TRUE(lease.tryAcquireTcp(&tcp));
    const uint64_t tcpGeneration = lease.generation();
    TEST_ASSERT_TRUE(lease.acquireBluetooth(&bluetooth));
    TEST_ASSERT_GREATER_THAN_UINT64(tcpGeneration, lease.generation());
    TEST_ASSERT_FALSE(lease.isHeldBy(FullPhoneApiLease::Owner::TCP, &tcp));
    TEST_ASSERT_FALSE(lease.isHeldBy(FullPhoneApiLease::Owner::BLUETOOTH, &bluetooth));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FullPhoneApiLease::Owner::BLUETOOTH_PENDING), static_cast<int>(lease.owner()));

    // Releasing the preempted transport acknowledges its cooperative cleanup;
    // it cannot clear the BLE claimant that replaces it.
    TEST_ASSERT_TRUE(lease.release(FullPhoneApiLease::Owner::TCP, &tcp));
    TEST_ASSERT_TRUE(lease.isHeldBy(FullPhoneApiLease::Owner::BLUETOOTH, &bluetooth));
    TEST_ASSERT_FALSE(lease.release(FullPhoneApiLease::Owner::TCP, &tcp));
    TEST_ASSERT_TRUE(lease.isHeldBy(FullPhoneApiLease::Owner::BLUETOOTH, &bluetooth));
}

void test_tcp_is_rejected_until_bluetooth_releases()
{
    FullPhoneApiLease lease;
    int tcp = 0;
    int bluetooth = 0;

    TEST_ASSERT_TRUE(lease.acquireBluetooth(&bluetooth));
    TEST_ASSERT_FALSE(lease.tryAcquireTcp(&tcp));
    TEST_ASSERT_TRUE(lease.release(FullPhoneApiLease::Owner::BLUETOOTH, &bluetooth));
    TEST_ASSERT_TRUE(lease.tryAcquireTcp(&tcp));
}

void test_disconnecting_bluetooth_cancels_a_pending_handoff()
{
    FullPhoneApiLease lease;
    int tcp = 0;
    int bluetooth = 0;

    TEST_ASSERT_TRUE(lease.tryAcquireTcp(&tcp));
    TEST_ASSERT_TRUE(lease.acquireBluetooth(&bluetooth));
    TEST_ASSERT_TRUE(lease.release(FullPhoneApiLease::Owner::BLUETOOTH, &bluetooth));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FullPhoneApiLease::Owner::NONE), static_cast<int>(lease.owner()));
    TEST_ASSERT_FALSE(lease.release(FullPhoneApiLease::Owner::TCP, &tcp));
}

void test_null_holders_never_acquire_or_release()
{
    FullPhoneApiLease lease;

    TEST_ASSERT_FALSE(lease.tryAcquireTcp(nullptr));
    TEST_ASSERT_FALSE(lease.acquireBluetooth(nullptr));
    TEST_ASSERT_FALSE(lease.release(FullPhoneApiLease::Owner::NONE, nullptr));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(FullPhoneApiLease::Owner::NONE), static_cast<int>(lease.owner()));
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_tcp_is_exclusive_and_holder_checked);
    RUN_TEST(test_bluetooth_preemption_waits_for_cooperative_tcp_cleanup);
    RUN_TEST(test_tcp_is_rejected_until_bluetooth_releases);
    RUN_TEST(test_disconnecting_bluetooth_cancels_a_pending_handoff);
    RUN_TEST(test_null_holders_never_acquire_or_release);
    exit(UNITY_END());
}

void loop() {}

#else

void setUp() {}
void tearDown() {}

void test_full_phone_api_lease_is_excluded_from_this_environment()
{
    TEST_ASSERT_TRUE(true);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_full_phone_api_lease_is_excluded_from_this_environment);
    exit(UNITY_END());
}

void loop() {}

#endif
