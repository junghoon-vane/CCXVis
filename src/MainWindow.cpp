// SPDX-License-Identifier: GPL-2.0-only
#include "MainWindow.h"
#include "CurvePlot.h"
#include "Viewport.h"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {
constexpr int magnitude = ccxvis::magnitudeComponent;
constexpr int mises = ccxvis::misesComponent;
constexpr int sectionSteps = 1000;
using SectionAxis = Viewport::SectionAxis;
constexpr std::array sectionAxes{SectionAxis::X, SectionAxis::Y, SectionAxis::Z};

double sectionMidpoint(const std::array<double, 2>& bounds)
{
    return 0.5 * bounds[0] + 0.5 * bounds[1];
}

QString frameLabel(const ccxvis::Frame& frame, int ordinal)
{
    QString label;
    if (frame.mode > 0)
        label = QString("Mode %1").arg(frame.mode);
    else if (frame.step > 0 || frame.increment > 0)
        label = QString("Step %1 · Increment %2").arg(frame.step).arg(frame.increment);
    else
        label = QString("Frame %1").arg(ordinal + 1);
    if (frame.diameter >= 0) label += QString(" · ND %1").arg(frame.diameter);
    if (frame.subcase) label += QString(" · Subcase %1").arg(frame.subcase);
    if (frame.mode || frame.analysis == 1 || frame.parameter != 0)
        label += QString("  (%1)").arg(frame.parameter, 0, 'g', 8);
    return label;
}

QString fieldName(const std::string& name)
{
    if (name == "DISP") return "Displacement";
    if (name == "STRESS") return "Stress";
    if (name == "NDTEMP") return "Temperature";
    if (name == "TOSTRAIN") return "Total strain";
    return QString::fromStdString(name);
}

