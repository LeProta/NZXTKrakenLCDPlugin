#pragma once
#include <QImage>
#include <QPainter>
#include <QColor>
#include <QString>
#include <QFont>
#include <QGradient>
#include <QLinearGradient>
#include <QConicalGradient>
#include <QPixmap>
#include <QMovie>
#include <QMutex>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <memory>
#include <map>
#include <string>
#include <cmath>
#include <algorithm>
#include "../sensors/SystemSensors.h"
#include "../sensors/MediaSession.h"

namespace NZXTKraken {

// ─── Taille de l'écran ────────────────────────────────────────────────────────
static constexpr int LCD_W = 640;
static constexpr int LCD_H = 640;
static constexpr int LCD_R = 320;

// ─── Stop de gradient (offset 0-100 + couleur) ───────────────────────────────
struct GradStop {
    float  offset = 0.f;
    QColor color  = Qt::white;
};
using GradientStops = QList<GradStop>;

// ─── 15 swatches officiels (Background / Logo / Number-fill / Text-fill / Viz solid) ─
inline const QList<QColor> SWATCH_FILL_15 = {
    QColor("#FF0000"), QColor("#FF5000"), QColor("#FF9600"), QColor("#FFE500"),
    QColor("#C8FF00"), QColor("#28FF00"), QColor("#3E9F2D"), QColor("#00FFB4"),
    QColor("#0000FF"), QColor("#A900FF"), QColor("#FF64FF"), QColor("#FF0050"),
    QColor("#FFFFFF"), QColor("#000000"), QColor("#6A6767")
};

// ─── 15 swatches d'outline (premier = transparent, alpha=0) ──────────────────
inline const QList<QColor> SWATCH_OUTLINE_15 = {
    QColor(0, 0, 0, 0),                                  // transparent / aucun contour
    QColor("#FFFFFF"), QColor("#000000"), QColor("#6A6767"),
    QColor("#FF0000"), QColor("#FF5000"), QColor("#FF9600"),
    QColor("#FFE500"), QColor("#C8FF00"), QColor("#28FF00"),
    QColor("#3E9F2D"), QColor("#00FFB4"), QColor("#0000FF"),
    QColor("#A900FF"), QColor("#FF0050")
};

// ─── 15 presets de dégradés officiels (Visualization Single & Triple) ───────
inline const QList<GradientStops> GRADIENT_PRESETS_15 = {
    { {0, QColor("#00ff00")}, {50, QColor("#ffff00")}, {100, QColor("#ff0000")} },
    { {0, QColor("#6B00DE")}, {100, QColor("#D600BF")} },
    { {0, QColor("#fafafa")}, {100, QColor("#000000")} },
    { {0, QColor("#225e27")}, {50, QColor("#f6b904")}, {100, QColor("#922a2a")} },
    { {0, QColor("#00b4ff")}, {100, QColor("#00ff1e")} },
    { {0, QColor("#00fff7")}, {100, QColor("#fa00ff")} },
    { {0, QColor("#2400ff")}, {50, QColor("#bb00ff")}, {100, QColor("#ff0076")} },
    { {0, QColor("#00ff46")}, {50, QColor("#0e00ff")}, {100, QColor("#a500ff")} },
    { {0, QColor("#00d4ff")}, {50, QColor("#00ff06")}, {100, QColor("#ffac00")} },
    { {0, QColor("#1bdf29")}, {100, QColor("#242198")} },
    { {0, QColor("#19ff00")}, {100, QColor("#ff0064")} },
    { {0, QColor("#0021ff")}, {100, QColor("#ffb000")} },
    { {0, QColor("#0000ff")}, {100, QColor("#ff0000")} },
    { {0, QColor("#00ffff")}, {33, QColor("#00ff41")}, {66, QColor("#ffbe00")}, {100, QColor("#ff4600")} },
    { {0, QColor("#00ff3c")}, {33, QColor("#00ff41")}, {66, QColor("#0000ff")}, {100, QColor("#7800ff")} }
};

// ─── Config d'un capteur pour un mode infographic ─────────────────────────────
// Note : outline avec alpha=0 → pas de contour dessiné (transparent).
struct ReadingConfig {
    SensorType type = SensorType::CPU_TEMP;

    // [Number] valeur numérique : remplissage + contour
    QColor numberFill    { Qt::white };
    QColor numberOutline { 0, 0, 0, 0 };

