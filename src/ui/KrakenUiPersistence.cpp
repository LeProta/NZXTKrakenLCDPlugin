#include "KrakenUiPersistence.h"
#include "KrakenMediaCache.h"
#include "KrakenColorPicker.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QDebug>

namespace NZXTKraken {

// ── Helpers JSON ─────────────────────────────────────────────────────────────
static QJsonObject colorToJson(const QColor& c)
{
    return QJsonObject{
        {"r", c.red()}, {"g", c.green()}, {"b", c.blue()}, {"a", c.alpha()}
    };
}

static QColor jsonToColor(const QJsonObject& o, const QColor& def = Qt::white)
{
    if (o.isEmpty()) return def;
    return QColor(o["r"].toInt(def.red()),
                  o["g"].toInt(def.green()),
                  o["b"].toInt(def.blue()),
                  o["a"].toInt(def.alpha()));
}

static QJsonArray stopsToJson(const GradientStops& stops)
{
    QJsonArray arr;
    for (const auto& s : stops)
        arr.append(QJsonObject{ {"o", double(s.offset)}, {"c", colorToJson(s.color)} });
    return arr;
}

static GradientStops jsonToStops(const QJsonArray& arr, const GradientStops& fallback)
{
    GradientStops stops;
    for (const auto& v : arr) {
        QJsonObject so = v.toObject();
        GradStop s;
        s.offset = float(so["o"].toDouble(0));
        s.color  = jsonToColor(so["c"].toObject(), Qt::white);
        stops.append(s);
    }
    return stops.isEmpty() ? fallback : stops;
}

static QJsonObject readingToJson(const ReadingConfig& rc)
{
    QJsonArray stopsArr;
    for (const auto& s : rc.vizStops) {
        stopsArr.append(QJsonObject{
            {"o", double(s.offset)},
            {"c", colorToJson(s.color)}
        });
    }
    return QJsonObject{
        {"type",          int(rc.type)},
        {"numberFill",    colorToJson(rc.numberFill)},
        {"numberOutline", colorToJson(rc.numberOutline)},
        {"textFill",      colorToJson(rc.textFill)},
        {"textOutline",   colorToJson(rc.textOutline)},
        {"colorViz",      colorToJson(rc.colorViz)},
        {"vizStops",      stopsArr}
    };
}

// jsonToReading : reconstruit un ReadingConfig depuis JSON.
// Rétrocompat avec ancien format (colorNumber/colorText). Le caller peut
// passer un défaut spécifique au mode (singleReading, dualReading1, etc.).
static ReadingConfig jsonToReading(const QJsonObject& o, const ReadingConfig& fallback)
{
    if (o.isEmpty()) return fallback;

    ReadingConfig rc = fallback;
    rc.type = SensorType(o["type"].toInt(int(fallback.type)));

    if (o.contains("numberFill"))
        rc.numberFill = jsonToColor(o["numberFill"].toObject(), fallback.numberFill);
    else if (o.contains("colorNumber"))
        rc.numberFill = jsonToColor(o["colorNumber"].toObject(), fallback.numberFill);

    rc.numberOutline = jsonToColor(o["numberOutline"].toObject(), fallback.numberOutline);

    if (o.contains("textFill"))
        rc.textFill = jsonToColor(o["textFill"].toObject(), fallback.textFill);
    else if (o.contains("colorText"))
        rc.textFill = jsonToColor(o["colorText"].toObject(), fallback.textFill);

    rc.textOutline = jsonToColor(o["textOutline"].toObject(), fallback.textOutline);
    rc.colorViz    = jsonToColor(o["colorViz"].toObject(),    fallback.colorViz);

    if (o.contains("vizStops")) {
        QJsonArray arr = o["vizStops"].toArray();
        GradientStops stops;
        for (const auto& v : arr) {
            QJsonObject so = v.toObject();
            GradStop s;
            s.offset = float(so["o"].toDouble(0));
            s.color  = jsonToColor(so["c"].toObject(), Qt::white);
            stops.append(s);
        }
        if (!stops.isEmpty()) rc.vizStops = stops;
    }
    if (rc.vizStops.isEmpty()) rc.vizStops = fallback.vizStops;
    return rc;
}

// Helpers ModeConfig
static QJsonObject modeToJson(const ModeConfig& m)
{
    return QJsonObject{
        {"bgColor",   colorToJson(m.bgColor)},
        {"logoColor", colorToJson(m.logoColor)},
        {"showLogo",  m.showLogo},
        {"gifPath",   m.gifPath},
    };
}
static ModeConfig jsonToMode(const QJsonObject& o, const ModeConfig& fallback)
{
    if (o.isEmpty()) return fallback;
    ModeConfig m;
    m.bgColor   = jsonToColor(o["bgColor"].toObject(),   fallback.bgColor);
    m.logoColor = jsonToColor(o["logoColor"].toObject(), fallback.logoColor);
    m.showLogo  = o["showLogo"].toBool(fallback.showLogo);
    m.gifPath   = o["gifPath"].toString(fallback.gifPath);
    return m;
}

// ── Public ──────────────────────────────────────────────────────────────────
QString KrakenUiPersistence::settingsFilePath()
{
    return KrakenMediaCache::rootDirectory() + QStringLiteral("/settings.json");
}

FrameConfig KrakenUiPersistence::load()
{
    FrameConfig cfg;  // constructeur applique les défauts NZXT CAM
    QString path = settingsFilePath();

    QFile f(path);
    if (!f.exists()) return cfg;
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[KrakenLCD/Persistence] Cannot open" << path;
        return cfg;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "[KrakenLCD/Persistence] Invalid JSON:" << err.errorString();
        return cfg;
    }

