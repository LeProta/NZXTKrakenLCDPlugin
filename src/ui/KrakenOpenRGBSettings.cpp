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
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(exeDir + QStringLiteral("/OpenRGB.json")))
        return exeDir;

    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty())
        return appdata + QStringLiteral("/OpenRGB");

    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/OpenRGB");
}

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

    if (lang.contains(QStringLiteral("US")))
        return true;

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
