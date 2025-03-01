﻿// For license of this file, see <project-root-folder>/LICENSE.md.

#include "network-web/adblock/adblockmanager.h"

#include "exceptions/applicationexception.h"
#include "exceptions/networkexception.h"
#include "miscellaneous/application.h"
#include "miscellaneous/settings.h"
#include "network-web/adblock/adblockdialog.h"
#include "network-web/adblock/adblockicon.h"
#include "network-web/adblock/adblockrequestinfo.h"
#include "network-web/adblock/adblockurlinterceptor.h"
#include "network-web/networkfactory.h"
#include "network-web/networkurlinterceptor.h"
#include "network-web/webfactory.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QWebEngineProfile>

AdBlockManager::AdBlockManager(QObject* parent)
  : QObject(parent), m_loaded(false), m_enabled(false), m_interceptor(new AdBlockUrlInterceptor(this)),
  m_serverProcess(nullptr), m_cacheBlocks({}) {
  m_adblockIcon = new AdBlockIcon(this);
  m_adblockIcon->setObjectName(QSL("m_adblockIconAction"));
  m_unifiedFiltersFile = qApp->userDataFolder() + QDir::separator() + QSL("adblock-unified-filters.txt");
}

AdBlockManager::~AdBlockManager() {
  killServer();
}

BlockingResult AdBlockManager::block(const AdblockRequestInfo& request) {
  if (!isEnabled()) {
    return { false };
  }

  const QString url_string = request.requestUrl().toEncoded().toLower();
  const QString firstparty_url_string = request.firstPartyUrl().toEncoded().toLower();
  const QString url_scheme = request.requestUrl().scheme().toLower();
  const QPair<QString, QString> url_pair = { firstparty_url_string, url_string };
  const QString url_type = request.resourceType();

  if (!canRunOnScheme(url_scheme)) {
    return { false };
  }
  else {
    if (m_cacheBlocks.contains(url_pair)) {
      qDebugNN << LOGSEC_ADBLOCK
               << "Found blocking data in cache, URL:"
               << QUOTE_W_SPACE_DOT(url_pair);

      return m_cacheBlocks.value(url_pair);
    }

    if (m_serverProcess != nullptr && m_serverProcess->state() == QProcess::ProcessState::Running) {
      try {
        auto result = askServerIfBlocked(firstparty_url_string, url_string, url_type);

        m_cacheBlocks.insert(url_pair, result);

        qDebugNN << LOGSEC_ADBLOCK
                 << "Inserted blocking data to cache for:"
                 << QUOTE_W_SPACE_DOT(url_pair);

        return result;
      }
      catch (const ApplicationException& ex) {
        qCriticalNN << LOGSEC_ADBLOCK
                    << "HTTP error when calling server for blocking rules:"
                    << QUOTE_W_SPACE_DOT(ex.message());
        return { false };
      }
    }
    else {
      return { false };
    }
  }
}

void AdBlockManager::setEnabled(bool enabled) {
  if (enabled == m_enabled) {
    return;
  }

  if (!m_loaded) {
    qApp->web()->urlIinterceptor()->installUrlInterceptor(m_interceptor);
    m_loaded = true;
  }

  m_enabled = enabled;
  emit enabledChanged(m_enabled);

  if (m_enabled) {
    try {
      updateUnifiedFiltersFileAndStartServer();
    }
    catch (const ApplicationException& ex) {
      qCriticalNN << LOGSEC_ADBLOCK
                  << "Failed to write unified filters to file or re-start server, error:"
                  << QUOTE_W_SPACE_DOT(ex.message());

      m_enabled = false;
      emit enabledChanged(m_enabled);
    }
  }
  else {
    killServer();
  }
}

bool AdBlockManager::isEnabled() const {
  return m_enabled;
}

bool AdBlockManager::canRunOnScheme(const QString& scheme) const {
  return !(scheme == QSL("file") || scheme == QSL("qrc") || scheme == QSL("data") || scheme == QSL("abp"));
}

QString AdBlockManager::elementHidingRulesForDomain(const QUrl& url) const {
  if (m_serverProcess != nullptr &&  m_serverProcess->state() == QProcess::ProcessState::Running) {
    try {
      auto result = askServerForCosmeticRules(url.toString());

      return result;
    }
    catch (const ApplicationException& ex) {
      qCriticalNN << LOGSEC_ADBLOCK
                  << "HTTP error when calling server for cosmetic rules:"
                  << QUOTE_W_SPACE_DOT(ex.message());
      return {};
    }
  }
  else {
    return {};
  }
}

