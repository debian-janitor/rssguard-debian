// For license of this file, see <project-root-folder>/LICENSE.md.

#ifndef TTRSSFEED_H
#define TTRSSFEED_H

#include "services/abstract/feed.h"

class TtRssServiceRoot;

class TtRssFeed : public Feed {
  Q_OBJECT

  public:
    explicit TtRssFeed(RootItem* parent = nullptr);

    virtual bool canBeDeleted() const;
    virtual bool deleteViaGui();
    virtual QList<QAction*> contextMenuFeedsList();

  private:
    TtRssServiceRoot* serviceRoot() const;
    bool removeItself();

  private:
    QAction* m_actionShareToPublished;
};

#endif // TTRSSFEED_H