    QJsonObject o = doc.object();

    const int mRaw        = o["mode"].toInt(int(DisplayMode::SINGLE_INFOGRAPHIC));
    const int cfgVersion  = o["cfgVersion"].toInt(1);   // 1 = ancien enum (avant fusion GIF)
    cfg.rotation    = o["rotation"].toInt(0);
    cfg.brightness  = o["brightness"].toInt(100);
    cfg.gifPath     = o["gifPath"].toString();
    cfg.dualVertical= o["dualVertical"].toBool(true);

    // Background générique (Image/GIF, Clock, Audio)
    cfg.bgColor     = jsonToColor(o["bgColor"].toObject(), Qt::black);

    // ─── Migration ancien format → bundles par mode ─────────────────────────
    // Ancien format : bgColor, logoColor, showLogo, reading1/2/3 (partagés)
    // Nouveau format : single, dual, triple (ModeConfig) + readings par mode.
    // Si le nouveau format est absent, on initialise depuis l'ancien.
    const bool hasNewFormat = o.contains("single") || o.contains("singleReading");

    if (hasNewFormat) {
        // ─── Nouveau format : lit directement les bundles ───────────────────
        cfg.single = jsonToMode(o["single"].toObject(), cfg.single);
        cfg.dual   = jsonToMode(o["dual"].toObject(),   cfg.dual);
        cfg.triple = jsonToMode(o["triple"].toObject(), cfg.triple);

        cfg.singleReading   = jsonToReading(o["singleReading"].toObject(),   cfg.singleReading);
        cfg.dualReading1    = jsonToReading(o["dualReading1"].toObject(),    cfg.dualReading1);
        cfg.dualReading2    = jsonToReading(o["dualReading2"].toObject(),    cfg.dualReading2);
        cfg.tripleReading1  = jsonToReading(o["tripleReading1"].toObject(),  cfg.tripleReading1);
        cfg.tripleReading2  = jsonToReading(o["tripleReading2"].toObject(),  cfg.tripleReading2);
        cfg.tripleReading3  = jsonToReading(o["tripleReading3"].toObject(),  cfg.tripleReading3);
    } else {
        // ─── Ancien format : migre vers les bundles par mode ────────────────
        const QColor oldBg   = jsonToColor(o["bgColor"].toObject(),   Qt::black);
        const QColor oldLogo = jsonToColor(o["logoColor"].toObject(), Qt::white);
        const bool   oldShow = o["showLogo"].toBool(true);

        ModeConfig migrated;
        migrated.bgColor   = oldBg;
        migrated.logoColor = oldLogo;
        migrated.showLogo  = oldShow;
        cfg.single = migrated;
        cfg.dual   = migrated;
        cfg.triple = migrated;

        // Migre reading1/2/3 → bundles par mode (chaque mode reçoit les readings
        // qui le concernent ; les autres restent aux défauts NZXT).
        const ReadingConfig defS = cfg.singleReading;
        const ReadingConfig defD1 = cfg.dualReading1;
        const ReadingConfig defD2 = cfg.dualReading2;
        const ReadingConfig defT1 = cfg.tripleReading1;
        const ReadingConfig defT2 = cfg.tripleReading2;
        const ReadingConfig defT3 = cfg.tripleReading3;

        const ReadingConfig r1 = jsonToReading(o["reading1"].toObject(), defS);
        const ReadingConfig r2 = jsonToReading(o["reading2"].toObject(), defT2);
        const ReadingConfig r3 = jsonToReading(o["reading3"].toObject(), defT3);

        cfg.singleReading  = r1;
        cfg.dualReading1   = r1;  // dans l'ancien, reading1 servait aussi pour Dual
        cfg.dualReading2   = (o.contains("reading2")) ? r2 : defD2;
        cfg.tripleReading1 = r1;
        cfg.tripleReading2 = r2;
        cfg.tripleReading3 = r3;

        // En Dual, le colorViz par défaut était magenta — si l'utilisateur ne
        // l'a jamais changé, on remet les défauts NZXT (CPU violet / GPU magenta).
        if (cfg.dualReading1.colorViz == QColor(0xFF, 0x00, 0xFF))
            cfg.dualReading1.colorViz = defaultDualReading1Color();
        if (cfg.dualReading2.colorViz == QColor(0xFF, 0x00, 0xFF))
            cfg.dualReading2.colorViz = defaultDualReading2Color();
    }

