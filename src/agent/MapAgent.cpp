#include "MapAgent.h"
#include "core/MapDeployer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QDebug>
#include <cstdlib>

MapAgent::MapAgent(QObject *parent) : QObject(parent)
{
    connect(&m_timer, &QTimer::timeout, this, &MapAgent::tick);
}

MapAgent::~MapAgent() = default;

bool MapAgent::configure(const QString &path, QString *error)
{
    QSettings config(path, QSettings::IniFormat);
    QUrl base(config.value("fleet/url").toString());
    QByteArray token = qgetenv("AMR_MAP_AGENT_TOKEN").trimmed();
    if (token.isEmpty()) token = config.value("fleet/token").toByteArray().trimmed();
    if (!QFileInfo::exists(path) || !base.isValid() || base.host().isEmpty() ||
        !base.userInfo().isEmpty() || base.hasQuery() || base.hasFragment() ||
        (base.path() != "" && base.path() != "/") ||
        (base.scheme() != "https" &&
         !(base.scheme() == "http" && config.value("fleet/allow_http", false).toBool())) ||
        token.isEmpty() || token.contains('\n') || token.contains('\r')) {
        *error = "Set a Fleet origin URL and token; plain HTTP requires fleet/allow_http=true";
        return false;
    }
    m_concurrency = config.value("agent/concurrency", 1).toInt();
    const int seconds = config.value("agent/poll_seconds", 10).toInt();
    if (m_concurrency < 1 || m_concurrency > 3 || seconds < 1 || seconds > 3600) {
        *error = "concurrency must be 1-3 and poll_seconds must be 1-3600";
        return false;
    }
    m_pollMs = seconds * 1000;
    const auto directory = QFileInfo(path).absoluteDir();
    m_stateFile = directory.absoluteFilePath(
        config.value("agent/state_file", "agent-state.json").toString());
    m_client = std::make_unique<FleetClient>(base, token, this);
    return true;
}

bool MapAgent::start(QString *error)
{
    if (!m_client) { *error = "Agent is not configured"; return false; }
    if (!QDir().mkpath(QFileInfo(m_stateFile).absolutePath())) {
        *error = "Cannot create state directory";
        return false;
    }
    m_lock = std::make_unique<QLockFile>(m_stateFile + ".lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) { *error = "Another agent owns this state file"; return false; }
    QFile file(m_stateFile);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) { *error = "Cannot read state file"; return false; }
        const auto doc = QJsonDocument::fromJson(file.readAll());
        // Windows cannot atomically replace the state file while this handle is open.
        file.close();
        if (!doc.isObject() || doc.object().value("version").toInt() != 1 ||
            !doc.object().value("jobs").isArray()) {
            *error = "Invalid state file; preserve it for investigation";
            return false;
        }
        for (const auto &value : doc.object().value("jobs").toArray()) {
            auto job = value.toObject();
            const auto id = job.value("id").toString();
            if (id.isEmpty() || job.value("claim_id").toString().isEmpty() || m_jobs.contains(id)) {
                *error = "Invalid or duplicate job in state file";
                return false;
            }
            if (!job.value("result_body").isObject()) {
                job["result_body"] = QJsonObject{{"result", "failed"},
                    {"message", "Agent restarted before completing verification"}};
            }
            m_jobs.insert(id, job);
        }
    }
    save();
    m_timer.start(m_pollMs);
    QTimer::singleShot(0, this, &MapAgent::tick);
    qInfo() << "Map agent started; concurrency" << m_concurrency;
    return true;
}

void MapAgent::save()
{
    QJsonArray jobs;
    for (const auto &job : m_jobs) jobs.append(job);
    QSaveFile file(m_stateFile);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"jobs", jobs}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        qCritical() << "Cannot persist agent state; stopping before further work";
        std::exit(2);
    }
}

void MapAgent::tick()
{
    reportNext();
    downloadNext();
    // Unacknowledged results occupy slots too, bounding the durable outbox.
    if (!m_claiming && m_jobs.size() < m_concurrency) claim();
}

void MapAgent::claim()
{
    m_claiming = true;
    const int free = m_concurrency - int(m_jobs.size());
    m_client->request("POST", m_client->url("/api/v1/map-rollouts/claim/"),
        QJsonDocument(QJsonObject{{"limit", free}}).toJson(QJsonDocument::Compact),
        [this, free](FleetResponse response) {
            m_claiming = false;
            if (response.status != 200 || !response.error.isEmpty()) {
                qWarning() << "Claim failed; HTTP" << response.status;
                return;
            }
            const auto doc = QJsonDocument::fromJson(response.body);
            if (!doc.isObject() || !doc.object().value("robots").isArray() ||
                doc.object().value("robots").toArray().size() > free) {
                qCritical() << "Invalid claim response";
                return;
            }
            const auto claimedAt = QDateTime::currentDateTimeUtc();
            for (const auto &value : doc.object().value("robots").toArray()) {
                auto job = value.toObject();
                const auto id = QString::number(job.value("id").toInteger());
                if (job.value("id").toInteger() <= 0 || m_jobs.contains(id) ||
                    job.value("claim_id").toString().isEmpty()) {
                    qCritical() << "Missing job/claim identifier; update Fleet before starting the agent";
                    continue;
                }
                job["id"] = id;
                auto deadline = claimedAt.addSecs(8 * 60);
                const auto leaseEnd = QDateTime::fromString(
                    job.value("claim_expires_at").toString(), Qt::ISODateWithMs);
                if (!leaseEnd.isValid()) {
                    job["result_body"] = QJsonObject{{"result", "failed"},
                        {"message", "Missing or invalid claim expiry; update Fleet"}};
                } else if (leaseEnd.addSecs(-60) < deadline) {
                    deadline = leaseEnd.addSecs(-60);
                }
                job["local_deadline"] = deadline.toString(Qt::ISODateWithMs);
                job["running"] = false;
                m_jobs.insert(id, job);
            }
            save();
            downloadNext();
        });
}

