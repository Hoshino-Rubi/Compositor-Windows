#include "MainWindow.h"
#include "Renderer.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QImageWriter>
#include <cstdio>
int main(int argc, char **argv) {
    Q_INIT_RESOURCE(translations);
    Q_INIT_RESOURCE(icons);
    Q_INIT_RESOURCE(controls);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Compositor");
    QCoreApplication::setApplicationName("Compositor");
    QCoreApplication::setApplicationVersion("0.3.2");
    QApplication::setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("Compositor native Windows image editor");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("project", "Open a .comp directory, PSD/PSB, RAW or image.");
    parser.addOption({"sample", "Open the sample composition."});
    parser.addOption({"language", "Set interface language (en, zh-CN, zh-TW).", "locale"});
    parser.addOption({"capture-size", "Window size for app capture.", "WxH"});
    parser.addOption({"capture-tool", "Tool to show in app capture.", "tool"});
    parser.addOption({"capture", "Save a screenshot of this application and exit.", "png"});
    parser.addOption({"render-project", "Render a .comp project to PNG and exit.", "directory"});
    parser.addOption({"output", "Output path for --render-project.", "png"});
    parser.process(app);
    if (parser.isSet("render-project")) {
        try {
            const auto project = comp::readProject(parser.value("render-project"));
            const auto image = comp::render(project.state);
            QImageWriter writer(parser.value("output"), "png");
            if (!writer.write(image))
                throw comp::Error("fileError");
            return 0;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    comp::MainWindow window;
    if (parser.isSet("language"))
        window.localization()->setLanguage(parser.value("language"));
    if (parser.isSet("capture"))
        window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    if (parser.isSet("sample"))
        window.sample();
    else if (!parser.positionalArguments().isEmpty())
        window.openProject(QFileInfo(parser.positionalArguments().first()).absoluteFilePath());
    if (parser.isSet("capture")) {
        if (parser.isSet("capture-size")) {
            const auto parts = parser.value("capture-size").split('x');
            if (parts.size() == 2) {
                const int w = parts[0].toInt(), h = parts[1].toInt();
                if (w >= 900 && w <= 3000 && h >= 600 && h <= 2000)
                    window.resize(w, h);
            }
        }
        if (parser.isSet("capture-tool")) {
            auto *action = window.findChild<QAction *>(parser.value("capture-tool") + "Tool");
            if (action)
                action->trigger();
        }
        window.canvas()->fit();
        const QString file = QFileInfo(parser.value("capture")).absoluteFilePath();
        QDir().mkpath(QFileInfo(file).absolutePath());
        QTimer::singleShot(2000, &window, [&window, file] {
            const bool saved = window.grab().save(file);
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
