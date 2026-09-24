#pragma once

#include "configuration.h"

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

#include <cstdint>
#include <mutex>

namespace meshtastic::portduino
{

/**
 * Serializes access to the destructive, process-wide PhoneAPI queues on Linux.
 *
 * The BlueZ event loop and the cooperative firmware loop both inspect this
 * object, so ownership changes are deliberately tiny and mutex protected. No
 * PhoneAPI or firmware-core method is called while the mutex is held.
 */
class FullPhoneApiLease
{
  public:
    enum class Owner : uint8_t {
        NONE,
        TCP,
        /** TCP has been preempted but has not completed cooperative cleanup. */
        BLUETOOTH_PENDING,
        BLUETOOTH
    };

    FullPhoneApiLease() = default;
    FullPhoneApiLease(const FullPhoneApiLease &) = delete;
    FullPhoneApiLease &operator=(const FullPhoneApiLease &) = delete;

    /** Acquire the lease for a TCP PhoneAPI. Fails while BLE owns it. */
    bool tryAcquireTcp(const void *holder);

    /**
     * Acquire the lease for BLE. BLE has priority and atomically starts a
     * handoff from a TCP holder; BLE becomes active after the cooperative TCP
     * worker closes itself and releases its prior lease.
     */
    bool acquireBluetooth(const void *holder);

    /**
     * Release only when both the owner kind and holder still match. Releasing
     * a preempted TCP lease completes the pending handoff to BLE instead of
     * clearing the BLE claimant.
     */
    bool release(Owner owner, const void *holder);

    bool isHeldBy(Owner owner, const void *holder) const;
    Owner owner() const;
    uint64_t generation() const;

  private:
    mutable std::mutex mutex;
    Owner currentOwner = Owner::NONE;
    const void *currentHolder = nullptr;
    const void *preemptedTcpHolder = nullptr;
    uint64_t currentGeneration = 0;
};

/** Process-wide lease shared by the BlueZ and TCP PhoneAPI transports. */
FullPhoneApiLease &fullPhoneApiLease();

} // namespace meshtastic::portduino

#endif
