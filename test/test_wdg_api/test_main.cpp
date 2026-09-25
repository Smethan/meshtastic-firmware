#include "MeshService.h"
#include "MeshTypes.h"
#include "NodeDB.h"
#include "TestUtil.h"
#include "UptimeClock.h"
#include "configuration.h"
#include <unity.h>

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "platform/portduino/FullPhoneApiLease.h"
#include "platform/portduino/PortduinoGlue.h"
#include "platform/portduino/WdgApi.h"
#include "platform/portduino/WdgPolicy.h"
#include <acl/libacl.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <json/json.h>
#include <memory>
#include <string>
#include <sys/acl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using meshtastic::portduino::WdgApi;
using meshtastic::portduino::WdgPeerCredentials;
using meshtastic::portduino::WdgPolicy;

class TestWdgApi : public WdgApi
{
  public:
    using WdgApi::applyPathAccessControl;
    using WdgApi::runOnce;
    using WdgApi::sanitizeTextPayload;
    using WdgApi::WdgApi;
};

static std::string testDirectory;
static std::string socketPath;
static TestWdgApi *testApi = nullptr;
static std::vector<int> clientFds;
static MeshService *savedService = nullptr;
static NodeDB *savedNodeDb = nullptr;
static NodeDB *scratchNodeDb = nullptr;
static meshtastic_NodeDatabase savedNodeDatabase{};
static meshtastic_MyNodeInfo savedMyNodeInfo = meshtastic_MyNodeInfo_init_zero;
static meshtastic_User savedOwner = meshtastic_User_init_zero;
static meshtastic_LocalConfig savedConfig = meshtastic_LocalConfig_init_zero;
static meshtastic_ChannelFile savedChannelFile = meshtastic_ChannelFile_init_zero;
static bool savedPortduinoBluetoothEnabled = false;
static std::string savedBluetoothAdapter;

static std::vector<uid_t> namedAclUsers(const std::string &path)
{
    std::vector<uid_t> users;
    acl_t acl = acl_get_file(path.c_str(), ACL_TYPE_ACCESS);
    TEST_ASSERT_NOT_NULL(acl);
    acl_entry_t entry;
    int entryId = ACL_FIRST_ENTRY;
    while (acl_get_entry(acl, entryId, &entry) == 1) {
        entryId = ACL_NEXT_ENTRY;
        acl_tag_t tag = ACL_UNDEFINED_TAG;
        TEST_ASSERT_EQUAL_INT(0, acl_get_tag_type(entry, &tag));
        if (tag != ACL_USER)
            continue;
        auto *uid = static_cast<uid_t *>(acl_get_qualifier(entry));
        TEST_ASSERT_NOT_NULL(uid);
        users.push_back(*uid);
        acl_free(uid);
    }
    acl_free(acl);
    return users;
}

static Json::Value receiveJson(int client)
{
    std::array<char, WdgApi::MAX_PACKET_BYTES> response{};
    ssize_t length = recv(client, response.data(), response.size(), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, length);

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    TEST_ASSERT_TRUE_MESSAGE(reader->parse(response.data(), response.data() + length, &value, &errors), errors.c_str());
    return value;
}

