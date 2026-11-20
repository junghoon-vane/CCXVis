// SPDX-License-Identifier: GPL-2.0-only
#include "MainWindow.h"
#include "CurvePlot.h"
#include "Viewport.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMenuBar>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QListWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QSurfaceFormat>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkActor2DCollection.h>
#include <vtkAxesActor.h>
#include <vtkCell.h>
#include <vtkLookupTable.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProp.h>
#include <vtkPropCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

vtkScalarBarActor* contourLegend(Viewport& viewport)
{
    auto* actors = viewport.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors2D();
    actors->InitTraversal();
    while (auto* actor = actors->GetNextActor2D()) {
        if (auto* legend = vtkScalarBarActor::SafeDownCast(actor)) return legend;
    }
    return nullptr;
}

template<class Predicate>
void await(Predicate done, const char* message = "Timed out waiting for the results workflow")
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < 10000) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(done(), message);
}

void testQuadraticProbe()
{
    Viewport viewport;
    viewport.renderWindow()->SetSize(600, 600);
    ccxvis::Mesh mesh;
    mesh.nodes = {{10, {0, 0, 0}}, {11, {1, 0, 0}}, {12, {0, 1, 0}},
                  {99, {0.5, 0, 0}}, {14, {0.5, 0.5, 0}}, {15, {0, 0.5, 0}}};
    mesh.elements = {{1, 8, {0, 1, 2, 3, 4, 5}}};
    viewport.setMesh(mesh);
    viewport.view({0, 0, 1}, {0, 1, 0});
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    renderer->SetWorldPoint(0.5, 0.002, 0., 1.);
    renderer->WorldToDisplay();
    const auto* pixel = renderer->GetDisplayPoint();
    const auto point = viewport.pickAt(std::lround(pixel[0]), std::lround(pixel[1]));
    require(point && mesh.nodes[*point].id == 99, "Midside probing preserves the original node ID");
    viewport.setVisualization(Viewport::Visualization::Edge);
    const auto wirePoint = viewport.pickAt(std::lround(pixel[0]), std::lround(pixel[1]));
    require(wirePoint && mesh.nodes[*wirePoint].id == 99, "Wireframe probing uses the original node ID");
}

