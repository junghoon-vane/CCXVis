// SPDX-License-Identifier: GPL-2.0-only
// FRD block decoding adapted from CalculiX GraphiX 2.23 readfrd.c.
// Copyright (C) 1996 Klaus Wittig.
#include "FrdDocument.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace ccxvis {
namespace {

using View = std::string_view;
constexpr auto missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::array<int, 13> nodeCounts{0, 8, 6, 4, 20, 15, 10, 3, 6, 4, 8, 2, 3};

[[noreturn]] void invalid(const std::string& message)
{
    throw std::runtime_error("FRD: " + message);
}

View trim(View value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == View::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

View column(View line, std::size_t offset, std::size_t length)
{
    return offset < line.size() ? trim(line.substr(offset, length)) : View{};
}

template<class T>
T number(View value)
{
    if (value.empty()) invalid("missing numeric value");
    if (value.front() == '+') value.remove_prefix(1);
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        invalid("invalid number '" + std::string(value) + "'");
    return result;
}

template<class T = int>
T integer(View line, std::size_t offset, std::size_t length, T defaultValue = 0)
{
    const auto value = column(line, offset, length);
    return value.empty() ? defaultValue : number<T>(value);
}

double real(View value)
{
    const auto exponent = value.find_first_of("dD");
    if (exponent == View::npos) return number<double>(value);
    std::string fixed(value);
    fixed[exponent] = 'E';
    return number<double>(fixed);
}

bool line(std::istream& file, std::string& record)
{
    if (!std::getline(file, record)) return false;
    if (record.size() > 4096) invalid("oversized text record");
    if (!record.empty() && record.back() == '\r') record.pop_back();
    return true;
}

void next(std::istream& file, std::string& record)
{
    if (!line(file, record)) invalid("unexpected end of file");
}

template<class T>
T binary(std::istream& file)
{
    std::array<unsigned char, sizeof(T)> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        invalid("truncated binary block");
    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    Bits bits = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bits |= static_cast<Bits>(bytes[i]) << (8 * i);
    T result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

int formatOf(View header)
{
    const int format = integer(header, 73, 2);
    if (format < 0 || format > 3) invalid("unsupported record format " + std::to_string(format));
    return format;
}

std::size_t countOf(View header)
{
    const auto count = integer<std::streamoff>(header, 24, 12);
    if (count < 0) invalid("negative record count");
    return static_cast<std::size_t>(count);
}

std::size_t nodesInElement(int type)
{
    if (type <= 0 || type >= static_cast<int>(nodeCounts.size()))
        invalid("unsupported element type " + std::to_string(type));
    return nodeCounts[type];
}

std::vector<Node> readNodes(std::istream& file, int format, std::size_t count)
{
    std::vector<Node> nodes;
    std::string record;
    const int width = format == 0 ? 5 : 10;
    while (format < 2 || nodes.size() < count) {
        Node node{};
        if (format < 2) {
            next(file, record);
            const int code = integer(record, 0, 3);
            if (code == -3) break;
            if (code != -1) invalid("expected node record");
            node.id = integer<Id>(record, 3, width);
            for (int axis = 0; axis < 3; ++axis)
                node.position[axis] = real(column(record, 3 + width + 12 * axis, 12));
        } else {
            node.id = binary<std::int32_t>(file);
            for (double& coordinate : node.position)
                coordinate = format == 2 ? binary<float>(file) : binary<double>(file);
        }
        if (node.id <= 0 || !std::all_of(node.position.begin(), node.position.end(),
                                        [](double coordinate) { return std::isfinite(coordinate); }))
            invalid("invalid node label or coordinates");
        nodes.push_back(node);
    }
    if (count && nodes.size() != count) invalid("node count differs from header");
    return nodes;
}

std::vector<Element> readElements(std::istream& file, int format, std::size_t count,
                                  const std::unordered_map<Id, std::size_t>& nodes)
{
    std::vector<Element> elements;
    std::unordered_set<Id> ids;
    std::string record;
    const int width = format == 0 ? 5 : 10;
    if (format == 3) invalid("unsupported element record format");
    while (format < 2 || elements.size() < count) {
        Element element{};
        if (format < 2) {
            next(file, record);
            const int code = integer(record, 0, 3);
            if (code == -3) break;
            if (code != -1) invalid("expected element record");
            element.id = integer<Id>(record, 3, width);
            element.type = integer(record, 3 + width, 5);
        } else {
            element.id = binary<std::int32_t>(file);
            element.type = binary<std::int32_t>(file);
            binary<std::int32_t>(file);  // Group
            binary<std::int32_t>(file);  // Material
        }
        if (element.id <= 0 || !ids.insert(element.id).second) invalid("invalid or duplicate element ID");
        const auto size = nodesInElement(element.type);
        std::size_t position = 0;
        for (std::size_t i = 0; i < size; ++i) {
            Id id;
            if (format < 2) {
                if (position >= record.size() || i == 0) {
                    next(file, record);
                    if (integer(record, 0, 3) != -2) invalid("expected element connectivity");
                    position = 3;
                }
                id = integer<Id>(record, position, width);
                position += width;
            } else {
                id = binary<std::int32_t>(file);
            }
            const auto found = nodes.find(id);
            if (found == nodes.end()) invalid("element references unknown node " + std::to_string(id));
            element.points.push_back(found->second);
        }
        elements.push_back(std::move(element));
    }
    if (count && elements.size() != count) invalid("element count differs from header");
    return elements;
}

template<std::size_t N>
std::optional<std::array<std::size_t, N>> components(
    const Dataset& dataset, const std::array<View, N>& names)
{
    std::array<std::size_t, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
        const auto found = std::find(dataset.components.begin(), dataset.components.end(), names[i]);
        if (found == dataset.components.end()) return std::nullopt;
        result[i] = std::distance(dataset.components.begin(), found);
    }
    return result;
}

std::optional<std::array<std::size_t, 6>> stressComponents(const Dataset& dataset)
{
    if (dataset.field != "STRESS" || dataset.complex) return std::nullopt;
    auto result = components<6>(dataset, {"SXX", "SYY", "SZZ", "SXY", "SYZ", "SZX"});
    if (!result) result = components<6>(dataset, {"SXX", "SYY", "SZZ", "SXY", "SYZ", "SXZ"});
    return result;
}

} // namespace

std::optional<std::array<std::size_t, 3>> displacementComponents(const Dataset& dataset)
{
    if (dataset.field != "DISP" || dataset.complex) return std::nullopt;
    return components<3>(dataset, {"D1", "D2", "D3"});
}

bool canComputeMises(const Dataset& dataset)
{
    return stressComponents(dataset).has_value();
}

std::vector<double> derive(const Dataset& dataset, const FieldValues& values, Derived kind)
{
    const auto size = values.components.at(0).size();
    std::vector<double> output(size, missing);
    if (kind == Derived::Magnitude) {
        const auto xyz = displacementComponents(dataset);
        if (!xyz) invalid("displacement components unavailable");
        for (std::size_t i = 0; i < size; ++i)
            output[i] = std::hypot(values.components[(*xyz)[0]][i],
                                   values.components[(*xyz)[1]][i], values.components[(*xyz)[2]][i]);
    } else {
        const auto six = stressComponents(dataset);
        if (!six) invalid("stress components unavailable");
        const auto& c = values.components;
        const auto& j = *six;
        for (std::size_t i = 0; i < size; ++i) {
            const auto x = c[j[0]][i], y = c[j[1]][i], z = c[j[2]][i];
            const auto xy = c[j[3]][i], yz = c[j[4]][i], zx = c[j[5]][i];
            output[i] = std::hypot(std::hypot(x-y, y-z, z-x) / std::sqrt(2.0),
                                   std::sqrt(3.0) * std::hypot(xy, yz, zx));
        }
    }
    return output;
}

std::vector<std::size_t> vtkConnectivity(const Element& element)
{
    auto points = element.points;
    if (element.type == 4) {
        // FRD stores vertical midside nodes before the upper face midside nodes.
        std::rotate(points.begin() + 12, points.begin() + 16, points.end());
    } else if (element.type == 2 || element.type == 5) {
        // VTK orders both wedge triangles with the reverse winding of FRD.
        constexpr std::array<std::size_t, 15> order{0, 2, 1, 3, 5, 4, 8, 7, 6, 14, 13, 12, 9, 11, 10};
        std::transform(order.begin(), order.begin() + points.size(), points.begin(),
                       [&](std::size_t index) { return element.points[index]; });
    }
    return points;
}

FrdDocument FrdDocument::open(const std::filesystem::path& path)
{
    FrdDocument doc;
    auto& file = doc.file_;
    file.open(path, std::ios::binary);
    if (!file) invalid("could not open " + path.string());
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    file.seekg(0);
    std::string record;
    next(file, record);
    if (column(record, 0, 6) != "1C") invalid("missing file header");

    bool sawNodes = false, sawElements = false, terminated = false;
    Frame pending;
    using FrameKey = std::tuple<int, int, int, int, int, int, std::string, std::string>;
    std::map<FrameKey, std::size_t> frameIndex;
    std::unordered_set<std::size_t> ambiguousFrames;
    while (line(file, record)) {
        const auto code = column(record, 0, 6);
        if (code.empty() || code == "9999") {
            if (code == "9999") terminated = true;
            continue;
        }
        terminated = false;
        if (code == "1P") {
            const auto key = column(record, 5, 5);
            if (key == "PSTEP") {
                pending = {};
                pending.increment = integer(record, 36, 12);
                pending.step = integer(record, 48, 12);
            } else if (key == "PMODE") {
                pending.mode = integer(record, 24, 12);
            } else if (key == "PHID") {
                pending.diameter = integer(record, 24, 12, -1);
            } else if (key == "PSUBC") {
                pending.subcase = integer(record, 24, 12);
            }
        } else if (code == "2C") {
            if (sawNodes) invalid("multiple meshes are unsupported");
            sawNodes = true;
            doc.mesh_.nodes = readNodes(file, formatOf(record), countOf(record));
            for (std::size_t i = 0; i < doc.mesh_.nodes.size(); ++i)
                if (!doc.nodeIndex_.emplace(doc.mesh_.nodes[i].id, i).second)
                    invalid("duplicate node ID");
        } else if (code == "3C") {
            if (!sawNodes || sawElements) invalid("unexpected element block");
            sawElements = true;
            doc.mesh_.elements = readElements(file, formatOf(record), countOf(record), doc.nodeIndex_);
        } else if (code == "100C") {
            if (!sawNodes) invalid("results precede the mesh");
            const auto format = formatOf(record);
            const auto count = countOf(record);
            const std::string sequence(column(record, 6, 6));
            const std::string parameter(column(record, 12, 12));
            auto frame = pending;
            frame.analysisName = std::string(column(record, 63, 10));
            frame.description = std::string(column(record, 36, 20));
            frame.analysis = integer(record, 56, 2);
            frame.parameter = parameter.empty() ? 0 : real(parameter);
            const int frdStep = integer(record, 58, 5);
            const bool identifiable = !sequence.empty() || (frame.step && frame.increment) || frame.mode;
            if (!frame.step) frame.step = frdStep;
            if (!frame.mode && frame.analysisName == "MODAL") frame.mode = frdStep;
            if (!frame.increment && !frame.mode) frame.increment = frdStep;
            const FrameKey key{frame.step, frame.increment, frame.mode, frame.diameter,
                               frame.subcase, frame.analysis, sequence, frame.analysisName};
            pending = {};

            next(file, record);
            if (integer(record, 0, 3) != -4) invalid("expected result definition");
            Dataset dataset;
            dataset.field = std::string(column(record, 5, 8));
            const int componentCount = integer(record, 13, 5);
            const int resultType = integer(record, 18, 5);
            if (componentCount <= 0 || componentCount > 1024) invalid("invalid component count");
            for (int i = 0; i < componentCount; ++i) {
                next(file, record);
                if (integer(record, 0, 3) != -5) invalid("expected component definition");
                const auto type = integer(record, 18, 5);
                dataset.complex = dataset.complex || type == 12 || type == 14;
                if (integer(record, 33, 5) != 1)
                    dataset.components.emplace_back(column(record, 5, 8));
            }
            const auto offset = file.tellg();
            if (offset < 0) invalid("missing results payload");
            std::size_t rows = 0;
            bool supported = resultType == 1 && !dataset.components.empty();
            if (format < 2) {
                for (;;) {
                    next(file, record);
                    const auto flag = integer(record, 0, 3);
                    if (flag == -3) break;
                    if (flag == -1) ++rows;
                    else if (flag == -6) supported = false;
                    else if (flag != -2) invalid("unexpected result record");
                }
                if (supported && count && rows != count) invalid("result count differs from header");
            } else {
                if (resultType != 1) invalid("unsupported binary result type");
                const auto stride = 4 + (format == 2 ? 4 : 8) * dataset.components.size();
                if (count > static_cast<std::size_t>(end - offset) / stride)
                    invalid("truncated binary results");
                file.seekg(offset + static_cast<std::streamoff>(count * stride));
                if (!file) invalid("could not seek to next binary result block");
                rows = count;
            }
            if (!supported) {
                ++doc.skipped_;
                continue;
            }
            const auto ordinal = doc.datasets_.size();
            auto target = doc.frames_.size();
            if (identifiable && std::isfinite(frame.parameter)) {
                auto [it, inserted] = frameIndex.emplace(key, target);
                if (!inserted) {
                    auto& candidate = doc.frames_[it->second];
                    const auto duplicate = std::any_of(candidate.datasets.begin(), candidate.datasets.end(),
                        [&](std::size_t id) { return doc.datasets_[id].field == dataset.field; });
                    if (duplicate || candidate.parameter != frame.parameter) ambiguousFrames.insert(it->second);
                    if (!ambiguousFrames.count(it->second)) target = it->second;
                    else candidate.displacement.reset();
                }
            }
            if (target == doc.frames_.size()) doc.frames_.push_back(std::move(frame));
            auto& selected = doc.frames_[target];
            selected.datasets.push_back(ordinal);
            if (displacementComponents(dataset) && !ambiguousFrames.count(target))
                selected.displacement = ordinal;
            doc.blocks_.push_back({offset, rows, format});
            doc.datasets_.push_back(std::move(dataset));
        } else if (code != "1U") {
            invalid("unsupported top-level record " + std::string(code));
        }
    }
    if (!terminated) invalid("missing end-of-file marker (file may still be written)");
    if (!sawNodes || doc.mesh_.nodes.empty()) invalid("no mesh nodes found");
    return doc;
}

FieldValues FrdDocument::readField(std::size_t dataset)
{
    const auto& block = blocks_.at(dataset);
    const auto channels = datasets_.at(dataset).components.size();
    FieldValues values;
    values.components.assign(channels, std::vector<double>(mesh_.nodes.size(), missing));
    std::vector<bool> seen(mesh_.nodes.size());
    file_.clear();
    file_.seekg(block.offset);
    if (!file_) invalid("could not seek to results");
    std::string record;
    const int width = block.format == 0 ? 5 : 10;
    for (std::size_t row = 0; row < block.rows; ++row) {
        Id id;
        if (block.format < 2) {
            next(file_, record);
            if (integer(record, 0, 3) != -1) invalid("expected nodal value record");
            id = integer<Id>(record, 3, width);
        } else {
            id = binary<std::int32_t>(file_);
        }
        const auto found = nodeIndex_.find(id);
        if (found == nodeIndex_.end() || seen[found->second]) invalid("unknown or duplicate result node ID");
        seen[found->second] = true;
        for (std::size_t i = 0; i < channels; ++i) {
            double value;
            if (block.format < 2) {
                if (i && i % 6 == 0) {
                    next(file_, record);
                    if (integer(record, 0, 3) != -2) invalid("expected result continuation");
                }
                value = real(column(record, 3 + width + 12 * (i % 6), 12));
            } else {
                value = block.format == 2 ? binary<float>(file_) : binary<double>(file_);
            }
            if (std::isfinite(value)) values.components[i][found->second] = value;
        }
    }
    if (block.format < 2) {
        next(file_, record);
        if (integer(record, 0, 3) != -3) invalid("expected end of results block");
    }
    return values;
}

} // namespace ccxvis
