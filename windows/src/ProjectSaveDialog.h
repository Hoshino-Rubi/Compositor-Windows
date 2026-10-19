#pragma once
#include <QString>
#include <optional>
class QWidget;
namespace comp {
class Localization;
struct ProjectSaveChoice { QString path; bool compatibility = false; };
std::optional<ProjectSaveChoice> chooseProjectSave(QWidget *parent, const Localization &language,
                                                  const QString &path, bool compatibility);
}
