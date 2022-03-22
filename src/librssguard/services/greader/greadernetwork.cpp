// For license of this file, see <project-root-folder>/LICENSE.md.

#include "services/greader/greadernetwork.h"

#include "3rd-party/boolinq/boolinq.h"
#include "database/databasequeries.h"
#include "exceptions/applicationexception.h"
#include "exceptions/feedfetchexception.h"
#include "exceptions/networkexception.h"
#include "miscellaneous/application.h"
#include "network-web/networkfactory.h"
#include "network-web/oauth2service.h"
#include "network-web/webfactory.h"
#include "services/abstract/category.h"
#include "services/abstract/label.h"
#include "services/abstract/labelsnode.h"
#include "services/greader/definitions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

GreaderNetwork::GreaderNetwork(QObject* parent)
  : QObject(parent), m_root(nullptr), m_service(GreaderServiceRoot::Service::FreshRss), m_username(QString()),
  m_password(QString()), m_baseUrl(QString()), m_batchSize(GREADER_DEFAULT_BATCH_SIZE), m_downloadOnlyUnreadMessages(false),
  m_prefetchedMessages({}), m_prefetchedStatus(Feed::Status::Normal), m_performGlobalFetching(false),
  m_intelligentSynchronization(true), m_newerThanFilter(QDate::currentDate().addYears(-1)),
  m_oauth(new OAuth2Service(QSL(INO_OAUTH_AUTH_URL), QSL(INO_OAUTH_TOKEN_URL),
                            {}, {}, QSL(INO_OAUTH_SCOPE), this)) {
  initializeOauth();
  clearCredentials();
}

QNetworkReply::NetworkError GreaderNetwork::editLabels(const QString& state,
                                                       bool assign,
                                                       const QStringList& msg_custom_ids,
                                                       const QNetworkProxy& proxy) {
  QString full_url = generateFullUrl(Operations::EditTag);
  int timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();

  QNetworkReply::NetworkError network_err = QNetworkReply::NetworkError::UnknownNetworkError;

  if (!ensureLogin(proxy, &network_err)) {
    return network_err;
  }

  QStringList trimmed_ids; trimmed_ids.reserve(msg_custom_ids.size());

  for (const QString& id : msg_custom_ids) {
    trimmed_ids.append(QSL("i=") + id);
  }

  QStringList working_subset; working_subset.reserve(std::min(GREADER_API_EDIT_TAG_BATCH, int(trimmed_ids.size())));

  // Now, we perform messages update in batches (max X messages per batch).
  while (!trimmed_ids.isEmpty()) {
    // We take X IDs.
    for (int i = 0; i < GREADER_API_EDIT_TAG_BATCH && !trimmed_ids.isEmpty(); i++) {
      working_subset.append(trimmed_ids.takeFirst());
    }

    QString args;

    if (assign) {
      args = QSL("a=") + state + "&";
    }
    else {
      args = QSL("r=") + state + "&";
    }

    args += working_subset.join(QL1C('&'));

    if (m_service == GreaderServiceRoot::Service::Reedah) {
      args += QSL("&T=%1").arg(m_authToken);
    }

    // We send this batch.
    QByteArray output;
    auto result_edit = NetworkFactory::performNetworkOperation(full_url,
                                                               timeout,
                                                               args.toUtf8(),
                                                               output,
                                                               QNetworkAccessManager::Operation::PostOperation,
                                                               { authHeader(),
                                                                 { QSL(HTTP_HEADERS_CONTENT_TYPE).toLocal8Bit(),
                                                                   QSL("application/x-www-form-urlencoded").toLocal8Bit() } },
                                                               false,
                                                               {},
                                                               {},
                                                               proxy);

    if (result_edit.m_networkError != QNetworkReply::NetworkError::NoError) {
      return result_edit.m_networkError;
    }

    // Cleanup for next batch.
    working_subset.clear();
  }

  return QNetworkReply::NetworkError::NoError;
}

QVariantHash GreaderNetwork::userInfo(const QNetworkProxy& proxy) {
  QString full_url = generateFullUrl(Operations::UserInfo);
  int timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();
  QNetworkReply::NetworkError network_err = QNetworkReply::NetworkError::UnknownNetworkError;

  if (!ensureLogin(proxy, &network_err)) {
    throw NetworkException(network_err);
  }

  QByteArray output;
  auto res = NetworkFactory::performNetworkOperation(full_url,
                                                     timeout,
                                                     {},
                                                     output,
                                                     QNetworkAccessManager::Operation::GetOperation,
                                                     { authHeader() },
                                                     false,
                                                     {},
                                                     {},
                                                     proxy);

  if (res.m_networkError != QNetworkReply::NetworkError::NoError) {
    throw NetworkException(res.m_networkError);
  }

  return QJsonDocument::fromJson(output).object().toVariantHash();
}

void GreaderNetwork::clearPrefetchedMessages() {
  m_prefetchedMessages.clear();
  m_prefetchedStatus = Feed::Status::Normal;
}

