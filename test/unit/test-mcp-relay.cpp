#include <catch2/catch_test_macros.hpp>

#include "../../libs/http-service/src/mcp-viewer-relay.h"

#include <fstream>
#include <future>

using namespace mapget::detail;
using Json = nlohmann::json;

namespace
{

/** Deterministic relay peer: no HTTP, sleeps, worker threads or running viewer required. */
class ViewerRelayTest
{
public:
    static constexpr auto first = "b3e68f32-3b51-472d-8cab-14b597f7de91";
    static constexpr auto second = "b3e68f32-3b51-472d-8cab-14b597f7de92";
    static constexpr auto third = "b3e68f32-3b51-472d-8cab-14b597f7de93";

    std::chrono::system_clock::time_point wall = std::chrono::system_clock::now();
    std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::now();
    std::map<std::string, std::vector<Json>> messages;
    std::vector<Json> replies;
    bool transportAvailable = true;
    std::function<void(Json const&)> onSend;
    McpViewerRelay relay;

    /** Construct the relay after the state captured by its clock/transport callbacks. */
    explicit ViewerRelayTest(mapget::McpConfig::Limits limits = {})
        : relay(
              catalog(),
              limits,
              [this] { return wall; },
              [this] { return steady; })
    {
    }

    /** Read the coordinated contract snapshot, independent of a sibling checkout. */
    static Json fixture(std::string const& name)
    {
        std::ifstream file(std::filesystem::path(MAPGET_TEST_DATA_DIR) / "viewer-actions" / name);
        return Json::parse(file);
    }

    /** Share an immutable compiled fixture just as connections share the deployed catalog. */
    static std::shared_ptr<McpActionCatalog const> catalog()
    {
        static auto value = std::make_shared<McpActionCatalog>(fixture("web-mcp-actions.json"));
        return value;
    }

    /** Create verified test identities; production token verification is a separate boundary. */
    McpViewerRelay::Principal principal(std::string subject = "alice") const
    {
        return {
            "https://issuer.example",
            std::move(subject),
            wall + std::chrono::minutes(5),
            true,
            true};
    }

    /** Open a connection and check emitted controls against the shared browser schema. */
    bool attach(
        std::string clientId,
        McpViewerRelay::Principal identity,
        std::string origin = "https://viewer.example")
    {
        return relay.attach(
            clientId,
            std::move(identity),
            std::move(origin),
            [this, clientId](Json const& value)
            {
                static nlohmann::json_schema::json_validator const
                    validator(fixture("viewer-action-relay.schema.json").at("server"));
                nlohmann::json_schema::basic_error_handler errors;
                validator.validate(value, errors);
                CHECK_FALSE(static_cast<bool>(errors));
                messages[clientId].push_back(value);
                // A real transport is asynchronous, but this probes reentrant lifecycle edges.
                auto callback = onSend;
                if (callback) {
                    callback(value);
                }
                return transportAvailable;
            });
    }

    /** Advertise the fixture handlers, optionally as an update to an existing registration. */
    Json registration(bool update = false) const
    {
        return {
            {"type", update ? "mapget.actions.update" : "mapget.actions.register"},
            {"version", 1},
            {"catalogId", catalog()->id()},
            {"label", "Viewer"},
            {"actions",
             {"viewer_describe_app_state", "viewer_get_app_state", "viewer_set_app_state"}}};
    }

    /** Attach and register one compatible browser without making any tile request. */
    void open(
        std::string const& clientId,
        std::string subject = "alice",
        std::string origin = "https://viewer.example")
    {
        REQUIRE(attach(clientId, principal(std::move(subject)), std::move(origin)));
        REQUIRE(relay.receive(clientId, registration()));
    }

    /** Take a valid application argument fixture and add native routing. */
    static Json arguments(std::string const& action, std::string const& clientId)
    {
        auto cases = fixture("fixtures.json");
        for (auto const& entry : cases.at("actions")) {
            if (entry.at("action") == action && entry.at("valid").get<bool>()) {
                auto value = entry.at("arguments");
                value["clientId"] = clientId;
                return value;
            }
        }
        throw std::logic_error("Missing action argument fixture.");
    }

