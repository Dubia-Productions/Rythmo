#include "rythmo/main_window.hpp"

#include <QApplication>
#include <QColor>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void isolatePortableQtPlugins() {
#ifdef Q_OS_WIN
    wchar_t executablePath[32768]{};
    constexpr DWORD pathCapacity = static_cast<DWORD>(sizeof(executablePath) / sizeof(executablePath[0]));
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
                                           pathCapacity);
    if (length > 0 && length < pathCapacity) {
        const QString appDir = QFileInfo(QString::fromWCharArray(executablePath,
                                                                static_cast<qsizetype>(length)))
                                   .absolutePath();
        const QByteArray pluginRoot = QDir::toNativeSeparators(appDir).toUtf8();
        const QByteArray platformPlugins = QDir::toNativeSeparators(appDir + QStringLiteral("/platforms")).toUtf8();
        qputenv("QT_PLUGIN_PATH", pluginRoot);
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", platformPlugins);
    }
#endif
}

void applyStudioPalette(QApplication& app) {
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app.setStyle(fusion);
    }
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#131315")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#EDEDEF")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#161618")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#1A1A1D")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#222226")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#EDEDEF")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#EDEDEF")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6E6E76")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#202024")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#EDEDEF")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#D4D4D8")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#323238")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#FFFFFF")));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#55555C")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#55555C")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#55555C")));
    app.setPalette(palette);
}
} // namespace

int main(int argc, char* argv[]) {
    isolatePortableQtPlugins();
    QApplication app(argc, argv);
    applyStudioPalette(app);
    QCoreApplication::setApplicationName("Rythmo by Dubia Productions");
    app.setWindowIcon(QIcon(QStringLiteral(":/app/rythmo.png")));
    QCoreApplication::setApplicationVersion(QStringLiteral(RYTHMO_VERSION));
    QCommandLineParser parser;
    parser.setApplicationDescription("Rythmo by Dubia Productions: studio de synchronisation et de production vocale.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("document", "Fichier DetX à ouvrir.", "[document.detx]");
    parser.process(app);

    const auto initial = parser.positionalArguments().isEmpty() ? QString{} : parser.positionalArguments().front();
    rythmo::MainWindow window(initial);
    window.show();
    return app.exec();
}
