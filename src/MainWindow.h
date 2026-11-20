// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "ContourSequence.h"
#include "Viewport.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QThreadPool>
#include <QTimer>

#include <memory>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMenu;
class QSlider;
class QToolBar;
class QToolButton;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void openFile(const QString& path);

private:
    class Animation;
    using Selection = ccxvis::ContourSelection;
    void closeEvent(QCloseEvent* event) override;
    void showAnimation();
    void refreshAnimation();
    void stopAnimation();
    void shutdownAnimation();
    struct Result {
        Selection selection;
        QString name;
        QString error;
        std::vector<double> scalars;
        std::vector<ccxvis::Point> displacements;
    };
    struct Import {
        std::shared_ptr<ccxvis::FrdDocument> document;
        QString error;
        QString filename;
        Result initial;
    };

    void rebuildFields(int frame, bool preserve, bool load = true);
    void selectField(std::size_t dataset, int component);
    void fetchResult();
    static Result readResult(ccxvis::FrdDocument& document, Selection selection, bool readDisplacements = true);
    void showResult(Result result);
    void updateDeformation();
    void setSectionEnabled(bool enabled);
    void applySection(bool clearProbe);
    void updateProbe();
    void updateControls();
    void exportPng();

    Viewport* viewport_;
    QComboBox* step_;
    QMenu* fields_;
    QActionGroup* fieldChoices_;
    QLabel* probe_;
    QAction* deform_;
    QDoubleSpinBox* scale_;
    QAction* open_;
    QAction* export_;
    QAction* probeAction_;
    QAction* view_;
    QAction* fit_;
    QAction* result_;
    QAction* stepFrame_;
    QAction* plot_;
    QToolButton* fieldButton_;
    QAction* options_;
    QAction* scaleOption_;
    QAction* animate_;
    Animation* animation_ = nullptr;
    QAction* sectionAction_;
    QToolBar* sectionBar_;
    QComboBox* sectionAxis_;
    QLineEdit* sectionCoordinate_;
    QSlider* sectionPosition_;
    QToolButton* sectionFlip_;
    QLabel* sectionNotice_;
    QTimer sectionTimer_;
    std::optional<Viewport::SectionPlane> section_;
    QThreadPool reader_;
    QFutureWatcher<Import> importer_;
    QFutureWatcher<Result> fieldReader_;
    std::shared_ptr<ccxvis::FrdDocument> document_;
    std::weak_ptr<ccxvis::FrdDocument> fieldDocument_;
    Result displayed_;
    Selection nextSelection_;
    std::optional<std::size_t> picked_;
    bool opening_ = false;
    bool reading_ = false;
    bool queued_ = false;
    bool warped_ = false;
    bool animationActive_ = false;
};
