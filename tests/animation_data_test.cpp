// SPDX-License-Identifier: GPL-2.0-only
#include "ContourSequence.h"
#include "MovieExport.h"
#include "MainWindow.h"
#include "Viewport.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <vtkActor2DCollection.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkLookupTable.h>
#include <vtkMapper.h>
#include <vtkPlane.h>
#include <vtkPlaneCollection.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void await(Predicate done, const char* message, int timeout = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeout) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(done(), message);
}

void matching()
{
    const std::vector<ccxvis::Dataset> datasets{
        {"DISP", {"D1", "D2", "D3"}}, {"DISP", {"D3", "D1", "D2"}},
        {"DISP", {"D1", "D3"}}, {"OTHER", {"D2"}}, {"DISP", {"D1", "D2", "D3"}, true}
    };
    std::vector<ccxvis::Frame> frames(8);
    for (auto& frame : frames) { frame.analysis = 2; frame.subcase = 1; frame.mode = 1; }
    frames[0].subcase = 2;
    frames[0].datasets = {0};
    frames[1].datasets = {0};
    frames[2].datasets = {3};
    frames[3].datasets = {1};
    frames[4].datasets = {2};
    frames[5].datasets = {4};
    frames[6].mode = 0;
    frames[6].datasets = {0};
    frames[7].datasets = {0};
    const auto sequence = ccxvis::compatibleFrames(frames, datasets, {3, 1, 2});
    require(sequence.missing == 3 && sequence.frames.size() == 2 &&
            sequence.frames[0].frame == 1 && sequence.frames[0].component == 1 &&
            sequence.frames[1].frame == 3 && sequence.frames[1].component == 2,
            "Use field/component identity and retain saved order inside the active modal series");
}

void fixedRange()
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    ccxvis::ScalarExtent extent;
    extent.include({nan, 2., 6.});
    extent.include({-1., 4., std::numeric_limits<double>::infinity()});
    require(extent.range() == std::optional<std::array<double, 2>>{{-1, 6}},
            "Preflight range combines finite values across frames");
    Viewport viewport;
    ccxvis::Mesh mesh;
    mesh.nodes = {{1, {0, 0, 0}}, {2, {1, 0, 0}}};
    viewport.setMesh(mesh);
    viewport.setResult({2, 3}, "Displacement", extent.range());
    auto* renderer = viewport.renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* actors = renderer->GetActors2D();
    actors->InitTraversal();
    vtkScalarBarActor* legend = nullptr;
    while (auto* actor = actors->GetNextActor2D())
        if (auto* candidate = vtkScalarBarActor::SafeDownCast(actor)) legend = candidate;
    require(legend && legend->GetVisibility(), "Fixed range shows the contour legend");
    const auto* range = legend->GetLookupTable()->GetRange();
    viewport.setResult({nan, nan}, "Displacement", extent.range());
    require(legend->GetVisibility() && range[0] == -1 && range[1] == 6,
            "A blank frame still displays the fixed animation legend");
    viewport.setResult({4, 5}, "Displacement");
    require(range[0] == 4 && range[1] == 5, "Ordinary result display restores its automatic range");
}

