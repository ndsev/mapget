#include "../../libs/http-service/src/mcp-auth.h"
#include "mcp-test-issuer.h"

#include <catch2/catch_test_macros.hpp>

using mapget::detail::McpAuthentication;
using mapget::test::McpTestIssuer;
using nlohmann::json;

TEST_CASE(
    "MCP HTTP authentication schemes are case insensitive, tokens are not",
    "[mcp-auth][mcp-actions]")
{
    McpTestIssuer issuer;
    McpAuthentication auth(McpTestIssuer::configuration(), ".");
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
    McpAuthentication auth(McpTestIssuer::configuration(), ".");
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
    McpAuthentication auth(McpTestIssuer::configuration(), ".");
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
    McpAuthentication auth(McpTestIssuer::configuration(), ".");
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
    McpAuthentication auth(config, ".");
    auto info = auth.info("sha256:test");
    CHECK(info["authentication"] == "oauth");
    CHECK(info["scopes"] == json::array({"viewer"}));
    CHECK(info["oauthClientId"] == "public-client");
    CHECK_FALSE(info.contains("oauth"));
    CHECK(auth.metadata()["resource"] == config["endpoint"]);
    CHECK(auth.metadata()["authorization_servers"] == json::array({config["oauth"]["issuer"]}));
    CHECK(
        auth.challenge().find("https://viewer.example/.well-known/oauth-protected-resource/mcp") !=
        std::string::npos);
    CHECK(auth.challenge(true).find("insufficient_scope") != std::string::npos);
    json const invalidSettings{
        {"/authentication", "automatic"},
        {"/allowedHosts", json::array()},
        {"/allowedOrigins", {"*"}},
        {"/endpoint", "http://viewer.example/mcp"},
        {"/oauth/jwksUrl", "http://issuer.example/keys"},
        {"/oauth/clockSkewSeconds", 3600},
        {"/oauth/browser/trustedProxyAddresses", {"*"}},
        {"/oauth/browser/expiryHeader", "test-subject"},
        {"/oauth/browser/maxLifetimeSeconds", 0},
        {"/oauth/requiredScopes", {"viewer\"bad"}},
        {"/oauth/permissions/viewer-read/claim", "not-a-pointer"}};
    for (auto const& [pointer, value] : invalidSettings.items()) {
        INFO(pointer);
        auto invalid = config;
        invalid[json::json_pointer(pointer)] = value;
        CHECK_THROWS(McpAuthentication(invalid, "."));
    }
    auto invalid = config;
    invalid["trustedAll"] = true;
    CHECK_THROWS(McpAuthentication(invalid, "."));
    invalid = config;
    invalid["oauth"]["jwksFile"] = "keys.json";
    CHECK_THROWS(McpAuthentication(invalid, "."));
    json local{
        {"authentication", "local"},
        {"endpoint", "http://127.0.0.1:8099/mcp"},
        {"catalogPath", "viewer-actions.json"},
        {"allowedHosts", {"127.0.0.1:8099"}},
        {"allowedOrigins", {"http://127.0.0.1:8099"}}};
    McpAuthentication localAuth(local, ".");
    CHECK(localAuth.localPrincipal().valid(std::chrono::system_clock::now()));
    CHECK(localAuth.info("test")["scopes"].empty());
    local["allowedHosts"].push_back("remote.example");
    CHECK_THROWS(McpAuthentication(local, "."));
    CHECK_THROWS(McpAuthentication::parseJson(std::string(100, ' '), 10));
    CHECK_THROWS(McpAuthentication::parseJson("[[[[0]]]]", 100, 2));
}
