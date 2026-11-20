// SPDX-License-Identifier: GPL-2.0-only
#include "Viewport.h"

#include <QAction>
#include <QApplication>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>

#include <vtkActor.h>
#include <vtkAxesActor.h>
#include <vtkCaptionActor2D.h>
#include <vtkCell.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCellPicker.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellType.h>
#include <vtkColorTransferFunction.h>
#include <vtkCoordinate.h>
#include <vtkCutter.h>
#include <vtkDataSetSurfaceFilter.h>
#include <vtkDataSetTriangleFilter.h>
#include <vtkDoubleArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkIdTypeArray.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPlane.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkPolygon.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnstructuredGrid.h>
#include <vtkUnstructuredGridGeometryFilter.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

#ifdef CCXVIS_EXPLICIT_VTK_AUTOINIT
#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkInteractionStyle)
VTK_MODULE_INIT(vtkRenderingFreeType)
VTK_MODULE_INIT(vtkRenderingOpenGL2)
#endif

namespace {
constexpr std::array<int, 13> cellTypes{
    VTK_EMPTY_CELL, VTK_HEXAHEDRON, VTK_WEDGE, VTK_TETRA, VTK_QUADRATIC_HEXAHEDRON,
    VTK_QUADRATIC_WEDGE, VTK_QUADRATIC_TETRA, VTK_TRIANGLE, VTK_QUADRATIC_TRIANGLE,
    VTK_QUAD, VTK_QUADRATIC_QUAD, VTK_LINE, VTK_QUADRATIC_EDGE
};
constexpr char originalPoints[] = "OriginalMeshPoint";
constexpr char sectionSourceCells[] = "SectionSourceCell";
constexpr double featureAngle = 30.0;

// Samples 0, 32, ..., 224, 255 of the CC0 tables at https://bids.github.io/colormap/.
using Palette = std::array<std::array<double, 3>, 9>;
constexpr Palette viridis{{
    {0.267004, 0.004874, 0.329415}, {0.278826, 0.175490, 0.483397}, {0.229739, 0.322361, 0.545706},
    {0.172719, 0.448791, 0.557885}, {0.127568, 0.566949, 0.550556}, {0.157851, 0.683765, 0.501686},
    {0.369214, 0.788888, 0.382914}, {0.678489, 0.863742, 0.189503}, {0.993248, 0.906157, 0.143936}
}};
constexpr Palette inferno{{
    {0.001462, 0.000466, 0.013866}, {0.129285, 0.047293, 0.290788}, {0.341500, 0.062325, 0.429425},
    {0.540920, 0.134729, 0.415123}, {0.735683, 0.215906, 0.330245}, {0.894305, 0.353399, 0.193584},
    {0.978422, 0.557937, 0.034931}, {0.974638, 0.797692, 0.206332}, {0.988362, 0.998364, 0.644924}
}};
constexpr Palette plasma{{
    {0.050383, 0.029803, 0.527975}, {0.299855, 0.009561, 0.631624}, {0.494877, 0.011990, 0.657865},
    {0.665129, 0.138566, 0.585582}, {0.798216, 0.280197, 0.469538}, {0.901807, 0.425087, 0.359688},
    {0.973416, 0.585761, 0.251540}, {0.993033, 0.771720, 0.154808}, {0.940015, 0.975158, 0.131326}
}};

void addPalette(vtkColorTransferFunction& transfer, const Palette& palette)
{
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const auto& [red, green, blue] = palette[i];
        const auto position = i == palette.size() - 1 ? 1. : i * (32. / 255.);
        transfer.AddRGBPoint(position, red, green, blue);
    }
}

void fillColorTable(vtkLookupTable& table, Viewport::ColorMap map, int count)
{
    using ColorMap = Viewport::ColorMap;
    table.SetNumberOfTableValues(count);
    vtkNew<vtkColorTransferFunction> transfer;
    switch (map) {
    case ColorMap::Rainbow:
        table.ForceBuild();
        return;
    case ColorMap::Viridis:
        addPalette(*transfer, viridis);
        break;
    case ColorMap::Inferno:
        addPalette(*transfer, inferno);
        break;
    case ColorMap::Plasma:
        addPalette(*transfer, plasma);
        break;
    case ColorMap::CoolToWarm:
        // Kenneth Moreland's diverging cool-to-warm endpoints.
        transfer->SetColorSpaceToDiverging();
        transfer->AddRGBPoint(0., 0.231373, 0.298039, 0.752941);
        transfer->AddRGBPoint(1., 0.705882, 0.015686, 0.149020);
        break;
    case ColorMap::Jet:
        transfer->AddRGBPoint(0., 0., 0., 0.5);
        transfer->AddRGBPoint(0.125, 0., 0., 1.);
        transfer->AddRGBPoint(0.375, 0., 1., 1.);
        transfer->AddRGBPoint(0.625, 1., 1., 0.);
        transfer->AddRGBPoint(0.875, 1., 0., 0.);
        transfer->AddRGBPoint(1., 0.5, 0., 0.);
        break;
    }
    for (int i = 0; i < count; ++i) {
        double rgb[3];
        transfer->GetColor(static_cast<double>(i) / (count - 1), rgb);
        table.SetTableValue(i, rgb[0], rgb[1], rgb[2], 1.);
    }
    table.BuildSpecialColors();
}