    /** Collect asynchronous results separately from the returned cancellation handle. */
    std::string
    invoke(std::string const& clientId, bool mutation = false, std::string subject = "alice")
    {
        std::string action = mutation ? "viewer_set_app_state" : "viewer_get_app_state";
        return relay.invoke(
            principal(std::move(subject)),
            action,
            arguments(action, clientId),
            [this](Json value) { replies.push_back(std::move(value)); });
    }

    /** Return a valid terminal result, including for a caller which already timed out. */
    bool reply(std::string const& clientId, std::string const& callId, bool mutation = false)
    {
        auto cases = fixture("fixtures.json");
        for (auto const& entry : cases.at("results")) {
            if (entry.at("action") ==
                    (mutation ? "viewer_set_app_state" : "viewer_get_app_state") &&
                entry.at("valid").get<bool>())
            {
                return relay.receive(
                    clientId,
                    {{"type", "mapget.actions.result"},
                     {"version", 1},
                     {"callId", callId},
                     {"result", entry.at("value")}});
            }
        }
        throw std::logic_error("Missing action result fixture.");
    }
};

}  // namespace

TEST_CASE("MCP viewer registration preserves verified connection ownership", "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first, "alice", "https://first.example");
    test.open(test.second, "alice", "https://second.example");
    test.open(test.third, "bob");
    CHECK(test.relay.sessions(test.principal()).size() == 2);
    CHECK(test.relay.sessions(test.principal("bob")).size() == 1);
    CHECK(test.relay.sessions(test.principal("unknown")).empty());
    auto impostor = test.principal();
    impostor.issuer = "https://another-issuer.example";
    CHECK(test.relay.sessions(impostor).empty());
    impostor = test.principal();
    impostor.read = false;
    CHECK(test.relay.sessions(impostor).empty());
    impostor = test.principal();
    impostor.expiresAt = test.wall;
    CHECK(test.relay.sessions(impostor).empty());
    CHECK_FALSE(test.attach(test.first, test.principal("bob")));
    CHECK_FALSE(test.attach("1", test.principal()));
    CHECK_FALSE(test.attach("not-a-uuid", test.principal()));
    CHECK_FALSE(test.attach("b3e68f32-3b51-472d-8cab-14b597f7de94", {}));

    auto before = test.relay.sessions(test.principal());
    auto update = test.registration(true);
    update["catalogId"] = "sha256:" + std::string(64, 'b');
    CHECK_FALSE(test.relay.receive(test.first, update));
    CHECK(test.relay.sessions(test.principal()) == before);
    CHECK(test.messages[test.first].back().at("type") == "mapget.actions.error");
    CHECK_FALSE(test.messages[test.first].back().contains("requestId"));

    update = test.registration(true);
    update["owner"] = "bob";
    CHECK_FALSE(test.relay.receive(test.first, update));
    update = test.registration(true);
    update["actions"].push_back("mapget_get_config");
    CHECK_FALSE(test.relay.receive(test.first, update));
    CHECK(test.relay.sessions(test.principal()) == before);
    update = test.registration(true);
    update["label"] = "Renamed tab";
    update["actions"] = Json::array();
    CHECK(test.relay.receive(test.first, update));
    CHECK(test.relay.sessions(test.principal()).at(0).at("label") == "Renamed tab");
    CHECK(test.invoke(test.first).empty());
    CHECK(test.replies.back().at("error").at("code") == "unsupported_action");
}

TEST_CASE("MCP viewer calls are bound to their exact connection and action schema", "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    test.open(test.second);
    test.open(test.third, "bob");
    CHECK(test.invoke(test.first, false, "bob").empty());
    auto denied = test.replies.back();
    CHECK(test.invoke("b3e68f32-3b51-472d-8cab-14b597f7de99", false, "bob").empty());
    CHECK(test.replies.back() == denied);
    test.replies.clear();

    auto id = test.invoke(test.first);
    REQUIRE_FALSE(id.empty());
    CHECK(test.replies.empty());
    auto message = test.messages[test.first].back();
    CHECK(message.at("type") == "mapget.actions.invoke");
    CHECK(message.at("callId") == id);
    CHECK(message.at("timeoutMs") == 30000);
    CHECK_FALSE(message.at("arguments").contains("clientId"));
    CHECK_FALSE(message.contains("requestId"));
    CHECK_FALSE(test.reply(test.second, id));
    CHECK_FALSE(test.reply(test.third, id));
    CHECK(test.replies.empty());
    CHECK(test.reply(test.first, id));
    REQUIRE(test.replies.size() == 1);
    CHECK(test.replies.back().contains("result"));
    CHECK_FALSE(test.reply(test.first, id));
    CHECK(test.replies.size() == 1);

    auto readOnly = test.principal();
    readOnly.control = false;
    CHECK(test.relay
              .invoke(
                  readOnly,
                  "viewer_set_app_state",
                  test.arguments("viewer_set_app_state", test.first),
                  [&](Json reply) { test.replies.push_back(std::move(reply)); })
              .empty());
    CHECK(test.replies.back().at("error").at("code") == "not_available");
}

