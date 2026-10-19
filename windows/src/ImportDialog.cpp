#include "MainWindow.h"
#include "Importers.h"
#include "RawAutoLens.h"
#include "Cmyk.h"
#include <QtConcurrent>
#include <QFileInfo>
#include <QFileDialog>
#include <QFile>
#include <QCheckBox>
#include <QScrollArea>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMessageBox>
namespace comp {
void MainWindow::insertImported(const QString &name, const QImage &image, bool openAsDocument) {
    if (image.isNull()) throw Error("imageError");
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
                      if (result->state.metadata.contains("windowsCMYKProfile"))
                          chooseCmykProfile(QByteArray::fromBase64(result->state.metadata["windowsCMYKProfile"].toString().toLatin1()));
                      adoptDocument(result->state, QFileInfo(path).completeBaseName());
                      remember(path);
                  }
                  importNext(paths, openAsDocument);
              });
        return;
    }
    if (QFileInfo(path).isDir() && suffix == "comp") {
        auto loaded = std::make_shared<LoadedProject>();
        runIO([loaded, path] { *loaded = readProject(path); }, [this, loaded, path, paths, openAsDocument] {
            adoptDocument(loaded->state, QFileInfo(path).completeBaseName(), path, loaded->digest, true);
            remember(path);
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
    auto automatic = std::make_shared<RawLensCorrection>();
    runIO([frame, automatic, path] { *frame = decodeRaw(path); *automatic = automaticRawLens(*frame); },
          [this, frame, automatic, path, remaining, openAsDocument] {
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
              camera.setTextFormat(Qt::PlainText);
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
              QDoubleSpinBox noise; noise.setRange(0,100); noise.setObjectName("rawDecodeNoise");
              QCheckBox recovery(t("rawHighlightRecovery")), lens(t("rawLensCorrection"));
              recovery.setObjectName("rawHighlightRecovery"); lens.setObjectName("rawLensCorrection");
              RawLensCorrection correction = *automatic;
              lens.setEnabled(correction.available()); lens.setChecked(correction.available());
              QPushButton loadLens(t("rawLoadLensProfile")), autoLens(t("rawUseAutomaticLens"));
              loadLens.setObjectName("rawLoadLensProfile"); autoLens.setObjectName("rawUseAutomaticLens");
              form->addRow(t("rawDecodeNoise"), &noise); form->addRow(&recovery); form->addRow(&lens);
              QLabel lensNote; lensNote.setTextFormat(Qt::PlainText); lensNote.setObjectName("rawLensStatus"); lensNote.setWordWrap(true); form->addRow(&lensNote);
              form->addRow(&loadLens); form->addRow(&autoLens);
              QLabel lensHint(t("rawLensProfileHint")); lensHint.setWordWrap(true); form->addRow(&lensHint);
              auto showLensStatus = [&] {
                  QString message = t(correction.status);
                  if (correction.available()) {
                      QStringList effects;
                      if (correction.distortion) effects.append(t("rawLensDistortion"));
                      if (correction.vignetting) effects.append(t("rawLensVignetting"));
                      message = message.arg(correction.profile, effects.join(", "));
                  }
                  lensNote.setText(message);
              };
              showLensStatus();
              QImage correctedPreview;
              bool openCameraRaw=false;
              QLabel note(t("rawDevelopNote"));
              note.setWordWrap(true);
              form->addRow(&note);
              auto *controls=new QWidget;controls->setLayout(form);
              auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setMinimumWidth(300);scroll->setWidget(controls);body->addWidget(scroll);
              outer->addLayout(body);
              auto *buttons =
                  new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
              auto *rawButton=buttons->addButton(t("cameraRaw"),QDialogButtonBox::ActionRole);
              connect(rawButton,&QPushButton::clicked,&dialog,[&]{openCameraRaw=true;dialog.accept();});
              outer->addWidget(buttons);
              connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
              connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
              auto update = [&] {
                  auto developed = *frame;
                  if (lens.isChecked()) {
                      if (correctedPreview.isNull()) correctedPreview = applyRawLens(*frame, correction, 700);
                      developed.linear = correctedPreview;
                  }
                  preview.setPixmap(QPixmap::fromImage(developRaw(developed,exposure.value(),temperature.value(),tint.value(),boost.value(),700))
                                      .scaled(preview.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
              };
              QFutureWatcher<RawFrame> decodeWatcher;QTimer decodeTimer;decodeTimer.setSingleShot(true);decodeTimer.setInterval(180);
              bool pending=false,closing=false;
              auto decodeStart=[&] {
                  if(closing)return;if(decodeWatcher.isRunning()){pending=true;return;}
                  pending=false;buttons->button(QDialogButtonBox::Ok)->setEnabled(false);rawButton->setEnabled(false);
                  decodeWatcher.setFuture(QtConcurrent::run([bytes=frame->encoded,n=noise.value(),h=recovery.isChecked()]{return decodeRawBytes(bytes,n,h);}));
              };
              connect(&decodeTimer,&QTimer::timeout,&dialog,decodeStart);
              connect(&decodeWatcher,&QFutureWatcher<RawFrame>::finished,&dialog,[&]{
                  if(closing)return;
                  try{*frame=decodeWatcher.result();correctedPreview={};update();buttons->button(QDialogButtonBox::Ok)->setEnabled(true);rawButton->setEnabled(true);}
                  catch(const Error &e){showError(e.key,e.detail);}
                  if(pending){decodeTimer.stop();decodeStart();}
              });
              auto requestDecode=[&]{pending=true;buttons->button(QDialogButtonBox::Ok)->setEnabled(false);rawButton->setEnabled(false);decodeTimer.start();};
              connect(&noise,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,requestDecode);
              connect(&recovery,&QCheckBox::toggled,&dialog,requestDecode);
              connect(&lens,&QCheckBox::toggled,&dialog,[&]{invoke(update);});
              connect(&loadLens,&QPushButton::clicked,&dialog,[&]{
                  const auto path=QFileDialog::getOpenFileName(&dialog,t("rawLoadLensProfile"),{},"Lensfun XML (*.xml)");if(path.isEmpty())return;
                  invoke([&]{QFile file(path);if(!file.open(QIODevice::ReadOnly))throw Error("fileError");if(file.size()>4*1024*1024)throw Error("tooLarge");
                      const auto selected = manualRawLens(*frame, file.readAll());
                      correction=selected; correctedPreview={}; showLensStatus();
                      lens.setEnabled(true); lens.setChecked(true); update();});
              });
              connect(&autoLens,&QPushButton::clicked,&dialog,[&] {
                  correction=*automatic; correctedPreview={}; showLensStatus();
                  lens.setEnabled(correction.available()); lens.setChecked(correction.available()); invoke(update);
              });
              for (auto *box : {&exposure, &temperature, &tint, &boost})
                  connect(box, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&] { update(); });
              connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, &dialog, [&] {
                  exposure.setValue(0);
                  temperature.setValue(6500);
                  tint.setValue(0);
                  boost.setValue(1);noise.setValue(0);recovery.setChecked(false);autoLens.click();
              });
              update();
              const bool accepted=dialog.exec()==QDialog::Accepted;
              closing=true;decodeTimer.stop();decodeWatcher.waitForFinished();
              if (!accepted) {
                  importNext(remaining, openAsDocument);
                  return;
              }
              double e = exposure.value(), temp = temperature.value(), ti = tint.value(), b = boost.value();
              const auto selectedCorrection = lens.isChecked() ? correction : RawLensCorrection{};
              auto image = std::make_shared<QImage>();
              runIO([frame, image, e, temp, ti, b, selectedCorrection] {
                        auto developed = *frame;
                        developed.linear = applyRawLens(*frame, selectedCorrection);
                        *image = developRaw(developed, e, temp, ti, b);
                    },
                    [this, image, path, remaining, openAsDocument, openCameraRaw] {
                        invoke([&] {
                            insertImported(QFileInfo(path).completeBaseName(), *image, openAsDocument);
                            if (openAsDocument) remember(path);
                        });
                        if (openCameraRaw) {
                            cameraRawDialog();
                            if (busy_) {
                                auto applied = std::move(completion_);
                                completion_ = [this, applied = std::move(applied), remaining, openAsDocument] {
                                    if (applied) applied();
                                    importNext(remaining, openAsDocument);
                                };
                                return;
                            }
                        }
                        importNext(remaining, openAsDocument);
                    });
          });
}
} // namespace comp
