#include "FleetClient.h"
#include <QNetworkReply>
#include <QNetworkRequest>

FleetClient::FleetClient(QUrl base, QByteArray token, QObject *parent)
    : QObject(parent), m_base(base), m_token(token) {}

QUrl FleetClient::url(const QString &path) const
{
    return m_base.resolved(QUrl(path));
}

void FleetClient::request(const QByteArray &method, const QUrl &url, const QByteArray &body,
                          std::function<void(FleetResponse)> callback)
{
    if (url.scheme() != m_base.scheme() || url.host() != m_base.host() ||
        url.port() != m_base.port() || !url.userInfo().isEmpty()) {
        callback({0, {}, "Fleet returned a file URL outside its configured origin"});
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + m_token);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(60000);
    auto *reply = method == "GET" ? m_network.get(request) : m_network.post(request, body);
    connect(reply, &QNetworkReply::finished, this, [reply, callback] {
        FleetResponse response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) response.error = reply->errorString();
        callback(response);
        reply->deleteLater();
    });
}