TEST_CASE("MCP relay limits reject new work instead of building another queue", "[mcp-actions]")
{
    mapget::McpConfig::Limits limits;
    std::string constrained;
    SECTION("Per-tab cap")
    {
        limits.callsPerSession = 1;
        constrained = ViewerRelayTest::first;
    }
    SECTION("Per-principal cap across tabs")
    {
        limits.callsPerPrincipal = 1;
        constrained = ViewerRelayTest::second;
    }
    SECTION("Process-wide cap across users")
    {
        limits.pendingCalls = 1;
        constrained = ViewerRelayTest::third;
    }
    ViewerRelayTest test(limits);
    test.open(test.first);
    test.open(test.second);
    test.open(test.third, "bob");
    auto id = test.invoke(test.first);
    REQUIRE_FALSE(id.empty());
    bool otherUser = constrained == test.third;
    auto before = test.messages[constrained].size();
    CHECK(test.invoke(constrained, false, otherUser ? "bob" : "alice").empty());
    CHECK(test.messages[constrained].size() == before);
    CHECK(test.replies.back().at("error").at("code") == "busy");
    CHECK(test.reply(test.first, id));
    CHECK_FALSE(test.invoke(constrained, false, otherUser ? "bob" : "alice").empty());
}

TEST_CASE(
    "MCP synchronous transport callbacks see admitted and retired call state",
    "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    SECTION("Invoke can synchronously deliver its result")
    {
        test.onSend = [&](Json const& message)
        {
            if (message.at("type") == "mapget.actions.invoke") {
                CHECK(test.reply(test.first, message.at("callId").get<std::string>(), true));
            }
        };
        auto id = test.invoke(test.first, true);
        REQUIRE_FALSE(id.empty());
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().contains("result"));
        CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == false);
    }
    SECTION("Cancel can synchronously confirm termination without reviving its callback")
    {
        auto id = test.invoke(test.first, true);
        test.onSend = [&](Json const& message)
        {
            if (message.at("type") == "mapget.actions.cancel") {
                CHECK_FALSE(test.relay.receive(
                    test.first,
                    {{"type", "mapget.actions.result"},
                     {"version", 1},
                     {"callId", message.at("callId")},
                     {"error", {{"code", "cancelled"}, {"message", "Stopped"}}}}));
            }
        };
        test.relay.cancel(id);
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().at("error").at("code") == "cancelled");
        CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == false);
    }
    SECTION("Completion may immediately admit the next mutation")
    {
        std::string next;
        auto id = test.relay.invoke(
            test.principal(),
            "viewer_set_app_state",
            test.arguments("viewer_set_app_state", test.first),
            [&](Json reply)
            {
                REQUIRE(reply.contains("result"));
                next = test.invoke(test.first, true);
            });
        CHECK(test.reply(test.first, id, true));
        REQUIRE_FALSE(next.empty());
        CHECK(next != id);
        CHECK(test.reply(test.first, next, true));
        REQUIRE(test.replies.size() == 1);
    }
    test.onSend = {};
}

TEST_CASE("MCP abandoned mutations still count toward cross-tab admission limits", "[mcp-actions]")
{
    mapget::McpConfig::Limits limits;
    SECTION("Principal limit")
    {
        limits.callsPerPrincipal = 1;
    }
    SECTION("Global limit")
    {
        limits.pendingCalls = 1;
    }
    ViewerRelayTest test(limits);
    test.open(test.first);
    test.open(test.second);
    auto id = test.invoke(test.first, true);
    REQUIRE_FALSE(id.empty());
    test.relay.cancel(id);
    CHECK(test.invoke(test.second).empty());
    CHECK(test.replies.back().at("error").at("code") == "busy");
    CHECK_FALSE(test.reply(test.first, id, true));
    CHECK_FALSE(test.invoke(test.second).empty());
}