class ContourLegend final : public vtkScalarBarActor {
public:
    static ContourLegend* New();
    vtkTypeMacro(ContourLegend, vtkScalarBarActor);

protected:
    void ConfigureScalarBar() override
    {
        Superclass::ConfigureScalarBar();
        auto* table = vtkLookupTable::SafeDownCast(GetLookupTable());
        if (!table || table->GetIndexedLookup() || table->GetRange()[0] == table->GetRange()[1]) return;
        auto* swatches = vtkUnsignedCharArray::SafeDownCast(ScalarBar->GetCellData()->GetScalars());
        const auto count = table->GetNumberOfTableValues();
        if (!swatches || count != ScalarBar->GetNumberOfPolys()) return;

        // Older VTK versions sample bin boundaries, where rounding can repeat or skip a color.
        const auto components = swatches->GetNumberOfComponents();
        for (vtkIdType i = 0; i < count; ++i)
            std::copy_n(table->GetPointer(i), 3, swatches->GetPointer(i * components));
        swatches->Modified();
    }
};
vtkStandardNewMacro(ContourLegend);

double screenDistanceToPoint(vtkRenderer* renderer, const double* point, int x, int y)
{
    renderer->SetWorldPoint(point[0], point[1], point[2], 1.);
    renderer->WorldToDisplay();
    const auto* display = renderer->GetDisplayPoint();
    return std::hypot(display[0] - x, display[1] - y);
}

using EdgeNodes = std::array<vtkIdType, 3>;

void addEdge(vtkCellArray* lines, const EdgeNodes& nodes)
{
    if (nodes[2] < 0) {
        lines->InsertNextCell(2, nodes.data());
    } else {
        const vtkIdType path[]{nodes[0], nodes[2], nodes[1]};
        lines->InsertNextCell(3, path);
    }
}

void buildEdgeGeometry(vtkUnstructuredGrid* grid, vtkPolyData* wire, vtkPolyData* mesh,
                       vtkPolyData* features)
{
    // Keep the original faces so the rendering tessellation adds no mesh edges.
    vtkNew<vtkUnstructuredGridGeometryFilter> boundary;
    boundary->SetInputData(grid);
    boundary->MergingOff();
    boundary->PassThroughPointIdsOn();
    boundary->SetOriginalPointIdsName(originalPoints);
    boundary->Update();
    auto* exterior = boundary->GetOutput();
    auto* original = vtkIdTypeArray::SafeDownCast(exterior->GetPointData()->GetArray(originalPoints));
    const auto pointId = [original](vtkIdType id) {
        return original ? original->GetValue(id) : id;
    };
    const auto edgeNodes = [&](vtkCell* edge) {
        return EdgeNodes{pointId(edge->GetPointId(0)), pointId(edge->GetPointId(1)),
                         edge->GetNumberOfPoints() == 3 ? pointId(edge->GetPointId(2)) : -1};
    };

    struct Incidence {
        ccxvis::Point normal;
        bool reversed;
        int faces;
        bool crease;
    };
    std::map<EdgeNodes, Incidence> edges;
    vtkNew<vtkCellArray> wireLines;
    vtkNew<vtkCellArray> vertices;
    vtkNew<vtkCellArray> meshLines;
    vtkNew<vtkCellArray> featureLines;
    const auto creaseLimit = std::cos(vtkMath::RadiansFromDegrees(featureAngle));
    for (vtkIdType i = 0; i < exterior->GetNumberOfCells(); ++i) {
        auto* cell = exterior->GetCell(i);
        const auto dimension = cell->GetCellDimension();
        if (dimension == 0) {
            const auto id = pointId(cell->GetPointId(0));
            vertices->InsertNextCell(1, &id);
        } else if (dimension == 1) {
            addEdge(wireLines, edgeNodes(cell));
        } else if (dimension == 2) {
            const auto edgeCount = cell->GetNumberOfEdges();
            ccxvis::Point normal{};
            vtkPolygon::ComputeNormal(cell->GetPoints(), edgeCount, nullptr, normal.data());
            for (int j = 0; j < edgeCount; ++j) {
                auto key = edgeNodes(cell->GetEdge(j));
                const bool reversed = key[0] > key[1];
                if (reversed) std::swap(key[0], key[1]);
                auto [it, added] = edges.try_emplace(key, Incidence{normal, reversed, 1, false});
                if (!added) {
                    auto& edge = it->second;
                    ++edge.faces;
                    // Account for adjacent shell faces that have opposite winding.
                    const auto alignment = edge.reversed == reversed ? -1.0 : 1.0;
                    if (vtkMath::Norm(edge.normal.data()) > 0 && vtkMath::Norm(normal.data()) > 0)
                        edge.crease |= alignment * vtkMath::Dot(edge.normal.data(), normal.data()) < creaseLimit;
                }
            }
        }
    }
    for (const auto& [nodes, edge] : edges) {
        addEdge(wireLines, nodes);
        addEdge(meshLines, nodes);
        if (edge.faces != 2 || edge.crease) addEdge(featureLines, nodes);
    }

    for (auto* data : {wire, mesh, features}) {
        data->Initialize();
        data->SetPoints(grid->GetPoints());
    }
    wire->SetVerts(vertices);
    wire->SetLines(wireLines);
    mesh->SetLines(meshLines);
    features->SetLines(featureLines);
}
}