static int connectClient(const std::string &path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    clientFds.push_back(fd);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    TEST_ASSERT_LESS_THAN_size_t(sizeof(address.sun_path), path.size());
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    TEST_ASSERT_EQUAL_INT(0, connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    return fd;
}

static Json::Value exchange(TestWdgApi &api, int client, const std::string &request)
{
    TEST_ASSERT_EQUAL_INT(request.size(), send(client, request.data(), request.size(), MSG_NOSIGNAL));
    api.runOnce();
    for (size_t i = 0; i < 16; ++i) {
        Json::Value value = receiveJson(client);
        if (value.get("type", "").asString() == "reply")
            return value;
    }
    TEST_FAIL_MESSAGE("WDG API did not return a correlated reply");
    return Json::Value{};
}

static bool arrayContains(const Json::Value &array, const char *value)
{
    for (const Json::Value &entry : array)
        if (entry.isString() && entry.asString() == value)
            return true;
    return false;
}

void setUp()
{
    meshtastic::portduino::resetWdgPolicyForTest();
    testApi = nullptr;
    clientFds.clear();
    char pathTemplate[] = "/tmp/meshtastic-wdg-api-XXXXXX";
    char *created = mkdtemp(pathTemplate);
    TEST_ASSERT_NOT_NULL(created);
    testDirectory = created;
    socketPath = testDirectory + "/wdg.sock";
    savedService = service;
    static MeshService packetService;
    service = &packetService;
    savedNodeDb = nodeDB;
    savedNodeDatabase = nodeDatabase;
    savedMyNodeInfo = myNodeInfo;
    savedOwner = owner;
    savedConfig = config;
    savedChannelFile = channelFile;
    savedPortduinoBluetoothEnabled = portduino_config.bluetooth_enabled;
    savedBluetoothAdapter = portduino_config.bluetooth_adapter;
    scratchNodeDb = nullptr;
}

void tearDown()
{
    delete testApi;
    testApi = nullptr;
    delete scratchNodeDb;
    scratchNodeDb = nullptr;
    service = savedService;
    nodeDB = savedNodeDb;
    nodeDatabase = savedNodeDatabase;
    myNodeInfo = savedMyNodeInfo;
    owner = savedOwner;
    config = savedConfig;
    channelFile = savedChannelFile;
    portduino_config.bluetooth_enabled = savedPortduinoBluetoothEnabled;
    portduino_config.bluetooth_adapter = savedBluetoothAdapter;
    for (int fd : clientFds)
        close(fd);
    clientFds.clear();
    unlink(socketPath.c_str());
    rmdir(testDirectory.c_str());
    unsetenv("MESHTASTIC_WDG_SOCKET");
    unsetenv("MESHTASTIC_WDG_ALLOWED_UID");
    unsetenv("MESHTASTIC_WDG_BLUETOOTH_SYSFS");
    unsetenv("MESHTASTIC_WDG_DISABLE_BLUETOOTH");
    unsetenv("MESHTASTIC_WDG_POLICY");
    meshtastic::portduino::resetWdgPolicyForTest();
    Time::useRealClock();
}

static void createNodeDbFixture()
{
    nodeDatabase.version = 0;
    nodeDatabase.nodes.clear();
    myNodeInfo = meshtastic_MyNodeInfo_init_zero;
    myNodeInfo.my_node_num = 0x00000001;
    nodeDB = scratchNodeDb = new NodeDB();
}

void test_socket_path_uses_environment_override()
{
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_SOCKET", socketPath.c_str(), 1));
    TEST_ASSERT_EQUAL_STRING(socketPath.c_str(), WdgApi::resolveSocketPath().c_str());
}

void test_policy_parser_enforces_security_contract()
{
    constexpr const char *valid = R"(
phone_ble:
  enabled: true
  adapter_address: AA:BB:CC:DD:EE:FF
  pairing_window_seconds: 90
  max_bonds: 1
wdg_api:
  enabled: true
  socket_path: /tmp/wdg.sock
  allowed_uid: 1000
full_client_policy:
  ble_priority: true
)";
    WdgPolicy policy;
    std::string error;
    TEST_ASSERT_TRUE_MESSAGE(meshtastic::portduino::parseWdgPolicyText(valid, policy, error), error.c_str());
    TEST_ASSERT_TRUE(policy.valid);
    TEST_ASSERT_TRUE(policy.apiEnabled);
    TEST_ASSERT_TRUE(policy.phoneBleEnabled);
    TEST_ASSERT_TRUE(policy.hasAllowedUid);
    TEST_ASSERT_EQUAL_UINT32(1000, policy.allowedUid);
    TEST_ASSERT_EQUAL_UINT32(90, policy.pairingWindowSeconds);

    std::string invalid = valid;
    invalid.replace(invalid.find("max_bonds: 1"), strlen("max_bonds: 1"), "max_bonds: 2");
    TEST_ASSERT_FALSE(meshtastic::portduino::parseWdgPolicyText(invalid, policy, error));
    TEST_ASSERT_TRUE(error.find("max_bonds") != std::string::npos);

    invalid = valid;
    invalid.replace(invalid.find("ble_priority: true"), strlen("ble_priority: true"), "ble_priority: false");
    TEST_ASSERT_FALSE(meshtastic::portduino::parseWdgPolicyText(invalid, policy, error));
    TEST_ASSERT_TRUE(error.find("ble_priority") != std::string::npos);

    invalid = valid;
    invalid.replace(invalid.find("pairing_window_seconds: 90"), strlen("pairing_window_seconds: 90"),
                    "pairing_window_seconds: 121");
    TEST_ASSERT_FALSE(meshtastic::portduino::parseWdgPolicyText(invalid, policy, error));
    TEST_ASSERT_TRUE(error.find("pairing_window_seconds") != std::string::npos);
}

