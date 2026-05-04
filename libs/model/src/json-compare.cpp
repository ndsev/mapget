#include "mapget/model/json-compare.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace mapget
{
namespace
{

/** Recursively compare JSON values while collecting path-qualified errors. */
bool compareJsonWithToleranceImpl(
    nlohmann::json const& expected,
    nlohmann::json const& actual,
    std::string const& path,
    double floatTolerance,
    std::vector<std::string>* errors)
{
    auto const currentPath = path.empty() ? "$" : path;

    if ((expected.type() != actual.type()) && !(expected.is_number() && actual.is_number())) {
        if (errors) {
            errors->push_back(
                "Type mismatch at " + currentPath + ": " + expected.type_name() + " vs " +
                actual.type_name());
        }
        return false;
    }

    if (expected.is_number() && actual.is_number()) {
        if (!expected.is_number_float() && !actual.is_number_float()) {
            // Keep integer-only comparisons exact so id-like fields do not silently drift.
            if (expected != actual) {
                if (errors) {
                    errors->push_back(
                        "Value mismatch at " + currentPath + ": " + expected.dump() + " vs " +
                        actual.dump());
                }
                return false;
            }
            return true;
        }

        // Float-vs-float and int-vs-float comparisons are normalized through double tolerance.
        if (!nearlyEqual(expected.get<double>(), actual.get<double>(), floatTolerance)) {
            if (errors) {
                std::ostringstream message;
                message << "Float mismatch at " << currentPath << ": " << expected.get<double>()
                        << " vs " << actual.get<double>();
                errors->push_back(message.str());
            }
            return false;
        }
        return true;
    }

    switch (expected.type()) {
    case nlohmann::json::value_t::object: {
        auto matches = true;
        if (expected.size() != actual.size()) {
            if (errors) {
                errors->push_back(
                    "Object size mismatch at " + currentPath + ": " +
                    std::to_string(expected.size()) + " vs " + std::to_string(actual.size()));
            }
            matches = false;
        }

        for (auto const& item : expected.items()) {
            auto const childPath = currentPath + "." + item.key();
            auto actualIt = actual.find(item.key());
            if (actualIt == actual.end()) {
                if (errors) {
                    errors->push_back("Missing key at " + childPath);
                }
                matches = false;
                continue;
            }
            matches = compareJsonWithToleranceImpl(
                          item.value(),
                          *actualIt,
                          childPath,
                          floatTolerance,
                          errors) &&
                      matches;
        }
        return matches;
    }
    case nlohmann::json::value_t::array: {
        if (expected.size() != actual.size()) {
            if (errors) {
                errors->push_back(
                    "Array size mismatch at " + currentPath + ": " +
                    std::to_string(expected.size()) + " vs " + std::to_string(actual.size()));
            }
            return false;
        }

        auto matches = true;
        for (size_t i = 0; i < expected.size(); ++i) {
            matches = compareJsonWithToleranceImpl(
                          expected[i],
                          actual[i],
                          currentPath + "[" + std::to_string(i) + "]",
                          floatTolerance,
                          errors) &&
                      matches;
        }
        return matches;
    }
    default:
        if (expected != actual) {
            if (errors) {
                errors->push_back(
                    "Value mismatch at " + currentPath + ": " + expected.dump() + " vs " +
                    actual.dump());
            }
            return false;
        }
        return true;
    }
}

}  // namespace

/** Compare floats robustly across quantization and serialization roundtrips. */
bool nearlyEqual(double a, double b, double epsilon)
{
    auto const absA = std::abs(a);
    auto const absB = std::abs(b);
    auto const diff = std::abs(a - b);

    if (a == b) {
        return true;
    }
    if (a == 0.0 || b == 0.0 || diff < std::numeric_limits<double>::min()) {
        // Near zero, pure relative error becomes unstable, so fall back to absolute error.
        return diff < epsilon;
    }

    auto const relDiff = diff / std::min((absA + absB), std::numeric_limits<double>::max());
    return relDiff < epsilon;
}

/** Public entry point for tolerant full-document comparison. */
bool compareJsonWithTolerance(
    nlohmann::json const& expected,
    nlohmann::json const& actual,
    double floatTolerance,
    std::vector<std::string>* errors)
{
    return compareJsonWithToleranceImpl(expected, actual, "", floatTolerance, errors);
}

/** Public entry point that compares only per-feature payloads. */
bool compareFeatureCollectionJsonWithTolerance(
    nlohmann::json const& expected,
    nlohmann::json const& actual,
    double floatTolerance,
    std::vector<std::string>* errors)
{
    if (!expected.contains("features") || !actual.contains("features")) {
        if (errors) {
            errors->push_back("Both JSON values must contain a top-level 'features' array.");
        }
        return false;
    }
    if (!expected["features"].is_array() || !actual["features"].is_array()) {
        if (errors) {
            errors->push_back("Top-level 'features' must be arrays.");
        }
        return false;
    }
    if (expected["features"].size() != actual["features"].size()) {
        if (errors) {
            errors->push_back(
                "Feature array size mismatch: " + std::to_string(expected["features"].size()) +
                " vs " + std::to_string(actual["features"].size()));
        }
        return false;
    }

    auto matches = true;
    for (size_t i = 0; i < expected["features"].size(); ++i) {
        auto const& expectedFeature = expected["features"][i];
        auto const& actualFeature = actual["features"][i];
        auto const featureId =
            expectedFeature.contains("id") && expectedFeature["id"].is_string()
                ? expectedFeature["id"].get<std::string>()
                : std::string("<feature-without-id>");

        std::vector<std::string> featureErrors;
        auto const featureMatches = compareJsonWithToleranceImpl(
            expectedFeature,
            actualFeature,
            "$.features[" + std::to_string(i) + "]",
            floatTolerance,
            &featureErrors);
        if (!featureMatches) {
            matches = false;
            if (errors) {
                // Prefix child errors with feature identity so large snapshots stay debuggable.
                for (auto const& error : featureErrors) {
                    errors->push_back(
                        "Feature " + featureId + " at index " + std::to_string(i) + ": " +
                        error);
                }
            }
        }
    }

    return matches;
}

/** Render collected comparison errors into test-friendly multiline output. */
std::string formatJsonComparisonErrors(std::vector<std::string> const& errors)
{
    std::ostringstream output;
    for (auto const& error : errors) {
        output << error << '\n';
    }
    return output.str();
}

}
