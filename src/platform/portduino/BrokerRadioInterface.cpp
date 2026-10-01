#include "BrokerRadioInterface.h"

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "LinuxBluetooth.h"
#include "MeshService.h"
#include "PortduinoGlue.h"
#include "RadioTxHook.h"
#include "Router.h"
#include "airtime.h"
#include "mesh/NodeDB.h"
#include "target_specific.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern LinuxBluetooth *linuxBluetooth;

namespace
{
constexpr size_t MAX_BROKER_PACKET = 4096;

std::string compactJson(const Json::Value &value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool parseJson(const char *data, size_t length, Json::Value &result)
{
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(data, data + length, &result, &errors) && result.isObject();
}

std::string encodeBase64(const uint8_t *data, size_t length)
{
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((length + 2) / 3) * 4);
    for (size_t index = 0; index < length; index += 3) {
        uint32_t value = static_cast<uint32_t>(data[index]) << 16;
        const size_t remaining = length - index;
        if (remaining > 1)
            value |= static_cast<uint32_t>(data[index + 1]) << 8;
        if (remaining > 2)
            value |= data[index + 2];
        output.push_back(table[(value >> 18) & 0x3f]);
        output.push_back(table[(value >> 12) & 0x3f]);
        output.push_back(remaining > 1 ? table[(value >> 6) & 0x3f] : '=');
        output.push_back(remaining > 2 ? table[value & 0x3f] : '=');
    }
    return output;
}

bool decodeBase64(const std::string &input, uint8_t *output, size_t capacity, size_t &length)
{
    static std::array<int8_t, 256> decode = [] {
        std::array<int8_t, 256> result{};
        result.fill(-1);
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int index = 0; alphabet[index]; ++index)
            result[static_cast<uint8_t>(alphabet[index])] = static_cast<int8_t>(index);
        return result;
    }();
    if (input.empty() || input.size() % 4 != 0)
        return false;
    length = 0;
    for (size_t index = 0; index < input.size(); index += 4) {
        uint32_t value = 0;
        int padding = 0;
        for (int offset = 0; offset < 4; ++offset) {
            const unsigned char byte = input[index + offset];
            if (byte == '=') {
                if (offset < 2)
                    return false;
                ++padding;
                value <<= 6;
            } else {
                if (padding || decode[byte] < 0)
                    return false;
                value = (value << 6) | static_cast<uint8_t>(decode[byte]);
            }
        }
        const size_t produced = 3 - padding;
        if (length + produced > capacity)
            return false;
        output[length++] = static_cast<uint8_t>(value >> 16);
        if (padding < 2)
            output[length++] = static_cast<uint8_t>(value >> 8);
        if (padding < 1)
            output[length++] = static_cast<uint8_t>(value);
    }
    return true;
}
} // namespace

BrokerRadioInterface::BrokerRadioInterface(std::string socketPath)
    : OSThread("BrokerRadio", 10), socketPath(std::move(socketPath))
{
}

BrokerRadioInterface::~BrokerRadioInterface()
{
    disconnectBroker(true);
}

bool BrokerRadioInterface::init()
{
    if (!RadioInterface::init())
        return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!connectBroker() && std::chrono::steady_clock::now() < deadline)
        usleep(100000);
    if (socketFd < 0) {
        LOG_ERROR("SX1262 manager unavailable at %s", socketPath.c_str());
        return false;
    }
    return leaseReady ? configureBroker() : true;
}

bool BrokerRadioInterface::reconfigure()
{
    RadioInterface::reconfigure();
    return !leaseReady || configureBroker();
}

bool BrokerRadioInterface::sleep()
{
    Json::Value result;
    return !leaseReady || request("sleep", Json::Value(Json::objectValue), result);
}

bool BrokerRadioInterface::canSleep(bool deepSleep)
{
    return txQueue.empty() && !(deepSleep && txInFlight);
}

ErrorCode BrokerRadioInterface::send(meshtastic_MeshPacket *packet)
{
    if (!leaseReady || disabled || !config.lora.tx_enabled) {
        packetPool.release(packet);
        return ERRNO_DISABLED;
    }
    printPacket("enqueue broker send", packet);
    bool dropped = false;
    if (!txQueue.enqueue(packet, &dropped)) {
        packetPool.release(packet);
        return ERRNO_UNKNOWN;
    }
    scheduleTransmit();
    return ERRNO_OK;
}