QString componentName(const ccxvis::Dataset& dataset, int component)
{
    if (component == magnitude) return "Magnitude";
    if (component == mises) return "von Mises";
    return QString::fromStdString(dataset.components.at(component));
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle("CCXVis");
    resize(1280, 800);
    reader_.setMaxThreadCount(1);

    auto* menus = menuBar();
    const auto addMenu = [menus](const QString& title, const QString& name) {
        auto* menu = menus->addMenu(title);
        menu->setObjectName(name);
        return menu;
    };
    auto* fileMenu = addMenu("&File", "fileMenu");
    auto* viewMenu = addMenu("&View", "viewMenu");
    auto* resultMenu = addMenu("&Result", "resultMenu");
    auto* plotMenu = addMenu("&Plot", "plotMenu");
    auto* optionsMenu = addMenu("&Options", "optionsMenu");
    auto* toolsMenu = addMenu("&Tools", "toolsMenu");
    auto* animateMenu = addMenu("A&nimate", "animateMenu");

    open_ = fileMenu->addAction("Open FRD…");
    open_->setShortcut(QKeySequence::Open);
    export_ = fileMenu->addAction("Export PNG…");
    export_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    addAction(export_);

    view_ = viewMenu->menuAction();
    fit_ = viewMenu->addAction("Fit");
    fit_->setShortcut(QKeySequence(Qt::Key_F));
    addAction(fit_);
    auto* orthogonal = viewMenu->addAction("Orthogonal");
    orthogonal->setCheckable(true);
    orthogonal->setChecked(true);
    viewMenu->addSeparator();

    result_ = resultMenu->menuAction();
    stepFrame_ = resultMenu->addAction("Step/Frame…");
    fields_ = resultMenu->addMenu("Field Output");
    fields_->setObjectName("fieldsMenu");
    fieldChoices_ = new QActionGroup(fields_);

    plot_ = plotMenu->menuAction();
    plotMenu->setToolTipsVisible(true);
    auto* visualizationGroup = new QActionGroup(plotMenu);
    using Mode = Viewport::Visualization;
    const auto addVisualization = [&](const QString& name, Mode mode) {
        auto* action = plotMenu->addAction(name, this, [this, mode] { viewport_->setVisualization(mode); });
        action->setCheckable(true);
        visualizationGroup->addAction(action);
        action->setChecked(mode == Mode::Surface);
    };
    addVisualization("Surface", Mode::Surface);
    addVisualization("Edge", Mode::Edge);
    addVisualization("Mesh", Mode::Mesh);
    addVisualization("Featured Edge", Mode::FeaturedEdge);
    addVisualization("Featured Edge with Mesh", Mode::FeaturedEdgeWithMesh);
    plotMenu->addSeparator();
    deform_ = plotMenu->addAction("Show deformation");
    deform_->setObjectName("deformation");
    deform_->setCheckable(true);

    auto* commands = addToolBar("Result shortcuts");
    commands->setObjectName("resultsToolbar");
    commands->setMovable(false);
    commands->addAction(fit_);
    commands->addSeparator();
    auto* frameLabelWidget = new QLabel("Fra&me", commands);
    step_ = new QComboBox(commands);
    step_->setObjectName("frames");
    step_->setAccessibleName("Result frame");
    step_->setMaxVisibleItems(18);
    step_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    step_->setMinimumContentsLength(12);
    step_->setMaximumWidth(340);
    frameLabelWidget->setBuddy(step_);
    commands->addWidget(frameLabelWidget);
    commands->addWidget(step_);
    fieldButton_ = new QToolButton(commands);
    fieldButton_->setObjectName("fieldOutputButton");
    fieldButton_->setText("Field");
    fieldButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    fieldButton_->setAutoRaise(true);
    fieldButton_->setPopupMode(QToolButton::InstantPopup);
    fieldButton_->setMenu(fields_);
    commands->addWidget(fieldButton_);

    options_ = optionsMenu->menuAction();
    optionsMenu->setToolTipsVisible(true);

    auto* scaleRow = new QWidget;
    auto* scaleLayout = new QHBoxLayout(scaleRow);
    scaleLayout->setContentsMargins(12, 4, 12, 4);
    auto* scaleLabel = new QLabel("Deformation &scale", scaleRow);
    scale_ = new QDoubleSpinBox(scaleRow);
    scale_->setObjectName("deformationScale");
    scale_->setAccessibleName("Deformation scale");
    scale_->setRange(0.000001, 1000000.);
    scale_->setDecimals(6);
    scale_->setValue(1.);
    scale_->setKeyboardTracking(false);
    scaleLabel->setBuddy(scale_);
    scaleLayout->addWidget(scaleLabel);
    scaleLayout->addWidget(scale_, 1);
    auto* scaleOption = new QWidgetAction(optionsMenu);
    scaleOption->setDefaultWidget(scaleRow);
    optionsMenu->addAction(scaleOption);
    scaleOption_ = scaleOption;

    optionsMenu->addSeparator();
    auto* palettes = optionsMenu->addMenu("Colormap");
    palettes->setObjectName("colormapMenu");
    auto* paletteGroup = new QActionGroup(palettes);
    using ColorMap = Viewport::ColorMap;
    const auto addPalette = [&](const QString& name, ColorMap map) {
        auto* action = palettes->addAction(name, this, [this, map] { viewport_->setColorMap(map); });
        action->setCheckable(true);
        paletteGroup->addAction(action);
        action->setChecked(map == ColorMap::Rainbow);
    };
    addPalette("Viridis", ColorMap::Viridis);
    addPalette("Inferno", ColorMap::Inferno);
    addPalette("Rainbow", ColorMap::Rainbow);
    addPalette("Cool-to-Warm", ColorMap::CoolToWarm);
    addPalette("Jet", ColorMap::Jet);
    addPalette("Plasma", ColorMap::Plasma);

    auto* colorScale = optionsMenu->addMenu("Color scale");
    colorScale->setObjectName("colorScaleMenu");
    auto* colorScaleGroup = new QActionGroup(colorScale);
    auto* continuous = colorScale->addAction("Continuous");
    auto* discrete = colorScale->addAction("Discrete");
    for (auto* action : {continuous, discrete}) {
        action->setCheckable(true);
        colorScaleGroup->addAction(action);
    }
    continuous->setChecked(true);

    auto* stepsRow = new QWidget;
    auto* stepsLayout = new QHBoxLayout(stepsRow);
    stepsLayout->setContentsMargins(12, 4, 12, 4);
    auto* stepsLabel = new QLabel("&Steps", stepsRow);
    auto* colorSteps = new QSpinBox(stepsRow);
    colorSteps->setObjectName("colorSteps");
    colorSteps->setAccessibleName("Number of color steps");
    colorSteps->setRange(Viewport::minimumColorSteps, Viewport::maximumColorSteps);
    colorSteps->setValue(11);
    colorSteps->setKeyboardTracking(false);
    stepsLabel->setBuddy(colorSteps);
    stepsLayout->addWidget(stepsLabel);
    stepsLayout->addWidget(colorSteps, 1);
    auto* stepsOption = new QWidgetAction(colorScale);
    stepsOption->setDefaultWidget(stepsRow);
    stepsOption->setEnabled(false);
    colorScale->addAction(stepsOption);
    connect(discrete, &QAction::toggled, this, [this, colorSteps, stepsOption](bool enabled) {
        stepsOption->setEnabled(enabled);
        viewport_->setColorSteps(enabled ? std::optional{colorSteps->value()} : std::nullopt);
    });
    connect(colorSteps, qOverload<int>(&QSpinBox::valueChanged), this, [this, discrete](int count) {
        if (discrete->isChecked()) viewport_->setColorSteps(count);
    });

    auto* xyPlot = toolsMenu->addAction("XY Plot…");
    xyPlot->setObjectName("xyPlot");
    connect(xyPlot, &QAction::triggered, this, [this] {
        const auto files = QFileDialog::getOpenFileNames(this, "Open XY Plot", {},
            "Plot data (*.csv *.CSV *.dat *.DAT *.txt *.TXT);;All files (*)");
        for (const auto& file : files) openCurvePlot(this, file);
    });
    animate_ = animateMenu->addAction("Contour Animation…", this, &MainWindow::showAnimation);
    animate_->setObjectName("contourAnimation");
    probeAction_ = toolsMenu->addAction("Probe");
    probeAction_->setCheckable(true);
    sectionAction_ = toolsMenu->addAction("Section View");
    sectionAction_->setObjectName("sectionView");
    sectionAction_->setCheckable(true);

    viewport_ = new Viewport(this);
    viewport_->setObjectName("viewport");
    viewport_->setAccessibleName("Simulation results viewport");
    setCentralWidget(viewport_);

    sectionBar_ = new QToolBar("Section View", this);
    sectionBar_->setObjectName("sectionControls");
    sectionBar_->setMovable(false);
    sectionBar_->toggleViewAction()->setVisible(false);
    addToolBar(Qt::BottomToolBarArea, sectionBar_);
    sectionBar_->hide();

    auto* axisLabel = new QLabel("&Axis", sectionBar_);
    sectionAxis_ = new QComboBox(sectionBar_);
    sectionAxis_->setObjectName("sectionAxis");
    sectionAxis_->addItems({"X", "Y", "Z"});
    axisLabel->setBuddy(sectionAxis_);
    sectionBar_->addWidget(axisLabel);
    sectionBar_->addWidget(sectionAxis_);

    auto* positionLabel = new QLabel("Pos&ition", sectionBar_);
    sectionPosition_ = new QSlider(Qt::Horizontal, sectionBar_);
    sectionPosition_->setObjectName("sectionPosition");
    sectionPosition_->setAccessibleName("Section position slider");
    sectionPosition_->setRange(0, sectionSteps);
    sectionPosition_->setPageStep(sectionSteps / 10);
    sectionPosition_->setMinimumWidth(100);
    sectionPosition_->setMaximumWidth(220);
    sectionCoordinate_ = new QLineEdit(sectionBar_);
    sectionCoordinate_->setObjectName("sectionCoordinate");
    sectionCoordinate_->setAccessibleName("Section coordinate in model units");
    sectionCoordinate_->setToolTip("Plane coordinate in model units");
    sectionCoordinate_->setMaximumWidth(175);
    positionLabel->setBuddy(sectionCoordinate_);
    sectionBar_->addWidget(positionLabel);
    sectionBar_->addWidget(sectionPosition_);
    sectionBar_->addWidget(sectionCoordinate_);

    sectionFlip_ = new QToolButton(sectionBar_);
    sectionFlip_->setObjectName("sectionFlip");
    sectionFlip_->setText("Flip Side");
    sectionFlip_->setToolTip("Keep the opposite side of the plane");
    sectionFlip_->setCheckable(true);
    sectionBar_->addWidget(sectionFlip_);
    sectionNotice_ = new QLabel(sectionBar_);
    sectionBar_->addWidget(sectionNotice_);

    probe_ = new QLabel(statusBar());
    probe_->setObjectName("probeValue");
    probe_->setTextFormat(Qt::PlainText);
    probe_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addPermanentWidget(probe_);

    const auto addView = [this, viewMenu](const QString& name, ccxvis::Point direction, ccxvis::Point up) {
        viewMenu->addAction(name, this, [this, direction, up] { viewport_->view(direction, up); });
    };
    addView("Isometric", {1, 1, 1}, {0, 0, 1});
    addView("Front (+Y)", {0, 1, 0}, {0, 0, 1});
    addView("Back (−Y)", {0, -1, 0}, {0, 0, 1});
    addView("Right (+X)", {1, 0, 0}, {0, 0, 1});
    addView("Left (−X)", {-1, 0, 0}, {0, 0, 1});
    addView("Top (+Z)", {0, 0, 1}, {0, 1, 0});
    addView("Bottom (−Z)", {0, 0, -1}, {0, 1, 0});

    connect(open_, &QAction::triggered, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, "Open CalculiX results", {}, "FRD results (*.frd *.FRD)");
        if (!file.isEmpty()) openFile(file);
    });
    connect(fit_, &QAction::triggered, viewport_, &Viewport::fit);
    connect(orthogonal, &QAction::toggled, viewport_, &Viewport::setOrthogonal);
    connect(export_, &QAction::triggered, this, &MainWindow::exportPng);
    connect(stepFrame_, &QAction::triggered, this, [this] {
        QStringList frames;
        for (int i = 0; i < step_->count(); ++i) frames << step_->itemText(i);
        bool accepted = false;
        const auto frame = QInputDialog::getItem(this, "Step/Frame", "&Frame", frames,
                                                 step_->currentIndex(), false, &accepted);
        if (!accepted || !step_->isEnabled()) return;
        const int index = step_->findText(frame);
        if (index >= 0) step_->setCurrentIndex(index);
    });
    connect(probeAction_, &QAction::toggled, this, [this](bool active) {
        viewport_->setProbing(active);
        if (!active) picked_.reset();
        updateProbe();
    });
    viewport_->onNodePicked([this](std::optional<std::size_t> point) {
        picked_ = point;
        updateProbe();
    });
    connect(step_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int frame) { rebuildFields(frame, true); });
    connect(deform_, &QAction::toggled, this, &MainWindow::updateDeformation);
    connect(scale_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MainWindow::updateDeformation);

    sectionTimer_.setInterval(16);
    sectionTimer_.setSingleShot(true);
    connect(&sectionTimer_, &QTimer::timeout, this, [this] { applySection(true); });
    connect(sectionAction_, &QAction::toggled, this, &MainWindow::setSectionEnabled);
    connect(sectionAxis_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        section_->axis = sectionAxes.at(index);
        section_->coordinate = sectionMidpoint(viewport_->sectionBounds(section_->axis));
        applySection(true);
    });
    connect(sectionPosition_, &QSlider::valueChanged, this, [this](int value) {
        const auto bounds = viewport_->sectionBounds(section_->axis);
        const double fraction = static_cast<double>(value) / sectionSteps;
        section_->coordinate = (1. - fraction) * bounds[0] + fraction * bounds[1];
        sectionCoordinate_->setText(QString::number(section_->coordinate, 'g', 16));
        if (!sectionTimer_.isActive()) sectionTimer_.start();
    });
    connect(sectionCoordinate_, &QLineEdit::editingFinished, this, [this] {
        bool valid = false;
        const double coordinate = QLocale::c().toDouble(sectionCoordinate_->text(), &valid);
        if (valid && std::isfinite(coordinate)) section_->coordinate = coordinate;
        applySection(valid && std::isfinite(coordinate));
    });
    connect(sectionFlip_, &QToolButton::toggled, this, [this](bool flipped) {
        section_->keepPositive = !flipped;
        applySection(true);
    });

    connect(&importer_, &QFutureWatcher<Import>::finished, this, [this] {
        opening_ = false;
        auto import = importer_.result();
        if (!import.document) {
            rebuildFields(displayed_.selection.frame, true, false);
            statusBar()->showMessage(import.error);
            QMessageBox::warning(this, "Could not open FRD", import.error);
            updateControls();
            return;
        }
        sectionAction_->setChecked(false);
        section_.reset();
        document_ = std::move(import.document);
        setWindowTitle(QFileInfo(import.filename).fileName() + " — CCXVis");
        viewport_->setMesh(document_->mesh());
        displayed_ = {};
        warped_ = false;
        picked_.reset();
        probeAction_->setChecked(false);
        deform_->setChecked(false);
        {
            const QSignalBlocker blocker(step_);
            step_->clear();
            const auto& frames = document_->frames();
            for (std::size_t i = 0; i < frames.size(); ++i) {
                auto label = frameLabel(frames[i], i);
                if (step_->findText(label) >= 0) label += QString(" · Frame %1").arg(i + 1);
                step_->addItem(label);
            }
        }
        rebuildFields(step_->currentIndex(), false, false);
        if (import.initial.selection.frame >= 0) {
            showResult(std::move(import.initial));
        } else {
            statusBar()->showMessage(document_->skippedDatasets() ?
                "Mesh loaded · No supported nodal fields" : "Mesh loaded");
        }
        updateProbe();
        updateControls();
    });

    connect(&fieldReader_, &QFutureWatcher<Result>::finished, this, [this] {
        reading_ = false;
        auto result = fieldReader_.result();
        if (opening_) return;
        if (queued_) {
            fetchResult();
            return;
        }
        if (fieldDocument_.lock() != document_) {
            updateControls();
            return;
        }
        if (!result.error.isEmpty()) {
            if (displayed_.selection.frame >= 0) rebuildFields(displayed_.selection.frame, true, false);
            statusBar()->showMessage(result.error);
            updateControls();
            return;
        }
        showResult(std::move(result));
    });

    updateProbe();
    updateControls();
    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow()
{
    shutdownAnimation();
    reader_.waitForDone();
}

