#pragma once

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "MeshPacketQueue.h"
#include "RadioInterface.h"
#include "concurrency/OSThread.h"

#include <json/json.h>
#include <string>

/// Meshtastic framing/routing stays in the daemon; this interface delegates
/// only bounded SX1262 PHY operations to watchdogs-sx1262d.
class BrokerRadioInterface final : public RadioInterface, protected concurrency::OSThread
{
  public:
    explicit BrokerRadioInterface(std::string socketPath);
    ~BrokerRadioInterface() override;

    bool init() override;
    bool reconfigure() override;
    bool sleep() override;
    bool canSleep(bool deepSleep = false) override;
    ErrorCode send(meshtastic_MeshPacket *packet) override;
    meshtastic_QueueStatus getQueueStatus() override;
    bool cancelSending(NodeNum from, PacketId id) override;
    bool findInTxQueue(NodeNum from, PacketId id) override;
    uint32_t getPacketTime(uint32_t length, bool received = false) override;

  protected:
    int16_t getCurrentRSSI() override { return lastRssi; }
    int32_t runOnce() override;

  private:
    bool connectBroker();
    void disconnectBroker(bool dropQueued);
    bool request(const char *operation, const Json::Value &arguments, Json::Value &result);
    bool configureBroker();
    void processSocket();
    void processEvent(const Json::Value &event);
    void processRx(const Json::Value &event);
    void scheduleTransmit();
    void tryTransmit();
    void completeTransmit(bool success);
    void dropQueue();

    MeshPacketQueue txQueue{MAX_TX_QUEUE};
    std::string socketPath;
    int socketFd = -1;
    uint64_t requestId = 0;
    uint64_t generation = 0;
    bool leaseReady = false;
    bool txInFlight = false;
    bool lastCadBusy = false;
    uint32_t nextTxAt = 0;
    int16_t lastRssi = 0;
};

#endif