    // ─── Mode : migration de l'ancien enum (modes *_GIF supprimés) ──────────
    // Ancien enum : 0 Image,1 Single,2 Single+GIF,3 Dual,4 Dual+GIF,5 Triple,
    // 6 Triple+GIF,7 Clock,8 Audio,9 NowPlaying. Nouveau : les +GIF deviennent
    // le mode de base, en héritant de l'ancien gifPath partagé.
    if (cfgVersion >= 2) {
        cfg.mode = DisplayMode(mRaw);
    } else {
        switch (mRaw) {
            case 0:  cfg.mode = DisplayMode::IMAGE_GIF; break;
            case 1:  cfg.mode = DisplayMode::SINGLE_INFOGRAPHIC; break;
            case 2:  cfg.mode = DisplayMode::SINGLE_INFOGRAPHIC;
                     cfg.single.gifPath = cfg.gifPath; break;
            case 3:  cfg.mode = DisplayMode::DUAL_INFOGRAPHIC; break;
            case 4:  cfg.mode = DisplayMode::DUAL_INFOGRAPHIC;
                     cfg.dual.gifPath = cfg.gifPath; break;
            case 5:  cfg.mode = DisplayMode::TRIPLE_INFOGRAPHIC; break;
            case 6:  cfg.mode = DisplayMode::TRIPLE_INFOGRAPHIC;
                     cfg.triple.gifPath = cfg.gifPath; break;
            case 7:  cfg.mode = DisplayMode::CLOCKFACE; break;
            case 8:  cfg.mode = DisplayMode::AUDIO_VISUAL; break;
            case 9:  cfg.mode = DisplayMode::NOW_PLAYING; break;
            default: cfg.mode = DisplayMode::SINGLE_INFOGRAPHIC; break;
        }
    }

