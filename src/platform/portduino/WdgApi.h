#pragma once

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "MeshTypes.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <unordered_set>
#include <vector>

namespace Json
{
class Value;
}

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
    static constexpr size_t MAX_PENDING_REPLIES = 256;
    static constexpr size_t MAX_PENDING_REPLY_BYTES = 1024 * 1024;
    static constexpr size_t MAX_ACCEPTS_PER_POLL = 8;

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
    static bool applyPathAccessControl(const std::string &path, uid_t allowedUid, bool directory);
    static bool sanitizeTextPayload(const std::string &requested, std::string &sanitized);

  private:
    bool openListener();
    void acceptClients();
    void receiveCommands();
    void flushReplies();
    void handlePacket(const char *data, size_t length);
    bool enqueueReply(std::string reply);
    bool enqueueEvent(const char *name, const Json::Value &body, bool lowPriority = false);
    bool enqueuePacket(std::string packet, bool lowPriority);
    void pumpSnapshot();
    void drainObserverInbox();
    void pumpNodeUpdates();
    void expireBluetoothLeases();
    void pollRuntimeState();
    Json::Value makeStatusBody() const;
    Json::Value makeNodeBody(NodeNum nodeNum, bool cached) const;
    int onNodeChanged(NodeNum nodeNum);
    int onRemotePacketAccepted(const meshtastic_MeshPacket *packet);
    int onTextMessage(const meshtastic_MeshPacket *packet);
    void ensureObservers();
    void releaseClientLeases();
    bool authorizePeer(const WdgPeerCredentials &credentials) const;
    void closeClient();
    void closeListener();

    struct PendingPacket {
        std::string data;
        bool lowPriority = false;
    };

    struct PendingTextEvent {
        std::string text;
        NodeNum sender = 0;
        uint8_t channel = 0;
        int16_t rssi = 0;
        float snr = 0;
        int8_t hops = 0;
        PacketId packetId = 0;
    };

    std::string socketPath;
    PeerAuthorizer peerAuthorizer;
    int listenFd = -1;
    int clientFd = -1;
    bool ownsSocketPath = false;
    std::array<char, MAX_PACKET_BYTES> receiveBuffer{};
    std::deque<PendingPacket> pendingReplies;
    size_t pendingBytes = 0;
    uint64_t nextEventId = 1;
    size_t droppedEvents = 0;
    std::vector<NodeNum> snapshotNodes;
    size_t snapshotIndex = 0;
    bool snapshotActive = false;
    std::set<NodeNum> pendingNodeUpdates;
    std::atomic<uint64_t> packetsReceived{0};
    bool observingNodeDb = false;
    bool observingRemotePackets = false;
    bool observingText = false;
    std::atomic<bool> observerEventsEnabled{false};
    std::mutex observerInboxMutex;
    std::set<NodeNum> observerNodeUpdates;
    std::deque<PendingTextEvent> observerTextEvents;
    size_t observerTextBytes = 0;
    size_t observerDroppedEvents = 0;
    bool clientHelloComplete = false;
    bool bleScanLeaseHeld = false;
    bool bleScanLeaseShared = false;
    bool pairingAgentLeaseHeld = false;
    uint32_t bleScanLeaseStarted = 0;
    uint32_t bleScanLeaseDuration = 0;
    uint32_t pairingAgentLeaseStarted = 0;
    uint32_t pairingAgentLeaseDuration = 0;
    bool discoverySent = false;
    uint32_t lastDiscoveryMs = 0;
    bool lastPhoneConnected = false;
    std::string lastBleStatus;
    std::string lastRadioStatus;
    std::string lastFullClientOwner;
    uint64_t lastPasskeyToken = 0;
    std::deque<std::string> recentRequestIds;
    std::unordered_set<std::string> recentRequestIdSet;
    CallbackObserver<WdgApi, NodeNum> nodeObserver = CallbackObserver<WdgApi, NodeNum>(this, &WdgApi::onNodeChanged);
    CallbackObserver<WdgApi, const meshtastic_MeshPacket *> remotePacketObserver =
        CallbackObserver<WdgApi, const meshtastic_MeshPacket *>(this, &WdgApi::onRemotePacketAccepted);
    CallbackObserver<WdgApi, const meshtastic_MeshPacket *> textObserver =
        CallbackObserver<WdgApi, const meshtastic_MeshPacket *>(this, &WdgApi::onTextMessage);
};

} // namespace meshtastic::portduino

#endif
