#pragma once

#include "geometry-data.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mapget
{

/** How one attribute use site relates its stored records to original geometry vertices.
 * Paths are sequences of literal field names, not expressions. An empty field path
 * reads the element itself (e.g. dense scalars, or a column of a parallel array).
 */
struct PointSeries
{
    using Path = std::vector<std::string>;
    enum class Measure { Wgs84Planar2D, GeographicMetres };
    enum class Sampling { Exact, Linear };
    enum class Markers { None, AppendToValidity, PointValidities };

    struct Position
    {
        enum class Kind {
            Dense,
            VertexIndex,
            Fraction,
            Distance,
            LogicalIndex,
            Interval,
            VertexMeasure
        };
        Kind kind_ = Kind::Dense;
        Path field_;  ///< Index, fraction numerator, distance, or interval start in a record.
        Path bits_;   ///< Fraction width in the record; alternatively read bitsFromAttribute_.
        Path bitsFromAttribute_;
        Path end_;  ///< End of an interval, expressed on the same axis as field_.
        Kind intervalAxis_ = Kind::VertexIndex;
        bool includeStart_ = true;
        bool includeEnd_ = true;
        bool fromAssignmentStart_ = false;
        bool reverse_ = false;
        double distanceScale_ = 1.;  ///< Source distance unit to metres.
        Path vertexMeasures_;  ///< For VertexMeasure: occurrence-relative, one scalar per original
                               ///< vertex.
    };

    struct Field
    {
        std::string name_;  ///< Public contextual name.
        Path path_;         ///< Raw path relative to a record (or column relative to records_).
        std::string unit_;
        std::string codec_;  ///< Unset means identity; otherwise a registered portable codec.
    };

    struct Codec
    {
        struct Code
        {
            int64_t raw_ = 0;
            std::optional<double> value_;
            std::string status_;
            std::optional<double> lower_, upper_;
        };
        double scale_ = 1., offset_ = 0.;
        bool tableOnly_ =
            false;  ///< Unlisted codes have no physical value, rather than affine fallback.
        std::vector<Code> codes_;  ///< Exact values or exceptional/reserved codes.
    };

    std::string featureType_, attributeLayer_, attribute_;
    std::optional<std::string> geometryName_;  ///< Unset requires one geometry of geometryType_.
    GeomType geometryType_ = GeomType::Line;
    bool omittedValidityCovers_ = false;  ///< An explicit empty applicability list never covers.
    Measure measure_ = Measure::GeographicMetres;
    std::optional<Measure> assignmentMeasure_;  ///< Defaults to the anchor measure.
    Path records_;  ///< Array of records, or object of equal-length columns when parallel_ is set.
    bool parallel_ = false;
    Position position_;
    Sampling sampling_ = Sampling::Exact;
    std::vector<Field> fields_;
    Markers markers_ = Markers::None;

    // A shared payload can also contain per-target local profiles. The target key is
    // compared in the canonical host's identity, not by geometry or record order.
    Path branches_, branchTarget_, branchRecords_, hostTarget_;
    Path alternative_;  ///< Optional continuation or direction key on each record.
};

}  // namespace mapget
