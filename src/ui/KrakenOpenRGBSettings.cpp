#include "KrakenOpenRGBSettings.h"
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace NZXTKraken {
namespace OpenRGBSettings {

QString configDir()
{
    // 1) Mode portable : OpenRGB.json à côté de l'exécutable
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(exeDir + QStringLiteral("/OpenRGB.json")))
        return exeDir;

    // 2) Emplacement standard Windows : %APPDATA%/OpenRGB
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty())
        return appdata + QStringLiteral("/OpenRGB");

    // 3) Repli générique multiplateforme
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/OpenRGB");
}

// Charge la section "UserInterface" d'OpenRGB.json (objet vide si absent).
static QJsonObject userInterfaceSection()
{
    QFile f(configDir() + QStringLiteral("/OpenRGB.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return {};
    return doc.object().value(QStringLiteral("UserInterface")).toObject();
}

QString language()
{
    const QString lang = userInterfaceSection()
                             .value(QStringLiteral("language")).toString();
    return lang.isEmpty() ? QStringLiteral("default") : lang;
}

QString hexFormat()
{
    const QString fmt = userInterfaceSection()
                            .value(QStringLiteral("hex_format")).toString().toUpper();
    return (fmt == QLatin1String("BGR")) ? QStringLiteral("BGR")
                                         : QStringLiteral("RGB");
}

bool prefersFahrenheit()
{
    const QString lang = language();

    // Parmi les langues OpenRGB, seule "English - US" utilise le Fahrenheit
    // (English - UK / English - AU et toutes les autres sont en Celsius).
    if (lang.contains(QStringLiteral("US")))
        return true;

    // "System Default" (texte affiché) / "default" / vide -> suivre la locale
    // système. Toute autre langue explicite -> Celsius.
    const bool systemDefault =
           lang.isEmpty()
        || lang.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0
        || lang.contains(QStringLiteral("Default"), Qt::CaseInsensitive);
    if (!systemDefault)
        return false;

    switch (QLocale::system().country()) {
        case QLocale::UnitedStates:
        case QLocale::Bahamas:
        case QLocale::Belize:
        case QLocale::CaymanIslands:
        case QLocale::Palau:
        case QLocale::Liberia:
            return true;
        default:
            return false;
    }
}

} // namespace OpenRGBSettings
} // namespace NZXTKraken
