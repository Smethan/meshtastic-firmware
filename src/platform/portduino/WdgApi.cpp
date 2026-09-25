#include "platform/portduino/WdgApi.h"

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "DebugConfiguration.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "main.h"
#include "modules/NodeInfoModule.h"
#include "modules/TextMessageModule.h"
#include "platform/portduino/FullPhoneApiLease.h"
#include "platform/portduino/LinuxBluetooth.h"
#include "platform/portduino/PortduinoGlue.h"
#include "platform/portduino/WdgPolicy.h"
#include "target_specific.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <json/json.h>
#include <memory>
#include <sstream>
#include <sys/acl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace meshtastic::portduino
{
namespace
{

constexpr const char *DEFAULT_SOCKET_PATH = "/run/meshtasticd/wdg.sock";
constexpr size_t MAX_REQUEST_ID_BYTES = 128;
constexpr int32_t ACTIVE_POLL_MS = 20;
constexpr int32_t IDLE_POLL_MS = 100;
constexpr int32_t RETRY_POLL_MS = 1000;
constexpr size_t MAX_COMMANDS_PER_POLL = 8;
constexpr size_t MAX_EVENTS_PER_POLL = 8;
constexpr size_t SNAPSHOT_QUEUE_WATERMARK = 48;
constexpr size_t MAX_RECENT_REQUEST_IDS = 256;
constexpr size_t MAX_OBSERVER_NODE_UPDATES = 256;
constexpr size_t MAX_OBSERVER_TEXT_EVENTS = 128;
constexpr size_t MAX_OBSERVER_TEXT_BYTES = 64 * 1024;
constexpr uint32_t MAX_BLE_SCAN_LEASE_SECONDS = 20;
constexpr uint32_t MAX_PAIRING_AGENT_LEASE_SECONDS = 120;
constexpr uint32_t DISCOVERY_THROTTLE_MS = 60 * 1000;

const char *const CAPABILITIES[] = {
    "hello",
    "get_status",
    "snapshot_nodes",
    "send_text",
    "request_node_info",
    "set_phone_ble",
    "open_pairing",
    "forget_phone",
    "ble_scan_lease_acquire",
    "ble_scan_lease_release",
    "pairing_agent_lease_acquire",
    "pairing_agent_lease_release",
    "retry_shared_adapter",
};

std::string compactJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, value);
}

Json::Value makeReply(const Json::Value &requestId, bool ok)
{
    Json::Value reply(Json::objectValue);
    reply["v"] = WdgApi::PROTOCOL_VERSION;
    reply["type"] = "reply";
    reply["request_id"] = requestId;
    reply["ok"] = ok;
    return reply;
}

std::string errorReply(const Json::Value &requestId, const char *code, const char *message)
{
    Json::Value reply = makeReply(requestId, false);
    reply["error_code"] = code;
    reply["message"] = message;
    return compactJson(reply);
}

std::string nodeId(NodeNum num)
{
    char value[12];
    snprintf(value, sizeof(value), "!%08x", num);
    return value;
}

bool isContinuation(uint8_t value)
{
    return (value & 0xc0U) == 0x80U;
}

std::string sanitizeUtf8(const uint8_t *value, size_t length)
{
    if (!value)
        return {};

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length;) {
        const uint8_t lead = value[i];
        size_t sequence = 0;
        if (lead <= 0x7fU) {
            sequence = 1;
        } else if (lead >= 0xc2U && lead <= 0xdfU && i + 1 < length && isContinuation(value[i + 1])) {
            sequence = 2;
        } else if (lead >= 0xe0U && lead <= 0xefU && i + 2 < length && isContinuation(value[i + 1]) &&
                   isContinuation(value[i + 2]) && (lead != 0xe0U || value[i + 1] >= 0xa0U) &&
                   (lead != 0xedU || value[i + 1] <= 0x9fU)) {
            sequence = 3;
        } else if (lead >= 0xf0U && lead <= 0xf4U && i + 3 < length && isContinuation(value[i + 1]) &&
                   isContinuation(value[i + 2]) && isContinuation(value[i + 3]) && (lead != 0xf0U || value[i + 1] >= 0x90U) &&
                   (lead != 0xf4U || value[i + 1] <= 0x8fU)) {
            sequence = 4;
        }

        if (sequence == 0) {
            result.append("\xef\xbf\xbd", 3);
            ++i;
            continue;
        }
        result.append(reinterpret_cast<const char *>(value + i), sequence);
        i += sequence;
    }
    return result;
}

std::string boundedUtf8(const char *value, size_t capacity)
{
    if (!value)
        return {};
    return sanitizeUtf8(reinterpret_cast<const uint8_t *>(value), strnlen(value, capacity));
}

bool parseUnsigned(const Json::Value &value, uint32_t &result)
{
    if (value.isUInt()) {
        result = value.asUInt();
        return true;
    }
    if (!value.isString())
        return false;

    std::string text = value.asString();
    if (text.empty())
        return false;
    int base = 10;
    const char *start = text.c_str();
    if (text[0] == '!') {
        start++;
        base = 16;
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }

    errno = 0;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(start, &end, base);
    if (errno || end == start || *end != '\0' || parsed > UINT32_MAX)
        return false;
    result = static_cast<uint32_t>(parsed);
    return true;
}

bool parseAllowedUidFromEnvironment(uid_t &uid)
{
    const char *value = std::getenv("MESHTASTIC_WDG_ALLOWED_UID");
    if (!value || !*value)
        return false;

    errno = 0;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT_MAX)
        return false;

    uid = static_cast<uid_t>(parsed);
    return true;
}

const char *fullPhoneApiOwnerName(FullPhoneApiLease::Owner owner)
{
    switch (owner) {
    case FullPhoneApiLease::Owner::BLUETOOTH:
        return "bluetooth";
    case FullPhoneApiLease::Owner::BLUETOOTH_PENDING:
        return "bluetooth_pending";
    case FullPhoneApiLease::Owner::TCP:
        return "tcp";
    case FullPhoneApiLease::Owner::NONE:
    default:
        return "none";
    }
}

const char *pairingModeName(meshtastic_Config_BluetoothConfig_PairingMode mode)
{
    switch (mode) {
    case meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN:
        return "random_pin";
    case meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN:
        return "fixed_pin";
    case meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN:
        return "no_pin";
    default:
        return "unknown";
    }
}

bool intervalExpired(uint32_t started, uint32_t duration)
{
    return duration != 0 && Throttle::hasElapsed(started, duration);
}

} // namespace

WdgApi::WdgApi(std::string socketPath, PeerAuthorizer peerAuthorizer)
    : concurrency::OSThread("WdgApi"), socketPath(std::move(socketPath)), peerAuthorizer(std::move(peerAuthorizer))
{
    ensureObservers();
}

WdgApi::~WdgApi()
{
    stop();
}