void test_policy_file_rejects_writable_or_symlinked_input()
{
    const std::string policyPath = testDirectory + "/policy.yaml";
    const std::string symlinkPath = testDirectory + "/policy-link.yaml";
    FILE *policyFile = fopen(policyPath.c_str(), "w");
    TEST_ASSERT_NOT_NULL(policyFile);
    TEST_ASSERT_GREATER_THAN_INT(0, fputs("wdg_api: {}\n", policyFile));
    TEST_ASSERT_EQUAL_INT(0, fclose(policyFile));
    TEST_ASSERT_EQUAL_INT(0, chmod(policyPath.c_str(), 0644));

    std::string contents;
    std::string error;
    TEST_ASSERT_TRUE(meshtastic::portduino::readTrustedWdgPolicyFile(policyPath, geteuid(), contents, error));
    TEST_ASSERT_EQUAL_STRING("wdg_api: {}\n", contents.c_str());

    TEST_ASSERT_EQUAL_INT(0, chmod(policyPath.c_str(), 0664));
    TEST_ASSERT_FALSE(meshtastic::portduino::readTrustedWdgPolicyFile(policyPath, geteuid(), contents, error));
    TEST_ASSERT_EQUAL_INT(0, symlink(policyPath.c_str(), symlinkPath.c_str()));
    TEST_ASSERT_FALSE(meshtastic::portduino::readTrustedWdgPolicyFile(symlinkPath, geteuid(), contents, error));

    unlink(symlinkPath.c_str());
    unlink(policyPath.c_str());
}

void test_acl_replaces_stale_named_uid_and_preserves_modes()
{
    const std::string accessPath = testDirectory + "/access.sock";
    int fd = open(accessPath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0660);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    close(fd);
    TEST_ASSERT_EQUAL_INT(0, chmod(testDirectory.c_str(), 0770));
    TEST_ASSERT_EQUAL_INT(0, chmod(accessPath.c_str(), 0660));

    constexpr uid_t OLD_UID = 424241;
    constexpr uid_t NEW_UID = 424242;
    TEST_ASSERT_TRUE(TestWdgApi::applyPathAccessControl(testDirectory, OLD_UID, true));
    TEST_ASSERT_TRUE(TestWdgApi::applyPathAccessControl(accessPath, OLD_UID, false));
    TEST_ASSERT_TRUE(TestWdgApi::applyPathAccessControl(testDirectory, NEW_UID, true));
    TEST_ASSERT_TRUE(TestWdgApi::applyPathAccessControl(accessPath, NEW_UID, false));

    const std::vector<uid_t> directoryUsers = namedAclUsers(testDirectory);
    const std::vector<uid_t> socketUsers = namedAclUsers(accessPath);
    TEST_ASSERT_EQUAL_UINT(1, directoryUsers.size());
    TEST_ASSERT_EQUAL_UINT(1, socketUsers.size());
    TEST_ASSERT_EQUAL_UINT32(NEW_UID, directoryUsers.front());
    TEST_ASSERT_EQUAL_UINT32(NEW_UID, socketUsers.front());

    struct stat pathStat {
    };
    TEST_ASSERT_EQUAL_INT(0, stat(testDirectory.c_str(), &pathStat));
    TEST_ASSERT_EQUAL_UINT(0770, pathStat.st_mode & 0777);
    TEST_ASSERT_EQUAL_INT(0, stat(accessPath.c_str(), &pathStat));
    TEST_ASSERT_EQUAL_UINT(0660, pathStat.st_mode & 0777);
    unlink(accessPath.c_str());
}

void test_dry_run_bluetooth_override_does_not_change_protobuf_config()
{
    config.bluetooth.enabled = true;
    portduino_config.bluetooth_enabled = true;
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_DISABLE_BLUETOOTH", "1", 1));
    TEST_ASSERT_TRUE(meshtastic::portduino::loadWdgPolicyFromEnvironment());
    TEST_ASSERT_FALSE(meshtastic::portduino::wdgBluetoothAllowed());
    TEST_ASSERT_FALSE(portduino_config.bluetooth_enabled);
    TEST_ASSERT_TRUE(config.bluetooth.enabled);
}

void test_bluetooth_adapter_refresh_recovers_after_hotplug()
{
    portduino_config.bluetooth_adapter = "hci0";
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_BLUETOOTH_SYSFS", testDirectory.c_str(), 1));
    TEST_ASSERT_TRUE(meshtastic::portduino::wdgBluetoothAllowed());
    TEST_ASSERT_FALSE(meshtastic::portduino::refreshWdgBluetoothAdapter());

    const std::string adapterDirectory = testDirectory + "/hci7";
    TEST_ASSERT_EQUAL_INT(0, mkdir(adapterDirectory.c_str(), 0755));
    const std::string addressPath = adapterDirectory + "/address";
    FILE *address = fopen(addressPath.c_str(), "w");
    TEST_ASSERT_NOT_NULL(address);
    TEST_ASSERT_GREATER_THAN_INT(0, fputs("AA:BB:CC:DD:EE:FF\n", address));
    TEST_ASSERT_EQUAL_INT(0, fclose(address));

    TEST_ASSERT_TRUE(meshtastic::portduino::refreshWdgBluetoothAdapter());
    TEST_ASSERT_EQUAL_STRING("hci7", portduino_config.bluetooth_adapter.c_str());

    unlink(addressPath.c_str());
    rmdir(adapterDirectory.c_str());
}

