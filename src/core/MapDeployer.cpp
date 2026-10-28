#include "MapDeployer.h"
#include "protocol/RbkProtocol.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

MapDeployer::MapDeployer(QObject *parent) : QObject(parent) {}

void MapDeployer::abort()
{
    finish("failed", "Claim operation deadline reached");
}

void MapDeployer::query(quint16 port, quint16 command, const QByteArray &payload,
                        std::function<void(QJsonObject)> callback, int timeoutMs)
{
    if (m_finished) return;
    RequestJob::start(this, m_robot.host, port, command, payload,
        [this, callback](RequestResult response) {
            if (m_finished) return;
            if (!response.ok) { finish("failed", response.error); return; }
            const auto doc = QJsonDocument::fromJson(response.payload);
            if (!doc.isObject()) { finish("failed", "Invalid JSON response"); return; }
            const auto json = doc.object();
            if (json.value("ret_code").toInt(0) != 0) {
                finish("failed", json.value("err_msg").toString("Robot rejected request"));
                return;
            }
            callback(json);
        }, timeoutMs);
}

void MapDeployer::deploy(const Robot &robot, const QByteArray &bytes,
                         const QString &md5, bool switchAfterUpload)
{
    if (m_started) return;
    m_started = true;
    m_robot = robot;
    m_bytes = bytes;
    m_md5 = md5.toLower();
    m_switch = switchAfterUpload;
    if (!QJsonDocument::fromJson(bytes).isObject() ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex() != m_md5.toLatin1()) {
        finish("failed", "Invalid map file or local MD5 mismatch");
        return;
    }
    query(robot.statusPort, RbkProtocol::QueryMap, {}, [this](QJsonObject json) {
        m_result.mapBefore = json.value("current_map").toString();
        m_result.mapAfter = m_result.mapBefore;
        m_result.md5After = json.value("current_map_md5").toString();
        emit currentMap(m_result.mapBefore, m_result.md5After);
        if (m_result.md5After.compare(m_md5, Qt::CaseInsensitive) == 0) {
            finish("done", "Current map already matches the requested MD5");
            return;
        }
        checkIdle([this] { upload(); });
    });
}

void MapDeployer::checkIdle(std::function<void()> next)
{
    emit progress(QStringLiteral("检查导航状态…"), "Checking navigation status...");
    query(m_robot.statusPort, RbkProtocol::QueryTask,
          QJsonDocument(QJsonObject{{"simple", true}}).toJson(QJsonDocument::Compact),
          [this, next](QJsonObject json) {
        const int status = json.value("task_status").toInt(-1);
        emit navigationStatus(status);
        if (status >= 1 && status <= 3) {
            finish("busy", "Robot is waiting, running or suspended; no map switch performed");
        } else if (status < 0 || status > 6) {
            finish("failed", "Unknown navigation status; no map switch performed");
        } else {
            next();
        }
    });
}

void MapDeployer::upload()
{
    emit progress(QStringLiteral("上传地图…"), "Uploading map...");
    query(m_robot.configPort, RbkProtocol::UploadMap, m_bytes, [this](QJsonObject) {
        QTimer::singleShot(1500, this, [this] { verifyUpload(); });
    }, 120000);
}

void MapDeployer::verifyUpload(int attempt)
{
    emit progress(QStringLiteral("按 MD5 查找地图…"), "Finding the uploaded map by MD5...");
    query(m_robot.statusPort, RbkProtocol::QueryMap, {}, [this, attempt](QJsonObject json) {
        QStringList files;
        for (const auto &value : json.value("map_files_info").toArray()) {
            const auto name = value.toObject().value("name").toString().trimmed();
            if (!name.isEmpty()) files.append(name);
        }
        if (files.isEmpty()) {
            for (const auto &value : json.value("maps").toArray()) {
                auto name = value.toString().trimmed();
                if (!name.isEmpty()) {
                    if (!name.endsWith(".smap", Qt::CaseInsensitive)) name += ".smap";
                    files.append(name);
                }
            }
        }
        files.removeDuplicates();
        if (files.isEmpty()) {
            if (attempt >= 14) { finish("failed", "Robot map list is empty"); return; }
            QTimer::singleShot(2000, this, [this, attempt] { verifyUpload(attempt + 1); });
        } else {
            findMap(files, attempt);
        }
    });
}