void MainWindow::openFile(const QString& path)
{
    if (opening_ || animationActive_ || path.isEmpty()) return;
    opening_ = true;
    queued_ = false;
    statusBar()->showMessage("Opening " + QFileInfo(path).fileName() + "…");
    updateControls();
    const auto filename = QFileInfo(path).absoluteFilePath();
    importer_.setFuture(QtConcurrent::run(&reader_, [filename] {
        Import result;
        result.filename = filename;
        try {
            auto document = ccxvis::FrdDocument::open(QFile::encodeName(filename).constData());
            if (!document.frames().empty()) {
                const auto id = document.frames().front().datasets.front();
                const auto& dataset = document.datasets()[id];
                int component = 0;
                if (ccxvis::displacementComponents(dataset)) component = magnitude;
                else if (ccxvis::canComputeMises(dataset)) component = mises;
                result.initial = readResult(document, {0, id, component});
                if (!result.initial.error.isEmpty()) {
                    result.error = result.initial.error;
                    return result;
                }
            }
            result.document = std::make_shared<ccxvis::FrdDocument>(std::move(document));
        } catch (const std::exception& error) {
            result.error = QString::fromLocal8Bit(error.what());
        }
        return result;
    }));
}

void MainWindow::rebuildFields(int frame, bool preserve, bool load)
{
    const QSignalBlocker blockFrame(step_);
    step_->setCurrentIndex(frame);
    step_->setToolTip(step_->currentText());
    for (auto* action : fields_->actions()) delete action->menu();
    fieldButton_->setText("Field");
    if (!document_ || frame < 0) return;
    QAction* first = nullptr;
    QAction* previous = nullptr;
    const auto& datasets = document_->datasets();
    const auto selection = load && reading_ && fieldDocument_.lock() == document_ ?
        nextSelection_ : displayed_.selection;
    for (const auto id : document_->frames().at(frame).datasets) {
        const auto& dataset = datasets[id];
        auto* group = fields_->addMenu(fieldName(dataset.field));
        const auto add = [&](int index) {
            auto* item = group->addAction(componentName(dataset, index), this,
                                          [this, id, index] { selectField(id, index); });
            item->setCheckable(true);
            fieldChoices_->addAction(item);
            if (!first) first = item;
            if (preserve && selection.frame >= 0 &&
                dataset.field == datasets[selection.dataset].field &&
                item->text() == componentName(datasets[selection.dataset], selection.component))
                previous = item;
        };
        if (ccxvis::displacementComponents(dataset)) add(magnitude);
        if (ccxvis::canComputeMises(dataset)) add(mises);
        for (std::size_t i = 0; i < dataset.components.size(); ++i) add(static_cast<int>(i));
    }
    if (auto* item = previous ? previous : first) {
        if (load) {
            item->trigger();
        } else {
            item->setChecked(true);
            fieldButton_->setText("Field · " + item->text());
        }
    }
}