    // [Text] label : remplissage + contour
    QColor textFill      { Qt::white };
    QColor textOutline   { 0, 0, 0, 0 };

    // [Dual] jauge couleur unie
    QColor colorViz      { 0xFF, 0x00, 0xFF };

    // [Single / Triple] jauge dégradé multi-points
    GradientStops vizStops;
};

// ─── Bundle de config par mode (état indépendant Single/Dual/Triple) ─────────
// Chaque mode infographic a son propre background, couleur de logo et toggle.
// Modifier l'un n'affecte pas l'autre.
struct ModeConfig {
    QColor  bgColor   { Qt::black };
    QColor  logoColor { Qt::white };
    bool    showLogo  = true;
    QString gifPath;             // fond image/GIF de ce mode (par-dessus bgColor) ; vide = couleur unie seule
};

// ─── Modes d'affichage ────────────────────────────────────────────────────────
enum class DisplayMode {
    IMAGE_GIF,
    SINGLE_INFOGRAPHIC,
    DUAL_INFOGRAPHIC,
    TRIPLE_INFOGRAPHIC,
    CLOCKFACE,
    AUDIO_VISUAL,
    NOW_PLAYING,
};

// ─── Helpers de défauts NZXT CAM ─────────────────────────────────────────────
// Single Infographic : gradient vert → jaune → rouge (preset #0, classique CAM)
inline GradientStops defaultSingleVizStops() {
    return GRADIENT_PRESETS_15.value(0);
}
// Triple Infographic : gradient cyan → magenta (stops custom hors presets,
// d'après le défaut NZXT CAM officiel #00b4ff → #f100ff)
inline GradientStops defaultTripleVizStops() {
    return GradientStops{
        { 0.f,   QColor("#00b4ff") },
        { 100.f, QColor("#f100ff") }
    };
}
// Dual Infographic : couleurs unies NZXT CPU (violet) et GPU (magenta)
inline QColor defaultDualReading1Color() {
    return QColor(0x7C, 0x3A, 0xED);  // NZXT_CPU
}
inline QColor defaultDualReading2Color() {
    return QColor(0xE0, 0x40, 0xFB);  // NZXT_GPU
}

// Backward-compat : ancien helper utilisé un peu partout — équivaut à Single.
inline GradientStops defaultVizStops() {
    return defaultSingleVizStops();
}

// ─── Presets de dégradés spécifiques à l'HORLOGE (Clockface 2 & 3) ────────────
// Différents de Single/Triple. 15/15 (positions régulières, mesurées sur les captures).
inline const QList<GradientStops> CLOCK_GRADIENT_PRESETS = {
    { {0, QColor("#9000ff")}, {100, QColor("#ff9f00")} },
    { {0, QColor("#00ff00")}, {50, QColor("#ffff00")}, {100, QColor("#ff0000")} },
    { {0, QColor("#f100ff")}, {100, QColor("#ffae98")} },
    { {0, QColor("#f12711")}, {100, QColor("#fff800")} },
    { {0, QColor("#00b4ff")}, {100, QColor("#00ff1e")} },
    { {0, QColor("#e3ff00")}, {100, QColor("#00ff00")} },
    { {0, QColor("#2400ff")}, {25, QColor("#bb00ff")}, {50, QColor("#ff0076")}, {75, QColor("#bb00ff")}, {100, QColor("#2400ff")} },
    { {0, QColor("#a500ff")}, {50, QColor("#0e00ff")}, {100, QColor("#00ff46")} },
    { {0, QColor("#00d4ff")}, {50, QColor("#00ff06")}, {100, QColor("#ffac00")} },
    { {0, QColor("#00b4ff")}, {100, QColor("#f100ff")} },
    { {0, QColor("#19ff00")}, {100, QColor("#ff0064")} },
    { {0, QColor("#00ffd3")}, {100, QColor("#0d00ff")} },
    { {0, QColor("#ff0000")}, {100, QColor("#0000ff")} },
    { {0, QColor("#00ff3c")}, {33, QColor("#00ffff")}, {67, QColor("#0000ff")}, {100, QColor("#7800ff")} },
    { {0, QColor("#D0021B")}, {17, QColor("#FAA50B")}, {33, QColor("#F8D41C")}, {50, QColor("#C4E335")}, {67, QColor("#21D373")}, {83, QColor("#0076FF")}, {100, QColor("#51007A")} },
};
inline GradientStops defaultClockArcStops() {     // Clockface 2 : violet → bleu
    return GradientStops{ {0.f, QColor("#6B00DE")}, {100.f, QColor("#00B4FF")} };
}
inline GradientStops defaultClockDigitalStops() { // Clockface 3 : 1er preset (violet → orange)
    return GradientStops{ {0.f, QColor("#9000ff")}, {100.f, QColor("#ff9f00")} };
}

// ─── Config globale du frame ─────────────────────────────────────────────────
// Refactor : les modes infographic (Single, Dual, Triple) ont chacun leur propre
// bundle indépendant. Modifier le gradient du mode Single n'affecte plus Triple.
struct FrameConfig {
    FrameConfig() {
        // Défauts NZXT CAM appliqués au constructeur (pas via member init
        // pour garantir l'ordre d'initialisation des QList globales).
        singleReading.type     = SensorType::CPU_TEMP;
        singleReading.vizStops = defaultSingleVizStops();

        dualReading1.type      = SensorType::CPU_TEMP;
        dualReading1.colorViz  = defaultDualReading1Color();
        dualReading2.type      = SensorType::GPU_TEMP;
        dualReading2.colorViz  = defaultDualReading2Color();

        tripleReading1.type     = SensorType::GPU_TEMP;      // Primary (défaut)
        tripleReading1.vizStops = defaultTripleVizStops();
        tripleReading2.type     = SensorType::CPU_TEMP;      // Secondary 1 (défaut)
        tripleReading3.type     = SensorType::LIQUID_TEMP;   // Secondary 2 (défaut)

        clockArcStops     = defaultClockArcStops();
        clockDigitalStops = defaultClockDigitalStops();

        audioStops        = defaultVizStops();
    }

