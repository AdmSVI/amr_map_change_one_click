#include "core/MapDeployer.h"
#include "protocol/RbkProtocol.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QDebug>
#include <memory>

// Real asynchronous TCP, including fragmented replies; no physical robot.
class RobotStub : public QObject {
public:
    QTcpServer server;
    QByteArray bytes = R"({"header":{"mapName":"TEST"},"advancedPointList":[]})";
    QString md5 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
    bool switched = false;
    bool uploaded = false;
    bool alreadyCurrent = false;
    bool staleEntry = false;
    bool busyBeforeSwitch = false;
    int taskStatus = 0;
    int taskQueries = 0;
    int switchCommands = 0;
    int uploadCommands = 0;
    bool silent = false;

    RobotStub()
    {
        if (!server.listen(QHostAddress::LocalHost)) qFatal("Cannot start robot stub");
        connect(&server, &QTcpServer::newConnection, this, [this] {
            auto *socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                *buffer += socket->readAll();
                if (buffer->size() < RbkProtocol::HeaderSize) return;
                RbkProtocol::Header header;
                QString error;
                if (!RbkProtocol::decodeHeader(*buffer, &header, &error)) qFatal("Bad stub header");
                if (buffer->size() < RbkProtocol::HeaderSize + header.payloadLength) return;
                if (silent) return;
                QJsonObject reply;
                switch (header.command) {
                case RbkProtocol::QueryMap:
                    reply = {{"current_map", switched || alreadyCurrent ? "robot-name" : "OLD"},
                             {"current_map_md5", switched || alreadyCurrent ? md5 : "old-md5"},
                             {"maps", QJsonArray{"robot-name", "stale"}}};
                    break;
                case RbkProtocol::QueryTask:
                    ++taskQueries;
                    reply = {{"task_status", busyBeforeSwitch && taskQueries > 1 ? 2 : taskStatus}};
                    break;
                case RbkProtocol::UploadMap:
                    ++uploadCommands;
                    uploaded = buffer->mid(RbkProtocol::HeaderSize) == bytes;
                    break;
                case RbkProtocol::QueryMapMd5: {
                    const auto request = QJsonDocument::fromJson(buffer->mid(RbkProtocol::HeaderSize)).object();
                    const auto names = request.value("map_names").toArray();
                    if (staleEntry && names.size() > 1) reply = {{"ret_code", 1}};
                    else reply = {{"map_info", QJsonArray{QJsonObject{{"name", "robot-name.smap"},
                                                                       {"md5", md5}}}}};
                    break;
                }
                case RbkProtocol::LoadMap:
                    ++switchCommands;
                    switched = QJsonDocument::fromJson(buffer->mid(RbkProtocol::HeaderSize))
                                   .object().value("map_name").toString() == "robot-name";
                    break;
                default: qFatal("Unexpected robot command");
                }
                const auto packet = RbkProtocol::encodeRequest(header.sequence,
                    RbkProtocol::expectedResponse(header.command), QJsonDocument(reply).toJson());
                socket->write(packet.left(7));
                QTimer::singleShot(5, socket, [socket, packet] { socket->write(packet.mid(7)); });
                buffer->clear();
            });
        });
    }
};

static DeployResult run(RobotStub &stub, bool switchAfter = true, bool invalid = false)
{
    MapDeployer deployer;
    QEventLoop loop;
    DeployResult result;
    bool finished = false;
    QObject::connect(&deployer, &MapDeployer::finished, &loop, [&](DeployResult value) {
        result = value;
        finished = true;
        loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [] { qFatal("Deployment test timed out"); });
    timeout.start(10000);
    Robot robot;
    robot.host = "127.0.0.1";
    robot.statusPort = robot.controlPort = robot.configPort = stub.server.serverPort();
    deployer.deploy(robot, stub.bytes, invalid ? "bad" : stub.md5, switchAfter);
    if (!finished) loop.exec();
    return result;
}

static void check(bool value, const char *message)
{
    if (!value) qFatal("%s", message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    {
        RobotStub stub;
        stub.staleEntry = true;
        auto result = run(stub);
        check(result.result == "done" && result.md5After == stub.md5 && stub.uploaded && stub.switched,
              "Upload, stale MD5 fallback, remote name resolution or switch failed");
    }
    for (int state : {1, 2, 3}) {
        RobotStub stub;
        stub.taskStatus = state;
        check(run(stub).result == "busy" && stub.uploadCommands == 0 && stub.switchCommands == 0,
              "Busy robot was modified");
    }
    {
        RobotStub stub;
        stub.taskStatus = 99;
        check(run(stub).result == "failed" && stub.uploadCommands == 0, "Unknown status accepted");
    }
    {
        RobotStub stub;
        stub.busyBeforeSwitch = true;
        check(run(stub).result == "busy" && stub.uploaded && stub.switchCommands == 0,
              "Busy status was not rechecked before switch");
    }
    {
        RobotStub stub;
        stub.alreadyCurrent = true;
        check(run(stub).result == "done" && stub.uploadCommands == 0 && stub.switchCommands == 0,
              "Identical current map was uploaded again");
    }
    {
        RobotStub stub;
        check(run(stub, false).result == "done" && stub.uploaded && stub.switchCommands == 0,
              "Upload-only switched the map");
    }
    {
        RobotStub stub;
        check(run(stub, true, true).result == "failed" && stub.uploadCommands == 0,
              "Invalid local MD5 reached the robot");
    }
    {
        RobotStub stub;
        stub.silent = true;
        QEventLoop loop;
        bool timedOut = false;
        RequestJob::start(&loop, "127.0.0.1", stub.server.serverPort(), RbkProtocol::QueryMap, {},
            [&](RequestResult result) { timedOut = !result.ok; loop.quit(); }, 100);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        check(timedOut, "Silent robot did not time out");
    }
    qInfo() << "MapDeployer: 10 scenarios passed";
}