void playback()
{
    QTemporaryDir directory;
    const auto path = directory.filePath("sequence.frd");
    {
        std::ofstream out(QFile::encodeName(path).constData());
        out << "    1C\n    2C\n";
        const auto row = [&](int id, int x) {
            out << " -1" << std::setw(5) << id << std::setw(12) << x << std::setw(12) << 0
                << std::setw(12) << 0 << '\n';
        };
        row(1, 0);
        row(2, 1);
        out << " -3\n";
        for (int frame = 1; frame <= 3; ++frame) {
            out << "  100C" << std::setw(6) << 100 + frame << std::setw(12) << frame << '\n'
                << " -4  DISP        3    1\n"
                << " -5  D1          1    2    1    0    0\n"
                << " -5  D2          1    2    2    0    0\n"
                << " -5  D3          1    2    3    0    0\n";
            row(1, 0);
            if (frame < 3) row(2, frame * 2);
            out << " -3\n";
        }
        out << " 9999\n";
        require(static_cast<bool>(out), "Write the synthetic FRD sequence");
    }

    MainWindow window;
    window.openFile(path);
    auto* action = window.findChild<QAction*>("contourAnimation");
    auto* deform = window.findChild<QAction*>("deformation");
    auto* scale = window.findChild<QDoubleSpinBox*>("deformationScale");
    auto* frame = window.findChild<QComboBox*>("frames");
    auto* section = window.findChild<QAction*>("sectionView");
    auto* viewport = dynamic_cast<Viewport*>(window.centralWidget());
    require(action && deform && scale && frame && section && viewport, "Find animation and model controls");
    await([&] { return action->isEnabled(); }, "Load frames before enabling animation");
    deform->trigger();
    scale->setValue(2);
    section->trigger();
    const auto savedBounds = viewport->sectionBounds(Viewport::SectionAxis::X);
    auto* renderer = viewport->renderWindow()->GetRenderers()->GetFirstRenderer();
    auto* actors3D = renderer->GetActors();
    actors3D->InitTraversal();
    auto* clippingPlanes = actors3D->GetNextActor()->GetMapper()->GetClippingPlanes();
    require(clippingPlanes && clippingPlanes->GetNumberOfItems() == 1, "Section plane is active before playback");
    auto* plane = clippingPlanes->GetItem(0);
    const auto savedSection = plane->GetOrigin()[0];
    auto* camera = renderer->GetActiveCamera();
    camera->SetClippingRange(.04, 80.);
    const auto savedScale = camera->GetParallelScale();
    action->trigger();
    auto* dialog = window.findChild<QDialog*>("contourAnimationDialog");
    require(dialog && dialog->isVisible(), "Open the contour animation dialog");
    auto* play = dialog->findChild<QPushButton*>("animationPlay");
    auto* stop = dialog->findChild<QPushButton*>("animationStop");
    auto* last = dialog->findChild<QComboBox*>("animationLast");
    auto* fps = dialog->findChild<QSpinBox*>("animationFps");
    auto* status = dialog->findChild<QLabel*>("animationStatus");
    require(play && stop && last && fps && status && last->currentIndex() == 2 && fps->value() == 12,
            "Default to the complete matching range and 12 FPS");
    play->click();
    await([&] { return play->text() == "Play" && status->text().contains("usable displacements"); },
          "Preflight reports a frame with incomplete displacements");
    require(status->text().contains("Frame 3") && viewport->sectionBounds(Viewport::SectionAxis::X) == savedBounds,
            "Preflight identifies the failing frame and preserves the model");

    last->setCurrentIndex(0);
    require(!play->isEnabled(), "A single matching frame cannot be played");
    last->setCurrentIndex(1);
    fps->setValue(2);
    play->click();
    require(!frame->isEnabled() && !viewport->isEnabled() && stop->isEnabled(), "Playback locks conflicting edits");
    await([&] { return status->text().startsWith("Frame 1"); }, "Show the first frame");
    play->click();
    QElapsedTimer pause;
    pause.start();
    await([&] { return pause.elapsed() >= 550; }, "Wait past the frame interval");
    require(status->text() == "Paused" && viewport->sectionBounds(Viewport::SectionAxis::X) == savedBounds,
            "Pause holds the currently displayed frame");
    play->click();
    await([&] { return status->text().startsWith("Frame 2"); }, "Resume advances to the next frame");
    require(viewport->sectionBounds(Viewport::SectionAxis::X)[1] == 9,
            "Preview applies the selected displacement scale");
    require(!section->isEnabled() && section->isChecked() && plane->GetOrigin()[0] == savedSection,
            "Preview retains the active section plane and locks its controls");
    auto* actors = renderer->GetActors2D();
    actors->InitTraversal();
    vtkTextActor* overlay = nullptr;
    while (auto* actor = actors->GetNextActor2D())
        if (auto* text = vtkTextActor::SafeDownCast(actor)) overlay = text;
    require(overlay && overlay->GetVisibility() && QString::fromUtf8(overlay->GetInput()).startsWith("Frame 2"),
            "The frame label is drawn inside the viewport");

    stop->click();
    require(frame->isEnabled() && viewport->isEnabled() && frame->currentIndex() == 0 && deform->isChecked() &&
            !overlay->GetVisibility() && camera->GetParallelScale() == savedScale &&
            camera->GetClippingRange()[0] == .04 && camera->GetClippingRange()[1] == 80 &&
            viewport->sectionBounds(Viewport::SectionAxis::X) == savedBounds,
            "Stop restores viewport state and unlocks the original selection");
    fps->setValue(120);
    play->click();
    await([&] { return status->text() == "Playback complete."; }, "Playback completes after the inclusive last frame");
    require(!overlay->GetVisibility() && viewport->sectionBounds(Viewport::SectionAxis::X) == savedBounds &&
            section->isEnabled() && section->isChecked() && plane->GetOrigin()[0] == savedSection,
            "Completion restores the original contour and section controls");
}

