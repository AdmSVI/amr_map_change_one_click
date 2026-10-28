#pragma once
#include "FleetClient.h"
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QTimer>
#include <memory>

class QLockFile;

class MapAgent final : public QObject {
    Q_OBJECT
public:
    explicit MapAgent(QObject *parent = nullptr);
    ~MapAgent() override;
    bool configure(const QString &path, QString *error);
    bool start(QString *error);

private:
    void tick();
    void claim();
    void run(const QString &id, QByteArray bytes);
    void downloadNext();
    void reportNext();
    void complete(const QString &id, const QJsonObject &result);
    void save();

    std::unique_ptr<FleetClient> m_client;
    std::unique_ptr<QLockFile> m_lock;
    QTimer m_timer;
    QMap<QString, QJsonObject> m_jobs;
    QHash<QString, QByteArray> m_maps;
    QString m_stateFile;
    int m_concurrency = 1;
    int m_pollMs = 10000;
    bool m_claiming = false;
    bool m_downloading = false;
    bool m_reporting = false;
};
