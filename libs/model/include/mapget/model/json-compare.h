#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mapget
{

/** Compare two floating-point values using mixed absolute/relative tolerance. */
bool nearlyEqual(double a, double b, double epsilon);

/**
 * Compare arbitrary JSON values while tolerating small floating-point drift.
 *
 * When `errors` is provided, mismatches are appended as human-readable paths.
 */
bool compareJsonWithTolerance(
    nlohmann::json const& expected,
    nlohmann::json const& actual,
    double floatTolerance = 1e-5,
    std::vector<std::string>* errors = nullptr);

/**
 * Compare only the top-level `features` arrays of two feature collections.
 *
 * This is useful for snapshot tests where envelope metadata is intentionally
 * ignored but per-feature structure must stay stable.
 */
bool compareFeatureCollectionJsonWithTolerance(
    nlohmann::json const& expected,
    nlohmann::json const& actual,
    double floatTolerance = 1e-5,
    std::vector<std::string>* errors = nullptr);

/** Join collected comparison errors into a newline-separated debug string. */
[[nodiscard]] std::string formatJsonComparisonErrors(std::vector<std::string> const& errors);

}
