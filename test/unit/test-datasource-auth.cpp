#include "mapget/service/authorization.h"
#include "mapget/service/config.h"
#include "mapget/service/service.h"
#include "mapget/http-service/proxy-auth.h"
#include "../../libs/http-service/src/bounded-json.h"
#include "../../libs/http-service/src/mcp-auth.h"
#include "mcp-test-issuer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>

using namespace mapget;
using nlohmann::json;

namespace
{

/** Count backend access so denied requests cannot hide a cache/admission bypass. */
class ClaimsSource final : public DataSource
{
public:
    explicit ClaimsSource(bool addOn = false)
    {
        auto metadata = json::parse(R"({
            "mapId":"ClaimsMap", "stringPoolId":"claims-base",
            "layers":{"Road":{"type":"Features","featureTypes":[
                {"name":"Road","uniqueIdCompositions":[[{"partId":"id","datatype":"U32"}]]}
            ],"featureModelSchema":{"type":"object","properties":{"typeId":{"type":"string"}}}}}
        })");
        metadata["addOn"] = addOn;
        if (addOn)
            metadata["stringPoolId"] = "claims-addon";
        info_ = DataSourceInfo::fromJson(metadata);
    }

    DataSourceInfo info() override { return info_; }
    void fill(PartitionFeatureLayer::Ptr const& tile) override
    {
        ++fills;
        tile->newFeature("Road", {{"id", int64_t{1}}});
    }
    void fill(PartitionSourceDataLayer::Ptr const&) override {}
    std::vector<LocateCandidate> locate(LocateRequest const&) override
    {
        ++locates;
        return {LocateCandidate(
            MapPartitionKey(LayerType::Features, "ClaimsMap", "Road", TileId::fromTileXY(0, 0, 0)),
            "Road.1")};
    }

    std::atomic_uint fills{0};
    std::atomic_uint locates{0};

private:
    DataSourceInfo info_;
};

}  // namespace

TEST_CASE("Datasource claims require exact verified membership and AND every rule", "[datasource-auth]")
{
    AuthClaimRequirements required{{json::json_pointer("/groups"), "SMS_users"},
                                 {json::json_pointer("/tenant/name"), "nds"}};
    ClientAuthorization anonymous{{{"x-claims", R"({"groups":["SMS_users"]})"}}};
    CHECK(anonymous.matches({}));
    CHECK_FALSE(anonymous.matches(required));

    auto groups = GENERATE(json("SMS_users"), json::array({"other", "SMS_users"}));
    ClientAuthorization identity{{}, std::make_shared<json const>(
        json{{"groups", groups}, {"tenant", {{"name", "nds"}}}})};
    CHECK(identity.matches(required));
    CHECK(identity == ClientAuthorization({}, std::make_shared<json const>(*identity.claims)));
    CHECK_FALSE(identity == anonymous);

    for (auto const& invalid : json::array({nullptr, false, 7, "sms_users", "/SMS_users",
                                         "SMS_users_extra", "other,SMS_users", json::object(),
                                         json::array({"other", "/SMS_users"})})) {
        auto claims = *identity.claims;
        claims["groups"] = invalid;
        ClientAuthorization denied{{}, std::make_shared<json const>(std::move(claims))};
        CHECK_FALSE(denied.matches(required));
    }
    auto claims = *identity.claims;
    claims.erase("tenant");
    CHECK_FALSE(ClientAuthorization({}, std::make_shared<json const>(claims)).matches(required));
    claims["tenant"] = {{"name", "other"}};
    CHECK_FALSE(ClientAuthorization({}, std::make_shared<json const>(claims)).matches(required));

    AuthClaimRequirements escaped{{json::json_pointer("/a~1b/~0name"), "allowed"}};
    CHECK(ClientAuthorization({}, std::make_shared<json const>(json{{"a/b", {{"~name", "allowed"}}}})).matches(escaped));
    ClientAuthorization arrayIdentity{{}, std::make_shared<json const>(json{{"nested", json::array({"allowed"})}})};
    CHECK(arrayIdentity.hasClaim(json::json_pointer("/nested/0"), "allowed"));
    for (auto const* pointer : {"/nested/", "/nested/name", "/nested/01",
                               "/nested/9999999999999999999999999999999999999999"})
        CHECK_FALSE(arrayIdentity.hasClaim(json::json_pointer(pointer), "allowed"));
}