void MapDeployer::findMap(const QStringList &files, int attempt, int fallback)
{
    if (m_finished) return;
    QJsonArray names;
    if (fallback < 0) {
        for (const auto &file : files) names.append(file);
    } else {
        names.append(files.at(fallback));
    }
    // A stale stored-map entry may reject the batch: try files individually.
    RequestJob::start(this, m_robot.host, m_robot.statusPort, RbkProtocol::QueryMapMd5,
        QJsonDocument(QJsonObject{{"map_names", names}}).toJson(QJsonDocument::Compact),
        [this, files, attempt, fallback](RequestResult response) {
            if (m_finished) return;
            if (!response.ok) { finish("failed", response.error); return; }
            const auto doc = QJsonDocument::fromJson(response.payload);
            if (!doc.isObject()) { finish("failed", "Invalid MD5 response"); return; }
            const auto json = doc.object();
            if (json.value("ret_code").toInt(0) == 0) {
                for (const auto &value : json.value("map_info").toArray()) {
                    const auto info = value.toObject();
                    if (info.value("md5").toString().compare(m_md5, Qt::CaseInsensitive) != 0)
                        continue;
                    m_remoteName = info.value("name").toString().trimmed();
                    if (m_remoteName.isEmpty()) continue;
                    if (m_remoteName.endsWith(".smap", Qt::CaseInsensitive)) m_remoteName.chop(5);
                    if (m_switch) checkIdle([this] { switchMap(); });
                    else finish("done", "Upload and MD5 verification succeeded");
                    return;
                }
            } else if (fallback < 0) {
                findMap(files, attempt, 0);
                return;
            }
            if (fallback >= 0 && fallback + 1 < files.size()) {
                findMap(files, attempt, fallback + 1);
            } else if (attempt < 14) {
                QTimer::singleShot(2000, this, [this, attempt] { verifyUpload(attempt + 1); });
            } else {
                finish("failed", "No stored map matches the local MD5");
            }
        });
}

void MapDeployer::switchMap()
{
    emit progress(QStringLiteral("切换地图…"), "Switching map...");
    query(m_robot.controlPort, RbkProtocol::LoadMap,
          QJsonDocument(QJsonObject{{"map_name", m_remoteName}}).toJson(QJsonDocument::Compact),
          [this](QJsonObject) { verifyCurrent(); }, 60000);
}

void MapDeployer::verifyCurrent(int attempt)
{
    emit progress(QStringLiteral("验证当前地图…"), "Verifying current map...");
    query(m_robot.statusPort, RbkProtocol::QueryMap, {}, [this, attempt](QJsonObject json) {
        m_result.mapAfter = json.value("current_map").toString();
        m_result.md5After = json.value("current_map_md5").toString();
        emit currentMap(m_result.mapAfter, m_result.md5After);
        auto name = m_result.mapAfter;
        if (name.endsWith(".smap", Qt::CaseInsensitive)) name.chop(5);
        if (name.compare(m_remoteName, Qt::CaseInsensitive) == 0 &&
            m_result.md5After.compare(m_md5, Qt::CaseInsensitive) == 0) {
            finish("done", "Upload, verification and switch succeeded");
        } else if (attempt < 14) {
            QTimer::singleShot(2000, this, [this, attempt] { verifyCurrent(attempt + 1); });
        } else {
            finish("failed", "Current map verification mismatch");
        }
    });
}

void MapDeployer::finish(const QString &result, const QString &message)
{
    if (m_finished) return;
    m_finished = true;
    m_result.result = result;
    m_result.message = message;
    emit finished(m_result);
}
