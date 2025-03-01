// For license of this file, see <project-root-folder>/LICENSE.md.

#include "network-web/downloader.h"

#include "miscellaneous/application.h"
#include "miscellaneous/iofactory.h"
#include "network-web/cookiejar.h"
#include "network-web/networkfactory.h"
#include "network-web/silentnetworkaccessmanager.h"
#include "network-web/webfactory.h"

#include <QHttpMultiPart>
#include <QNetworkCookie>
#include <QRegularExpression>
#include <QTimer>

Downloader::Downloader(QObject* parent)
  : QObject(parent), m_activeReply(nullptr), m_downloadManager(new SilentNetworkAccessManager(this)),
  m_timer(new QTimer(this)), m_inputData(QByteArray()),
  m_inputMultipartData(nullptr), m_targetProtected(false), m_targetUsername(QString()), m_targetPassword(QString()),
  m_lastOutputData(QByteArray()), m_lastOutputError(QNetworkReply::NoError) {
  m_timer->setInterval(DOWNLOAD_TIMEOUT);
  m_timer->setSingleShot(true);
  connect(m_timer, &QTimer::timeout, this, &Downloader::cancel);

  m_downloadManager->setCookieJar(qApp->web()->cookieJar());
  qApp->web()->cookieJar()->setParent(nullptr);
}

Downloader::~Downloader() {
  qDebugNN << LOGSEC_NETWORK << "Destroying Downloader instance.";
}

void Downloader::downloadFile(const QString& url, int timeout, bool protected_contents, const QString& username,
                              const QString& password) {
  manipulateData(url, QNetworkAccessManager::GetOperation, QByteArray(), timeout,
                 protected_contents, username, password);
}

void Downloader::uploadFile(const QString& url, const QByteArray& data, int timeout,
                            bool protected_contents, const QString& username, const QString& password) {
  manipulateData(url, QNetworkAccessManager::Operation::PostOperation, data, timeout, protected_contents, username, password);
}

void Downloader::manipulateData(const QString& url, QNetworkAccessManager::Operation operation,
                                QHttpMultiPart* multipart_data, int timeout,
                                bool protected_contents, const QString& username, const QString& password) {
  manipulateData(url, operation, QByteArray(), multipart_data, timeout, protected_contents, username, password);
}

void Downloader::manipulateData(const QString& url, QNetworkAccessManager::Operation operation, const QByteArray& data,
                                int timeout, bool protected_contents, const QString& username, const QString& password) {
  manipulateData(url, operation, data, nullptr, timeout, protected_contents, username, password);
}

void Downloader::manipulateData(const QString& url,
                                QNetworkAccessManager::Operation operation,
                                const QByteArray& data,
                                QHttpMultiPart* multipart_data,
                                int timeout,
                                bool protected_contents,
                                const QString& username,
                                const QString& password) {
  QString sanitized_url = NetworkFactory::sanitizeUrl(url);
  auto cookies = CookieJar::extractCookiesFromUrl(sanitized_url);

  if (!cookies.isEmpty()) {
    qApp->web()->cookieJar()->setCookiesFromUrl(cookies, sanitized_url);
  }

  QNetworkRequest request;
  QHashIterator<QByteArray, QByteArray> i(m_customHeaders);

  while (i.hasNext()) {
    i.next();
    request.setRawHeader(i.key(), i.value());
  }

  m_inputData = data;
  m_inputMultipartData = multipart_data;

  // Set url for this request and fire it up.
  m_timer->setInterval(timeout);

  request.setUrl(qApp->web()->processFeedUriScheme(sanitized_url));

  m_targetProtected = protected_contents;
  m_targetUsername = username;
  m_targetPassword = password;

  if (operation == QNetworkAccessManager::Operation::PostOperation) {
    if (m_inputMultipartData == nullptr) {
      runPostRequest(request, m_inputData);
    }
    else {
      runPostRequest(request, m_inputMultipartData);
    }
  }
  else if (operation == QNetworkAccessManager::GetOperation) {
    runGetRequest(request);
  }
  else if (operation == QNetworkAccessManager::PutOperation) {
    runPutRequest(request, m_inputData);
  }
  else if (operation == QNetworkAccessManager::DeleteOperation) {
    runDeleteRequest(request);
  }
}