void MainWindow::selectField(std::size_t dataset, int component)
{
    if (opening_ || animationActive_) return;
    fieldButton_->setText("Field · " + componentName(document_->datasets().at(dataset), component));
    nextSelection_ = {step_->currentIndex(), dataset, component};
    queued_ = true;
    if (!reading_) fetchResult();
}

void MainWindow::fetchResult()
{
    queued_ = false;
    reading_ = true;
    const auto selection = nextSelection_;
    const auto document = document_;
    fieldDocument_ = document;
    const auto& dataset = document->datasets()[selection.dataset];
    auto name = fieldName(dataset.field) + " · " + componentName(dataset, selection.component);
    statusBar()->showMessage("Loading " + name + "…");
    updateControls();
    fieldReader_.setFuture(QtConcurrent::run(&reader_, [document, selection] {
        return readResult(*document, selection);
    }));
}

MainWindow::Result MainWindow::readResult(ccxvis::FrdDocument& document, Selection selection, bool readDisplacements)
{
    Result result;
    result.selection = selection;
    const auto& metadata = document.datasets().at(selection.dataset);
    result.name = fieldName(metadata.field) + " · " + componentName(metadata, selection.component);
    try {
        auto values = document.readField(selection.dataset);
        if (selection.component < 0)
            result.scalars = ccxvis::derive(metadata, values, selection.component == magnitude ?
                                         ccxvis::Derived::Magnitude : ccxvis::Derived::Mises);
        const auto displacement = document.frames().at(selection.frame).displacement;
        if (readDisplacements && displacement) {
            const auto xyz = *ccxvis::displacementComponents(document.datasets()[*displacement]);
            try {
                auto vectors = *displacement == selection.dataset ?
                    ccxvis::FieldValues{} : document.readField(*displacement);
                const auto& channels = *displacement == selection.dataset ? values.components : vectors.components;
                const auto size = document.mesh().nodes.size();
                for (std::size_t i = 0; i < size; ++i) {
                    const ccxvis::Point point{channels[xyz[0]][i], channels[xyz[1]][i], channels[xyz[2]][i]};
                    if (!std::all_of(point.begin(), point.end(), [](double v) { return std::isfinite(v); })) {
                        result.displacements.clear();
                        break;
                    }
                    result.displacements.push_back(point);
                }
            } catch (const std::runtime_error&) {
                // The selected scalar field remains viewable if its companion cannot be read.
            }
        }
        if (selection.component >= 0)
            result.scalars = std::move(values.components.at(selection.component));
    } catch (const std::exception& error) {
        result.error = QString::fromLocal8Bit(error.what());
    }
    return result;
}