TEST_CASE(
    "MCP relay registration cannot reset a mutation or extend browser authority",
    "[mcp-actions]")
{
    mapget::McpConfig::Limits limits;
    limits.sessions = 1;
    ViewerRelayTest test(limits);
    auto identity = test.principal();
    identity.expiresAt = test.wall + std::chrono::seconds(2);
    REQUIRE(test.attach(test.first, identity));
    CHECK_FALSE(test.relay.receive(test.first, test.registration(true)));
    REQUIRE(test.relay.receive(test.first, test.registration()));
    CHECK_FALSE(test.attach(test.second, test.principal()));
    auto id = test.invoke(test.first, true);
    REQUIRE_FALSE(id.empty());
    REQUIRE(test.relay.receive(test.first, test.registration()));
    CHECK(test.invoke(test.first, true).empty());
    auto update = test.registration(true);
    update["expiresAt"] = "2099-01-01";
    CHECK_FALSE(test.relay.receive(test.first, update));
    test.wall += std::chrono::seconds(3);
    CHECK_FALSE(test.relay.receive(test.first, test.registration()));
    CHECK(test.relay.sessions(test.principal()).empty());
    CHECK(test.replies.back().at("error").at("code") == "disconnected");
    CHECK(test.messages[test.first].back().at("type") == "mapget.actions.cancel");
    // A different, freshly authenticated connection is independent of the expired UUID.
    test.open(test.second);
    CHECK_FALSE(test.reply(test.second, id, true));
    CHECK_FALSE(test.invoke(test.second, true).empty());
}

TEST_CASE(
    "MCP mutation ownership survives caller timeout or cancellation until a terminal reply",
    "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    test.open(test.second);
    auto mutation = test.invoke(test.first, true);
    REQUIRE_FALSE(mutation.empty());
    CHECK(test.invoke(test.first, true).empty());
    CHECK(test.replies.back().at("error").at("code") == "busy");
    auto read = test.invoke(test.first);
    REQUIRE_FALSE(read.empty());
    REQUIRE(test.reply(test.first, read));
    test.replies.clear();

    SECTION("Monotonic timeout despite unchanged wall clock")
    {
        test.steady += std::chrono::seconds(31);
        test.relay.expire();
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().at("error").at("code") == "timeout");
    }
    SECTION("Caller cancellation")
    {
        test.relay.cancel(mutation);
        test.relay.cancel(mutation);
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().at("error").at("code") == "cancelled");
    }
    CHECK(test.replies.back().at("error").at("outcome") == "unknown");
    CHECK(test.messages[test.first].back().at("type") == "mapget.actions.cancel");
    CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == true);
    CHECK(test.invoke(test.first, true).empty());
    CHECK_FALSE(test.reply(test.second, mutation, true));
    CHECK(test.invoke(test.first, true).empty());
    CHECK_FALSE(test.invoke(test.first).empty());

    auto replyCount = test.replies.size();
    CHECK_FALSE(test.reply(test.first, mutation, true));
    CHECK(test.replies.size() == replyCount);
    CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == false);
    CHECK_FALSE(test.invoke(test.first, true).empty());
}

TEST_CASE("MCP terminal replies after the deadline cannot beat the expiry sweep", "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    auto id = test.invoke(test.first, true);
    test.steady += std::chrono::seconds(31);
    CHECK_FALSE(test.reply(test.first, id, true));
    REQUIRE(test.replies.size() == 1);
    CHECK(test.replies.back().at("error").at("code") == "timeout");
    // The terminal reply confirms completion, so no quarantine is needed.
    CHECK_FALSE(test.invoke(test.first, true).empty());
}