void Downloader::finished() {
  auto* reply = qobject_cast<QNetworkReply*>(sender());

  QNetworkAccessManager::Operation reply_operation = reply->operation();

  m_timer->stop();

  // In this phase, some part of downloading process is completed.
  QUrl redirection_url = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();

  if (redirection_url.isValid()) {
    // Communication indicates that HTTP redirection is needed.
    // Setup redirection URL and download again.
    QNetworkRequest request = reply->request();

    qWarningNN << LOGSEC_NETWORK
               << "Network layer indicates HTTP redirection is needed.";
    qWarningNN << LOGSEC_NETWORK
               << "Origin URL:"
               << QUOTE_W_SPACE_DOT(request.url().toString());
    qWarningNN << LOGSEC_NETWORK
               << "Proposed redirection URL:"
               << QUOTE_W_SPACE_DOT(redirection_url.toString());

    redirection_url = request.url().resolved(redirection_url);

    qWarningNN << LOGSEC_NETWORK
               << "Resolved redirection URL:"
               << QUOTE_W_SPACE_DOT(redirection_url.toString());

    request.setUrl(redirection_url);

    m_activeReply->deleteLater();
    m_activeReply = nullptr;

    if (reply_operation == QNetworkAccessManager::GetOperation) {
      runGetRequest(request);
    }
    else if (reply_operation == QNetworkAccessManager::PostOperation) {
      if (m_inputMultipartData == nullptr) {
        runPostRequest(request, m_inputData);
      }
      else {
        runPostRequest(request, m_inputMultipartData);
      }
    }
    else if (reply_operation == QNetworkAccessManager::PutOperation) {
      runPutRequest(request, m_inputData);
    }
    else if (reply_operation == QNetworkAccessManager::DeleteOperation) {
      runDeleteRequest(request);
    }
  }
  else {
    // No redirection is indicated. Final file is obtained in our "reply" object.
    // Read the data into output buffer.
    if (m_inputMultipartData == nullptr) {
      m_lastOutputData = reply->readAll();
    }
    else {
      m_lastOutputMultipartData = decodeMultipartAnswer(reply);
    }

    m_lastContentType = reply->header(QNetworkRequest::ContentTypeHeader);
    m_lastOutputError = reply->error();
    m_activeReply->deleteLater();
    m_activeReply = nullptr;

    if (m_inputMultipartData != nullptr) {
      m_inputMultipartData->deleteLater();
    }

    emit completed(m_lastOutputError, m_lastOutputData);
  }
}

void Downloader::progressInternal(qint64 bytes_received, qint64 bytes_total) {
  if (m_timer->interval() > 0) {
    m_timer->start();
  }

  emit progress(bytes_received, bytes_total);
}

void Downloader::setCustomPropsToReply(QNetworkReply* reply) {
  reply->setProperty("protected", m_targetProtected);
  reply->setProperty("username", m_targetUsername);
  reply->setProperty("password", m_targetPassword);
}

QList<HttpResponse> Downloader::decodeMultipartAnswer(QNetworkReply* reply) {
  QByteArray data = reply->readAll();

  if (data.isEmpty()) {
    return QList<HttpResponse>();
  }

  QString content_type = reply->header(QNetworkRequest::KnownHeaders::ContentTypeHeader).toString();
  QString boundary = content_type.mid(content_type.indexOf(QL1S("boundary=")) + 9);
  QRegularExpression regex(QL1S("--") + boundary + QL1S("(--)?(\\r\\n)?"));
  QStringList list = QString::fromUtf8(data).split(regex,
#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
                                                   Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
                                                   QString::SplitBehavior::SkipEmptyParts);
#endif

  QList<HttpResponse> parts;

  parts.reserve(list.size());

  for (const QString& http_response_str : list) {
    // We separate headers and body.
    HttpResponse new_part;
    int start_of_http = http_response_str.indexOf(QL1S("HTTP/1.1"));
    int start_of_headers = http_response_str.indexOf(QRegularExpression(QSL("\\r\\r?\\n")), start_of_http);
    int start_of_body = http_response_str.indexOf(QRegularExpression(QSL("(\\r\\r?\\n){2,}")), start_of_headers + 2);
    QString body = http_response_str.mid(start_of_body);
    QString headers = http_response_str.mid(start_of_headers,
                                            start_of_body - start_of_headers).replace(QRegularExpression(QSL("[\\n\\r]+")),
                                                                                      QSL("\n"));

    auto header_lines = headers.split(QL1C('\n'),
#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
                                      Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
                                      QString::SplitBehavior::SkipEmptyParts);
#endif

    for (const QString& header_line : qAsConst(header_lines)) {
      int index_colon = header_line.indexOf(QL1C(':'));

      if (index_colon > 0) {
        new_part.appendHeader(header_line.mid(0, index_colon), header_line.mid(index_colon + 2));
      }
    }

    new_part.setBody(body);
    parts.append(new_part);
  }

  return parts;
}

