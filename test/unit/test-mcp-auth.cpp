#include "../../libs/http-service/src/mcp-auth.h"
#include "mcp-test-issuer.h"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

using mapget::detail::McpAuthentication;
using mapget::test::McpTestIssuer;
using nlohmann::json;

TEST_CASE(
    "MCP native privileges and datasource identity come only from configured verified claims",
    "[mcp-auth][mcp-native]")
{
    McpTestIssuer issuer;
    auto settings = McpTestIssuer::configuration();
    settings.configReadClaim = settings.configWriteClaim = settings.diagnosticsClaim = "/admin";
    settings.configReadValue = "config-read";
    settings.configWriteValue = "config-write";
    settings.diagnosticsValue = "diagnostics";
    settings.datasourceHeaderClaims = {"x-user=/email"};
    McpAuthentication auth(settings);
    auth.installKeys(issuer.jwks);
    bool insufficient = false;
    auto claims = McpTestIssuer::claims();
    claims["email"] = "member@example.test";
    auto regular = auth.bearer(issuer.token(claims), insufficient);
    REQUIRE(regular.valid(std::chrono::system_clock::now()));
    CHECK_FALSE(regular.configRead);
    CHECK_FALSE(regular.configWrite);
    CHECK_FALSE(regular.diagnostics);
    CHECK(regular.datasourceHeaders.at("x-user") == "member@example.test");
    claims["admin"] = {"config-read", "diagnostics"};
    auto admin = auth.bearer(issuer.token(claims), insufficient);
    CHECK(admin.configRead);
    CHECK(admin.diagnostics);
    CHECK_FALSE(admin.configWrite);
    claims["email"] = "spoof\r\nAuthorization: attacker";
    CHECK_FALSE(
        auth.bearer(issuer.token(claims), insufficient).valid(std::chrono::system_clock::now()));
    settings.datasourceHeaderClaims = {"X-User=/email"};
    CHECK_THROWS(settings.validate());
    settings.datasourceHeaderClaims = {"x-user=/email", "x-user=/sub"};
    CHECK_THROWS(settings.validate());
    settings.datasourceHeaderClaims.clear();
    settings.configWriteEnabled = true;
    CHECK_THROWS(settings.validate());
    settings.directConfigPersistence = true;
    CHECK_NOTHROW(settings.validate());

    // Config/diagnostics authority must work without implicitly granting data or viewer access.
    settings.readClaim.clear();
    settings.readValue.clear();
    settings.controlClaim.clear();
    settings.controlValue.clear();
    McpAuthentication adminOnly(settings);
    adminOnly.installKeys(issuer.jwks);
    claims.erase("email");
    auto restricted = adminOnly.bearer(issuer.token(claims), insufficient);
    REQUIRE(restricted.valid(std::chrono::system_clock::now()));
    CHECK(restricted.configRead);
    CHECK(restricted.diagnostics);
    CHECK_FALSE(restricted.read);
    CHECK_FALSE(restricted.control);
}

