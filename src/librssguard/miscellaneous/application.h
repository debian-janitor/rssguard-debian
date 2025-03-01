// For license of this file, see <project-root-folder>/LICENSE.md.

#ifndef APPLICATION_H
#define APPLICATION_H

#include "core/feeddownloader.h"
#include "database/databasefactory.h"
#include "definitions/definitions.h"
#include "gui/systemtrayicon.h"
#include "miscellaneous/feedreader.h"
#include "miscellaneous/iofactory.h"
#include "miscellaneous/localization.h"
#include "miscellaneous/notification.h"
#include "miscellaneous/settings.h"
#include "miscellaneous/singleapplication.h"
#include "miscellaneous/skinfactory.h"
#include "miscellaneous/systemfactory.h"
#include "network-web/downloadmanager.h"

#include <QCommandLineParser>
#include <QList>

#include <functional>

#if defined(qApp)
#undef qApp
#endif

// Define new qApp macro. Yeaaaaah.
#define qApp (Application::instance())

class FormMain;
class IconFactory;
class QAction;
class Mutex;
class QWebEngineDownloadItem;
class WebFactory;
class NotificationFactory;

class RSSGUARD_DLLSPEC Application : public SingleApplication {
  Q_OBJECT

  public:
    explicit Application(const QString& id, int& argc, char** argv);
    virtual ~Application();

    void reactOnForeignNotifications();
    void hideOrShowMainForm();
    void loadDynamicShortcuts();
    void showPolls() const;
    void offerChanges() const;

    bool isAlreadyRunning();

    QStringList builtinSounds() const;

    FeedReader* feedReader();
    void setFeedReader(FeedReader* feed_reader);

    // Globally accessible actions.
    QList<QAction*> userActions();

    // Check whether this application starts for the first time (ever).
    bool isFirstRun() const;

    // Check whether CURRENT VERSION of the application starts for the first time.
    bool isFirstRunCurrentVersion() const;

    QCommandLineParser* cmdParser();
    WebFactory* web() const;
    SystemFactory* system();
    SkinFactory* skins();
    Localization* localization();
    DatabaseFactory* database();
    IconFactory* icons();
    DownloadManager* downloadManager();
    Settings* settings() const;
    Mutex* feedUpdateLock();
    FormMain* mainForm();
    QWidget* mainFormWidget();
    SystemTrayIcon* trayIcon();
    NotificationFactory* notifications() const;

    QIcon desktopAwareIcon() const;

    QString tempFolder() const;
    QString documentsFolder() const;
    QString homeFolder() const;
    QString configFolder() const;

    // These return user ready folders.
    QString userDataAppFolder() const;
    QString userDataHomeFolder() const;
    QString customDataFolder() const;

    // Returns the base folder to which store user data, the "data" folder.
    // NOTE: Use this to get correct path under which store user data.
    QString userDataFolder();

    QString replaceDataUserDataFolderPlaceholder(QString text) const;
    QStringList replaceDataUserDataFolderPlaceholder(QStringList texts) const;

    void setMainForm(FormMain* main_form);

    void backupDatabaseSettings(bool backup_database, bool backup_settings,
                                const QString& target_path, const QString& backup_name);
    void restoreDatabaseSettings(bool restore_database, bool restore_settings,
                                 const QString& source_database_file_path = QString(),
                                 const QString& source_settings_file_path = QString());

    void showTrayIcon();
    void deleteTrayIcon();

    // Displays given simple message in tray icon bubble or OSD
    // or in message box if tray icon is disabled.
    void showGuiMessage(Notification::Event event, const QString& title, const QString& message,
                        QSystemTrayIcon::MessageIcon message_type, bool show_at_least_msgbox = false,
                        QWidget* parent = nullptr, const QString& functor_heading = {},
                        std::function<void()> functor = nullptr);

    // Returns pointer to "GOD" application singleton.
    static Application* instance();

    // Custom debug/console log handler.
    static void performLogging(QtMsgType type, const QMessageLogContext& context, const QString& msg);

  public slots:

    // Restarts the application.
    void restart();

    // Processes incoming message from another RSS Guard instance.
    void parseCmdArgumentsFromOtherInstance(const QString& message);
    void parseCmdArgumentsFromMyInstance();

  private slots:
    void onCommitData(QSessionManager& manager);
    void onSaveState(QSessionManager& manager);
    void onAboutToQuit();
    void showMessagesNumber(int unread_messages, bool any_feed_has_unread_messages);

#if defined(USE_WEBENGINE)
    void downloadRequested(QWebEngineDownloadItem* download_item);
    void onAdBlockFailure();
#endif

    void onFeedUpdatesFinished(const FeedDownloadResults& results);

  private:
    void setupCustomDataFolder(const QString& data_folder);
    void determineFirstRuns();
    void eliminateFirstRuns();

  private:
    QCommandLineParser m_cmdParser;
    FeedReader* m_feedReader;

    bool m_quitLogicDone;

    // This read-write lock is used by application on its close.
    // Application locks this lock for WRITING.
    // This means that if application locks that lock, then
    // no other transaction-critical action can acquire lock
    // for reading and won't be executed, so no critical action
    // will be running when application quits
    //
    // EACH critical action locks this lock for READING.
    // Several actions can lock this lock for reading.
    // But of user decides to close the application (in other words,
    // tries to lock the lock for writing), then no other
    // action will be allowed to lock for reading.
    QScopedPointer<Mutex> m_updateFeedsLock;

    QList<QAction*> m_userActions;
    FormMain* m_mainForm;
    SystemTrayIcon* m_trayIcon;
    Settings* m_settings;
    WebFactory* m_webFactory;
    SystemFactory* m_system;
    SkinFactory* m_skins;
    Localization* m_localization;
    IconFactory* m_icons;
    DatabaseFactory* m_database;
    DownloadManager* m_downloadManager;
    NotificationFactory* m_notifications;
    bool m_shouldRestart;
    bool m_firstRunEver;
    bool m_firstRunCurrentVersion;
    QString m_customDataFolder;
    bool m_allowMultipleInstances;
};

inline Application* Application::instance() {
  return static_cast<Application*>(QCoreApplication::instance());
}

#endif // APPLICATION_H