void testRotationCenter()
{
    Viewport viewport;
    constexpr int side = 600;
    viewport.resize(side, side);
    const auto ratio = viewport.effectiveDevicePixelRatio();
    const int physicalSize = qRound(side * ratio);
    viewport.renderWindow()->SetSize(physicalSize, physicalSize);
    auto* menu = viewport.findChild<QMenu*>("viewportContextMenu");
    require(menu && menu->actions().size() == 1 && menu->actions().front()->text() == "Set Rotation Center",
            "The viewport has a Set Rotation Center menu item");

    const auto mouseEvent = [&](QEvent::Type type, QPoint position, Qt::MouseButton button,
                                Qt::MouseButtons held) {
        QMouseEvent event(type, QPointF(position), QPointF(viewport.mapToGlobal(position)),
                          button, held, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &event);
    };
    const auto rightClick = [&](QPoint position) {
        mouseEvent(QEvent::MouseButtonPress, position, Qt::RightButton, Qt::RightButton);
        mouseEvent(QEvent::MouseButtonRelease, position, Qt::RightButton, Qt::NoButton);
    };
    rightClick({100, 100});
    require(!menu->isVisible(), "The context menu is unavailable on an empty viewport");

    ccxvis::Mesh mesh;
    mesh.nodes = {{1, {0, 0, 0}}, {2, {1, 0, 2}}, {3, {0, 1, 0}}};
    mesh.elements = {{1, 7, {0, 1, 2}}};
    viewport.setMesh(mesh);
    viewport.view({0, 0, 1}, {0, 1, 0});
    viewport.setPositions({{0, 0, 1}, {1, 0, 3}, {0, 1, 1}});
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* camera = renderer->GetActiveCamera();
    const auto scale = camera->GetParallelScale();
    const auto distance = camera->GetDistance();
    ccxvis::Point before;
    camera->GetFocalPoint(before.data());
    renderer->SetWorldPoint(0.25, 0.3, 1.5, 1.);
    renderer->WorldToDisplay();
    const auto* pixel = renderer->GetDisplayPoint();
    const QPoint click(qRound(pixel[0] / ratio), qRound((physicalSize - 1 - pixel[1]) / ratio));

    mouseEvent(QEvent::MouseButtonPress, click, Qt::RightButton, Qt::RightButton);
    const auto drag = click + QPoint(QApplication::startDragDistance() + 2, 0);
    mouseEvent(QEvent::MouseMove, drag, Qt::NoButton, Qt::RightButton);
    mouseEvent(QEvent::MouseButtonRelease, drag, Qt::RightButton, Qt::NoButton);
    require(!menu->isVisible(), "Dragging with the right button does not open the menu");

    rightClick(click);
    require(menu->isVisible() && camera->GetFocalPoint()[0] == before[0],
            "Right click opens the menu without changing the rotation center");
    menu->actions().front()->trigger();
    menu->hide();
    const auto* center = camera->GetFocalPoint();
    require(std::abs(center[0] - 0.25) < 0.01 && std::abs(center[1] - 0.3) < 0.01 &&
                std::abs(center[2] - 1.5) < 0.01, "The rotation center is on the displayed deformed surface");
    require(std::abs(camera->GetDistance() - distance) < 1e-8 &&
                camera->GetParallelScale() == scale && camera->GetDirectionOfProjection()[2] < -0.999,
            "The new center preserves viewing direction and zoom");

    const auto surfaceDepth = center[2];
    rightClick({side / 10, side - side / 10});
    require(menu->isVisible(), "The triad corner opens the rotation center menu");
    menu->actions().front()->trigger();
    menu->hide();
    require(std::abs(camera->GetFocalPoint()[2] - surfaceDepth) < 1e-8,
            "A background click uses the current focal plane");
    viewport.fit();
    require(std::abs(camera->GetFocalPoint()[0] - 0.5) < 1e-8, "Fit restores the model center");
}

void testVisualization()
{
    Viewport viewport;
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    ccxvis::Mesh mesh;
    mesh.nodes = {{1, {0, 0, 0}}, {2, {1, 0, 0}}, {3, {0, 1, 0}}, {4, {1, 1, 0}},
                  {5, {0.5, 0, 0}}, {6, {0.5, 0.5, 0}}, {7, {0, 0.5, 0}},
                  {8, {1, 0.5, 0}}, {9, {0.5, 1, 0}}};
    mesh.elements = {{1, 8, {0, 1, 2, 4, 5, 6}}, {2, 8, {2, 1, 3, 5, 7, 8}}};
    viewport.setMesh(mesh);

    auto* actors = renderer->GetActors();
    auto* surface = actors->GetItemAsObject(0);
    const auto visibleLines = [&] {
        std::vector<vtkIdType> counts;
        actors->InitTraversal();
        while (auto* actor = actors->GetNextActor()) {
            if (actor == surface || !actor->GetVisibility()) continue;
            auto* mapper = vtkPolyDataMapper::SafeDownCast(actor->GetMapper());
            mapper->Update();
            counts.push_back(mapper->GetInput()->GetNumberOfLines());
        }
        return counts;
    };
    using Mode = Viewport::Visualization;
    require(visibleLines().empty(), "Surface remains the default");
    viewport.setResult(std::vector<double>(mesh.nodes.size(), 2.), "Result");
    viewport.setVisualization(Mode::Edge);
    auto* edgeActor = vtkActor::SafeDownCast(actors->GetItemAsObject(1));
    auto* mapper = vtkPolyDataMapper::SafeDownCast(edgeActor->GetMapper());
    require(mapper->GetScalarVisibility() && mapper->GetInput()->GetPointData()->GetScalars(),
            "Wireframe retains result colors");
    require(!vtkActor::SafeDownCast(surface)->GetVisibility() &&
                visibleLines() == std::vector<vtkIdType>{5}, "Edge shows the original wireframe");
    viewport.setVisualization(Mode::Mesh);
    require(vtkActor::SafeDownCast(surface)->GetVisibility() &&
                visibleLines() == std::vector<vtkIdType>{5}, "Mesh overlays finite-element edges");
    viewport.setVisualization(Mode::FeaturedEdge);
    require(visibleLines() == std::vector<vtkIdType>{4}, "Coplanar seam is excluded from feature edges");
    viewport.setVisualization(Mode::FeaturedEdgeWithMesh);
    require(visibleLines() == std::vector<vtkIdType>({5, 4}), "Combined display shows both layers");

    auto* data = mapper->GetInput();
    require(!mapper->GetScalarVisibility(), "The overlay has a neutral color");
    auto* firstEdge = data->GetCell(0);
    require(firstEdge->GetNumberOfPoints() == 3 && firstEdge->GetPointId(1) == 4,
            "The quadratic edge path includes its midside node");
    auto positions = std::vector<ccxvis::Point>(mesh.nodes.size());
    for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = mesh.nodes[i].position;
    positions[4][2] = 0.1;
    viewport.setPositions(positions);
    require(std::abs(data->GetPoint(4)[2] - 0.1) < 1e-8, "Edges follow deformed midside nodes");

    for (int i : {3, 7, 8}) {
        auto& point = mesh.nodes[i].position;
        point[2] = point[0] + point[1] - 1;
    }
    viewport.setMesh(mesh);
    require(visibleLines() == std::vector<vtkIdType>({5, 5}), "A sharp shared edge is a feature");
}