std::string WdgApi::resolveSocketPath()
{
    const WdgPolicy &policy = wdgPolicy();
    if (policy.configured)
        return policy.socketPath;
    const char *overridePath = std::getenv("MESHTASTIC_WDG_SOCKET");
    return overridePath && *overridePath ? overridePath : DEFAULT_SOCKET_PATH;
}

bool WdgApi::start()
{
    if (listenFd >= 0)
        return true;
    const WdgPolicy &policy = wdgPolicy();
    if (policy.configured && (!policy.valid || !policy.apiEnabled)) {
        LOG_ERROR("WDG API is disabled by host policy");
        return false;
    }
    return openListener();
}

void WdgApi::stop()
{
    closeClient();
    closeListener();
}

bool WdgApi::openListener()
{
    sockaddr_un address{};
    if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path)) {
        LOG_ERROR("WDG API socket path is empty or too long");
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_ERROR("WDG API socket failed: %s", std::strerror(errno));
        return false;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);

    struct stat existing {
    };
    if (lstat(socketPath.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            LOG_ERROR("WDG API path exists and is not a socket: %s", socketPath.c_str());
            close(fd);
            return false;
        }

        int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            LOG_ERROR("WDG API could not probe existing socket: %s", std::strerror(errno));
            close(fd);
            return false;
        }

        int result = connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        int connectError = errno;
        close(probe);
        if (result == 0 || connectError != ECONNREFUSED) {
            LOG_ERROR("WDG API socket is already in use: %s", socketPath.c_str());
            close(fd);
            return false;
        }

        if (unlink(socketPath.c_str()) != 0) {
            LOG_ERROR("WDG API could not remove stale socket: %s", std::strerror(errno));
            close(fd);
            return false;
        }
    } else if (errno != ENOENT) {
        LOG_ERROR("WDG API could not inspect socket path: %s", std::strerror(errno));
        close(fd);
        return false;
    }

    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        LOG_ERROR("WDG API bind failed for %s: %s", socketPath.c_str(), std::strerror(errno));
        close(fd);
        return false;
    }
    ownsSocketPath = true;

    if (chmod(socketPath.c_str(), 0660) != 0) {
        LOG_ERROR("WDG API chmod failed: %s", std::strerror(errno));
        close(fd);
        unlink(socketPath.c_str());
        ownsSocketPath = false;
        return false;
    }

    const WdgPolicy &policy = wdgPolicy();
    if (policy.configured && policy.hasAllowedUid) {
        const std::filesystem::path parent = std::filesystem::path(socketPath).parent_path();
        if (parent.empty() || !applyPathAccessControl(parent.string(), policy.allowedUid, true) ||
            !applyPathAccessControl(socketPath, policy.allowedUid, false)) {
            LOG_ERROR("WDG API could not grant access to uid %u", static_cast<unsigned>(policy.allowedUid));
            close(fd);
            unlink(socketPath.c_str());
            ownsSocketPath = false;
            return false;
        }
    }

    if (listen(fd, static_cast<int>(MAX_ACCEPTS_PER_POLL * 2)) != 0) {
        LOG_ERROR("WDG API listen failed: %s", std::strerror(errno));
        close(fd);
        unlink(socketPath.c_str());
        ownsSocketPath = false;
        return false;
    }

    listenFd = fd;
    LOG_INFO("WDG API listening on %s", socketPath.c_str());
    return true;
}

void WdgApi::acceptClients()
{
    for (size_t processed = 0; processed < MAX_ACCEPTS_PER_POLL && listenFd >= 0; ++processed) {
        int accepted = accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                LOG_WARN("WDG API accept failed: %s", std::strerror(errno));
            return;
        }

        struct {
            pid_t pid;
            uid_t uid;
            gid_t gid;
        } nativeCredentials{};
        socklen_t credentialsLength = sizeof(nativeCredentials);
        WdgPeerCredentials credentials{};
        bool credentialsValid = getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &nativeCredentials, &credentialsLength) == 0 &&
                                credentialsLength == sizeof(nativeCredentials);
        if (credentialsValid) {
            credentials = {nativeCredentials.pid, nativeCredentials.uid, nativeCredentials.gid};
        }

        if (clientFd >= 0 || !credentialsValid || !authorizePeer(credentials)) {
            close(accepted);
            continue;
        }

        clientFd = accepted;
        LOG_INFO("WDG API client connected: pid=%d uid=%u", credentials.pid, static_cast<unsigned>(credentials.uid));
    }
}

bool WdgApi::authorizePeer(const WdgPeerCredentials &credentials) const
{
    if (peerAuthorizer)
        return peerAuthorizer(credentials);

    const WdgPolicy &policy = wdgPolicy();
    if (policy.configured)
        return credentials.uid == 0 || (policy.valid && policy.hasAllowedUid && credentials.uid == policy.allowedUid);

    uid_t allowedUid = 0;
    if (parseAllowedUidFromEnvironment(allowedUid) && credentials.uid == allowedUid)
        return true;
    return credentials.uid == 0 || credentials.uid == geteuid();
}

bool WdgApi::applyPathAccessControl(const std::string &path, uid_t allowedUid, bool directory)
{
    std::ostringstream value;
    if (directory) {
        value << "user::rwx,user:" << static_cast<uintmax_t>(allowedUid) << ":--x,group::rwx,mask::rwx,other::---";
    } else {
        value << "user::rw-,user:" << static_cast<uintmax_t>(allowedUid) << ":rw-,group::rw-,mask::rw-,other::---";
    }

    acl_t acl = acl_from_text(value.str().c_str());
    if (!acl) {
        LOG_ERROR("WDG API could not construct ACL for %s: %s", path.c_str(), std::strerror(errno));
        return false;
    }
    const bool valid = acl_valid(acl) == 0;
    const bool applied = valid && acl_set_file(path.c_str(), ACL_TYPE_ACCESS, acl) == 0;
    if (!applied)
        LOG_ERROR("WDG API could not set ACL on %s: %s", path.c_str(), std::strerror(errno));
    acl_free(acl);
    return applied;
}

void WdgApi::receiveCommands()
{
    if (clientFd < 0)
        return;

    for (size_t processed = 0; processed < MAX_COMMANDS_PER_POLL; ++processed) {
        ssize_t length = recv(clientFd, receiveBuffer.data(), receiveBuffer.size(), MSG_DONTWAIT | MSG_TRUNC);
        if (length == 0) {
            closeClient();
            return;
        }
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            LOG_WARN("WDG API receive failed: %s", std::strerror(errno));
            closeClient();
            return;
        }
        if (static_cast<size_t>(length) > MAX_PACKET_BYTES) {
            if (!enqueueReply(errorReply(Json::Value(Json::nullValue), "packet_too_large", "Packet exceeds 65536 bytes")))
                return;
            continue;
        }
        handlePacket(receiveBuffer.data(), static_cast<size_t>(length));
        if (clientFd < 0)
            return;
    }
}