TEST_CASE("Proxy claim trust is explicit and bounded", "[datasource-auth]")
{
    ProxyAuthentication disabled;
    CHECK_NOTHROW(disabled.validate());
    ProxyAuthentication settings{{"127.0.0.1", "::1"}, "x-verified-claims"};
    CHECK_NOTHROW(settings.validate());
    settings.claimsHeader.clear();
    CHECK_THROWS(settings.validate());
    settings = {{}, "x-verified-claims"};
    CHECK_THROWS(settings.validate());
    for (auto const& address : {"", "localhost", "127.0.0.0/8", "0.0.0.0", "::", "999.1.2.3"}) {
        settings = {{address}, "x-verified-claims"};
        CHECK_THROWS(settings.validate());
    }
    for (auto const& header : {"X-Claims", "claims\r\ninjected", "authorization", "cookie", "host"}) {
        settings = {{"127.0.0.1"}, header};
        CHECK_THROWS(settings.validate());
    }
    auto request = drogon::HttpRequest::newHttpRequest();
    request->addHeader("x-verified-claims", R"({"groups":["SMS_users"]})");
    request->addHeader("x-forwarded-for", "127.0.0.1");
    CHECK_FALSE(disabled.authorization(request).claims);
    settings = {{"127.0.0.1"}, "x-verified-claims"};
    CHECK_FALSE(settings.authorization(request).claims);

    CHECK_THROWS(detail::parseBoundedJson(R"({"groups":[],"groups":["SMS_users"]})", 1024, 16));
    CHECK_THROWS(detail::parseBoundedJson("[[[[0]]]]", 1024, 2));
    CHECK_THROWS(detail::parseBoundedJson(std::string(1025, ' '), 1024, 16));
}

TEST_CASE("MCP retains verified claims without granting datasource membership or tool roles", "[datasource-auth][mcp-auth]")
{
    test::McpTestIssuer issuer;
    detail::McpAuthentication auth(test::McpTestIssuer::configuration());
    auth.installKeys(issuer.jwks);
    bool insufficient = false;
    auto claims = test::McpTestIssuer::claims();
    claims["groups"] = {"SMS_users"};
    auto principal = auth.bearer(issuer.token(claims), insufficient);
    REQUIRE(principal.read);
    CHECK(principal.datasourceAuthorization.hasClaim(json::json_pointer("/groups"), "SMS_users"));
    claims.erase("groups");
    principal = auth.bearer(issuer.token(claims), insufficient);
    REQUIRE(principal.read);
    CHECK_FALSE(principal.datasourceAuthorization.hasClaim(json::json_pointer("/groups"), "SMS_users"));
    claims["groups"] = {"SMS_users"};
    claims["access"]["roles"] = json::array();
    principal = auth.bearer(issuer.token(claims), insufficient);
    CHECK_FALSE(principal.valid(std::chrono::system_clock::now()));
    CHECK_FALSE(principal.datasourceAuthorization.claims);
    claims = test::McpTestIssuer::claims();
    claims["groups"] = {"SMS_users"};
    claims["aud"] = "https://wrong.example/mcp";
    CHECK_FALSE(auth.bearer(issuer.token(claims), insufficient).datasourceAuthorization.claims);
}

