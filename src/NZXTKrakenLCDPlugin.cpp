#include "NZXTKrakenLCDPlugin.h"
#include "Version.h"
#include "ui/KrakenLCDWidget.h"
#include "ui/KrakenOpenRGBSettings.h"
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
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

enum { KL_FATAL = 0, KL_ERROR = 1, KL_WARNING = 2, KL_INFO = 3,
       KL_VERBOSE = 4, KL_DEBUG = 5, KL_TRACE = 6 };

QFile*           g_logFile        = nullptr;
QtMessageHandler g_prevHandler    = nullptr;
QMutex           g_logMutex;
QElapsedTimer    g_clock;
bool             g_installed      = false;
bool             g_pinnedInChain  = false;
bool             g_fileEnabled    = true;
bool             g_consoleEnabled = false;
int              g_logLevel       = KL_INFO;
int              g_fileCountLimit = 10;   // OpenRGB 1.1 LogManager default

void loadOpenRGBLogSettings()
{
    const QJsonObject lm = NZXTKraken::OpenRGBSettings::section(QStringLiteral("LogManager"));
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

bool isPluginTag(const QString& root)
{
    return root == QLatin1String("KrakenLCD");
}

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
    QString tag;
    int close = -1;
    if (msg.startsWith('[')) {
        close = msg.indexOf(']');
        if (close > 1) tag = msg.mid(1, close - 1);
    }
    const int firstSlash = tag.indexOf('/');
    const QString root = (firstSlash >= 0) ? tag.left(firstSlash) : tag;

    if (!g_installed || tag.isEmpty() || !isPluginTag(root)) {
        if (g_prevHandler) g_prevHandler(type, ctx, msg);
        else               fprintf(stderr, "%s\n", qPrintable(msg));
        return;
    }

    const int lvl = levelForType(type);
    if (lvl > g_logLevel) return;

    const int lastSlash = tag.lastIndexOf('/');
    const QString shortTag = (lastSlash >= 0) ? tag.mid(lastSlash + 1) : tag;
    const QString outMsg = QStringLiteral("[") + shortTag + QStringLiteral("] ")
                           + msg.mid(close + 1).trimmed();

    const qint64 ms = g_clock.isValid() ? g_clock.elapsed() : 0;
    writeRaw(QStringLiteral("%1|%2%3")
                 .arg(QString::number(ms).leftJustified(6),
                      QString::fromLatin1(levelCode(lvl)).leftJustified(9),
                      outMsg));
}

void rotatePluginLogs(const QString& logsDir)
{
    if (g_fileCountLimit < 1) return;
    QDir d(logsDir);
    const QStringList files = d.entryList(QStringList()
                                  << QStringLiteral("NZXTKrakenLCD_*.log"),
                                  QDir::Files, QDir::Name);
    int removeCount = files.size() - g_fileCountLimit + 1;
    for (int i = 0; i < removeCount && i < files.size(); ++i)
        QFile::remove(d.filePath(files[i]));
}

void installKrakenLogger(const QString& hidVer)
{
    if (g_installed) return;
    g_installed = true;
    g_clock.start();

    const QString cfgDir = NZXTKraken::OpenRGBSettings::configDir();
    loadOpenRGBLogSettings();

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));

    if (g_fileEnabled) {
        const QString logsDir = cfgDir + QStringLiteral("/logs");
        QDir().mkpath(logsDir);
        rotatePluginLogs(logsDir);
        g_logFile = new QFile(logsDir + QStringLiteral("/NZXTKrakenLCD_") + stamp + QStringLiteral(".log"));
        if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            delete g_logFile;
            g_logFile     = nullptr;
            g_fileEnabled = false;
        }
    }

    if (!g_pinnedInChain)
        g_prevHandler = qInstallMessageHandler(krakenMessageHandler);

    writeRaw(QStringLiteral("    NZXT Kraken LCD plugin v" NZXT_LCD_VERSION));
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
    QtMessageHandler cur = qInstallMessageHandler(g_prevHandler);
    if (cur != krakenMessageHandler) {
        qInstallMessageHandler(cur);
        g_pinnedInChain = true;
#ifdef _WIN32
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&krakenMessageHandler), &self);
#endif
    } else {
        g_prevHandler   = nullptr;
        g_pinnedInChain = false;
    }
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
    info.Version     = NZXT_LCD_VERSION;
    info.Commit      = "release";
    info.URL         = "https://github.com/LeProta/NZXTKrakenLCDPlugin";
    info.Location    = OPENRGB_PLUGIN_LOCATION_TOP;
    info.Label       = "NZXT Kraken LCD";
    info.TabIconString = "";
    info.ProtocolVersion = 0;   // no SDK commands

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
    return OPENRGB_PLUGIN_API_VERSION;
}

void NZXTKrakenLCDPlugin::Load(OpenRGBPluginAPIInterface* rm)
{
    const struct hid_api_version* v = hid_version();
    const QString hidVer = v ? QString("%1.%2.%3").arg(v->major).arg(v->minor).arg(v->patch)
                             : QStringLiteral("(unknown)");
    NZXTKraken::OpenRGBSettings::init(rm);
    installKrakenLogger(hidVer);

    qInfo() << "[KrakenLCD/Core] Plugin loaded.";

#ifdef _WIN32
    const bool lhwmPresent =
        QFileInfo::exists(QCoreApplication::applicationDirPath()
                          + QStringLiteral("/lhwm-wrapper.dll"));
    if (!lhwmPresent) {
        qWarning() << "[KrakenLCD/Core] lhwm-wrapper.dll not found in"
                   << QCoreApplication::applicationDirPath()
                   << "— widget disabled (install message shown).";
        return;
    }
#endif

    if (!m_widget)
        m_widget = new NZXTKraken::KrakenLCDWidget(rm);
}

QWidget* NZXTKrakenLCDPlugin::GetWidget()
{
    if (m_widget) return m_widget;

    QLabel* label = new QLabel(
        QStringLiteral(
            "<h1>Cannot load the plugin.</h1>"
            "<p>Make sure you downloaded "
            "<a href=\"https://github.com/LeProta/lhwm-wrapper/releases\">lhwm-wrapper</a></p>"
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

void NZXTKrakenLCDPlugin::SettingsManagerUpdated(unsigned int)
{
    NZXTKraken::OpenRGBSettings::refresh();
}

void NZXTKrakenLCDPlugin::Unload()
{
    if (m_widget)
    {
        delete m_widget;
        m_widget = nullptr;
        qInfo() << "[KrakenLCD/Core] Plugin unloaded.";
    }
    uninstallKrakenLogger();
}