    DisplayMode mode      = DisplayMode::SINGLE_INFOGRAPHIC;
    int         rotation  = 0;
    int         brightness = 100;
    QString     gifPath;
    bool        dualVertical = true;

    // Background pour modes qui n'ont pas leur propre ModeConfig (IMAGE_GIF,
    // CLOCKFACE, AUDIO_VISUAL utilisent celui-ci ; les infographic l'ignorent).
    QColor      bgColor   { Qt::black };

    // ─── Bundles par mode infographic — état indépendant ────────────────────
    ModeConfig  single;
    ModeConfig  dual;
    ModeConfig  triple;

    // ─── Readings par mode (plus de partage entre Single et Triple) ─────────
    ReadingConfig singleReading;
    ReadingConfig dualReading1, dualReading2;
    ReadingConfig tripleReading1, tripleReading2, tripleReading3;

    // ─── Clockface ──────────────────────────────────────────────────────────
    int         clockStyle = 0;     // 0 = Analog points, 1 = Arc gradient, 2 = Digital

    // Style 0 — Analogique à points (couleurs unies ; fond = bgColor)
    QColor      clockDial     { Qt::white };        // points + aiguilles H/M
    QColor      clockSeconds  { 0xAA, 0x00, 0xFF }; // aiguille des secondes

    // Style 1 — Arc dégradé
    bool          clockArc24h    = true;            // format 12/24 h
    QColor        clockArcDial   { Qt::white };      // aiguille (heures)
    QColor        clockArcText   { Qt::white };      // texte hh:mm
    GradientStops clockArcStops;                     // dégradé conique plein écran

    // Style 2 — Digital
    bool          clockDigital24h = false;          // format 12/24 h
    QColor        clockDigitalBg  { Qt::black };     // fond
    GradientStops clockDigitalStops;                 // dégradé vertical du texte

    // ─── Audio Visual ───────────────────────────────────────────────────────
    GradientStops audioStops;                        // dégradé (mêmes presets que single/triple)
    bool        audioInvert    = false;
    bool        audioConnected = false;              // relie les pics en une forme lissée
    bool        audioShowLogo  = true;
    QColor      audioLogoColor { Qt::white };
    float       audioSensitivity = 0.5556f;          // défaut -80 dB  (s = (-30 - dB) / 90)