void test_explicit_bluetooth_adapter_survives_auto_policy_refresh()
{
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_BLUETOOTH_SYSFS", testDirectory.c_str(), 1));
    const std::string hci0 = testDirectory + "/hci0";
    const std::string hci1 = testDirectory + "/hci1";
    TEST_ASSERT_EQUAL_INT(0, mkdir(hci0.c_str(), 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir(hci1.c_str(), 0755));
    FILE *first = fopen((hci0 + "/address").c_str(), "w");
    FILE *second = fopen((hci1 + "/address").c_str(), "w");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_GREATER_THAN_INT(0, fputs("00:11:22:33:44:55\n", first));
    TEST_ASSERT_GREATER_THAN_INT(0, fputs("AA:BB:CC:DD:EE:FF\n", second));
    TEST_ASSERT_EQUAL_INT(0, fclose(first));
    TEST_ASSERT_EQUAL_INT(0, fclose(second));

    TEST_ASSERT_TRUE(meshtastic::portduino::selectWdgBluetoothAdapter("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_EQUAL_STRING("hci1", portduino_config.bluetooth_adapter.c_str());
    TEST_ASSERT_TRUE(meshtastic::portduino::refreshWdgBluetoothAdapter());
    TEST_ASSERT_EQUAL_STRING("hci1", portduino_config.bluetooth_adapter.c_str());

    unlink((hci0 + "/address").c_str());
    unlink((hci1 + "/address").c_str());
    rmdir(hci0.c_str());
    rmdir(hci1.c_str());
}

void test_hello_and_status_echo_request_ids()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    struct stat socketStat {
    };
    TEST_ASSERT_EQUAL_INT(0, stat(socketPath.c_str(), &socketStat));
    TEST_ASSERT_EQUAL_UINT(0660, socketStat.st_mode & 0777);
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value hello = exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"hello-1","name":"hello","body":{}})");
    TEST_ASSERT_TRUE(hello["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("hello-1", hello["request_id"].asCString());
    TEST_ASSERT_EQUAL_UINT32(1, hello["body"]["protocol_version"].asUInt());
    TEST_ASSERT_EQUAL_UINT64(WdgApi::MAX_PACKET_BYTES, hello["body"]["max_packet_bytes"].asUInt64());
    TEST_ASSERT_TRUE(arrayContains(hello["body"]["capabilities"], "snapshot_nodes"));
    TEST_ASSERT_TRUE(arrayContains(hello["body"]["capabilities"], "send_text"));
    TEST_ASSERT_TRUE(arrayContains(hello["body"]["capabilities"], "request_node_info"));
    TEST_ASSERT_TRUE(arrayContains(hello["body"]["capabilities"], "ble_scan_lease_acquire"));

    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"status-1","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("status-1", status["request_id"].asCString());
    TEST_ASSERT_EQUAL_STRING("ready", status["body"]["state"].asCString());
    TEST_ASSERT_EQUAL_STRING("unavailable", status["body"]["radio_status"].asCString());
}

void test_status_reports_key_presence_without_key_material()
{
    config.has_security = true;
    config.security.public_key.size = 32;
    config.security.private_key.size = 32;
    memset(config.security.public_key.bytes, 0xa5, config.security.public_key.size);
    memset(config.security.private_key.bytes, 0x5a, config.security.private_key.size);

    channelFile = meshtastic_ChannelFile_init_zero;
    channelFile.channels_count = 2;
    channelFile.channels[0].index = 0;
    channelFile.channels[0].role = meshtastic_Channel_Role_PRIMARY;
    channelFile.channels[0].has_settings = true;
    strncpy(channelFile.channels[0].settings.name, "Private", sizeof(channelFile.channels[0].settings.name) - 1);
    channelFile.channels[0].settings.psk.size = 16;
    memset(channelFile.channels[0].settings.psk.bytes, 0xc3, channelFile.channels[0].settings.psk.size);
    channelFile.channels[1].index = 1;
    channelFile.channels[1].role = meshtastic_Channel_Role_SECONDARY;
    channelFile.channels[1].has_settings = true;
    strncpy(channelFile.channels[1].settings.name, "Open", sizeof(channelFile.channels[1].settings.name) - 1);

    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"keys","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_TRUE(status["body"]["identity"]["has_public_key"].asBool());
    TEST_ASSERT_TRUE(status["body"]["identity"]["has_private_key"].asBool());
    TEST_ASSERT_TRUE(status["body"]["channels"][0]["has_psk"].asBool());
    TEST_ASSERT_FALSE(status["body"]["channels"][1]["has_psk"].asBool());

    const std::string serialized = Json::writeString(Json::StreamWriterBuilder{}, status);
    TEST_ASSERT_TRUE(serialized.find("a5a5") == std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("5a5a") == std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("c3c3") == std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("\"public_key\"") == std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("\"private_key\"") == std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("\"psk\"") == std::string::npos);
}

void test_status_reports_non_secret_phone_pairing_mode()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
    Json::Value random =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"pair-random","name":"get_status","body":{}})");
    TEST_ASSERT_EQUAL_STRING("random_pin", random["body"]["phone_ble_pairing_mode"].asCString());
    TEST_ASSERT_TRUE(random["body"]["phone_ble_pairing_authenticated"].asBool());
    TEST_ASSERT_FALSE(random["body"].isMember("phone_ble_pairing_error"));

    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN;
    Json::Value open =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"pair-open","name":"get_status","body":{}})");
    TEST_ASSERT_EQUAL_STRING("no_pin", open["body"]["phone_ble_pairing_mode"].asCString());
    TEST_ASSERT_FALSE(open["body"]["phone_ble_pairing_authenticated"].asBool());

    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
    config.bluetooth.fixed_pin = 654321;
    Json::Value fixed =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"pair-fixed","name":"get_status","body":{}})");
    TEST_ASSERT_EQUAL_STRING("fixed_pin", fixed["body"]["phone_ble_pairing_mode"].asCString());
    TEST_ASSERT_EQUAL_STRING("fixed_pin_unsupported", fixed["body"]["phone_ble_pairing_error"].asCString());
    const std::string serialized = Json::writeString(Json::StreamWriterBuilder{}, fixed);
    TEST_ASSERT_TRUE(serialized.find(std::to_string(config.bluetooth.fixed_pin)) == std::string::npos);
}

