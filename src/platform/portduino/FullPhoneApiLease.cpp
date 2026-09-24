#include "FullPhoneApiLease.h"

#if defined(ARCH_PORTDUINO) && defined(PORTDUINO_LINUX_HARDWARE) && defined(PORTDUINO_BLUEZ) && defined(MESHTASTIC_LINUX_BLE)

namespace meshtastic::portduino
{

bool FullPhoneApiLease::tryAcquireTcp(const void *holder)
{
    if (holder == nullptr)
        return false;

    std::lock_guard<std::mutex> guard(mutex);
    if (currentOwner == Owner::TCP && currentHolder == holder)
        return true;
    if (currentOwner != Owner::NONE)
        return false;

    currentOwner = Owner::TCP;
    currentHolder = holder;
    ++currentGeneration;
    return true;
}

bool FullPhoneApiLease::acquireBluetooth(const void *holder)
{
    if (holder == nullptr)
        return false;

    std::lock_guard<std::mutex> guard(mutex);
    if (currentOwner == Owner::BLUETOOTH || currentOwner == Owner::BLUETOOTH_PENDING)
        return currentHolder == holder;

    // BLE has explicit priority, but core access waits until the cooperative
    // TCP worker has closed its PhoneAPI state. The BlueZ callback only records
    // this pending handoff and wakes the firmware loop.
    if (currentOwner == Owner::TCP) {
        preemptedTcpHolder = currentHolder;
        currentOwner = Owner::BLUETOOTH_PENDING;
    } else {
        preemptedTcpHolder = nullptr;
        currentOwner = Owner::BLUETOOTH;
    }
    currentHolder = holder;
    ++currentGeneration;
    return true;
}

bool FullPhoneApiLease::release(Owner owner, const void *holder)
{
    if (owner == Owner::NONE || holder == nullptr)
        return false;

    std::lock_guard<std::mutex> guard(mutex);
    if (currentOwner == Owner::BLUETOOTH_PENDING && owner == Owner::TCP && preemptedTcpHolder == holder) {
        preemptedTcpHolder = nullptr;
        currentOwner = Owner::BLUETOOTH;
        ++currentGeneration;
        return true;
    }
    if (currentOwner == Owner::BLUETOOTH_PENDING && owner == Owner::BLUETOOTH && currentHolder == holder) {
        currentOwner = Owner::NONE;
        currentHolder = nullptr;
        preemptedTcpHolder = nullptr;
        ++currentGeneration;
        return true;
    }
    if (currentOwner != owner || currentHolder != holder)
        return false;

    currentOwner = Owner::NONE;
    currentHolder = nullptr;
    preemptedTcpHolder = nullptr;
    ++currentGeneration;
    return true;
}

bool FullPhoneApiLease::isHeldBy(Owner owner, const void *holder) const
{
    std::lock_guard<std::mutex> guard(mutex);
    return currentOwner == owner && currentHolder == holder;
}

FullPhoneApiLease::Owner FullPhoneApiLease::owner() const
{
    std::lock_guard<std::mutex> guard(mutex);
    return currentOwner;
}

uint64_t FullPhoneApiLease::generation() const
{
    std::lock_guard<std::mutex> guard(mutex);
    return currentGeneration;
}

FullPhoneApiLease &fullPhoneApiLease()
{
    // The TCP listener is another translation-unit static and can be destroyed
    // late during process exit. Keep the tiny process-lifetime arbiter alive so
    // a ServerAPI destructor never observes a destroyed mutex due to undefined
    // cross-translation-unit destruction order.
    static auto *lease = new FullPhoneApiLease();
    return *lease;
}

} // namespace meshtastic::portduino

#endif