bool WdgApi::sanitizeTextPayload(const std::string &requested, std::string &sanitized)
{
    sanitized = sanitizeUtf8(reinterpret_cast<const uint8_t *>(requested.data()), requested.size());
    return !sanitized.empty() && sanitized.size() <= meshtastic_Constants_DATA_PAYLOAD_LEN;
}

void WdgApi::handlePacket(const char *data, size_t length)
{
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value command;
    std::string errors;
    if (!reader->parse(data, data + length, &command, &errors) || !command.isObject()) {
        enqueueReply(errorReply(Json::Value(Json::nullValue), "invalid_json", "Packet must be one JSON object"));
        return;
    }

    Json::Value requestId(Json::nullValue);
    if (command.isMember("request_id") && command["request_id"].isString() && !command["request_id"].asString().empty() &&
        command["request_id"].asString().size() <= MAX_REQUEST_ID_BYTES) {
        requestId = command["request_id"];
    } else {
        enqueueReply(errorReply(requestId, "invalid_request_id", "request_id must be a non-empty string of at most 128 bytes"));
        return;
    }

    if (!command.isMember("v") || !command["v"].isUInt() || command["v"].asUInt() != PROTOCOL_VERSION) {
        enqueueReply(errorReply(requestId, "unsupported_version", "Only protocol version 1 is supported"));
        return;
    }
    if (!command.isMember("type") || !command["type"].isString() || command["type"].asString() != "command") {
        enqueueReply(errorReply(requestId, "invalid_type", "type must be command"));
        return;
    }
    if (!command.isMember("name") || !command["name"].isString()) {
        enqueueReply(errorReply(requestId, "invalid_command", "name must be a string"));
        return;
    }
    if (command.isMember("body") && !command["body"].isObject()) {
        enqueueReply(errorReply(requestId, "invalid_body", "body must be an object"));
        return;
    }

    const std::string requestKey = requestId.asString();
    if (recentRequestIdSet.count(requestKey)) {
        enqueueReply(errorReply(requestId, "duplicate_request_id", "request_id was already used on this connection"));
        return;
    }
    recentRequestIds.push_back(requestKey);
    recentRequestIdSet.insert(requestKey);
    if (recentRequestIds.size() > MAX_RECENT_REQUEST_IDS) {
        recentRequestIdSet.erase(recentRequestIds.front());
        recentRequestIds.pop_front();
    }

    const std::string name = command["name"].asString();
    const Json::Value body = command.get("body", Json::Value(Json::objectValue));
    Json::Value reply = makeReply(requestId, true);
    Json::Value response(Json::objectValue);
    const char *deferredEvent = nullptr;
    Json::Value deferredBody(Json::objectValue);

    if (name == "hello") {
        response["protocol_version"] = PROTOCOL_VERSION;
        response["max_packet_bytes"] = static_cast<Json::UInt64>(MAX_PACKET_BYTES);
        Json::Value capabilities(Json::arrayValue);
        for (const char *capability : CAPABILITIES)
            capabilities.append(capability);
        response["capabilities"] = capabilities;
        clientHelloComplete = true;
        deferredEvent = "ready";
        deferredBody = makeStatusBody();
        lastPhoneConnected = deferredBody["phone_connected"].asBool();
        lastBleStatus = deferredBody["ble_status"].asString();
        lastRadioStatus = deferredBody["radio_status"].asString();
        lastFullClientOwner = deferredBody["full_client_owner"].asString();
    } else if (name == "get_status") {
        response = makeStatusBody();
    } else if (name == "snapshot_nodes") {
        if (!nodeDB) {
            enqueueReply(errorReply(requestId, "not_ready", "Node database is not ready"));
            return;
        }
        if (snapshotActive) {
            enqueueReply(errorReply(requestId, "busy", "A node snapshot is already active"));
            return;
        }
        snapshotNodes.clear();
        snapshotIndex = 0;
        const NodeNum self = nodeDB->getNodeNum();
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); ++i) {
            const meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (node && node->num && node->num != self)
                snapshotNodes.push_back(node->num);
        }
        snapshotActive = true;
        response["count"] = static_cast<Json::UInt64>(snapshotNodes.size());
        deferredEvent = "snapshot_begin";
        deferredBody["count"] = response["count"];
    } else if (name == "send_text") {
        if (!router || !service || !nodeDB) {
            enqueueReply(errorReply(requestId, "not_ready", "Meshtastic radio service is not ready"));
            return;
        }
        if (!body.isMember("text") || !body["text"].isString()) {
            enqueueReply(errorReply(requestId, "invalid_text", "text must be a string"));
            return;
        }
        const std::string requestedText = body["text"].asString();
        std::string text;
        if (!sanitizeTextPayload(requestedText, text)) {
            enqueueReply(errorReply(requestId, "invalid_text", "text must contain 1 to 233 bytes"));
            return;
        }

        NodeNum destination = NODENUM_BROADCAST;
        if (body.isMember("destination") && !body["destination"].isNull()) {
            if (!parseUnsigned(body["destination"], destination) || destination == 0) {
                enqueueReply(errorReply(requestId, "invalid_destination", "destination must be a node number or !xxxxxxxx ID"));
                return;
            }
        }
        const Json::Value channelValue = body.get("channel", 0);
        if (!channelValue.isUInt() || channelValue.asUInt() >= MAX_NUM_CHANNELS) {
            enqueueReply(errorReply(requestId, "invalid_channel", "channel is outside the configured channel table"));
            return;
        }
        if (body.isMember("want_ack") && !body["want_ack"].isBool()) {
            enqueueReply(errorReply(requestId, "invalid_want_ack", "want_ack must be a boolean"));
            return;
        }
        meshtastic_MeshPacket *packet = router->allocForSending();
        if (!packet) {
            enqueueReply(errorReply(requestId, "no_memory", "Could not allocate a mesh packet"));
            return;
        }
        if (!service->tryReserveTextMessageSend()) {
            service->releaseToPool(packet);
            enqueueReply(errorReply(requestId, "rate_limited", "Text messages can only be sent once every 2 seconds"));
            return;
        }
        packet->to = destination;
        packet->channel = static_cast<ChannelIndex>(channelValue.asUInt());
        packet->want_ack = body.get("want_ack", true).asBool();
        packet->decoded.dest = destination;
        packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        packet->decoded.payload.size = text.size();
        memcpy(packet->decoded.payload.bytes, text.data(), text.size());

        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(destination);
        if (node && destination != nodeDB->getNodeNum() && nodeInfoLiteHasUser(node) && node->public_key.size == 32) {
            packet->pki_encrypted = true;
            packet->channel = 0;
        }

        const ChannelIndex effectiveChannel = packet->channel;
        const PacketId packetId = packet->id;
        const ErrorCode result = service->sendToMeshWithResult(packet, RX_SRC_LOCAL, true);
        if (result != ERRNO_OK && result != ERRNO_SHOULD_RELEASE) {
            Json::Value failed(Json::objectValue);
            failed["request_id"] = requestId;
            failed["packet_id"] = packetId;
            failed["error"] = result;
            failed["message"] = "Meshtastic rejected the text packet";
            if (!enqueueReply(errorReply(requestId, "send_rejected", "Meshtastic rejected the text packet")))
                return;
            enqueueEvent("send_failed", failed);
            return;
        }
        response["packet_id"] = packetId;
        deferredEvent = "send_accepted";
        deferredBody["request_id"] = requestId;
        deferredBody["packet_id"] = packetId;
        deferredBody["destination"] = nodeId(destination);
        deferredBody["channel"] = effectiveChannel;
        deferredBody["text"] = text;
    } else if (name == "request_node_info") {
        if (body.isMember("hop_limit") && (!body["hop_limit"].isUInt() || body["hop_limit"].asUInt() != 0)) {
            enqueueReply(errorReply(requestId, "invalid_hop_limit", "WDG discovery is restricted to zero routed hops"));
            return;
        }
        if (!nodeInfoModule || !service || !router || !router->getRadioIface() || portduino_status.LoRa_in_error) {
            enqueueReply(errorReply(requestId, "not_ready", "Meshtastic radio and NodeInfo must be ready"));
            return;
        }
        if (discoverySent && Throttle::isWithinTimespanMs(lastDiscoveryMs, DISCOVERY_THROTTLE_MS)) {
            enqueueReply(errorReply(requestId, "rate_limited", "NodeInfo discovery can be sent once every 60 seconds"));
            return;
        }
        const ErrorCode result = nodeInfoModule->sendOurNodeInfoZeroHop(NODENUM_BROADCAST, true, 0, true);
        if (result != ERRNO_OK && result != ERRNO_SHOULD_RELEASE) {
            enqueueReply(errorReply(requestId, "discovery_rejected", "Meshtastic rejected the NodeInfo request"));
            return;
        }
        discoverySent = true;
        lastDiscoveryMs = Time::getMillis();
        response["hop_limit"] = 0;
        response["message"] = "Zero-hop NodeInfo request sent";
        deferredEvent = "discovery_sent";
        deferredBody["request_id"] = requestId;
        deferredBody["hop_limit"] = 0;
        deferredBody["message"] = response["message"];
    } else if (name == "set_phone_ble") {
        if (!body.isMember("enabled") || !body["enabled"].isBool()) {
            enqueueReply(errorReply(requestId, "invalid_enabled", "enabled must be a boolean"));
            return;
        }
        const bool enabled = body["enabled"].asBool();
        const WdgPolicy &policy = wdgPolicy();
        if (enabled && !wdgBluetoothAllowed()) {
            enqueueReply(errorReply(requestId, "ble_disabled_by_policy", "Phone Bluetooth is disabled by host policy"));
            return;
        }
        if (body.isMember("adapter") && !body["adapter"].isString()) {
            enqueueReply(errorReply(requestId, "invalid_adapter", "adapter must be auto, hciX, or a controller address"));
            return;
        }
        if (!enabled && body.isMember("adapter")) {
            enqueueReply(errorReply(requestId, "adapter_requires_enabled",
                                    "adapter cannot be changed while phone BLE is disabled; enable phone BLE to select it"));
            return;
        }
        // Omitting adapter preserves the current stable selection. An explicit
        // "auto" remains available when the caller wants to return to policy
        // based controller selection.
        const std::string requestedAdapter =
            body.isMember("adapter") ? body["adapter"].asString() : wdgPolicy().runtimeAdapterAddress;
        const std::string previousAdapter = portduino_config.bluetooth_adapter;
        std::string adapter = previousAdapter;
        if (enabled && policy.configured && policy.adapterAddress != "auto") {
            const std::string policyController = resolveWdgBluetoothAdapter(policy.adapterAddress);
            const std::string requestedController =
                requestedAdapter == "auto" ? policyController : resolveWdgBluetoothAdapter(requestedAdapter);
            if (requestedController != policyController) {
                enqueueReply(errorReply(requestId, "adapter_restricted", "Bluetooth adapter is fixed by host policy"));
                return;
            }
            adapter = policyController;
        } else if (enabled) {
            adapter = resolveWdgBluetoothAdapter(requestedAdapter);
        }
        if (enabled && adapter.empty()) {
            enqueueReply(errorReply(requestId, "invalid_adapter", "adapter must be auto, hciX, or a controller address"));
            return;
        }
        const bool adapterChanged = adapter != previousAdapter;
        if (enabled && linuxBluetooth && linuxBluetooth->isConnected() && adapterChanged) {
            enqueueReply(errorReply(requestId, "phone_connected", "Disconnect the phone before changing Bluetooth adapters"));
            return;
        }
        if (enabled && !selectWdgBluetoothAdapter(requestedAdapter)) {
            enqueueReply(errorReply(requestId, "invalid_adapter", "Bluetooth adapter became unavailable"));
            return;
        }
        if (adapterChanged) {
            releaseClientLeases();
            if (linuxBluetooth) {
                linuxBluetooth->closePairingWindow();
                linuxBluetooth->deinit();
                delete linuxBluetooth;
                linuxBluetooth = nullptr;
            }
            portduino_config.bluetooth_adapter = adapter;
        }
        config.bluetooth.enabled = enabled;
        portduino_config.bluetooth_enabled = enabled;
        if (!enabled) {
            releaseClientLeases();
            if (linuxBluetooth) {
                linuxBluetooth->closePairingWindow();
                linuxBluetooth->deinit();
            }
        } else {
            setBluetoothEnable(true);
            if (!linuxBluetooth || !linuxBluetooth->isEnabled()) {
                const bool fixedPinUnsupported = linuxBluetooth && linuxBluetooth->isFixedPinUnsupported();
                enqueueReply(errorReply(requestId, fixedPinUnsupported ? "fixed_pin_unsupported" : "ble_unavailable",
                                        fixedPinUnsupported
                                            ? "BlueZ cannot provide Meshtastic FIXED_PIN mode; select RANDOM_PIN or NO_PIN"
                                            : "BlueZ could not start the Meshtastic phone service"));
                return;
            }
        }
        response["enabled"] = enabled;
        // Report the stable selector retained by policy, rather than echoing a
        // request such as "auto" after policy resolved it to a fixed address.
        response["adapter"] = wdgPolicy().runtimeAdapterAddress;
        response["controller"] = portduino_config.bluetooth_adapter;
        response["adapter_address"] = selectedWdgBluetoothAdapterAddress();
    } else if (name == "forget_phone") {
        if (!linuxBluetooth || !linuxBluetooth->isEnabled()) {
            enqueueReply(errorReply(requestId, "ble_unavailable", "Meshtastic phone Bluetooth is unavailable"));
            return;
        }
        if (linuxBluetooth->isConnected()) {
            enqueueReply(errorReply(requestId, "phone_connected", "Disconnect the phone before removing its bond"));
            return;
        }
        linuxBluetooth->closePairingWindow();
        if (!linuxBluetooth->clearBonds()) {
            enqueueReply(errorReply(requestId, "bond_removal_failed", "BlueZ could not remove the Meshtastic phone bond"));
            return;
        }
        response["forgot"] = true;
    } else if (name == "open_pairing") {
        uint32_t seconds = 0;
        if (!body.isMember("seconds") || !parseUnsigned(body["seconds"], seconds) || seconds == 0) {
            enqueueReply(errorReply(requestId, "invalid_duration", "seconds must be a positive integer"));
            return;
        }
        seconds = std::min(seconds, LinuxBluetooth::MAX_PAIRING_WINDOW_SECONDS);
        if (wdgPolicy().configured)
            seconds = std::min(seconds, wdgPolicy().pairingWindowSeconds);
        if (!linuxBluetooth || !linuxBluetooth->isEnabled()) {
            enqueueReply(errorReply(requestId, "ble_unavailable", "Meshtastic phone Bluetooth is unavailable"));
            return;
        }
        if (linuxBluetooth->isConnected()) {
            enqueueReply(errorReply(requestId, "phone_connected", "A Meshtastic phone is already connected"));
            return;
        }
        if (linuxBluetooth->hasBondedPhone()) {
            enqueueReply(errorReply(requestId, "bond_exists", "Forget the bonded Meshtastic phone before pairing a replacement"));
            return;
        }
        uint32_t previousPasskey = 0;
        uint64_t previousPasskeyToken = 0;
        linuxBluetooth->getLatestPasskey(previousPasskey, previousPasskeyToken);
        if (!linuxBluetooth->openPairingWindow(seconds)) {
            enqueueReply(errorReply(requestId, "pairing_unavailable", "BlueZ could not open the pairing window"));
            return;
        }
        lastPasskeyToken = previousPasskeyToken;
        response["seconds"] = seconds;
        response["state"] = "pairing";
        lastBleStatus = "pairing";
        deferredEvent = "ble_status";
        deferredBody["state"] = "pairing";
        deferredBody["message"] = "Meshtastic phone pairing window open";
    } else if (name == "ble_scan_lease_acquire") {
        uint32_t seconds = 0;
        if (!body.isMember("seconds") || !parseUnsigned(body["seconds"], seconds) || seconds == 0) {
            enqueueReply(errorReply(requestId, "invalid_duration", "seconds must be a positive integer"));
            return;
        }
        seconds = std::min(seconds, MAX_BLE_SCAN_LEASE_SECONDS);
        if (!linuxBluetooth || !linuxBluetooth->isEnabled()) {
            response["granted"] = true;
            response["reason"] = "phone BLE disabled";
        } else if (linuxBluetooth->isConnected()) {
            // Keep the phone session intact and let the host make one bounded
            // concurrent discovery attempt. WDG degrades to phone priority if
            // BlueZ rejects discovery or the controller proves unstable.
            bleScanLeaseHeld = true;
            bleScanLeaseShared = true;
            bleScanLeaseStarted = Time::getMillis();
            bleScanLeaseDuration = seconds * 1000U;
            response["granted"] = true;
            response["shared"] = true;
            response["reason"] = "shared with connected phone";
            response["seconds"] = seconds;
        } else if (linuxBluetooth->isPairingWindowOpen()) {
            response["granted"] = false;
            response["reason"] = "phone pairing window is active";
        } else {
            linuxBluetooth->setScanSuspended(true);
            bleScanLeaseHeld = true;
            bleScanLeaseShared = false;
            bleScanLeaseStarted = Time::getMillis();
            bleScanLeaseDuration = seconds * 1000U;
            response["granted"] = true;
            response["reason"] = "adapter yielded";
            response["seconds"] = seconds;
        }
    } else if (name == "ble_scan_lease_release") {
        if (linuxBluetooth && bleScanLeaseHeld && !bleScanLeaseShared)
            linuxBluetooth->setScanSuspended(false);
        bleScanLeaseHeld = false;
        bleScanLeaseShared = false;
        bleScanLeaseStarted = 0;
        bleScanLeaseDuration = 0;
        response["released"] = true;
    } else if (name == "pairing_agent_lease_acquire") {
        uint32_t seconds = 0;
        if (!body.isMember("seconds") || !parseUnsigned(body["seconds"], seconds) || seconds == 0) {
            enqueueReply(errorReply(requestId, "invalid_duration", "seconds must be a positive integer"));
            return;
        }
        seconds = std::min(seconds, MAX_PAIRING_AGENT_LEASE_SECONDS);
        if (!linuxBluetooth || !linuxBluetooth->isEnabled()) {
            response["granted"] = true;
            response["reason"] = "phone BLE disabled";
        } else {
            linuxBluetooth->setPairingAgentSuspended(true);
            pairingAgentLeaseHeld = true;
            pairingAgentLeaseStarted = Time::getMillis();
            pairingAgentLeaseDuration = seconds * 1000U;
            response["granted"] = true;
            response["reason"] = "pairing agent yielded";
            response["seconds"] = seconds;
        }
    } else if (name == "pairing_agent_lease_release") {
        if (linuxBluetooth && pairingAgentLeaseHeld)
            linuxBluetooth->setPairingAgentSuspended(false);
        pairingAgentLeaseHeld = false;
        pairingAgentLeaseStarted = 0;
        pairingAgentLeaseDuration = 0;
        response["released"] = true;
    } else if (name == "retry_shared_adapter") {
        if (linuxBluetooth) {
            if (bleScanLeaseHeld && !bleScanLeaseShared)
                linuxBluetooth->setScanSuspended(false);
            linuxBluetooth->retrySharedAdapter();
        }
        bleScanLeaseHeld = false;
        bleScanLeaseShared = false;
        bleScanLeaseStarted = 0;
        bleScanLeaseDuration = 0;
        response["retried"] = true;
    } else {
        enqueueReply(errorReply(requestId, "unsupported_command", "Command is not implemented by this build"));
        return;
    }
    reply["body"] = response;
    if (!enqueueReply(compactJson(reply)))
        return;
    if (name == "hello")
        observerEventsEnabled.store(true, std::memory_order_release);
    if (deferredEvent)
        enqueueEvent(deferredEvent, deferredBody);
}

