#include <zlib.h>
#include "MainWindow.h"
#include "Pixels.h"
#include "LayerClipboard.h"
#include "ProjectSaveDialog.h"
#include "NumericScrub.h"
#include "Cmyk.h"
#include "UpdateInstaller.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QCheckBox>
#include <QFormLayout>
#include <QSignalSpy>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QTest>
#include <QProcess>
#include <QtEndian>
#include <lcms2.h>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif
using namespace comp;
namespace {
State fixture() {
    State s; s.id = newId(); s.size = QSize(64, 48); s.bitDepth = 16;
    Layer layer; layer.id = newId(); layer.name = "Precision";
    layer.image = QImage(s.size, pixelFormat(16)); layer.image.fill(QColor::fromRgba64(12001, 23009, 34007, 65535));
    layer.mask = QImage(s.size, QImage::Format_Grayscale16);
    for (int y = 0; y < s.size.height(); ++y)
        for (int x = 0; x < s.size.width(); ++x)
            reinterpret_cast<quint16 *>(layer.mask.scanLine(y))[x] = quint16(10001 + x * 103 + y * 71);
    layer.placement.size = s.size; layer.maskPlacement = layer.placement;
    s.layers.append(layer); s.active = layer.id;
    return s;
}
void write(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) qFatal("Cannot write fixture");
}
QByteArray read(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read fixture");
    return file.readAll();
}
void mouse(QWidget *widget, QEvent::Type type, QPoint point, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {}) {
    QMouseEvent event(type, QPointF(point), QPointF(widget->mapToGlobal(point)), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
}
void drop(QWidget *widget, const QMimeData &mime, QPoint point) {
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QApplication::sendEvent(widget, &enter);
    QDropEvent event(QPointF(point), Qt::CopyAction, &mime, Qt::LeftButton, {});
    QApplication::sendEvent(widget, &event);
}
QByteArray zip(const QVector<QPair<QString, QByteArray>> &files, bool deflate) {
    QByteArray out, central;
    auto u16=[](QByteArray &data, quint16 v){char bytes[2];qToLittleEndian(v,bytes);data.append(bytes,2);};
    auto u32=[](QByteArray &data, quint32 v){char bytes[4];qToLittleEndian(v,bytes);data.append(bytes,4);};
    for(const auto &file:files) {
        const auto name=file.first.toUtf8(), raw=file.second;
        auto bytes=raw;
        if(deflate) {
            bytes.resize(compressBound(raw.size())+64);
            z_stream stream{};deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-MAX_WBITS,8,Z_DEFAULT_STRATEGY);
            stream.next_in=reinterpret_cast<Bytef *>(const_cast<char *>(raw.constData()));stream.avail_in=raw.size();
            stream.next_out=reinterpret_cast<Bytef *>(bytes.data());stream.avail_out=bytes.size();
            ::deflate(&stream,Z_FINISH);bytes.resize(stream.total_out);deflateEnd(&stream);
        }
        const quint32 crc=crc32(0,reinterpret_cast<const Bytef *>(raw.constData()),raw.size()),offset=out.size();
        u32(out,0x04034b50);u16(out,20);u16(out,0x800);u16(out,deflate?8:0);u16(out,0);u16(out,0);
        u32(out,crc);u32(out,bytes.size());u32(out,raw.size());u16(out,name.size());u16(out,0);out+=name;out+=bytes;
        u32(central,0x02014b50);u16(central,20);u16(central,20);u16(central,0x800);u16(central,deflate?8:0);
        u16(central,0);u16(central,0);u32(central,crc);u32(central,bytes.size());u32(central,raw.size());
        u16(central,name.size());u16(central,0);u16(central,0);u16(central,0);u16(central,0);u32(central,0);u32(central,offset);central+=name;
    }
    const auto offset=out.size();out+=central;
    u32(out,0x06054b50);u16(out,0);u16(out,0);u16(out,files.size());u16(out,files.size());
    u32(out,central.size());u32(out,offset);u16(out,0);return out;
}
QString installerJob(QTemporaryDir &temp) {
    const auto target = temp.filePath("portable"), work = temp.filePath(".compositor-update-test");
    const QStringList names{"Compositor.exe", "CompositorUpdater.exe", "plugins/new.dll"};
    QJsonArray records;
    for (const auto &name : names) {
        const auto bytes = "new-" + name.toUtf8();
        write(work + "/payload/" + name, bytes);
        records.append(QJsonObject{{"path", name}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
    }
    write(target + "/Compositor.exe", "old-app");
    write(target + "/CompositorUpdater.exe", "old-helper");
    write(target + "/my-project.comp/notes.txt", "untouched-user-data");
    write(work + "/job.json", QJsonDocument(QJsonObject{{"version",1},{"installation",target},{"files",records}}).toJson());
    return work + "/job.json";
}
}
class InteractionTests final : public QObject {
    Q_OBJECT
  private slots:
    void compatibilityRoundTrip() {
        QTemporaryDir temp; const auto s = fixture();
        const auto path = temp.filePath("compatible.comp");
        const auto hash = writeProject(path, s, {}, true);
        const auto manifest = QJsonDocument::fromJson(read(path + "/manifest.json")).object();
        QCOMPARE(manifest["version"].toInt(), 12);
        QVERIFY(manifest["windowsCompatibility"].toBool());
        const auto mask = read(path + "/images/" + s.active + ".mask.png");
        QCOMPARE(quint8(mask[24]), quint8(8)); QCOMPARE(quint8(mask[25]), quint8(0));
        const auto loaded = readProject(path);
        QCOMPARE(loaded.digest, hash);
        QCOMPARE(loaded.state.layers[0].mask, s.layers[0].mask);
        QCOMPARE(loaded.state.layers[0].image, s.layers[0].image);
        QCOMPARE(loaded.state.bitDepth, 16);
        QCOMPARE(s.layers[0].mask, fixture().layers[0].mask);
        Document doc; doc.reset(loaded.state, true);
        doc.fill(QColor::fromRgba64(17321, 17321, 17321, 65535), false, true);
        QVERIFY(wideImage(doc.state().layers[0].mask));
        doc.undo(); QCOMPARE(doc.state().layers[0].mask, s.layers[0].mask);
        const auto native = temp.filePath("native.comp");
        writeProject(native, loaded.state);
        const auto normal = readProject(native);
        QCOMPARE(normal.state.metadata["version"].toInt(), ProjectManifest::current);
        QVERIFY(!normal.state.metadata.contains("windowsMaskPrecision"));
        QCOMPARE(normal.state.layers[0].mask, s.layers[0].mask);
        QCOMPARE(quint8(read(native + "/images/" + s.active + ".mask.png")[24]), quint8(16));
    }
    void externalMaskEditWins() {
        QTemporaryDir temp; const auto s = fixture(); const auto path = temp.filePath("edited.comp");
        writeProject(path, s, {}, true);
        auto changed = maskImage(s.layers[0].mask, 8); changed.fill(207);
        write(path + "/images/" + s.active + ".mask.png", pngBytes(changed));
        const auto reopened = readProject(path);
        QCOMPARE(reopened.state.bitDepth, 16);
        QCOMPARE(reinterpret_cast<const quint16 *>(reopened.state.layers[0].mask.constScanLine(0))[0], quint16(207 * 257));
        writeProject(path, reopened.state, reopened.digest, true);
        QCOMPARE(readProject(path).state.layers[0].mask, reopened.state.layers[0].mask);
    }
    void missingPreciseMaskIsExplicit() {
        QTemporaryDir temp; const auto s = fixture(); const auto path = temp.filePath("missing.comp");
        writeProject(path, s, {}, true);
        QVERIFY(QFile::remove(path + "/windows/masks/" + s.active + ".png"));
        QVERIFY_EXCEPTION_THROWN(readProject(path), Error);
    }
    void saveDialogCompatibilityOption() {
        QTemporaryDir temp; QWidget owner; Localization language; language.setLanguage("en");
        bool seen = false;
        QTimer::singleShot(0, &owner, [&] {
            auto *dialog = owner.findChild<QFileDialog *>("projectSaveDialog");
            QVERIFY(dialog);
            auto *mode = dialog->findChild<QCheckBox *>("saveCompatibility"); QVERIFY(mode);
            QVERIFY(!mode->isChecked()); mode->setChecked(true); seen = true;
            dialog->selectFile(temp.filePath("chosen.comp"));
            QMetaObject::invokeMethod(dialog, "accept");
        });
        const auto result = chooseProjectSave(&owner, language, temp.filePath("chosen.comp"), false);
        QVERIFY(seen); QVERIFY(result); QVERIFY(result->compatibility);
        QCOMPARE(result->path, temp.filePath("chosen.comp"));
        QTimer::singleShot(0, &owner, [&] {
            auto *dialog = owner.findChild<QFileDialog *>("projectSaveDialog"); QVERIFY(dialog);
            QVERIFY(dialog->findChild<QCheckBox *>("saveCompatibility")->isChecked());
            dialog->reject();
        });
        QVERIFY(!chooseProjectSave(&owner, language, result->path, true));
    }
    void welcomePastePreservesPrecision() {
        MainWindow window;
        auto *mime = new QMimeData; const auto image = fixture().layers[0].image;
        mime->setData("image/png", pngBytes(image));
        QApplication::clipboard()->setMimeData(mime);
        auto *paste = window.findChild<QAction *>("pasteAction"); QVERIFY(paste); QVERIFY(paste->isEnabled());
        paste->trigger();
        QVERIFY(window.document()->hasDocument());
        QCOMPARE(window.document()->state().size, image.size());
        QCOMPARE(window.document()->state().layers[0].image, image);
        QCOMPARE(window.document()->state().bitDepth, 16);
    }
    void clipboardFileWinsOverThumbnail() {
        QTemporaryDir temp; const auto image = fixture().layers[0].image;
        const auto path = temp.filePath("photo.png"); write(path, pngBytes(image));
        auto *mime = new QMimeData; mime->setUrls({QUrl::fromLocalFile(path)});
        QImage icon(16, 16, QImage::Format_ARGB32); icon.fill(Qt::red); mime->setImageData(icon);
        QApplication::clipboard()->setMimeData(mime);
        MainWindow window; window.findChild<QAction *>("pasteAction")->trigger();
        QTRY_VERIFY(window.document()->hasDocument());
        QCOMPARE(window.document()->state().size, image.size());
        QCOMPARE(window.document()->state().layers[0].image, readImage(path));
        QCOMPARE(window.document()->state().bitDepth, 16);
    }
    void dropLayersFitsTransformedBounds() {
        auto s = fixture(); auto &layer = s.layers[0];
        layer.placement.origin = QPointF(-17.25, 13.75); layer.placement.rotation = 37; layer.placement.flipX = true;
        layer.maskLinked = false; layer.maskPlacement.origin = QPointF(8.25, -3.5);
        const auto bounds = layer.placement.matrix(layer.image.size()).mapRect(QRectF(QPointF(), layer.image.size())).toAlignedRect();
        const auto data = encodeLayers(s, {s.active});
        const auto created = layersAsDocument(data);
        QCOMPARE(created.size, bounds.size());
        QCOMPARE(created.layers[0].placement.origin, layer.placement.origin - QPointF(bounds.topLeft()));
        QCOMPARE(created.layers[0].maskPlacement.origin, layer.maskPlacement.origin - QPointF(bounds.topLeft()));
        QCOMPARE(created.layers[0].mask, layer.mask);
        MainWindow window; window.show(); QTest::qWait(10);
        auto *tabs = window.findChild<QTabBar *>("documentTabs"); QVERIFY(tabs);
        QMimeData mime; mime.setData(LayerMimeType, data); drop(tabs, mime, QPoint(15, 12));
        QVERIFY(window.document()->hasDocument());
        QCOMPARE(window.document()->state().size, bounds.size());
        QCOMPARE(window.document()->state().layers[0].placement.rotation, 37.);
    }
    void middleDragDoesNotSelect() {
        MainWindow window; window.sample(); window.show();
        for (int i = 0; i < 30; ++i) window.document()->addLayer("Layer " + QString::number(i), QImage(1, 1, pixelFormat(8)));
        auto *tree = window.findChild<QTreeWidget *>(); QVERIFY(tree);
        QTest::qWait(10); auto *scroll = tree->verticalScrollBar();
        scroll->setValue(scroll->maximum()/2); const auto before = scroll->value();
        const auto selected = window.document()->state().active;
        mouse(tree->viewport(), QEvent::MouseButtonPress, QPoint(70, 80), Qt::MiddleButton, Qt::MiddleButton);
        mouse(tree->viewport(), QEvent::MouseMove, QPoint(70, 30), Qt::NoButton, Qt::MiddleButton);
        mouse(tree->viewport(), QEvent::MouseButtonRelease, QPoint(70, 30), Qt::MiddleButton, {});
        QVERIFY(scroll->value() > before);
        QCOMPARE(window.document()->state().active, selected);
    }
    void numericLabelsScrubAndCancel() {
        QWidget widget; QFormLayout form(&widget); QDoubleSpinBox box;
        box.setRange(-1000, 1000); box.setValue(20);
        form.addRow("Radius", &box); installNumericScrubbing(&widget);
        widget.show(); auto *label = qobject_cast<QLabel *>(form.labelForField(&box)); QVERIFY(label);
        QSignalSpy finished(&box, &QDoubleSpinBox::editingFinished);
        mouse(label, QEvent::MouseButtonPress, QPoint(5,5), Qt::LeftButton, Qt::LeftButton);
        mouse(label, QEvent::MouseMove, QPoint(45,5), Qt::NoButton, Qt::LeftButton);
        mouse(label, QEvent::MouseButtonRelease, QPoint(45,5), Qt::LeftButton, {});
        QCOMPARE(box.value(),30.); QCOMPARE(finished.count(),1);
        mouse(label, QEvent::MouseButtonPress, QPoint(5,5), Qt::LeftButton, Qt::LeftButton);
        mouse(label, QEvent::MouseMove, QPoint(45,5), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        QCOMPARE(box.value(),31.);
        QTest::keyClick(&box, Qt::Key_Escape); QCOMPARE(box.value(),30.);
        box.setReadOnly(true);
        mouse(label, QEvent::MouseButtonPress, QPoint(5,5), Qt::LeftButton, Qt::LeftButton);
        mouse(label, QEvent::MouseMove, QPoint(45,5), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(box.value(),30.);
    }
    void zoomDragCursorReverses() {
        MainWindow window; window.sample(); window.show();
        auto *canvas = window.canvas(); canvas->setTool(Tool::Zoom);
        const QPoint start(250,250); const double initial = canvas->zoom();
        mouse(canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,start+QPoint(25,0),Qt::NoButton,Qt::LeftButton);
        QVERIFY(canvas->zoom()>initial); QVERIFY(!canvas->property("zoomCursorOut").toBool());
        mouse(canvas,QEvent::MouseMove,start+QPoint(10,0),Qt::NoButton,Qt::LeftButton);
        QVERIFY(canvas->property("zoomCursorOut").toBool());
        mouse(canvas,QEvent::MouseButtonRelease,start+QPoint(10,0),Qt::LeftButton,{});
        QVERIFY(!canvas->property("zoomCursorOut").toBool());
    }
    void savingDoesNotOverwriteLaterEditsOrOtherTabs() {
        QTemporaryDir temp;
        auto first=fixture(), second=fixture(); first.layers[0].name="First"; second.layers[0].name="Second";
        const auto path1=temp.filePath("first.comp"), path2=temp.filePath("second.comp");
        writeProject(path1,first); writeProject(path2,second);
        MainWindow window; window.openProject(path1);
        QTRY_COMPARE(window.document()->state().id,first.id);
        window.openProject(path2); QTRY_COMPARE(window.document()->state().id,second.id);
        auto *tabs=window.findChild<QTabBar *>("documentTabs"); tabs->setCurrentIndex(0);
        window.document()->edit([](State &s){s.layers[0].name="Snapshot";});
        auto *save=window.findChild<QAction *>("saveAction"); save->trigger();
        QVERIFY(window.canvas()->isEnabled()); QVERIFY(!save->isEnabled());
        window.document()->edit([](State &s){s.layers[0].name="After save started";});
        tabs->setCurrentIndex(1);
        window.document()->edit([](State &s){s.layers[0].name="Second edited";});
        QTRY_VERIFY(save->isEnabled());
        QCOMPARE(window.document()->state().layers[0].name,QString("Second edited"));
        QVERIFY(window.document()->dirty());
        QCOMPARE(readProject(path1).state.layers[0].name,QString("Snapshot"));
        tabs->setCurrentIndex(0);
        QCOMPARE(window.document()->state().layers[0].name,QString("After save started"));
        QVERIFY(window.document()->dirty());
        window.document()->undo(); QVERIFY(!window.document()->dirty());
    }
    void cmykLibraryRetainsProfiles() {
        auto profile = cmsCreateProfilePlaceholder(nullptr);
        cmsSetProfileVersion(profile,2.1); cmsSetDeviceClass(profile,cmsSigOutputClass);
        cmsSetColorSpace(profile,cmsSigCmykData); cmsSetPCS(profile,cmsSigLabData);
        cmsWriteTag(profile,cmsSigMediaWhitePointTag,cmsD50_XYZ());
        cmsMLU *name=cmsMLUalloc(nullptr,1); cmsMLUsetASCII(name,"en","US","Library test");
        cmsWriteTag(profile,cmsSigProfileDescriptionTag,name); cmsMLUfree(name);
        cmsUInt32Number size=0; cmsSaveProfileToMem(profile,nullptr,&size);
        QByteArray data(size,0); cmsSaveProfileToMem(profile,data.data(),&size); cmsCloseProfile(profile);
        chooseCmykProfile(data); chooseCmykProfile(data);
        const auto entries=installedCmykProfiles();
        int count=0; QString path;
        for (const auto &entry:entries) if(entry.data==data){++count;path=entry.path;QVERIFY(entry.removable);}
        QCOMPARE(count,1); QVERIFY(QFileInfo::exists(path));
        QVERIFY_EXCEPTION_THROWN(removeCmykProfile(path),Error);
        QSettings().remove("print/cmykProfile");
        removeCmykProfile(path); QVERIFY(!QFileInfo::exists(path));
        QVERIFY_EXCEPTION_THROWN(chooseCmykProfile("not an ICC"),Error);
    }
    void archivePreparationAndValidation() {
        for(bool compressed:{false,true}) {
            QTemporaryDir temp;const auto target=temp.filePath("portable");
            for(const auto &name:{"Compositor.exe","CompositorUpdater.exe","Qt6Core.dll","z.dll","vcruntime140.dll","vcruntime140_1.dll","msvcp140.dll"})
                write(target+"/"+name,"runtime");
            auto files=QVector<QPair<QString,QByteArray>>{{"Package/Compositor.exe","new app"},
                {"Package/CompositorUpdater.exe","new helper"},{"Package/Qt6Core.dll","core"},
                {"Package/platforms/qwindows.dll","platform"},{"Package/models/model.onnx",QByteArray(20000,'x')}};
            const auto archive=temp.filePath("update.zip");const auto bytes=zip(files,compressed);write(archive,bytes);
            const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
            const auto job=prepareUpdateArchive(archive,bytes.size(),hash,target);
            validateUpdateJob(job);
            const auto work=QFileInfo(job).absolutePath();
            QCOMPARE(read(work+"/payload/models/model.onnx"),QByteArray(20000,'x'));
            QCOMPARE(read(work+"/control/CompositorUpdater.exe"),QByteArray("runtime"));
            discardPreparedUpdate(job);QVERIFY(!QFileInfo::exists(work));
            QVERIFY_EXCEPTION_THROWN(prepareUpdateArchive(archive,bytes.size(),QByteArray(64,'0'),target),std::exception);
            files.append(QPair<QString,QByteArray>{"Package/../escape.exe","bad"});const auto bad=zip(files,compressed);write(archive,bad);
            QVERIFY_EXCEPTION_THROWN(prepareUpdateArchive(archive,bad.size(),QCryptographicHash::hash(bad,QCryptographicHash::Sha256).toHex(),target),std::exception);
            QVERIFY(!QFileInfo::exists(temp.filePath("escape.exe")));
        }
    }
    void detachedInstallerWaitsAndRestarts() {
#ifdef Q_OS_WIN
        QTemporaryDir temp;const auto target=temp.filePath("portable"),work=temp.filePath(".compositor-update-detached");
        const auto exe=QCoreApplication::applicationFilePath(), helper=QCoreApplication::applicationDirPath()+"/CompositorUpdater.exe";
        QVERIFY(QFileInfo::exists(helper));
        write(target+"/Compositor.exe",read(exe));
        write(target+"/CompositorUpdater.exe",read(helper));
        write(work+"/control/CompositorUpdater.exe",read(helper));
        QVector<QPair<QString,QByteArray>> files{{"Compositor.exe",read(exe)},{"CompositorUpdater.exe",read(helper)},{"version.txt","updated"}};
        QJsonArray records;
        for(const auto &file:files) {
            write(work+"/payload/"+file.first,file.second);
            records.append(QJsonObject{{"path",file.first},{"sha256",QString::fromLatin1(QCryptographicHash::hash(file.second,QCryptographicHash::Sha256).toHex())}});
        }
        write(work+"/job.json",QJsonDocument(QJsonObject{{"version",1},{"installation",target},{"files",records}}).toJson());
        const auto marker=temp.filePath("restarted.txt");
        QProcess parent;
        auto environment=QProcessEnvironment::systemEnvironment();
        environment.insert("PATH",QCoreApplication::applicationDirPath()+";"+environment.value("PATH"));
        parent.setProcessEnvironment(environment);
        parent.start(target+"/Compositor.exe",{"--installer-parent-test",work+"/job.json",marker});
        QVERIFY(parent.waitForStarted());
        QVERIFY(parent.waitForFinished(20000));QCOMPARE(parent.exitCode(),0);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker),15000);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(work+"/result.txt"),15000);
        QCOMPARE(read(target+"/version.txt"),QByteArray("updated"));
        QCOMPARE(read(work+"/backup/Compositor.exe"),read(exe));
#else
        QSKIP("Windows updater");
#endif
    }
    void installAndRollbackPreservesUnrelatedFiles() {
        QTemporaryDir temp; const auto job=installerJob(temp); QString error;
        validateUpdateJob(job);
        QVERIFY2(applyUpdateJob(job,error),qPrintable(error));
        QCOMPARE(read(temp.filePath("portable/Compositor.exe")),QByteArray("new-Compositor.exe"));
        QCOMPARE(read(temp.filePath("portable/my-project.comp/notes.txt")),QByteArray("untouched-user-data"));
        QVERIFY2(rollbackUpdateJob(job,error),qPrintable(error));
        QCOMPARE(read(temp.filePath("portable/Compositor.exe")),QByteArray("old-app"));
        QVERIFY(!QFileInfo::exists(temp.filePath("portable/plugins/new.dll")));
    }
    void installRefusesTamperingAndPathEscape() {
        QTemporaryDir temp; const auto job=installerJob(temp);
        write(temp.filePath(".compositor-update-test/payload/Compositor.exe"),"corrupted");
        QString error; QVERIFY(!applyUpdateJob(job,error));
        QCOMPARE(read(temp.filePath("portable/Compositor.exe")),QByteArray("old-app"));
        auto object=readUpdateJob(job); auto entries=object["files"].toArray();
        auto record=entries[0].toObject(); record["path"]="../outside.exe"; entries[0]=record; object["files"]=entries;
        write(job,QJsonDocument(object).toJson());
        QVERIFY_EXCEPTION_THROWN(validateUpdateJob(job),std::exception);
        QVERIFY(!QFileInfo::exists(temp.filePath("outside.exe")));
    }
    void lockedFileRollsBack() {
#ifdef Q_OS_WIN
        QTemporaryDir temp; const auto job=installerJob(temp);
        const auto path=QDir::toNativeSeparators(temp.filePath("portable/CompositorUpdater.exe"));
        HANDLE lock=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(lock!=INVALID_HANDLE_VALUE);
        QString error; const bool installed=applyUpdateJob(job,error); CloseHandle(lock);
        QVERIFY(!installed);
        QCOMPARE(read(temp.filePath("portable/Compositor.exe")),QByteArray("old-app"));
        QCOMPARE(read(temp.filePath("portable/CompositorUpdater.exe")),QByteArray("old-helper"));
#else
        QSKIP("Windows sharing modes");
#endif
    }
};
int main(int argc,char **argv) {
    if(argc==4 && QByteArray(argv[1])=="--prepare-update-test") {
        QCoreApplication app(argc,argv);
        try {
            const auto archive=app.arguments()[2],target=app.arguments()[3];
            const auto bytes=read(archive);
            const auto job=prepareUpdateArchive(archive,bytes.size(),QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex(),target);
            validateUpdateJob(job);
            discardPreparedUpdate(job);
            return 0;
        } catch(const std::exception &e) { qWarning("%s",e.what());return 1; }
    }
#ifdef Q_OS_WIN
    if(argc==4 && QByteArray(argv[1])=="--installer-parent-test") {
        QCoreApplication app(argc,argv);
        const auto job=app.arguments()[2],marker=app.arguments()[3];
        qputenv("COMPOSITOR_RELAUNCH_TEST_MARKER",marker.toUtf8());
        const auto name="Local\\Compositor-Test-"+QUuid::createUuid().toString(QUuid::WithoutBraces),cancelName=name+"-cancel";
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,reinterpret_cast<LPCWSTR>(name.utf16()));
        HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,reinterpret_cast<LPCWSTR>(cancelName.utf16()));
        const auto control=QFileInfo(job).absolutePath()+"/control";
        bool started=QProcess::startDetached(control+"/CompositorUpdater.exe",
            {"--apply-update",job,QString::number(app.applicationPid()),name},control);
        bool armed=started && WaitForSingleObject(ready,10000)==WAIT_OBJECT_0;
        if(!armed)SetEvent(cancel);
        CloseHandle(ready);CloseHandle(cancel);return armed?0:3;
    }
    if(qEnvironmentVariableIsSet("COMPOSITOR_RELAUNCH_TEST_MARKER")) {
        QCoreApplication app(argc,argv);
        write(qEnvironmentVariable("COMPOSITOR_RELAUNCH_TEST_MARKER"),"restarted");
        return 0;
    }
#endif
    qputenv("COMPOSITOR_HEADLESS_TEST","1"); qputenv("COMPOSITOR_DISABLE_GPU","1");
    Q_INIT_RESOURCE(translations); Q_INIT_RESOURCE(icons); Q_INIT_RESOURCE(controls);
    QApplication app(argc,argv); app.setStyle("Fusion");
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("Interactions-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QStandardPaths::setTestModeEnabled(true);
    const auto data=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    InteractionTests tests; const int result=QTest::qExec(&tests,argc,argv);
    QDir(data).removeRecursively();
    return result;
}
#include "InteractionTests.moc"
