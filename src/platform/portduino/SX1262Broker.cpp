#include "SX1262Broker.h"

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "PortduinoGlue.h"
#include "mesh/RadioLibInterface.h"

#include <RadioLib.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace meshtastic::portduino
{
namespace
{

using Clock = std::chrono::steady_clock;
constexpr auto HEARTBEAT_TIMEOUT = std::chrono::seconds(5);
constexpr auto QUIESCE_TIMEOUT = std::chrono::seconds(5);

volatile sig_atomic_t stopRequested = 0;

void handleSignal(int)
{
    stopRequested = 1;
}

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

bool decodeBase64(const std::string &input, std::vector<uint8_t> &output)
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
    output.clear();
    output.reserve((input.size() / 4) * 3);
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
        output.push_back(static_cast<uint8_t>(value >> 16));
        if (padding < 2)
            output.push_back(static_cast<uint8_t>(value >> 8));
        if (padding < 1)
            output.push_back(static_cast<uint8_t>(value));
    }
    return output.size() <= SX1262_BROKER_MAX_PAYLOAD;
}

enum class Mode { OFF, STARTING, MESHTASTIC, MESHCORE, RETICULUM, TRANSITION, FAULT };

const char *modeName(Mode mode)
{
    switch (mode) {
    case Mode::OFF:
        return "OFF";
    case Mode::STARTING:
        return "STARTING";
    case Mode::MESHTASTIC:
        return "MESHTASTIC";
    case Mode::MESHCORE:
        return "MESHCORE";
    case Mode::RETICULUM:
        return "RETICULUM";
    case Mode::TRANSITION:
        return "TRANSITION";
    case Mode::FAULT:
        return "FAULT";
    }
    return "FAULT";
}

Mode parseMode(const std::string &mode)
{
    if (mode == "meshtastic")
        return Mode::MESHTASTIC;
    if (mode == "meshcore")
        return Mode::MESHCORE;
    if (mode == "reticulum")
        return Mode::RETICULUM;
    return Mode::FAULT;
}

const char *roleForMode(Mode mode)
{
    if (mode == Mode::MESHTASTIC)
        return "meshtastic";
    if (mode == Mode::MESHCORE)
        return "meshcore";
    if (mode == Mode::RETICULUM)
        return "reticulum";
    return "";
}

struct PhyConfig {
    float frequencyMHz = 0;
    float bandwidthKHz = 0;
    int spreadingFactor = 0;
    int codingRate = 0;
    uint16_t syncWord = 0;
    int preambleLength = 0;
    bool implicitHeader = false;
    int implicitLength = 0;
    bool crc = true;
    bool invertIq = false;
    int txPower = 0;
};

class RadioBackend
{
  public:
    virtual ~RadioBackend() = default;
    virtual int probe() = 0;
    virtual int reset() = 0;
    virtual int configure(const PhyConfig &) = 0;
    virtual int startReceive() = 0;
    virtual int standby() = 0;
    virtual int sleep() = 0;
    virtual int cad() = 0;
    virtual int startTransmit(const uint8_t *, size_t) = 0;
    /// Return true once an asynchronous transmit completed. ``result`` is the
    /// RadioLib completion status.
    virtual bool pollTransmit(int &result) = 0;
    virtual bool receive(std::vector<uint8_t> &, float &, float &, float &) = 0;
};

class FakeRadioBackend final : public RadioBackend
{
  public:
    int probe() override { return RADIOLIB_ERR_NONE; }
    int reset() override { return RADIOLIB_ERR_NONE; }
    int configure(const PhyConfig &) override { return RADIOLIB_ERR_NONE; }
    int startReceive() override { return RADIOLIB_ERR_NONE; }
    int standby() override { return RADIOLIB_ERR_NONE; }
    int sleep() override { return RADIOLIB_ERR_NONE; }
    int cad() override { return RADIOLIB_CHANNEL_FREE; }
    int startTransmit(const uint8_t *, size_t) override
    {
        transmitting = true;
        return RADIOLIB_ERR_NONE;
    }
    bool pollTransmit(int &result) override
    {
        if (!transmitting)
            return false;
        transmitting = false;
        result = RADIOLIB_ERR_NONE;
        return true;
    }
    bool receive(std::vector<uint8_t> &, float &, float &, float &) override { return false; }