bool WdgApi::enqueueReply(std::string reply)
{
    return enqueuePacket(std::move(reply), false);
}

bool WdgApi::enqueueEvent(const char *name, const Json::Value &body, bool lowPriority)
{
    Json::Value event(Json::objectValue);
    event["v"] = PROTOCOL_VERSION;
    event["type"] = "event";
    event["event_id"] = static_cast<Json::UInt64>(nextEventId++);
    event["name"] = name;
    event["body"] = body;
    return enqueuePacket(compactJson(event), lowPriority);
}

bool WdgApi::enqueuePacket(std::string packet, bool lowPriority)
{
    if (clientFd < 0)
        return false;
    if (packet.size() > MAX_PACKET_BYTES) {
        if (lowPriority) {
            droppedEvents++;
            return false;
        }
        LOG_WARN("WDG API generated an oversized reply; disconnecting client");
        closeClient();
        return false;
    }

    while (pendingReplies.size() >= MAX_PENDING_REPLIES || pendingBytes + packet.size() > MAX_PENDING_REPLY_BYTES) {
        auto droppable = std::find_if(pendingReplies.begin(), pendingReplies.end(),
                                      [](const PendingPacket &candidate) { return candidate.lowPriority; });
        if (droppable == pendingReplies.end()) {
            if (lowPriority) {
                droppedEvents++;
                return false;
            }
            LOG_WARN("WDG API reply queue overflow; disconnecting client");
            closeClient();
            return false;
        }
        pendingBytes -= droppable->data.size();
        pendingReplies.erase(droppable);
        droppedEvents++;
    }

    pendingBytes += packet.size();
    pendingReplies.push_back({std::move(packet), lowPriority});
    return true;
}

