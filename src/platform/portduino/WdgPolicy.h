#pragma once

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace meshtastic::portduino
{

struct WdgPolicy {
    bool configured = false;
    bool valid = true;
    bool apiEnabled = true;
    bool phoneBleEnabled = true;
    bool bluetoothForceDisabled = false;
    bool blePriority = true;
    bool hasAllowedUid = false;
    uid_t allowedUid = 0;
    uint32_t pairingWindowSeconds = 120;
    std::string socketPath = "/run/meshtasticd/wdg.sock";
    std::string adapterAddress = "auto";
    // Runtime selection requested by WDG. This remains separate from the host
    // policy so an "auto" policy permits a stable per-user controller choice.
    std::string runtimeAdapterAddress = "auto";
    std::string adapterId = "hci0";
};

const WdgPolicy &wdgPolicy();
bool loadWdgPolicyFromEnvironment();
bool parseWdgPolicyText(const std::string &contents, WdgPolicy &policy, std::string &error);
bool readTrustedWdgPolicyFile(const std::string &path, uid_t requiredOwner, std::string &contents, std::string &error);
bool wdgBluetoothAllowed();
bool refreshWdgBluetoothAdapter();
bool selectWdgBluetoothAdapter(const std::string &requested);
std::string resolveWdgBluetoothAdapter(const std::string &requested);
std::string selectedWdgBluetoothAdapterAddress();

#ifdef PIO_UNIT_TESTING
void resetWdgPolicyForTest();
#endif

} // namespace meshtastic::portduino

#endif