void GreaderNetwork::prepareFeedFetching(GreaderServiceRoot* root,
                                         const QList<Feed*>& feeds,
                                         const QHash<QString, QHash<ServiceRoot::BagOfMessages, QStringList>>& stated_messages,
                                         const QHash<QString, QStringList>& tagged_messages,
                                         const QNetworkProxy& proxy) {
  Q_UNUSED(tagged_messages)

  m_prefetchedMessages.clear();
  m_prefetchedStatus = Feed::Status::Normal;

  try {
    double perc_of_fetching = (feeds.size() * 1.0) / root->getSubTreeFeeds().size();

    m_performGlobalFetching = perc_of_fetching > GREADER_GLOBAL_UPDATE_THRES;

    qDebugNN << LOGSEC_GREADER
             << "Percentage of feeds for fetching:"
             << QUOTE_W_SPACE_DOT(perc_of_fetching * 100.0);

    auto remote_starred_ids_list = itemIds(QSL(GREADER_API_FULL_STATE_IMPORTANT), false, proxy, -1, m_newerThanFilter);

    for (int i = 0; i < remote_starred_ids_list.size(); i++) {
      remote_starred_ids_list.replace(i, convertShortStreamIdToLongStreamId(remote_starred_ids_list.at(i)));
    }

    QSet<QString> remote_starred_ids = FROM_LIST_TO_SET(QSet<QString>, remote_starred_ids_list);
    QSet<QString> local_starred_ids;
    QList<QHash<ServiceRoot::BagOfMessages, QStringList>> all_states = stated_messages.values();

    for (auto& lst : all_states) {
      auto s = lst.value(ServiceRoot::BagOfMessages::Starred);

      local_starred_ids.unite(FROM_LIST_TO_SET(QSet<QString>, s));
    }

    auto starred_to_download((remote_starred_ids - local_starred_ids).unite(local_starred_ids - remote_starred_ids));
    auto to_download = starred_to_download;

    if (m_performGlobalFetching) {
      qWarningNN << LOGSEC_GREADER << "Performing global contents fetching.";

      QStringList remote_all_ids_list = m_downloadOnlyUnreadMessages
                                      ? QStringList()
                                      : itemIds(QSL(GREADER_API_FULL_STATE_READING_LIST), false, proxy, -1, m_newerThanFilter);
      QStringList remote_unread_ids_list = itemIds(QSL(GREADER_API_FULL_STATE_READING_LIST), true, proxy, -1, m_newerThanFilter);

      for (int i = 0; i < remote_all_ids_list.size(); i++) {
        remote_all_ids_list.replace(i, convertShortStreamIdToLongStreamId(remote_all_ids_list.at(i)));
      }

      for (int i = 0; i < remote_unread_ids_list.size(); i++) {
        remote_unread_ids_list.replace(i, convertShortStreamIdToLongStreamId(remote_unread_ids_list.at(i)));
      }

      QSet<QString> remote_all_ids = FROM_LIST_TO_SET(QSet<QString>, remote_all_ids_list);
      QSet<QString> remote_unread_ids = FROM_LIST_TO_SET(QSet<QString>, remote_unread_ids_list);
      QSet<QString> remote_read_ids = remote_all_ids - remote_unread_ids;
      QSet<QString> local_unread_ids;
      QSet<QString> local_read_ids;

      for (auto& lst : all_states) {
        auto u = lst.value(ServiceRoot::BagOfMessages::Unread);
        auto r = lst.value(ServiceRoot::BagOfMessages::Read);

        local_unread_ids.unite(FROM_LIST_TO_SET(QSet<QString>, u));
        local_read_ids.unite(FROM_LIST_TO_SET(QSet<QString>, r));
      }

      if (!m_downloadOnlyUnreadMessages) {
        to_download += remote_all_ids - local_read_ids - local_unread_ids;
      }
      else {
        to_download += remote_unread_ids - local_read_ids - local_unread_ids;
      }

      auto moved_read = local_read_ids.intersect(remote_unread_ids);

      to_download += moved_read;

      if (!m_downloadOnlyUnreadMessages) {
        auto moved_unread = local_unread_ids.intersect(remote_read_ids);

        to_download += moved_unread;
      }
    }
    else {
      qWarningNN << LOGSEC_GREADER << "Performing feed-based contents fetching.";
    }

    Feed::Status error;
    QList<QString> to_download_list(to_download.values());

    if (!to_download_list.isEmpty()) {
      if (m_service == GreaderServiceRoot::Service::Reedah) {
        for (int i = 0; i < to_download_list.size(); i++) {
          to_download_list.replace(i, convertLongStreamIdToShortStreamId(to_download_list.at(i)));
        }
      }

      m_prefetchedMessages = itemContents(root, to_download_list, error, proxy);
    }
  }
  catch (const FeedFetchException& fex) {
    m_prefetchedStatus = fex.feedStatus();

    qCriticalNN << LOGSEC_CORE
                << "Failed to fetch item IDs for common stream:"
                << QUOTE_W_SPACE_DOT(fex.message());
  }
  catch (const NetworkException& nex) {
    m_prefetchedStatus = Feed::Status::NetworkError;

    qCriticalNN << LOGSEC_CORE
                << "Failed to fetch item IDs for common stream:"
                << QUOTE_W_SPACE_DOT(nex.message());
  }
  catch (const ApplicationException& aex) {
    m_prefetchedStatus = Feed::Status::OtherError;

    qCriticalNN << LOGSEC_CORE
                << "Failed to fetch item IDs for common stream:"
                << QUOTE_W_SPACE_DOT(aex.message());
  }
}