void encoder()
{
    QTemporaryDir directory;
    require(directory.isValid(), "Create encoder test directory");
    const auto destination = directory.filePath("output.mp4");
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Write test file");
    };
    const auto read = [](const QString& path) {
        QFile file(path);
        require(file.open(QIODevice::ReadOnly), "Read staged output");
        return file.readAll();
    };
    const auto stub = directory.filePath("encoder stub");
    for (const auto& script : {QByteArray("printf movie > encoded.mp4\nprintf 'frame=2\\nprogress=end\\n'"),
                               QByteArray("for option in \"$@\"; do case $option in libx264) "
                                          "printf \"Unknown encoder 'libx264'\\n\" >&2; exit 1;; esac; done\n"
                                          "printf movie > encoded.mp4\nprintf 'frame=2\\nprogress=end\\n'"),
                               QByteArray("printf incomplete > encoded.mp4\nprintf failed >&2\nexit 1"),
                               QByteArray("printf incomplete > encoded.mp4\nwhile :; do :; done")}) {
        write(destination, "original");
        write(stub, "#!/bin/sh\n" + script + '\n');
        require(QFile::setPermissions(stub, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "Make stub executable");
        MovieExport exportMovie;
        bool finished = false;
        MovieExport::Outcome result = MovieExport::Outcome::Failed;
        int frame = 0;
        exportMovie.onProgress = [&](int count) { frame = count; };
        exportMovie.onFinished = [&](MovieExport::Outcome outcome, const QString&) {
            result = outcome;
            finished = true;
        };
        exportMovie.prepare(destination, 2, 12);
        auto staging = QFileInfo(exportMovie.framePath(0)).absolutePath();
        write(exportMovie.framePath(0), "first PNG");
        write(exportMovie.framePath(1), "second PNG");
        exportMovie.encode(stub);
        if (script.contains("while")) {
            await([&] { return QFile::exists(staging + "/encoded.mp4"); }, "Start encoding before cancellation");
            require(read(destination) == "original", "A partial encode does not replace the destination");
            exportMovie.cancel();
        }
        await([&] { return finished; }, "Finish encoder");
        require(!QFile::exists(staging), "Release staging files");
        if (script.contains("progress=end")) {
            require(result == MovieExport::Outcome::Saved && read(destination) == "movie" && frame == 2,
                    "Publish the output and report encoded frames");
        } else {
            require(result == (script.contains("while") ? MovieExport::Outcome::Cancelled : MovieExport::Outcome::Failed)
                    && read(destination) == "original", "Failure and cancellation preserve the destination");
        }
    }

    MovieExport capture;
    capture.prepare(destination, 2, 12);
    const auto staging = QFileInfo(capture.framePath(0)).absolutePath();
    write(capture.framePath(0), "first PNG");
    capture.cancel();
    require(!QFile::exists(staging) && read(destination) == "original", "Cancel capture before encoding");
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try {
        matching();
        fixedRange();
        playback();
        encoder();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
