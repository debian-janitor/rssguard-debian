// For license of this file, see <project-root-folder>/LICENSE.md.

#include "services/standard/standardcategory.h"

#include "core/feedsmodel.h"
#include "database/databasequeries.h"
#include "definitions/definitions.h"
#include "exceptions/applicationexception.h"
#include "gui/feedmessageviewer.h"
#include "gui/feedsview.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/settings.h"
#include "miscellaneous/textfactory.h"
#include "services/abstract/gui/formcategorydetails.h"
#include "services/standard/standardfeed.h"
#include "services/standard/standardserviceroot.h"

#include <QPointer>

StandardCategory::StandardCategory(RootItem* parent_item) : Category(parent_item) {}

StandardServiceRoot* StandardCategory::serviceRoot() const {
  return qobject_cast<StandardServiceRoot*>(getParentServiceRoot());
}

Qt::ItemFlags StandardCategory::additionalFlags() const {
  return Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
}

bool StandardCategory::performDragDropChange(RootItem* target_item) {
  QSqlDatabase database = qApp->database()->driver()->connection(metaObject()->className());

  try {
    DatabaseQueries::createOverwriteCategory(database, this, getParentServiceRoot()->accountId(), target_item->id());
    serviceRoot()->requestItemReassignment(this, target_item);
    return true;
  }
  catch (const ApplicationException& ex) {
    qCriticalNN << LOGSEC_DB
                << "Cannot overwrite category:"
                << QUOTE_W_SPACE_DOT(ex.message());
    qApp->showGuiMessage(Notification::Event::GeneralEvent,
                         tr("Error"),
                         tr("Cannot save data for category, detailed information was logged via debug log."),
                         QSystemTrayIcon::MessageIcon::Critical,
                         true);
    return false;
  }
}

bool StandardCategory::canBeEdited() const {
  return true;
}

bool StandardCategory::canBeDeleted() const {
  return true;
}

bool StandardCategory::editViaGui() {
  QScopedPointer<FormCategoryDetails> form_pointer(new FormCategoryDetails(serviceRoot(),
                                                                           nullptr,
                                                                           qApp->mainFormWidget()));

  form_pointer->addEditCategory(this);
  return false;
}

bool StandardCategory::deleteViaGui() {
  if (removeItself()) {
    serviceRoot()->requestItemRemoval(this);
    return true;
  }
  else {
    return false;
  }
}

bool StandardCategory::removeItself() {
  bool children_removed = true;

  // Remove all child items (feeds and categories)
  // from the database.
  auto chi = childItems();

  for (RootItem* child : qAsConst(chi)) {
    if (child->kind() == RootItem::Kind::Category) {
      children_removed &= dynamic_cast<StandardCategory*>(child)->removeItself();
    }
    else if (child->kind() == RootItem::Kind::Feed) {
      children_removed &= dynamic_cast<StandardFeed*>(child)->removeItself();
    }
  }

  if (children_removed) {
    // Children are removed, remove this standard category too.
    QSqlDatabase database = qApp->database()->driver()->connection(metaObject()->className());

    return DatabaseQueries::deleteCategory(database, id());
  }
  else {
    return false;
  }
}
