#include "MainWindow.h"
#include "Importers.h"
#include <QtConcurrent>
#include <QFileInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
namespace comp {
void MainWindow::insertImported(const QString &name, const QImage &image, bool openAsDocument) {
    if (openAsDocument || !document_.hasDocument()) {
        State s;
        s.id = newId();
        s.size = image.size();
        if (s.size.width() > MaxSide || s.size.height() > MaxSide)
            s.size.scale(MaxSide, MaxSide, Qt::KeepAspectRatio);
        while (qint64(s.size.width()) * s.size.height() > MaxCanvasPixels)
            s.size *= .9;
        adoptDocument(s, openAsDocument ? name : t("untitled"));
    }
    document_.addLayer(name, image);
    canvas_->fit();
}
void MainWindow::importNext(QStringList paths, bool openAsDocument) {
    if (paths.isEmpty() || busy_)
        return;
    const QString path = paths.takeFirst(), suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "psd" || suffix == "psb") {
        auto result = std::make_shared<ImportResult>();
        runIO([result, path] { *result = readPhotoshop(path); },
              [this, result, path, paths, openAsDocument] {
                  bool accepted = true;
                  if (!result->notices.isEmpty()) {
                      QStringList details;
                      for (auto note : result->notices) {
                          int sep = note.lastIndexOf(": ");
                          details.append(sep < 0 ? t(note) : note.left(sep + 2) + t(note.mid(sep + 2)));
                      }
                      QMessageBox message(QMessageBox::Information, t("psdReport"), t("psdReportBody"),
                                          QMessageBox::Ok | QMessageBox::Cancel, this);
                      message.setDetailedText(details.join('\n'));
                      accepted = message.exec() == QMessageBox::Ok;
                  }
                  if (accepted) {
                      adoptDocument(result->state, QFileInfo(path).completeBaseName());
                      remember(path);
                  }
                  importNext(paths, openAsDocument);
              });
        return;
    }
    if (isRawPath(path)) {
        importRaw(path, paths, openAsDocument);
        return;
    }
    auto image = std::make_shared<QImage>();
    runIO([image, path] { *image = readImage(path); },
          [this, image, path, paths, openAsDocument] {
              invoke([&] {
                  insertImported(QFileInfo(path).completeBaseName(), *image, openAsDocument);
                  if (openAsDocument) remember(path);
              });
              importNext(paths, openAsDocument);
          });
}
void MainWindow::importPhotoshop(const QString &path) {
    importNext({path});
}
void MainWindow::importRaw(const QString &path, QStringList remaining, bool openAsDocument) {
    auto frame = std::make_shared<RawFrame>();
    runIO([frame, path] { *frame = decodeRaw(path); },
          [this, frame, path, remaining, openAsDocument] {
              QDialog dialog(this);
              dialog.setObjectName("rawDevelopDialog");
              dialog.setWindowTitle(t("rawDevelop"));
              dialog.resize(950, 680);
              auto *outer = new QVBoxLayout(&dialog);
              auto *body = new QHBoxLayout;
              QLabel preview;
              preview.setMinimumSize(560, 420);
              preview.setAlignment(Qt::AlignCenter);
              body->addWidget(&preview, 1);
              auto *form = new QFormLayout;
              QLabel camera(frame->camera);
              form->addRow(&camera);
              QDoubleSpinBox exposure, temperature, tint, boost;
              exposure.setRange(-10, 10);
              temperature.setRange(2000, 50000);
              temperature.setValue(6500);
              tint.setRange(-100, 100);
              boost.setRange(0, 2);
              boost.setValue(1);
              for (auto *box : {&exposure, &temperature, &tint, &boost})
                  box->setKeyboardTracking(false);
              form->addRow(t("exposure"), &exposure);
              form->addRow(t("rawTemperature"), &temperature);
              form->addRow(t("tint"), &tint);
              form->addRow(t("rawBoost"), &boost);
              QLabel note(t("rawDevelopNote"));
              note.setWordWrap(true);
              form->addRow(&note);
              body->addLayout(form);
              outer->addLayout(body);
              auto *buttons =
                  new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
              outer->addWidget(buttons);
              connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
              connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
              auto update = [&] {
                  preview.setPixmap(QPixmap::fromImage(developRaw(*frame, exposure.value(), temperature.value(),
                                                                  tint.value(), boost.value(), 700))
                                        .scaled(preview.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
              };
              for (auto *box : {&exposure, &temperature, &tint, &boost})
                  connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&] { update(); });
              connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, &dialog, [&] {
                  exposure.setValue(0);
                  temperature.setValue(6500);
                  tint.setValue(0);
                  boost.setValue(1);
              });
              update();
              if (dialog.exec() != QDialog::Accepted) {
                  importNext(remaining, openAsDocument);
                  return;
              }
              double e = exposure.value(), temp = temperature.value(), ti = tint.value(), b = boost.value();
              auto image = std::make_shared<QImage>();
              runIO([frame, image, e, temp, ti, b] { *image = developRaw(*frame, e, temp, ti, b); },
                    [this, image, path, remaining, openAsDocument] {
                        invoke([&] {
                            insertImported(QFileInfo(path).completeBaseName(), *image, openAsDocument);
                            if (openAsDocument) remember(path);
                        });
                        importNext(remaining, openAsDocument);
                    });
          });
}
} // namespace comp
