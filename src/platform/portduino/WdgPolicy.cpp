#include "platform/portduino/WdgPolicy.h"

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "platform/portduino/PortduinoGlue.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace meshtastic::portduino
{
namespace
{

constexpr size_t MAX_POLICY_BYTES = 64 * 1024;
constexpr size_t MAX_SOCKET_PATH_BYTES = sizeof(sockaddr_un{}.sun_path) - 1;

WdgPolicy activePolicy;

std::string trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    return first < last ? std::string(first, last) : std::string{};
}

std::string uppercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
    return value;
}

bool isHciName(const std::string &value)
{
    return value.size() > 3 && value.rfind("hci", 0) == 0 &&
           std::all_of(value.begin() + 3, value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool isControllerAddress(const std::string &value)
{
    if (value.size() != 17)
        return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path adapterSysfsRoot()
{
    const char *overridePath = std::getenv("MESHTASTIC_WDG_BLUETOOTH_SYSFS");
    return overridePath && *overridePath ? overridePath : "/sys/class/bluetooth";
}

std::string readAdapterAddress(const std::string &adapter)
{
    std::ifstream address(adapterSysfsRoot() / adapter / "address");
    std::string value;
    return address >> value ? uppercase(trim(value)) : std::string{};
}

bool boolFromEnvironment(const char *name)
{
    const char *value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}

bool scalarBool(const YAML::Node &parent, const char *key, bool &result, std::string &error)
{
    const YAML::Node value = parent[key];
    if (!value || !value.IsScalar()) {
        error = std::string(key) + " must be a boolean";
        return false;
    }
    try {
        result = value.as<bool>();
        return true;
    } catch (const YAML::Exception &) {
        error = std::string(key) + " must be a boolean";
        return false;
    }
}

bool scalarUnsigned(const YAML::Node &parent, const char *key, uint64_t &result, std::string &error)
{
    const YAML::Node value = parent[key];
    if (!value || !value.IsScalar()) {
        error = std::string(key) + " must be an unsigned integer";
        return false;
    }
    try {
        result = value.as<uint64_t>();
        return true;
    } catch (const YAML::Exception &) {
        error = std::string(key) + " must be an unsigned integer";
        return false;
    }
}

bool scalarString(const YAML::Node &parent, const char *key, std::string &result, std::string &error)
{
    const YAML::Node value = parent[key];
    if (!value || !value.IsScalar()) {
        error = std::string(key) + " must be a string";
        return false;
    }
    try {
        result = trim(value.as<std::string>());
    } catch (const YAML::Exception &) {
        error = std::string(key) + " must be a string";
        return false;
    }
    if (result.empty() || result.find('\0') != std::string::npos) {
        error = std::string(key) + " must not be empty";
        return false;
    }
    return true;
}

void disableRestrictedInterfaces(const std::string &message)
{
    activePolicy.configured = true;
    activePolicy.valid = false;
    activePolicy.apiEnabled = false;
    activePolicy.phoneBleEnabled = false;
    portduino_config.bluetooth_enabled = false;
    std::cerr << "WDG policy rejected: " << message << "; local API and phone Bluetooth are disabled" << std::endl;
}

} // namespace

const WdgPolicy &wdgPolicy()
{
    return activePolicy;
}

std::string resolveWdgBluetoothAdapter(const std::string &requested)
{
    const std::string value = trim(requested);
    std::error_code error;
    std::vector<std::string> adapters;
    for (const auto &entry : std::filesystem::directory_iterator(adapterSysfsRoot(), error)) {
        const std::string name = entry.path().filename().string();
        if (isHciName(name))
            adapters.push_back(name);
    }
    std::sort(adapters.begin(), adapters.end());

    if (value == "auto")
        return adapters.empty() ? std::string{} : adapters.front();
    if (isHciName(value))
        return std::find(adapters.begin(), adapters.end(), value) != adapters.end() ? value : std::string{};
    if (!isControllerAddress(value))
        return {};

    const std::string wanted = uppercase(value);
    for (const std::string &adapter : adapters)
        if (readAdapterAddress(adapter) == wanted)
            return adapter;
    return {};
}

std::string selectedWdgBluetoothAdapterAddress()
{
    return activePolicy.adapterId.empty() ? std::string{} : readAdapterAddress(activePolicy.adapterId);
}

bool parseWdgPolicyText(const std::string &contents, WdgPolicy &policy, std::string &error)
{
    policy = WdgPolicy{};
    policy.configured = true;
    policy.valid = false;
    try {
        const YAML::Node root = YAML::Load(contents);
        const YAML::Node phone = root["phone_ble"];
        const YAML::Node api = root["wdg_api"];
        const YAML::Node fullClient = root["full_client_policy"];
        if (!root.IsMap() || !phone.IsMap() || !api.IsMap() || !fullClient.IsMap()) {
            error = "phone_ble, wdg_api, and full_client_policy mappings are required";
            return false;
        }

        if (!scalarBool(phone, "enabled", policy.phoneBleEnabled, error) ||
            !scalarString(phone, "adapter_address", policy.adapterAddress, error) ||
            !scalarBool(api, "enabled", policy.apiEnabled, error) ||
            !scalarString(api, "socket_path", policy.socketPath, error) ||
            !scalarBool(fullClient, "ble_priority", policy.blePriority, error))
            return false;

        uint64_t pairingSeconds = 0;
        uint64_t maxBonds = 0;
        uint64_t allowedUid = 0;
        if (!scalarUnsigned(phone, "pairing_window_seconds", pairingSeconds, error) ||
            !scalarUnsigned(phone, "max_bonds", maxBonds, error) || !scalarUnsigned(api, "allowed_uid", allowedUid, error))
            return false;
        if (pairingSeconds == 0 || pairingSeconds > 120) {
            error = "pairing_window_seconds must be between 1 and 120";
            return false;
        }
        if (maxBonds != 1) {
            error = "max_bonds must be 1";
            return false;
        }
        if (!policy.blePriority) {
            error = "ble_priority must be true";
            return false;
        }
        if (allowedUid > std::numeric_limits<uid_t>::max()) {
            error = "allowed_uid is outside the host UID range";
            return false;
        }
        if (policy.socketPath.front() != '/' || policy.socketPath.size() > MAX_SOCKET_PATH_BYTES) {
            error = "socket_path must be an absolute AF_UNIX path";
            return false;
        }
        if (policy.adapterAddress != "auto" && !isHciName(policy.adapterAddress) && !isControllerAddress(policy.adapterAddress)) {
            error = "adapter_address must be auto, hciX, or a controller address";
            return false;
        }

        policy.pairingWindowSeconds = static_cast<uint32_t>(pairingSeconds);
        policy.allowedUid = static_cast<uid_t>(allowedUid);
        policy.hasAllowedUid = true;
        policy.valid = true;
        return true;
    } catch (const YAML::Exception &e) {
        error = e.what();
        return false;
    }
}

bool readTrustedWdgPolicyFile(const std::string &path, uid_t requiredOwner, std::string &contents, std::string &error)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        error = std::string("cannot open ") + path + ": " + std::strerror(errno);
        return false;
    }

    struct stat policyStat {
    };
    if (fstat(fd, &policyStat) != 0) {
        error = std::string("cannot inspect ") + path + ": " + std::strerror(errno);
        close(fd);
        return false;
    }
    if (!S_ISREG(policyStat.st_mode) || policyStat.st_uid != requiredOwner || (policyStat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        error = "policy must be a regular file owned by root and not writable by "
                "group or other";
        close(fd);
        return false;
    }
    if (policyStat.st_size < 0 || static_cast<uint64_t>(policyStat.st_size) > MAX_POLICY_BYTES) {
        error = "policy file exceeds 65536 bytes";
        close(fd);
        return false;
    }

    contents.clear();
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = std::string("cannot read ") + path + ": " + std::strerror(errno);
            close(fd);
            return false;
        }
        if (contents.size() + static_cast<size_t>(count) > MAX_POLICY_BYTES) {
            error = "policy file exceeds 65536 bytes";
            close(fd);
            return false;
        }
        contents.append(buffer.data(), static_cast<size_t>(count));
    }
    close(fd);
    return true;
}

