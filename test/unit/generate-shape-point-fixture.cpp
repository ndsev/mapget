#include "shape-point-lane-fixture.h"

#include "mapget/model/incoming-relations.h"
#include "mapget/model/stream.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
/** Open a fixture member in binary mode and fail immediately on a short write. */
std::ofstream openOutput(std::filesystem::path const& directory, char const* name)
{
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(directory / name, std::ios::binary);
    return out;
}

template <typename Model>
void writeModel(std::filesystem::path const& directory, char const* name, Model& model)
{
    auto out = openOutput(directory, name);
    auto result = model.write(out);
    if (!result)
        throw std::runtime_error(result.error().message);
}

/** Use production model/stream writers and the same feature restriction as POST /tiles. */
void generate(std::filesystem::path const& directory)
{
    using namespace mapget;
    std::filesystem::create_directories(directory);
    auto source = test::shapePointLane();
    auto schema = source->layerInfo()->schemaOwner();
    auto result = test::shapePointLaneRequest().filter(*source);
    if (!result)
        throw std::runtime_error(result.error().message);
    auto subset = *result;
    auto row = subset->at(0)->shapePointEntryAt(0);
    if (!row || subset->at(0)->shapePointEntryCount() != 1 || !row->valueErrors().empty() ||
        row->values()->toJson() != R"([[-0.15296],[null]])"_json)
    {
        throw std::runtime_error(
            "Native lane projections disagree with the golden values: " + subset->toJson().dump());
    }
    std::array ids{row->featureId()->toString()};
    auto selected = cloneInspectionFeatures(source, ids);
    auto const* dependency = subset->localSourceDependency();
    if (!dependency || dependency->sourceTileKey_ != source->id() ||
        dependency->sourceFeatureCount_ != source->size() ||
        dependency->sourceId_ != source->sourceId() || selected->sourceId() != source->sourceId() ||
        selected->size() != 1)
        throw std::runtime_error(
            "Restricted tile and subset must identify the fixture provider and partition");
    auto host = selected->find(ids.front());
    if (!host || !host->shapePoints(row->geometryOrdinal()).at(row->pointIndex()))
        throw std::runtime_error("The picked vertex is absent from the restricted fixture");

    DataSourceInfo catalog;
    catalog.mapId_ = source->mapId();
    catalog.stringPoolId_ = source->stringPoolId();
    catalog.protocolVersion_ = TileLayerStream::CurrentProtocolVersion;
    catalog.layers_.emplace("Lane", source->layerInfo());
    catalog.setMapSchema(schema);
    auto descriptor = catalog.toJson();
    descriptor["sourceId"] = source->sourceId();
    openOutput(directory, "catalog.json") << nlohmann::json::array({descriptor}).dump(2) << '\n';

    writeModel(directory, "schema.bin", *schema);
    writeModel(directory, "source.bin", *source);
    writeModel(directory, "selected.bin", *selected);
    writeModel(directory, "subset.bin", *subset);
    TileLayerStream::StringPoolOffsetMap offsets;
    TileLayerStream::Writer writer(
        [&](std::string bytes, auto kind)
        {
            if (kind == TileLayerStream::MessageType::StringPool) {
                openOutput(directory, "dictionary.frame").write(bytes.data(), bytes.size());
            }
        },
        offsets,
        false);
    writer.write(source);

    auto expected = nlohmann::json{
        {"schemaId", schema->schemaId()},
        {"sourceId", source->sourceId()},
        {"partitionKey", MapPartitionKey(*source).toString()},
        {"channelOrdinal", 0},
        {"entryOrdinal", 0},
        {"row", row->toJson()},
        {"attributeLayer", "LaneAdasLayerWithGeo"},
        {"attribute", "CURVATURE"},
        {"recordPath", {"records", 0, "code"}},
        {"recordIndex", 0},
        {"raw", 7},
        {"unit", "m^-1"},
    };
    openOutput(directory, "expected.json") << expected.dump(2) << '\n';
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: generate-shape-point-fixture OUTPUT-DIRECTORY\n";
        return 2;
    }
    try {
        generate(argv[1]);
    }
    catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
