#pragma once

#include "mapget/model/layerschema.h"
#include "shapepoint-axis.h"
#include "shapepoint-context.h"

#include <map>
#include <span>

namespace mapget::shape
{
/** Query-local position indexes for one original host geometry. */
class RecordIndexes
{
public:
    using Alternative = std::variant<std::monostate, int64_t, std::string>;
    /** Preserve encoded anchor equality even for different widths or >53-bit numerators. */
    struct Fraction
    {
        uint64_t numerator = 0, denominator = 0;
        [[nodiscard]] int compare(Fraction const&) const;
        [[nodiscard]] int compare(double station) const;
    };
    struct Record
    {
        double station, end;
        uint32_t ordinal;
        Fraction fraction;
    };
    struct Table
    {
        struct Continuation
        {
            std::vector<Record> ordered;
            // Only for interval series: skip all intervals that ended before a query.
            std::vector<double> maxEnd;
        };
        std::once_flag built;
        std::map<Alternative, Continuation> alternatives;
    };
    // A width read outside the records can differ between two otherwise
    // identical attachments. Logical and stored validity positions depend on the owner.
    using NodeKey = std::pair<simfil::Model const*, uint32_t>;
    using Key = std::tuple<NodeKey, PointSeries const*, int64_t, NodeKey>;
    std::shared_ptr<Table> get(Key const& key)
    {
        std::lock_guard guard(mutex_);
        auto& table = tables_[key];
        if (!table)
            table = std::make_shared<Table>();
        return table;
    }

    /** All exact XYZ matches, including coincident original vertices with distinct ordinals. */
    [[nodiscard]] std::span<uint32_t const> verticesAt(Axis const& axis, Point p);

private:
    std::mutex mutex_;
    std::map<Key, std::shared_ptr<Table>> tables_;
    std::once_flag verticesBuilt_;
    std::map<std::array<double, 3>, std::vector<uint32_t>> verticesByPosition_;
};

/** Binds a declaration to one attachment, keeping its local coverage and raw
 * paths even when the position index is shared. Fields are decoded on demand. */
class SeriesReader
{
public:
    using Node = simfil::ModelNode::Ptr;
    using Kind = PointSeries::Position::Kind;
    SeriesReader(
        std::shared_ptr<Axis> axis,
        model_ptr<Attribute> occurrence,
        PointSeries const& decl,
        LayerSchema::Ptr schema);

    struct Sample
    {
        Node value;
        std::string status;
        std::optional<double> lower, upper;
        RecordIndexes::Alternative alternative;
        std::string unit;
        std::vector<std::pair<uint32_t, double>> contributors;
    };
    [[nodiscard]] std::vector<Sample>
    samples(uint32_t vertex, PointSeries::Field const& field, std::span<Axis::Span const> spans)
        const;
    [[nodiscard]] Node explain(Sample const&, PointSeries::Field const&) const;

    /** Number of generated markers. Stored anchors already have public validity slots. */
    [[nodiscard]] uint32_t markerCount() const;

    /** For TaggedValidity, element N is the stored validity ordinal paired with raw record N.
     * Includes off-axis anchors: their native markers still have a sample. */
    [[nodiscard]] std::span<uint32_t const> storedMarkerOrdinals() const;

    [[nodiscard]] std::optional<std::pair<Validity::GeometryOffsetType, double>>
    marker(uint32_t i) const;
    [[nodiscard]] Node markerSample(uint32_t record, PointSeries::Field const& field) const;
    /** An inserted logical point has its own authored coordinates, not the chord midpoint. */
    [[nodiscard]] std::optional<Point> logicalMarkerPosition(uint32_t record) const;
    PointSeries const& declaration() const { return decl_; }

private:
    using Record = RecordIndexes::Record;
    static std::optional<double> number(Node const&);
    static std::optional<int64_t> integer(Node const&);
    static RecordIndexes::Alternative alternativeKey(Node const&);
    struct Index
    {
        Node records;
        bool referenced = false;
        std::vector<std::variant<std::string, uint32_t>> path;
        uint32_t count = 0;
        std::vector<uint32_t> storedMarkerOrdinals;
        bool hasLogicalMarkers = false;
        std::shared_ptr<RecordIndexes::Table> table;
    };
    std::shared_ptr<Axis> axis_;
    model_ptr<Attribute> occurrence_;
    PointSeries const& decl_;
    LayerSchema::Ptr schema_;
    mutable std::once_flag selected_, indexed_;
    mutable Index index_;
    mutable std::once_flag measured_;
    mutable std::vector<double> measuredAxis_;
    mutable std::once_flag sequenceBound_;
    mutable model_ptr<AttrPointSequence> sequence_;
    [[nodiscard]] model_ptr<AttrPointSequence> const& sequence() const;
    [[nodiscard]] std::optional<double> markerStation(uint32_t record) const;
    [[nodiscard]] std::vector<double> const& measures() const;
    std::map<PointSeries::Path const*, std::vector<simfil::StringId>> paths_;
    [[nodiscard]] simfil::SchemaType
    valueType(Index const&, uint32_t, PointSeries::Field const&) const;
    void select() const;
    Index const& index() const;
    /** Emit stored anchors on this original geometry in raw record order. */
    void
    forEachStoredStation(Index const&, std::function<void(uint32_t record, double vertex)> const&)
        const;
    /** Physical distances to two tagged anchors and the intervening original vertex. */
    [[nodiscard]] std::array<double, 3>
    taggedDistances(Index const&, Record const& left, uint32_t vertex, Record const& right) const;
    [[nodiscard]] Node read(Node start, PointSeries::Path const&) const;
    [[nodiscard]] Node member(Index const&, uint32_t record, PointSeries::Path const&) const;
    [[nodiscard]] Sample
    decode(uint32_t record, PointSeries::Field const&, RecordIndexes::Alternative const& alt) const;
    [[nodiscard]] double station(
        Index const&,
        uint32_t record,
        PointSeries::Path const&,
        RecordIndexes::Fraction* fraction = nullptr) const;
    [[nodiscard]] double axisValue(double vertex, Kind) const;
    [[nodiscard]] static double logicalIndex(AttrPointSequence const&, uint32_t original);
};

}  // namespace mapget::shape
