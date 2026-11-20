// SPDX-License-Identifier: GPL-2.0-only
#include "ContourSequence.h"

#include <algorithm>
#include <cmath>

namespace ccxvis {
namespace {
bool sameSeries(const Frame& frame, const Frame& active)
{
    return frame.analysis == active.analysis && frame.analysisName == active.analysisName &&
           frame.subcase == active.subcase && (frame.mode > 0) == (active.mode > 0);
}

std::optional<int> matchedComponent(const Dataset& dataset, const Dataset& active, int component)
{
    if (dataset.field != active.field || dataset.complex != active.complex) return std::nullopt;
    if (component == magnitudeComponent)
        return displacementComponents(dataset) ? std::optional{component} : std::nullopt;
    if (component == misesComponent)
        return canComputeMises(dataset) ? std::optional{component} : std::nullopt;
    const auto found = std::find(dataset.components.begin(), dataset.components.end(),
                                 active.components.at(component));
    if (found == dataset.components.end()) return std::nullopt;
    return static_cast<int>(found - dataset.components.begin());
}
}

ContourSequence compatibleFrames(const std::vector<Frame>& frames, const std::vector<Dataset>& datasets,
                                 ContourSelection active)
{
    ContourSequence sequence;
    const auto& origin = frames.at(active.frame);
    const auto& dataset = datasets.at(active.dataset);
    int first = active.frame;
    while (first > 0 && sameSeries(frames[first - 1], origin)) --first;
    for (int i = first; i < static_cast<int>(frames.size()) && sameSeries(frames[i], origin); ++i) {
        const auto before = sequence.frames.size();
        for (const auto id : frames[i].datasets) {
            if (auto component = matchedComponent(datasets[id], dataset, active.component)) {
                sequence.frames.push_back({i, id, *component});
                break;
            }
        }
        if (sequence.frames.size() == before) ++sequence.missing;
    }
    return sequence;
}

bool deformPositions(const Mesh& mesh, const std::vector<Point>& displacements, double scale,
                     std::vector<Point>* positions)
{
    if (mesh.nodes.empty() || mesh.nodes.size() != displacements.size()) return false;
    if (positions) {
        positions->clear();
        positions->reserve(mesh.nodes.size());
    }
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        auto point = mesh.nodes[i].position;
        for (int axis = 0; axis < 3; ++axis) {
            point[axis] += scale * displacements[i][axis];
            if (!std::isfinite(point[axis])) return false;
        }
        if (positions) positions->push_back(point);
    }
    return true;
}

std::vector<Point> undeformedPositions(const Mesh& mesh)
{
    std::vector<Point> positions;
    positions.reserve(mesh.nodes.size());
    for (const auto& node : mesh.nodes) positions.push_back(node.position);
    return positions;
}

void ScalarExtent::include(const std::vector<double>& values)
{
    for (const auto value : values) {
        if (!std::isfinite(value)) continue;
        if (!range_) range_ = std::array{value, value};
        else {
            (*range_)[0] = std::min((*range_)[0], value);
            (*range_)[1] = std::max((*range_)[1], value);
        }
    }
}
}
