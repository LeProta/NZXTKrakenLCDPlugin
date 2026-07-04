#include "UpdateChecker.h"
#include "KrakenOpenRGBSettings.h"
#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include <QPointer>
#include <QMetaObject>
#include <QDebug>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>
#include <winhttp.h>

namespace NZXTKraken {

// Fichier MIROIR de src/ui/UpdateChecker.cpp du plugin Pump : toute correction
// ici doit y être reportée. Seules différences : les 4 constantes ci-dessous
// et le namespace.
static const wchar_t* kApiPath   =
    L"/repos/LeProta/NZXTKrakenLCDPlugin/releases/latest";
static const wchar_t* kUserAgent = L"NZXTKrakenLCDPlugin";
static const char*    kLogTag    = "[KrakenLCD/Update]";
static const QString  kConfigSub = QStringLiteral("/NZXTKrakenLCD");

// "v1.2.3-beta" -> [1, 2, 3] (préfixe v/V ignoré, suffixe pré-release ignoré).
static QVector<int> parseSemver(const QString& s)
{
    QString v = s.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V')))
        v.remove(0, 1);
    QVector<int> parts;
    for (const QString& p : v.split(QLatin1Char('.')))
        parts.append(p.section(QLatin1Char('-'), 0, 0).toInt());
    return parts;
}

static bool isNewer(const QString& latest, const QString& current)
{
    const QVector<int> a = parseSemver(latest), b = parseSemver(current);
    const int n = std::max(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const int x = a.value(i, 0), y = b.value(i, 0);
        if (x != y) return x > y;
    }
    return false;
}

// GET HTTPS bloquant via WinHTTP. Renvoie le corps, ou vide en cas d'échec.
// Timeouts courts : borne le temps de vie du thread réseau détaché.
static std::string httpsGet(const wchar_t* host, const wchar_t* path)
{
    std::string body;
    HINTERNET session = WinHttpOpen(kUserAgent,
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (conn) {
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
        if (req) {
            const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(req, hdr, -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse(req, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0)
                        break;
                    body.append(chunk.data(), read);
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return body;
}

UpdateChecker::UpdateChecker(QObject* parent) : QObject(parent) {}

void UpdateChecker::check(const QString& currentVersion)
{
    // Épingle la DLL en mémoire (une fois) : le thread réseau et la lambda
    // postée à l'event loop exécutent du code du plugin — un unload pendant
    // la requête exécuterait des pages démappées. PIN = DLL résidente jusqu'à
    // la fin du process (coût mémoire négligeable, crash impossible).
    static const bool pinned = [] {
        HMODULE mod = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                      | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&httpsGet), &mod) != 0;
    }();
    if (!pinned)
        return;

    // Cache disque 6 h (GitHub limite à 60 req/h non authentifié) : le dernier
    // tag vu est réutilisé — la bannière s'affiche même sans requête réseau.
    const QString cachePath = OpenRGBSettings::configDir() + kConfigSub
                              + QStringLiteral("/update_check.json");
    {
        QFile f(cachePath);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject c = QJsonDocument::fromJson(f.readAll()).object();
            const QString tag = c.value(QStringLiteral("tag")).toString();
            const QString url = c.value(QStringLiteral("url")).toString();
            const qint64 checked =
                qint64(c.value(QStringLiteral("checked")).toDouble());
            if (!tag.isEmpty() && isNewer(tag, currentVersion))
                emit updateAvailable(tag, url);
            if (QDateTime::currentSecsSinceEpoch() - checked < 6 * 3600)
                return;   // cache frais : pas de requête
        }
    }

    // Le QPointer n'est écrit (ici) et déréférencé (lambda GUI) QUE sur le
    // thread GUI — le thread réseau ne copie que le shared_ptr (thread-safe).
    struct Ctx { QPointer<UpdateChecker> self; QString version; QString cache; };
    auto ctx = std::make_shared<Ctx>();
    ctx->self    = this;
    ctx->version = currentVersion;
    ctx->cache   = cachePath;

    std::thread([ctx] {
        const std::string body = httpsGet(L"api.github.com", kApiPath);
        QCoreApplication* app = QCoreApplication::instance();
        if (!app)
            return;   // arrêt du process en cours
        QMetaObject::invokeMethod(app, [ctx, body] {
            const QJsonObject obj =
                QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
            const QString tag = obj.value(QStringLiteral("tag_name")).toString();
            const QString url = obj.value(QStringLiteral("html_url")).toString();
            if (tag.isEmpty()) {
                qInfo() << kLogTag << "check failed or empty response.";
                return;
            }
            QJsonObject c;
            c[QStringLiteral("tag")]     = tag;
            c[QStringLiteral("url")]     = url;
            c[QStringLiteral("checked")] =
                double(QDateTime::currentSecsSinceEpoch());
            QDir().mkpath(QFileInfo(ctx->cache).absolutePath());
            QFile f(ctx->cache);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(c).toJson(QJsonDocument::Compact));
            if (!ctx->self)
                return;
            if (isNewer(tag, ctx->version)) {
                qInfo() << kLogTag << "newer release available:" << tag;
                emit ctx->self->updateAvailable(tag, url);
            } else {
                qInfo() << kLogTag << "up to date (" << ctx->version << ").";
            }
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace NZXTKraken