void MapAgent::downloadNext()
{
    if (m_downloading) return;
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (it->value("running").toBool() || it->value("result_body").isObject()) continue;
        const auto id = it.key();
        const auto job = it.value();
        const auto md5 = job.value("map_md5").toString().toLower();
        if (m_maps.contains(md5)) { run(id, m_maps.value(md5)); return; }
        m_downloading = true;
        m_client->request("GET", QUrl(job.value("file_url").toString()), {},
            [this, id, md5](FleetResponse response) {
                m_downloading = false;
                if (response.status != 200 || !response.error.isEmpty()) {
                    complete(id, {{"result", "failed"}, {"message", "Map download failed"}});
                } else if (QCryptographicHash::hash(response.body, QCryptographicHash::Md5).toHex()
                           != md5.toLatin1()) {
                    complete(id, {{"result", "failed"}, {"message", "Downloaded MD5 mismatch"}});
                } else {
                    m_maps.insert(md5, response.body);
                    run(id, response.body);
                }
                QTimer::singleShot(0, this, &MapAgent::downloadNext);
            });
        return;
    }
}

void MapAgent::run(const QString &id, QByteArray bytes)
{
    auto &job = m_jobs[id];
    const auto deadline = QDateTime::fromString(job.value("local_deadline").toString(), Qt::ISODateWithMs);
    // Fleet leases last ten minutes. Stop robot work after eight, including download time.
    const qint64 remaining = QDateTime::currentDateTimeUtc().msecsTo(deadline);
    QHostAddress host(job.value("ip").toString());
    if (remaining <= 0 || host.protocol() != QAbstractSocket::IPv4Protocol ||
        !job.value("switch").isBool()) {
        complete(id, {{"result", "failed"}, {"message", "Expired claim or invalid robot request"}});
        return;
    }
    job["running"] = true;
    save();
    auto *deployer = new MapDeployer(this);
    auto *timer = new QTimer(deployer);
    timer->setSingleShot(true);
    connect(deployer, &MapDeployer::progress, this, [id](QString, QString message) {
        qInfo().noquote() << id << message;
    });
    connect(deployer, &MapDeployer::finished, this, [this, id, deployer, timer](DeployResult result) {
        timer->stop();
        complete(id, {{"result", result.result}, {"message", result.message},
                     {"map_before", result.mapBefore}, {"map_after", result.mapAfter},
                     {"map_md5_after", result.md5After}});
        deployer->deleteLater();
    });
    connect(timer, &QTimer::timeout, deployer, &MapDeployer::abort);
    timer->start(int(remaining));
    Robot robot;
    robot.name = job.value("amr").toString();
    robot.host = job.value("ip").toString();
    deployer->deploy(robot, bytes, job.value("map_md5").toString(), job.value("switch").toBool());
    QTimer::singleShot(0, this, &MapAgent::downloadNext);
}

void MapAgent::complete(const QString &id, const QJsonObject &result)
{
    m_jobs[id]["result_body"] = result;
    save();
    qInfo().noquote() << id << result.value("result").toString() << result.value("message").toString();
    reportNext();
}

void MapAgent::reportNext()
{
    if (m_reporting) return;
    for (auto it = m_jobs.cbegin(); it != m_jobs.cend(); ++it) {
        if (!it->value("result_body").isObject()) continue;
        const auto id = it.key();
        auto body = it->value("result_body").toObject();
        body["claim_id"] = it->value("claim_id");
        m_reporting = true;
        m_client->request("POST", m_client->url("/api/v1/map-rollouts/robots/" + id + "/result/"),
            QJsonDocument(body).toJson(QJsonDocument::Compact), [this, id](FleetResponse response) {
                m_reporting = false;
                if ((response.status >= 200 && response.status < 300 && response.error.isEmpty()) ||
                    response.status == 409) {
                    m_jobs.remove(id);
                    save();
                    for (const auto &md5 : m_maps.keys()) {
                        bool needed = false;
                        for (const auto &job : m_jobs) {
                            if (job.value("map_md5").toString().compare(md5, Qt::CaseInsensitive) == 0)
                                needed = true;
                        }
                        if (!needed) m_maps.remove(md5);
                    }
                    QTimer::singleShot(0, this, &MapAgent::reportNext);
                } else {
                    qWarning() << "Result retained for retry; job" << id << "HTTP" << response.status;
                }
            });
        return;
    }
}