QList<Message> GreaderNetwork::getMessagesIntelligently(ServiceRoot* root,
                                                        const QString& stream_id,
                                                        const QHash<ServiceRoot::BagOfMessages, QStringList>& stated_messages,
                                                        const QHash<QString, QStringList>& tagged_messages,
                                                        Feed::Status& error,
                                                        const QNetworkProxy& proxy) {
  Q_UNUSED(tagged_messages)

  QList<Message> msgs;

  if (m_prefetchedStatus != Feed::Status::Normal) {
    error = m_prefetchedStatus;
    return msgs;
  }

  if (!m_performGlobalFetching) {
    // 1. Get unread IDs for a feed.
    // 2. Get read IDs for a feed.
    // 3. Download messages/contents for missing or changed IDs.
    // 4. Add prefetched starred msgs.
    QStringList remote_all_ids_list, remote_unread_ids_list;

    try {
      remote_all_ids_list = m_downloadOnlyUnreadMessages
                                      ? QStringList()
                                      : itemIds(stream_id, false, proxy, -1, m_newerThanFilter);
      remote_unread_ids_list = itemIds(stream_id, true, proxy, -1, m_newerThanFilter);
    }
    catch (const FeedFetchException& fex) {
      error = fex.feedStatus();

      qCriticalNN << LOGSEC_CORE
                  << "Failed to fetch item IDs for specific stream:"
                  << QUOTE_W_SPACE_DOT(fex.message());

      return msgs;
    }
    catch (const NetworkException& nex) {
      error = Feed::Status::NetworkError;

      qCriticalNN << LOGSEC_CORE
                  << "Failed to fetch item IDs for specific stream:"
                  << QUOTE_W_SPACE_DOT(nex.message());

      return msgs;
    }
    catch (const ApplicationException& aex) {
      error = Feed::Status::OtherError;

      qCriticalNN << LOGSEC_CORE
                  << "Failed to fetch item IDs for specific stream:"
                  << QUOTE_W_SPACE_DOT(aex.message());

      return msgs;
    }

    // Convert item IDs to long form.
    for (int i = 0; i < remote_all_ids_list.size(); i++) {
      remote_all_ids_list.replace(i, convertShortStreamIdToLongStreamId(remote_all_ids_list.at(i)));
    }

    for (int i = 0; i < remote_unread_ids_list.size(); i++) {
      remote_unread_ids_list.replace(i, convertShortStreamIdToLongStreamId(remote_unread_ids_list.at(i)));
    }

    QSet<QString> remote_all_ids = FROM_LIST_TO_SET(QSet<QString>, remote_all_ids_list);

    // 1.
    auto local_unread_ids_list = stated_messages.value(ServiceRoot::BagOfMessages::Unread);
    QSet<QString> remote_unread_ids = FROM_LIST_TO_SET(QSet<QString>, remote_unread_ids_list);
    QSet<QString> local_unread_ids = FROM_LIST_TO_SET(QSet<QString>, local_unread_ids_list);

    // 2.
    auto local_read_ids_list = stated_messages.value(ServiceRoot::BagOfMessages::Read);
    QSet<QString> remote_read_ids = remote_all_ids - remote_unread_ids;
    QSet<QString> local_read_ids = FROM_LIST_TO_SET(QSet<QString>, local_read_ids_list);

    // 3.
    QSet<QString> to_download;

    if (!m_downloadOnlyUnreadMessages) {
      to_download += remote_all_ids - local_read_ids - local_unread_ids;
    }
    else {
      to_download += remote_unread_ids - local_read_ids - local_unread_ids;
    }

    auto moved_read = local_read_ids.intersect(remote_unread_ids);

    to_download += moved_read;

    if (!m_downloadOnlyUnreadMessages) {
      auto moved_unread = local_unread_ids.intersect(remote_read_ids);

      to_download += moved_unread;
    }

    QList<QString> to_download_list(to_download.values());

    if (!to_download_list.isEmpty()) {
      if (m_service == GreaderServiceRoot::Service::Reedah) {
        for (int i = 0; i < to_download_list.size(); i++) {
          to_download_list.replace(i, convertLongStreamIdToShortStreamId(to_download_list.at(i)));
        }
      }

      msgs = itemContents(root, to_download_list, error, proxy);
    }
  }

  // Add prefetched messages.
  for (int i = 0; i < m_prefetchedMessages.size(); i++) {
    auto prefetched_msg = m_prefetchedMessages.at(i);

    if (prefetched_msg.m_feedId == stream_id &&
        !boolinq::from(msgs).any([&prefetched_msg](const Message& ms) {
      return ms.m_customId == prefetched_msg.m_customId;
    })) {
      msgs.append(prefetched_msg);
      m_prefetchedMessages.removeAt(i--);
    }
  }

  return msgs;
}

QNetworkReply::NetworkError GreaderNetwork::markMessagesRead(RootItem::ReadStatus status,
                                                             const QStringList& msg_custom_ids,
                                                             const QNetworkProxy& proxy) {
  return editLabels(QSL(GREADER_API_FULL_STATE_READ), status == RootItem::ReadStatus::Read, msg_custom_ids, proxy);
}

QNetworkReply::NetworkError GreaderNetwork::markMessagesStarred(RootItem::Importance importance,
                                                                const QStringList& msg_custom_ids,
                                                                const QNetworkProxy& proxy) {
  return editLabels(QSL(GREADER_API_FULL_STATE_IMPORTANT),
                    importance == RootItem::Importance::Important,
                    msg_custom_ids,
                    proxy);
}

