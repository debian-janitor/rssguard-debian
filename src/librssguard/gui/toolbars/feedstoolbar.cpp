// For license of this file, see <project-root-folder>/LICENSE.md.

#include "gui/toolbars/feedstoolbar.h"

#include "gui/reusable/baselineedit.h"
#include "miscellaneous/application.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/settings.h"

#include <QWidgetAction>

FeedsToolBar::FeedsToolBar(const QString& title, QWidget* parent) : BaseToolBar(title, parent) {
  // Update right margin of filter textbox.
  QMargins margins = contentsMargins();

  margins.setRight(margins.right() + FILTER_RIGHT_MARGIN);
  setContentsMargins(margins);

  initializeSearchBox();
}

QList<QAction*> FeedsToolBar::availableActions() const {
  QList<QAction*> available_actions = qApp->userActions();

  available_actions.append(m_actionSearchMessages);

  return available_actions;
}

QList<QAction*> FeedsToolBar::activatedActions() const {
  return actions();
}

void FeedsToolBar::saveAndSetActions(const QStringList& actions) {
  qApp->settings()->setValue(GROUP(GUI), GUI::FeedsToolbarActions, actions.join(QSL(",")));
  loadSpecificActions(convertActions(actions));

  // If user hidden search messages box, then remove the filter.
  if (!activatedActions().contains(m_actionSearchMessages)) {
    m_txtSearchMessages->clear();
  }
}

QList<QAction*> FeedsToolBar::convertActions(const QStringList& actions) {
  QList<QAction*> available_actions = availableActions();
  QList<QAction*> spec_actions;

  // Iterate action names and add respectable actions into the toolbar.
  for (const QString& action_name : actions) {
    QAction* matching_action = findMatchingAction(action_name, available_actions);

    if (matching_action != nullptr) {
      // Add existing standard action.
      spec_actions.append(matching_action);
    }
    else if (action_name == SEPARATOR_ACTION_NAME) {
      // Add new separator.
      auto* act = new QAction(this);

      act->setSeparator(true);
      spec_actions.append(act);
    }
    else if (action_name == SEARCH_BOX_ACTION_NAME) {
      // Add search box.
      spec_actions.append(m_actionSearchMessages);
    }
    else if (action_name == SPACER_ACTION_NAME) {
      // Add new spacer.
      auto* spacer = new QWidget(this);

      spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      auto* action = new QWidgetAction(this);

      action->setDefaultWidget(spacer);
      action->setIcon(qApp->icons()->fromTheme(QSL("system-search")));
      action->setProperty("type", SPACER_ACTION_NAME);
      action->setProperty("name", tr("Toolbar spacer"));
      spec_actions.append(action);
    }
  }

  return spec_actions;
}

void FeedsToolBar::loadSpecificActions(const QList<QAction*>& actions, bool initial_load) {
  Q_UNUSED(initial_load)

  clear();

  for (QAction* act : actions) {
    addAction(act);
  }
}

QStringList FeedsToolBar::defaultActions() const {
  return QString(GUI::FeedsToolbarActionsDef).split(',',
#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
                                                    Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
                                                    QString::SplitBehavior::SkipEmptyParts);
#endif
}

QStringList FeedsToolBar::savedActions() const {
  return qApp->settings()->value(GROUP(GUI),
                                 SETTING(GUI::FeedsToolbarActions)).toString().split(',',
#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
                                                                                     Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
                                                                                     QString::SplitBehavior::SkipEmptyParts);
#endif
}

void FeedsToolBar::initializeSearchBox() {
  m_txtSearchMessages = new BaseLineEdit(this);
  m_txtSearchMessages->setSizePolicy(QSizePolicy::Policy::Expanding, m_txtSearchMessages->sizePolicy().verticalPolicy());
  m_txtSearchMessages->setPlaceholderText(tr("Search feeds"));

  // Setup wrapping action for search box.
  m_actionSearchMessages = new QWidgetAction(this);

  m_actionSearchMessages->setDefaultWidget(m_txtSearchMessages);
  m_actionSearchMessages->setIcon(qApp->icons()->fromTheme(QSL("system-search")));
  m_actionSearchMessages->setProperty("type", SEARCH_BOX_ACTION_NAME);
  m_actionSearchMessages->setProperty("name", tr("Feeds search box"));

  connect(m_txtSearchMessages, &BaseLineEdit::textChanged, this, &FeedsToolBar::feedsFilterPatternChanged);
}