void test_status_reports_full_phone_api_owner_and_transition()
{
    using meshtastic::portduino::FullPhoneApiLease;
    using meshtastic::portduino::fullPhoneApiLease;
    static int tcpHolder;

    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();
    Json::Value hello =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"owner-hello","name":"hello","body":{}})");
    TEST_ASSERT_TRUE(hello["ok"].asBool());
    Json::Value initial =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"owner-initial","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(initial["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("none", initial["body"]["full_client_owner"].asCString());

    TEST_ASSERT_TRUE(fullPhoneApiLease().tryAcquireTcp(&tcpHolder));
    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"owner-status","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("tcp", status["body"]["full_client_owner"].asCString());

    bool sawTransition = false;
    for (size_t i = 0; i < 4 && !sawTransition; ++i) {
        Json::Value event = receiveJson(client);
        if (event.get("type", "").asString() == "event" && event.get("name", "").asString() == "status" &&
            event["body"].get("full_client_owner", "").asString() == "tcp")
            sawTransition = true;
    }
    TEST_ASSERT_TRUE(sawTransition);
    TEST_ASSERT_TRUE(fullPhoneApiLease().release(FullPhoneApiLease::Owner::TCP, &tcpHolder));
}

void test_packets_received_counts_accepted_remote_packets_without_a_phone_client()
{
    TEST_ASSERT_NOT_NULL(service);
    testApi = new TestWdgApi(socketPath);

    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.from = 0x12345678;
    service->wdgRemotePacketAccepted.notifyObservers(&packet);
    packet.id = 2;
    service->wdgRemotePacketAccepted.notifyObservers(&packet);

    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();
    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"packet-count","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_EQUAL_UINT64(2, status["body"]["packets_received"].asUInt64());
}

void test_text_sanitization_enforces_encoded_payload_limit()
{
    std::string sanitized;
    const std::string valid(meshtastic_Constants_DATA_PAYLOAD_LEN, 'a');
    TEST_ASSERT_TRUE(TestWdgApi::sanitizeTextPayload(valid, sanitized));
    TEST_ASSERT_EQUAL_UINT(meshtastic_Constants_DATA_PAYLOAD_LEN, sanitized.size());

    const std::string invalidWithinLimit(77, static_cast<char>(0xff));
    TEST_ASSERT_TRUE(TestWdgApi::sanitizeTextPayload(invalidWithinLimit, sanitized));
    TEST_ASSERT_EQUAL_UINT(231, sanitized.size());

    const std::string invalidPastLimit(78, static_cast<char>(0xff));
    TEST_ASSERT_FALSE(TestWdgApi::sanitizeTextPayload(invalidPastLimit, sanitized));
    TEST_ASSERT_EQUAL_UINT(234, sanitized.size());
}