meshtastic_QueueStatus BrokerRadioInterface::getQueueStatus()
{
    meshtastic_QueueStatus status{};
    status.free = txQueue.getFree();
    status.maxlen = txQueue.getMaxLen();
    return status;
}

bool BrokerRadioInterface::cancelSending(NodeNum from, PacketId id)
{
    meshtastic_MeshPacket *packet = txQueue.remove(from, id);
    if (!packet)
        return false;
    RadioTxHooks::packetReleased(this, packet);
    packetPool.release(packet);
    return true;
}

bool BrokerRadioInterface::findInTxQueue(NodeNum from, PacketId id)
{
    return txQueue.find(from, id);
}

uint32_t BrokerRadioInterface::getPacketTime(uint32_t length, bool)
{
    const double bandwidthHz = bw * 1000.0;
    const double symbol = std::pow(2.0, sf) / bandwidthHz;
    const int lowDataRate = symbol >= 0.016 ? 1 : 0;
    const double numerator = 8.0 * length - 4.0 * sf + 28.0 + 16.0;
    const double denominator = 4.0 * (sf - 2 * lowDataRate);
    const double payloadSymbols = 8.0 + std::max(0.0, std::ceil(numerator / denominator) * cr);
    return static_cast<uint32_t>(std::ceil((preambleLength + 4.25 + payloadSymbols) * symbol * 1000.0));
}

int32_t BrokerRadioInterface::runOnce()
{
    if (socketFd < 0) {
        connectBroker();
        return socketFd < 0 ? 1000 : 10;
    }
    processSocket();
    if (leaseReady && !txInFlight && !txQueue.empty() && static_cast<int32_t>(millis() - nextTxAt) >= 0)
        tryTransmit();
    return 10;
}

