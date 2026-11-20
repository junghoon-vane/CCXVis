// SPDX-License-Identifier: GPL-2.0-only
#include "Viewport.h"

#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QSurfaceFormat>

#include <vtkActor.h>
#include <vtkActor2DCollection.h>
#include <vtkActorCollection.h>
#include <vtkAlgorithm.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkCutter.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Axis = Viewport::SectionAxis;
using Section = Viewport::SectionPlane;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-7;
}

double scalar(const ccxvis::Point& point)
{
    return 10 + point[0] + 2 * point[1] + 3 * point[2];
}

ccxvis::Mesh solid(int type)
{
    const ccxvis::Point points[]{
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
        {0.5, 0, 0}, {1, 0.5, 0}, {0.5, 1, 0}, {0, 0.5, 0},
        {0, 0, 0.5}, {1, 0, 0.5}, {1, 1, 0.5}, {0, 1, 0.5},
        {0.5, 0, 1}, {1, 0.5, 1}, {0.5, 1, 1}, {0, 0.5, 1}
    };
    ccxvis::Mesh mesh;
    auto& cell = mesh.elements.emplace_back(ccxvis::Element{1, type, {}});
    for (int i = 0; i < (type == 1 ? 8 : 20); ++i) {
        mesh.nodes.push_back({100 + i, points[i]});
        cell.points.push_back(i);
    }
    return mesh;
}

void testOrientationTriad()
{
    constexpr int side = 600;
    constexpr double halfSide = side / 2.;
    vtkSmartPointer<vtkRenderWindow> window;
    {
        Viewport viewport;
        window = viewport.renderWindow();
        window->SetSize(side, side);
        auto* renderers = window->GetRenderers();
        require(renderers->GetNumberOfItems() == 1, "The empty viewport has no orientation overlay");
        viewport.setMesh(solid(1));
        require(renderers->GetNumberOfItems() == 2, "Loading a mesh adds one orientation overlay");
        auto* scene = renderers->GetFirstRenderer();
        auto* overlay = vtkRenderer::SafeDownCast(renderers->GetItemAsObject(1));
        require(overlay && overlay->GetLayer() > scene->GetLayer() && !overlay->GetInteractive(),
                "The orientation renderer is a passive overlay");
        auto* axes = vtkAxesActor::SafeDownCast(overlay->GetViewProps()->GetItemAsObject(0));
        require(axes && axes->GetVisibility() && axes->GetAxisLabels() &&
                    QString::fromLatin1(axes->GetXAxisLabelText()) == "X" &&
                    QString::fromLatin1(axes->GetYAxisLabelText()) == "Y" &&
                    QString::fromLatin1(axes->GetZAxisLabelText()) == "Z", "The triad labels X, Y and Z");
        const auto* bounds = overlay->GetViewport();
        const std::array<double, 4> placement{bounds[0], bounds[1], bounds[2], bounds[3]};
        require(bounds[0] >= 0. && bounds[1] >= 0. && bounds[0] < 0.05 && bounds[1] < 0.05 &&
                    bounds[2] > bounds[0] && bounds[3] > bounds[1] && bounds[2] <= 0.25 && bounds[3] <= 0.25,
                "The triad occupies a compact lower-left viewport");

        auto* camera = scene->GetActiveCamera();
        auto* markerCamera = overlay->GetActiveCamera();
        const auto checkAlignment = [&] {
            // Run the renderer's camera update without a GL context.
            scene->InvokeEvent(vtkCommand::StartEvent);
            for (int axis = 0; axis < 3; ++axis) {
                require(near(camera->GetDirectionOfProjection()[axis], markerCamera->GetDirectionOfProjection()[axis]) &&
                            near(camera->GetViewUp()[axis], markerCamera->GetViewUp()[axis]),
                        "The triad camera follows the scene orientation");
            }
        };
        checkAlignment();
        viewport.view({0, 1, 0}, {0, 0, 1});
        checkAlignment();
        viewport.view({0, 0, 1}, {0, 1, 0});
        checkAlignment();

        auto* controller = window->GetInteractor();
        const int x = qRound(halfSide * (bounds[0] + bounds[2]));
        const int y = qRound(halfSide * (bounds[1] + bounds[3]));
        require(controller->FindPokedRenderer(x, y) == scene, "The triad corner routes input to the scene");
        controller->Enable();
        controller->EnableRenderOff();
        scene->SetWorldPoint(0.004, 0.004, 1., 1.);
        scene->WorldToDisplay();
        const auto* node = scene->GetDisplayPoint();
        camera->SetWindowCenter((node[0] - x) / halfSide, (node[1] - y) / halfSide);
        std::optional<std::size_t> picked;
        viewport.onNodePicked([&](auto point) { picked = point; });
        viewport.setProbing(true);
        controller->SetEventPosition(x, y);
        controller->LeftButtonPressEvent();
        require(picked == 4, "Probe can select a mesh node behind the triad");
        viewport.setProbing(false);
        camera->SetWindowCenter(0., 0.);

        controller->SetEventPosition(x, y);
        controller->LeftButtonPressEvent();
        controller->SetEventPosition(x + 12, y + 8);
        controller->MouseMoveEvent();
        controller->LeftButtonReleaseEvent();
        require(!near(camera->GetDirectionOfProjection()[2], -1.), "A drag over the triad orbits the model");
        checkAlignment();

        const auto scale = camera->GetParallelScale();
        controller->SetEventPosition(x, y);
        controller->MouseWheelForwardEvent();
        require(camera->GetParallelScale() < scale, "The mouse wheel zooms over the triad");
        const auto focalX = camera->GetFocalPoint()[0];
        controller->MiddleButtonPressEvent();
        controller->SetEventPosition(x + 12, y + 8);
        controller->MouseMoveEvent();
        controller->MiddleButtonReleaseEvent();
        require(!near(camera->GetFocalPoint()[0], focalX), "A middle drag over the triad pans the model");
        window->SetSize(800, 450);
        checkAlignment();
        require(std::equal(placement.begin(), placement.end(), overlay->GetViewport()),
                "Orbit, zoom, pan and resize preserve the triad placement");
    }
    require(window->GetRenderers()->GetNumberOfItems() == 1,
            "Destroying the viewport detaches the orientation overlay");
}