    cfg.clockStyle   = o["clockStyle"].toInt(0);
    cfg.clockDial    = jsonToColor(o["clockDial"].toObject(), Qt::white);
    cfg.clockSeconds = jsonToColor(o["clockSeconds"].toObject(), QColor(0xAA,0x00,0xFF));
    cfg.clockArc24h       = o["clockArc24h"].toBool(cfg.clockArc24h);
    cfg.clockArcDial      = jsonToColor(o["clockArcDial"].toObject(), cfg.clockArcDial);
    cfg.clockArcText      = jsonToColor(o["clockArcText"].toObject(), cfg.clockArcText);
    cfg.clockArcStops     = jsonToStops(o["clockArcStops"].toArray(), cfg.clockArcStops);
    cfg.clockDigital24h   = o["clockDigital24h"].toBool(cfg.clockDigital24h);
    cfg.clockDigitalBg    = jsonToColor(o["clockDigitalBg"].toObject(), cfg.clockDigitalBg);
    cfg.clockDigitalStops = jsonToStops(o["clockDigitalStops"].toArray(), cfg.clockDigitalStops);

    cfg.audioStops      = jsonToStops(o["audioStops"].toArray(), cfg.audioStops);
    cfg.audioInvert     = o["audioInvert"].toBool(false);
    cfg.audioConnected  = o["audioConnected"].toBool(false);
    cfg.audioLogoColor  = jsonToColor(o["audioLogoColor"].toObject(), Qt::white);
    cfg.audioShowLogo   = o.contains("audioShowLogo")
                            ? o["audioShowLogo"].toBool(true)
                            : o["showLogo"].toBool(true);  // fallback ancien
    cfg.audioSensitivity= float(o["audioSensitivity"].toDouble(0.5556));   // défaut -80 dB

    // Now Playing : fond propre (migration : hérite de l'ancien bgColor partagé)
    cfg.npBgColor       = jsonToColor(o["npBgColor"].toObject(),       cfg.bgColor);
    // Now Playing : couleurs unies artiste / titre (fill + contour)
    cfg.npArtistFill    = jsonToColor(o["npArtistFill"].toObject(),    cfg.npArtistFill);
    cfg.npArtistOutline = jsonToColor(o["npArtistOutline"].toObject(), cfg.npArtistOutline);
    cfg.npTitleFill     = jsonToColor(o["npTitleFill"].toObject(),     cfg.npTitleFill);
    cfg.npTitleOutline  = jsonToColor(o["npTitleOutline"].toObject(),  cfg.npTitleOutline);
    cfg.npShowProgress  = o["npShowProgress"].toBool(true);

    // ─── Restauration des couleurs récentes (max 5) ─────────────────────────────────
    if (o.contains("recentColors")) {
        QList<QColor> recents;
        QJsonArray arr = o["recentColors"].toArray();
        for (const auto& v : arr) recents.append(jsonToColor(v.toObject(), Qt::white));
        kSetColorHistory(recents);
    }
    if (o.contains("recentGradients")) {
        QList<GradientStops> grads;
        QJsonArray arr = o["recentGradients"].toArray();
        for (const auto& v : arr) {
            QJsonArray stopsArr = v.toArray();
            GradientStops stops;
            for (const auto& sv : stopsArr) {
                QJsonObject so = sv.toObject();
                stops.append({ float(so["o"].toDouble(0)),
                               jsonToColor(so["c"].toObject(), Qt::white) });
            }
            if (!stops.isEmpty()) grads.append(stops);
        }
        kSetGradientHistory(grads);
    }

    qDebug() << "[KrakenLCD/Persistence] Config loaded from" << path
             << (hasNewFormat ? "(new format)" : "(migrated from old format)");
    return cfg;
}