void WdgApi::flushReplies()
{
    while (clientFd >= 0 && !pendingReplies.empty()) {
        const std::string &reply = pendingReplies.front().data;
        ssize_t sent = send(clientFd, reply.data(), reply.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            LOG_WARN("WDG API send failed: %s", std::strerror(errno));
            closeClient();
            return;
        }
        if (static_cast<size_t>(sent) != reply.size()) {
            LOG_WARN("WDG API partial packet write; disconnecting client");
            closeClient();
            return;
        }

        pendingBytes -= reply.size();
        pendingReplies.pop_front();
    }
}

Json::Value WdgApi::makeStatusBody() const
{
    Json::Value body(Json::objectValue);
    body["state"] = "ready";
    body["client_connected"] = clientFd >= 0;
    body["pending_replies"] = static_cast<Json::UInt64>(pendingReplies.size());
    body["packets_received"] = static_cast<Json::UInt64>(packetsReceived.load(std::memory_order_relaxed));
    body["radio_status"] = router && router->getRadioIface() && !portduino_status.LoRa_in_error ? "ready" : "unavailable";
    body["full_client_owner"] = fullPhoneApiOwnerName(fullPhoneApiLease().owner());

    Json::Value identity(Json::objectValue);
    if (nodeDB)
        identity["node_id"] = nodeId(nodeDB->getNodeNum());
    identity["name"] = boundedUtf8(owner.long_name, sizeof(owner.long_name));
    identity["short_name"] = boundedUtf8(owner.short_name, sizeof(owner.short_name));
    identity["has_public_key"] = config.has_security && config.security.public_key.size > 0;
    identity["has_private_key"] = config.has_security && config.security.private_key.size > 0;
    body["identity"] = identity;

    Json::Value configuredChannels(Json::arrayValue);
    for (pb_size_t i = 0; i < channelFile.channels_count; ++i) {
        const meshtastic_Channel &channel = channelFile.channels[i];
        if (!channel.has_settings || channel.role == meshtastic_Channel_Role_DISABLED)
            continue;
        Json::Value item(Json::objectValue);
        item["index"] = channel.index;
        item["name"] = boundedUtf8(channel.settings.name, sizeof(channel.settings.name));
        item["role"] = channel.role;
        item["has_psk"] = channel.settings.psk.size > 0;
        configuredChannels.append(item);
    }
    body["channels"] = configuredChannels;

    const bool bleEnabled = linuxBluetooth && linuxBluetooth->isEnabled();
    const bool phoneConnected = bleEnabled && linuxBluetooth->isConnected();
    body["phone_connected"] = phoneConnected;
    if (!wdgBluetoothAllowed())
        body["ble_status"] = "disabled_by_policy";
    else if (linuxBluetooth && linuxBluetooth->isFixedPinUnsupported())
        body["ble_status"] = "fixed_pin_unsupported";
    else if (!bleEnabled)
        body["ble_status"] = "unavailable";
    else if (phoneConnected)
        body["ble_status"] = "connected";
    else if (linuxBluetooth->isPairingWindowOpen())
        body["ble_status"] = "pairing";
    else if (bleScanLeaseHeld)
        body["ble_status"] = "scan_lease";
    else if (pairingAgentLeaseHeld)
        body["ble_status"] = "pairing_agent_lease";
    else if (linuxBluetooth->isAdvertising())
        body["ble_status"] = "advertising";
    else
        body["ble_status"] = "ready";
    body["phone_ble_enabled"] = bleEnabled;
    body["phone_ble_adapter"] = portduino_config.bluetooth_adapter;
    body["phone_ble_adapter_address"] = selectedWdgBluetoothAdapterAddress();
    body["ble_scan_lease_active"] = bleScanLeaseHeld;
    body["ble_scan_lease_shared"] = bleScanLeaseShared;
    body["pairing_agent_lease_active"] = pairingAgentLeaseHeld;
    body["phone_ble_policy_enabled"] = wdgBluetoothAllowed();
    body["phone_ble_pairing_mode"] = pairingModeName(config.bluetooth.mode);
    body["phone_ble_pairing_authenticated"] = config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
    if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN)
        body["phone_ble_pairing_error"] = "fixed_pin_unsupported";
    return body;
}

