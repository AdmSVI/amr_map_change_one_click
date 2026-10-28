#include "MapAgent.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <cstdio>

namespace {
QFile logFile;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("amr-map-agent");
    QCoreApplication::setApplicationVersion("0.3.0");
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        const auto line = QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + message;
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        std::fflush(stderr);
        if (logFile.isOpen()) {
            logFile.write(line.toUtf8() + '\n');
            logFile.flush();
        }
    });
    QCommandLineParser parser;
    parser.setApplicationDescription("Fleet map rollout agent; no GUI or inbound HTTP server");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"config", "INI configuration file", "path", "agent.ini"});
    parser.addOption({"check-config", "Validate configuration without network or robot access"});
    parser.addOption({"log-file", "Append UTC log lines to this file", "path"});
    parser.process(app);
    if (parser.isSet("log-file")) {
        logFile.setFileName(parser.value("log-file"));
        if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
            qCritical() << "Cannot open log file";
            return 1;
        }
    }
    MapAgent agent;
    QString error;
    if (!agent.configure(parser.value("config"), &error)) { qCritical() << error; return 1; }
    if (parser.isSet("check-config")) { qInfo() << "Configuration valid"; return 0; }
    if (!agent.start(&error)) { qCritical() << error; return 1; }
    return app.exec();
}