QStringList GreaderNetwork::itemIds(const QString& stream_id, bool unread_only, const QNetworkProxy& proxy,
                                    int max_count, QDate newer_than) {
  if (!ensureLogin(proxy)) {
    throw FeedFetchException(Feed::Status::AuthError, tr("login failed"));
  }

  QString continuation;
  QStringList ids;

  do {
    QString full_url = generateFullUrl(Operations::ItemIds).arg(m_service == GreaderServiceRoot::Service::TheOldReader
                                                                     ? stream_id
                                                                     : QUrl::toPercentEncoding(stream_id),
                                                                QString::number(max_count <= 0
                                                                                ? GREADET_API_ITEM_IDS_MAX
                                                                                : max_count));
    auto timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();

    if (unread_only) {
      full_url += QSL("&xt=%1").arg(QSL(GREADER_API_FULL_STATE_READ));
    }

    if (!continuation.isEmpty()) {
      full_url += QSL("&c=%1").arg(continuation);
    }

    if (newer_than.isValid()) {
      full_url += QSL("&ot=%1").arg(
#if QT_VERSION < 0x050E00 // Qt < 5.14.0
        QDateTime(newer_than)
#else
        newer_than.startOfDay()
#endif
        .toSecsSinceEpoch());
    }

    QByteArray output_stream;
    auto result_stream = NetworkFactory::performNetworkOperation(full_url,
                                                                 timeout,
                                                                 {},
                                                                 output_stream,
                                                                 QNetworkAccessManager::Operation::GetOperation,
                                                                 { authHeader() },
                                                                 false,
                                                                 {},
                                                                 {},
                                                                 proxy);

    if (result_stream.m_networkError != QNetworkReply::NetworkError::NoError) {
      qCriticalNN << LOGSEC_GREADER
                  << "Cannot download item IDs for "
                  << QUOTE_NO_SPACE(stream_id)
                  << ", network error:"
                  << QUOTE_W_SPACE_DOT(result_stream.m_networkError);
      throw NetworkException(result_stream.m_networkError);
    }
    else {
      ids.append(decodeItemIds(output_stream, continuation));
    }
  }
  while (!continuation.isEmpty());

  return ids;
}

QList<Message> GreaderNetwork::itemContents(ServiceRoot* root, const QList<QString>& stream_ids,
                                            Feed::Status& error, const QNetworkProxy& proxy) {
  QString continuation;

  if (!ensureLogin(proxy)) {
    error = Feed::Status::AuthError;
    return {};
  }

  QList<Message> msgs;
  QList<QString> my_stream_ids(stream_ids);

  while (!my_stream_ids.isEmpty()) {
    int batch = (m_service == GreaderServiceRoot::Service::TheOldReader ||
                 m_service == GreaderServiceRoot::Service::FreshRss)
                ? TOR_ITEM_CONTENTS_BATCH
                : (m_service == GreaderServiceRoot::Service::Inoreader
                ? INO_ITEM_CONTENTS_BATCH
                : GREADER_API_ITEM_CONTENTS_BATCH);
    QList<QString> batch_ids = my_stream_ids.mid(0, batch);

    my_stream_ids = my_stream_ids.mid(batch);

    do {
      QString full_url = generateFullUrl(Operations::ItemContents);
      auto timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();

      if (!continuation.isEmpty()) {
        full_url += QSL("&c=%1").arg(continuation);
      }

      std::list inp = boolinq::from(batch_ids).select([this](const QString& id) {
        return QSL("i=%1").arg(m_service == GreaderServiceRoot::Service::TheOldReader
                             ? id
                             : QUrl::toPercentEncoding(id));
      }).toStdList();
      QByteArray input = FROM_STD_LIST(QStringList, inp).join(QSL("&")).toUtf8();
      QByteArray output_stream;
      auto result_stream = NetworkFactory::performNetworkOperation(full_url,
                                                                   timeout,
                                                                   input,
                                                                   output_stream,
                                                                   QNetworkAccessManager::Operation::PostOperation,
                                                                   { authHeader(),
                                                                     { QSL(HTTP_HEADERS_CONTENT_TYPE).toLocal8Bit(),
                                                                       QSL("application/x-www-form-urlencoded").toLocal8Bit() } },
                                                                   false,
                                                                   {},
                                                                   {},
                                                                   proxy);

      if (result_stream.m_networkError != QNetworkReply::NetworkError::NoError) {
        qCriticalNN << LOGSEC_GREADER
                    << "Cannot download messages for "
                    << batch_ids
                    << ", network error:"
                    << QUOTE_W_SPACE_DOT(result_stream.m_networkError);
        error = Feed::Status::NetworkError;
        return {};
      }
      else {
        msgs.append(decodeStreamContents(root, output_stream, QString(), continuation));
      }
    }
    while (!continuation.isEmpty());
  }

  error = Feed::Status::Normal;
  return msgs;
}

QList<Message> GreaderNetwork::streamContents(ServiceRoot* root, const QString& stream_id,
                                              Feed::Status& error, const QNetworkProxy& proxy) {
  QString continuation;

  if (!ensureLogin(proxy)) {
    error = Feed::Status::AuthError;
    return {};
  }

  QList<Message> msgs;
  int target_msgs_size = batchSize() <= 0 ? 2000000: batchSize();

  do {
    QString full_url = generateFullUrl(Operations::StreamContents).arg(
      (m_service == GreaderServiceRoot::Service::TheOldReader ||
       m_service == GreaderServiceRoot::Service::FreshRss)
                ? stream_id
                : QUrl::toPercentEncoding(stream_id),
      QString::number(target_msgs_size));
    auto timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();

    if (downloadOnlyUnreadMessages()) {
      full_url += QSL("&xt=%1").arg(QSL(GREADER_API_FULL_STATE_READ));
    }

    if (!continuation.isEmpty()) {
      full_url += QSL("&c=%1").arg(continuation);
    }

    if (m_newerThanFilter.isValid()) {
      full_url += QSL("&ot=%1").arg(
#if QT_VERSION < 0x050E00 // Qt < 5.14.0
        QDateTime(m_newerThanFilter)
#else
        m_newerThanFilter.startOfDay()
#endif
        .toSecsSinceEpoch());
    }

    QByteArray output_stream;
    auto result_stream = NetworkFactory::performNetworkOperation(full_url,
                                                                 timeout,
                                                                 {},
                                                                 output_stream,
                                                                 QNetworkAccessManager::Operation::GetOperation,
                                                                 { authHeader() },
                                                                 false,
                                                                 {},
                                                                 {},
                                                                 proxy);

    if (result_stream.m_networkError != QNetworkReply::NetworkError::NoError) {
      qCriticalNN << LOGSEC_GREADER
                  << "Cannot download messages for "
                  << QUOTE_NO_SPACE(stream_id)
                  << ", network error:"
                  << QUOTE_W_SPACE_DOT(result_stream.m_networkError);
      error = Feed::Status::NetworkError;
      return {};
    }
    else {
      msgs.append(decodeStreamContents(root, output_stream, stream_id, continuation));
    }
  }
  while (!continuation.isEmpty() && msgs.size() < target_msgs_size);

  error = Feed::Status::Normal;
  return msgs;
}

