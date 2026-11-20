// SPDX-License-Identifier: GPL-2.0-only
#include "MovieExport.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace {
constexpr auto encodedName = "encoded.mp4";
}

MovieExport::MovieExport()
{
    QObject::connect(&process_, &QProcess::readyReadStandardOutput, &process_, [this] { readProgress(); });
    QObject::connect(&process_, &QProcess::readyReadStandardError, &process_, [this] { readDiagnostics(); });
    QObject::connect(&process_, &QProcess::errorOccurred, &process_, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(cancelled_ ? Outcome::Cancelled : Outcome::Failed,
                   "Could not start FFmpeg. " + process_.errorString());
    });
    QObject::connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &process_,
        [this](int code, QProcess::ExitStatus status) {
            readProgress();
            readDiagnostics();
            if (!staging_) return;
            if (cancelled_) return finish(Outcome::Cancelled);
            if (status == QProcess::NormalExit && code != 0 && !useOpenH264_ &&
                diagnostics_.contains("Unknown encoder 'libx264'")) {
                useOpenH264_ = true;
                progress_.clear();
                diagnostics_.clear();
                startEncoder();
                return;
            }
            const auto source = staging_->filePath(encodedName);
            if (status != QProcess::NormalExit || code != 0 || QFileInfo(source).size() <= 0) {
                const auto detail = QString::fromLocal8Bit(diagnostics_).trimmed();
                return finish(Outcome::Failed, detail.isEmpty() ? "FFmpeg did not produce a movie." : detail);
            }
            // The staging directory is beside the destination, so rename publishes atomically.
            std::error_code error;
            std::filesystem::rename(QFile::encodeName(source).constData(),
                                    QFile::encodeName(destination_).constData(), error);
            if (error) return finish(Outcome::Failed, "Could not publish the MP4. " +
                                      QString::fromLocal8Bit(error.message().c_str()));
            finish(Outcome::Saved);
        });
}

MovieExport::~MovieExport()
{
    process_.disconnect();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished();
    }
}

void MovieExport::prepare(const QString& destination, int frames, int fps)
{
    if (staging_ || process_.state() != QProcess::NotRunning)
        throw std::logic_error("An MP4 export is already in progress");
    const QFileInfo file(destination);
    auto staging = std::make_unique<QTemporaryDir>(file.absolutePath() + "/.ccxvis-movie-XXXXXX");
    if (!staging->isValid()) throw std::runtime_error("Could not create temporary space beside the destination");
    destination_ = file.absoluteFilePath();
    frames_ = frames;
    fps_ = fps;
    cancelled_ = false;
    useOpenH264_ = false;
    progress_.clear();
    diagnostics_.clear();
    staging_ = std::move(staging);
}

QString MovieExport::framePath(int index) const
{
    return staging_->filePath(QString("frame-%1.png").arg(index, 8, 10, QChar('0')));
}

void MovieExport::encode(const QString& program)
{
    program_ = program;
    process_.setWorkingDirectory(staging_->path());
    startEncoder();
}

void MovieExport::startEncoder()
{
    QStringList arguments{"-hide_banner", "-loglevel", "error", "-nostdin", "-nostats", "-y",
                          "-progress", "pipe:1", "-framerate", QString::number(fps_),
                          "-start_number", "0", "-i", "frame-%08d.png", "-frames:v", QString::number(frames_),
                          "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-pix_fmt", "yuv420p"};
    if (useOpenH264_)
        arguments << "-c:v" << "libopenh264" << "-b:v" << "4M";
    else
        arguments << "-c:v" << "libx264";
    arguments << "-movflags" << "+faststart" << encodedName;
    process_.start(program_, arguments);
}

void MovieExport::cancel()
{
    if (!staging_) return;
    cancelled_ = true;
    if (process_.state() == QProcess::NotRunning) finish(Outcome::Cancelled);
    else process_.kill();
}

void MovieExport::readProgress()
{
    progress_ += process_.readAllStandardOutput();
    for (auto end = progress_.indexOf('\n'); end >= 0; end = progress_.indexOf('\n')) {
        const auto line = progress_.left(end);
        progress_.remove(0, end + 1);
        if (line.startsWith("frame=") && onProgress)
            onProgress(std::clamp(line.mid(6).toInt(), 0, frames_));
    }
}

void MovieExport::readDiagnostics()
{
    diagnostics_ = (diagnostics_ + process_.readAllStandardError()).right(8192);
}

void MovieExport::finish(Outcome outcome, const QString& detail)
{
    if (!staging_) return;
    staging_.reset();
    if (onFinished) onFinished(outcome, detail);
}