QStringList AdBlockManager::filterLists() const {
  return qApp->settings()->value(GROUP(AdBlock), SETTING(AdBlock::FilterLists)).toStringList();
}

void AdBlockManager::setFilterLists(const QStringList& filter_lists) {
  qApp->settings()->setValue(GROUP(AdBlock), AdBlock::FilterLists, filter_lists);
}

QStringList AdBlockManager::customFilters() const {
  return qApp->settings()->value(GROUP(AdBlock), SETTING(AdBlock::CustomFilters)).toStringList();
}

void AdBlockManager::setCustomFilters(const QStringList& custom_filters) {
  qApp->settings()->setValue(GROUP(AdBlock), AdBlock::CustomFilters, custom_filters);
}

QString AdBlockManager::generateJsForElementHiding(const QString& css) {
  QString source = QL1S("(function() {"
                        "var head = document.getElementsByTagName('head')[0];"
                        "if (!head) return;"
                        "var css = document.createElement('style');"
                        "css.setAttribute('type', 'text/css');"
                        "css.appendChild(document.createTextNode('%1'));"
                        "head.appendChild(css);"
                        "})()");
  QString style = css;

  style.replace(QL1S("'"), QL1S("\\'"));
  style.replace(QL1S("\n"), QL1S("\\n"));

  return source.arg(style);
}

void AdBlockManager::showDialog() {
  AdBlockDialog(qApp->mainFormWidget()).exec();
}

void AdBlockManager::onServerProcessFinished(int exit_code, QProcess::ExitStatus exit_status) {
  Q_UNUSED(exit_status)
  killServer();

  qCriticalNN << LOGSEC_ADBLOCK
              << "Process exited with exit code"
              << QUOTE_W_SPACE(exit_code)
              << "so check application log for more details.";

  m_enabled = false;
  emit processTerminated();
}

BlockingResult AdBlockManager::askServerIfBlocked(const QString& fp_url, const QString& url, const QString& url_type) const {
  QJsonObject req_obj;
  QByteArray out;
  QElapsedTimer tmr;

  req_obj["fp_url"] = fp_url;
  req_obj["url"] = url;
  req_obj["url_type"] = url_type,
  req_obj["filter"] = true;

  tmr.start();

  auto network_res = NetworkFactory::performNetworkOperation(QSL("http://%1:%2").arg(QHostAddress(QHostAddress::SpecialAddress::LocalHost).toString(),
                                                                                     QString::number(ADBLOCK_SERVER_PORT)),
                                                             500,
                                                             QJsonDocument(req_obj).toJson(),
                                                             out,
                                                             QNetworkAccessManager::Operation::PostOperation,
                                                             { {
                                                               QSL(HTTP_HEADERS_CONTENT_TYPE).toLocal8Bit(),
                                                               QSL("application/json").toLocal8Bit() } });

  if (network_res.first == QNetworkReply::NetworkError::NoError) {
    qDebugNN << LOGSEC_ADBLOCK
             << "Query for blocking info to server took "
             << tmr.elapsed()
             << " ms.";

    QJsonObject out_obj = QJsonDocument::fromJson(out).object();
    bool blocking = out_obj["filter"].toObject()["match"].toBool();

    return {
      blocking,
      blocking
          ? out_obj["filter"].toObject()["filter"].toObject()["filter"].toString()
          : QString()
    };
  }
  else {
    throw NetworkException(network_res.first);
  }
}

QString AdBlockManager::askServerForCosmeticRules(const QString& url) const {
  QJsonObject req_obj;
  QByteArray out;
  QElapsedTimer tmr;

  req_obj["url"] = url;
  req_obj["cosmetic"] = true;

  tmr.start();

  auto network_res = NetworkFactory::performNetworkOperation(QSL("http://%1:%2").arg(QHostAddress(QHostAddress::SpecialAddress::LocalHost).toString(),
                                                                                     QString::number(ADBLOCK_SERVER_PORT)),
                                                             500,
                                                             QJsonDocument(req_obj).toJson(),
                                                             out,
                                                             QNetworkAccessManager::Operation::PostOperation,
                                                             { {
                                                               QSL(HTTP_HEADERS_CONTENT_TYPE).toLocal8Bit(),
                                                               QSL("application/json").toLocal8Bit() } });

  if (network_res.first == QNetworkReply::NetworkError::NoError) {
    qDebugNN << LOGSEC_ADBLOCK
             << "Query for cosmetic rules to server took "
             << tmr.elapsed()
             << " ms.";

    QJsonObject out_obj = QJsonDocument::fromJson(out).object();

    return out_obj["cosmetic"].toObject()["styles"].toString();
  }
  else {
    throw NetworkException(network_res.first);
  }
}

