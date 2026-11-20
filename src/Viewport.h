// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "FrdDocument.h"

#include <QPoint>
#include <QVTKOpenGLNativeWidget.h>
#include <functional>
#include <memory>

class QMenu;

class Viewport : public QVTKOpenGLNativeWidget {
public:
    enum class Visualization { Surface, Edge, Mesh, FeaturedEdge, FeaturedEdgeWithMesh };
    enum class ColorMap { Rainbow, Viridis, Inferno, Plasma, CoolToWarm, Jet };
    static constexpr int minimumColorSteps = 2;
    static constexpr int maximumColorSteps = 256;
    enum class SectionAxis { X, Y, Z };
    struct SectionPlane {
        SectionAxis axis;
        double coordinate;
        bool keepPositive;
    };

    explicit Viewport(QWidget* parent = nullptr);
    ~Viewport() override;

    void setVisualization(Visualization mode);
    void setMesh(const ccxvis::Mesh& mesh);
    void setResult(const std::vector<double>& values, const QString& label,
                   std::optional<std::array<double, 2>> fixedRange = std::nullopt);
    void beginAnimation();
    void setAnimationLabel(const QString& label);
    void endAnimation();
    void setColorMap(ColorMap map);
    void setColorSteps(std::optional<int> steps);
    void setPositions(const std::vector<ccxvis::Point>& positions);
    void setSection(std::optional<SectionPlane> section);
    std::array<double, 2> sectionBounds(SectionAxis axis) const;
    bool sectionIntersectsMesh();
    void fit();
    void setOrthogonal(bool enabled);
    void view(const ccxvis::Point& direction, const ccxvis::Point& up);
    void setProbing(bool active);
    void onNodePicked(std::function<void(std::optional<std::size_t>)> callback);
    std::optional<std::size_t> pickAt(int x, int y);
    void markNode(std::optional<std::size_t> point);
    QSize saveImage(const QString& path, const QSize& outputSize = {});

protected:
    bool event(QEvent* event) override;

private:
    struct Render;
    std::unique_ptr<Render> render_;
    QMenu* contextMenu_;
    std::optional<QPoint> rightPress_;
    std::optional<ccxvis::Point> rotationTarget_;

    void buildSectionMesh();
    bool pickGeometry(int x, int y);
    std::optional<std::size_t> pickSectionNode(int x, int y);
    void showRotationMenu(const QPoint& position);
    void setRotationCenter(const ccxvis::Point& center);
    void updateColors();
    void applyVisualization();
    void redraw();
};