TEST_CASE("MCP browser authorization expiry explicitly retires UI registration", "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    std::string mutation;
    bool incoming = false;
    SECTION("An idle tab is notified by the expiry sweep") {}
    SECTION("An incoming registration cannot extend its authority")
    {
        incoming = true;
    }
    SECTION("A live mutation is cancelled after registration is retired")
    {
        mutation = test.invoke(test.first, true);
        REQUIRE_FALSE(mutation.empty());
    }
    test.messages[test.first].clear();
    test.onSend = [&](Json const& message)
    {
        CHECK_FALSE(test.relay.attached(test.first));
        if (message.at("type") == "mapget.actions.error" && !mutation.empty()) {
            CHECK_FALSE(test.reply(test.first, mutation, true));
        }
    };
    test.wall += std::chrono::minutes(6);
    if (incoming) {
        CHECK_FALSE(test.relay.receive(test.first, test.registration()));
    }
    else {
        test.relay.expire();
    }
    auto const messages = test.messages[test.first];
    REQUIRE_FALSE(messages.empty());
    CHECK(messages.front().at("type") == "mapget.actions.error");
    CHECK(messages.front().at("operation") == "register");
    CHECK(messages.front().at("error").at("code") == "not_available");
    CHECK(test.relay.sessions(test.principal()).empty());
    if (!mutation.empty()) {
        REQUIRE(messages.size() == 2);
        CHECK(messages.back().at("type") == "mapget.actions.cancel");
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.front().at("error").at("code") == "disconnected");
    }
    test.relay.expire();
    CHECK(test.messages[test.first] == messages);
    test.onSend = {};
    test.open(test.second);
    CHECK(test.relay.sessions(test.principal()).size() == 1);
}

TEST_CASE("MCP authorization expiry and disconnect prevent sensitive late results", "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    auto id = test.invoke(test.first, true);
    SECTION("Browser connection expiry")
    {
        test.wall += std::chrono::minutes(6);
        test.relay.expire();
    }
    SECTION("Socket disconnected")
    {
        test.relay.disconnect(test.first);
    }
    SECTION("Service shutdown")
    {
        test.relay.shutdown();
    }
    REQUIRE(test.replies.size() == 1);
    CHECK(test.replies.back().at("error").at("code") == "disconnected");
    CHECK_FALSE(test.replies.back().contains("result"));
    CHECK(test.relay.sessions(test.principal()).empty());
    CHECK_FALSE(test.reply(test.first, id, true));
    CHECK(test.replies.size() == 1);
}

TEST_CASE(
    "MCP call deadlines are capped by caller authority and configured limits",
    "[mcp-actions]")
{
    mapget::McpConfig::Limits limits;
    limits.timeout = std::chrono::seconds(60);
    ViewerRelayTest test(limits);
    test.open(test.first);
    auto caller = test.principal();
    caller.expiresAt = test.wall + std::chrono::seconds(2);
    auto id = test.relay.invoke(
        caller,
        "viewer_get_app_state",
        test.arguments("viewer_get_app_state", test.first),
        [&](Json reply) { test.replies.push_back(std::move(reply)); });
    REQUIRE_FALSE(id.empty());
    CHECK(test.messages[test.first].back().at("timeoutMs") == 2000);
    test.wall += std::chrono::seconds(3);
    CHECK_FALSE(test.reply(test.first, id));
    REQUIRE(test.replies.size() == 1);
    CHECK(test.replies.back().at("error").at("code") == "not_available");
    CHECK_FALSE(test.invoke(test.first).empty());
    CHECK(test.messages[test.first].back().at("timeoutMs") == 60000);
}

TEST_CASE(
    "MCP invalid results cannot leak data or silently release uncertain mutations",
    "[mcp-actions]")
{
    mapget::McpConfig::Limits limits;
    limits.resultBytes = 1024;
    ViewerRelayTest test(limits);
    test.open(test.first);
    auto id = test.invoke(test.first, true);
    Json message{
        {"type", "mapget.actions.result"},
        {"version", 1},
        {"callId", id},
        {"result", Json::object()}};
    bool envelopeInvalid = false;
    SECTION("Schema-invalid object is terminal but not returned")
    {
        message["result"]["secret"] = "do-not-forward";
    }
    SECTION("Mutually exclusive result and error")
    {
        message["error"] = {{"code", "internal_error"}, {"message", "bad"}};
        envelopeInvalid = true;
    }
    SECTION("Over-budget result")
    {
        message["result"]["large"] = std::string(1024, 'x');
        envelopeInvalid = true;
    }
    SECTION("Deep nesting")
    {
        auto deep = Json::object();
        for (size_t i = 0; i < 66; ++i) {
            deep = {{"child", std::move(deep)}};
        }
        message["result"] = std::move(deep);
        envelopeInvalid = true;
    }
    CHECK_FALSE(test.relay.receive(test.first, message));
    REQUIRE(test.replies.size() == 1);
    CHECK(test.replies.back().at("error").at("code") == "internal_error");
    CHECK(test.replies.back().dump().find("do-not-forward") == std::string::npos);
    CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == envelopeInvalid);
    CHECK_FALSE(test.relay.receive(
        test.first,
        {{"type", "mapget.actions.result"},
         {"version", 1},
         {"callId", id},
         {"error", {{"code", "cancelled"}, {"message", "Stopped"}, {"outcome", "unknown"}}}}));
    CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == false);
}