bool loadWdgPolicyFromEnvironment()
{
    activePolicy = WdgPolicy{};
    activePolicy.bluetoothForceDisabled = boolFromEnvironment("MESHTASTIC_WDG_DISABLE_BLUETOOTH");

    const char *path = std::getenv("MESHTASTIC_WDG_POLICY");
    if (!path || !*path) {
        const char *socketOverride = std::getenv("MESHTASTIC_WDG_SOCKET");
        if (socketOverride && *socketOverride)
            activePolicy.socketPath = socketOverride;
        activePolicy.adapterAddress = portduino_config.bluetooth_adapter.empty() ? "auto" : portduino_config.bluetooth_adapter;
        activePolicy.runtimeAdapterAddress = activePolicy.adapterAddress;
        activePolicy.adapterId = resolveWdgBluetoothAdapter(activePolicy.adapterAddress);
        if (activePolicy.bluetoothForceDisabled)
            portduino_config.bluetooth_enabled = false;
        return true;
    }

    std::string contents;
    std::string error;
    if (!readTrustedWdgPolicyFile(path, 0, contents, error)) {
        disableRestrictedInterfaces(error);
        return false;
    }

    WdgPolicy candidate;
    if (!parseWdgPolicyText(contents, candidate, error)) {
        disableRestrictedInterfaces(error);
        return false;
    }
    candidate.bluetoothForceDisabled = activePolicy.bluetoothForceDisabled;
    candidate.runtimeAdapterAddress = candidate.adapterAddress;
    candidate.adapterId = resolveWdgBluetoothAdapter(candidate.adapterAddress);
    if (candidate.phoneBleEnabled && candidate.adapterId.empty())
        std::cerr << "WDG policy Bluetooth adapter is unavailable: " << candidate.adapterAddress << std::endl;
    activePolicy = candidate;

    if (!activePolicy.adapterId.empty())
        portduino_config.bluetooth_adapter = activePolicy.adapterId;
    // Adapter availability is runtime state, not policy. Keep the requested
    // service enabled when a USB controller is absent at boot so the BlueZ
    // backend can retry and bind it after hot-plug.
    portduino_config.bluetooth_enabled = activePolicy.phoneBleEnabled && !activePolicy.bluetoothForceDisabled;
    return true;
}