RootItem* GreaderNetwork::categoriesFeedsLabelsTree(bool obtain_icons, const QNetworkProxy& proxy) {
  QString full_url = generateFullUrl(Operations::TagList);
  auto timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();

  if (!ensureLogin(proxy)) {
    return nullptr;
  }

  QByteArray output_labels;
  auto result_labels = NetworkFactory::performNetworkOperation(full_url,
                                                               timeout,
                                                               {},
                                                               output_labels,
                                                               QNetworkAccessManager::Operation::GetOperation,
                                                               { authHeader() },
                                                               false,
                                                               {},
                                                               {},
                                                               proxy);

  if (result_labels.m_networkError != QNetworkReply::NetworkError::NoError) {
    return nullptr;
  }

  full_url = generateFullUrl(Operations::SubscriptionList);
  QByteArray output_feeds;
  auto result_feeds = NetworkFactory::performNetworkOperation(full_url,
                                                              timeout,
                                                              {},
                                                              output_feeds,
                                                              QNetworkAccessManager::Operation::GetOperation,
                                                              { authHeader() },
                                                              false,
                                                              {},
                                                              {},
                                                              proxy);

  if (result_feeds.m_networkError != QNetworkReply::NetworkError::NoError) {
    return nullptr;
  }

  return decodeTagsSubscriptions(output_labels, output_feeds, obtain_icons, proxy);
}

RootItem* GreaderNetwork::decodeTagsSubscriptions(const QString& categories, const QString& feeds,
                                                  bool obtain_icons, const QNetworkProxy& proxy) {
  auto* parent = new RootItem();
  QMap<QString, RootItem*> cats;
  QList<RootItem*> lbls;
  QJsonArray json;

  if (m_service == GreaderServiceRoot::Service::Bazqux ||
      m_service == GreaderServiceRoot::Service::Reedah ||
      m_service == GreaderServiceRoot::Service::Inoreader) {
    // We need to process subscription list first and extract categories.
    json = QJsonDocument::fromJson(feeds.toUtf8()).object()[QSL("subscriptions")].toArray();

    for (const QJsonValue& feed : qAsConst(json)) {
      auto subscription = feed.toObject();
      auto json_cats = subscription[QSL("categories")].toArray();

      for (const QJsonValue& cat : qAsConst(json_cats)) {
        auto cat_obj = cat.toObject();
        auto cat_id = cat_obj[QSL("id")].toString();

        if (!cats.contains(cat_id)) {
          auto* category = new Category();

          category->setTitle(cat_id.mid(cat_id.lastIndexOf(QL1C('/')) + 1));
          category->setCustomId(cat_id);

          cats.insert(category->customId(), category);
          parent->appendChild(category);
        }
      }
    }
  }

  json = QJsonDocument::fromJson(categories.toUtf8()).object()[QSL("tags")].toArray();
  cats.insert(QString(), parent);

  for (const QJsonValue& obj : qAsConst(json)) {
    auto label = obj.toObject();
    QString label_id = label[QSL("id")].toString();

    if ((label[QSL("type")].toString() == QSL("folder") ||
         (m_service == GreaderServiceRoot::Service::TheOldReader &&
          label_id.contains(QSL("/label/"))))) {

      // We have category (not "state" or "tag" or "label").
      auto* category = new Category();

      category->setDescription(label[QSL("htmlUrl")].toString());
      category->setTitle(label_id.mid(label_id.lastIndexOf(QL1C('/')) + 1));
      category->setCustomId(label_id);

      cats.insert(category->customId(), category);
      parent->appendChild(category);
    }
    else if (label[QSL("type")] == QSL("tag")) {
      QString plain_name = QRegularExpression(".+\\/([^\\/]+)").match(label_id).captured(1);
      auto* new_lbl = new Label(plain_name, TextFactory::generateColorFromText(label_id));

      new_lbl->setCustomId(label_id);
      lbls.append(new_lbl);
    }
    else if ((m_service == GreaderServiceRoot::Service::Bazqux ||
              m_service == GreaderServiceRoot::Service::Reedah ||
              m_service == GreaderServiceRoot::Service::Inoreader) &&
             label_id.contains(QSL("/label/"))) {
      if (!cats.contains(label_id)) {
        // This stream is not a category, it is label, bitches!
        QString plain_name = QRegularExpression(QSL(".+\\/([^\\/]+)")).match(label_id).captured(1);
        auto* new_lbl = new Label(plain_name, TextFactory::generateColorFromText(label_id));

        new_lbl->setCustomId(label_id);
        lbls.append(new_lbl);
      }
    }
  }

  json = QJsonDocument::fromJson(feeds.toUtf8()).object()[QSL("subscriptions")].toArray();

  for (const QJsonValue& obj : qAsConst(json)) {
    auto subscription = obj.toObject();
    QString id = subscription[QSL("id")].toString();
    QString title = subscription[QSL("title")].toString();
    QString url = subscription[QSL("htmlUrl")].toString();
    QString parent_label;
    QJsonArray assigned_categories = subscription[QSL("categories")].toArray();

    if (id.startsWith(TOR_SPONSORED_STREAM_ID)) {
      continue;
    }

    for (const QJsonValue& cat : qAsConst(assigned_categories)) {
      QString potential_id = cat.toObject()[QSL("id")].toString();

      if (potential_id.contains(QSL("/label/"))) {
        parent_label = potential_id;
        break;
      }
    }

    // We have label (not "state").
    auto* feed = new Feed();

    feed->setDescription(url);
    feed->setSource(url);
    feed->setTitle(title);
    feed->setCustomId(id);

    if (obtain_icons) {
      QString icon_url = subscription[QSL("iconUrl")].toString();
      QList<QPair<QString, bool>> icon_urls;

      if (!icon_url.isEmpty()) {
        if (icon_url.startsWith(QSL("//"))) {
          icon_url = QUrl(baseUrl()).scheme() + QSL(":") + icon_url;
        }
        else if (service() == GreaderServiceRoot::Service::FreshRss) {
          QUrl icon_url_obj(icon_url);
          QUrl base_url(baseUrl());

          if (icon_url_obj.host() == base_url.host()) {
            icon_url_obj.setPort(base_url.port());
            icon_url = icon_url_obj.toString();
          }
        }

        icon_urls.append({ icon_url, true });
      }

      icon_urls.append({ url, false });

      QIcon icon;

      if (NetworkFactory::downloadIcon(icon_urls,
                                       1000,
                                       icon,
                                       {},
                                       proxy) == QNetworkReply::NetworkError::NoError) {
        feed->setIcon(icon);
      }
    }

    if (cats.contains(parent_label)) {
      cats[parent_label]->appendChild(feed);
    }
  }

  auto* lblroot = new LabelsNode(parent);

  lblroot->setChildItems(lbls);
  parent->appendChild(lblroot);

  return parent;
}

