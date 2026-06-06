#include "NZXTKrakenLCDPlugin.h"
#include "ui/KrakenLCDWidget.h"
#include <QPainter>
#include <QFont>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QElapsedTimer>
#include <QDebug>
#include <QDateTime>
#include <hidapi/hidapi.h>

// ============================================================================
//  Logging du plugin, integre aux reglages d'OpenRGB
//  ------------------------------------------------------------------------
//  Le plugin ne peut PAS appeler le LogManager d'OpenRGB directement : sous Windows il faudrait linker OpenRGB.exe (dont les symboles ne sont pas exportes), ce que ce plugin autonome evite volontairement. A la place on installe un handler de messages Qt qui REPRODUIT le comportement et le format d'OpenRGB :
//    * lit OpenRGB.json (section "LogManager") pour respecter les cases "Enable Log File" / "Enable Log Console", le "loglevel" et le "file_count_limit" ;
//    * ecrit dans le dossier logs/ d'OpenRGB, avec le meme schema de nom (NZXTKrakenLCD_<AAAAMMJJ_HHMMSS>.log, comme OpenRGB_<...>.log) ;
//    * meme entete (lignes indentees + separateur de 100 '=') et meme format de ligne :  <ms>  |<Code>  <message>  ;
//    * applique la rotation par file_count_limit (comme OpenRGB) ;
//    * filtre par niveau (meme logique qu'OpenRGB) ;
//    * relaie au handler precedent les messages des autres composants.
// ============================================================================
namespace {

// Niveaux alignes sur LogManager.h d'OpenRGB (enum LL_*)
enum { KL_FATAL = 0, KL_ERROR = 1, KL_WARNING = 2, KL_INFO = 3,
       KL_VERBOSE = 4, KL_DEBUG = 5, KL_TRACE = 6 };

QFile*           g_logFile        = nullptr;
QtMessageHandler g_prevHandler    = nullptr;
QMutex           g_logMutex;
QElapsedTimer    g_clock;                  // base du compteur ms (comme base_clock d'OpenRGB)
bool             g_installed      = false;
bool             g_fileEnabled    = true;  // defaut OpenRGB : fichier ON
bool             g_consoleEnabled = false; // defaut OpenRGB : console OFF
int              g_logLevel       = KL_INFO;
int              g_fileCountLimit = 0;     // 0 = pas de limite (defaut OpenRGB)

// ── Dossier de configuration d'OpenRGB (contient OpenRGB.json et logs/) ──────
QString openRGBConfigDir()
{
    // 1) Mode portable : OpenRGB.json a cote de l'executable
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(exeDir + QStringLiteral("/OpenRGB.json")))
        return exeDir;

    // 2) Emplacement standard Windows : %APPDATA%/OpenRGB
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty())
        return appdata + QStringLiteral("/OpenRGB");

    // 3) Repli generique multiplateforme
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/OpenRGB");
}

// ── Reglages de log depuis OpenRGB.json (section "LogManager") ───────────────
void loadOpenRGBLogSettings(const QString& cfgDir)
{
    QFile f(cfgDir + QStringLiteral("/OpenRGB.json"));
    if (!f.open(QIODevice::ReadOnly))
        return;   // OpenRGB.json introuvable -> on garde les defauts d'OpenRGB

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return;

    const QJsonObject lm = doc.object().value(QStringLiteral("LogManager")).toObject();
    if (lm.contains(QStringLiteral("log_file")))
        g_fileEnabled = lm.value(QStringLiteral("log_file")).toBool(g_fileEnabled);
    if (lm.contains(QStringLiteral("log_console")))
        g_consoleEnabled = lm.value(QStringLiteral("log_console")).toBool(g_consoleEnabled);
    if (lm.contains(QStringLiteral("loglevel")))
        g_logLevel = lm.value(QStringLiteral("loglevel")).toInt(g_logLevel);
    if (lm.contains(QStringLiteral("file_count_limit")))
        g_fileCountLimit = lm.value(QStringLiteral("file_count_limit")).toInt(g_fileCountLimit);
}

int levelForType(QtMsgType t)
{
    switch (t) {
        case QtFatalMsg:    return KL_FATAL;
        case QtCriticalMsg: return KL_ERROR;
        case QtWarningMsg:  return KL_WARNING;
        case QtInfoMsg:     return KL_INFO;
        case QtDebugMsg:    return KL_DEBUG;
        default:            return KL_DEBUG;
    }
}

// Codes EXACTEMENT identiques a ceux d'OpenRGB (LogManager::log_codes)
const char* levelCode(int lvl)
{
    switch (lvl) {
        case KL_FATAL:   return "FATAL:";
        case KL_ERROR:   return "ERROR:";
        case KL_WARNING: return "Warning:";
        case KL_INFO:    return "Info:";
        case KL_VERBOSE: return "Verbose:";
        case KL_DEBUG:   return "Debug:";
        default:         return "Trace:";
    }
}