bool KrakenUiPersistence::save(const FrameConfig& cfg)
{
    QString path = settingsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject o;
    o["mode"]         = int(cfg.mode);
    o["cfgVersion"]   = 2;     // enum sans les modes *_GIF (fond GIF par mode via gifPath)
    o["bgColor"]      = colorToJson(cfg.bgColor);
    o["rotation"]     = cfg.rotation;
    o["brightness"]   = cfg.brightness;
    o["gifPath"]      = cfg.gifPath;
    o["dualVertical"] = cfg.dualVertical;

    // ─── Bundles par mode (état indépendant) ─────────────────────────────
    o["single"] = modeToJson(cfg.single);
    o["dual"]   = modeToJson(cfg.dual);
    o["triple"] = modeToJson(cfg.triple);

    o["singleReading"]   = readingToJson(cfg.singleReading);
    o["dualReading1"]    = readingToJson(cfg.dualReading1);
    o["dualReading2"]    = readingToJson(cfg.dualReading2);
    o["tripleReading1"]  = readingToJson(cfg.tripleReading1);
    o["tripleReading2"]  = readingToJson(cfg.tripleReading2);
    o["tripleReading3"]  = readingToJson(cfg.tripleReading3);

    o["clockStyle"]   = cfg.clockStyle;
    o["clockDial"]    = colorToJson(cfg.clockDial);
    o["clockSeconds"] = colorToJson(cfg.clockSeconds);
    o["clockArc24h"]       = cfg.clockArc24h;
    o["clockArcDial"]      = colorToJson(cfg.clockArcDial);
    o["clockArcText"]      = colorToJson(cfg.clockArcText);
    o["clockArcStops"]     = stopsToJson(cfg.clockArcStops);
    o["clockDigital24h"]   = cfg.clockDigital24h;
    o["clockDigitalBg"]    = colorToJson(cfg.clockDigitalBg);
    o["clockDigitalStops"] = stopsToJson(cfg.clockDigitalStops);

    o["audioStops"]       = stopsToJson(cfg.audioStops);
    o["audioInvert"]      = cfg.audioInvert;
    o["audioConnected"]   = cfg.audioConnected;
    o["audioLogoColor"]   = colorToJson(cfg.audioLogoColor);
    o["audioShowLogo"]    = cfg.audioShowLogo;
    o["audioSensitivity"] = double(cfg.audioSensitivity);

    o["npBgColor"]       = colorToJson(cfg.npBgColor);
    o["npArtistFill"]    = colorToJson(cfg.npArtistFill);
    o["npArtistOutline"] = colorToJson(cfg.npArtistOutline);
    o["npTitleFill"]     = colorToJson(cfg.npTitleFill);
    o["npTitleOutline"]  = colorToJson(cfg.npTitleOutline);
    o["npShowProgress"]  = cfg.npShowProgress;

    // ─── Sauvegarde des couleurs récentes (custom + presets pickés) ─────────────────
    {
        QJsonArray arr;
        for (const auto& c : kColorHistory()) arr.append(colorToJson(c));
        o["recentColors"] = arr;
    }
    {
        QJsonArray arr;
        for (const auto& g : kGradientHistory()) {
            QJsonArray stopsArr;
            for (const auto& s : g) {
                stopsArr.append(QJsonObject{
                    {"o", double(s.offset)},
                    {"c", colorToJson(s.color)}
                });
            }
            arr.append(stopsArr);
        }
        o["recentGradients"] = arr;
    }

    // QSaveFile : écriture atomique (temp + rename). Un crash/coupure pendant
    // l'écriture ne peut plus corrompre settings.json.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[KrakenLCD/Persistence] Cannot write" << path;
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        qWarning() << "[KrakenLCD/Persistence] Commit failed for" << path;
        return false;
    }
    return true;
}

} // namespace NZXTKraken
