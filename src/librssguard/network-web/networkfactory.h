// For license of this file, see <project-root-folder>/LICENSE.md.

#ifndef NETWORKFACTORY_H
#define NETWORKFACTORY_H

#include "network-web/httpresponse.h"

#include <QCoreApplication>
#include <QHttpPart>
#include <QNetworkCookie>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPair>
#include <QVariant>

struct NetworkResult {
  QNetworkReply::NetworkError m_networkError;
  QString m_contentType;
  QList<QNetworkCookie> m_cookies;

  explicit NetworkResult();
  explicit NetworkResult(QNetworkReply::NetworkError err, const QString& ct, const QList<QNetworkCookie>& cook);
};

class Downloader;

class NetworkFactory {
  Q_DECLARE_TR_FUNCTIONS(NetworkFactory)

  private:
    explicit NetworkFactory() = default;

  public:
    static QStringList extractFeedLinksFromHtmlPage(const QUrl& url, const QString& html);
    static QPair<QByteArray, QByteArray> generateBasicAuthHeader(const QString& username, const QString& password);

    // Returns human readable text for given network error.
    static QString networkErrorText(QNetworkReply::NetworkError error_code);
    static QString sanitizeUrl(const QString& url);

    // Performs SYNCHRONOUS download if favicon for the site,
    // given URL belongs to.
    static QNetworkReply::NetworkError downloadIcon(const QList<QPair<QString, bool>>& urls,
                                                    int timeout,
                                                    QIcon& output,
                                                    const QList<QPair<QByteArray, QByteArray>>& additional_headers,
                                                    const QNetworkProxy& custom_proxy = QNetworkProxy::ProxyType::DefaultProxy);
    static NetworkResult performNetworkOperation(const QString& url, int timeout,
                                                 const QByteArray& input_data,
                                                 QByteArray& output,
                                                 QNetworkAccessManager::Operation operation,
                                                 const QList<QPair<QByteArray, QByteArray>>& additional_headers = QList<QPair<QByteArray, QByteArray>>(),
                                                 bool protected_contents = false,
                                                 const QString& username = QString(),
                                                 const QString& password = QString(),
                                                 const QNetworkProxy& custom_proxy = QNetworkProxy::ProxyType::DefaultProxy);
    static NetworkResult performNetworkOperation(const QString& url, int timeout,
                                                 QHttpMultiPart* input_data,
                                                 QList<HttpResponse>& output,
                                                 QNetworkAccessManager::Operation operation,
                                                 const QList<QPair<QByteArray, QByteArray>>& additional_headers = QList<QPair<QByteArray, QByteArray>>(),
                                                 bool protected_contents = false,
                                                 const QString& username = QString(),
                                                 const QString& password = QString(),
                                                 const QNetworkProxy& custom_proxy = QNetworkProxy::ProxyType::DefaultProxy);
};

#endif // NETWORKFACTORY_H
