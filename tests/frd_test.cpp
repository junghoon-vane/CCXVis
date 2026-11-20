// SPDX-License-Identifier: GPL-2.0-only
#include "FrdDocument.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv)
try {
    require(argc == 2, "Supply the upstream CGX result.frd");
    auto document = ccxvis::FrdDocument::open(argv[1]);
    const auto& mesh = document.mesh();
    require(mesh.nodes.size() == 3150 && mesh.elements.size() == 560, "Mesh counts");
    require(mesh.elements.front().type == 4 && mesh.nodes.front().id == 1, "Original topology and node IDs");
    require(document.frames().size() == 10 && document.datasets().size() == 20, "Modal catalog");
    const auto first = document.readField(0);
    require(std::abs(first.components.at(0).at(0) - 18.5181) < 1e-4, "First displacement");
    const auto last = document.readField(19);
    require(std::abs(last.components.at(0).at(0) + 14734.4) < 0.1, "Last stress");
    require(document.datasets()[0].components.size() == 3, "CGX ALL is derived");
    require(document.frames()[0].displacement == 0 && document.frames()[9].displacement == 18, "Displacement pairing");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