QNetworkReply::NetworkError GreaderNetwork::clientLogin(const QNetworkProxy& proxy) {
  QString full_url = generateFullUrl(Operations::ClientLogin);
  auto timeout = qApp->settings()->value(GROUP(Feeds), SETTING(Feeds::UpdateTimeout)).toInt();
  QByteArray output;
  QByteArray args = QSL("Email=%1&Passwd=%2").arg(QString::fromLocal8Bit(QUrl::toPercentEncoding(username())),
                                                  QString::fromLocal8Bit(QUrl::toPercentEncoding(password()))).toLocal8Bit();
  auto network_result = NetworkFactory::performNetworkOperation(full_url,
                                                                timeout,
                                                                args,
                                                                output,
                                                                QNetworkAccessManager::Operation::PostOperation,
                                                                { {
                                                                  QSL(HTTP_HEADERS_CONTENT_TYPE).toLocal8Bit(),
                                                                  QSL("application/x-www-form-urlencoded").toLocal8Bit()
                                                                } },
                                                                false,
                                                                {},
                                                                {},
                                                                proxy);

  if (network_result.m_networkError == QNetworkReply::NetworkError::NoError) {
    // Save credentials.
    auto lines = QString::fromUtf8(output).replace(QSL("\r"), QString()).split('\n');

    for (const QString& line : lines) {
      int eq = line.indexOf('=');

      if (eq > 0) {
        QString id = line.mid(0, eq);

        if (id == QSL("SID")) {
          m_authSid = line.mid(eq + 1);
        }
        else if (id == QSL("Auth")) {
          m_authAuth = line.mid(eq + 1);
        }
      }
    }

    QRegularExpression exp(QSL("^(NA|unused|none|null)$"));

    if (exp.match(m_authSid).hasMatch()) {
      m_authSid = QString();
    }

    if (exp.match(m_authAuth).hasMatch()) {
      m_authAuth = QString();
    }

    if (m_authAuth.isEmpty()) {
      clearCredentials();
      return QNetworkReply::NetworkError::InternalServerError;
    }

    if (m_service == GreaderServiceRoot::Service::Reedah) {
      // We need "T=" token for editing.
      full_url = generateFullUrl(Operations::Token);

      network_result = NetworkFactory::performNetworkOperation(full_url,
                                                               timeout,
                                                               args,
                                                               output,
                                                               QNetworkAccessManager::Operation::GetOperation,
                                                               { authHeader() },
                                                               false,
                                                               {},
                                                               {},
                                                               proxy);

      if (network_result.m_networkError == QNetworkReply::NetworkError::NoError) {
        m_authToken = output;
      }
      else {
        clearCredentials();
      }
    }
  }

  return network_result.m_networkError;
}

GreaderServiceRoot::Service GreaderNetwork::service() const {
  return m_service;
}

void GreaderNetwork::setService(GreaderServiceRoot::Service service) {
  m_service = service;
}

QString GreaderNetwork::username() const {
  return m_username;
}

void GreaderNetwork::setUsername(const QString& username) {
  m_username = username;
}

QString GreaderNetwork::password() const {
  return m_password;
}

void GreaderNetwork::setPassword(const QString& password) {
  m_password = password;
}

QString GreaderNetwork::baseUrl() const {
  return m_baseUrl;
}

void GreaderNetwork::setBaseUrl(const QString& base_url) {
  m_baseUrl = base_url;
}

QPair<QByteArray, QByteArray> GreaderNetwork::authHeader() const {
  if (m_service == GreaderServiceRoot::Service::Inoreader) {
    return { QSL(HTTP_HEADERS_AUTHORIZATION).toLocal8Bit(),
             m_oauth->bearer().toLocal8Bit() };
  }
  else {
    return { QSL(HTTP_HEADERS_AUTHORIZATION).toLocal8Bit(),
             QSL("GoogleLogin auth=%1").arg(m_authAuth).toLocal8Bit() };
  }
}

