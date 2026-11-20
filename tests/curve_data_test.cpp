// SPDX-License-Identifier: GPL-2.0-only
#include "CurveData.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int seriesNamed(const ccxvis::CurveData& data, const QString& name)
{
    for (std::size_t i = 0; i < data.series.size(); ++i)
        if (data.series[i].name == name) return static_cast<int>(i);
    throw std::runtime_error("Missing curve series: " + name.toStdString());
}

void tables()
{
    const auto csv = ccxvis::parseCurveData("\xef\xbb\xbf\"Motion, mm\",\"Load \"\"peak\"\"\"\r\n1D-3,2E+1\r\n,3\r\n2,NaN\r\n3,4\r\n", true);
    require(csv.series[0].name == "Motion, mm" && csv.series[1].name == "Load \"peak\"",
            "CSV headers support quoted commas and escaped quotes");
    auto curve = ccxvis::pairCurve(csv, 0, 1);
    require(curve.points.size() == 4 && curve.points[0] == QPointF(.001, 20) &&
            !curve.points[1] && !curve.points[2] && curve.points[3] == QPointF(3, 4),
            "CSV pairs rows and keeps blanks and nonfinite values as gaps");

    const auto noHeader = ccxvis::parseCurveData("0 1D0\n2E0 -Inf\n\n4 5\n", false);
    curve = ccxvis::pairCurve(noHeader, 0, 1);
    require(noHeader.series[0].name == "Column 1" && curve.points.size() == 4 &&
            curve.points.front() == QPointF(0, 1) && !curve.points[1] && !curve.points[2],
            "Whitespace data allow headerless numbers and gaps");

    const auto tabbed = ccxvis::parseCurveData("t\tx\ty\n0\t\t2\n1\t3\t4\n", false);
    require(!ccxvis::pairCurve(tabbed, 0, 1).points[0] &&
            ccxvis::pairCurve(tabbed, 0, 2).points[0] == QPointF(0, 2),
            "An empty tab-delimited field retains its column");
    try {
        ccxvis::parseCurveData("\"Multi\nline\",Y\n1,2\n3,broken\n", true);
        throw std::logic_error("Malformed numeric row accepted");
    } catch (const std::runtime_error& error) {
        require(QString::fromUtf8(error.what()).contains("line 4", Qt::CaseInsensitive),
                "Malformed row reports its physical line");
    }
}

void native()
{
    const auto data = ccxvis::parseCurveData(R"(
 displacements (vx,vy,vz) for set TIP and time 0D0

        9 2 0 0 L
       10 0 0 0
 total force (fx,fy,fz) for set BASE and time 0E0
          5 0 0
 displacements (vx,vy,vz) for set TIP and time 1
       10 4 0 0
 total force (fx,fy,fz) for set BASE and time 1
          6 0 0
 displacements (vx,vy,vz) for set TIP and time 2
        9 1 0 0 L
 total force (fx,fy,fz) for set BASE and time 2
          7 0 0
 displacements (vx,vy,vz) for set TIP and time 3
        9 2 0 0 L
 total force (fx,fy,fz) for set BASE and time 3
          8 0 0
 temperatures for set TIP and time 3
        9 298
 velocities (vx,vy,vz) for set TIP and time 3
        9 1 2 3
 heat generation for set TIP and time 3
        9 20
 total heat generation for set TIP and time 3
          20
 static pressures for set TIP and time 3
        9 10
 network pressures (total pressure for gases, static pressure for liquids and fluid depth for channels) for set TIP and time 3
        9 11
 mass flows for set TIP and time 3
        9 12
 stresses (elem, integ.pnt.,sxx,syy,szz,sxy,sxz,syz) for set EALL and time 3
        1 1 0 0 0 0 0 0
)", false);
    require(data.native && data.series.size() == 18, "Parse all supported nodal block quantities and totals");
    const int x = seriesNamed(data, "Displacement · TIP · Node 9 · vx (L)");
    const int y = seriesNamed(data, "Force · BASE · Set total · fx");
    const auto history = ccxvis::pairCurve(data, -1, x);
    require(history.points.size() == 4 && !history.points[1] && history.points[3] == QPointF(3, 2),
            "Native Time histories show a gap for an absent node");
    const auto xy = ccxvis::pairCurve(data, x, y);
    require(xy.points.size() == 4 && xy.points[0] == QPointF(2, 5) && !xy.points[1] &&
            xy.points[2] == QPointF(1, 7) && xy.points[3] == QPointF(2, 8) &&
            xy.unmatchedTimes == 1 && xy.ambiguousTimes == 0,
            "Native pairing uses time and preserves the force-displacement loop order");
}

void skipsAbsentNodesInBothAxes()
{
    const auto data = ccxvis::parseCurveData(
        "temperatures for set N and time 0\n1 10\n2 20\n"
        "temperatures for set N and time 1\n3 30\n"
        "temperatures for set N and time 2\n1 11\n2 21\n", false);
    const auto xy = ccxvis::pairCurve(data, 0, 1);
    require(xy.points.size() == 3 && xy.points[0] == QPointF(10, 20) && !xy.points[1] &&
            xy.points[2] == QPointF(11, 21),
            "A native XY curve preserves the gap when both nodes are absent at a recorded time");
}

void fluidQuantities()
{
    const auto data = ccxvis::parseCurveData(
        "static temperatures for set N and time 0\n3 300\n"
        "total temperatures for set N and time 0\n3 310\n"
        "static pressures for set N and time 0\n3 101000\n"
        "total pressures for set N and time 0\n3 105000\n", false);
    require(data.series.size() == 4, "Keep static and total fluid temperature and pressure as distinct quantities");
    const int x = seriesNamed(data, "Static temperature · N · Node 3");
    const int y = seriesNamed(data, "Total temperature · N · Node 3");
    const auto xy = ccxvis::pairCurve(data, x, y);
    require(xy.points.size() == 1 && xy.points.front() == QPointF(300, 310),
            "Pair static and total fluid temperatures at the same time");
}

void duplicateAndUnsupported()
{
    const auto data = ccxvis::parseCurveData(
        "temperatures for set N and time 0\n1 10\n"
        "temperatures for set N and time 1\n1 20\n"
        "temperatures for set N and time 1\n1 21\n"
        "temperatures for set N and time 2\n1 30\n"
        "heat generation for set N and time 1\n1 5\n"
        "heat generation for set N and time 2\n1 6\n", false);
    const auto xy = ccxvis::pairCurve(data, 0, 1);
    require(xy.ambiguousTimes == 1 && xy.unmatchedTimes == 1 && !xy.points.front() &&
            xy.points.back() == QPointF(30, 6), "Duplicate times are ambiguous and unmatched times are counted");

    try {
        ccxvis::parseCurveData("stresses (elem, integ.pnt.,sxx) for set EALL and time 1\n1 1 42\n", false);
        throw std::logic_error("Element-only DAT accepted");
    } catch (const std::runtime_error& error) {
        require(QString::fromUtf8(error.what()).contains("*EL PRINT"), "Explain unsupported native output");
    }
}
}

int main()
{
    try {
        tables();
        native();
        skipsAbsentNodesInBothAxes();
        fluidQuantities();
        duplicateAndUnsupported();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
