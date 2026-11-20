// SPDX-License-Identifier: GPL-2.0-only
#include "CurveData.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace ccxvis {
namespace {
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

struct Row {
    int line;
    QStringList fields;
};

[[noreturn]] void malformed(int line, const QString& detail)
{
    throw std::runtime_error(QString("Line %1: %2").arg(line).arg(detail).toStdString());
}

const QRegularExpression whitespace("[ \\t]+");

std::optional<double> number(QString field)
{
    field = field.trimmed().toLower();
    if (field.isEmpty()) return missing;
    static const QRegularExpression special("^[+-]?(?:nan(?:\\([^)]*\\))?|inf(?:inity)?)$|^\\*+$");
    if (special.match(field).hasMatch()) return missing;
    field.replace('d', 'e');
    bool ok = false;
    const auto value = field.toDouble(&ok);
    if (ok) return std::isfinite(value) ? value : missing;
    // QString reports overflow as an infinity with ok == false.
    if (std::isinf(value)) return missing;
    return std::nullopt;
}

double valueAt(const QString& field, int line)
{
    if (const auto value = number(field)) return *value;
    malformed(line, "Expected a number, found “" + field + "”.");
}

std::vector<Row> csvRows(const QString& text)
{
    enum class State { Start, Plain, Quoted, Closed };
    auto state = State::Start;
    std::vector<Row> rows;
    Row row{1, {}};
    QString field;
    int line = 1;
    const auto endField = [&] {
        row.fields << field;
        field.clear();
        state = State::Start;
    };
    for (auto i = 0; i < text.size(); ++i) {
        const auto ch = text[i];
        if (state == State::Quoted) {
            if (ch == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += ch;
                    ++i;
                } else {
                    state = State::Closed;
                }
            } else {
                field += ch;
                if (ch == '\n') ++line;
            }
            continue;
        }
        if (ch == ',') {
            endField();
        } else if (ch == '\n') {
            if (state != State::Start || !row.fields.isEmpty()) endField();
            rows.push_back(std::move(row));
            row = {++line, {}};
        } else if (ch == '"') {
            if (state != State::Start) malformed(line, "Unexpected quote in CSV field.");
            state = State::Quoted;
        } else if (state == State::Closed) {
            if (!ch.isSpace()) malformed(line, "Unexpected text after a quoted CSV field.");
        } else if (state != State::Start || !ch.isSpace()) {
            state = State::Plain;
            field += ch;
        }
    }
    if (state == State::Quoted) malformed(row.line, "Unterminated quoted CSV field.");
    if (state != State::Start || !row.fields.isEmpty()) {
        endField();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Row> whitespaceRows(const QStringList& lines)
{
    std::vector<Row> rows;
    // A consistently tab-delimited table can represent an empty field explicitly.
    bool tabs = false;
    for (const auto& line : lines) {
        if (line.trimmed().isEmpty()) continue;
        const auto fields = line.split('\t');
        tabs = fields.size() > 1 && std::all_of(fields.begin(), fields.end(), [](const QString& field) {
            return !field.trimmed().contains(whitespace);
        });
        break;
    }
    for (int i = 0; i < lines.size(); ++i) {
        const auto trimmed = lines[i].trimmed();
        rows.push_back({i + 1, trimmed.isEmpty() ? QStringList{} :
            tabs && lines[i].contains('\t') ? lines[i].split('\t') : trimmed.split(whitespace)});
    }
    return rows;
}

CurveData tableData(const std::vector<Row>& rows)
{
    const auto hasFields = [](const Row& row) { return !row.fields.isEmpty(); };
    const auto first = std::find_if(rows.begin(), rows.end(), hasFields);
    if (first == rows.end()) throw std::runtime_error("The file has no tabular data.");
    const auto end = std::find_if(rows.rbegin(), rows.rend(), hasFields).base();
    const auto width = first->fields.size();
    if (width < 2) malformed(first->line, "An XY table needs at least two columns.");

    const bool header = std::none_of(first->fields.begin(), first->fields.end(), [](const QString& field) {
        return !field.trimmed().isEmpty() && number(field).has_value();
    }) && std::any_of(first->fields.begin(), first->fields.end(), [](const QString& field) {
        return !field.trimmed().isEmpty();
    });
    CurveData data;
    for (int i = 0; i < width; ++i) {
        const auto name = header ? first->fields[i].trimmed() : QString{};
        data.series.push_back({name.isEmpty() ? QString("Column %1").arg(i + 1) : name, {}});
    }
    if (header && first + 1 == end) malformed(first->line, "The table has a header but no sample rows.");
    for (auto row = first + header; row != end; ++row) {
        if (!row->fields.isEmpty() && row->fields.size() != width)
            malformed(row->line, QString("Expected %1 columns, found %2.").arg(width).arg(row->fields.size()));
        for (int i = 0; i < width; ++i) {
            const auto value = row->fields.isEmpty() ? missing : valueAt(row->fields[i], row->line);
            data.series[i].samples.push_back({value});
        }
    }
    return data;
}

enum class Quantity {
    Displacement, Velocity, Temperature, StaticTemperature, TotalTemperature, Force, Heat,
    StaticPressure, TotalPressure, NetworkPressure, MassFlow
};

struct NodalBlock {
    Quantity quantity;
    QString label;
    QStringList components;
    bool total = false;
    bool generalized = false;
};

std::optional<NodalBlock> classify(QString heading)
{
    heading = heading.simplified().toLower();
    const auto name = heading.section('(', 0, 0).trimmed();
    if (name == "displacements") return NodalBlock{Quantity::Displacement, "Displacement", {"vx", "vy", "vz"},
                                                   false, heading.contains("ndof")};
    if (name == "velocities") return NodalBlock{Quantity::Velocity, "Velocity", {"vx", "vy", "vz"}};
    if (name == "forces" || name == "total force")
        return NodalBlock{Quantity::Force, "Force", {"fx", "fy", "fz"}, name == "total force"};
    if (name == "temperatures") return NodalBlock{Quantity::Temperature, "Temperature", {QString{}}};
    if (name == "static temperatures")
        return NodalBlock{Quantity::StaticTemperature, "Static temperature", {QString{}}};
    if (name == "total temperatures")
        return NodalBlock{Quantity::TotalTemperature, "Total temperature", {QString{}}};
    if (name == "heat generation" || name == "total heat generation")
        return NodalBlock{Quantity::Heat, "Heat generation", {QString{}}, name == "total heat generation"};
    if (name == "static pressures") return NodalBlock{Quantity::StaticPressure, "Static pressure", {QString{}}};
    if (name == "total pressures") return NodalBlock{Quantity::TotalPressure, "Total pressure", {QString{}}};
    if (name == "network pressures") return NodalBlock{Quantity::NetworkPressure, "Network pressure", {QString{}}};
    if (name == "mass flows") return NodalBlock{Quantity::MassFlow, "Mass flow", {QString{}}};
    return std::nullopt;
}

const QRegularExpression nativeHeading("^\\s*(.*?)\\s*for\\s+set\\s+(.+?)\\s+and\\s+time\\s+(.+?)\\s*$",
                                       QRegularExpression::CaseInsensitiveOption);

CurveData nodalData(const QStringList& lines)
{
    CurveData data;
    data.native = true;
    std::map<std::tuple<Quantity, QString, bool>, std::size_t> groups;
    std::map<std::tuple<std::size_t, qlonglong, int, bool>, std::size_t> indices;
    std::optional<NodalBlock> block;
    QString set;
    double time = 0;
    std::size_t group = 0, ordinal = 0;
    bool hasRows = false;

    for (int i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        const auto header = nativeHeading.match(line);
        if (header.hasMatch()) {
            block = classify(header.captured(1));
            if (!block) continue;
            set = header.captured(2).trimmed();
            time = valueAt(header.captured(3), i + 1);
            if (!std::isfinite(time)) malformed(i + 1, "A nodal block needs a finite recorded time.");
            const auto [entry, fresh] = groups.emplace(std::make_tuple(block->quantity, set, block->total), groups.size());
            if (fresh) data.blockCounts.push_back(0);
            group = entry->second;
            ordinal = data.blockCounts[group]++;
            hasRows = false;
            continue;
        }
        if (!block) continue;
        const auto text = line.trimmed();
        if (text.isEmpty()) {
            if (hasRows) block.reset();
            continue;
        }
        hasRows = true;
        auto fields = text.split(whitespace);
        const bool local = fields.back().compare("L", Qt::CaseInsensitive) == 0;
        if (local) fields.removeLast();
        qlonglong node = 0;
        if (!block->total) {
            bool ok = false;
            if (!fields.isEmpty()) node = fields.takeFirst().toLongLong(&ok);
            if (!ok || node <= 0) malformed(i + 1, "Expected a nodal ID.");
        }
        const auto count = fields.size();
        if (block->generalized ? count < 1 || count > 6 : count != block->components.size())
            malformed(i + 1, "Wrong number of values in the nodal result row.");

        for (int j = 0; j < count; ++j) {
            const auto value = valueAt(fields[j], i + 1);
            const auto [at, fresh] = indices.emplace(std::make_tuple(group, node, j, local), data.series.size());
            if (fresh) {
                auto name = block->label + " · " + set +
                    (block->total ? QString(" · Set total") : QString(" · Node %1").arg(node));
                const auto component = block->generalized ? QString("v%1").arg(j + 1) : block->components[j];
                if (!component.isEmpty()) name += " · " + component;
                if (local) name += " (L)";
                data.series.push_back({std::move(name), {}, group});
            }
            data.series[at->second].samples.push_back({value, time, ordinal});
        }
    }
    if (data.series.empty())
        throw std::runtime_error("No supported *NODE PRINT samples were found. "
                                 "Native blocks such as *EL PRINT, section output, and frequency tables are unsupported.");
    return data;
}

std::optional<QPointF> point(double x, double y)
{
    if (std::isfinite(x) && std::isfinite(y)) return QPointF(x, y);
    return std::nullopt;
}

struct TimeMatch {
    const CurveSample* x = nullptr;
    std::size_t xCount = 0;
    std::size_t yCount = 0;
};
}

CurveData parseCurveData(QString text, bool csv)
{
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    text.replace("\r\n", "\n");
    text.replace('\r', '\n');
    if (csv) return tableData(csvRows(text));
    const auto lines = text.split('\n');
    static const QRegularExpression unsupportedNative(
        "^\\s*(?:\\*(?:EL|NODE|SECTION)\\s+PRINT\\b|E\\s*I\\s*G\\s*E\\s*N\\s*V\\s*A\\s*L\\s*U\\s*E)",
        QRegularExpression::CaseInsensitiveOption);
    const bool native = std::any_of(lines.begin(), lines.end(), [](const QString& line) {
        return nativeHeading.match(line).hasMatch() || unsupportedNative.match(line).hasMatch();
    });
    return native ? nodalData(lines) : tableData(whitespaceRows(lines));
}

PairedCurve pairCurve(const CurveData& data, int x, int y)
{
    PairedCurve curve;
    const auto& source = data.series.at(y);
    curve.points.reserve(source.samples.size());
    if (!data.native) {
        const auto& abscissa = data.series.at(x).samples;
        for (std::size_t i = 0; i < source.samples.size(); ++i)
            curve.points.push_back(point(abscissa.at(i).value, source.samples[i].value));
        return curve;
    }
    if (x == -1) {
        std::size_t next = 0;
        for (const auto& sample : source.samples) {
            while (next < sample.block) {
                curve.points.push_back(std::nullopt);
                ++next;
            }
            curve.points.push_back(point(sample.time, sample.value));
            next = sample.block + 1;
        }
        while (next++ < data.blockCounts.at(source.group)) curve.points.push_back(std::nullopt);
        return curve;
    }

    std::map<double, TimeMatch> byTime;
    for (const auto& sample : data.series.at(x).samples) {
        auto& match = byTime[sample.time];
        match.x = &sample;
        ++match.xCount;
    }
    for (const auto& sample : source.samples) ++byTime[sample.time].yCount;
    std::set<double> missingY;
    for (const auto& [time, match] : byTime) {
        if (match.xCount > 1 || match.yCount > 1) ++curve.ambiguousTimes;
        if (match.xCount == 0 || match.yCount == 0) ++curve.unmatchedTimes;
        if (match.yCount == 0) missingY.insert(time);
    }
    const auto skippedBlock = [](const CurveSample& a, const CurveSample& b) {
        return std::max(a.block, b.block) - std::min(a.block, b.block) > 1;
    };
    const CurveSample* previousX = nullptr;
    const CurveSample* previousY = nullptr;
    for (const auto& sample : source.samples) {
        const auto& match = byTime.at(sample.time);
        const auto pair = match.xCount == 1 && match.yCount == 1 ? point(match.x->value, sample.value) : std::nullopt;
        if (pair && previousY) {
            const auto next = missingY.upper_bound(std::min(previousY->time, sample.time));
            const bool unmatchedBetween = next != missingY.end() && *next < std::max(previousY->time, sample.time);
            if (unmatchedBetween || skippedBlock(*previousX, *match.x) || skippedBlock(*previousY, sample))
                curve.points.push_back(std::nullopt);
        }
        curve.points.push_back(pair);
        previousX = pair ? match.x : nullptr;
        previousY = pair ? &sample : nullptr;
    }
    return curve;
}

}