QProcess* AdBlockManager::startServer(int port) {
  QString temp_server = QDir::toNativeSeparators(IOFactory::getSystemFolder(QStandardPaths::StandardLocation::TempLocation)) +
                        QDir::separator() +
                        QSL("adblock-server.js");

  if (!IOFactory::copyFile(QSL(":/scripts/adblock/adblock-server.js"), temp_server)) {
    qWarningNN << LOGSEC_ADBLOCK << "Failed to copy server file to TEMP.";
  }

  QProcess* proc = new QProcess(this);

#if defined(Q_OS_WIN)
  proc->setProgram(QSL("node.exe"));
#else
  proc->setProgram(QSL("node"));
#endif

  proc->setArguments({
    QDir::toNativeSeparators(temp_server),
    QString::number(port),
    QDir::toNativeSeparators(m_unifiedFiltersFile)
  });

  proc->setProcessEnvironment(QProcessEnvironment::systemEnvironment());

  auto pe = proc->processEnvironment();
  QString default_node_path =
#if defined(Q_OS_WIN)
    pe.value(QSL("APPDATA")) + QDir::separator() + QSL("npm") + QDir::separator() + QSL("node_modules");
#elif defined(Q_OS_LINUX)
    QSL("/usr/lib/node_modules");
#elif defined(Q_OS_MACOS)
    QSL("/usr/local/lib/node_modules");
#else
    QSL("");
#endif

  if (!pe.contains(QSL("NODE_PATH")) && !default_node_path.isEmpty()) {
    pe.insert(QSL("NODE_PATH"), default_node_path);
  }

  proc->setProcessEnvironment(pe);
  proc->setProcessChannelMode(QProcess::ProcessChannelMode::ForwardedErrorChannel);

  connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &AdBlockManager::onServerProcessFinished);

  proc->open();

  qDebugNN << LOGSEC_ADBLOCK << "Attempting to start AdBlock server.";
  return proc;
}

void AdBlockManager::killServer() {
  if (m_serverProcess != nullptr) {
    disconnect(m_serverProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
               this, &AdBlockManager::onServerProcessFinished);

    if (m_serverProcess->state() == QProcess::ProcessState::Running) {
      m_serverProcess->kill();
    }

    m_serverProcess->deleteLater();
    m_serverProcess = nullptr;
  }
}

void AdBlockManager::updateUnifiedFiltersFileAndStartServer() {
  m_cacheBlocks.clear();
  killServer();

  if (QFile::exists(m_unifiedFiltersFile)) {
    QFile::remove(m_unifiedFiltersFile);
  }

  QString unified_contents;
  auto filter_lists = filterLists();

  // Download filters one by one and append.
  for (const QString& filter_list_url : qAsConst(filter_lists)) {
    if (filter_list_url.simplified().isEmpty()) {
      continue;
    }

    QByteArray out;
    auto res = NetworkFactory::performNetworkOperation(filter_list_url,
                                                       2000,
                                                       {},
                                                       out,
                                                       QNetworkAccessManager::Operation::GetOperation);

    if (res.first == QNetworkReply::NetworkError::NoError) {
      unified_contents = unified_contents.append(QString::fromUtf8(out));
      unified_contents = unified_contents.append('\n');

      qDebugNN << LOGSEC_ADBLOCK
               << "Downloaded filter list from"
               << QUOTE_W_SPACE_DOT(filter_list_url);
    }
    else {
      throw NetworkException(res.first, tr("failed to download filter list '%1'").arg(filter_list_url));
    }
  }

  unified_contents = unified_contents.append(customFilters().join(QSL("\n")));

  // Save.
  m_unifiedFiltersFile = IOFactory::getSystemFolder(QStandardPaths::StandardLocation::TempLocation) +
                         QDir::separator() +
                         QSL("adblock.filters");

  IOFactory::writeFile(m_unifiedFiltersFile, unified_contents.toUtf8());

  if (m_enabled) {
    m_serverProcess = startServer(ADBLOCK_SERVER_PORT);
  }
}