bool GreaderNetwork::ensureLogin(const QNetworkProxy& proxy, QNetworkReply::NetworkError* output) {
  if (m_service == GreaderServiceRoot::Service::Inoreader) {
    return !m_oauth->bearer().isEmpty();
  }

  if (m_authSid.isEmpty() && m_authAuth.isEmpty()) {
    auto login = clientLogin(proxy);

    if (output != nullptr) {
      *output = login;
    }

    if (login != QNetworkReply::NetworkError::NoError) {
      qCriticalNN << LOGSEC_GREADER
                  << "Login failed with error:"
                  << QUOTE_W_SPACE_DOT(NetworkFactory::networkErrorText(login));
      return false;
    }
    else {
      qDebugNN << LOGSEC_GREADER << "Login successful.";
    }
  }

  return true;
}

QString GreaderNetwork::convertLongStreamIdToShortStreamId(const QString& stream_id) const {
  return QString::number(QString(stream_id).replace(QSL("tag:google.com,2005:reader/item/"),
                                                    QString()).toULongLong(nullptr, 16));
}

QString GreaderNetwork::convertShortStreamIdToLongStreamId(const QString& stream_id) const {
  if (stream_id.startsWith(QSL("tag:google.com,2005:reader/item/"))) {
    return stream_id;
  }

  if (m_service == GreaderServiceRoot::Service::TheOldReader) {
    return QSL("tag:google.com,2005:reader/item/%1").arg(stream_id);
  }
  else {
    return QSL("tag:google.com,2005:reader/item/%1").arg(stream_id.toULongLong(),
                                                         16,
                                                         16,
                                                         QL1C('0'));
  }
}

QString GreaderNetwork::simplifyStreamId(const QString& stream_id) const {
  return QString(stream_id).replace(QRegularExpression(QSL("\\/\\d+\\/")), QSL("/-/"));
}

QStringList GreaderNetwork::decodeItemIds(const QString& stream_json_data, QString& continuation) {
  QStringList ids;
  QJsonDocument json_doc = QJsonDocument::fromJson(stream_json_data.toUtf8());
  QJsonArray json = json_doc.object()[QSL("itemRefs")].toArray();

  continuation = json_doc.object()[QSL("continuation")].toString();
  ids.reserve(json.count());

  for (const QJsonValue& id : json) {
    ids.append(id.toObject()[QSL("id")].toString());
  }

  return ids;
}

QList<Message> GreaderNetwork::decodeStreamContents(ServiceRoot* root,
                                                    const QString& stream_json_data,
                                                    const QString& stream_id,
                                                    QString& continuation) {
  QList<Message> messages;
  QJsonDocument json_doc = QJsonDocument::fromJson(stream_json_data.toUtf8());
  QJsonArray json = json_doc.object()[QSL("items")].toArray();
  auto active_labels = root->labelsNode() != nullptr ? root->labelsNode()->labels() : QList<Label*>();

  continuation = json_doc.object()[QSL("continuation")].toString();
  messages.reserve(json.count());

  for (const QJsonValue& obj : json) {
    auto message_obj = obj.toObject();
    Message message;

    message.m_title = qApp->web()->unescapeHtml(message_obj[QSL("title")].toString());
    message.m_author = qApp->web()->unescapeHtml(message_obj[QSL("author")].toString());
    message.m_created = QDateTime::fromSecsSinceEpoch(message_obj[QSL("published")].toInt(), Qt::TimeSpec::UTC);
    message.m_createdFromFeed = true;
    message.m_customId = message_obj[QSL("id")].toString();

    auto alternates = message_obj[QSL("alternate")].toArray();
    auto enclosures = message_obj[QSL("enclosure")].toArray();
    auto categories = message_obj[QSL("categories")].toArray();

    for (const QJsonValue& alt : alternates) {
      auto alt_obj = alt.toObject();
      QString mime = alt_obj[QSL("type")].toString();
      QString href = alt_obj[QSL("href")].toString();

      if (mime.isEmpty() || mime == QL1S("text/html")) {
        message.m_url = href;
      }
      else {
        message.m_enclosures.append(Enclosure(href, mime));
      }
    }

    for (const QJsonValue& enc : enclosures) {
      auto enc_obj = enc.toObject();
      QString mime = enc_obj[QSL("type")].toString();
      QString href = enc_obj[QSL("href")].toString();

      message.m_enclosures.append(Enclosure(href, mime));
    }

    for (const QJsonValue& cat : categories) {
      QString category = cat.toString();

      if (category.endsWith(QSL(GREADER_API_STATE_READ))) {
        message.m_isRead = true;
      }
      else if (category.endsWith(QSL(GREADER_API_STATE_IMPORTANT))) {
        message.m_isImportant = true;
      }
      else if (category.contains(QSL("label"))) {
        Label* label = boolinq::from(active_labels.begin(), active_labels.end()).firstOrDefault([category](Label* lbl) {
          return lbl->customId() == category;
        });

        if (label != nullptr) {
          // We found live Label object for our assigned label.
          message.m_assignedLabels.append(label);
        }
      }
    }

    message.m_contents = message_obj[QSL("summary")].toObject()[QSL("content")].toString();
    message.m_rawContents = QJsonDocument(message_obj).toJson(QJsonDocument::JsonFormat::Compact);
    message.m_feedId = stream_id.isEmpty()
                       ? message_obj[QSL("origin")].toObject()[QSL("streamId")].toString()
                       : stream_id;

    if (message.m_title.isEmpty()) {
      message.m_title = message.m_url;
    }

    messages.append(message);
  }

  return messages;
}