struct Viewport::Render {
    vtkNew<vtkGenericOpenGLRenderWindow> window;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkOrientationMarkerWidget> orientation;
    vtkNew<vtkUnstructuredGrid> grid;
    vtkNew<vtkDataSetSurfaceFilter> surface;
    vtkNew<vtkPolyDataMapper> mapper;
    vtkNew<vtkActor> meshActor;
    vtkNew<vtkPolyData> wireEdges;
    vtkNew<vtkPolyData> meshEdges;
    vtkNew<vtkPolyData> featureEdges;
    vtkNew<vtkPolyDataMapper> edgeMapper;
    vtkNew<vtkPolyDataMapper> featureMapper;
    vtkNew<vtkActor> edgeActor;
    vtkNew<vtkActor> featureActor;
    vtkNew<vtkUnstructuredGrid> sectionGrid;
    vtkNew<vtkPlane> sectionPlane;
    vtkNew<vtkCutter> sectionCut;
    vtkNew<vtkPolyDataMapper> sectionMapper;
    vtkNew<vtkActor> sectionActor;
    std::optional<SectionPlane> section;
    Visualization visualization = Visualization::Surface;
    ColorMap colorMap = ColorMap::Rainbow;
    int colorCount = maximumColorSteps;
    vtkNew<vtkLookupTable> colors;
    vtkNew<ContourLegend> legend;
    vtkNew<vtkTextActor> frameLabel;
    std::optional<std::array<double, 2>> originalClipping;
    vtkNew<vtkCellPicker> picker;
    vtkNew<vtkPolyData> pickedPoint;
    vtkNew<vtkPolyDataMapper> pickedMapper;
    vtkNew<vtkActor> highlight;
    vtkNew<vtkCallbackCommand> click;
    std::function<void(std::optional<std::size_t>)> onPick;
    bool probing = false;
};

