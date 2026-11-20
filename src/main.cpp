// SPDX-License-Identifier: GPL-2.0-only
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>

int main(int argc, char** argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    application.setApplicationName("CCXVis");
    application.setApplicationVersion("0.1.0");
    application.setStyleSheet(
        "QToolBar {spacing: 5px; padding: 5px;} "
        "QToolButton {padding: 6px 10px;}"
    );
    QCommandLineParser command;
    command.setApplicationDescription("Results visualization for CalculiX");
    command.addHelpOption();
    command.addVersionOption();
    command.addPositionalArgument("results", "FRD file to open", "[results.frd]");
    command.process(application);
    if (command.positionalArguments().size() > 1) command.showHelp(1);

    MainWindow window;
    window.show();
    if (!command.positionalArguments().isEmpty())
        window.openFile(command.positionalArguments().first());
    return application.exec();
}
