#include "point.h"

namespace mapget
{

mapget::Point::Point() : glm::dvec3(.0, .0, .0) {}

mapget::Point::Point(const double& x, const double& y, const double& z) : glm::dvec3(x, y, z) {}

std::string Point::toString() const
{
    return fmt::format("[{},{},{}]", x, y, z);
}

double Point::angleTo(const glm::dvec3& o) const
{
    return glm::atan(o.y - y, o.x - x);
}

double Point::distanceTo(const glm::dvec3& o) const
{
    return glm::distance(*this, o);
}

bool Point::operator==(const Point& o) const
{
    return x == o.x && y == o.y && z == o.z;
}

double Point::geographicDistanceTo(const glm::dvec3& other) const
{
    constexpr double EARTH_RADIUS_IN_METERS = 6371000.8;
    const double dLat = glm::radians(other.y - y);
    const double dLon = glm::radians(other.x - x);
    const double a = glm::sin(dLat * 0.5) *
        glm::sin(dLat * 0.5) + glm::cos(glm::radians(y)) *
        glm::cos(glm::radians(other.y)) *
        glm::sin(dLon * 0.5) *
        glm::sin(dLon * 0.5);
    const double c = 2 * glm::atan(glm::sqrt(a), glm::sqrt(1 - a));
    return EARTH_RADIUS_IN_METERS * c;
}

void to_json(nlohmann::json& j, const Point& p)
{
    j = nlohmann::json::array({p.x, p.y, p.z});
}

void from_json(const nlohmann::json& j, Point& p)
{
    j.at(0).get_to(p.x);
    j.at(1).get_to(p.y);
    if (j.size() > 2) {
        j.at(2).get_to(p.z);
    }
}

}
