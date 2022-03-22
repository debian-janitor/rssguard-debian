// For license of this file, see <project-root-folder>/LICENSE.md.

#include "network-web/basenetworkaccessmanager.h"

#include "miscellaneous/application.h"
#include "miscellaneous/textfactory.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>

#if defined(USE_WEBENGINE)
#include <QWebEngineProfile>
#endif

BaseNetworkAccessManager::BaseNetworkAccessManager(QObject* parent)
  : QNetworkAccessManager(parent) {
  connect(this, &BaseNetworkAccessManager::sslErrors, this, &BaseNetworkAccessManager::onSslErrors);
  loadSettings();
}

void BaseNetworkAccessManager::loadSettings() {
  QNetworkProxy new_proxy;
  const QNetworkProxy::ProxyType selected_proxy_type = static_cast<QNetworkProxy::ProxyType>(qApp->settings()->value(GROUP(Proxy),
                                                                                                                     SETTING(Proxy::Type)).
                                                                                             toInt());

  if (selected_proxy_type == QNetworkProxy::ProxyType::NoProxy) {
    // No extra setting is needed, set new proxy and exit this method.
    setProxy(QNetworkProxy::ProxyType::NoProxy);
  }
  else {
    qWarningNN << LOGSEC_NETWORK << "Using application-wide proxy.";

    if (QNetworkProxy::applicationProxy().type() != QNetworkProxy::ProxyType::DefaultProxy &&
        QNetworkProxy::applicationProxy().type() != QNetworkProxy::ProxyType::NoProxy) {
      qWarningNN << LOGSEC_NETWORK
                 << "Used proxy address:"
                 << QUOTE_W_SPACE_COMMA(QNetworkProxy::applicationProxy().hostName())
                 << " type:"
                 << QUOTE_W_SPACE_DOT(QNetworkProxy::applicationProxy().type());
    }

    setProxy(QNetworkProxy::applicationProxy());
  }

  qDebugNN << LOGSEC_NETWORK << "Settings of BaseNetworkAccessManager loaded.";
}

void BaseNetworkAccessManager::onSslErrors(QNetworkReply* reply, const QList<QSslError>& error) {
  qWarningNN << LOGSEC_NETWORK
             << "Ignoring SSL errors for"
             << QUOTE_W_SPACE_DOT(reply->url().toString());
  reply->ignoreSslErrors(error);
}

QNetworkReply* BaseNetworkAccessManager::createRequest(QNetworkAccessManager::Operation op,
                                                       const QNetworkRequest& request,
                                                       QIODevice* outgoingData) {
  QNetworkRequest new_request = request;

#if defined(Q_OS_WIN)
  new_request.setAttribute(QNetworkRequest::Attribute::HttpPipeliningAllowedAttribute, true);

#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
  new_request.setAttribute(QNetworkRequest::Attribute::Http2AllowedAttribute, true);
#endif
#endif

  new_request.setRawHeader(HTTP_HEADERS_COOKIE, QSL("JSESSIONID= ").toLocal8Bit());
  new_request.setRawHeader(HTTP_HEADERS_USER_AGENT, HTTP_COMPLETE_USERAGENT);

  auto reply = QNetworkAccessManager::createRequest(op, new_request, outgoingData);
  return reply;
}