  private:
    bool transmitting = false;
};

class RadioLibSX1262Backend final : public RadioBackend
{
  public:
    RadioLibSX1262Backend()
        : hal(SPI, SPISettings(portduino_config.spiSpeed, MSBFIRST, SPI_MODE0)),
          module(&hal, portduino_config.lora_cs_pin.pin, portduino_config.lora_irq_pin.pin, portduino_config.lora_reset_pin.pin,
                 portduino_config.lora_busy_pin.pin),
          radio(&module)
    {
    }

    int probe() override
    {
        const float tcxo = static_cast<float>(portduino_config.dio3_tcxo_voltage) / 1000.0f;
        int result = radio.begin(915.0, 125.0, 7, 5, 0x12, 10, 8, tcxo, false);
        if (result != RADIOLIB_ERR_NONE)
            return result;
        result = radio.setCurrentLimit(140);
        if (result != RADIOLIB_ERR_NONE)
            return result;
        if (portduino_config.dio2_as_rf_switch) {
            result = radio.setDio2AsRfSwitch(true);
            if (result != RADIOLIB_ERR_NONE)
                return result;
        }
        if (portduino_config.has_rfswitch_table)
            radio.setRfSwitchTable(portduino_config.rfswitch_dio_pins, portduino_config.rfswitch_table);
        return radio.standby();
    }

    int reset() override { return radio.reset(true); }

    int configure(const PhyConfig &config) override
    {
        int result = radio.standby();
        if (result != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setFrequency(config.frequencyMHz)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setBandwidth(config.bandwidthKHz)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setSpreadingFactor(config.spreadingFactor)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setCodingRate(config.codingRate)) != RADIOLIB_ERR_NONE)
            return result;
        const uint8_t word = config.syncWord > 0xff
                                 ? static_cast<uint8_t>(((config.syncWord >> 8) & 0xf0) | ((config.syncWord >> 4) & 0x0f))
                                 : static_cast<uint8_t>(config.syncWord);
        const uint8_t controls =
            config.syncWord > 0xff ? static_cast<uint8_t>(((config.syncWord >> 4) & 0xf0) | (config.syncWord & 0x0f)) : 0x44;
        if ((result = radio.setSyncWord(word, controls)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setPreambleLength(config.preambleLength)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = config.implicitHeader ? radio.implicitHeader(config.implicitLength) : radio.explicitHeader()) !=
            RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.setCRC(config.crc ? 2 : 0)) != RADIOLIB_ERR_NONE)
            return result;
        if ((result = radio.invertIQ(config.invertIq)) != RADIOLIB_ERR_NONE)
            return result;
        return radio.setOutputPower(config.txPower);
    }

    int startReceive() override { return radio.startReceive(); }
    int standby() override { return radio.standby(); }
    int sleep() override { return radio.sleep(false); }
    int cad() override { return radio.scanChannel(); }
    int startTransmit(const uint8_t *data, size_t length) override { return radio.startTransmit(data, length); }
    bool pollTransmit(int &result) override
    {
        const RadioLibIrqFlags_t flags = radio.getIrqFlags();
        if (!(flags & RADIOLIB_SX126X_IRQ_TX_DONE))
            return false;
        result = radio.finishTransmit();
        return true;
    }

    bool receive(std::vector<uint8_t> &payload, float &rssi, float &snr, float &frequencyError) override
    {
        const RadioLibIrqFlags_t flags = radio.getIrqFlags();
        if (!(flags & RADIOLIB_SX126X_IRQ_RX_DONE))
            return false;
        const size_t length = radio.getPacketLength();
        if (length == 0 || length > SX1262_BROKER_MAX_PAYLOAD) {
            radio.finishReceive();
            radio.startReceive();
            return false;
        }
        payload.resize(length);
        if (radio.readData(payload.data(), length) != RADIOLIB_ERR_NONE) {
            radio.startReceive();
            return false;
        }
        rssi = radio.getRSSI(true);
        snr = radio.getSNR();
        frequencyError = radio.getFrequencyError();
        radio.startReceive();
        return true;
    }

  private:
    LockingArduinoHal hal;
    Module module;
    SX1262 radio;
};