    // ─── Now Playing (musique) : fond propre + couleurs unies + contour ──
    // npBgColor séparé de bgColor : avant, le bouton "Background" de Now
    // Playing et celui du Clockface Analog modifiaient la même valeur.
    QColor      npBgColor       { Qt::black };
    QColor      npArtistFill    { Qt::white };
    QColor      npArtistOutline { 0, 0, 0, 0 };
    QColor      npTitleFill     { Qt::white };
    QColor      npTitleOutline  { 0, 0, 0, 0 };
    bool        npShowProgress  = true;   // barre + temps (affichés seulement si le média fournit la durée)
};

// ─── Helpers gradient ────────────────────────────────────────────────────────
// QLinearGradient horizontal (0 → 1) — utilisé pour les *aperçus* (swatches,
// éditeur), pas pour le rendu de l'arc.
inline QLinearGradient makeLinearGradient(const QRectF& rect, const GradientStops& stops)
{
    QLinearGradient g(rect.left(), 0.0, rect.right(), 0.0);
    if (stops.isEmpty()) {
        g.setColorAt(0.0, Qt::white);
        g.setColorAt(1.0, Qt::white);
        return g;
    }
    GradientStops sorted = stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradStop& a, const GradStop& b) { return a.offset < b.offset; });
    for (const auto& s : sorted)
        g.setColorAt(std::clamp(s.offset / 100.f, 0.f, 1.f), s.color);
    return g;
}

// QConicalGradient qui suit la courbe d'un arc. L'arc est défini en degrés
// horloge (0 = 12h, sens horaire positif). Le gradient s'étale le long du
// chemin de l'arc : offset 0 % = début de l'arc, offset 100 % = fin.
//
// Conversion : clockDeg → Qt conical angle = (90 - clockDeg) mod 360
// Direction : l'arc est dessiné dans le sens horaire (drawArc avec span
// négatif), donc le gradient conical doit interpoler dans le sens opposé
// (CCW Qt = sens horaire écran inversé) avec un point de départ à la fin
// de l'arc.
inline QConicalGradient makeArcConicalGradient(const QPointF& center,
                                                const GradientStops& stops,
                                                float clockStart, float clockSweep)
{
    const float clockEnd      = clockStart + clockSweep;
    const float qtStartAngle  = std::fmod(90.f - clockEnd + 7200.f, 360.f);
    const float sweepFrac     = std::clamp(clockSweep / 360.f, 0.001f, 1.f);

    QConicalGradient cg(center, qtStartAngle);
    GradientStops sorted = stops.isEmpty() ? defaultVizStops() : stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradStop& a, const GradStop& b) { return a.offset < b.offset; });
    for (const auto& s : sorted) {
        const float t_offset = std::clamp(s.offset / 100.f, 0.f, 1.f);
        // offset=0  → position sweepFrac (début d'arc, dernière étape CCW)
        // offset=100 → position 0        (fin d'arc, départ du conical)
        const float pos = sweepFrac * (1.f - t_offset);
        cg.setColorAt(std::clamp(pos, 0.f, 1.f), s.color);
    }

    // ─── Anti-couture (seam) du dégradé conique ──────────────────────────────────
    // Le conical interpole sur 360°. La portion hors-arc (le "gap", entre
    // pos=sweepFrac=début et la couture pos 1↔0=fin) n'est pas censée être
    // peinte, MAIS les RoundCap aux deux extrémités débordent légèrement dessus
    // et captent alors une couleur interpolée erronée (frange visible en bout
    // d'arc). On neutralise ça en figeant la couleur terminale de part et
    // d'autre du gap, sur une marge un peu plus large que le rayon du cap.
    if (!sorted.isEmpty()) {
        const QColor firstColor = sorted.first().color; // offset 0   → début d'arc
        const QColor lastColor  = sorted.last().color;  // offset 100 → fin d'arc
        const float capPad = 0.03f;                     // marge > portée du RoundCap
        // Côté début (pos ≈ sweepFrac) : maintient la 1ère couleur sous le cap.
        cg.setColorAt(std::clamp(sweepFrac + capPad, 0.f, 0.999f), firstColor);
        // Côté fin (couture pos 1↔0) : maintient la dernière couleur des 2 côtés.
        cg.setColorAt(std::clamp(1.f - capPad, 0.f, 1.f), lastColor);
        cg.setColorAt(1.f, lastColor);
    }
    return cg;
}

