// For license of this file, see <project-root-folder>/LICENSE.md.

#include "services/standard/gui/formeditstandardaccount.h"

#include "miscellaneous/application.h"
#include "database/databasefactory.h"
#include "database/databasequeries.h"
#include "miscellaneous/iconfactory.h"
#include "services/standard/standardserviceroot.h"

FormEditStandardAccount::FormEditStandardAccount(QWidget* parent)
  : FormAccountDetails(qApp->icons()->fromTheme(QSL("rssguard")), parent) {}

void FormEditStandardAccount::apply() {
  FormAccountDetails::apply();

  m_account->saveAccountDataToDatabase();
  accept();
}