struct Client {
    int fd = -1;
    uid_t uid = 0;
    gid_t gid = 0;
    pid_t pid = 0;
    uint64_t connectionId = 0;
    std::string role;
    bool hello = false;
};

class Broker
{
  public:
    Broker(std::unique_ptr<RadioBackend> radio, std::string socketPath, std::string forcedOffPath, bool enforceRoles)
        : radio(std::move(radio)), socketPath(std::move(socketPath)), forcedOffPath(std::move(forcedOffPath)),
          enforceRoles(enforceRoles)
    {
    }

    ~Broker()
    {
        for (const auto &client : clients)
            close(client.fd);
        if (listenFd >= 0)
            close(listenFd);
        unlink(socketPath.c_str());
    }

    int run()
    {
        if (!openSocket())
            return EXIT_FAILURE;
        forcedOff = access(forcedOffPath.c_str(), F_OK) == 0;
        state = forcedOff ? Mode::OFF : Mode::STARTING;
        if (!forcedOff) {
            setRail(true);
            const int probe = radio->probe();
            if (probe != RADIOLIB_ERR_NONE) {
                enterFault("probe", probe);
            } else {
                state = Mode::MESHTASTIC;
                ++generation;
            }
        } else {
            setRail(false);
        }
        while (!stopRequested) {
            processTimeouts();
            pollRadio();
            std::vector<pollfd> pollfds;
            pollfds.reserve(clients.size() + 1);
            pollfds.push_back({listenFd, POLLIN, 0});
            for (const auto &client : clients)
                pollfds.push_back({client.fd, POLLIN | POLLHUP | POLLERR, 0});
            const int ready = poll(pollfds.data(), pollfds.size(), 20);
            if (ready < 0 && errno != EINTR) {
                perror("watchdogs-sx1262d poll");
                return EXIT_FAILURE;
            }
            if (ready <= 0)
                continue;
            if (pollfds[0].revents & POLLIN)
                acceptClient();
            for (size_t index = pollfds.size(); index-- > 1;) {
                if (pollfds[index].revents & (POLLIN | POLLHUP | POLLERR))
                    serviceClient(index - 1, pollfds[index].revents);
            }
        }
        return EXIT_SUCCESS;
    }

  private:
    bool openSocket()
    {
        listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (listenFd < 0) {
            perror("watchdogs-sx1262d socket");
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socketPath.size() >= sizeof(address.sun_path)) {
            fprintf(stderr, "watchdogs-sx1262d socket path is too long\n");
            return false;
        }
        strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);
        unlink(socketPath.c_str());
        if (bind(listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
            perror("watchdogs-sx1262d bind");
            return false;
        }
        if (chmod(socketPath.c_str(), 0660) < 0) {
            perror("watchdogs-sx1262d chmod");
            return false;
        }
        if (listen(listenFd, 8) < 0) {
            perror("watchdogs-sx1262d listen");
            return false;
        }
        return true;
    }