void MainWindow::showResult(Result result)
{
    displayed_ = std::move(result);
    viewport_->setResult(displayed_.scalars, displayed_.name);
    updateDeformation();
    QString message = QString("%1 nodes · %2 elements")
        .arg(document_->mesh().nodes.size()).arg(document_->mesh().elements.size());
    if (document_->skippedDatasets())
        message += QString(" · %1 unsupported result blocks").arg(document_->skippedDatasets());
    if (std::none_of(displayed_.scalars.begin(), displayed_.scalars.end(),
                     [](double value) { return std::isfinite(value); }))
        message += " · No values for this component";
    statusBar()->showMessage(message);
    updateControls();
}

void MainWindow::updateDeformation()
{
    if (!document_) return;
    bool active = deform_->isChecked() && !displayed_.displacements.empty();
    if (active || warped_) {
        const auto& mesh = document_->mesh();
        std::vector<ccxvis::Point> positions;
        if (active && !ccxvis::deformPositions(mesh, displayed_.displacements, scale_->value(), &positions)) {
            active = false;
            statusBar()->showMessage("Deformation scale exceeds the coordinate range");
        }
        if (!active) positions = ccxvis::undeformedPositions(mesh);
        viewport_->setPositions(positions);
        applySection(false);
    }
    warped_ = active;
    updateProbe();
    updateControls();
}