struct LegendLayout : vtkScalarBarActor {
    static vtkPolyData* swatches(vtkScalarBarActor& legend, vtkViewport* viewport)
    {
        // Run and inspect VTK's layout without drawing any OpenGL geometry.
        (legend.*&LegendLayout::RebuildLayout)(viewport);
        return legend.*&LegendLayout::ScalarBar;
    }
};

void testDiscreteLegend()
{
    Viewport viewport;
    viewport.renderWindow()->SetSize(600, 600);
    viewport.setMesh(solid(1));
    viewport.setColorMap(Viewport::ColorMap::Viridis);
    viewport.setColorSteps(11);
    viewport.setResult({13., 14., 15., 16., 17., 18., 19., 25.}, "Test range");

    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* actors = renderer->GetActors2D();
    actors->InitTraversal();
    vtkScalarBarActor* legend = nullptr;
    while (auto* actor = actors->GetNextActor2D()) {
        if ((legend = vtkScalarBarActor::SafeDownCast(actor))) break;
    }
    require(legend, "The viewport has a contour legend");
    auto* table = vtkLookupTable::SafeDownCast(legend->GetLookupTable());
    auto* swatches = LegendLayout::swatches(*legend, renderer);
    require(table && swatches->GetNumberOfPolys() == 11, "The legend lays out eleven swatches");
    auto* swatchColors = swatches->GetCellData()->GetScalars();
    for (int i = 0; i < 11; ++i) {
        const auto* expected = table->GetPointer(i);
        for (int component = 0; component < 3; ++component)
            require(std::abs(swatchColors->GetComponent(i, component) - expected[component]) <= 1.,
                    "Each legend swatch shows its matching contour color");
    }
}

vtkActor* cutActor(vtkRenderer* renderer)
{
    auto* actors = renderer->GetActors();
    actors->InitTraversal();
    while (auto* actor = actors->GetNextActor()) {
        auto* algorithm = actor->GetMapper()->GetInputAlgorithm();
        if (algorithm && algorithm->IsA("vtkCutter")) return actor;
    }
    throw std::runtime_error("Section actor uses the cut of the unstructured grid");
}

void checkSectionField(vtkPolyData* cut, int axis, double coordinate, double xDisplacement = 0)
{
    auto* values = cut->GetPointData()->GetArray("Result");
    require(values && cut->GetNumberOfPolys() > 0, "Solid section has a filled nodal contour");
    for (vtkIdType id = 0; id < cut->GetNumberOfPoints(); ++id) {
        ccxvis::Point point;
        cut->GetPoint(id, point.data());
        require(near(point[axis], coordinate), "Section vertices lie on the requested plane");
        point[0] -= xDisplacement;
        require(near(scalar(point), values->GetTuple1(id)), "Section contour interpolates the selected result");
    }
}