int main(int argc, char** argv)
try {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    require(argc == 2 || argc == 3, "Supply result.frd and optionally a render output path");
    testQuadraticProbe();
    testVisualization();
    testRotationCenter();
    MainWindow window;
    const bool render = argc == 3;
    if (render) window.show();

    QStringList menuTitles;
    for (auto* action : window.menuBar()->actions())
        menuTitles << action->text().remove('&');
    require(menuTitles == QStringList{"File", "View", "Result", "Plot", "Options", "Tools", "Animate"},
            "The main menu bar follows the results workflow");

    auto* frames = window.findChild<QComboBox*>("frames");
    auto* file = window.findChild<QMenu*>("fileMenu");
    auto* result = window.findChild<QMenu*>("resultMenu");
    auto* fields = window.findChild<QMenu*>("fieldsMenu");
    auto* fieldButton = window.findChild<QToolButton*>("fieldOutputButton");
    auto* tools = window.findChild<QMenu*>("toolsMenu");
    auto* toolbar = window.findChild<QToolBar*>("resultsToolbar");
    auto* viewMenu = window.findChild<QMenu*>("viewMenu");
    auto* plot = window.findChild<QMenu*>("plotMenu");
    auto* options = window.findChild<QMenu*>("optionsMenu");
    auto* deformation = window.findChild<QAction*>("deformation");
    auto* scale = window.findChild<QDoubleSpinBox*>("deformationScale");
    auto* viewport = dynamic_cast<Viewport*>(window.findChild<QWidget*>("viewport"));
    require(frames && file && result && fields && fieldButton && viewMenu && plot && tools && options &&
                toolbar && deformation && scale && viewport,
            "Viewer controls are present");
    require(window.centralWidget() == viewport, "The viewport fills the main area");
    require(window.windowTitle() == "CCXVis", "The window starts with the CCXVis title");
    const auto menuAction = [](QMenu* menu, const QString& name) -> QAction* {
        for (auto* action : menu->actions()) {
            if (action->text() == name) return action;
        }
        throw std::runtime_error("Missing menu action " + name.toStdString());
    };
    auto* open = menuAction(file, "Open FRD…");
    auto* exportPng = menuAction(file, "Export PNG…");
    auto* stepFrame = menuAction(result, "Step/Frame…");
    auto* fit = menuAction(viewMenu, "Fit");
    auto* orthogonal = menuAction(viewMenu, "Orthogonal");
    require(open->isEnabled() && !exportPng->isEnabled() && !fit->isEnabled() &&
                !stepFrame->isEnabled() && !frames->isEnabled() && !fields->menuAction()->isEnabled() &&
                !fieldButton->isEnabled(), "Commands that need a result start disabled");
    require(open->shortcut() == QKeySequence(QKeySequence::Open) && fit->shortcut() == QKeySequence(Qt::Key_F) &&
                exportPng->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
            "The File and View menus retain the existing keyboard shortcuts");
    require(plot->actions().contains(deformation) && options->isAncestorOf(scale),
            "Plot contains the deformation toggle and Options contains its scale");
    require(result->actions().contains(fields->menuAction()) && fields->title() == "Field Output" &&
                fieldButton->menu() == fields && toolbar->isAncestorOf(fieldButton),
            "Result and the quick toolbar share the Field Output menu");
    require(!toolbar->actions().contains(open), "Open FRD is available from the File menu");
    require(toolbar->isAncestorOf(frames) && toolbar->actions().contains(fit),
            "The top toolbar keeps shortcuts for fitting and selecting a frame");
    {
        auto* xyPlot = menuAction(tools, "XY Plot…");
        require(xyPlot->isEnabled(), "XY Plot is available before an FRD is opened");
        QTemporaryFile csv(QDir::tempPath() + "/ccxvis-plot-XXXXXX.csv");
        require(csv.open() && csv.write("Time,Force,Travel\n0,1,2\n1,2,1\n") > 0 && csv.flush(),
                "Write a curve fixture");
        openCurvePlot(&window, csv.fileName());
        auto* chart = window.findChild<QDialog*>("xyPlotWindow");
        require(chart && chart->isVisible() && !chart->isModal(), "A curve opens in a nonmodal plot window");
        auto* x = chart->findChild<QComboBox*>("xyPlotX");
        auto* y = chart->findChild<QListWidget*>("xyPlotY");
        require(x && x->currentText() == "Time" && y && y->count() == 3 &&
                y->item(1)->checkState() == Qt::Checked, "The first column is X and the second is initially Y");
        y->item(2)->setCheckState(Qt::Checked);
        require(y->item(1)->checkState() == Qt::Checked && y->item(2)->checkState() == Qt::Checked &&
                !chart->grab().isNull(), "Two curves can be drawn without a viewport context");
        openCurvePlot(&window, csv.fileName());
        require(window.findChildren<QDialog*>("xyPlotWindow").size() == 1, "Reuse the plot window for the same file");
        chart->close();
    }
    window.openFile(QString::fromLocal8Bit(argv[1]));
    await([&] { return deformation->isEnabled(); });
    require(exportPng->isEnabled() && stepFrame->isEnabled() && frames->isEnabled() &&
                fields->menuAction()->isEnabled() && fieldButton->isEnabled(), "Opening results enables both entry points");
    bool choseFrame = false;
    QTimer::singleShot(0, &window, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            choseFrame = dialog->comboBoxItems().contains(frames->itemText(2));
            dialog->setTextValue(frames->itemText(2));
            dialog->accept();
        }
    });
    stepFrame->trigger();
    require(choseFrame && frames->currentIndex() == 2, "Step/Frame changes the same selection as the toolbar");
    frames->setCurrentIndex(0);
    await([&] { return deformation->isEnabled(); });
    const auto orientationAxes = [viewport]() -> vtkAxesActor* {
        auto* renderers = viewport->renderWindow()->GetRenderers();
        if (renderers->GetNumberOfItems() != 2) return nullptr;
        auto* overlay = vtkRenderer::SafeDownCast(renderers->GetItemAsObject(1));
        return overlay ? vtkAxesActor::SafeDownCast(overlay->GetViewProps()->GetItemAsObject(0)) : nullptr;
    };
    const auto triadVisible = [&] {
        auto* axes = orientationAxes();
        return axes && axes->GetVisibility();
    };
    require(triadVisible(), "Opening an FRD displays the orientation triad");
    require(menuAction(plot, "Surface")->isChecked() && menuAction(plot, "Mesh")->isCheckable() &&
                menuAction(plot, "Featured Edge")->isCheckable(), "Plot offers the existing display modes");
    require(window.windowTitle() == QFileInfo(QString::fromLocal8Bit(argv[1])).fileName() + " — CCXVis",
            "The title bar shows the opened filename and the CCXVis name");
    require(orthogonal->isChecked(), "View starts with orthogonal projection");
    auto* camera = viewport->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera();
    require(camera->GetParallelProjection(), "The initial projection is orthogonal");
    orthogonal->trigger();
    require(!camera->GetParallelProjection(), "Orthogonal off uses perspective");
    orthogonal->trigger();
    require(camera->GetParallelProjection(), "Orthogonal on uses parallel projection");
    const auto fittedScale = camera->GetParallelScale();
    camera->SetParallelScale(fittedScale / 2);
    fit->trigger();
    require(std::abs(camera->GetParallelScale() - fittedScale) < 1e-8, "Fit works from View");

    auto* sectionAction = window.findChild<QAction*>("sectionView");
    auto* sectionControls = window.findChild<QToolBar*>("sectionControls");
    auto* sectionAxis = window.findChild<QComboBox*>("sectionAxis");
    auto* coordinate = window.findChild<QLineEdit*>("sectionCoordinate");
    auto* position = window.findChild<QSlider*>("sectionPosition");
    auto* flip = window.findChild<QToolButton*>("sectionFlip");
    auto* probeAction = menuAction(tools, "Probe");
    require(sectionAction && tools->actions().contains(sectionAction), "Section View is in Tools");
    require(sectionControls && sectionAxis && coordinate && position && flip &&
                window.toolBarArea(sectionControls) == Qt::BottomToolBarArea &&
                sectionControls->isHidden() && !sectionAction->isChecked(), "Section starts closed");
    const auto menus = window.menuBar()->actions();
    for (auto* bar : {toolbar, sectionControls}) {
        for (auto* label : bar->findChildren<QLabel*>()) {
            const auto key = QKeySequence::mnemonic(label->text());
            if (key.isEmpty()) continue;
            for (auto* menu : menus)
                require(QKeySequence::mnemonic(menu->text()) != key, "Toolbar labels leave menu mnemonics available");
        }
    }

    sectionAction->trigger();
    require(!sectionControls->isHidden() && sectionAxis->currentIndex() == 0 &&
                !flip->isChecked() && viewport->sectionIntersectsMesh(), "Section starts with an X midpoint cut");
    require(triadVisible(), "The orientation triad stays visible during a section cut");
    auto* palettes = options->findChild<QMenu*>("colormapMenu");
    auto* colorScale = options->findChild<QMenu*>("colorScaleMenu");
    auto* colorSteps = options->findChild<QSpinBox*>("colorSteps");
    require(palettes && colorScale && colorSteps && colorScale->isAncestorOf(colorSteps),
            "Options contains colormap, color scale, and step controls");
    auto* rainbow = menuAction(palettes, "Rainbow");
    auto* continuous = menuAction(colorScale, "Continuous");
    auto* discrete = menuAction(colorScale, "Discrete");
    require(rainbow->isChecked() && continuous->isChecked() && !colorSteps->isEnabled(),
            "Rainbow and Continuous are the defaults and steps start disabled");
    auto* legend = contourLegend(*viewport);
    require(legend && legend->GetVisibility(), "A contour legend is visible during a section cut");
    auto* colors = vtkLookupTable::SafeDownCast(legend->GetLookupTable());
    require(colors && colors->GetNumberOfTableValues() == 256, "Continuous uses a smooth lookup table");
    const std::array<double, 2> resultRange{colors->GetRange()[0], colors->GetRange()[1]};
    const auto rainbowBlue = colors->GetTableValue(0)[2];
    for (const auto& [name, lowBlue] : std::array<std::pair<const char*, double>, 6>{
            {{"Inferno", .014}, {"Jet", .5}, {"Cool-to-Warm", .753},
             {"Plasma", .528}, {"Rainbow", rainbowBlue}, {"Viridis", .329}}}) {
        auto* preset = menuAction(palettes, name);
        preset->trigger();
        require(preset->isChecked() && std::abs(colors->GetTableValue(0)[2] - lowBlue) < .01,
                "Each preset applies its palette to the active legend");
    }
    require(!rainbow->isChecked() && std::equal(resultRange.begin(), resultRange.end(), colors->GetRange()),
            "Changing the palette preserves the active result range");
    discrete->trigger();
    require(discrete->isChecked() && !continuous->isChecked() && colorSteps->isEnabled() &&
                colorSteps->value() == 11 && legend->GetMaximumNumberOfColors() == 11 &&
                colors->GetNumberOfTableValues() == 11, "Discrete gives the legend and model eleven bands");
    const auto bin = [&](double fraction) {
        return colors->GetIndex(resultRange[0] + fraction * (resultRange[1] - resultRange[0]));
    };
    for (int i = 0; i < 11; ++i)
        require(bin((i + 0.25) / 11.) == i && bin((i + 0.75) / 11.) == i,
                "Each equal-width interval maps to one discrete color");
    const auto checkContourMappers = [&](int expected) {
        auto* actors = viewport->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
        actors->InitTraversal();
        int contourCount = 0;
        while (auto* actor = actors->GetNextActor()) {
            auto* mapper = actor->GetMapper();
            if (mapper && mapper->GetScalarVisibility() && actor->GetVisibility()) {
                require(mapper->GetLookupTable() == colors && mapper->GetInterpolateScalarsBeforeMapping(),
                        "Visible contours map interpolated values through the legend's bands");
                ++contourCount;
            }
        }
        require(contourCount == expected, "The expected actors show the contour");
    };
    checkContourMappers(2);
    menuAction(plot, "Edge")->trigger();
    checkContourMappers(1);
    menuAction(plot, "Surface")->trigger();
    colorSteps->setValue(7);
    require(legend->GetMaximumNumberOfColors() == 7 && colors->GetNumberOfTableValues() == 7,
            "The step control updates both color scales immediately");
    continuous->trigger();
    require(!colorSteps->isEnabled() && colors->GetNumberOfTableValues() == 256,
            "Continuous restores smooth colors while retaining the step setting");
    discrete->trigger();
    require(colorSteps->value() == 7 && legend->GetMaximumNumberOfColors() == 7,
            "Returning to Discrete restores the chosen number of steps");
    sectionAxis->setCurrentIndex(1);
    using Axis = Viewport::SectionAxis;
    const auto yBounds = viewport->sectionBounds(Axis::Y);
    const auto yMid = (yBounds[0] + yBounds[1]) / 2;
    require(std::abs(coordinate->text().toDouble() - yMid) < 1e-8, "Changing axis uses its midpoint");

    const auto getPlane = [&] {
        auto* actors = viewport->renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
        actors->InitTraversal();
        auto* planes = actors->GetNextActor()->GetMapper()->GetClippingPlanes();
        return planes && planes->GetNumberOfItems() ? planes->GetItem(0) : nullptr;
    };
    position->setValue(position->maximum() / 4);
    position->setValue(position->maximum() / 3);
    const auto pendingPosition = 0.375 * (yBounds[1] - yBounds[0]) + yBounds[0];
    const auto latest = qRound(0.375 * position->maximum());
    position->setValue(latest);
    await([&] { return getPlane() && std::abs(getPlane()->GetOrigin()[1] - pendingPosition) < 0.01; });
    const auto requested = 0.6 * (yBounds[1] - yBounds[0]) + yBounds[0];
    const auto enterCoordinate = [&](double value) {
        coordinate->setText(QString::number(value, 'g', 16));
        QMetaObject::invokeMethod(coordinate, "editingFinished");
    };
    enterCoordinate(yBounds[1] + 100.);
    require(getPlane() && getPlane()->GetOrigin()[1] == yBounds[1],
            "An out-of-range coordinate is clamped to the displayed bounds");
    enterCoordinate(requested);
    require(getPlane() && std::abs(getPlane()->GetOrigin()[1] - requested) < 1e-8,
            "An entered coordinate positions the plane");
    coordinate->setText("unavailable");
    QMetaObject::invokeMethod(coordinate, "editingFinished");
    require(std::abs(coordinate->text().toDouble() - requested) < 1e-8,
            "An invalid entry restores the current coordinate");
    flip->click();
    require(getPlane()->GetNormal()[1] == -1., "Flip Side switches the kept side");
    sectionAction->trigger();
    require(sectionControls->isHidden() && !getPlane(), "Turning off the section restores the full model");
    sectionAction->trigger();
    require(!sectionControls->isHidden() && getPlane() && flip->isChecked() &&
                std::abs(getPlane()->GetOrigin()[1] - requested) < 1e-8,
            "Reopening on the same model restores section settings");
    auto* probeReadout = window.findChild<QLabel*>("probeValue");
    probeAction->trigger();
    require(probeReadout && window.statusBar()->isAncestorOf(probeReadout) &&
                probeAction->isChecked() && !probeReadout->text().isEmpty(), "Probe works from the status bar");
    probeAction->trigger();
    require(!probeAction->isChecked() && probeReadout->text().isEmpty(), "Probe can be turned off from Tools");
    auto* combined = menuAction(plot, "Featured Edge with Mesh");
    combined->trigger();
    require(combined->isChecked() && !menuAction(plot, "Surface")->isChecked(),
            "Plot selects the combined mode");
    const auto selectedField = [fields] {
        for (auto* group : fields->actions())
            for (auto* component : group->menu()->actions())
                if (component->isChecked()) return component->text();
        return QString{};
    };
    require(frames->count() == 10 && selectedField() == "Magnitude", "Initial modal selection");
    frames->setCurrentIndex(2);
    frames->setCurrentIndex(5);
    frames->setCurrentIndex(9);
    require(fields->actions().size() == 2, "Last mode fields");
    auto* mises = fields->actions().at(1)->menu()->actions().front();
    require(mises->text() == "von Mises", "Stress invariant in the Field menu");
    mises->trigger();
    await([&] { return deformation->isEnabled(); });
    require(frames->currentIndex() == 9 && selectedField() == "von Mises",
            "Latest frame and field are selected after rapid changes");
    require(fieldButton->text() == "Field · von Mises" && fields->title() == "Field Output",
            "The toolbar shows the selected component and the Result menu keeps its command label");
    require(getPlane() && std::abs(getPlane()->GetOrigin()[1] - requested) < 1e-8,
            "Field and frame changes preserve the section coordinate");
    require(!scale->isEnabled(), "Scale is disabled while deformation is off");
    deformation->trigger();
    require(deformation->isChecked() && scale->isEnabled(), "Options enables scale with deformation");
    const auto checkSectionClamp = [&](double previous) {
        const auto bounds = viewport->sectionBounds(Axis::Y);
        require(std::abs(getPlane()->GetOrigin()[1] - std::clamp(previous, bounds[0], bounds[1])) < 1e-8,
                "Deformation keeps the section coordinate within the displayed bounds");
    };
    checkSectionClamp(requested);
    const auto afterWarp = getPlane()->GetOrigin()[1];
    scale->setValue(2);
    checkSectionClamp(afterWarp);
    const auto afterScale = getPlane()->GetOrigin()[1];
    deformation->trigger();
    checkSectionClamp(afterScale);
    require(!deformation->isChecked() && !scale->isEnabled(), "Options can turn deformation off");

    QTimer dismiss;
    dismiss.setInterval(20);
    QObject::connect(&dismiss, &QTimer::timeout, &window, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* warning = qobject_cast<QMessageBox*>(widget)) {
                warning->accept();
                dismiss.stop();
            }
        }
    });
    dismiss.start();
    window.openFile(QString::fromLocal8Bit(argv[1]) + ".does-not-exist");
    await([&] { return window.statusBar()->currentMessage().contains("could not open"); });
    require(frames->currentIndex() == 9 && selectedField() == "von Mises" && getPlane(),
            "Failed open preserves the displayed result and section");

    QFile original(QString::fromLocal8Bit(argv[1]));
    require(original.open(QIODevice::ReadOnly), "Open upstream fixture for mutation");
    const auto originalContents = original.readAll();
    auto contents = originalContents;
    const auto field = contents.indexOf(" -4  DISP");
    const auto payload = contents.indexOf("\n -1", field);
    require(field >= 0 && payload >= 0, "Find first displacement value");
    contents.replace(payload + 14, 12, " UNAVAILABLE");
    QTemporaryFile malformed;
    require(malformed.open() && malformed.write(contents) == contents.size() && malformed.flush(),
            "Write a fixture with a bad displacement value");
    dismiss.start();
    window.openFile(malformed.fileName());
    await([&] { return window.statusBar()->currentMessage().contains("invalid number"); });
    require(frames->currentIndex() == 9 && selectedField() == "von Mises",
            "Invalid initial data preserves the previous results");

    QTemporaryFile replacement;
    require(replacement.open() && replacement.write(originalContents) == originalContents.size() &&
                replacement.flush(), "Write a replacement FRD");
    window.openFile(replacement.fileName());
    await([&] { return frames->currentIndex() == 0 && deformation->isEnabled(); });
    require(!sectionAction->isChecked() && sectionControls->isHidden() && !getPlane(),
            "A successful open resets the section for the new document");
    require(triadVisible(), "Loading a replacement FRD preserves a single visible orientation triad");
    colors->Build();
    require(discrete->isChecked() && colorSteps->value() == 7 && legend->GetMaximumNumberOfColors() == 7 &&
                colors->GetNumberOfTableValues() == 7 && std::abs(colors->GetTableValue(0)[2] - .329) < .01,
            "A replacement FRD retains the palette and band count");

    if (render) {
        await([&] { return viewport->isValid(); }, "No OpenGL context for the PNG rendering test");
        sectionAction->trigger();
        QApplication::processEvents();
        viewport->view({-1, 0, 0}, {0, 0, 1});
        require(viewport->sectionIntersectsMesh(), "The PNG export view has a filled section");
        require(legend && legend->GetVisibility(), "The contour legend is visible on the section view");
        const auto checkOverlayPixels = [&](const QImage& image, vtkProp* prop, const QRect& region,
                                            const char* message) {
            prop->VisibilityOff();
            viewport->renderWindow()->Render();
            const auto withoutOverlay = viewport->grabFramebuffer();
            prop->VisibilityOn();
            viewport->renderWindow()->Render();
            require(image.size() == withoutOverlay.size() && image.copy(region).convertToFormat(QImage::Format_RGB32) !=
                        withoutOverlay.copy(region).convertToFormat(QImage::Format_RGB32), message);
        };
        const auto lowerLeft = [](const QImage& image) {
            return QRect(0, 3 * image.height() / 4, image.width() / 4, image.height() / 4);
        };
        const auto path = QString::fromLocal8Bit(argv[2]);
        viewport->saveImage(path);
        const QImage image(path);
        require(!image.isNull() && image.width() > 100 && image.height() > 100, "Viewport PNG is readable");
        auto* axes = orientationAxes();
        require(axes && axes->GetVisibility(), "The orientation triad is visible on the section view");
        checkOverlayPixels(image, axes, lowerLeft(image), "The PNG includes the lower-left orientation triad");
        const QRect right(3 * image.width() / 4, 0, image.width() / 4, image.height());
        checkOverlayPixels(image, legend, right, "The PNG includes the contour legend");
        viewport->setSection(std::nullopt);
        const auto exterior = viewport->grabFramebuffer();
        require(image.size() == exterior.size() &&
                    image.convertToFormat(QImage::Format_RGB32) != exterior.convertToFormat(QImage::Format_RGB32),
                "Export pixels reflect the section view");
        const auto originalSize = viewport->size();
        window.resize(800, 600);
        await([&] { return viewport->size() != originalSize; });
        const auto resized = viewport->grabFramebuffer();
        checkOverlayPixels(resized, axes, lowerLeft(resized), "The resized viewport still draws the triad");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
