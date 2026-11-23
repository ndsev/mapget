#pragma once

#include "point.h"
#include "simfil/model/nodes.h"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace mapget
{
class Feature;
class Geometry;
class ShapePointView;
class PointQueryError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;

    /** SIMFIL delivers values through noexcept callbacks. Guard one expression (including
     * materialization), then rethrow at its normal error boundary instead of unwinding a
     * callback. Without a Capture, a direct inspection read throws at the failing field. */
    class Capture
    {
    public:
        Capture() noexcept;
        ~Capture();
        Capture(Capture const&) = delete;
        Capture& operator=(Capture const&) = delete;
        void rethrowIfFailed() const;
        /** Called only from a catch (PointQueryError const&) at a lazy model read. */
        static bool recordCurrent() noexcept;

    private:
        static thread_local Capture* active_;
        Capture* previous_;
        std::exception_ptr error_;
    };
};

/** One feature's visible geometry and its query-local lazy indexes. Geometry/vertex
 * ordinals always refer to the original published order, never filtered ordinals. */
class ShapePointRange
{
public:
    /** Actual lazy work performed in this retained geometry, not estimates or global counters. */
    struct Work
    {
        uint64_t inventories, recordIndexes, recordsIndexed, distanceTables;
    };
    [[nodiscard]] Work work() const;
    [[nodiscard]] uint32_t size() const;
    [[nodiscard]] simfil::model_ptr<Geometry> geometry() const;
    /** A missing source vertex is not silently clamped or redirected. */
    [[nodiscard]] std::optional<ShapePointView> at(uint32_t pointIndex) const;

private:
    friend class Feature;
    friend class ShapePointView;
    struct State;
    ShapePointRange(
        simfil::model_ptr<Feature> host,
        uint32_t ordinal,
        std::function<bool()> cancelled);
    std::shared_ptr<State> state_;
};

/** Inspection and S4E2 evaluation both start here. The root and any child
 * returned from it own the input snapshot independently of this handle. */
class ShapePointView
{
public:
    [[nodiscard]] Point position() const;
    [[nodiscard]] uint32_t pointIndex() const { return index_; }
    [[nodiscard]] simfil::ModelNode::Ptr queryContext() const;

private:
    friend class ShapePointRange;
    ShapePointView(std::shared_ptr<ShapePointRange::State> state, uint32_t index)
        : state_(std::move(state)), index_(index)
    {
    }
    std::shared_ptr<ShapePointRange::State> state_;
    uint32_t index_;
};
}  // namespace mapget
