#include "MainWindow.h"
#include "Cmyk.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QListWidget>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
namespace comp {
void MainWindow::cmykProfilesDialog() {
    QDialog dialog(this);
    dialog.setObjectName("cmykProfilesDialog");
    dialog.setWindowTitle(t("cmykProfiles"));
    dialog.resize(600, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(t("cmykProfilesHint"));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *list = new QListWidget;
    list->setObjectName("cmykProfilesList");
    layout->addWidget(list, 1);
    auto *row = new QHBoxLayout;
    auto *load = new QPushButton(t("loadCmykProfile"));
    auto *remove = new QPushButton(t("removeCmykProfile"));
    row->addWidget(load); row->addWidget(remove); row->addStretch();
    layout->addLayout(row);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Apply)->setText(t("apply"));
    buttons->button(QDialogButtonBox::Close)->setText(t("close"));
    layout->addWidget(buttons);
    QVector<CmykProfileEntry> profiles;
    auto selection = [&] {
        const int index = list->currentRow();
        const bool valid = index >= 0 && index < profiles.size();
        remove->setEnabled(valid && profiles[index].removable && profiles[index].data != chosenCmykProfile());
        buttons->button(QDialogButtonBox::Apply)->setEnabled(valid);
    };
    auto refresh = [&] {
        profiles = installedCmykProfiles();
        list->clear();
        for (const auto &profile : profiles) {
            const bool chosen = profile.data == chosenCmykProfile();
            auto *item = new QListWidgetItem(profile.name + (chosen ? " — " + t("cmykCurrent") : ""), list);
            item->setToolTip(profile.path);
            if (chosen) list->setCurrentItem(item);
        }
        selection();
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, selection);
    connect(load, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, t("loadCmykProfile"), {}, "ICC (*.icc *.icm)");
        if (path.isEmpty()) return;
        try {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) throw Error("fileError");
            chooseCmykProfile(file.readAll());
            canvas_->setCmykProof(actions_["cmykProof"]->isChecked(), actions_["cmykGamut"]->isChecked());
            refresh();
        } catch (const Error &e) { showError(e.key, e.detail); }
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentRow();
        if (index < 0 || index >= profiles.size()) return;
        if (QMessageBox::question(&dialog, t("removeCmykProfile"),
                t("removeCmykProfileConfirm").arg(profiles[index].name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        try { removeCmykProfile(profiles[index].path); refresh(); }
        catch (const Error &e) { showError(e.key, e.detail); }
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentRow();
        if (index < 0 || index >= profiles.size()) return;
        try {
            chooseCmykProfile(profiles[index].data);
            canvas_->setCmykProof(actions_["cmykProof"]->isChecked(), actions_["cmykGamut"]->isChecked());
            refresh();
        } catch (const Error &e) { showError(e.key, e.detail); }
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    refresh();
    dialog.exec();
}
}
