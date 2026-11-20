// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QPointF>
#include <QString>

#include <optional>
#include <vector>

namespace ccxvis {

struct CurveSample {
    double value;
    double time = 0;
    std::size_t block = 0;
};

struct CurveSeries {
    QString name;
    std::vector<CurveSample> samples;
    std::size_t group = 0;
};

struct CurveData {
    bool native = false;
    std::vector<CurveSeries> series;
    std::vector<std::size_t> blockCounts;
};

struct PairedCurve {
    std::vector<std::optional<QPointF>> points;
    std::size_t unmatchedTimes = 0;
    std::size_t ambiguousTimes = 0;
};

CurveData parseCurveData(QString text, bool csv);
// X = -1 selects the recorded Time axis of a native DAT file.
PairedCurve pairCurve(const CurveData& data, int x, int y);

}
