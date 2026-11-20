// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "FrdDocument.h"

namespace ccxvis {

inline constexpr int magnitudeComponent = -1;
inline constexpr int misesComponent = -2;

struct ContourSelection {
    int frame = -1;
    std::size_t dataset = 0;
    int component = 0;
};

struct ContourSequence {
    std::vector<ContourSelection> frames;
    std::size_t missing = 0;
};

ContourSequence compatibleFrames(const std::vector<Frame>& frames, const std::vector<Dataset>& datasets,
                                 ContourSelection active);

// Accepts complete displacements and checks scaled coordinates for overflow.
bool deformPositions(const Mesh& mesh, const std::vector<Point>& displacements, double scale,
                     std::vector<Point>* positions = nullptr);
std::vector<Point> undeformedPositions(const Mesh& mesh);

struct ScalarExtent {
    void include(const std::vector<double>& values);
    std::optional<std::array<double, 2>> range() const { return range_; }

private:
    std::optional<std::array<double, 2>> range_;
};

}