Viewport::Viewport(QWidget* parent)
    : QVTKOpenGLNativeWidget(parent), render_(new Render), contextMenu_(new QMenu(this))
{
    setContextMenuPolicy(Qt::PreventContextMenu);
    contextMenu_->setObjectName("viewportContextMenu");
    contextMenu_->addAction("Set Rotation Center", this, [this] {
        if (rotationTarget_) setRotationCenter(*rotationTarget_);
    });

    auto& r = *render_;
    setRenderWindow(r.window);
    r.window->AddRenderer(r.renderer);
    r.window->SetMultiSamples(0);
    r.renderer->SetBackground(0.075, 0.10, 0.15);
    r.renderer->GetActiveCamera()->ParallelProjectionOn();

    vtkNew<vtkAxesActor> axes;
    axes->SetShaftTypeToCylinder();
    axes->SetCylinderRadius(0.035);
    const auto styleAxis = [](vtkProperty* shaft, vtkProperty* tip, vtkCaptionActor2D* caption,
                              double red, double green, double blue) {
        shaft->SetColor(red, green, blue);
        tip->SetColor(red, green, blue);
        auto* label = caption->GetCaptionTextProperty();
        label->SetColor(red, green, blue);
        label->ItalicOff();
    };
    styleAxis(axes->GetXAxisShaftProperty(), axes->GetXAxisTipProperty(),
              axes->GetXAxisCaptionActor2D(), 1., 0.30, 0.30);
    styleAxis(axes->GetYAxisShaftProperty(), axes->GetYAxisTipProperty(),
              axes->GetYAxisCaptionActor2D(), 0.38, 0.90, 0.41);
    styleAxis(axes->GetZAxisShaftProperty(), axes->GetZAxisTipProperty(),
              axes->GetZAxisCaptionActor2D(), 0.42, 0.75, 1.);
    r.orientation->SetOrientationMarker(axes);
    r.orientation->SetViewport(0.02, 0.02, 0.20, 0.24);
    r.orientation->KeyPressActivationOff();
    r.orientation->SetInteractor(interactor());

    r.surface->SetInputData(r.grid);
    r.surface->PassThroughPointIdsOn();
    r.surface->SetOriginalPointIdsName(originalPoints);
    r.surface->SetNonlinearSubdivisionLevel(1);
    r.mapper->SetInputConnection(r.surface->GetOutputPort());
    r.mapper->ScalarVisibilityOff();
    r.mapper->InterpolateScalarsBeforeMappingOn();
    r.mapper->SetLookupTable(r.colors);
    r.meshActor->SetMapper(r.mapper);
    r.meshActor->GetProperty()->SetColor(0.70, 0.78, 0.86);
    r.meshActor->GetProperty()->SetPointSize(5);
    r.renderer->AddActor(r.meshActor);

    vtkMapper::SetResolveCoincidentTopologyToPolygonOffset();
    r.edgeMapper->SetInputData(r.meshEdges);
    r.edgeMapper->SetLookupTable(r.colors);
    r.edgeMapper->UseLookupTableScalarRangeOn();
    r.edgeMapper->InterpolateScalarsBeforeMappingOn();
    r.edgeMapper->SetRelativeCoincidentTopologyLineOffsetParameters(0., -2.);
    r.edgeActor->SetMapper(r.edgeMapper);
    r.edgeActor->GetProperty()->LightingOff();
    r.edgeActor->GetProperty()->SetPointSize(5);
    r.renderer->AddActor(r.edgeActor);

    r.featureMapper->SetInputData(r.featureEdges);
    r.featureMapper->ScalarVisibilityOff();
    r.featureMapper->SetRelativeCoincidentTopologyLineOffsetParameters(0., -4.);
    r.featureActor->SetMapper(r.featureMapper);
    r.featureActor->PickableOff();
    r.featureActor->GetProperty()->LightingOff();
    r.featureActor->GetProperty()->SetColor(0.82, 0.91, 0.99);
    r.featureActor->GetProperty()->SetLineWidth(2.);
    r.renderer->AddActor(r.featureActor);

    r.sectionCut->SetInputData(r.sectionGrid);
    r.sectionCut->SetCutFunction(r.sectionPlane);
    r.sectionCut->GenerateCutScalarsOff();
    r.sectionCut->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);
    r.sectionMapper->SetInputConnection(r.sectionCut->GetOutputPort());
    r.sectionMapper->SetLookupTable(r.colors);
    r.sectionMapper->UseLookupTableScalarRangeOn();
    r.sectionMapper->SetScalarModeToUsePointFieldData();
    r.sectionMapper->SelectColorArray("Result");
    r.sectionMapper->InterpolateScalarsBeforeMappingOn();
    r.sectionActor->SetMapper(r.sectionMapper);
    r.sectionActor->GetProperty()->SetColor(r.meshActor->GetProperty()->GetColor());
    r.sectionActor->GetProperty()->LightingOff();
    r.sectionActor->GetProperty()->SetLineWidth(2.);
    r.sectionActor->GetProperty()->SetPointSize(5.);
    r.renderer->AddActor(r.sectionActor);
    applyVisualization();

    r.colors->SetNanColor(0.56, 0.56, 0.62, 1.);
    r.colors->SetHueRange(0.67, 0.0);
    fillColorTable(*r.colors, r.colorMap, r.colorCount);
    r.legend->SetLookupTable(r.colors);
    r.legend->SetMaximumNumberOfColors(r.colorCount);
    r.legend->SetNumberOfLabels(5);
    r.legend->SetLabelFormat("%.4g");
    r.legend->SetMaximumWidthInPixels(130);
    r.legend->SetPosition(0.83, 0.15);
    r.legend->SetWidth(0.16);
    r.legend->SetHeight(0.68);
    r.legend->GetLabelTextProperty()->SetColor(0.92, 0.94, 1.);
    r.legend->GetTitleTextProperty()->SetColor(0.92, 0.94, 1.);
    r.legend->VisibilityOff();
    r.renderer->AddActor2D(r.legend);

    r.frameLabel->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    r.frameLabel->SetPosition(0.025, 0.965);
    r.frameLabel->GetTextProperty()->SetVerticalJustificationToTop();
    r.frameLabel->GetTextProperty()->SetFontSize(17);
    r.frameLabel->GetTextProperty()->SetColor(0.92, 0.94, 1.);
    r.frameLabel->GetTextProperty()->ShadowOn();
    r.frameLabel->VisibilityOff();
    r.renderer->AddActor2D(r.frameLabel);

    r.picker->PickFromListOn();
    r.picker->AddPickList(r.meshActor);
    r.picker->AddPickList(r.edgeActor);
    r.picker->AddPickList(r.sectionActor);
    r.picker->SetTolerance(0.012);
    r.pickedMapper->SetInputData(r.pickedPoint);
    r.highlight->SetMapper(r.pickedMapper);
    r.highlight->GetProperty()->SetColor(1., 1., 1.);
    r.highlight->GetProperty()->SetPointSize(11);
    r.highlight->PickableOff();
    r.highlight->VisibilityOff();
    r.renderer->AddActor(r.highlight);

    r.click->SetClientData(this);
    r.click->SetCallback([](vtkObject* caller, unsigned long, void* data, void*) {
        auto& self = *static_cast<Viewport*>(data);
        auto& state = *self.render_;
        state.click->SetAbortFlag(0);
        if (!state.probing) return;
        state.click->SetAbortFlag(1);
        const auto* xy = vtkRenderWindowInteractor::SafeDownCast(caller)->GetEventPosition();
        if (state.onPick) state.onPick(self.pickAt(xy[0], xy[1]));
    });
    interactor()->AddObserver(vtkCommand::LeftButtonPressEvent, r.click, 1.f);
}

