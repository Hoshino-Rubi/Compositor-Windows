#include "ProjectSaveDialog.h"
#include "Localization.h"
#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#endif
namespace comp {
std::optional<ProjectSaveChoice> chooseProjectSave(QWidget *parent, const Localization &language,
                                                  const QString &path, bool compatibility) {
    const auto title = language.text("saveAs");
    const auto label = language.text("saveCompatibility");
    const auto hint = language.text("saveCompatibilityHint");
#ifdef Q_OS_WIN
    if (QApplication::platformName() == "windows" && !qEnvironmentVariableIsSet("COMPOSITOR_HEADLESS_TEST")) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        struct Cleanup { bool owned; ~Cleanup() { if (owned) CoUninitialize(); } } cleanup{SUCCEEDED(initialized)};
        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        Microsoft::WRL::ComPtr<IFileDialogCustomize> controls;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(dialog.As(&controls))) {
            const auto wide = [](const QString &s) { return reinterpret_cast<LPCWSTR>(s.utf16()); };
            const COMDLG_FILTERSPEC filters[] = {{L"Compositor (*.comp)", L"*.comp"}};
            FILEOPENDIALOGOPTIONS options{};
            dialog->GetOptions(&options);
            dialog->SetOptions((options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR) &
                               ~(FOS_OVERWRITEPROMPT | FOS_FILEMUSTEXIST));
            dialog->SetTitle(wide(title));
            dialog->SetFileTypes(1, filters);
            dialog->SetDefaultExtension(L"comp");
            const QFileInfo info(path);
            const auto name = info.fileName();
            dialog->SetFileName(wide(name));
            if (info.isAbsolute()) {
                Microsoft::WRL::ComPtr<IShellItem> folder;
                const auto directory = info.absolutePath();
                if (SUCCEEDED(SHCreateItemFromParsingName(wide(directory), nullptr, IID_PPV_ARGS(&folder))))
                    dialog->SetFolder(folder.Get());
            }
            if (SUCCEEDED(controls->AddCheckButton(1001, wide(label), compatibility)) &&
                SUCCEEDED(controls->AddText(1002, wide(hint)))) {
                const HRESULT shown = dialog->Show(reinterpret_cast<HWND>(parent->winId()));
                if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
                Microsoft::WRL::ComPtr<IShellItem> item;
                if (SUCCEEDED(shown) && SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR selected = nullptr;
                    BOOL checked = FALSE;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) &&
                        SUCCEEDED(controls->GetCheckButtonState(1001, &checked))) {
                        const auto result = QString::fromWCharArray(selected);
                        CoTaskMemFree(selected);
                        return ProjectSaveChoice{result, checked != FALSE};
                    }
                    CoTaskMemFree(selected);
                }
                if (FAILED(shown)) return {};
            }
        }
    }
#endif
    QFileDialog dialog(parent, title, path, "Compositor (*.comp)");
    dialog.setObjectName("projectSaveDialog");
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setOption(QFileDialog::DontConfirmOverwrite);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("comp");
    auto *check = new QCheckBox(label, &dialog);
    check->setObjectName("saveCompatibility");
    check->setChecked(compatibility);
    auto *description = new QLabel(hint, &dialog);
    description->setWordWrap(true);
    auto *layout = qobject_cast<QGridLayout *>(dialog.layout());
    if (layout) {
        layout->addWidget(check, layout->rowCount(), 0, 1, layout->columnCount());
        layout->addWidget(description, layout->rowCount(), 0, 1, layout->columnCount());
    }
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    return ProjectSaveChoice{dialog.selectedFiles().first(), check->isChecked()};
}
}