void MainWindow::setSectionEnabled(bool enabled)
{
    sectionBar_->setVisible(enabled);
    if (!enabled) {
        sectionTimer_.stop();
        viewport_->setSection(std::nullopt);
    } else {
        if (!section_) {
            auto axis = SectionAxis::X;
            for (const auto candidate : sectionAxes) {
                const auto bounds = viewport_->sectionBounds(candidate);
                if (bounds[0] < bounds[1]) {
                    axis = candidate;
                    break;
                }
            }
            section_ = Viewport::SectionPlane{axis, sectionMidpoint(viewport_->sectionBounds(axis)), true};
        }
        applySection(true);
    }
}

void MainWindow::applySection(bool clearProbe)
{
    sectionTimer_.stop();
    if (!sectionAction_->isChecked() || !section_) return;
    const auto bounds = viewport_->sectionBounds(section_->axis);
    const auto previous = section_->coordinate;
    section_->coordinate = std::clamp(previous, bounds[0], bounds[1]);

    const QSignalBlocker blockAxis(sectionAxis_);
    const QSignalBlocker blockPosition(sectionPosition_);
    const QSignalBlocker blockFlip(sectionFlip_);
    sectionAxis_->setCurrentIndex(static_cast<int>(section_->axis));
    sectionFlip_->setChecked(!section_->keepPositive);
    sectionCoordinate_->setText(QString::number(section_->coordinate, 'g', 16));
    const bool movable = bounds[0] < bounds[1];
    sectionPosition_->setEnabled(movable);
    if (movable) {
        const long double low = bounds[0], high = bounds[1];
        const auto fraction = (section_->coordinate - low) / (high - low);
        sectionPosition_->setValue(static_cast<int>(std::lround(fraction * sectionSteps)));
    }

    if ((clearProbe || previous != section_->coordinate) && picked_) {
        picked_.reset();
        updateProbe();
    }
    viewport_->setSection(section_);
    sectionNotice_->setText(viewport_->sectionIntersectsMesh() ? QString{} : "No intersection");
}