// Interpole une couleur à un offset donné (pour le dot d'extrémité d'arc).
inline QColor colorAtPercent(const GradientStops& stops, float pct)
{
    if (stops.isEmpty()) return Qt::white;
    GradientStops sorted = stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradStop& a, const GradStop& b) { return a.offset < b.offset; });
    pct = std::clamp(pct, 0.f, 100.f);
    GradStop lo = sorted.first(), hi = sorted.last();
    for (int i = 0; i < sorted.size() - 1; ++i) {
        if (pct >= sorted[i].offset && pct <= sorted[i+1].offset) {
            lo = sorted[i]; hi = sorted[i+1]; break;
        }
    }
    if (lo.offset == hi.offset) return lo.color;
    const float t = (pct - lo.offset) / (hi.offset - lo.offset);
    return QColor(int(lo.color.red()   + (hi.color.red()   - lo.color.red())   * t),
                  int(lo.color.green() + (hi.color.green() - lo.color.green()) * t),
                  int(lo.color.blue()  + (hi.color.blue()  - lo.color.blue())  * t));
}

// ─── Classe de rendu ──────────────────────────────────────────────────────────
class FrameRenderer {
public:
    explicit FrameRenderer(SystemSensors* sensors);
    ~FrameRenderer();

    QImage render(const FrameConfig& cfg);

    static QByteArray toJpeg(const QImage& img, int quality = 90);
    static QByteArray toQ565(const QImage& img);

    void setAudioLevels(const QVector<float>& bands);

    // Unité de température °C/°F : drapeau global lu au rendu (display-only).
    // L'arc/jauge utilise toujours la valeur brute en °C ; seule la valeur
    // numérique affichée est convertie. Thread-safe (atomic interne).
    static void setFahrenheit(bool on);
    static bool fahrenheit();

    // Now Playing (musique) : pousse l'instantané média courant (thread worker).
    void setMediaInfo(const MediaSnapshot& snap);

private:
    void renderBackground(QPainter& p, const FrameConfig& cfg,
                          QMovie* gif, const QImage* staticImg);
    void renderLogo(QPainter& p, const ModeConfig& mode);
    void renderSingleInfographic(QPainter& p, const FrameConfig& cfg);
    void renderDualInfographic  (QPainter& p, const FrameConfig& cfg);
    void renderTripleInfographic(QPainter& p, const FrameConfig& cfg);
    void renderClock            (QPainter& p, const FrameConfig& cfg);
    void renderAudioVisual      (QPainter& p, const FrameConfig& cfg);
    void renderNowPlaying       (QPainter& p, const FrameConfig& cfg);

    // (drawGaugeArc / drawSensorReading / drawNZXTLogo supprimes : etaient des stubs vides.)
    void circularClip(QPainter& p);
    QString formatValue(float v, SensorType t) const;
    QString formatValue(const SensorValue& sv, SensorType t) const;
    QString getShortLabel(SensorType t) const;

    // Helper pour résoudre le ModeConfig actif selon DisplayMode.
    static const ModeConfig& activeMode(const FrameConfig& cfg);

    std::unique_ptr<QMovie> loadGif(const QString& path);
    // Caches bornés a UN media (le courant) : vidés au changement de chemin.
    std::map<std::string, std::unique_ptr<QMovie>> m_gifCache;
    std::map<std::string, QImage>                  m_imageCache;

    // Memoisation de la resolution media (evite std::string + lookup map/frame).
    QString        m_lastMediaPath;
    QMovie*        m_lastGif       = nullptr;
    const QImage*  m_lastStaticImg = nullptr;

    // Frame GIF courante deja mise a l'echelle LCD (memoisee par n° de frame :
    // le rendu tourne plus vite que le GIF, inutile de re-scaler a chaque tick).
    QPixmap        m_gifScaled;
    int            m_gifScaledNo = -1;

    QVector<float> m_audioBands;
    QMutex         m_audioMutex;

    MediaSnapshot  m_media;       // dernier instantané "now playing" (pushé par le worker)
    qint64         m_npScrollAnchorMs = 0;   // ancre temps du défilement Now Playing (reset au changement de piste)
    QString        m_npScrollKey;            // clé piste courante (artiste|titre) pour détecter le changement
    SystemSensors* m_sensors;
};

} // namespace NZXTKraken
