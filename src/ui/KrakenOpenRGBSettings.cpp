#include "KrakenOpenRGBSettings.h"
#include "OpenRGBPluginInterface.h"
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>

namespace NZXTKraken {
namespace OpenRGBSettings {

static OpenRGBPluginAPIInterface* g_api = nullptr;
static QMutex                     g_mutex;
static QJsonObject                g_ui;   // cached "UserInterface" section

void init(OpenRGBPluginAPIInterface* api)
{
    g_api = api;
    refresh();
}

void refresh()
{
    const QJsonObject ui = section(QStringLiteral("UserInterface"));
    QMutexLocker lk(&g_mutex);
    g_ui = ui;
}

QJsonObject section(const QString& key)
{
    if (!g_api) return {};
    const std::string json = g_api->GetSettings(key.toStdString()).dump();
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
}

QString configDir()
{
    if (!g_api)
        return qEnvironmentVariable("APPDATA") + QStringLiteral("/OpenRGB");
    QString dir = QString::fromStdString(g_api->GetConfigurationDirectory().generic_u8string());
    while (dir.endsWith(QLatin1Char('/'))) dir.chop(1);
    return dir;
}

static QJsonObject userInterfaceSection()
{
    QMutexLocker lk(&g_mutex);
    return g_ui;
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

    switch (QLocale::system().territory()) {
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