Viewport::~Viewport()
{
    render_->orientation->EnabledOff();
    render_->orientation->SetInteractor(nullptr);
    if (auto* controller = interactor()) controller->RemoveObserver(render_->click);
    setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
}

bool Viewport::event(QEvent* event)
{
    const auto type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease &&
        type != QEvent::MouseButtonDblClick && type != QEvent::MouseMove)
        return QVTKOpenGLNativeWidget::event(event);

    const auto* mouse = static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto position = mouse->position().toPoint();
#else
    const auto position = mouse->pos();
#endif
    const auto dragDistance = QApplication::startDragDistance();
    if (type == QEvent::MouseMove && rightPress_ &&
        (position - *rightPress_).manhattanLength() >= dragDistance)
        rightPress_.reset();

    bool contextClick = false;
    if (mouse->button() == Qt::RightButton) {
        if (type == QEvent::MouseButtonRelease) {
            contextClick = rightPress_ && rect().contains(position) &&
                           (position - *rightPress_).manhattanLength() < dragDistance;
            rightPress_.reset();
        } else {
            rightPress_ = position;
        }
    }

    // VTK must finish the right-button interaction before the menu takes the mouse grab.
    const bool handled = QVTKOpenGLNativeWidget::event(event);
    if (contextClick) showRotationMenu(position);
    return handled || contextClick;
}

void Viewport::showRotationMenu(const QPoint& position)
{
    auto& r = *render_;
    const auto* size = r.window->GetSize();
    if (r.grid->GetNumberOfPoints() == 0 || size[0] <= 0 || size[1] <= 0) return;

    const double ratio = effectiveDevicePixelRatio();
    const int x = std::clamp(qRound(position.x() * ratio), 0, size[0] - 1);
    const int y = size[1] - 1 - std::clamp(qRound(position.y() * ratio), 0, size[1] - 1);
    ccxvis::Point target;
    if (pickGeometry(x, y)) {
        r.picker->GetPickPosition(target.data());
    } else {
        // A background click uses the depth of the current rotation center.
        const auto* focal = r.renderer->GetActiveCamera()->GetFocalPoint();
        r.renderer->SetWorldPoint(focal[0], focal[1], focal[2], 1.);
        r.renderer->WorldToDisplay();
        const double depth = r.renderer->GetDisplayPoint()[2];
        r.renderer->SetDisplayPoint(x, y, depth);
        r.renderer->DisplayToWorld();
        const auto* point = r.renderer->GetWorldPoint();
        if (point[3] == 0.) return;
        for (int axis = 0; axis < 3; ++axis) target[axis] = point[axis] / point[3];
    }
    if (!std::all_of(target.begin(), target.end(), [](double value) { return std::isfinite(value); }))
        return;
    rotationTarget_ = target;
    contextMenu_->popup(mapToGlobal(position));
}

void Viewport::setRotationCenter(const ccxvis::Point& center)
{
    auto* renderer = render_->renderer.GetPointer();
    auto* camera = renderer->GetActiveCamera();
    const auto* focal = camera->GetFocalPoint();
    const auto* position = camera->GetPosition();
    ccxvis::Point newPosition;
    for (int axis = 0; axis < 3; ++axis)
        newPosition[axis] = center[axis] + (position[axis] - focal[axis]);
    camera->SetFocalPoint(center.data());
    camera->SetPosition(newPosition.data());
    renderer->ResetCameraClippingRange();
    redraw();
}