Json::Value WdgApi::makeNodeBody(NodeNum nodeNum, bool cached) const
{
    Json::Value body(Json::objectValue);
    if (!nodeDB)
        return body;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeNum);
    if (!node)
        return body;

    body["id"] = nodeId(node->num);
    body["num"] = node->num;
    body["name"] = boundedUtf8(node->long_name, sizeof(node->long_name));
    body["short_name"] = boundedUtf8(node->short_name, sizeof(node->short_name));
    body["hardware"] = node->hw_model;
    body["rssi"] = 0;
    body["snr"] = nodeInfoLiteHasSnr(node) ? node->snr : 0;
    body["hops"] = node->has_hops_away ? node->hops_away : 0;
    body["last_heard"] = node->last_heard;
    body["cached"] = cached;

    meshtastic_PositionLite position = meshtastic_PositionLite_init_zero;
    if (nodeDB->copyNodePosition(nodeNum, position)) {
        body["lat"] = static_cast<double>(position.latitude_i) / 10000000.0;
        body["lon"] = static_cast<double>(position.longitude_i) / 10000000.0;
    } else {
        body["lat"] = 0;
        body["lon"] = 0;
    }
    return body;
}

int WdgApi::onNodeChanged(NodeNum nodeNum)
{
    if (!nodeNum || !observerEventsEnabled.load(std::memory_order_acquire))
        return 0;
    std::lock_guard<std::mutex> guard(observerInboxMutex);
    if (!observerEventsEnabled.load(std::memory_order_relaxed))
        return 0;
    if (observerNodeUpdates.size() >= MAX_OBSERVER_NODE_UPDATES && !observerNodeUpdates.count(nodeNum)) {
        observerDroppedEvents++;
        return 0;
    }
    observerNodeUpdates.insert(nodeNum);
    return 0;
}

