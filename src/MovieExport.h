// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <QProcess>
#include <QTemporaryDir>

#include <functional>
#include <memory>

class MovieExport {
public:
    enum class Outcome { Saved, Failed, Cancelled };
    std::function<void(int)> onProgress;
    std::function<void(Outcome, const QString&)> onFinished;

    MovieExport();
    ~MovieExport();
    void prepare(const QString& destination, int frames, int fps);
    QString framePath(int index) const;
    void encode(const QString& program);
    void cancel();

private:
    void startEncoder();
    void readProgress();
    void readDiagnostics();
    void finish(Outcome outcome, const QString& detail = {});

    std::unique_ptr<QTemporaryDir> staging_;
    QString destination_;
    QString program_;
    int frames_ = 0;
    int fps_ = 0;
    bool cancelled_ = false;
    bool useOpenH264_ = false;
    QByteArray progress_;
    QByteArray diagnostics_;
    QProcess process_;
};