void Viewport::setMesh(const ccxvis::Mesh& mesh)
{
    contextMenu_->hide();
    rightPress_.reset();
    rotationTarget_.reset();
    setSection(std::nullopt);
    auto& r = *render_;
    r.sectionGrid->Initialize();
    r.grid->Initialize();
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    points->Allocate(mesh.nodes.size());
    for (const auto& node : mesh.nodes) points->InsertNextPoint(node.position.data());
    r.grid->SetPoints(points);
    r.grid->Allocate(mesh.elements.empty() ? mesh.nodes.size() : mesh.elements.size());
    for (const auto& element : mesh.elements) {
        const auto indices = ccxvis::vtkConnectivity(element);
        const std::vector<vtkIdType> ids(indices.begin(), indices.end());
        r.grid->InsertNextCell(cellTypes.at(element.type), ids.size(), ids.data());
    }
    if (mesh.elements.empty()) {
        for (vtkIdType id = 0; id < points->GetNumberOfPoints(); ++id)
            r.grid->InsertNextCell(VTK_VERTEX, 1, &id);
    }
    buildEdgeGeometry(r.grid, r.wireEdges, r.meshEdges, r.featureEdges);
    r.highlight->VisibilityOff();
    r.legend->VisibilityOff();
    r.mapper->ScalarVisibilityOff();
    applyVisualization();
    if (mesh.nodes.empty()) {
        r.orientation->EnabledOff();
    } else if (!r.orientation->GetEnabled()) {
        r.orientation->SetCurrentRenderer(r.renderer);
        r.orientation->EnabledOn();
        // VTK's overlay renderer is non-interactive. Disable the widget's mouse handlers too.
        r.orientation->InteractiveOff();
    }
    view({1, 1, 1}, {0, 0, 1});
}

void Viewport::setPositions(const std::vector<ccxvis::Point>& positions)
{
    auto& r = *render_;
    auto* points = r.grid->GetPoints();
    for (std::size_t i = 0; i < positions.size(); ++i) points->SetPoint(i, positions[i].data());
    points->Modified();
    r.renderer->ResetCameraClippingRange();
    redraw();
}