// Premier segment du tag entre crochets : identifie les messages du plugin
bool isPluginTag(const QString& root)
{
    return root == QLatin1String("Device")
        || root == QLatin1String("HID")
        || root == QLatin1String("USB")
        || root == QLatin1String("Frame")
        || root == QLatin1String("Sensors")
        || root == QLatin1String("Renderer")
        || root == QLatin1String("Media")
        || root == QLatin1String("Persistence")
        || root == QLatin1String("Worker")
        || root == QLatin1String("Audio")
        || root == QLatin1String("UI")
        || root == QLatin1String("Core");
}

// Ecriture bas niveau (entete + lignes deja formatees), thread-safe
void writeRaw(const QString& text)
{
    QMutexLocker lk(&g_logMutex);
    if (g_fileEnabled && g_logFile && g_logFile->isOpen()) {
        g_logFile->write(text.toUtf8());
        g_logFile->write("\n", 1);
        g_logFile->flush();
    }
    if (g_consoleEnabled) {
        fputs(qPrintable(text), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

void krakenMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    // Tag [Sous-systeme] en tete : le premier segment sert au routage, mais on n'affiche que le dernier ([KrakenLCD/WinHID] -> [WinHID], [Sensors] -> [Sensors]).
    QString tag;
    int close = -1;
    if (msg.startsWith('[')) {
        close = msg.indexOf(']');
        if (close > 1) tag = msg.mid(1, close - 1);
    }
    const int firstSlash = tag.indexOf('/');
    const QString root = (firstSlash >= 0) ? tag.left(firstSlash) : tag;

    // Message qui ne vient pas du plugin -> relai au handler precedent
    if (tag.isEmpty() || !isPluginTag(root)) {
        if (g_prevHandler) g_prevHandler(type, ctx, msg);
        else               fprintf(stderr, "%s\n", qPrintable(msg));
        return;
    }

    const int lvl = levelForType(type);
    if (lvl > g_logLevel) return;   // filtrage par niveau, identique a OpenRGB

    // Ne garder que l'element concerne dans le tag affiche
    const int lastSlash = tag.lastIndexOf('/');
    const QString shortTag = (lastSlash >= 0) ? tag.mid(lastSlash + 1) : tag;
    const QString outMsg = QStringLiteral("[") + shortTag + QStringLiteral("] ")
                           + msg.mid(close + 1).trimmed();

    // Format OpenRGB :  "<ms_left6>|<Code_left9><message>"
    const qint64 ms = g_clock.isValid() ? g_clock.elapsed() : 0;
    writeRaw(QStringLiteral("%1|%2%3")
                 .arg(QString::number(ms).leftJustified(6),
                      QString::fromLatin1(levelCode(lvl)).leftJustified(9),
                      outMsg));
}

// Rotation des logs du plugin (memes regles qu'OpenRGB::rotate_logs)
void rotatePluginLogs(const QString& logsDir)
{
    if (g_fileCountLimit < 1) return;   // 0/absent = pas de limite (defaut OpenRGB)
    QDir d(logsDir);
    // Tri par nom = tri chronologique (horodatage AAAAMMJJ_HHMMSS en tete)
    const QStringList files = d.entryList(QStringList()
                                  << QStringLiteral("NZXTKrakenLCD_*.log"),
                                  QDir::Files, QDir::Name);
    int removeCount = files.size() - g_fileCountLimit + 1;  // +1 : place pour le nouveau
    for (int i = 0; i < removeCount && i < files.size(); ++i)
        QFile::remove(d.filePath(files[i]));
}

void installKrakenLogger(const QString& hidVer)
{
    if (g_installed) return;
    g_installed = true;
    g_clock.start();

    const QString cfgDir = openRGBConfigDir();
    loadOpenRGBLogSettings(cfgDir);

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));

    if (g_fileEnabled) {
        const QString logsDir = cfgDir + QStringLiteral("/logs");
        QDir().mkpath(logsDir);
        rotatePluginLogs(logsDir);
        g_logFile = new QFile(logsDir + QStringLiteral("/NZXTKrakenLCD_") + stamp + QStringLiteral(".log"));
        if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            delete g_logFile;
            g_logFile     = nullptr;
            g_fileEnabled = false;   // dossier non inscriptible -> on desactive le fichier
        }
    }

    g_prevHandler = qInstallMessageHandler(krakenMessageHandler);

    // Entete, meme forme qu'OpenRGB (lignes indentees + separateur de 100 '=')
    writeRaw(QStringLiteral("    NZXT Kraken LCD plugin v1.0.0"));
    writeRaw(QStringLiteral("    hidapi %1 | Qt %2").arg(hidVer, QString::fromLatin1(qVersion())));
    writeRaw(QStringLiteral("    OpenRGB config: %1").arg(cfgDir));
    writeRaw(QStringLiteral("    Log settings: file=%1  console=%2  loglevel=%3")
                 .arg(g_fileEnabled    ? QStringLiteral("on") : QStringLiteral("off"),
                      g_consoleEnabled ? QStringLiteral("on") : QStringLiteral("off"))
                 .arg(g_logLevel));
    writeRaw(QStringLiteral("    Launched: %1").arg(stamp));
    writeRaw(QString(100, QChar('=')));
    writeRaw(QString());
}

