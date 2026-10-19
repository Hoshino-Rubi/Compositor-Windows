#include "CameraRaw.h"
namespace comp {
QJsonObject resetCameraRawSection(QJsonObject s, const QString &section) {
    QStringList keys;
    if (section == "light") keys = {"exposure", "contrast", "highlights", "shadows", "whites", "blacks"};
    else if (section == "color") keys = {"whiteBalance", "temperature", "tint", "vibrance", "saturation"};
    else if (section == "effects") keys = {"texture", "clarity", "dehaze", "glow", "glowStyle", "glowRange", "glowSpread", "glowWarmth",
        "vignetteAmount", "vignetteStyle", "vignetteMidpoint", "vignetteRoundness", "vignetteFeather", "vignetteHighlights",
        "grainAmount", "grainSize", "grainRoughness"};
    else if (section == "pointColor") {
        auto mixer = s["mixer"].toObject(); mixer.remove("points"); s["mixer"] = mixer;
        s.remove("point"); s.remove("pickPoint"); s.remove("visualizePoint"); s["enableMixer"] = true; return s;
    } else if (QStringList{"curve", "mixer", "grading", "detail", "optics", "geometry", "calibration"}.contains(section)) keys = {section};
    else return s;
    for (const auto &key : keys) s.remove(key);
    if (section == "mixer") { s.remove("point"); s.remove("pickPoint"); s.remove("visualizePoint"); s.remove("mixerControl"); }
    QString enabled = section; enabled[0] = enabled[0].toUpper();
    s["enable" + enabled] = true;
    return s;
}
} // namespace comp