void Viewport::setResult(const std::vector<double>& values, const QString& label,
                         std::optional<std::array<double, 2>> fixedRange)
{
    auto& r = *render_;
    auto low = std::numeric_limits<double>::infinity();
    auto high = -low;
    vtkNew<vtkDoubleArray> scalars;
    scalars->SetName("Result");
    scalars->SetNumberOfValues(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto value = values[i];
        scalars->SetValue(i, value);
        if (std::isfinite(value)) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    if (fixedRange && std::isfinite((*fixedRange)[0]) && std::isfinite((*fixedRange)[1]) &&
        (*fixedRange)[0] <= (*fixedRange)[1]) {
        low = (*fixedRange)[0];
        high = (*fixedRange)[1];
    }
    if (!values.empty() && low <= high) {
        r.grid->GetPointData()->SetScalars(scalars);
        if (r.sectionGrid->GetPoints()) r.sectionGrid->GetPointData()->SetScalars(scalars);
        r.wireEdges->GetPointData()->SetScalars(scalars);
        r.colors->SetRange(low, high);
        r.mapper->UseLookupTableScalarRangeOn();
        r.mapper->ScalarVisibilityOn();
        r.legend->SetTitle(label.toUtf8().constData());
        r.legend->VisibilityOn();
    } else {
        r.grid->GetPointData()->SetScalars(nullptr);
        if (r.sectionGrid->GetPoints()) r.sectionGrid->GetPointData()->SetScalars(nullptr);
        r.wireEdges->GetPointData()->SetScalars(nullptr);
        r.mapper->ScalarVisibilityOff();
        r.legend->VisibilityOff();
    }
    applyVisualization();
    redraw();
}

void Viewport::beginAnimation()
{
    auto& r = *render_;
    const auto* clipping = r.renderer->GetActiveCamera()->GetClippingRange();
    r.originalClipping = std::array{clipping[0], clipping[1]};
    contextMenu_->hide();
    rightPress_.reset();
    setEnabled(false);
}

void Viewport::setAnimationLabel(const QString& label)
{
    render_->frameLabel->SetInput(label.toUtf8().constData());
    render_->frameLabel->SetVisibility(!label.isEmpty());
    redraw();
}

void Viewport::endAnimation()
{
    auto& r = *render_;
    if (r.originalClipping) {
        r.renderer->GetActiveCamera()->SetClippingRange(r.originalClipping->data());
        r.originalClipping.reset();
    }
    setEnabled(true);
    setAnimationLabel({});
}

void Viewport::setColorMap(ColorMap map)
{
    if (render_->colorMap == map) return;
    render_->colorMap = map;
    updateColors();
}

void Viewport::setColorSteps(std::optional<int> steps)
{
    const auto count = std::clamp(steps.value_or(maximumColorSteps), minimumColorSteps, maximumColorSteps);
    if (render_->colorCount == count) return;
    render_->colorCount = count;
    updateColors();
}

void Viewport::updateColors()
{
    auto& r = *render_;
    fillColorTable(*r.colors, r.colorMap, r.colorCount);
    r.legend->SetMaximumNumberOfColors(r.colorCount);
    redraw();
}

void Viewport::setVisualization(Visualization mode)
{
    render_->visualization = mode;
    applyVisualization();
    redraw();
}

void Viewport::applyVisualization()
{
    auto& r = *render_;
    const auto mode = r.visualization;
    const bool wire = mode == Visualization::Edge;
    const bool showMesh = mode == Visualization::Mesh || mode == Visualization::FeaturedEdgeWithMesh;
    const bool showFeatures = mode == Visualization::FeaturedEdge || mode == Visualization::FeaturedEdgeWithMesh;
    r.meshActor->SetVisibility(!wire);
    r.edgeActor->SetVisibility(wire || showMesh);
    r.edgeActor->SetPickable(wire);
    r.edgeMapper->SetInputData(wire ? r.wireEdges.GetPointer() : r.meshEdges.GetPointer());
    r.edgeMapper->SetScalarVisibility(wire && r.mapper->GetScalarVisibility());
    if (wire) r.edgeActor->GetProperty()->SetColor(0.70, 0.78, 0.86);
    else r.edgeActor->GetProperty()->SetColor(0.12, 0.15, 0.21);
    r.featureActor->SetVisibility(showFeatures);
    r.sectionActor->SetVisibility(!wire && r.section.has_value());
    r.sectionMapper->SetScalarVisibility(r.mapper->GetScalarVisibility());
}

void Viewport::buildSectionMesh()
{
    auto& r = *render_;
    vtkNew<vtkIdTypeArray> sourceCells;
    sourceCells->SetName(sectionSourceCells);
    sourceCells->SetNumberOfValues(r.grid->GetNumberOfCells());
    for (vtkIdType i = 0; i < r.grid->GetNumberOfCells(); ++i) sourceCells->SetValue(i, i);
    r.grid->GetCellData()->AddArray(sourceCells);

    // Cut and render the same tessellation so the cap joins curved exterior faces.
    // The output retains the input points, which are updated by setPositions().
    vtkNew<vtkDataSetTriangleFilter> tessellator;
    tessellator->SetInputData(r.grid);
    tessellator->Update();
    r.sectionGrid->ShallowCopy(tessellator->GetOutput());
    r.grid->GetCellData()->RemoveArray(sectionSourceCells);
}

void Viewport::setSection(std::optional<SectionPlane> section)
{
    auto& r = *render_;
    if (!section && !r.section) return;
    if (section && r.section && section->axis == r.section->axis &&
        section->coordinate == r.section->coordinate && section->keepPositive == r.section->keepPositive)
        return;

    if (section) {
        const auto axis = static_cast<int>(section->axis);
        ccxvis::Point origin{}, normal{};
        origin[axis] = section->coordinate;
        normal[axis] = section->keepPositive ? 1. : -1.;
        r.sectionPlane->SetOrigin(origin.data());
        r.sectionPlane->SetNormal(normal.data());
    }
    if (section.has_value() != r.section.has_value()) {
        if (section && !r.sectionGrid->GetPoints()) buildSectionMesh();
        r.surface->SetInputData(section ? r.sectionGrid.GetPointer() : r.grid.GetPointer());
        for (auto* mapper : {r.mapper.GetPointer(), r.edgeMapper.GetPointer(),
                             r.featureMapper.GetPointer(), r.pickedMapper.GetPointer()}) {
            if (section) mapper->AddClippingPlane(r.sectionPlane);
            else mapper->RemoveClippingPlane(r.sectionPlane);
        }
    }
    r.section = section;
    applyVisualization();
    redraw();
}

std::array<double, 2> Viewport::sectionBounds(SectionAxis axis) const
{
    const auto* bounds = render_->grid->GetBounds();
    const int index = 2 * static_cast<int>(axis);
    return {bounds[index], bounds[index + 1]};
}

bool Viewport::sectionIntersectsMesh()
{
    auto& r = *render_;
    if (!r.section) return false;
    r.sectionCut->Update();
    return r.sectionCut->GetOutput()->GetNumberOfCells() > 0;
}

void Viewport::fit()
{
    render_->renderer->ResetCamera();
    redraw();
}

void Viewport::setOrthogonal(bool enabled)
{
    render_->renderer->GetActiveCamera()->SetParallelProjection(enabled);
    redraw();
}

void Viewport::view(const ccxvis::Point& direction, const ccxvis::Point& up)
{
    auto* camera = render_->renderer->GetActiveCamera();
    const auto* center = camera->GetFocalPoint();
    camera->SetPosition(center[0] + direction[0], center[1] + direction[1], center[2] + direction[2]);
    camera->SetViewUp(up.data());
    camera->OrthogonalizeViewUp();
    fit();
}

void Viewport::setProbing(bool active)
{
    render_->probing = active;
    setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
}

void Viewport::onNodePicked(std::function<void(std::optional<std::size_t>)> callback)
{
    render_->onPick = std::move(callback);
}

bool Viewport::pickGeometry(int x, int y)
{
    auto& r = *render_;
    if (r.visualization != Visualization::Edge) r.surface->Update();
    if (r.sectionActor->GetVisibility()) r.sectionCut->Update();
    return r.picker->Pick(x, y, 0., r.renderer) != 0;
}

std::optional<std::size_t> Viewport::pickAt(int x, int y)
{
    if (!pickGeometry(x, y)) return std::nullopt;
    auto& r = *render_;
    if (r.picker->GetActor() == r.sectionActor.GetPointer()) return pickSectionNode(x, y);

    const bool wire = r.visualization == Visualization::Edge;
    const auto id = r.picker->GetPointId();
    auto* data = wire ? r.wireEdges.GetPointer() : r.surface->GetOutput();
    if (id < 0 || id >= data->GetNumberOfPoints()) return std::nullopt;
    auto original = id;
    if (!wire) {
        auto* ids = vtkIdTypeArray::SafeDownCast(data->GetPointData()->GetArray(originalPoints));
        if (!ids) return std::nullopt;
        original = ids->GetValue(id);
    }

    // A hit on a face can otherwise select a corner far from the pointer.
    if (screenDistanceToPoint(r.renderer, data->GetPoint(id), x, y) > 12. * devicePixelRatioF())
        return std::nullopt;
    if (original < 0 || original >= r.grid->GetNumberOfPoints()) return std::nullopt;
    if (r.section && r.sectionPlane->EvaluateFunction(r.grid->GetPoint(original)) < 0.)
        return std::nullopt;
    return static_cast<std::size_t>(original);
}

std::optional<std::size_t> Viewport::pickSectionNode(int x, int y)
{
    auto& r = *render_;
    auto* cells = r.sectionCut->GetOutput()->GetCellData();
    auto* originals = vtkIdTypeArray::SafeDownCast(cells->GetArray(sectionSourceCells));
    const auto cutCell = r.picker->GetCellId();
    if (!r.section || !originals || cutCell < 0 || cutCell >= originals->GetNumberOfValues())
        return std::nullopt;
    const auto originalCell = originals->GetValue(cutCell);
    if (originalCell < 0 || originalCell >= r.grid->GetNumberOfCells()) return std::nullopt;

    vtkIdType count = 0;
    const vtkIdType* nodes = nullptr;
    r.grid->GetCellPoints(originalCell, count, nodes);
    const int axis = static_cast<int>(r.section->axis);
    const auto bounds = sectionBounds(r.section->axis);
    const double extent = std::max(std::abs(bounds[0]), std::abs(bounds[1]));
    const double tolerance = 8. * std::numeric_limits<double>::epsilon() * extent;
    double bestDistance = 12. * devicePixelRatioF();
    std::optional<std::size_t> nearest;
    // Only true nodes of the element that generated the cut are eligible.
    for (vtkIdType i = 0; i < count; ++i) {
        const auto* point = r.grid->GetPoint(nodes[i]);
        if (std::abs(point[axis] - r.section->coordinate) > tolerance) continue;
        const auto distance = screenDistanceToPoint(r.renderer, point, x, y);
        if (distance <= bestDistance) {
            bestDistance = distance;
            nearest = static_cast<std::size_t>(nodes[i]);
        }
    }
    return nearest;
}

void Viewport::markNode(std::optional<std::size_t> point)
{
    auto& r = *render_;
    r.highlight->SetVisibility(point.has_value());
    if (point) {
        vtkNew<vtkPoints> points;
        points->SetDataTypeToDouble();
        points->InsertNextPoint(r.grid->GetPoint(*point));
        vtkNew<vtkCellArray> vertices;
        vertices->InsertNextCell(1);
        vertices->InsertCellPoint(0);
        r.pickedPoint->SetPoints(points);
        r.pickedPoint->SetVerts(vertices);
    }
    redraw();
}

QSize Viewport::saveImage(const QString& path, const QSize& outputSize)
{
    QImage image = grabFramebuffer();
    if (!image.isNull() && outputSize.isValid() && image.size() != outputSize) {
        // Keep the encoded frame size stable even if the screen DPI or viewport size changes.
        auto fitted = image.scaled(outputSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        fitted.setDevicePixelRatio(1);
        image = QImage(outputSize, QImage::Format_RGB32);
        image.fill(QColor::fromRgbF(0.075, 0.10, 0.15));
        QPainter painter(&image);
        painter.drawImage((image.width() - fitted.width()) / 2, (image.height() - fitted.height()) / 2, fitted);
    }
    QSaveFile file(path);
    if (image.isNull() || !file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit())
        throw std::runtime_error("Could not save viewport PNG");
    return image.size();
}

void Viewport::redraw()
{
    if (isValid()) render_->window->Render();
    update();
}
