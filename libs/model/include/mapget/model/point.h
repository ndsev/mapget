#pragma once

#include <fmt/core.h>
#include <cmath>

#include "glm/glm.hpp"
#include "nlohmann/json.hpp"

namespace mapget
{

/**
 * Concept which is used to construct points
 * from arbitrary other compatible structures.
 */
template <typename T, typename Precision>
concept HasXY = requires(T t) {
    { t.x } -> std::convertible_to<Precision>;
    { t.y } -> std::convertible_to<Precision>;
};

/** Minimal 3D point structure. */
struct Point : public glm::dvec3
{
    /** Define trivial constructors */
    Point();
    Point(Point const&) = default;
    Point(double const& x, double const& y, double const& z = .0);

    /**
     * Allow constructing a point from any class which has .x and .y members.
     * If the other class has a z member, it will be applied as well.
     */
    template <typename T>
    requires HasXY<T, double>
    Point(T const& other) : glm::dvec3(other.x, other.y, .0)  // NOLINT: Allow implicit conversion
    {
        if constexpr (requires { {other.z} -> std::convertible_to<double>; }) {
            z = other.z;
        }
    }

    [[nodiscard]] std::string toString() const;

    [[nodiscard]] double angleTo(const glm::dvec3& o) const;

    [[nodiscard]] double distanceTo(const glm::dvec3& o) const;

    bool operator==(const Point& o) const;
};

/** nlohmann::json bindings */
void to_json(nlohmann::json& j, const Point& p);

void from_json(const nlohmann::json& j, Point& p);

}
