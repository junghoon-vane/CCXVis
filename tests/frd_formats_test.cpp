// SPDX-License-Identifier: GPL-2.0-only
#include "FrdDocument.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void header(std::ostream& out, const char* name, int count, int format)
{
    out << std::left << std::setw(24) << name << std::right << std::setw(12) << count
        << std::setw(39) << format << "\n";
}

template<class T>
void binary(std::ostream& out, T value)
{
    // These fixtures target the little-endian Rocky Linux build.
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void testBinary(const std::filesystem::path& path, int format)
{
    {
        std::ofstream out(path, std::ios::binary);
        out << "    1C\n";
        header(out, "    2C", 3, format);
        for (int id : {12, 1800000000, 27}) {
            binary(out, std::int32_t(id));
            for (int i = 0; i < 3; ++i) {
                if (format == 2) binary(out, static_cast<float>(i == 0 ? id % 10 : id % 7));
                else binary(out, static_cast<double>(i == 0 ? id % 10 : id % 7));
            }
        }
        header(out, "    3C", 1, 2);
        for (int value : {45, 12, 0, 1, 12, 1800000000, 27}) binary(out, std::int32_t(value));

        out << "  100C" << std::setw(6) << "101" << std::setw(12) << 1
            << std::setw(12) << 2 << std::setw(39) << format << '\n';
        out << " -4  DISP        4    1\n"
            << " -5  D1          1    2    1    0    0\n"
            << " -5  D2          1    2    2    0    0\n"
            << " -5  D3          1    2    3    0    0\n"
            << " -5  ALL         1    2    0    0    1ALL\n";
        for (int id : {27, 12}) {
            binary(out, std::int32_t(id));
            for (int value : {0, 3, 4}) {
                if (format == 2) binary(out, static_cast<float>(value));
                else binary(out, static_cast<double>(value));
            }
        }
        out << " 9999\n";
    }

    auto doc = ccxvis::FrdDocument::open(path);
    require(doc.mesh().nodes.size() == 3 && doc.mesh().nodes[1].id == 1800000000, "Sparse binary IDs");
    require(doc.mesh().elements[0].points == std::vector<std::size_t>({0, 1, 2}), "Element connectivity");
    const auto values = doc.readField(0);
    require(values.components.size() == 3 && values.components[0][0] == 0, "Binary value and virtual component");
    require(std::isnan(values.components[0][1]), "Absent node differs from zero");
    const auto magnitude = ccxvis::derive(doc.datasets()[0], values, ccxvis::Derived::Magnitude);
    require(magnitude[0] == 5 && std::isnan(magnitude[1]), "Magnitude propagates missing values");
    require(doc.readField(0).components[2][2] == 4, "Repeat lazy read");

    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 9);
    bool rejected = false;
    try {
        ccxvis::FrdDocument::open(path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Truncated binary results rejected");
}

void testAscii(const std::filesystem::path& path)
{
    {
        std::ofstream out(path, std::ios::binary);
        out << "    1C\r\n    2C\r\n"
            << " -1" << std::setw(5) << 10 << std::setw(12) << "7.5D-1"
            << std::setw(12) << "0.0" << std::setw(12) << "2.0" << "\r\n"
            << " -3\r\n"
            << "  100C" << std::setw(6) << 101 << std::setw(12) << 0 << "\r\n"
            << " -4  TEST        7    1\r\n";
        for (int i = 1; i <= 7; ++i)
            out << " -5  " << std::left << std::setw(8) << ("V" + std::to_string(i))
                << std::right << std::setw(5) << 1 << std::setw(5) << 1 << "\r\n";
        out << " -1" << std::setw(5) << 10;
        for (int i = 1; i <= 6; ++i) out << std::setw(12) << i;
        out << "\r\n -2" << std::setw(5) << "" << std::setw(12) << 7
            << "\r\n -3\r\n 9999\r\n";
    }
    auto doc = ccxvis::FrdDocument::open(path);
    require(doc.mesh().nodes[0].position[0] == 0.75, "Short ASCII real and CRLF");
    require(doc.readField(0).components[6][0] == 7, "ASCII continuation");
    {
        std::ofstream out(path, std::ios::binary);
        out << "    1C\n    2C\n -1" << std::setw(5) << 10
            << std::setw(12) << 0 << std::setw(12) << 0 << std::setw(12) << 0
            << "\n -3\n 9999\n";
    }
    require(ccxvis::FrdDocument::open(path).datasets().empty(), "Mesh-only import");
}

int main(int argc, char** argv)
try {
    require(argc == 2, "Supply scratch directory");
    const auto dir = std::filesystem::path(argv[1]);
    const auto file = dir / "ccxvis-formats-test.frd";
    testBinary(file, 2);
    testBinary(file, 3);
    testAscii(file);
    std::filesystem::remove(file);

    const ccxvis::Dataset stress{"STRESS", {"SXX", "SYY", "SZZ", "SXY", "SYZ", "SZX"}};
    const auto mises = ccxvis::derive(stress, {{{100, 0}, {0, 0}, {0, 0}, {0, 1}, {0, 0}, {0, 0}}},
                                  ccxvis::Derived::Mises);
    require(std::abs(mises[0] - 100) < 1e-10 && std::abs(mises[1] - std::sqrt(3.0)) < 1e-10,
            "von Mises in tension and shear");
    ccxvis::Element hex{1, 4, {}};
    for (std::size_t i = 0; i < 20; ++i) hex.points.push_back(i);
    const auto ordered = ccxvis::vtkConnectivity(hex);
    require(ordered[12] == 16 && ordered[15] == 19 && ordered[16] == 12, "HE20 ordering");
    ccxvis::Element wedge{2, 5, {}};
    for (std::size_t i = 0; i < 15; ++i) wedge.points.push_back(i);
    const auto vtkWedge = ccxvis::vtkConnectivity(wedge);
    require(vtkWedge == std::vector<std::size_t>({0, 2, 1, 3, 5, 4, 8, 7, 6, 14, 13, 12, 9, 11, 10}),
            "PE15 outward faces and edge connectivity");
    require(ccxvis::vtkConnectivity({3, 2, {0, 1, 2, 3, 4, 5}}) ==
                std::vector<std::size_t>({0, 2, 1, 3, 5, 4}),
            "PE6 outward faces");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
