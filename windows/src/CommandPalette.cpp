#include "MainWindow.h"
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>
#include <QTimer>
#include <algorithm>
namespace comp {
void MainWindow::commandPalette() {
    QDialog dialog(this);
    dialog.setObjectName("commandPalette");
    dialog.setWindowTitle(t("searchCommands"));
    dialog.resize(580, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *search = new QLineEdit;
    search->setObjectName("commandSearch");
    search->setPlaceholderText(t("searchCommands"));
    auto *list = new QListWidget;
    list->setObjectName("commandResults");
    layout->addWidget(search);
    layout->addWidget(list);
    struct Command {
        QString text, search;
        QAction *action = nullptr;
        std::function<void()> run;
    };
    QVector<Command> commands;
    for (auto it = actions_.cbegin(); it != actions_.cend(); ++it) {
        auto *action = it.value();
        if (!action->isEnabled() || it.key() == "searchCommands")
            continue;
        QString text = action->text().remove('&');
        if (action->isCheckable() && action->isChecked())
            text.prepend(QString::fromUtf8("✓ "));
        if (!action->shortcut().isEmpty())
            text += "     " + action->shortcut().toString(QKeySequence::NativeText);
        commands.append({text, text + " " + it.key(), action, {}});
    }
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        auto *action = it.value();
        commands.append({action->text(), action->text() + " " + ToolKeys[it.key()], action, {}});
    }
    for (auto [box, tool] : {std::pair{retouchMode_, Tool::Blur}, std::pair{lassoMode_, Tool::Lasso},
                             std::pair{wandMode_, Tool::Wand}, std::pair{shapeMode_, Tool::Rectangle}})
        if (box)
            for (int i = 0; i < box->count(); ++i) {
                auto title = t(ToolKeys[int(tool)]) + " · " + box->itemText(i);
                commands.append({title, title, nullptr, [this, box, tool, i] {
                                     setTool(tool);
                                     box->setCurrentIndex(i);
                                 }});
            }
    auto update = [&] {
        list->clear();
        auto words = search->text().simplified().split(' ', Qt::SkipEmptyParts);
        QVector<std::pair<int, int>> matches;
        for (int i = 0; i < commands.size(); ++i) {
            int score = 0;
            bool match = true;
            for (const auto &word : words) {
                const int at = commands[i].search.indexOf(word, 0, Qt::CaseInsensitive);
                if (at < 0) {
                    match = false;
                    break;
                }
                score += at == 0 || commands[i].search[at - 1].isSpace() ? 100 : 10;
            }
            if (match)
                matches.append({score, i});
        }
        std::stable_sort(matches.begin(), matches.end(), [&](const auto &a, const auto &b) {
            return a.first != b.first ? a.first > b.first
                                      : commands[a.second].text.localeAwareCompare(commands[b.second].text) < 0;
        });
        for (const auto &[score, i] : matches) {
            auto *item = new QListWidgetItem(commands[i].text, list);
            item->setData(Qt::UserRole, i);
            item->setSizeHint(QSize(0, 34));
        }
        if (list->count())
            list->setCurrentRow(0);
    };
    int selected = -1;
    auto activate = [&] {
        if (auto *item = list->currentItem()) {
            selected = item->data(Qt::UserRole).toInt();
            dialog.accept();
        }
    };
    connect(search, &QLineEdit::textChanged, &dialog, update);
    connect(search, &QLineEdit::returnPressed, &dialog, activate);
    connect(list, &QListWidget::itemActivated, &dialog, activate);
    auto *down = new QAction(&dialog);
    down->setShortcut(Qt::Key_Down);
    dialog.addAction(down);
    auto *up = new QAction(&dialog);
    up->setShortcut(Qt::Key_Up);
    dialog.addAction(up);
    connect(down, &QAction::triggered, &dialog,
            [&] { list->setCurrentRow(std::min(list->count() - 1, list->currentRow() + 1)); });
    connect(up, &QAction::triggered, &dialog, [&] { list->setCurrentRow(std::max(0, list->currentRow() - 1)); });
    update();
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted || selected < 0)
        return;
    const auto command = commands[selected];
    if (command.action)
        command.action->trigger();
    else if (command.run)
        command.run();
}
} // namespace comp