void testSolidSections(int type)
{
    Viewport viewport;
    const auto mesh = solid(type);
    viewport.setMesh(mesh);
    std::vector<double> values;
    for (const auto& node : mesh.nodes) values.push_back(scalar(node.position));
    viewport.setResult(values, "Known gradient");

    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* cut = cutActor(renderer);
    auto* cutMapper = vtkPolyDataMapper::SafeDownCast(cut->GetMapper());
    // Clipping is attached to the original exterior actor in addition to the overlays.
    auto* actors = renderer->GetActors();
    actors->InitTraversal();
    auto* surfaceMapper = actors->GetNextActor()->GetMapper();
    const auto scale = renderer->GetActiveCamera()->GetParallelScale();

    for (const auto axis : {Axis::X, Axis::Y, Axis::Z}) {
        require(viewport.sectionBounds(axis) == std::array<double, 2>{0., 1.}, "Displayed section bounds");
        for (bool keepPositive : {true, false}) {
            viewport.setSection(Section{axis, 0.375, keepPositive});
            require(cut->GetVisibility() && viewport.sectionIntersectsMesh(), "The solid is cut");
            require(cutMapper->GetLookupTable() == surfaceMapper->GetLookupTable() &&
                        cutMapper->GetUseLookupTableScalarRange(), "The cut shares the exterior contour range");
            const auto index = static_cast<int>(axis);
            checkSectionField(cutMapper->GetInput(), index, 0.375);
            ccxvis::Point point{};
            point[index] = 1.;
            auto* plane = surfaceMapper->GetClippingPlanes()->GetItem(0);
            require((plane->EvaluateFunction(point.data()) > 0) == keepPositive, "Flip applies the complementary clip");
        }
    }

    constexpr double translation = 1000000.;
    std::vector<ccxvis::Point> displaced;
    for (const auto& node : mesh.nodes)
        displaced.push_back({node.position[0] + translation, node.position[1], node.position[2]});
    viewport.setSection(Section{Axis::X, 0.375, true});
    viewport.setPositions(displaced);
    require(viewport.sectionBounds(Axis::X) == std::array<double, 2>{translation, translation + 1.} &&
                !viewport.sectionIntersectsMesh(), "Displayed coordinates drive the cut and its bounds");
    viewport.setSection(Section{Axis::X, translation + 0.37, true});
    require(viewport.sectionIntersectsMesh(), "A translated mesh can still be cut");
    checkSectionField(cutMapper->GetInput(), 0, translation + 0.37, translation);

    values[0] = std::numeric_limits<double>::quiet_NaN();
    viewport.setResult(values, "Sparse data");
    require(viewport.sectionIntersectsMesh(), "Missing nodal results do not remove cut geometry");
    auto* sparse = cutMapper->GetInput()->GetPointData()->GetArray("Result");
    bool sawMissing = false;
    for (vtkIdType i = 0; i < sparse->GetNumberOfTuples(); ++i)
        sawMissing |= std::isnan(sparse->GetTuple1(i));
    require(sawMissing, "Missing values do not turn into numeric zero on the cut");

    viewport.setVisualization(Viewport::Visualization::Edge);
    require(!cut->GetVisibility(), "Wireframe shows only the clipped exterior wires");
    viewport.setVisualization(Viewport::Visualization::FeaturedEdgeWithMesh);
    require(cut->GetVisibility(), "Shaded modes restore the cut contour");
    viewport.setSection(std::nullopt);
    require(!cut->GetVisibility() && !viewport.sectionIntersectsMesh() &&
                surfaceMapper->GetNumberOfClippingPlanes() == 0, "Clearing restores the exterior");
    require(renderer->GetActiveCamera()->GetParallelScale() == scale, "Section changes leave camera zoom alone");
}

void testCurvedSection()
{
    Viewport viewport;
    auto mesh = solid(4);
    for (auto& node : mesh.nodes) {
        if (node.id >= 108 && node.position[0] == 1.) node.position[0] = 1.2;
    }
    viewport.setMesh(mesh);
    viewport.setSection(Section{Axis::Z, 0.5, false});
    require(viewport.sectionIntersectsMesh(), "The curved solid is cut");
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* cut = vtkPolyDataMapper::SafeDownCast(cutActor(renderer)->GetMapper())->GetInput();
    auto* actors = renderer->GetActors();
    actors->InitTraversal();
    auto* exterior = vtkPolyDataMapper::SafeDownCast(actors->GetNextActor()->GetMapper());

    vtkNew<vtkPlane> plane;
    plane->SetOrigin(0., 0., 0.5);
    plane->SetNormal(0., 0., 1.);
    vtkNew<vtkCutter> outline;
    outline->SetInputConnection(exterior->GetInputConnection(0, 0));
    outline->SetCutFunction(plane);
    outline->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);
    outline->Update();
    require(outline->GetOutput()->GetNumberOfLines() > 0, "The bowed exterior intersects the plane");
    require(near(cut->GetBounds()[1], outline->GetOutput()->GetBounds()[1]),
            "The cap and exterior meet along the bowed face");
}

