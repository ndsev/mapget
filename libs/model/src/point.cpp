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
