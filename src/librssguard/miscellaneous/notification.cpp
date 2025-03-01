// For license of this file, see <project-root-folder>/LICENSE.md.

#include "miscellaneous/notification.h"

#include "miscellaneous/application.h"

#include <QDir>

#if !defined(Q_OS_OS2)
#include <QMediaPlayer>
#endif

Notification::Notification(Notification::Event event, bool balloon, const QString& sound_path, int volume)
  : m_event(event), m_balloonEnabled(balloon), m_soundPath(sound_path), m_volume(volume) {}

Notification::Event Notification::event() const {
  return m_event;
}

void Notification::setEvent(Event event) {
  m_event = event;
}

QString Notification::soundPath() const {
  return m_soundPath;
}

void Notification::setSoundPath(const QString& sound_path) {
  m_soundPath = sound_path;
}

void Notification::playSound(Application* app) const {
  if (!m_soundPath.isEmpty()) {
#if !defined(Q_OS_OS2)
    QMediaPlayer* play = new QMediaPlayer(app);

    QObject::connect(play, &QMediaPlayer::stateChanged, play, [play](QMediaPlayer::State state) {
      if (state == QMediaPlayer::State::StoppedState) {
        play->deleteLater();
      }
    });

    if (m_soundPath.startsWith(QSL(":"))) {
      play->setMedia(QMediaContent(QUrl(QSL("qrc") + m_soundPath)));

    }
    else {
      play->setMedia(QMediaContent(
                       QUrl::fromLocalFile(
                         QDir::toNativeSeparators(app->replaceDataUserDataFolderPlaceholder(m_soundPath)))));
    }

    play->setVolume(m_volume);
    play->play();
#endif
  }
}

QList<Notification::Event> Notification::allEvents() {
  return {
    Event::GeneralEvent,
    Event::NewUnreadArticlesFetched,
    Event::ArticlesFetchingStarted,
    Event::LoginDataRefreshed,
    Event::LoginFailure,
    Event::NewAppVersionAvailable,
  };
}

QString Notification::nameForEvent(Notification::Event event) {
  switch (event) {
    case Notification::Event::NewUnreadArticlesFetched:
      return QObject::tr("New (unread) articles fetched");

    case Notification::Event::ArticlesFetchingStarted:
      return QObject::tr("Fetching articles right now");

    case Notification::Event::LoginDataRefreshed:
      return QObject::tr("Login data refreshed");

    case Notification::Event::LoginFailure:
      return QObject::tr("Login failed");

    case Notification::Event::NewAppVersionAvailable:
      return QObject::tr("New %1 version is available").arg(QSL(APP_NAME));

    case Notification::Event::GeneralEvent:
      return QObject::tr("Miscellaneous events");

    default:
      return QObject::tr("Unknown event");
  }
}

int Notification::volume() const {
  return m_volume;
}

void Notification::setVolume(int volume) {
  m_volume = volume;
}

bool Notification::balloonEnabled() const {
  return m_balloonEnabled;
}