void testShellAndBeam()
{
    Viewport viewport;
    const ccxvis::Mesh mesh{
        {{1, {0, 0, 0}}, {2, {1, 0, 0}}, {3, {1, 1, 0}}, {4, {0, 1, 0}},
         {5, {0, 2, 0}}, {6, {1, 2, 0}}},
        {{1, 9, {0, 1, 2, 3}}, {2, 11, {4, 5}}}
    };
    viewport.setMesh(mesh);
    viewport.setSection(Section{Axis::X, 0.375, true});
    require(viewport.sectionIntersectsMesh(), "Shells and beams can be cut");
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* cut = vtkPolyDataMapper::SafeDownCast(cutActor(renderer)->GetMapper())->GetInput();
    require(cut->GetNumberOfPolys() == 0 && cut->GetNumberOfLines() > 0 && cut->GetNumberOfVerts() > 0,
            "Shells cut to lines and beams cut to points");
}

void testOriginalNodeOnCut()
{
    Viewport viewport;
    viewport.renderWindow()->SetSize(600, 600);
    ccxvis::Mesh mesh;
    const auto nodeIndex = [](int x, int y, int z) -> std::size_t { return x + 3 * y + 9 * z; };
    for (int z = 0; z <= 2; ++z)
        for (int y = 0; y <= 2; ++y)
            for (int x = 0; x <= 2; ++x)
                mesh.nodes.push_back({static_cast<ccxvis::Id>(100 + nodeIndex(x, y, z)), {0.5 * x, 0.5 * y, 0.5 * z}});
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                mesh.elements.push_back({static_cast<ccxvis::Id>(mesh.elements.size() + 1), 1,
                    {nodeIndex(x, y, z), nodeIndex(x+1, y, z), nodeIndex(x+1, y+1, z), nodeIndex(x, y+1, z),
                     nodeIndex(x, y, z+1), nodeIndex(x+1, y, z+1), nodeIndex(x+1, y+1, z+1), nodeIndex(x, y+1, z+1)}});
    viewport.setMesh(mesh);
    viewport.view({0, 0, 1}, {0, 1, 0});
    viewport.setSection(Section{Axis::Z, 0.5, false});
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    renderer->SetWorldPoint(0.5, 0.5, 0.5, 1.);
    renderer->WorldToDisplay();
    const auto* pixel = renderer->GetDisplayPoint();
    require(viewport.pickAt(qRound(pixel[0]), qRound(pixel[1])) == nodeIndex(1, 1, 1),
            "The probe recognizes an original interior node exposed by the cut");
}

void testPicking()
{
    Viewport viewport;
    viewport.resize(600, 600);
    const auto ratio = viewport.effectiveDevicePixelRatio();
    const auto physicalSize = qRound(600 * ratio);
    viewport.renderWindow()->SetSize(physicalSize, physicalSize);
    viewport.setMesh(solid(1));
    viewport.view({0, 0, 1}, {0, 1, 0});
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();

    renderer->SetWorldPoint(0.004, 0.004, 1., 1.);
    renderer->WorldToDisplay();
    const auto* display = renderer->GetDisplayPoint();
    const int x = qRound(display[0]);
    const int y = qRound(display[1]);
    require(viewport.pickAt(x, y) == 4, "The original front node is selectable");
    viewport.setSection(Section{Axis::Z, 0.375, false});
    require(!viewport.pickAt(x, y), "The cut face blocks original nodes behind it");
    viewport.setVisualization(Viewport::Visualization::Edge);
    require(viewport.pickAt(x, y) == 0, "Wireframe can select the visible kept node");
    viewport.setVisualization(Viewport::Visualization::Surface);

    renderer->SetWorldPoint(0.3, 0.4, 0.375, 1.);
    renderer->WorldToDisplay();
    display = renderer->GetDisplayPoint();
    const QPoint local(qRound(display[0] / ratio), qRound((physicalSize - 1 - display[1]) / ratio));
    auto* menu = viewport.findChild<QMenu*>("viewportContextMenu");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(viewport.mapToGlobal(local)),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(viewport.mapToGlobal(local)),
                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &press);
    QApplication::sendEvent(&viewport, &release);
    require(menu && menu->isVisible(), "The section exposes the rotation center menu");
    menu->actions().front()->trigger();
    const auto* focal = renderer->GetActiveCamera()->GetFocalPoint();
    require(std::abs(focal[0] - 0.3) < 0.01 && std::abs(focal[1] - 0.4) < 0.01 && near(focal[2], 0.375),
            "The rotation center is picked on the actual cut");
}
}

int main(int argc, char** argv)
try {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    testOrientationTriad();
    testDiscreteLegend();
    testSolidSections(1);
    testSolidSections(4);
    testCurvedSection();
    testShellAndBeam();
    testPicking();
    testOriginalNodeOnCut();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