bool wdgBluetoothAllowed()
{
    return activePolicy.valid && activePolicy.phoneBleEnabled && !activePolicy.bluetoothForceDisabled;
}

bool refreshWdgBluetoothAdapter()
{
    if (!wdgBluetoothAllowed())
        return false;

    const std::string requested =
        activePolicy.runtimeAdapterAddress.empty() ? activePolicy.adapterAddress : activePolicy.runtimeAdapterAddress;
    const std::string adapter = resolveWdgBluetoothAdapter(requested);
    if (adapter.empty()) {
        activePolicy.adapterId.clear();
        return false;
    }
    activePolicy.adapterId = adapter;
    portduino_config.bluetooth_adapter = adapter;
    return true;
}

bool selectWdgBluetoothAdapter(const std::string &requested)
{
    if (!wdgBluetoothAllowed())
        return false;
    const std::string value = trim(requested.empty() ? "auto" : requested);
    if (activePolicy.configured && activePolicy.adapterAddress != "auto") {
        const std::string policyAdapter = resolveWdgBluetoothAdapter(activePolicy.adapterAddress);
        const std::string requestedAdapter = value == "auto" ? policyAdapter : resolveWdgBluetoothAdapter(value);
        if (policyAdapter.empty() || requestedAdapter != policyAdapter)
            return false;
        activePolicy.runtimeAdapterAddress = activePolicy.adapterAddress;
    } else {
        activePolicy.runtimeAdapterAddress = value;
    }
    return refreshWdgBluetoothAdapter();
}

#ifdef PIO_UNIT_TESTING
void resetWdgPolicyForTest()
{
    activePolicy = WdgPolicy{};
}
#endif

} // namespace meshtastic::portduino

#endif
