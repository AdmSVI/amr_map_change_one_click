#pragma once
#include <QNetworkAccessManager>
#include <QUrl>
#include <functional>

struct FleetResponse {
    int status = 0;
    QByteArray body;
    QString error;
};

class FleetClient final : public QObject {
    Q_OBJECT
public:
    FleetClient(QUrl base, QByteArray token, QObject *parent = nullptr);
    void request(const QByteArray &method, const QUrl &url, const QByteArray &body,
                 std::function<void(FleetResponse)> callback);
    QUrl url(const QString &path) const;

private:
    QUrl m_base;
    QByteArray m_token;
    QNetworkAccessManager m_network;
};