    void acceptClient()
    {
        const int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            return;
        ucred credentials{};
        socklen_t length = sizeof(credentials);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0) {
            close(fd);
            return;
        }
        clients.push_back({fd, credentials.uid, credentials.gid, credentials.pid, nextConnectionId++});
    }

    void removeClient(size_t index)
    {
        const std::string role = clients[index].role;
        close(clients[index].fd);
        clients.erase(clients.begin() + index);
        if (role == "controller") {
            controllerConnection = 0;
            if ((state == Mode::MESHCORE || state == Mode::RETICULUM) && !forcedOff)
                requestTransition(Mode::MESHTASTIC);
        }
        if (transitionWaitingConnection && !clientByConnection(transitionWaitingConnection))
            finishTransition();
    }

    void serviceClient(size_t index, short revents)
    {
        if (revents & (POLLHUP | POLLERR)) {
            removeClient(index);
            return;
        }
        std::array<char, SX1262_BROKER_MAX_PACKET + 1> buffer{};
        const ssize_t received = recv(clients[index].fd, buffer.data(), buffer.size(), 0);
        if (received <= 0 || received > SX1262_BROKER_MAX_PACKET) {
            removeClient(index);
            return;
        }
        Json::Value request;
        if (!parseJson(buffer.data(), static_cast<size_t>(received), request)) {
            sendError(clients[index], Json::Value(), "invalid_json", "Packet must be one JSON object");
            return;
        }
        if (!clients[index].hello) {
            handleHello(clients[index], request);
            return;
        }
        handleRequest(clients[index], request);
    }

    void handleHello(Client &client, const Json::Value &request)
    {
        const Json::Value requestId = request.get("request_id", Json::Value());
        const Json::Value api = request.get("api", Json::Value());
        const std::string role = request.get("role", "").asString();
        if (request.get("type", "").asString() != "hello" || !api.isObject() || api.get("major", -1).asInt() != 1) {
            sendError(client, requestId, "api_mismatch", "Broker API major 1 is required");
            return;
        }
        if (role != "controller" && role != "meshtastic" && role != "meshcore" && role != "reticulum") {
            sendError(client, requestId, "invalid_role", "Unknown broker client role");
            return;
        }
        const passwd *meshtasticAccount = getpwnam("meshtasticd");
        const passwd *managerAccount = getpwnam("watchdogs-sx1262d");
        const bool isMeshtastic = meshtasticAccount && client.uid == meshtasticAccount->pw_uid;
        const bool isManager = managerAccount && client.uid == managerAccount->pw_uid;
        if (enforceRoles && ((role == "meshtastic" && !isMeshtastic) || (role != "meshtastic" && (isMeshtastic || isManager)))) {
            sendError(client, requestId, "unauthorized_role", "Peer credentials do not permit this broker role");
            return;
        }
        if (role == "controller" && controllerConnection != 0) {
            sendError(client, requestId, "role_busy", "A WDG controller is already connected");
            return;
        }
        client.role = role;
        client.hello = true;
        if (role == "controller") {
            controllerConnection = client.connectionId;
            lastHeartbeat = Clock::now();
        }
        Json::Value result(Json::objectValue);
        result["api"]["major"] = SX1262_BROKER_API_MAJOR;
        result["api"]["minor"] = SX1262_BROKER_API_MINOR;
        result["connection_id"] = Json::UInt64(client.connectionId);
        result["generation"] = Json::UInt64(generation);
        sendSuccess(client, requestId, result);
        if (!forcedOff && role == roleForMode(state))
            sendEvent(client, "lease_granted");
    }

    void handleRequest(Client &client, const Json::Value &request)
    {
        const Json::Value requestId = request.get("request_id", Json::Value());
        if (request.get("type", "").asString() != "request" || requestId.isNull()) {
            sendError(client, requestId, "invalid_envelope", "Request envelope is incomplete");
            return;
        }
        const uint64_t requestGeneration = request.get("generation", Json::UInt64(0)).asUInt64();
        if (requestGeneration != generation) {
            sendError(client, requestId, "stale_generation", "Radio ownership generation has changed");
            return;
        }
        const std::string operation = request.get("op", "").asString();
        if (operation == "get_status") {
            sendSuccess(client, requestId, status());
            return;
        }
        if (operation == "heartbeat") {
            if (!requireController(client, requestId))
                return;
            lastHeartbeat = Clock::now();
            sendSuccess(client, requestId, Json::Value(Json::objectValue));
            return;
        }
        if (operation == "activate_mode") {
            if (!requireController(client, requestId))
                return;
            const Mode requested = parseMode(request.get("mode", "").asString());
            if (requested == Mode::FAULT) {
                sendError(client, requestId, "invalid_mode", "Expected meshtastic, meshcore, or reticulum");
                return;
            }
            if (forcedOff || state == Mode::OFF) {
                sendError(client, requestId, "forced_off", "Administrative force-off is active");
                return;
            }
            requestTransition(requested);
            Json::Value result(Json::objectValue);
            result["pending"] = state == Mode::TRANSITION;
            result["target_mode"] = modeName(requested);
            sendSuccess(client, requestId, result);
            return;
        }
        if (operation == "release_mode") {
            if (!requireController(client, requestId))
                return;
            requestTransition(Mode::MESHTASTIC);
            sendSuccess(client, requestId, Json::Value(Json::objectValue));
            return;
        }
        if (operation == "admin_power_off") {
            if (!requireController(client, requestId))
                return;
            forcePowerOff();
            sendSuccess(client, requestId, status());
            return;
        }
        if (operation == "admin_power_on") {
            if (!requireController(client, requestId))
                return;
            const Mode preferred = request.isMember("mode") ? parseMode(request["mode"].asString()) : Mode::MESHTASTIC;
            if (preferred == Mode::FAULT) {
                sendError(client, requestId, "invalid_mode", "Invalid preferred mode");
                return;
            }
            if (!forcePowerOn(preferred)) {
                sendError(client, requestId, "probe_failed", faultDetail);
                return;
            }
            sendSuccess(client, requestId, status());
            return;
        }
        if (operation == "clear_fault") {
            if (!requireController(client, requestId))
                return;
            if (!forcePowerOn(Mode::MESHTASTIC)) {
                sendError(client, requestId, "probe_failed", faultDetail);
                return;
            }
            sendSuccess(client, requestId, status());
            return;
        }
        if (operation == "quiesced") {
            if (state != Mode::TRANSITION || client.connectionId != transitionWaitingConnection) {
                sendError(client, requestId, "not_revoking", "Client is not being revoked");
                return;
            }
            sendSuccess(client, requestId, Json::Value(Json::objectValue));
            finishTransition();
            return;
        }
        if (!isActiveProtocol(client)) {
            sendError(client, requestId, "not_owner", "Client does not hold the active radio lease");
            return;
        }
        handleRadioOperation(client, requestId, operation, request);
    }

    void handleRadioOperation(Client &client, const Json::Value &requestId, const std::string &operation,
                              const Json::Value &request)
    {
        int result = RADIOLIB_ERR_NONE;
        if (operation == "configure_phy") {
            PhyConfig config;
            std::string error;
            if (!parsePhy(request["phy"], config, error)) {
                sendError(client, requestId, "invalid_phy", error);
                return;
            }
            result = radio->configure(config);
        } else if (operation == "start_rx") {
            result = radio->startReceive();
            receiving = result == RADIOLIB_ERR_NONE;
        } else if (operation == "standby") {
            receiving = false;
            result = radio->standby();
        } else if (operation == "sleep") {
            receiving = false;
            result = radio->sleep();
        } else if (operation == "cad") {
            result = radio->cad();
            Json::Value event(Json::objectValue);
            event["detected"] = result != RADIOLIB_CHANNEL_FREE;
            sendEvent(client, "cad_result", event);
            result = RADIOLIB_ERR_NONE;
        } else if (operation == "transmit") {
            if (txOwnerConnection != 0) {
                sendError(client, requestId, "tx_busy", "A radio transmission is already in flight");
                return;
            }
            std::vector<uint8_t> payload;
            if (!decodeBase64(request.get("payload", "").asString(), payload)) {
                sendError(client, requestId, "invalid_payload", "Payload must be 1 through 255 base64-encoded bytes");
                return;
            }
            receiving = false;
            result = radio->startTransmit(payload.data(), payload.size());
            if (result == RADIOLIB_ERR_NONE) {
                txOwnerConnection = client.connectionId;
                sendEvent(client, "tx_started");
            }
        } else if (operation == "get_metrics") {
            sendSuccess(client, requestId, metrics());
            return;
        } else {
            sendError(client, requestId, "unknown_operation", "Unknown protocol operation");
            return;
        }
        if (result != RADIOLIB_ERR_NONE) {
            sendError(client, requestId, "radio_error", std::to_string(result));
            return;
        }
        sendSuccess(client, requestId, Json::Value(Json::objectValue));
    }

    bool parsePhy(const Json::Value &value, PhyConfig &config, std::string &error)
    {
        if (!value.isObject()) {
            error = "PHY must be an object";
            return false;
        }
        config.frequencyMHz = static_cast<float>(value.get("frequency", 0).asDouble() / 1000000.0);
        config.bandwidthKHz = static_cast<float>(value.get("bandwidth", 0).asDouble() / 1000.0);
        config.spreadingFactor = value.get("spreading_factor", 0).asInt();
        config.codingRate = value.get("coding_rate", 0).asInt();
        config.syncWord = static_cast<uint16_t>(value.get("sync_word", 0).asUInt());
        config.preambleLength = value.get("preamble_length", 0).asInt();
        config.implicitHeader = value.get("header_mode", "explicit").asString() == "implicit";
        config.implicitLength = value.get("implicit_length", 0).asInt();
        config.crc = value.get("crc", true).asBool();
        config.invertIq = value.get("iq_inversion", false).asBool();
        config.txPower = value.get("tx_power", 0).asInt();
        static const float bandwidths[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
        const bool bandwidthValid = std::any_of(std::begin(bandwidths), std::end(bandwidths), [&](float candidate) {
            return std::abs(candidate - config.bandwidthKHz) < 0.2f;
        });
        if (config.frequencyMHz < 150.0f || config.frequencyMHz > 960.0f || !bandwidthValid || config.spreadingFactor < 5 ||
            config.spreadingFactor > 12 || config.codingRate < 5 || config.codingRate > 8 || config.preambleLength < 4 ||
            config.preambleLength > 65535 || config.txPower < -9 || config.txPower > portduino_config.sx126x_max_power ||
            (config.implicitHeader && (config.implicitLength < 1 || config.implicitLength > 255))) {
            error = "PHY values are outside SX1262/board limits";
            return false;
        }
        return true;
    }

    void requestTransition(Mode target)
    {
        if (state == target)
            return;
        if (state == Mode::TRANSITION) {
            transitionTarget = target;
            return;
        }
        transitionTarget = target;
        const char *oldRole = roleForMode(state);
        Client *oldClient = clientByRole(oldRole);
        state = Mode::TRANSITION;
        receiving = false;
        transitionDeadline = Clock::now() + QUIESCE_TIMEOUT;
        transitionWaitingConnection = oldClient ? oldClient->connectionId : 0;
        if (oldClient)
            sendEvent(*oldClient, "prepare_revoke");
        else
            finishTransition();
    }

    void finishTransition()
    {
        if (txOwnerConnection != 0) {
            transitionRadioDeadline = Clock::now() + std::chrono::seconds(15);
            return;
        }
        radio->standby();
        radio->reset();
        ++generation;
        state = transitionTarget;
        transitionWaitingConnection = 0;
        receiving = false;
        Client *newClient = clientByRole(roleForMode(state));
        broadcastEvent("lease_revoked");
        if (newClient)
            sendEvent(*newClient, "lease_granted");
    }

    void activateImmediately(Mode target)
    {
        transitionTarget = target;
        state = Mode::TRANSITION;
        transitionWaitingConnection = 0;
        finishTransition();
    }

    void processTimeouts()
    {
        const auto now = Clock::now();
        if (state == Mode::TRANSITION && transitionWaitingConnection && now >= transitionDeadline) {
            // Fail closed: keep TRANSITION, revoke the old generation, and expose
            // neither frontend until the process exits or proves quiescence.
            ++generation;
            transitionWaitingConnection = 0;
            faultDetail = "protocol failed to quiesce within five seconds";
            broadcastEvent("radio_fault");
        }
        if (state == Mode::TRANSITION && transitionWaitingConnection == 0 && txOwnerConnection == 0 &&
            transitionRadioDeadline != Clock::time_point{}) {
            transitionRadioDeadline = Clock::time_point{};
            finishTransition();
        }
        if (state == Mode::TRANSITION && transitionRadioDeadline != Clock::time_point{} && now >= transitionRadioDeadline) {
            txOwnerConnection = 0;
            transitionRadioDeadline = Clock::time_point{};
            finishTransition();
        }
        if ((state == Mode::MESHCORE || state == Mode::RETICULUM) && controllerConnection &&
            now - lastHeartbeat >= HEARTBEAT_TIMEOUT && !forcedOff) {
            // A living-but-stalled WDG process may still have a GATT
            // application registered. Give its protocol client the same
            // fail-closed quiescence barrier as an operator-requested switch.
            // If WDG actually exited, removeClient() observes the closed peer
            // and completes the transition immediately.
            requestTransition(Mode::MESHTASTIC);
        }
    }

    void pollRadio()
    {
        if (txOwnerConnection != 0) {
            int result = RADIOLIB_ERR_NONE;
            if (radio->pollTransmit(result)) {
                if (Client *owner = clientByConnection(txOwnerConnection))
                    sendEvent(*owner, result == RADIOLIB_ERR_NONE ? "tx_done" : "tx_failed");
                if (result == RADIOLIB_ERR_NONE)
                    ++txPackets;
                txOwnerConnection = 0;
                if (state != Mode::TRANSITION && !forcedOff)
                    receiving = radio->startReceive() == RADIOLIB_ERR_NONE;
            }
        }
        if (!receiving || state == Mode::TRANSITION || forcedOff)
            return;
        Client *owner = clientByRole(roleForMode(state));
        if (!owner)
            return;
        std::vector<uint8_t> payload;
        float rssi = 0, snr = 0, frequencyError = 0;
        if (!radio->receive(payload, rssi, snr, frequencyError))
            return;
        Json::Value body(Json::objectValue);
        body["payload"] = encodeBase64(payload.data(), payload.size());
        body["rssi"] = rssi;
        body["snr"] = snr;
        body["frequency_error"] = frequencyError;
        body["monotonic_ns"] =
            Json::UInt64(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
        if (!sendEvent(*owner, "rx_packet", body, true))
            ++rxDrops;
        else
            ++rxPackets;
    }

    bool forcePowerOn(Mode preferred)
    {
        setRail(true);
        usleep(100000);
        const int result = radio->probe();
        if (result != RADIOLIB_ERR_NONE) {
            enterFault("probe", result);
            return false;
        }
        forcedOff = false;
        unlink(forcedOffPath.c_str());
        ++generation;
        state = preferred;
        faultDetail.clear();
        broadcastEvent("power_changed");
        if (Client *client = clientByRole(roleForMode(state)))
            sendEvent(*client, "lease_granted");
        return true;
    }

    void forcePowerOff()
    {
        state = Mode::TRANSITION;
        broadcastEvent("prepare_revoke");
        receiving = false;
        txOwnerConnection = 0;
        radio->standby();
        radio->sleep();
        setRail(false);
        persistForcedOff();
        forcedOff = true;
        state = Mode::OFF;
        ++generation;
        broadcastEvent("power_changed");
    }

    void setRail(bool enabled)
    {
        for (const auto &pin : portduino_config.extra_pins) {
            if (pin.config_section == "Lora" && pin.config_name == "Enable_Pins" && pin.enabled) {
                pinMode(pin.pin, OUTPUT);
                digitalWrite(pin.pin, enabled ? HIGH : LOW);
            }
        }
        railPowered = enabled;
    }

    void persistForcedOff()
    {
        const std::string temporary = forcedOffPath + ".tmp." + std::to_string(getpid());
        {
            std::ofstream output(temporary, std::ios::out | std::ios::trunc);
            output << "forced-off\n";
            output.flush();
        }
        chmod(temporary.c_str(), 0640);
        if (rename(temporary.c_str(), forcedOffPath.c_str()) < 0) {
            unlink(temporary.c_str());
            enterFault("persist_forced_off", errno);
        }
    }

    void enterFault(const char *stage, int error)
    {
        ++faults;
        receiving = false;
        radio->sleep();
        setRail(false);
        state = Mode::FAULT;
        faultDetail = std::string(stage) + " failed: " + std::to_string(error);
        broadcastEvent("radio_fault");
    }

    Json::Value status() const
    {
        Json::Value result(Json::objectValue);
        result["state"] = modeName(state);
        result["active_mode"] = std::string(roleForMode(state));
        result["generation"] = Json::UInt64(generation);
        result["power"] = railPowered;
        result["forced_off"] = forcedOff;
        result["fault"] = faultDetail;
        result["broker_version"] = "1.0";
        result["lease_age_ms"] =
            Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - lastHeartbeat).count());
        result["metrics"] = metrics();
        return result;
    }

    Json::Value metrics() const
    {
        Json::Value result(Json::objectValue);
        result["rx_packets"] = Json::UInt64(rxPackets);
        result["rx_drops"] = Json::UInt64(rxDrops);
        result["tx_packets"] = Json::UInt64(txPackets);
        result["faults"] = Json::UInt64(faults);
        return result;
    }

    bool requireController(Client &client, const Json::Value &requestId)
    {
        if (client.role == "controller" && client.connectionId == controllerConnection)
            return true;
        sendError(client, requestId, "unauthorized", "WDG controller role is required");
        return false;
    }

    bool isActiveProtocol(const Client &client) const { return !forcedOff && client.role == roleForMode(state); }

    Client *clientByRole(const std::string &role)
    {
        if (role.empty())
            return nullptr;
        for (auto &client : clients)
            if (client.hello && client.role == role)
                return &client;
        return nullptr;
    }

    Client *clientByConnection(uint64_t connectionId)
    {
        for (auto &client : clients)
            if (client.connectionId == connectionId)
                return &client;
        return nullptr;
    }

    bool sendPacket(const Client &client, const Json::Value &value, bool passive = false)
    {
        const std::string packet = compactJson(value);
        if (packet.size() > SX1262_BROKER_MAX_PACKET)
            return false;
        const ssize_t sent = send(client.fd, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent == static_cast<ssize_t>(packet.size()))
            return true;
        if (!passive)
            fprintf(stderr, "watchdogs-sx1262d dropped control packet for pid %d: %s\n", client.pid, strerror(errno));
        return false;
    }

    void sendSuccess(const Client &client, const Json::Value &requestId, const Json::Value &result)
    {
        Json::Value reply(Json::objectValue);
        reply["type"] = "response";
        reply["request_id"] = requestId;
        reply["ok"] = true;
        reply["generation"] = Json::UInt64(generation);
        reply["result"] = result;
        sendPacket(client, reply);
    }

    void sendError(const Client &client, const Json::Value &requestId, const char *code, const std::string &message)
    {
        Json::Value reply(Json::objectValue);
        reply["type"] = "response";
        reply["request_id"] = requestId;
        reply["ok"] = false;
        reply["generation"] = Json::UInt64(generation);
        reply["error"]["code"] = code;
        reply["error"]["message"] = message;
        sendPacket(client, reply);
    }

    bool sendEvent(const Client &client, const char *name, Json::Value body = Json::Value(Json::objectValue),
                   bool passive = false)
    {
        body["type"] = "event";
        body["event"] = name;
        body["generation"] = Json::UInt64(generation);
        return sendPacket(client, body, passive);
    }

    void broadcastEvent(const char *name)
    {
        for (const auto &client : clients)
            if (client.hello)
                sendEvent(client, name);
    }

    std::unique_ptr<RadioBackend> radio;
    std::string socketPath;
    std::string forcedOffPath;
    int listenFd = -1;
    std::vector<Client> clients;
    uint64_t nextConnectionId = 1;
    uint64_t generation = 1;
    uint64_t controllerConnection = 0;
    Mode state = Mode::STARTING;
    Mode transitionTarget = Mode::MESHTASTIC;
    uint64_t transitionWaitingConnection = 0;
    uint64_t txOwnerConnection = 0;
    Clock::time_point transitionDeadline{};
    Clock::time_point transitionRadioDeadline{};
    Clock::time_point lastHeartbeat = Clock::now();
    bool forcedOff = false;
    bool railPowered = false;
    bool receiving = false;
    std::string faultDetail;
    uint64_t rxPackets = 0;
    uint64_t rxDrops = 0;
    uint64_t txPackets = 0;
    uint64_t faults = 0;
    bool enforceRoles = true;
};

} // namespace

int runSX1262Broker(const char *socketPath, const char *forcedOffPath, bool fakeRadio)
{
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);
    std::unique_ptr<RadioBackend> radio;
    if (fakeRadio)
        radio = std::make_unique<FakeRadioBackend>();
    else
        radio = std::make_unique<RadioLibSX1262Backend>();
    Broker broker(std::move(radio), socketPath, forcedOffPath, !fakeRadio);
    return broker.run();
}

} // namespace meshtastic::portduino

#endif