void test_protocol_errors_are_bounded_replies()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value version =
        exchange(*testApi, client, R"({"v":2,"type":"command","request_id":"bad-version","name":"hello","body":{}})");
    TEST_ASSERT_FALSE(version["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("unsupported_version", version["error_code"].asCString());

    Json::Value unsupported =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"future","name":"future_command","body":{}})");
    TEST_ASSERT_FALSE(unsupported["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("unsupported_command", unsupported["error_code"].asCString());
    TEST_ASSERT_LESS_OR_EQUAL_size_t(WdgApi::MAX_PENDING_REPLIES, testApi->pendingReplyCount());
    TEST_ASSERT_LESS_OR_EQUAL_size_t(WdgApi::MAX_PENDING_REPLY_BYTES, testApi->pendingReplyBytes());

    std::string oversized(WdgApi::MAX_PACKET_BYTES + 1, 'x');
    TEST_ASSERT_EQUAL_INT(oversized.size(), send(client, oversized.data(), oversized.size(), MSG_NOSIGNAL));
    testApi->runOnce();
    char response[4096];
    ssize_t length = recv(client, response, sizeof(response), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, length);
    std::string oversizedReply(response, static_cast<size_t>(length));
    TEST_ASSERT_TRUE(oversizedReply.find("packet_too_large") != std::string::npos);
}

void test_request_ids_are_required_and_cannot_be_replayed()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    const std::string missingId = R"({"v":1,"type":"command","name":"get_status","body":{}})";
    TEST_ASSERT_EQUAL_INT(missingId.size(), send(client, missingId.data(), missingId.size(), MSG_NOSIGNAL));
    testApi->runOnce();
    Json::Value invalid = receiveJson(client);
    TEST_ASSERT_FALSE(invalid["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("invalid_request_id", invalid["error_code"].asCString());

    constexpr const char *request = R"({"v":1,"type":"command","request_id":"same-id","name":"get_status","body":{}})";
    Json::Value first = exchange(*testApi, client, request);
    TEST_ASSERT_TRUE(first["ok"].asBool());
    Json::Value replay = exchange(*testApi, client, request);
    TEST_ASSERT_FALSE(replay["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("duplicate_request_id", replay["error_code"].asCString());
}

void test_snapshot_streams_cached_nodes_without_phone_queue()
{
    createNodeDbFixture();
    constexpr NodeNum REMOTE = 0x00000042;
    meshtastic_NodeInfoLite *node = nodeDB->getOrCreateMeshNode(REMOTE);
    TEST_ASSERT_NOT_NULL(node);
    memcpy(node->long_name, "Near", 4);
    node->long_name[4] = static_cast<char>(0xff);
    node->long_name[5] = '\0';
    strncpy(node->short_name, "NB", sizeof(node->short_name) - 1);
    node->last_heard = 1234;

    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();
    Json::Value hello = exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"hello","name":"hello","body":{}})");
    TEST_ASSERT_TRUE(hello["ok"].asBool());

    const std::string request = R"({"v":1,"type":"command","request_id":"snapshot","name":"snapshot_nodes","body":{}})";
    TEST_ASSERT_EQUAL_INT(request.size(), send(client, request.data(), request.size(), MSG_NOSIGNAL));
    testApi->runOnce();

    bool sawReply = false;
    bool sawBegin = false;
    bool sawNode = false;
    bool sawComplete = false;
    for (size_t i = 0; i < 8 && !sawComplete; ++i) {
        Json::Value value = receiveJson(client);
        if (value.get("type", "").asString() == "reply" && value.get("request_id", "").asString() == "snapshot") {
            sawReply = value["ok"].asBool();
            continue;
        }
        const std::string name = value.get("name", "").asString();
        if (name == "snapshot_begin")
            sawBegin = true;
        else if (name == "node" && value["body"]["id"].asString() == "!00000042") {
            sawNode = true;
            TEST_ASSERT_EQUAL_STRING("Near\xef\xbf\xbd", value["body"]["name"].asCString());
            TEST_ASSERT_TRUE(value["body"]["cached"].asBool());
        } else if (name == "snapshot_complete")
            sawComplete = true;
    }
    TEST_ASSERT_TRUE(sawReply);
    TEST_ASSERT_TRUE(sawBegin);
    TEST_ASSERT_TRUE(sawNode);
    TEST_ASSERT_TRUE(sawComplete);
}

void test_observer_overflow_requests_a_snapshot_resync()
{
    createNodeDbFixture();
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();
    Json::Value hello = exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"hello","name":"hello","body":{}})");
    TEST_ASSERT_TRUE(hello["ok"].asBool());

    for (NodeNum node = 2; node < 302; ++node)
        nodeDB->wdgNodeChanged.notifyObservers(node);
    testApi->runOnce();

    bool sawOverflow = false;
    for (size_t i = 0; i < 4 && !sawOverflow; ++i) {
        Json::Value event = receiveJson(client);
        if (event.get("type", "").asString() == "event" && event.get("name", "").asString() == "overflow") {
            sawOverflow = true;
            TEST_ASSERT_TRUE(event["body"]["resync_required"].asBool());
            TEST_ASSERT_GREATER_THAN_UINT(0, event["body"]["count"].asUInt64());
        }
    }
    TEST_ASSERT_TRUE(sawOverflow);
}

