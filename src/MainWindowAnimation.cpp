// SPDX-License-Identifier: GPL-2.0-only
#include "MainWindow.h"
#include "MovieExport.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <stdexcept>

class MainWindow::Animation : public QDialog {
public:
    explicit Animation(MainWindow& window)
        : QDialog(&window), window_(window)
    {
        setObjectName("contourAnimationDialog");
        setWindowTitle("Contour Animation");
        setMinimumWidth(410);
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(this);
        summary_->setTextFormat(Qt::PlainText);
        summary_->setWordWrap(true);
        layout->addWidget(summary_);

        auto* form = new QFormLayout;
        first_ = new QComboBox(this);
        first_->setObjectName("animationFirst");
        last_ = new QComboBox(this);
        last_->setObjectName("animationLast");
        fps_ = new QSpinBox(this);
        fps_->setObjectName("animationFps");
        fps_->setRange(1, 120);
        fps_->setValue(12);
        form->addRow("&First frame", first_);
        form->addRow("&Last frame", last_);
        form->addRow("F&PS", fps_);
        layout->addLayout(form);

        auto* buttons = new QHBoxLayout;
        play_ = new QPushButton("Play", this);
        play_->setObjectName("animationPlay");
        stop_ = new QPushButton("Stop", this);
        stop_->setObjectName("animationStop");
        export_ = new QPushButton("Export MP4…", this);
        export_->setObjectName("animationExport");
        buttons->addWidget(play_);
        buttons->addWidget(stop_);
        buttons->addStretch();
        buttons->addWidget(export_);
        layout->addLayout(buttons);

        status_ = new QLabel(this);
        status_->setObjectName("animationStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status_->setWordWrap(true);
        progress_ = new QProgressBar(this);
        progress_->setTextVisible(true);
        progress_->hide();
        layout->addWidget(status_);
        layout->addWidget(progress_);

        const auto changed = [this] {
            status_->clear();
            updateButtons();
        };
        connect(first_, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
        connect(last_, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
        connect(play_, &QPushButton::clicked, this, [this] { togglePlayback(); });
        connect(stop_, &QPushButton::clicked, this, [this] {
            stop(exporting_ ? "MP4 export cancelled." : "Animation stopped.");
        });
        connect(export_, &QPushButton::clicked, this, [this] { chooseDestination(); });

        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, [this] { advance(); });
        connect(&checker_, &QFutureWatcher<Preflight>::finished, this, [this] {
            checking_ = false;
            if (phase_ == Phase::Stopping) return completeStop();
            const auto check = checker_.result();
            if (!check.error.isEmpty()) return stop(check.error);
            range_ = check.range;
            phase_ = Phase::Frames;
            if (!paused_) advance();
        });
        connect(&loader_, &QFutureWatcher<Result>::finished, this, [this] {
            reading_ = false;
            if (phase_ == Phase::Stopping) return completeStop();
            auto result = loader_.result();
            if (!result.error.isEmpty())
                return stop(frameError(static_cast<int>(next_), result.error));
            if (paused_) pending_ = std::move(result);
            else present(result);
        });
        movie_.onProgress = [this](int count) {
            if (phase_ == Phase::Encoding) progress_->setValue(count);
        };
        movie_.onFinished = [this](MovieExport::Outcome outcome, const QString& detail) {
            if (phase_ == Phase::Stopping) {
                encoding_ = false;
                completeStop();
            } else if (outcome == MovieExport::Outcome::Saved) {
                finish("Saved " + QFileInfo(destination_).fileName() + ".");
            } else {
                finish("MP4 export failed. " + detail);
            }
        };
        refresh();
    }

    void refresh()
    {
        available_ = window_.document_ && !window_.opening_ && !window_.reading_ &&
                     window_.displayed_.selection.frame >= 0;
        if (phase_ == Phase::Idle && available_) {
            const auto active = window_.displayed_.selection;
            if (window_.document_ != listedDocument_.lock() || active.frame != listedSelection_.frame ||
                active.dataset != listedSelection_.dataset || active.component != listedSelection_.component) {
                listedDocument_ = window_.document_;
                listedSelection_ = active;
                const auto& document = *window_.document_;
                sequence_ = ccxvis::compatibleFrames(document.frames(), document.datasets(), active);
                const QSignalBlocker blockFirst(first_), blockLast(last_);
                first_->clear();
                last_->clear();
                for (const auto& match : sequence_.frames) {
                    const auto label = window_.step_->itemText(match.frame);
                    first_->addItem(label);
                    last_->addItem(label);
                }
                first_->setCurrentIndex(0);
                last_->setCurrentIndex(last_->count() - 1);
                summary_->setText(window_.displayed_.name + "\n" +
                    QString("%1 matching saved frames · %2 missing this field or component")
                        .arg(sequence_.frames.size()).arg(sequence_.missing));
                status_->clear();
            }
        }
        updateButtons();
    }

    void reject() override
    {
        stop(exporting_ ? "MP4 export cancelled." : "Animation stopped.");
        QDialog::reject();
    }

    void requestShutdown()
    {
        if (cancelled_) cancelled_->store(true);
    }

private:
    enum class Phase { Idle, Preflight, Frames, Encoding, Stopping };
    struct Preflight {
        std::optional<std::array<double, 2>> range;
        QString error;
    };

    bool validRange() const { return first_->currentIndex() >= 0 && first_->currentIndex() < last_->currentIndex(); }

    void updateButtons()
    {
        const bool idle = phase_ == Phase::Idle;
        const bool canPause = !exporting_ && (phase_ == Phase::Preflight || phase_ == Phase::Frames);
        first_->setEnabled(idle && available_);
        last_->setEnabled(idle && available_);
        fps_->setEnabled(idle && available_);
        play_->setEnabled(canPause || (idle && available_ && validRange()));
        play_->setText(canPause && !paused_ ? "Pause" : "Play");
        stop_->setEnabled(!idle && phase_ != Phase::Stopping);
        stop_->setText(exporting_ ? "Cancel" : "Stop");
        export_->setEnabled(idle && available_ && validRange());
        if (idle && available_ && !validRange())
            status_->setText("Choose an inclusive range with at least two matching frames.");
        else if (idle && !available_)
            status_->setText("Waiting for the active contour field.");
    }

    void togglePlayback()
    {
        if (phase_ == Phase::Idle) {
            begin({});
            return;
        }
        paused_ = !paused_;
        if (paused_) {
            timer_.stop();
            status_->setText("Paused");
        } else if (phase_ == Phase::Preflight) {
            status_->setText("Checking the selected frames…");
        } else if (pending_) {
            auto result = std::move(*pending_);
            pending_.reset();
            present(result);
        } else {
            status_->setText(next_ ? labels_.at(next_ - 1) : QString("Reading the first frame…"));
            if (!reading_) timer_.start(framePeriod_);
        }
        updateButtons();
    }

    void chooseDestination()
    {
        const auto program = QStandardPaths::findExecutable("ffmpeg");
        if (program.isEmpty()) {
            status_->setText("FFmpeg was not found on PATH.");
            return;
        }
        if (!window_.viewport_->isValid()) {
            status_->setText("MP4 export needs a visible viewport with a working OpenGL context.");
            return;
        }
        QFileDialog picker(this, "Export MP4", "results.mp4", "MP4 video (*.mp4)");
        picker.setAcceptMode(QFileDialog::AcceptSave);
        picker.setDefaultSuffix("mp4");
        if (picker.exec() != QDialog::Accepted) return;
        encoder_ = program;
        begin(picker.selectedFiles().front());
    }

    void begin(const QString& destination)
    {
        if (phase_ != Phase::Idle || !available_ || !validRange()) return;
        const auto first = first_->currentIndex(), last = last_->currentIndex();
        selected_.assign(sequence_.frames.begin() + first, sequence_.frames.begin() + last + 1);
        labels_.clear();
        for (const auto& selection : selected_) labels_ << window_.step_->itemText(selection.frame);
        if (!destination.isEmpty()) {
            try {
                movie_.prepare(destination, static_cast<int>(selected_.size()), fps_->value());
            } catch (const std::exception& error) {
                status_->setText(QString::fromLocal8Bit(error.what()));
                return;
            }
        }

        // Finish any section edit queued before controls are locked.
        if (window_.sectionTimer_.isActive()) window_.applySection(true);
        exporting_ = !destination.isEmpty();
        destination_ = destination;
        phase_ = Phase::Preflight;
        paused_ = false;
        next_ = 0;
        captureSize_ = {};
        deform_ = window_.deform_->isChecked();
        scale_ = window_.scale_->value();
        framePeriod_ = qRound(1000. / fps_->value());
        cancelled_ = std::make_shared<std::atomic_bool>(false);
        window_.animationActive_ = true;
        window_.viewport_->beginAnimation();
        window_.updateControls();
        status_->setText("Checking the selected frames…");
        progress_->setRange(0, 0);
        progress_->show();

        checking_ = true;
        checker_.setFuture(QtConcurrent::run(&window_.reader_, [document = window_.document_,
                           selections = selected_, labels = labels_, deformed = deform_, scale = scale_,
                           cancelled = cancelled_] {
            ccxvis::ScalarExtent extent;
            for (std::size_t i = 0; i < selections.size(); ++i) {
                if (cancelled->load()) return Preflight{};
                const auto result = readResult(*document, selections[i], deformed);
                if (!result.error.isEmpty())
                    return Preflight{{}, labels.at(i) + " — " + result.error};
                if (deformed && !ccxvis::deformPositions(document->mesh(), result.displacements, scale))
                    return Preflight{{}, labels.at(i) + " — Complete, usable displacements are required."};
                extent.include(result.scalars);
            }
            if (!extent.range()) return Preflight{{}, "The selected frames contain no finite contour values."};
            return Preflight{extent.range(), {}};
        }));
    }

    QString frameError(int index, const QString& error) const { return labels_.at(index) + " — " + error; }

    void advance()
    {
        if (phase_ != Phase::Frames || paused_ || reading_) return;
        if (next_ == selected_.size()) {
            if (!exporting_) return finish("Playback complete.");
            phase_ = Phase::Encoding;
            encoding_ = true;
            status_->setText("Encoding MP4…");
            progress_->setValue(0);
            updateButtons();
            movie_.encode(encoder_);
            return;
        }
        frameClock_.restart();
        reading_ = true;
        loader_.setFuture(QtConcurrent::run(&window_.reader_, [document = window_.document_,
                                selection = selected_[next_], deformed = deform_] {
            return readResult(*document, selection, deformed);
        }));
    }

    void present(const Result& result)
    {
        auto* viewport = window_.viewport_;
        if (deform_) {
            std::vector<ccxvis::Point> positions;
            if (!ccxvis::deformPositions(window_.document_->mesh(), result.displacements, scale_, &positions))
                return stop(frameError(static_cast<int>(next_), "Complete, usable displacements are required."));
            viewport->setPositions(positions);
            viewport->markNode(window_.picked_);
        }
        presented_ = true;
        viewport->setResult(result.scalars, result.name, range_);
        viewport->setAnimationLabel(labels_.at(next_));
        const auto label = labels_.at(next_);
        if (exporting_) {
            try {
                captureSize_ = viewport->saveImage(movie_.framePath(static_cast<int>(next_)), captureSize_);
            } catch (const std::exception& error) {
                return stop(frameError(static_cast<int>(next_), QString::fromLocal8Bit(error.what())));
            }
        }
        ++next_;
        status_->setText(exporting_ ? "Captured " + label : label);
        progress_->setRange(0, static_cast<int>(selected_.size()));
        progress_->setValue(static_cast<int>(next_));
        // Never queue or skip frames. A slow read or render extends the playback time.
        const auto remaining = std::max<qint64>(0, framePeriod_ - frameClock_.elapsed());
        const int delay = next_ == selected_.size() ? framePeriod_ : static_cast<int>(remaining);
        timer_.start(exporting_ ? 0 : delay);
    }

    void restoreResults()
    {
        auto* viewport = window_.viewport_;
        if (presented_) {
            if (deform_) {
                const auto& mesh = window_.document_->mesh();
                std::vector<ccxvis::Point> positions;
                if (!window_.warped_ || !ccxvis::deformPositions(mesh, window_.displayed_.displacements, scale_, &positions))
                    positions = ccxvis::undeformedPositions(mesh);
                viewport->setPositions(positions);
                viewport->markNode(window_.picked_);
            }
            viewport->setResult(window_.displayed_.scalars, window_.displayed_.name);
            presented_ = false;
        }
        viewport->setAnimationLabel({});
    }

    void stop(const QString& message)
    {
        if (phase_ == Phase::Idle || phase_ == Phase::Stopping) return;
        phase_ = Phase::Stopping;
        terminal_ = message;
        cancelled_->store(true);
        timer_.stop();
        pending_.reset();
        restoreResults();
        if (checking_ || reading_ || encoding_) status_->setText("Stopping…");
        updateButtons();
        if (exporting_) movie_.cancel();
        completeStop();
    }

    void completeStop()
    {
        if (phase_ == Phase::Stopping && !checking_ && !reading_ && !encoding_) finish(terminal_);
    }

    void finish(const QString& message)
    {
        const auto outcome = message;
        restoreResults();
        window_.viewport_->endAnimation();
        phase_ = Phase::Idle;
        exporting_ = false;
        encoding_ = false;
        window_.animationActive_ = false;
        progress_->hide();
        window_.updateControls();
        status_->setText(outcome);
        window_.statusBar()->showMessage(outcome, 8000);
    }

    MainWindow& window_;
    QComboBox* first_;
    QComboBox* last_;
    QSpinBox* fps_;
    QPushButton* play_;
    QPushButton* stop_;
    QPushButton* export_;
    QLabel* summary_;
    QLabel* status_;
    QProgressBar* progress_;
    QTimer timer_;
    QElapsedTimer frameClock_;
    QFutureWatcher<Preflight> checker_;
    QFutureWatcher<Result> loader_;
    MovieExport movie_;
    Phase phase_ = Phase::Idle;
    bool available_ = false;
    bool paused_ = false;
    bool exporting_ = false;
    bool deform_ = false;
    bool checking_ = false;
    bool reading_ = false;
    bool encoding_ = false;
    bool presented_ = false;
    double scale_ = 1.;
    int framePeriod_ = 0;
    std::size_t next_ = 0;
    QSize captureSize_;
    std::weak_ptr<ccxvis::FrdDocument> listedDocument_;
    Selection listedSelection_;
    ccxvis::ContourSequence sequence_;
    std::vector<Selection> selected_;
    QStringList labels_;
    QString encoder_, destination_, terminal_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::optional<std::array<double, 2>> range_;
    std::optional<Result> pending_;
};

void MainWindow::showAnimation()
{
    if (!animation_) animation_ = new Animation(*this);
    animation_->refresh();
    animation_->show();
    animation_->raise();
    animation_->activateWindow();
}

void MainWindow::refreshAnimation()
{
    if (animation_) animation_->refresh();
}

void MainWindow::stopAnimation()
{
    if (animation_) animation_->reject();
}

void MainWindow::shutdownAnimation()
{
    if (animation_) animation_->requestShutdown();
}
