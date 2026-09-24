#include "platform/portduino/WdgApi.h"

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "DebugConfiguration.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <json/json.h>
#include <memory>
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

bool parseAllowedUid(uid_t &uid)
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

} // namespace

WdgApi::WdgApi(std::string socketPath, PeerAuthorizer peerAuthorizer)
    : concurrency::OSThread("WdgApi"), socketPath(std::move(socketPath)), peerAuthorizer(std::move(peerAuthorizer))
{
}

WdgApi::~WdgApi()
{
    stop();
}

std::string WdgApi::resolveSocketPath()
{
    const char *overridePath = std::getenv("MESHTASTIC_WDG_SOCKET");
    return overridePath && *overridePath ? overridePath : DEFAULT_SOCKET_PATH;
}

bool WdgApi::start()
{
    if (listenFd >= 0)
        return true;
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

    struct stat existing{};
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

    if (listen(fd, 1) != 0) {
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
    while (listenFd >= 0) {
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

    uid_t allowedUid = 0;
    if (parseAllowedUid(allowedUid) && credentials.uid == allowedUid)
        return true;
    return credentials.uid == 0 || credentials.uid == geteuid();
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
    if (command.isMember("request_id") && command["request_id"].isString() &&
        command["request_id"].asString().size() <= MAX_REQUEST_ID_BYTES) {
        requestId = command["request_id"];
    } else {
        enqueueReply(errorReply(requestId, "invalid_request_id", "request_id must be a string of at most 128 bytes"));
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

    const std::string name = command["name"].asString();
    Json::Value reply = makeReply(requestId, true);
    Json::Value body(Json::objectValue);
    if (name == "hello") {
        body["protocol_version"] = PROTOCOL_VERSION;
        body["max_packet_bytes"] = static_cast<Json::UInt64>(MAX_PACKET_BYTES);
        Json::Value capabilities(Json::arrayValue);
        capabilities.append("hello");
        capabilities.append("get_status");
        body["capabilities"] = capabilities;
    } else if (name == "get_status") {
        body["state"] = "ready";
        body["client_connected"] = true;
        body["pending_replies"] = static_cast<Json::UInt64>(pendingReplies.size());
    } else {
        enqueueReply(errorReply(requestId, "unsupported_command", "Command is not implemented by this build"));
        return;
    }
    reply["body"] = body;
    enqueueReply(compactJson(reply));
}

bool WdgApi::enqueueReply(std::string reply)
{
    if (reply.size() > MAX_PACKET_BYTES || pendingReplies.size() >= MAX_PENDING_REPLIES ||
        pendingBytes + reply.size() > MAX_PENDING_REPLY_BYTES) {
        LOG_WARN("WDG API reply queue overflow; disconnecting client");
        closeClient();
        return false;
    }

    pendingBytes += reply.size();
    pendingReplies.push_back(std::move(reply));
    return true;
}

void WdgApi::flushReplies()
{
    while (clientFd >= 0 && !pendingReplies.empty()) {
        const std::string &reply = pendingReplies.front();
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

void WdgApi::closeClient()
{
    if (clientFd >= 0) {
        close(clientFd);
        clientFd = -1;
    }
    pendingReplies.clear();
    pendingBytes = 0;
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
    if (listenFd < 0)
        return start() ? IDLE_POLL_MS : RETRY_POLL_MS;

    acceptClients();
    receiveCommands();
    flushReplies();
    return clientFd >= 0 || !pendingReplies.empty() ? ACTIVE_POLL_MS : IDLE_POLL_MS;
}

} // namespace meshtastic::portduino

#endif