void MainWindow::updateProbe()
{
    viewport_->markNode(picked_);
    probe_->setVisible(probeAction_->isChecked());
    if (!picked_) {
        probe_->setText(probeAction_->isChecked() ? "Click a visible mesh node." : QString{});
        return;
    }
    const auto& node = document_->mesh().nodes.at(*picked_);
    auto label = QString("Node %1 · X %2   Y %3   Z %4").arg(node.id)
        .arg(node.position[0], 0, 'g', 8).arg(node.position[1], 0, 'g', 8)
        .arg(node.position[2], 0, 'g', 8);
    if (!displayed_.scalars.empty()) {
        const auto value = displayed_.scalars.at(*picked_);
        label += " · " + displayed_.name + "  " +
                 (std::isfinite(value) ? QString::number(value, 'g', 8) : "No value");
    }
    probe_->setText(label);
}

void MainWindow::updateControls()
{
    const bool present = static_cast<bool>(document_);
    const bool busy = opening_ || reading_ || animationActive_;
    const bool editable = present && !animationActive_;
    open_->setEnabled(!opening_ && !animationActive_);
    const bool canChooseFrame = editable && !opening_ && step_->count() > 0;
    const bool canChooseField = editable && !opening_ && !fields_->isEmpty();
    step_->setEnabled(canChooseFrame);
    stepFrame_->setEnabled(canChooseFrame);
    fields_->menuAction()->setEnabled(canChooseField);
    fieldButton_->setEnabled(canChooseField);
    result_->setEnabled(canChooseFrame || canChooseField);
    view_->setEnabled(editable);
    fit_->setEnabled(editable);
    plot_->setEnabled(editable);
    options_->setEnabled(editable);
    sectionAction_->setEnabled(editable && !opening_);
    sectionBar_->setEnabled(editable);
    export_->setEnabled(present && !busy);
    probeAction_->setEnabled(present && !busy);
    viewport_->setProbing(present && !busy && probeAction_->isChecked());
    deform_->setEnabled(!busy && !displayed_.displacements.empty());
    deform_->setToolTip(displayed_.displacements.empty() ? "This frame needs a complete displacement field." : QString{});
    scaleOption_->setEnabled(deform_->isEnabled() && deform_->isChecked());
    animate_->setEnabled(present && !opening_ && !reading_ && displayed_.selection.frame >= 0);
    refreshAnimation();
}

void MainWindow::exportPng()
{
    if (sectionTimer_.isActive()) applySection(true);
    auto path = QFileDialog::getSaveFileName(this, "Export viewport", "results.png", "PNG images (*.png)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
    try {
        viewport_->saveImage(path);
        statusBar()->showMessage("Saved " + QFileInfo(path).fileName(), 5000);
    } catch (const std::exception& error) {
        QMessageBox::warning(this, "Export failed", QString::fromLocal8Bit(error.what()));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    stopAnimation();
    QMainWindow::closeEvent(event);
}