void uninstallKrakenLogger()
{
    if (!g_installed) return;
    qInstallMessageHandler(g_prevHandler);   // restaure le handler precedent
    g_prevHandler = nullptr;
    {
        QMutexLocker lk(&g_logMutex);
        if (g_logFile) { g_logFile->close(); delete g_logFile; g_logFile = nullptr; }
    }
    g_installed = false;
}

} // namespace

NZXTKrakenLCDPlugin::~NZXTKrakenLCDPlugin()
{
    Unload();
}

OpenRGBPluginInfo NZXTKrakenLCDPlugin::GetPluginInfo()
{
    OpenRGBPluginInfo info;

    info.Name        = "NZXT Kraken LCD";
    info.Description = "Controls the LCD screen of NZXT Kraken coolers.";
    info.Version     = "1.0.0";
    info.Commit      = "release";
    info.URL         = "https://github.com/LeProta/NZXTKrakenLCDPlugin";
    info.Location    = OPENRGB_PLUGIN_LOCATION_TOP;
    info.Label       = "NZXT Kraken LCD";
    info.TabIconString = "";

    // Icône 64×64 générée sans fichier externe
    QImage icon(64, 64, QImage::Format_ARGB32);
    icon.fill(Qt::black);
    {
        QPainter ip(&icon);
        ip.setRenderHint(QPainter::Antialiasing);
        ip.setPen(QPen(QColor(0, 180, 255), 2));
        ip.setBrush(Qt::NoBrush);
        ip.drawEllipse(4, 4, 56, 56);
        QFont f("Arial", 10, QFont::Bold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        ip.setFont(f);
        ip.setPen(Qt::white);
        ip.drawText(QRect(0, 0, 64, 64), Qt::AlignCenter, "NZXT\nLCD");
    }
    info.Icon    = icon;
    info.TabIcon = icon.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    return info;
}

unsigned int NZXTKrakenLCDPlugin::GetPluginAPIVersion()
{
    return OPENRGB_PLUGIN_API_VERSION;  // 4
}

void NZXTKrakenLCDPlugin::Load(ResourceManagerInterface* /*rm*/)
{
    const struct hid_api_version* v = hid_version();
    const QString hidVer = v ? QString("%1.%2.%3").arg(v->major).arg(v->minor).arg(v->patch)
                             : QStringLiteral("(unknown)");
    installKrakenLogger(hidVer);

    qInfo() << "[Core] Plugin loaded.";

#ifdef _WIN32
    // lhwm-wrapper.dll (assembly .NET de LibreHardwareMonitor) doit être à côté
    // d'OpenRGB.exe. La lib statique liée le charge au runtime : on ne crée donc
    // le widget (qui démarre les capteurs) QUE si la DLL est présente. Sinon
    // GetWidget() renvoie un message d'installation, sans rien appeler côté LHWM.
    const bool lhwmPresent =
        QFileInfo::exists(QCoreApplication::applicationDirPath()
                          + QStringLiteral("/lhwm-wrapper.dll"));
    if (!lhwmPresent) {
        qWarning() << "[Core] lhwm-wrapper.dll not found in"
                   << QCoreApplication::applicationDirPath()
                   << "— widget disabled (install message shown).";
        return;   // m_widget reste nullptr -> GetWidget() affiche le message
    }
#endif

    if (!m_widget)
        m_widget = new NZXTKraken::KrakenLCDWidget;
}

QWidget* NZXTKrakenLCDPlugin::GetWidget()
{
    if (m_widget) return m_widget;

    // lhwm-wrapper.dll absente → message global (comme le plugin Hardware Fan Sync).
    QLabel* label = new QLabel(
        QStringLiteral(
            "<h1>Cannot load the plugin.</h1>"
            "<p>Make sure you downloaded "
            "<a href=\"https://gitlab.com/OpenRGBDevelopers/lhwm-wrapper/-/jobs/artifacts/master/download?job=main_build\">lhwm-wrapper</a></p>"
            "<p>Place this DLL inside <b>%1</b> and restart OpenRGB.</p>")
        .arg(QCoreApplication::applicationDirPath()));
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setOpenExternalLinks(true);
    label->setMargin(20);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

QMenu* NZXTKrakenLCDPlugin::GetTrayMenu()
{
    return nullptr;
}

void NZXTKrakenLCDPlugin::Unload()
{
    if (m_widget)
    {
        delete m_widget;
        m_widget = nullptr;
        qInfo() << "[Core] Plugin unloaded.";
    }
    uninstallKrakenLogger();
}