TEST_CASE(
    "MCP relay contains transport and completion failures without retaining work",
    "[mcp-actions]")
{
    ViewerRelayTest test;
    test.open(test.first);
    SECTION("Failed delivery disconnects the peer")
    {
        test.transportAvailable = false;
        CHECK_FALSE(test.invoke(test.first, true).empty());
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().at("error").at("code") == "disconnected");
        CHECK(test.relay.sessions(test.principal()).empty());
    }
    SECTION("Failed registration-error delivery also disconnects pending calls")
    {
        auto id = test.invoke(test.first, true);
        REQUIRE_FALSE(id.empty());
        test.transportAvailable = false;
        auto message = test.registration(true);
        SECTION("Invalid envelope")
        {
            message["owner"] = "bob";
        }
        SECTION("Incompatible catalog")
        {
            message["catalogId"] = "sha256:" + std::string(64, 'b');
        }
        CHECK_FALSE(test.relay.receive(test.first, message));
        REQUIRE(test.replies.size() == 1);
        CHECK(test.replies.back().at("error").at("code") == "disconnected");
        CHECK(test.relay.sessions(test.principal()).empty());
    }
    SECTION("Reentrant completion sees the already released mutation slot")
    {
        auto id = test.relay.invoke(
            test.principal(),
            "viewer_set_app_state",
            test.arguments("viewer_set_app_state", test.first),
            [&](Json reply)
            {
                CHECK(reply.contains("result"));
                CHECK(test.relay.sessions(test.principal()).at(0).at("mutationBusy") == false);
                test.relay.disconnect(test.first);
            });
        CHECK(test.reply(test.first, id, true));
        CHECK(test.relay.sessions(test.principal()).empty());
    }
    SECTION("Throwing completion does not escape or leak the mutation slot")
    {
        auto id = test.relay.invoke(
            test.principal(),
            "viewer_set_app_state",
            test.arguments("viewer_set_app_state", test.first),
            [](Json) { throw std::runtime_error("Test callback failure"); });
        CHECK(test.reply(test.first, id, true));
        CHECK_FALSE(test.invoke(test.first, true).empty());
    }
    SECTION("Wrong thread is rejected rather than waiting behind a coarse lock")
    {
        auto result = std::async(
            std::launch::async,
            [&]
            {
                try {
                    (void)test.relay.sessions(test.principal());
                }
                catch (std::logic_error const&) {
                    return true;
                }
                return false;
            });
        CHECK(result.get());
    }
}

TEST_CASE("MCP relay teardown contains clock failures and releases callbacks", "[mcp-actions]")
{
    bool failClock = false;
    auto now = std::chrono::system_clock::now();
    auto retained = std::make_shared<int>(42);
    std::weak_ptr<int> observer = retained;
    {
        McpViewerRelay relay(
            ViewerRelayTest::catalog(),
            {},
            [&]
            {
                if (failClock)
                    throw std::runtime_error("Test clock failure during shutdown");
                return now;
            });
        REQUIRE(relay.attach(
            ViewerRelayTest::first,
            {"https://issuer.example", "alice", now + std::chrono::minutes(5), true, true},
            "https://viewer.example",
            [retained](Json const&) { return *retained == 42; }));
        retained.reset();
        REQUIRE_FALSE(observer.expired());
        failClock = true;
        // The injected failure must not escape the destructor at scope exit.
    }
    CHECK(observer.expired());
}
