#include "MeshTypes.h"
#include "TestUtil.h"
#include "configuration.h"
#include <unity.h>

#if defined(MESHTASTIC_WDG_API) && defined(__linux__)

#include "platform/portduino/WdgApi.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <json/json.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

using meshtastic::portduino::WdgApi;
using meshtastic::portduino::WdgPeerCredentials;

class TestWdgApi : public WdgApi
{
  public:
    using WdgApi::runOnce;
    using WdgApi::WdgApi;
};

static std::string testDirectory;
static std::string socketPath;
static TestWdgApi *testApi = nullptr;
static std::vector<int> clientFds;

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

    char response[4096];
    ssize_t length = recv(client, response, sizeof(response), 0);
    TEST_ASSERT_GREATER_THAN_INT(0, length);

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    TEST_ASSERT_TRUE_MESSAGE(reader->parse(response, response + length, &value, &errors), errors.c_str());
    return value;
}

void setUp()
{
    testApi = nullptr;
    clientFds.clear();
    char pathTemplate[] = "/tmp/meshtastic-wdg-api-XXXXXX";
    char *created = mkdtemp(pathTemplate);
    TEST_ASSERT_NOT_NULL(created);
    testDirectory = created;
    socketPath = testDirectory + "/wdg.sock";
}

void tearDown()
{
    delete testApi;
    testApi = nullptr;
    for (int fd : clientFds)
        close(fd);
    clientFds.clear();
    unlink(socketPath.c_str());
    rmdir(testDirectory.c_str());
    unsetenv("MESHTASTIC_WDG_SOCKET");
    unsetenv("MESHTASTIC_WDG_ALLOWED_UID");
}

void test_socket_path_uses_environment_override()
{
    TEST_ASSERT_EQUAL_INT(0, setenv("MESHTASTIC_WDG_SOCKET", socketPath.c_str(), 1));
    TEST_ASSERT_EQUAL_STRING(socketPath.c_str(), WdgApi::resolveSocketPath().c_str());
}

void test_hello_and_status_echo_request_ids()
{
    testApi = new TestWdgApi(socketPath);
    TEST_ASSERT_TRUE(testApi->start());
    struct stat socketStat{};
    TEST_ASSERT_EQUAL_INT(0, stat(socketPath.c_str(), &socketStat));
    TEST_ASSERT_EQUAL_UINT(0660, socketStat.st_mode & 0777);
    int client = connectClient(socketPath);
    testApi->runOnce();

    Json::Value hello = exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"hello-1","name":"hello","body":{}})");
    TEST_ASSERT_TRUE(hello["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("hello-1", hello["request_id"].asCString());
    TEST_ASSERT_EQUAL_UINT32(1, hello["body"]["protocol_version"].asUInt());
    TEST_ASSERT_EQUAL_UINT64(WdgApi::MAX_PACKET_BYTES, hello["body"]["max_packet_bytes"].asUInt64());

    Json::Value status =
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"status-1","name":"get_status","body":{}})");
    TEST_ASSERT_TRUE(status["ok"].asBool());
    TEST_ASSERT_EQUAL_STRING("status-1", status["request_id"].asCString());
    TEST_ASSERT_EQUAL_STRING("ready", status["body"]["state"].asCString());
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
        exchange(*testApi, client, R"({"v":1,"type":"command","request_id":"future","name":"snapshot_nodes","body":{}})");
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

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_socket_path_uses_environment_override);
    RUN_TEST(test_hello_and_status_echo_request_ids);
    RUN_TEST(test_protocol_errors_are_bounded_replies);
    RUN_TEST(test_peer_authorizer_can_reject_connection);
    RUN_TEST(test_only_one_client_is_accepted);
    exit(UNITY_END());
}

void loop() {}

#else

void setUp() {}
void tearDown() {}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}

void loop() {}

#endif