int WdgApi::onRemotePacketAccepted(const meshtastic_MeshPacket *packet)
{
    if (packet)
        packetsReceived.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int WdgApi::onTextMessage(const meshtastic_MeshPacket *packet)
{
    if (!observerEventsEnabled.load(std::memory_order_acquire) || !packet ||
        packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        packet->decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP)
        return 0;

    PendingTextEvent event;
    const size_t payloadSize = std::min<size_t>(packet->decoded.payload.size, sizeof(packet->decoded.payload.bytes));
    event.text.assign(reinterpret_cast<const char *>(packet->decoded.payload.bytes), payloadSize);
    event.sender = getFrom(packet);
    event.channel = packet->channel;
    event.rssi = packet->has_rx_rssi ? packet->rx_rssi : 0;
    event.snr = packet->rx_snr;
    event.hops = std::max<int8_t>(0, getHopsAway(*packet));
    event.packetId = packet->id;

    std::lock_guard<std::mutex> guard(observerInboxMutex);
    if (!observerEventsEnabled.load(std::memory_order_relaxed))
        return 0;
    while (!observerTextEvents.empty() && (observerTextEvents.size() >= MAX_OBSERVER_TEXT_EVENTS ||
                                           observerTextBytes + event.text.size() > MAX_OBSERVER_TEXT_BYTES)) {
        observerTextBytes -= observerTextEvents.front().text.size();
        observerTextEvents.pop_front();
        observerDroppedEvents++;
    }
    if (event.text.size() > MAX_OBSERVER_TEXT_BYTES) {
        observerDroppedEvents++;
        return 0;
    }
    observerTextBytes += event.text.size();
    observerTextEvents.push_back(std::move(event));
    return 0;
}

void WdgApi::drainObserverInbox()
{
    std::set<NodeNum> nodes;
    std::deque<PendingTextEvent> messages;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> guard(observerInboxMutex);
        nodes.swap(observerNodeUpdates);
        messages.swap(observerTextEvents);
        observerTextBytes = 0;
        dropped = observerDroppedEvents;
        observerDroppedEvents = 0;
    }

    const NodeNum self = nodeDB ? nodeDB->getNodeNum() : 0;
    for (NodeNum nodeNum : nodes) {
        if (nodeNum == self)
            continue;
        if (pendingNodeUpdates.size() >= MAX_OBSERVER_NODE_UPDATES && !pendingNodeUpdates.count(nodeNum)) {
            dropped++;
            continue;
        }
        pendingNodeUpdates.insert(nodeNum);
    }
    droppedEvents += dropped;

    for (const PendingTextEvent &message : messages) {
        Json::Value body(Json::objectValue);
        body["text"] = sanitizeUtf8(reinterpret_cast<const uint8_t *>(message.text.data()), message.text.size());
        body["sender_id"] = nodeId(message.sender);
        const meshtastic_NodeInfoLite *sender = nodeDB ? nodeDB->getMeshNode(message.sender) : nullptr;
        body["sender"] = sender ? boundedUtf8(sender->long_name, sizeof(sender->long_name)) : nodeId(message.sender);
        body["channel"] = message.channel;
        body["rssi"] = message.rssi;
        body["snr"] = message.snr;
        body["hops"] = message.hops;
        body["packet_id"] = message.packetId;
        if (!enqueueEvent("message", body))
            break;
    }
}

void WdgApi::ensureObservers()
{
    if (!observingNodeDb && nodeDB) {
        nodeObserver.observe(&nodeDB->wdgNodeChanged);
        observingNodeDb = true;
    }
    if (!observingRemotePackets && service) {
        remotePacketObserver.observe(&service->wdgRemotePacketAccepted);
        observingRemotePackets = true;
    }
    if (!observingText && textMessageModule) {
        textObserver.observe(textMessageModule);
        observingText = true;
    }
}

void WdgApi::pumpSnapshot()
{
    if (!snapshotActive || clientFd < 0)
        return;
    size_t emitted = 0;
    while (snapshotIndex < snapshotNodes.size() && emitted < MAX_EVENTS_PER_POLL &&
           pendingReplies.size() < SNAPSHOT_QUEUE_WATERMARK) {
        Json::Value body = makeNodeBody(snapshotNodes[snapshotIndex++], true);
        if (!body.empty())
            enqueueEvent("node", body, true);
        emitted++;
    }
    if (snapshotIndex == snapshotNodes.size()) {
        Json::Value complete(Json::objectValue);
        complete["count"] = static_cast<Json::UInt64>(snapshotNodes.size());
        enqueueEvent("snapshot_complete", complete);
        snapshotNodes.clear();
        snapshotIndex = 0;
        snapshotActive = false;
    }
}

void WdgApi::pumpNodeUpdates()
{
    if (clientFd < 0 || snapshotActive)
        return;
    if (droppedEvents && pendingReplies.size() < SNAPSHOT_QUEUE_WATERMARK) {
        Json::Value overflow(Json::objectValue);
        overflow["count"] = static_cast<Json::UInt64>(droppedEvents);
        overflow["resync_required"] = true;
        const size_t count = droppedEvents;
        droppedEvents = 0;
        if (!enqueueEvent("overflow", overflow))
            droppedEvents += count;
    }

    for (size_t emitted = 0; emitted < MAX_EVENTS_PER_POLL && !pendingNodeUpdates.empty(); ++emitted) {
        const NodeNum nodeNum = *pendingNodeUpdates.begin();
        pendingNodeUpdates.erase(pendingNodeUpdates.begin());
        Json::Value body = makeNodeBody(nodeNum, false);
        if (!body.empty())
            enqueueEvent("node", body, true);
    }
}

void WdgApi::expireBluetoothLeases()
{
    if (bleScanLeaseHeld && intervalExpired(bleScanLeaseStarted, bleScanLeaseDuration)) {
        if (linuxBluetooth && !bleScanLeaseShared)
            linuxBluetooth->setScanSuspended(false);
        bleScanLeaseHeld = false;
        bleScanLeaseShared = false;
        bleScanLeaseStarted = 0;
        bleScanLeaseDuration = 0;
    }
    if (pairingAgentLeaseHeld && intervalExpired(pairingAgentLeaseStarted, pairingAgentLeaseDuration)) {
        if (linuxBluetooth)
            linuxBluetooth->setPairingAgentSuspended(false);
        pairingAgentLeaseHeld = false;
        pairingAgentLeaseStarted = 0;
        pairingAgentLeaseDuration = 0;
    }
}