TEST_CASE("Datasource headers and claims are cumulative across catalog cache and locate", "[datasource-auth]")
{
    Service service;
    auto source = std::make_shared<ClaimsSource>();
    source->requireAuthHeaderRegexMatchOption("x-supplier", std::regex("^(here|mcp)$"));
    source->requireAuthClaim("/groups", "SMS_users");
    service.add(source);
    ClientAuthorization allowed{{{"x-supplier", "mcp"}},
        std::make_shared<json const>(json{{"groups", {"SMS_users"}}})};
    ClientAuthorization headerOnly{{{"x-supplier", "mcp"}}};
    ClientAuthorization claimsOnly{{}, allowed.claims};
    CHECK(source->isDataSourceAuthorized(allowed));
    CHECK_FALSE(source->isDataSourceAuthorized(headerOnly));
    CHECK_FALSE(source->isDataSourceAuthorized(claimsOnly));

    auto catalog = service.sourceCatalog(allowed);
    REQUIRE(catalog.sources.size() == 1);
    auto const sourceId = catalog.sources.front().descriptor.sourceId;
    auto const schemaId = source->info().schemaId_;
    CHECK(service.info(allowed).size() == 1);
    CHECK(service.getSchema(sourceId, schemaId, allowed));
    CHECK(service.schemaInventory(allowed).size() == 1);
    for (auto const& denied : {headerOnly, claimsOnly, ClientAuthorization{}}) {
        CHECK(service.info(denied).empty());
        CHECK(service.sourceCatalog(denied).sources.empty());
        CHECK_FALSE(service.getSchema(sourceId, schemaId, denied));
        CHECK(service.schemaInventory(denied).empty());
    }

    auto load = [&](ClientAuthorization const& identity) {
        auto request = std::make_shared<LayerTilesRequest>("ClaimsMap", "Road",
            std::vector<TileId>{TileId::fromTileXY(0, 0, 0)});
        auto accepted = service.request({request}, identity);
        request->wait();
        return std::pair{accepted, request->getStatus()};
    };
    REQUIRE(load(allowed).first);
    CHECK(source->fills == 1);
    CHECK_FALSE(load(headerOnly).first);
    CHECK(source->fills == 1);
    REQUIRE(load(allowed).first);
    CHECK(source->fills == 1);

    LocateRequest locate("ClaimsMap", "Road", {{"id", int64_t{1}}});
    CHECK(service.locate(locate, headerOnly).empty());
    CHECK(source->locates == 0);
    CHECK(service.locate(locate, allowed).size() == 1);
    CHECK(source->locates == 1);
    CHECK(source->fills == 1);

    auto addon = std::make_shared<ClaimsSource>(true);
    addon->requireAuthClaim("/groups", "addon-users");
    service.add(addon);
    CHECK(service.info(allowed).empty());
    CHECK_FALSE(service.getSchema(sourceId, schemaId, allowed));
    CHECK_FALSE(load(allowed).first);
}

TEST_CASE("Configured claim requirements exist before datasource initialization", "[datasource-auth]")
{
    auto& config = DataSourceConfigService::get();
    auto node = YAML::Load(R"(
type: UnknownSource
auth-claims:
  /groups: SMS_users
  /tenant/name: nds
)");
    auto descriptor = config.describeDataSource(node, 0);
    CHECK(descriptor.requiredClaims.size() == 2);
    CHECK_FALSE(ClientAuthorization{}.matches(descriptor.requiredClaims));
    ClientAuthorization allowed{{}, std::make_shared<json const>(
        json{{"groups", {"SMS_users"}}, {"tenant", {{"name", "nds"}}}})};
    CHECK(allowed.matches(descriptor.requiredClaims));
    for (auto const* invalid : {"auth-claims: []", "auth-claims: {/groups~invalid: SMS_users}",
                               "auth-claims: {groups: SMS_users}",
                               "auth-claims: {/groups: SMS_users, /groups: other}",
                               "auth-claims: {/groups: [SMS_users]}"}) {
        CHECK_THROWS(config.describeDataSource(YAML::Load(invalid), 0));
    }
}