TEST_CASE("MCP local defaults use trusted startup inputs", "[mcp-auth][mcp-actions]")
{
    using mapget::McpConfig;
    McpConfig disabled;
    disabled.resolveDefaults("0.0.0.0", 0, {});
    CHECK_NOTHROW(disabled.validate());
    CHECK(disabled.endpoint.empty());

    McpConfig local;
    local.mode = McpConfig::Mode::Local;
    CHECK_THROWS(local.resolveDefaults("0.0.0.0", 8099, "/viewer"));
    CHECK_THROWS(local.resolveDefaults("127.0.0.1", 0, "/viewer"));
    local.resolveDefaults("127.0.0.1", 8099, "/viewer");
    CHECK_NOTHROW(local.validate());
    CHECK(local.catalogPath == std::filesystem::path("/viewer/web-mcp-actions.json"));
    CHECK(local.endpoint == "http://127.0.0.1:8099/mcp");
    CHECK(
        local.allowedHosts ==
        std::vector<std::string>{"localhost:8099", "127.0.0.1:8099", "[::1]:8099"});
    local.issuer = "https://issuer.example";
    CHECK_THROWS(local.validate());

    McpConfig ipv6;
    ipv6.mode = McpConfig::Mode::Local;
    ipv6.catalogPath = "custom.json";
    ipv6.allowedHosts = {"[::1]"};
    ipv6.allowedOrigins = {"http://[::1]"};
    ipv6.resolveDefaults("::1", 80, "/viewer");
    CHECK_NOTHROW(ipv6.validate());
    CHECK(ipv6.catalogPath == "custom.json");
    CHECK(ipv6.endpoint == "http://[::1]/mcp");
    CHECK(ipv6.allowedHosts == std::vector<std::string>{"[::1]"});

    auto hosted = mapget::test::McpTestIssuer::configuration();
    hosted.catalogPath.clear();
    hosted.resolveDefaults("0.0.0.0", 8089, "/viewer");
    CHECK_NOTHROW(hosted.validate());
    CHECK(hosted.endpoint == "https://viewer.example/mcp");
    CHECK(hosted.allowedHosts == std::vector<std::string>{"viewer.example"});
}

TEST_CASE("MCP CLI settings are individual typed options", "[mcp-auth][mcp-actions]")
{
    mapget::McpConfig config;
    CLI::App app;
    config.addOptions(app);
    app.parse(
        "--mcp local --mcp-timeout-ms 42 --mcp-sessions 3 --mcp-catalog web-mcp-actions.json "
        "--mcp-allowed-hosts localhost:8099 127.0.0.1:8099 --mcp-help-docs docs extra.md");
    CHECK(config.mode == mapget::McpConfig::Mode::Local);
    CHECK(config.limits.timeout == std::chrono::milliseconds(42));
    CHECK(config.limits.sessions == 3);
    CHECK(config.allowedHosts.size() == 2);
    CHECK(config.catalogPath == "web-mcp-actions.json");
    CHECK(config.helpDocs == std::vector<std::filesystem::path>{"docs", "extra.md"});
    CHECK(app.get_option("--mcp")->results() == std::vector<std::string>{"local"});
    CHECK_THROWS_AS(app.parse("--mcp automatic"), CLI::ParseError);
    CHECK_THROWS_AS(app.parse("--mcp 1"), CLI::ParseError);
    CHECK_THROWS_AS(app.parse("--mcp-config old.json"), CLI::ParseError);
    CHECK_THROWS_AS(app.parse("--mcp-sessions not-an-integer"), CLI::ParseError);
}

TEST_CASE(
    "MCP HTTP authentication schemes are case insensitive, tokens are not",
    "[mcp-auth][mcp-actions]")
{
    McpTestIssuer issuer;
    McpAuthentication auth(McpTestIssuer::configuration());
    auth.installKeys(issuer.jwks);
    auto const token = issuer.token();
    for (auto scheme : {"Bearer ", "bearer ", "BEARER ", "bEaReR   "}) {
        auto extracted = McpAuthentication::bearerToken(std::string(scheme) + token);
        REQUIRE(extracted == token);
        bool insufficient = false;
        CHECK(auth.bearer(extracted, insufficient).valid(std::chrono::system_clock::now()));
    }
    for (auto header : {"", "Bearer", "Bearer   ", "Basic token", "BearerX token", "Bearer\ttoken"})
    {
        CHECK(McpAuthentication::bearerToken(header).empty());
    }
}

