#pragma once

#include "mapget/http-service/mcp-config.h"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <openssl/core_names.h>
#include <openssl/pem.h>

#include <chrono>
#include <memory>
#include <stdexcept>

namespace mapget::test
{

/** Disposable RSA authority for real signature/claim tests; never writes a production secret. */
class McpTestIssuer
{
public:
    /** Generate one independent test key so cross-key signature substitution can be exercised. */
    McpTestIssuer()
    {
        auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
            EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048),
            EVP_PKEY_free);
        auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), BIO_free);
        if (!key || !bio ||
            PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
                1)
        {
            throw std::runtime_error("Cannot generate ephemeral MCP test issuer.");
        }
        char* data = nullptr;
        auto length = BIO_get_mem_data(bio.get(), &data);
        privateKey_.assign(data, static_cast<size_t>(length));
        auto component = [&](char const* name)
        {
            BIGNUM* raw = nullptr;
            if (EVP_PKEY_get_bn_param(key.get(), name, &raw) != 1)
                throw std::runtime_error("Cannot read test RSA key.");
            auto number = std::unique_ptr<BIGNUM, decltype(&BN_free)>(raw, BN_free);
            std::string bytes(static_cast<size_t>(BN_num_bytes(number.get())), '\0');
            BN_bn2bin(number.get(), reinterpret_cast<unsigned char*>(bytes.data()));
            auto encoded = jwt::base::encode<jwt::alphabet::base64url>(bytes);
            encoded.erase(std::remove(encoded.begin(), encoded.end(), '='), encoded.end());
            return encoded;
        };
        jwks = {
            {"keys",
             {{{"kty", "RSA"},
               {"use", "sig"},
               {"alg", "RS256"},
               {"kid", "test-key"},
               {"n", component(OSSL_PKEY_PARAM_RSA_N)},
               {"e", component(OSSL_PKEY_PARAM_RSA_E)}}}}};
    }

    /** Return explicit provider-neutral settings with a network key URL replaced by tests when
     * needed. */
    [[nodiscard]] static McpConfig configuration()
    {
        McpConfig config;
        config.mode = McpConfig::Mode::OAuth;
        config.endpoint = "https://viewer.example/mcp";
        config.catalogPath = "web-mcp-actions.json";
        config.allowedHosts = {"viewer.example"};
        config.allowedOrigins = {"https://viewer.example", "https://supplier.example"};
        config.issuer = "https://issuer.example/realm";
        config.jwksUrl = "https://issuer.example/keys";
        config.requiredScopes = {"viewer"};
        config.oauthClientId = "public-client";
        config.clockSkewSeconds = 0;
        config.readClaim = config.controlClaim = "/access/roles";
        config.readValue = "read";
        config.controlValue = "control";
        config.trustedProxyAddresses = {"127.0.0.1"};
        config.issuerHeader = "test-issuer";
        config.subjectHeader = "test-subject";
        config.expiryHeader = "test-expiry";
        config.permissionsHeader = "test-permissions";
        return config;
    }

    /** Start with valid signed-user claims; callers change one condition per rejection test. */
    [[nodiscard]] static nlohmann::json claims()
    {
        return {
            {"iss", "https://issuer.example/realm"},
            {"sub", "user-1"},
            {"aud", "https://viewer.example/mcp"},
            {"exp",
             std::chrono::duration_cast<std::chrono::seconds>(
                 (std::chrono::system_clock::now() + std::chrono::minutes(10)).time_since_epoch())
                 .count()},
            {"scope", "openid viewer"},
            {"access", {{"roles", {"read", "control"}}}}};
    }

    /** Sign arbitrary bounded claims/headers with the test authority's private key. */
    [[nodiscard]] std::string token(
        nlohmann::json const& claims = McpTestIssuer::claims(),
        nlohmann::json headers = {{"kid", "test-key"}}) const
    {
        auto builder = jwt::create().set_type("JWT");
        for (auto const& [name, value] : claims.items())
            builder.set_payload_claim(name, jwt::claim(value));
        for (auto const& [name, value] : headers.items())
            builder.set_header_claim(name, jwt::claim(value));
        return builder.sign(jwt::algorithm::rs256("", privateKey_));
    }

    nlohmann::json jwks;

private:
    std::string privateKey_;
};

}  // namespace mapget::test