void Downloader::runDeleteRequest(const QNetworkRequest& request) {
  m_timer->start();
  m_activeReply = m_downloadManager->deleteResource(request);
  setCustomPropsToReply(m_activeReply);
  connect(m_activeReply, &QNetworkReply::downloadProgress, this, &Downloader::progressInternal);
  connect(m_activeReply, &QNetworkReply::finished, this, &Downloader::finished);
}

void Downloader::runPutRequest(const QNetworkRequest& request, const QByteArray& data) {
  m_timer->start();
  m_activeReply = m_downloadManager->put(request, data);
  setCustomPropsToReply(m_activeReply);
  connect(m_activeReply, &QNetworkReply::downloadProgress, this, &Downloader::progressInternal);
  connect(m_activeReply, &QNetworkReply::finished, this, &Downloader::finished);
}

void Downloader::runPostRequest(const QNetworkRequest& request, QHttpMultiPart* multipart_data) {
  m_timer->start();
  m_activeReply = m_downloadManager->post(request, multipart_data);
  setCustomPropsToReply(m_activeReply);
  connect(m_activeReply, &QNetworkReply::downloadProgress, this, &Downloader::progressInternal);
  connect(m_activeReply, &QNetworkReply::finished, this, &Downloader::finished);
}

void Downloader::runPostRequest(const QNetworkRequest& request, const QByteArray& data) {
  m_timer->start();
  m_activeReply = m_downloadManager->post(request, data);
  setCustomPropsToReply(m_activeReply);
  connect(m_activeReply, &QNetworkReply::downloadProgress, this, &Downloader::progressInternal);
  connect(m_activeReply, &QNetworkReply::finished, this, &Downloader::finished);
}

void Downloader::runGetRequest(const QNetworkRequest& request) {
  m_timer->start();
  m_activeReply = m_downloadManager->get(request);
  setCustomPropsToReply(m_activeReply);
  connect(m_activeReply, &QNetworkReply::downloadProgress, this, &Downloader::progressInternal);
  connect(m_activeReply, &QNetworkReply::finished, this, &Downloader::finished);
}

QVariant Downloader::lastContentType() const {
  return m_lastContentType;
}

void Downloader::setProxy(const QNetworkProxy& proxy) {
  qWarningNN << LOGSEC_NETWORK
             << "Setting specific downloader proxy, address:"
             << QUOTE_W_SPACE_COMMA(proxy.hostName())
             << " type:"
             << QUOTE_W_SPACE_DOT(proxy.type());

  m_downloadManager->setProxy(proxy);
}

void Downloader::cancel() {
  if (m_activeReply != nullptr) {
    // Download action timed-out, too slow connection or target is not reachable.
    m_activeReply->abort();
  }
}

void Downloader::appendRawHeader(const QByteArray& name, const QByteArray& value) {
  if (!value.isEmpty()) {
    m_customHeaders.insert(name, value);
  }
}

QNetworkReply::NetworkError Downloader::lastOutputError() const {
  return m_lastOutputError;
}

QList<HttpResponse> Downloader::lastOutputMultipartData() const {
  return m_lastOutputMultipartData;
}

QByteArray Downloader::lastOutputData() const {
  return m_lastOutputData;
}