int GreaderNetwork::batchSize() const {
  return m_batchSize;
}

void GreaderNetwork::setBatchSize(int batch_size) {
  m_batchSize = batch_size;
}

void GreaderNetwork::clearCredentials() {
  m_authAuth = m_authSid = m_authToken = QString();
}

QString GreaderNetwork::sanitizedBaseUrl() const {
  QString base_url = m_service == GreaderServiceRoot::Service::Inoreader
                     ? QSL(GREADER_URL_INOREADER)
                     : m_baseUrl;

  if (!base_url.endsWith('/')) {
    base_url = base_url + QL1C('/');
  }

  switch (m_service) {
    case GreaderServiceRoot::Service::FreshRss:
      base_url += QSL(FRESHRSS_BASE_URL_PATH);
      break;

    default:
      break;
  }

  return base_url;
}

QString GreaderNetwork::generateFullUrl(GreaderNetwork::Operations operation) const {
  switch (operation) {
    case Operations::ClientLogin:
      return sanitizedBaseUrl() + QSL(GREADER_API_CLIENT_LOGIN);

    case Operations::Token:
      return sanitizedBaseUrl() + QSL(GREADER_API_TOKEN);

    case Operations::TagList:
      return sanitizedBaseUrl() + QSL(GREADER_API_TAG_LIST);

    case Operations::SubscriptionList:
      return sanitizedBaseUrl() + QSL(GREADER_API_SUBSCRIPTION_LIST);

    case Operations::StreamContents:
      return sanitizedBaseUrl() + QSL(GREADER_API_STREAM_CONTENTS);

    case Operations::UserInfo:
      return sanitizedBaseUrl() + QSL(GREADER_API_USER_INFO);

    case Operations::EditTag:
      return sanitizedBaseUrl() + QSL(GREADER_API_EDIT_TAG);

    case Operations::ItemIds:
      return sanitizedBaseUrl() + QSL(GREADER_API_ITEM_IDS);

    case Operations::ItemContents:
      return sanitizedBaseUrl() + QSL(GREADER_API_ITEM_CONTENTS);

    default:
      return sanitizedBaseUrl();
  }
}

void GreaderNetwork::onTokensError(const QString& error, const QString& error_description) {
  Q_UNUSED(error)

  qApp->showGuiMessage(Notification::Event::LoginFailure, {
    tr("Inoreader: authentication error"),
    tr("Click this to login again. Error is: '%1'").arg(error_description),
    QSystemTrayIcon::MessageIcon::Critical },
                       {}, {
    tr("Login"),
    [this]() {
      m_oauth->setAccessToken(QString());
      m_oauth->setRefreshToken(QString());
      m_oauth->login();
    } });
}

void GreaderNetwork::onAuthFailed() {
  qApp->showGuiMessage(Notification::Event::LoginFailure, {
    tr("Inoreader: authorization denied"),
    tr("Click this to login again."),
    QSystemTrayIcon::MessageIcon::Critical },
                       {}, {
    tr("Login"),
    [this]() {
      m_oauth->login();
    } });
}

void GreaderNetwork::initializeOauth() {
#if defined(INOREADER_OFFICIAL_SUPPORT)
  m_oauth->setClientSecretId(TextFactory::decrypt(QSL(INOREADER_CLIENT_ID), OAUTH_DECRYPTION_KEY));
  m_oauth->setClientSecretSecret(TextFactory::decrypt(QSL(INOREADER_CLIENT_SECRET), OAUTH_DECRYPTION_KEY));
#endif

  m_oauth->setRedirectUrl(QSL(OAUTH_REDIRECT_URI) +
                          QL1C(':') +
                          QString::number(INO_OAUTH_REDIRECT_URI_PORT),
                          false);

  connect(m_oauth, &OAuth2Service::tokensRetrieveError, this, &GreaderNetwork::onTokensError);
  connect(m_oauth, &OAuth2Service::authFailed, this, &GreaderNetwork::onAuthFailed);
  connect(m_oauth, &OAuth2Service::tokensRetrieved, this, [this](QString access_token, QString refresh_token, int expires_in) {
    Q_UNUSED(expires_in)
    Q_UNUSED(access_token)

    if (m_root != nullptr && m_root->accountId() > 0 && !refresh_token.isEmpty()) {
      QSqlDatabase database = qApp->database()->driver()->connection(metaObject()->className());

      DatabaseQueries::storeNewOauthTokens(database, refresh_token, m_root->accountId());
    }
  });
}

QDate GreaderNetwork::newerThanFilter() const {
  return m_newerThanFilter;
}

void GreaderNetwork::setNewerThanFilter(const QDate& newer_than) {
  m_newerThanFilter = newer_than;
}

OAuth2Service* GreaderNetwork::oauth() const {
  return m_oauth;
}

void GreaderNetwork::setOauth(OAuth2Service* oauth) {
  m_oauth = oauth;
}

void GreaderNetwork::setRoot(GreaderServiceRoot* root) {
  m_root = root;
}

bool GreaderNetwork::intelligentSynchronization() const {
  return m_intelligentSynchronization;
}

void GreaderNetwork::setIntelligentSynchronization(bool intelligent_synchronization) {
  m_intelligentSynchronization = intelligent_synchronization;
}

bool GreaderNetwork::downloadOnlyUnreadMessages() const {
  return m_downloadOnlyUnreadMessages;
}

void GreaderNetwork::setDownloadOnlyUnreadMessages(bool download_only_unread) {
  m_downloadOnlyUnreadMessages = download_only_unread;
}
