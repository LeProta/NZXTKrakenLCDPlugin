#include "KrakenMediaCache.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

namespace NZXTKraken {

// ── Helpers ─────────────────────────────────────────────────────────────────
static QString krakenBaseDir()
{
    // AppDataLocation → .../AppData/Roaming/OpenRGB  (quand l'hôte est OpenRGB)
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // Évite le doublon .../OpenRGB/OpenRGB/ si AppDataLocation ne finit pas par OpenRGB
    if (!base.endsWith(QStringLiteral("/OpenRGB"), Qt::CaseInsensitive)
        && !base.endsWith(QStringLiteral("\\OpenRGB"), Qt::CaseInsensitive))
        base += QStringLiteral("/OpenRGB");
    return base + QStringLiteral("/NZXTKrakenLCD");
}

// ── Public ──────────────────────────────────────────────────────────────────
QString KrakenMediaCache::rootDirectory()
{
    QString dir = krakenBaseDir();
    QDir().mkpath(dir);
    return dir;
}

QString KrakenMediaCache::mediaDirectory()
{
    QString dir = krakenBaseDir() + QStringLiteral("/media");
    QDir().mkpath(dir);
    return dir;
}

QString KrakenMediaCache::importFile(const QString& sourcePath)
{
    if (sourcePath.isEmpty()) return {};

    QFileInfo fi(sourcePath);
    if (!fi.exists() || !fi.isFile()) {
        qWarning() << "[KrakenLCD/Media] Source file not found:" << sourcePath;
        return {};
    }

    QString destDir = mediaDirectory();
    QString destPath = destDir + QStringLiteral("/") + fi.fileName();

    // Si c'est déjà dans le dossier media, on ne recopie pas
    if (QFileInfo(destPath).absoluteFilePath() == fi.absoluteFilePath())
        return destPath;

    // Écrase si existe déjà
    if (QFile::exists(destPath))
        QFile::remove(destPath);

    if (!QFile::copy(sourcePath, destPath)) {
        qWarning() << "[KrakenLCD/Media] Copy failed:" << sourcePath << "→" << destPath;
        return {};
    }

    qDebug() << "[KrakenLCD/Media] Imported:" << fi.fileName() << "→" << destDir;
    return destPath;
}

QString KrakenMediaCache::downloadsDirectory()
{
    QString dl = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dl.isEmpty())
        dl = QDir::homePath() + QStringLiteral("/Downloads");
    return dl;
}

} // namespace NZXTKraken