void test_ble_lease_is_safe_when_phone_ble_is_unavailable()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value response =
        exchange(*testApi, client,
                 R"({"v":1,"type":"command","request_id":"lease","name":"ble_scan_lease_acquire","body":{"seconds":999}})");
    TEST_ASSERT_TRUE(response["ok"].asBool());
    TEST_ASSERT_TRUE(response["body"]["granted"].asBool());
    TEST_ASSERT_EQUAL_STRING("phone BLE disabled", response["body"]["reason"].asCString());
}

void test_phone_ble_disable_preserves_adapter_and_rejects_mixed_update()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    config.bluetooth.enabled = true;
    portduino_config.bluetooth_enabled = true;
    portduino_config.bluetooth_adapter = "hci0";
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_BLUETOOTH_SYSFS", testDirectory.c_str(), 1));

    Json::Value disabled = exchange(
        *testApi, client, R"({"v":1,"type":"command","request_id":"disable","name":"set_phone_ble","body":{"enabled":false}})");
    TEST_ASSERT_TRUE(disabled["ok"].asBool());
    TEST_ASSERT_FALSE(disabled["body"]["enabled"].asBool());
    TEST_ASSERT_FALSE(config.bluetooth.enabled);
    TEST_ASSERT_FALSE(portduino_config.bluetooth_enabled);
    TEST_ASSERT_EQUAL_STRING("hci0", portduino_config.bluetooth_adapter.c_str());

    Json::Value mixed = exchange(
        *testApi, client,
        R"({"v":1,"type":"command","request_id":"disable-adapter","name":"set_phone_ble","body":{"enabled":false,"adapter":"AA:BB:CC:DD:EE:FF"}})");
    TEST_ASSERT_FALSE(mixed["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("adapter_requires_enabled", mixed["error_code"].asCString());
    TEST_ASSERT_FALSE(config.bluetooth.enabled);
    TEST_ASSERT_FALSE(portduino_config.bluetooth_enabled);
    TEST_ASSERT_EQUAL_STRING("hci0", portduino_config.bluetooth_adapter.c_str());

    Json::Value invalid = exchange(
        *testApi, client,
        R"({"v":1,"type":"command","request_id":"bad-adapter","name":"set_phone_ble","body":{"enabled":true,"adapter":"hci999"}})");
    TEST_ASSERT_FALSE(invalid["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("invalid_adapter", invalid["error_code"].asCString());
    TEST_ASSERT_EQUAL_STRING("hci0", portduino_config.bluetooth_adapter.c_str());
}

void test_text_rate_limit_is_shared_by_local_clients()
{
    Time::setTestMillis(UINT32_MAX - 500);
    MeshService localService;
    TEST_ASSERT_TRUE(localService.tryReserveTextMessageSend());
    Time::advanceTestMillis(1000);
    TEST_ASSERT_FALSE(localService.tryReserveTextMessageSend());
    Time::advanceTestMillis(1000);
    TEST_ASSERT_TRUE(localService.tryReserveTextMessageSend());
}

void test_text_rate_limit_allows_only_one_concurrent_reservation()
{
    Time::setTestMillis(1234);
    MeshService localService;
    std::atomic<size_t> waiting{0};
    std::atomic<size_t> accepted{0};
    std::atomic<bool> start{false};
    std::array<std::thread, 8> workers;
    for (std::thread &worker : workers) {
        worker = std::thread([&] {
            waiting.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (localService.tryReserveTextMessageSend())
                accepted.fetch_add(1, std::memory_order_relaxed);
        });
    }
    while (waiting.load(std::memory_order_acquire) != workers.size())
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (std::thread &worker : workers)
        worker.join();
    TEST_ASSERT_EQUAL_UINT(1, accepted.load());
}

void test_discovery_rejects_nonzero_hops_without_touching_core()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value response = exchange(
        *testApi, client, R"({"v":1,"type":"command","request_id":"routed","name":"request_node_info","body":{"hop_limit":1}})");
    TEST_ASSERT_FALSE(response["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("invalid_hop_limit", response["error_code"].asCString());

    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"still-open","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());

    Json::Value unavailable =
        exchange(*testApi, client,
                 R"({"v":1,"type":"command","request_id":"zero-hop","name":"request_node_info","body":{"hop_limit":0}})");
    TEST_ASSERT_FALSE(unavailable["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("not_ready", unavailable["error_code"].asCString());
}

void test_peer_authorizer_can_reject_connection()
{
    bool sawCredentials = false;
    testApi = new TestWdgApi(socketPath, [&sawCredentials](const WdgPeerCredentials &credentials) {
        sawCredentials = credentials.pid > 0;
        return false;
    });
    TEST_ASSERT_TRUE(testApi->start());
    int client = connectClient(socketPath);
    testApi->runOnce();

    TEST_ASSERT_TRUE(sawCredentials);
    TEST_ASSERT_FALSE(testApi->hasClient());
    char byte = 0;
    TEST_ASSERT_EQUAL_INT(0, recv(client, &byte, sizeof(byte), 0));
}

void test_only_one_client_is_accepted()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    int first = connectClient(socketPath);
    testApi->runOnce();
    TEST_ASSERT_TRUE(testApi->hasClient());

    int second = connectClient(socketPath);
    testApi->runOnce();
    char byte = 0;
    TEST_ASSERT_EQUAL_INT(0, recv(second, &byte, sizeof(byte), 0));

    Json::Value status =
        exchange(*testApi, first, R"({"v":1,"type":"command","request_id":"still-first","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("still-first", status["request_id"].asCString());
}

void test_accept_work_is_bounded_per_cooperative_poll()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());

    std::vector<int> clients;
    for (size_t i = 0; i < WdgApi::MAX_ACCEPTS_PER_POLL + 4; ++i)
        clients.push_back(connectClient(socketPath));

    testApi->runOnce();
    TEST_ASSERT_TRUE(testApi->hasClient());
    size_t closed = 0;
    for (int client : clients) {
        char byte = 0;
        if (recv(client, &byte, sizeof(byte), MSG_DONTWAIT) == 0)
            ++closed;
    }
    TEST_ASSERT_EQUAL_UINT(WdgApi::MAX_ACCEPTS_PER_POLL - 1, closed);

    testApi->runOnce();
    closed = 0;
    for (int client : clients) {
        char byte = 0;
        if (recv(client, &byte, sizeof(byte), MSG_DONTWAIT) == 0)
            ++closed;
    }
    TEST_ASSERT_EQUAL_UINT(clients.size() - 1, closed);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_socket_path_uses_environment_override);
    RUN_TEST(test_policy_parser_enforces_security_contract);
    RUN_TEST(test_policy_file_rejects_writable_or_symlinked_input);
    RUN_TEST(test_acl_replaces_stale_named_uid_and_preserves_modes);
    RUN_TEST(test_dry_run_bluetooth_override_does_not_change_protobuf_config);
    RUN_TEST(test_bluetooth_adapter_refresh_recovers_after_hotplug);
    RUN_TEST(test_explicit_bluetooth_adapter_survives_auto_policy_refresh);
    RUN_TEST(test_hello_and_status_echo_request_ids);
    RUN_TEST(test_status_reports_key_presence_without_key_material);
    RUN_TEST(test_status_reports_non_secret_phone_pairing_mode);
    RUN_TEST(test_status_reports_full_phone_api_owner_and_transition);
    RUN_TEST(test_packets_received_counts_accepted_remote_packets_without_a_phone_client);
    RUN_TEST(test_text_sanitization_enforces_encoded_payload_limit);
    RUN_TEST(test_protocol_errors_are_bounded_replies);
    RUN_TEST(test_request_ids_are_required_and_cannot_be_replayed);
    RUN_TEST(test_snapshot_streams_cached_nodes_without_phone_queue);
    RUN_TEST(test_observer_overflow_requests_a_snapshot_resync);
    RUN_TEST(test_discovery_rejects_nonzero_hops_without_touching_core);
    RUN_TEST(test_ble_lease_is_safe_when_phone_ble_is_unavailable);
    RUN_TEST(test_phone_ble_disable_preserves_adapter_and_rejects_mixed_update);
    RUN_TEST(test_text_rate_limit_is_shared_by_local_clients);
    RUN_TEST(test_text_rate_limit_allows_only_one_concurrent_reservation);
    RUN_TEST(test_peer_authorizer_can_reject_connection);
    RUN_TEST(test_only_one_client_is_accepted);
    RUN_TEST(test_accept_work_is_bounded_per_cooperative_poll);
    exit(UNITY_END());
}

void loop() {}

#else

void setUp() {}
void tearDown() {}

void test_wdg_api_is_excluded_from_this_environment()
{
    TEST_ASSERT_TRUE(true);
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_wdg_api_is_excluded_from_this_environment);
    exit(UNITY_END());
}

void loop() {}

#endif
