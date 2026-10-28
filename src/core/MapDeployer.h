#pragma once

#include "core/Robot.h"
#include "network/RequestJob.h"
#include <QJsonObject>
#include <QStringList>

struct DeployResult {
    QString result;
    QString message;
    QString mapBefore;
    QString mapAfter;
    QString md5After;
};

// One instance owns one robot operation. The caller limits concurrency.
class MapDeployer final : public QObject {
    Q_OBJECT
public:
    explicit MapDeployer(QObject *parent = nullptr);
    void deploy(const Robot &robot, const QByteArray &bytes, const QString &md5,
                bool switchAfterUpload);
    void abort();

signals:
    void progress(QString chinese, QString english);
    void navigationStatus(int status);
    void currentMap(QString name, QString md5);
    void finished(DeployResult result);

private:
    void query(quint16 port, quint16 command, const QByteArray &payload,
               std::function<void(QJsonObject)> callback, int timeoutMs = 10000);
    void checkIdle(std::function<void()> next);
    void upload();
    void verifyUpload(int attempt = 0);
    void findMap(const QStringList &files, int attempt, int fallback = -1);
    void switchMap();
    void verifyCurrent(int attempt = 0);
    void finish(const QString &result, const QString &message);

    Robot m_robot;
    QByteArray m_bytes;
    QString m_md5;
    QString m_remoteName;
    DeployResult m_result;
    bool m_switch = false;
    bool m_started = false;
    bool m_finished = false;
};