TEST_CASE(
    "MCP OAuth validates real signatures and independent identity claims",
    "[mcp-auth][mcp-actions]")
{
    McpTestIssuer issuer;
    McpAuthentication auth(McpTestIssuer::configuration());
    auth.installKeys(issuer.jwks);
    bool insufficient = false;
    auto valid = auth.bearer(issuer.token(), insufficient);
    REQUIRE_FALSE(insufficient);
    REQUIRE(valid.valid(std::chrono::system_clock::now()));
    CHECK(valid.issuer == "https://issuer.example/realm");
    CHECK(valid.subject == "user-1");
    CHECK(valid.read);
    CHECK(valid.control);
    CHECK(auth.localPrincipal().issuer.empty());

    json const invalidClaims{
        {"iss", "https://other.example/realm"},
        {"sub", ""},
        {"aud", "https://other.example/mcp"},
        {"exp", 1},
        {"nbf", 4102444800LL},
        {"iat", 4102444800LL}};
    for (auto const& [field, value] : invalidClaims.items()) {
        INFO(field);
        auto claims = McpTestIssuer::claims();
        claims[field] = value;
        CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient)
                        .valid(std::chrono::system_clock::now()));
        CHECK_FALSE(insufficient);
    }
    for (auto const* field : {"iss", "sub", "aud", "exp"}) {
        auto claims = McpTestIssuer::claims();
        claims.erase(field);
        CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient)
                        .valid(std::chrono::system_clock::now()));
    }
    auto claims = McpTestIssuer::claims();
    claims["aud"] = {"other", "https://viewer.example/mcp"};
    CHECK(auth.bearer(issuer.token(claims), insufficient).valid(std::chrono::system_clock::now()));
    claims["sub"] = "other-user";
    CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient).sameUser(valid));

    McpTestIssuer otherIssuer;
    CHECK_FALSE(
        auth.bearer(otherIssuer.token(), insufficient).valid(std::chrono::system_clock::now()));
    auto corrupt = issuer.token();
    corrupt[corrupt.rfind('.') + 1] = corrupt[corrupt.rfind('.') + 1] == 'a' ? 'b' : 'a';
    CHECK_FALSE(auth.bearer(corrupt, insufficient).valid(std::chrono::system_clock::now()));
    CHECK_FALSE(
        auth.bearer(
                jwt::create()
                    .set_key_id("test-key")
                    .sign(jwt::algorithm::hs256("not-a-public-key")),
                insufficient)
            .valid(std::chrono::system_clock::now()));
}

TEST_CASE("MCP scopes and JSON pointer roles never imply one another", "[mcp-auth][mcp-actions]")
{
    McpTestIssuer issuer;
    McpAuthentication auth(McpTestIssuer::configuration());
    auth.installKeys(issuer.jwks);
    bool insufficient = false;
    auto claims = McpTestIssuer::claims();
    for (auto scope : {"", "viewer-extra", "other"}) {
        claims["scope"] = scope;
        CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient)
                        .valid(std::chrono::system_clock::now()));
        CHECK(insufficient);
    }
    claims = McpTestIssuer::claims();
    for (auto roles : {json::array(), json({{"read", true}}), json(1), json("read-extra")}) {
        claims["access"]["roles"] = roles;
        CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient)
                        .valid(std::chrono::system_clock::now()));
        CHECK(insufficient);
    }
    claims["access"]["roles"] = "read";
    auto reader = auth.bearer(issuer.token(claims), insufficient);
    CHECK_FALSE(insufficient);
    CHECK(reader.read);
    CHECK_FALSE(reader.control);
    claims["access"]["roles"] = {"control"};
    auto controller = auth.bearer(issuer.token(claims), insufficient);
    CHECK_FALSE(controller.read);
    CHECK(controller.control);
}