void WdgApi::pollRuntimeState()
{
    if (!clientHelloComplete || clientFd < 0)
        return;

    const bool connectedNow = linuxBluetooth && linuxBluetooth->isEnabled() && linuxBluetooth->isConnected();
    bool scanLeaseRevoked = false;
    if (connectedNow && bleScanLeaseHeld && !bleScanLeaseShared) {
        linuxBluetooth->setScanSuspended(false);
        bleScanLeaseHeld = false;
        bleScanLeaseShared = false;
        bleScanLeaseStarted = 0;
        bleScanLeaseDuration = 0;
        scanLeaseRevoked = true;
    } else if (!connectedNow && bleScanLeaseHeld && bleScanLeaseShared) {
        // The phone left while a shared scan was active. Keep the scan alive,
        // but suppress advertising until its bounded lease ends.
        linuxBluetooth->setScanSuspended(true);
        bleScanLeaseShared = false;
    }

    const bool bleEnabled = linuxBluetooth && linuxBluetooth->isEnabled();
    const bool phoneConnected = bleEnabled && linuxBluetooth->isConnected();
    if (phoneConnected != lastPhoneConnected) {
        Json::Value phone(Json::objectValue);
        phone["phone_connected"] = phoneConnected;
        phone["message"] = phoneConnected ? "Meshtastic phone connected" : "Meshtastic phone disconnected";
        if (!enqueueEvent(phoneConnected ? "phone_connected" : "phone_disconnected", phone))
            return;
        lastPhoneConnected = phoneConnected;
    }

    std::string bleStatus;
    if (!wdgBluetoothAllowed())
        bleStatus = "disabled_by_policy";
    else if (linuxBluetooth && linuxBluetooth->isFixedPinUnsupported())
        bleStatus = "fixed_pin_unsupported";
    else if (!bleEnabled)
        bleStatus = "unavailable";
    else if (phoneConnected)
        bleStatus = "connected";
    else if (linuxBluetooth->isPairingWindowOpen())
        bleStatus = "pairing";
    else if (bleScanLeaseHeld)
        bleStatus = "scan_lease";
    else if (pairingAgentLeaseHeld)
        bleStatus = "pairing_agent_lease";
    else if (linuxBluetooth->isAdvertising())
        bleStatus = "advertising";
    else
        bleStatus = "ready";
    if (bleStatus != lastBleStatus) {
        Json::Value ble(Json::objectValue);
        ble["state"] = bleStatus;
        ble["phone_connected"] = phoneConnected;
        ble["advertising"] = linuxBluetooth && linuxBluetooth->isAdvertising();
        ble["scan_lease_active"] = bleScanLeaseHeld;
        ble["scan_lease_shared"] = bleScanLeaseShared;
        ble["pairing_agent_lease_active"] = pairingAgentLeaseHeld;
        if (scanLeaseRevoked) {
            ble["host_ble_paused"] = true;
            ble["reason"] = "Meshtastic phone connected during host Bluetooth scan";
        }
        if (!enqueueEvent("ble_status", ble))
            return;
        lastBleStatus = bleStatus;
    }

    const std::string radioStatus =
        router && router->getRadioIface() && !portduino_status.LoRa_in_error ? "ready" : "unavailable";
    if (radioStatus != lastRadioStatus) {
        Json::Value radio(Json::objectValue);
        radio["state"] = radioStatus;
        if (!enqueueEvent("radio_status", radio))
            return;
        lastRadioStatus = radioStatus;
    }

    const std::string fullClientOwner = fullPhoneApiOwnerName(fullPhoneApiLease().owner());
    if (fullClientOwner != lastFullClientOwner) {
        Json::Value status(Json::objectValue);
        status["full_client_owner"] = fullClientOwner;
        if (!enqueueEvent("status", status))
            return;
        lastFullClientOwner = fullClientOwner;
    }

    if (linuxBluetooth) {
        uint32_t passkey = 0;
        uint64_t token = 0;
        if (linuxBluetooth->isPairingWindowOpen() && linuxBluetooth->getLatestPasskey(passkey, token) &&
            token != lastPasskeyToken) {
            char value[7];
            snprintf(value, sizeof(value), "%06u", passkey % 1000000U);
            Json::Value pairing(Json::objectValue);
            pairing["passkey"] = value;
            pairing["pin"] = value;
            pairing["message"] = "Enter this passkey in the Meshtastic phone app";
            if (!enqueueEvent("pairing_passkey", pairing))
                return;
            lastPasskeyToken = token;
        }
    }
}

void WdgApi::releaseClientLeases()
{
    if (linuxBluetooth && bleScanLeaseHeld && !bleScanLeaseShared)
        linuxBluetooth->setScanSuspended(false);
    if (linuxBluetooth && pairingAgentLeaseHeld)
        linuxBluetooth->setPairingAgentSuspended(false);
    bleScanLeaseHeld = false;
    bleScanLeaseShared = false;
    pairingAgentLeaseHeld = false;
    bleScanLeaseStarted = 0;
    bleScanLeaseDuration = 0;
    pairingAgentLeaseStarted = 0;
    pairingAgentLeaseDuration = 0;
}

void WdgApi::closeClient()
{
    observerEventsEnabled.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(observerInboxMutex);
        observerNodeUpdates.clear();
        observerTextEvents.clear();
        observerTextBytes = 0;
        observerDroppedEvents = 0;
    }
    releaseClientLeases();
    if (clientFd >= 0) {
        close(clientFd);
        clientFd = -1;
    }
    pendingReplies.clear();
    pendingBytes = 0;
    snapshotNodes.clear();
    snapshotIndex = 0;
    snapshotActive = false;
    pendingNodeUpdates.clear();
    droppedEvents = 0;
    clientHelloComplete = false;
    lastPhoneConnected = false;
    lastBleStatus.clear();
    lastRadioStatus.clear();
    lastFullClientOwner.clear();
    lastPasskeyToken = 0;
    recentRequestIds.clear();
    recentRequestIdSet.clear();
}

void WdgApi::closeListener()
{
    if (listenFd >= 0) {
        close(listenFd);
        listenFd = -1;
    }
    if (ownsSocketPath) {
        unlink(socketPath.c_str());
        ownsSocketPath = false;
    }
}

int32_t WdgApi::runOnce()
{
    ensureObservers();
    if (listenFd < 0)
        return start() ? IDLE_POLL_MS : RETRY_POLL_MS;

    acceptClients();
    receiveCommands();
    drainObserverInbox();
    expireBluetoothLeases();
    pollRuntimeState();
    pumpSnapshot();
    pumpNodeUpdates();
    flushReplies();
    return clientFd >= 0 || !pendingReplies.empty() ? ACTIVE_POLL_MS : IDLE_POLL_MS;
}

} // namespace meshtastic::portduino

#endif
