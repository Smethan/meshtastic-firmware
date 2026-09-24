#pragma once

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "concurrency/OSThread.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <sys/types.h>

namespace meshtastic::portduino
{

struct WdgPeerCredentials {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

class WdgApi : public concurrency::OSThread
{
  public:
    static constexpr uint32_t PROTOCOL_VERSION = 1;
    static constexpr size_t MAX_PACKET_BYTES = 65536;
    static constexpr size_t MAX_PENDING_REPLIES = 64;
    static constexpr size_t MAX_PENDING_REPLY_BYTES = 256 * 1024;

    using PeerAuthorizer = std::function<bool(const WdgPeerCredentials &)>;

    explicit WdgApi(std::string socketPath = resolveSocketPath(), PeerAuthorizer peerAuthorizer = {});
    ~WdgApi() override;

    bool start();
    void stop();
    bool isListening() const { return listenFd >= 0; }
    bool hasClient() const { return clientFd >= 0; }
    size_t pendingReplyCount() const { return pendingReplies.size(); }
    size_t pendingReplyBytes() const { return pendingBytes; }
    const std::string &getSocketPath() const { return socketPath; }

    static std::string resolveSocketPath();

  protected:
    int32_t runOnce() override;

  private:
    bool openListener();
    void acceptClients();
    void receiveCommands();
    void flushReplies();
    void handlePacket(const char *data, size_t length);
    bool enqueueReply(std::string reply);
    bool authorizePeer(const WdgPeerCredentials &credentials) const;
    void closeClient();
    void closeListener();

    std::string socketPath;
    PeerAuthorizer peerAuthorizer;
    int listenFd = -1;
    int clientFd = -1;
    bool ownsSocketPath = false;
    std::array<char, MAX_PACKET_BYTES> receiveBuffer{};
    std::deque<std::string> pendingReplies;
    size_t pendingBytes = 0;
};

} // namespace meshtastic::portduino

#endif