TEST_CASE(
    "MCP key updates are atomic and token headers cannot select trust",
    "[mcp-auth][mcp-actions]")
{
    McpTestIssuer issuer;
    McpAuthentication auth(McpTestIssuer::configuration());
    bool insufficient = false;
    CHECK(auth.needsKeys(issuer.token()));
    CHECK_FALSE(auth.needsKeys("garbage"));
    CHECK_FALSE(auth.bearer(issuer.token(), insufficient).valid(std::chrono::system_clock::now()));
    auth.installKeys(issuer.jwks);
    CHECK_FALSE(auth.needsKeys(issuer.token()));
    auto duplicate = issuer.jwks;
    duplicate["keys"].push_back(duplicate["keys"][0]);
    CHECK_THROWS(auth.installKeys(duplicate));
    CHECK(auth.bearer(issuer.token(), insufficient).valid(std::chrono::system_clock::now()));
    json const untrustedHeaders{
        {"jku", "https://attacker.example/key"},
        {"x5u", "https://attacker.example/key"},
        {"crit", {"x"}},
        {"b64", false}};
    for (auto const& [name, value] : untrustedHeaders.items()) {
        CHECK_FALSE(
            auth.bearer(
                    issuer.token(McpTestIssuer::claims(), {{"kid", "test-key"}, {name, value}}),
                    insufficient)
                .valid(std::chrono::system_clock::now()));
    }
    CHECK(auth.needsKeys(issuer.token(McpTestIssuer::claims(), {{"kid", "rotated"}})));
    McpTestIssuer rotated;
    auth.installKeys(rotated.jwks);
    CHECK_FALSE(auth.bearer(issuer.token(), insufficient).valid(std::chrono::system_clock::now()));
    CHECK(auth.bearer(rotated.token(), insufficient).valid(std::chrono::system_clock::now()));
}

TEST_CASE("MCP security configuration and public hints fail closed", "[mcp-auth][mcp-actions]")
{
    auto config = McpTestIssuer::configuration();
    McpAuthentication auth(config);
    auto info = auth.info("sha256:test");
    CHECK(info["authentication"] == "oauth");
    CHECK(info["scopes"] == json::array({"viewer"}));
    CHECK(info["oauthClientId"] == "public-client");
    CHECK_FALSE(info.contains("oauth"));
    CHECK(auth.metadata()["resource"] == config.endpoint);
    CHECK(auth.metadata()["authorization_servers"] == json::array({config.issuer}));
    CHECK(
        auth.challenge().find("https://viewer.example/.well-known/oauth-protected-resource/mcp") !=
        std::string::npos);
    CHECK(auth.challenge(true).find("insufficient_scope") != std::string::npos);
    using mapget::McpConfig;
    for (auto const& mutate :
         std::vector<std::function<void(McpConfig&)>>{
             [](auto& c) { c.mode = static_cast<McpConfig::Mode>(100); },
             [](auto& c) { c.allowedHosts.clear(); },
             [](auto& c) { c.allowedOrigins = {"*"}; },
             [](auto& c) { c.endpoint = "http://viewer.example/mcp"; },
             [](auto& c) { c.jwksUrl = "http://issuer.example/keys"; },
             [](auto& c) { c.clockSkewSeconds = 3600; },
             [](auto& c) { c.trustedProxyAddresses = {"*"}; },
             [](auto& c) { c.expiryHeader = c.subjectHeader; },
             [](auto& c) { c.maxBrowserLifetimeSeconds = 0; },
             [](auto& c) { c.requiredScopes = {"viewer\"bad"}; },
             [](auto& c) { c.readClaim = "not-a-pointer"; },
             [](auto& c) { c.jwksFile = "keys.json"; },
             [](auto& c) { c.readValue.clear(); },
             [](auto& c)
             {
                 c.readClaim.clear();
                 c.controlClaim.clear();
             },
             [](auto& c) { c.limits.callsPerSession = 0; },
             [](auto& c)
             {
                 c.limits.timeout = std::chrono::milliseconds(-1);
             }})
    {
        auto invalid = config;
        mutate(invalid);
        CHECK_THROWS(McpAuthentication(invalid));
    }
    McpConfig local;
    local.mode = McpConfig::Mode::Local;
    local.resolveDefaults("127.0.0.1", 8099, "/viewer");
    McpAuthentication localAuth(local);
    CHECK(localAuth.localPrincipal().valid(std::chrono::system_clock::now()));
    CHECK(localAuth.info("test")["scopes"].empty());
    local.allowedHosts.push_back("remote.example");
    CHECK_THROWS(McpAuthentication(local));
    CHECK_THROWS(McpAuthentication::parseJson(std::string(100, ' '), 10));
    CHECK_THROWS(McpAuthentication::parseJson("[[[[0]]]]", 100, 2));
}
