#include "RawAutoLens.h"
#include "MainWindow.h"
#include "Pixels.h"
#include "Updates.h"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSettings>
#include <QTest>
#include <QtEndian>
#include <bit>
using namespace comp;
namespace {
QByteArray database(bool ambiguous = false) {
    QByteArray lens = R"(<lens><maker>Acme</maker><model>Prime 50mm f/2</model><mount>Acme X</mount><cropfactor>1</cropfactor><calibration><distortion model="poly3" focal="50" k1="-0.05"/><vignetting model="pa" focal="50" aperture="2" distance="1000" k1="-0.2" k2="0" k3="0"/></calibration></lens>)";
    return R"(<lensdatabase version="1"><mount><name>Acme X</name></mount><camera><maker>Acme</maker><model>Test Camera</model><mount>Acme X</mount><cropfactor>1</cropfactor></camera>)" + lens +
        (ambiguous ? QByteArray(lens).replace("<maker>Acme</maker>","<maker>Other</maker>") : QByteArray{}) + "</lensdatabase>";
}
RawFrame frame() {
    RawFrame f; f.make="Acme"; f.model="Test Camera"; f.lens="Prime 50mm f/2"; f.focal=50; f.aperture=2;
    f.linear=QImage(65,49,QImage::Format_RGBA64);
    for(int y=0;y<f.linear.height();++y)for(int x=0;x<f.linear.width();++x)
        reinterpret_cast<QRgba64 *>(f.linear.scanLine(y))[x]=QRgba64::fromRgba64(10001+x*71,12003+y*89,14009,65535);
    return f;
}
void be32(QByteArray &b,quint32 v) { char p[4];qToBigEndian(v,p);b.append(p,4); }
void be64(QByteArray &b,double v) { char p[8];qToBigEndian(std::bit_cast<quint64>(v),p);b.append(p,8); }
QByteArray operation(int kind,std::initializer_list<double> coefficients,double cx=.5,double cy=.5,int flags=0,int planes=1) {
    QByteArray p;if(kind==1 || kind==2)be32(p,planes);
    for(int plane=0;plane<(kind==3?1:planes);++plane)for(double v:coefficients)be64(p,v);
    be64(p,cx);be64(p,cy);
    QByteArray result;be32(result,kind);be32(result,0x01030000);be32(result,flags);be32(result,p.size());return result+p;
}
QByteArray opcodes(std::initializer_list<QByteArray> ops) {
    QByteArray b;be32(b,ops.size());for(const auto &op:ops)b+=op;return b;
}
QByteArray le16(quint16 value){QByteArray b(2,0);qToLittleEndian(value,b.data());return b;}
QByteArray le32(quint32 value){QByteArray b(4,0);qToLittleEndian(value,b.data());return b;}
struct Tag {quint16 id,type;quint32 count;QByteArray data;};
quint32 appendIfd(QByteArray &file,QVector<Tag> tags) {
    std::sort(tags.begin(),tags.end(),[](const auto&a,const auto&b){return a.id<b.id;});
    const quint32 start=file.size(),base=start+2+12*tags.size()+4;
    QByteArray records=le16(tags.size()),extra;
    for(const auto &t:tags){records+=le16(t.id)+le16(t.type)+le32(t.count);
        if(t.data.size()<=4){auto bytes=t.data;bytes.resize(4);records+=bytes;}
        else{records+=le32(base+extra.size());extra+=t.data;if(extra.size()%2)extra.append('\0');}}
    records+=le32(0);file+=records+extra;return start;
}
QByteArray dng(const QByteArray &ops,int orientation=1,bool matchingCamera=false,QRect crop={}) {
    QFile source(QString(COMPOSITOR_FIXTURES)+"/linear.dng");if(!source.open(QIODevice::ReadOnly))qFatal("Missing DNG fixture");
    QByteArray file=source.readAll();const quint32 old=qFromLittleEndian<quint32>(file.constData()+4);
    const auto count=qFromLittleEndian<quint16>(file.constData()+old);QVector<Tag> tags;
    for(int i=0;i<count;++i){const auto p=file.constData()+old+2+i*12;const auto id=qFromLittleEndian<quint16>(p);
        if(id==274 || id==34665 || id==51022 || (matchingCamera && (id==271 || id==272)))continue;
        // Keep original offsets, including the unmodified pixel strip.
        tags.append({id,qFromLittleEndian<quint16>(p+2),qFromLittleEndian<quint32>(p+4),QByteArray(p+8,4)});}
    auto ascii=[](quint16 id,QByteArray text){text.append('\0');return Tag{id,2,quint32(text.size()),text};};
    const auto exif=appendIfd(file,{ascii(42035,"Canon"),ascii(42036,"Canon EF 50mm f/1.8 II"),
        {37386,5,1,le32(50)+le32(1)},{33437,5,1,le32(28)+le32(10)}});
    if(matchingCamera){tags.append(ascii(271,"Canon"));tags.append(ascii(272,"Canon EOS 5D Mark II"));}
    if(!crop.isEmpty()){tags.append({50719,4,2,le32(crop.x())+le32(crop.y())});tags.append({50720,4,2,le32(crop.width())+le32(crop.height())});}
    tags.append({34665,4,1,le32(exif)});tags.append({274,3,1,le16(orientation)});
    if(!ops.isEmpty())tags.append({51022,7,quint32(ops.size()),ops});
    const auto root=appendIfd(file,tags);qToLittleEndian(root,file.data()+4);return file;
}
QJsonObject release(const QString &version,bool pre=false) {
    const auto tag="windows-v"+version,file="Compositor-"+version+"-win-x64.zip";
    return {{"tag_name",tag},{"prerelease",pre},{"assets",QJsonArray{QJsonObject{{"name",file},{"size",100},
        {"digest","sha256:"+QString(64,'a')},{"browser_download_url","https://github.com/Hoshino-Rubi/Compositor-Windows/releases/download/"+tag+"/"+file}}}}};
}
}
class RawLensTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){QStandardPaths::setTestModeEnabled(true);QCoreApplication::setOrganizationName("CompositorTests");QCoreApplication::setApplicationName("RawLens053p1");QSettings().clear();}
    void bundledDatabaseAndAliases(){auto f=frame();f.make="Canon";f.model="Canon EOS 5D Mark II";f.lens="Canon EF 50mm f/1.8 II";f.lensMake="Canon";f.aperture=2.8;
        const auto c=automaticRawLens(f);QVERIFY2(c.available(),qPrintable(c.status));QVERIFY(c.distortion);QVERIFY(c.vignetting);QVERIFY(!c.embedded);}
    void metadataMatchingAndNoWrongLens(){auto f=frame();auto c=automaticRawLens(f,database());QVERIFY(c.available());QVERIFY(c.distortion);QVERIFY(c.vignetting);
        f.lens="Prime 50mm f/1.4";QVERIFY(!automaticRawLens(f,database()).available());
        f=frame();f.make="Other";QVERIFY(!automaticRawLens(f,database()).available());
        f=frame();f.focal=85;QVERIFY(!automaticRawLens(f,database()).available());
        f=frame();f.lens={};QVERIFY(!automaticRawLens(f,database()).available());
        f=frame();f.lens="Prime 50mm f/14";QVERIFY(!automaticRawLens(f,database().replace("50mm f/2","50mm f/1.4")).available());}
    void ambiguousMatchDoesNotGuess(){auto c=automaticRawLens(frame(),database(true));QVERIFY(!c.available());QCOMPARE(c.status,QString("rawLensAmbiguous"));}
    void missingApertureDoesNotInventVignetting(){auto f=frame();f.aperture=0;auto c=automaticRawLens(f,database());QVERIFY(c.available());QVERIFY(c.distortion);QVERIFY(!c.vignetting);}
    void lensfunAppliesBothWithoutMutatingSource(){const auto f=frame();const auto original=f.linear.copy();const auto c=automaticRawLens(f,database());const auto out=applyRawLens(f,c);
        QCOMPARE(out.size(),f.linear.size());QVERIFY(wideImage(out));QVERIFY(out!=f.linear);QCOMPARE(f.linear,original);QCOMPARE(applyRawLens(f,c),out);
        QVERIFY(pixel64(out,32,0).blue()>pixel64(f.linear,32,0).blue());}
    void manualProfileAndMalformedData(){auto c=manualRawLens(frame(),database());QVERIFY(c.available());QCOMPARE(c.status,QString("rawLensManual"));
        QVERIFY_EXCEPTION_THROWN(manualRawLens(frame(),"<invalid>"),Error);QVERIFY_EXCEPTION_THROWN(manualRawLens(frame(),database().replace("k1=\"-0.05\"","k1=\"nan\"")),Error);QVERIFY_EXCEPTION_THROWN(manualRawLens(frame(),database(true)),Error);}
    void embeddedIdentityAndPriority(){auto f=frame();f.dngOpcodes=opcodes({operation(1,{1,0,0,0,0,0})});auto c=automaticRawLens(f,database());QVERIFY(c.embedded);QVERIFY(c.distortion);QCOMPARE(applyRawLens(f,c),f.linear);}
    void embeddedVignettingAndPrecision(){auto f=frame();f.dngOpcodes=opcodes({operation(3,{.2,0,0,0,0})});const auto c=automaticRawLens(f);QVERIFY(c.vignetting);
        auto out=applyRawLens(f,c);QCOMPARE(pixel64(out,32,24),pixel64(f.linear,32,24));QCOMPARE(pixel64(out,0,0).red(),quint16(qRound(10001*1.2)));QVERIFY(pixel64(out,0,0).red()%257!=0);}
    void embeddedWarpAndOperationOrder(){auto f=frame();f.dngOpcodes=opcodes({operation(1,{1,.08,0,0,.002,.003}),operation(3,{.1,0,0,0,0})});auto c=automaticRawLens(f);QVERIFY(c.embedded);QVERIFY(c.distortion&&c.vignetting);QVERIFY(applyRawLens(f,c)!=f.linear);
        f.dngOpcodes=opcodes({operation(2,{1,0,0,0})});c=automaticRawLens(f);QVERIFY(c.embedded);QVERIFY(applyRawLens(f,c)!=f.linear);}
    void embeddedValidation(){auto f=frame();f.make={};const auto good=opcodes({operation(1,{1,0,0,0,0,0})});
        for(int cut:{0,1,4,10,int(good.size()-1)}){f.dngOpcodes=good.left(cut);QVERIFY(!automaticRawLens(f).available());}
        f.dngOpcodes=opcodes({operation(1,{std::numeric_limits<double>::quiet_NaN(),0,0,0,0,0})});QVERIFY(!automaticRawLens(f).available());
        auto invalid=good;qToBigEndian<quint32>(0xffffffff,invalid.data()+16);f.dngOpcodes=invalid;QVERIFY(!automaticRawLens(f).available());
        f.dngOpcodes=opcodes({operation(99,{})});QVERIFY(!automaticRawLens(f).available());
        f.dngOpcodes=opcodes({operation(99,{},.5,.5,1),operation(1,{1,0,0,0,0,0})});auto c=automaticRawLens(f);QVERIFY(c.available());QCOMPARE(c.status,QString("rawLensEmbeddedPartial"));
        f.dngOpcodes=good;f.embeddedGeometryCompatible=false;QVERIFY(!automaticRawLens(f).available());}
    void channelWarpsMustAgree(){auto f=frame();f.make={};f.dngOpcodes=opcodes({operation(1,{1,0,0,0,0,0},.5,.5,0,3)});QVERIFY(automaticRawLens(f).available());
        auto bytes=f.dngOpcodes;char d[8];qToBigEndian(std::bit_cast<quint64>(1.1),d);bytes.replace(24+48,8,QByteArray(d,8));f.dngOpcodes=bytes;QVERIFY(!automaticRawLens(f).available());}
    void orientationsPreserveOpticalCenter(){auto f=frame();f.dngOpcodes=opcodes({operation(3,{.2,0,0,0,0},.2,.7)});const auto corrected=applyRawLens(f,automaticRawLens(f));
        for(int flip=0;flip<8;++flip){auto rotated=f;rotated.orientation=flip;rotated.linear=orientRawImage(f.linear,flip);
            QCOMPARE(orientRawImage(rotated.linear,flip,true),f.linear);QCOMPARE(applyRawLens(rotated,automaticRawLens(rotated)),orientRawImage(corrected,flip));}}
    void embeddedDefaultCropAfterCorrection(){auto f=frame();f.dngDefaultCrop=QRect(5,7,42,30);f.dngOpcodes=opcodes({operation(1,{1,0,0,0,0,0})});
        const auto c=automaticRawLens(f);QCOMPARE(applyRawLens(f,c),f.linear.copy(f.dngDefaultCrop));
        f.orientation=6;f.linear=orientRawImage(f.linear,6);QCOMPARE(applyRawLens(f,c),orientRawImage(orientRawImage(f.linear,6,true).copy(f.dngDefaultCrop),6));}
    void reducedPreviewAndDisabledPath(){auto f=frame();auto c=automaticRawLens(f,database());auto p=applyRawLens(f,c,32);QVERIFY(p.width()<=32&&p.height()<=32);QVERIFY(wideImage(p));QCOMPARE(applyRawLens(f,{}),f.linear);}
    void librawExtractsEmbeddedAndExif(){const auto ops=opcodes({operation(3,{.2,0,0,0,0},.2,.7)});auto f=decodeRawBytes(dng(ops,6),0,false);
        QCOMPARE(f.focal,50.);QVERIFY(std::abs(f.aperture-2.8)<.001);QCOMPARE(f.lens,QString("Canon EF 50mm f/1.8 II"));QCOMPARE(f.lensMake,QString("Canon"));QCOMPARE(f.orientation,6);QCOMPARE(f.dngOpcodes,ops);
        auto c=automaticRawLens(f);QVERIFY2(c.available(),qPrintable(c.status));QVERIFY(c.embedded);auto out=applyRawLens(f,c);QVERIFY(out!=f.linear);QCOMPARE(out.size(),f.linear.size());
        const auto again=decodeRawBytes(f.encoded,35,true);QCOMPARE(again.dngOpcodes,ops);QCOMPARE(again.orientation,6);}
    void librawMetadataFindsOfflineProfile(){auto f=decodeRawBytes(dng({},1,true),0,false);QCOMPARE(f.make,QString("Canon"));
        auto c=automaticRawLens(f);QVERIFY2(c.available(),qPrintable(c.status));QVERIFY(!c.embedded);QVERIFY(c.distortion&&c.vignetting);QVERIFY(applyRawLens(f,c)!=f.linear);}
    void librawDefaultCrop(){auto f=decodeRawBytes(dng(opcodes({operation(1,{1,0,0,0,0,0})}),1,false,QRect(2,3,26,18)),0,false);
        QCOMPARE(f.dngDefaultCrop,QRect(2,3,26,18));const auto c=automaticRawLens(f);QVERIFY(c.embedded);QCOMPARE(applyRawLens(f,c),f.linear.copy(f.dngDefaultCrop));}
    void rawDialogAutomaticToggleResetAndImport_data(){QTest::addColumn<bool>("embedded");QTest::newRow("embedded")<<true;QTest::newRow("database")<<false;}
    void rawDialogAutomaticToggleResetAndImport(){QFETCH(bool,embedded);QTemporaryDir dir;QVERIFY(dir.isValid());const auto bytes=dng(embedded?opcodes({operation(3,{.2,0,0,0,0})}):QByteArray{},1,true);const auto path=dir.filePath("embedded.dng");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(bytes);file.close();
        MainWindow window;window.localization()->setLanguage("en");bool checked=false;QTimer modal;
        connect(&modal,&QTimer::timeout,this,[&]{auto *d=qobject_cast<QDialog *>(QApplication::activeModalWidget());if(!d)return;if(d->objectName()!="rawDevelopDialog"){qWarning()<<"Unexpected dialog:"<<d->windowTitle();d->reject();return;}
            modal.stop();auto *box=d->findChild<QCheckBox *>("rawLensCorrection");auto *status=d->findChild<QLabel *>("rawLensStatus");
            checked=box&&box->isEnabled()&&box->isChecked()&&status&&status->text().contains(embedded?"Embedded DNG":"Automatic:");
            if(box){box->setChecked(false);auto *buttons=d->findChild<QDialogButtonBox *>();buttons->button(QDialogButtonBox::Reset)->click();checked&=box->isChecked();}
            d->accept();});modal.start(20);window.openProject(path);QTRY_VERIFY_WITH_TIMEOUT(window.document()->hasDocument(),15000);QVERIFY(checked);
        auto f=decodeRawBytes(bytes,0,false);f.linear=applyRawLens(f,automaticRawLens(f));QCOMPARE(window.document()->state().layers[0].image,developRaw(f));window.document()->markSaved(window.document()->revision());}
    void patchVersionsOrderCorrectly(){auto feed=QJsonDocument(QJsonArray{release("0.5.3p1"),release("0.5.3"),release("0.5.4a1",true)}).toJson();
        auto update=findUpdate(feed,"0.5.3");QVERIFY(update);QCOMPARE(update->version,QString("0.5.3p1"));QVERIFY(!findUpdate(feed,"0.5.3p1"));
        feed=QJsonDocument(QJsonArray{release("0.5.3p2"),release("0.5.3p10"),release("0.5.4a1",true)}).toJson();QCOMPARE(findUpdate(feed,"0.5.3p1")->version,QString("0.5.3p10"));
        feed=QJsonDocument(QJsonArray{release("0.5.4")}).toJson();QCOMPARE(findUpdate(feed,"0.5.3p10")->version,QString("0.5.4"));
        feed=QJsonDocument(QJsonArray{release("0.5.3p0"),release("0.5.3p1",true)}).toJson();QVERIFY(!findUpdate(feed,"0.5.3"));}
};
QTEST_MAIN(RawLensTests)
#include "RawLensTests.moc"