bool BrokerRadioInterface::connectBroker()
{
    disconnectBroker(false);
    socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socketFd < 0)
        return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        disconnectBroker(false);
        return false;
    }
    strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
    if (connect(socketFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        disconnectBroker(false);
        return false;
    }
    timeval timeout{5, 0};
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    Json::Value hello(Json::objectValue);
    hello["type"] = "hello";
    hello["api"]["major"] = 1;
    hello["api"]["minor"] = 1;
    hello["role"] = "meshtastic";
    hello["pid"] = getpid();
    hello["request_id"] = Json::UInt64(++requestId);
    const std::string packet = compactJson(hello);
    if (::send(socketFd, packet.data(), packet.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(packet.size())) {
        disconnectBroker(false);
        return false;
    }
    std::array<char, MAX_BROKER_PACKET + 1> buffer{};
    const ssize_t received = recv(socketFd, buffer.data(), buffer.size(), 0);
    Json::Value response;
    if (received <= 0 || received > static_cast<ssize_t>(MAX_BROKER_PACKET) || !parseJson(buffer.data(), received, response) ||
        !response.get("ok", false).asBool()) {
        disconnectBroker(false);
        return false;
    }
    const Json::Value result = response["result"];
    generation = result.get("generation", Json::UInt64(0)).asUInt64();
    if (result["api"].get("major", -1).asInt() != 1 || generation == 0) {
        disconnectBroker(false);
        return false;
    }
    const int flags = fcntl(socketFd, F_GETFL, 0);
    fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
    leaseReady = false;
    processSocket();
    return true;
}

void BrokerRadioInterface::disconnectBroker(bool dropQueued)
{
    if (socketFd >= 0)
        close(socketFd);
    socketFd = -1;
    leaseReady = false;
    txInFlight = false;
    generation = 0;
    if (dropQueued)
        dropQueue();
}

bool BrokerRadioInterface::request(const char *operation, const Json::Value &arguments, Json::Value &result)
{
    if (socketFd < 0)
        return false;
    Json::Value command = arguments;
    command["type"] = "request";
    command["op"] = operation;
    command["request_id"] = Json::UInt64(++requestId);
    command["generation"] = Json::UInt64(generation);
    const uint64_t expected = requestId;
    const std::string packet = compactJson(command);
    if (packet.size() > MAX_BROKER_PACKET ||
        ::send(socketFd, packet.data(), packet.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(packet.size())) {
        disconnectBroker(true);
        return false;
    }
    pollfd descriptor{socketFd, POLLIN, 0};
    const uint32_t deadline = millis() + 5000;
    while (static_cast<int32_t>(millis() - deadline) < 0) {
        if (poll(&descriptor, 1, 100) <= 0)
            continue;
        std::array<char, MAX_BROKER_PACKET + 1> buffer{};
        const ssize_t received = recv(socketFd, buffer.data(), buffer.size(), 0);
        Json::Value response;
        if (received <= 0 || received > static_cast<ssize_t>(MAX_BROKER_PACKET) ||
            !parseJson(buffer.data(), received, response)) {
            disconnectBroker(true);
            return false;
        }
        if (response.get("type", "").asString() == "event") {
            processEvent(response);
            continue;
        }
        if (response.get("request_id", Json::UInt64(0)).asUInt64() != expected)
            return false;
        const uint64_t responseGeneration = response.get("generation", Json::UInt64(generation)).asUInt64();
        if (responseGeneration != generation) {
            generation = responseGeneration;
            leaseReady = false;
            dropQueue();
        }
        if (!response.get("ok", false).asBool()) {
            LOG_WARN("SX1262 broker rejected %s: %s", operation, response["error"].get("message", "unknown").asCString());
            return false;
        }
        result = response["result"];
        return true;
    }
    return false;
}

bool BrokerRadioInterface::configureBroker()
{
    // Match SX126xInterface::programModemParams(): regional configuration can
    // request more power than the physical SX1262 supports (US defaults to
    // 30 dBm, while the AIO radio is bounded to 22 dBm).  The hardware broker
    // correctly rejects out-of-range PHY requests, so clamp on the protocol
    // side before serializing the configuration.
    limitPower(portduino_config.sx126x_max_power);
    Json::Value arguments(Json::objectValue);
    Json::Value &phy = arguments["phy"];
    phy["frequency"] = Json::UInt64(static_cast<uint64_t>(std::llround(savedFreq * 1000000.0)));
    phy["bandwidth"] = static_cast<uint32_t>(std::lround(bw * 1000.0));
    phy["spreading_factor"] = sf;
    phy["coding_rate"] = cr;
    phy["sync_word"] = 0x2b;
    phy["preamble_length"] = preambleLength;
    phy["header_mode"] = "explicit";
    phy["crc"] = true;
    phy["iq_inversion"] = false;
    phy["tx_power"] = power;
    Json::Value result;
    return request("configure_phy", arguments, result) && request("start_rx", Json::Value(Json::objectValue), result);
}

void BrokerRadioInterface::processSocket()
{
    while (socketFd >= 0) {
        std::array<char, MAX_BROKER_PACKET + 1> buffer{};
        const ssize_t received = recv(socketFd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (received <= 0 || received > static_cast<ssize_t>(MAX_BROKER_PACKET)) {
            disconnectBroker(true);
            return;
        }
        Json::Value event;
        if (!parseJson(buffer.data(), received, event)) {
            disconnectBroker(true);
            return;
        }
        if (event.get("type", "").asString() == "event")
            processEvent(event);
    }
}

void BrokerRadioInterface::processEvent(const Json::Value &event)
{
    const uint64_t eventGeneration = event.get("generation", Json::UInt64(generation)).asUInt64();
    const std::string name = event.get("event", "").asString();
    if (eventGeneration != generation) {
        generation = eventGeneration;
        dropQueue();
    }
    if (name == "lease_granted") {
        leaseReady = true;
        if (configureBroker()) {
            setBluetoothEnable(true);
        } else {
            leaseReady = false;
            LOG_ERROR("SX1262 broker lease could not be configured");
        }
    } else if (name == "prepare_revoke") {
        leaseReady = false;
        dropQueue();
        if (linuxBluetooth)
            linuxBluetooth->deinit();
        Json::Value result;
        request("quiesced", Json::Value(Json::objectValue), result);
    } else if (name == "lease_revoked") {
        leaseReady = false;
    } else if (name == "rx_packet" && leaseReady) {
        processRx(event);
    } else if (name == "cad_result") {
        lastCadBusy = event.get("detected", false).asBool();
    } else if (name == "tx_done") {
        completeTransmit(true);
    } else if (name == "tx_failed") {
        completeTransmit(false);
    } else if (name == "radio_fault" || name == "power_changed") {
        leaseReady = false;
        if (linuxBluetooth)
            linuxBluetooth->deinit();
    }
}

void BrokerRadioInterface::processRx(const Json::Value &event)
{
    size_t length = 0;
    if (!decodeBase64(event.get("payload", "").asString(), reinterpret_cast<uint8_t *>(&radioBuffer), sizeof(radioBuffer),
                      length) ||
        length < sizeof(PacketHeader))
        return;
    const int32_t payloadLength = static_cast<int32_t>(length - sizeof(PacketHeader));
    if (radioBuffer.header.from == 0 || payloadLength < 0)
        return;
    meshtastic_MeshPacket *packet = packetPool.allocZeroed();
    if (!packet)
        return;
    packet->from = radioBuffer.header.from;
    packet->to = radioBuffer.header.to;
    packet->id = radioBuffer.header.id;
    packet->channel = radioBuffer.header.channel;
    packet->hop_limit = radioBuffer.header.flags & PACKET_FLAGS_HOP_LIMIT_MASK;
    packet->hop_start = (radioBuffer.header.flags & PACKET_FLAGS_HOP_START_MASK) >> PACKET_FLAGS_HOP_START_SHIFT;
    packet->want_ack = !!(radioBuffer.header.flags & PACKET_FLAGS_WANT_ACK_MASK);
    packet->via_mqtt = !!(radioBuffer.header.flags & PACKET_FLAGS_VIA_MQTT_MASK);
    packet->next_hop = packet->hop_start == 0 ? NO_NEXT_HOP_PREFERENCE : radioBuffer.header.next_hop;
    packet->relay_node = packet->hop_start == 0 ? NO_RELAY_NODE : radioBuffer.header.relay_node;
    packet->rx_snr = event.get("snr", 0).asFloat();
    packet->rx_rssi = static_cast<int32_t>(std::lround(event.get("rssi", 0).asDouble()));
    packet->has_rx_rssi = true;
    lastRssi = packet->rx_rssi;
    packet->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
    memcpy(packet->encrypted.bytes, radioBuffer.payload, payloadLength);
    packet->encrypted.size = payloadLength;
    airTime->logAirtime(RX_LOG, getPacketTime(length, true));
    deliverToReceiver(packet);
}

void BrokerRadioInterface::scheduleTransmit()
{
    meshtastic_MeshPacket *packet = txQueue.getFront();
    if (!packet)
        return;
    const uint32_t delay = packet->rx_rssi ? getTxDelayMsecWeighted(packet) : getTxDelayMsec();
    nextTxAt = millis() + std::max<uint32_t>(1, delay);
}

void BrokerRadioInterface::tryTransmit()
{
    meshtastic_MeshPacket *packet = txQueue.getFront();
    if (!packet)
        return;
    if (packet->tx_after && static_cast<int32_t>(millis() - packet->tx_after) < 0) {
        nextTxAt = packet->tx_after;
        return;
    }
    Json::Value result;
    lastCadBusy = false;
    if (!request("cad", Json::Value(Json::objectValue), result)) {
        scheduleTransmit();
        return;
    }
    if (lastCadBusy) {
        scheduleTransmit();
        return;
    }
    const RadioTxHook::PreTxAction action = RadioTxHooks::beforeTransmit(this, packet);
    if (action == RadioTxHook::PRETX_DEFER) {
        scheduleTransmit();
        return;
    }
    packet = txQueue.dequeue();
    if (!packet)
        return;
    if (action == RadioTxHook::PRETX_DROP) {
        RadioTxHooks::packetReleased(this, packet);
        packetPool.release(packet);
        scheduleTransmit();
        return;
    }
    const size_t length = beginSending(packet);
    Json::Value arguments(Json::objectValue);
    arguments["payload"] = encodeBase64(reinterpret_cast<uint8_t *>(&radioBuffer), length);
    if (!request("transmit", arguments, result)) {
        completeTransmit(false);
        return;
    }
    txInFlight = true;
    lastTxStart = millis();
}

void BrokerRadioInterface::completeTransmit(bool success)
{
    meshtastic_MeshPacket *packet = sendingPacket;
    sendingPacket = nullptr;
    txInFlight = false;
    if (packet) {
        if (success)
            airTime->logAirtime(TX_LOG, RadioInterface::getPacketTime(packet));
        RadioTxHooks::packetReleased(this, packet);
        packetPool.release(packet);
    }
    Json::Value result;
    if (leaseReady)
        request("start_rx", Json::Value(Json::objectValue), result);
    scheduleTransmit();
}

void BrokerRadioInterface::dropQueue()
{
    while (meshtastic_MeshPacket *packet = txQueue.dequeue()) {
        RadioTxHooks::packetReleased(this, packet);
        packetPool.release(packet);
    }
    if (sendingPacket) {
        RadioTxHooks::packetReleased(this, sendingPacket);
        packetPool.release(sendingPacket);
        sendingPacket = nullptr;
    }
    txInFlight = false;
}

#endif
