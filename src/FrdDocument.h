// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ccxvis {

using Id = std::int64_t;
using Point = std::array<double, 3>;

struct Node {
    Id id;
    Point position;
};

struct Element {
    Id id;
    int type;
    std::vector<std::size_t> points;
};

struct Mesh {
    std::vector<Node> nodes;
    std::vector<Element> elements;
};

struct Dataset {
    std::string field;
    std::vector<std::string> components;
    bool complex = false;
};

struct Frame {
    int step = 0;
    int increment = 0;
    int mode = 0;
    int diameter = -1;
    int subcase = 0;
    int analysis = 0;
    std::string description;
    std::string analysisName;
    double parameter = 0;
    std::vector<std::size_t> datasets;
    std::optional<std::size_t> displacement;
};

struct FieldValues {
    // A nonfinite entry denotes a missing result at that mesh point.
    std::vector<std::vector<double>> components;
};

enum class Derived { Magnitude, Mises };

std::optional<std::array<std::size_t, 3>> displacementComponents(const Dataset& dataset);
bool canComputeMises(const Dataset& dataset);
std::vector<double> derive(const Dataset& dataset, const FieldValues& values, Derived kind);
std::vector<std::size_t> vtkConnectivity(const Element& element);

class FrdDocument {
public:
    static FrdDocument open(const std::filesystem::path& path);
    const Mesh& mesh() const { return mesh_; }
    const std::vector<Dataset>& datasets() const { return datasets_; }
    const std::vector<Frame>& frames() const { return frames_; }
    std::size_t skippedDatasets() const { return skipped_; }
    FieldValues readField(std::size_t dataset);

private:
    struct Block {
        std::streamoff offset;
        std::size_t rows;
        int format;
    };

    std::ifstream file_;
    Mesh mesh_;
    std::unordered_map<Id, std::size_t> nodeIndex_;
    std::vector<Dataset> datasets_;
    std::vector<Frame> frames_;
    std::vector<Block> blocks_;
    std::size_t skipped_ = 0;
};

}
