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

static constexpr int LCD_W = 640;
static constexpr int LCD_H = 640;
static constexpr int LCD_R = 320;

struct GradStop {
    float  offset = 0.f;
    QColor color  = Qt::white;
};
using GradientStops = QList<GradStop>;

inline const QList<QColor> SWATCH_FILL_15 = {
    QColor("#FF0000"), QColor("#FF5000"), QColor("#FF9600"), QColor("#FFE500"),
    QColor("#C8FF00"), QColor("#28FF00"), QColor("#3E9F2D"), QColor("#00FFB4"),
    QColor("#0000FF"), QColor("#A900FF"), QColor("#FF64FF"), QColor("#FF0050"),
    QColor("#FFFFFF"), QColor("#000000"), QColor("#6A6767")
};

inline const QList<QColor> SWATCH_OUTLINE_15 = {
    QColor(0, 0, 0, 0),
    QColor("#FFFFFF"), QColor("#000000"), QColor("#6A6767"),
    QColor("#FF0000"), QColor("#FF5000"), QColor("#FF9600"),
    QColor("#FFE500"), QColor("#C8FF00"), QColor("#28FF00"),
    QColor("#3E9F2D"), QColor("#00FFB4"), QColor("#0000FF"),
    QColor("#A900FF"), QColor("#FF0050")
};

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

struct ReadingConfig {
    SensorType type = SensorType::CPU_TEMP;

    QColor numberFill    { Qt::white };
    QColor numberOutline { 0, 0, 0, 0 };

    QColor textFill      { Qt::white };
    QColor textOutline   { 0, 0, 0, 0 };

    QColor colorViz      { 0xFF, 0x00, 0xFF };

    GradientStops vizStops;
};

struct ModeConfig {
    QColor  bgColor   { Qt::black };
    QColor  logoColor { Qt::white };
    bool    showLogo  = true;
    QString gifPath;
};

enum class DisplayMode {
    IMAGE_GIF,
    SINGLE_INFOGRAPHIC,
    DUAL_INFOGRAPHIC,
    TRIPLE_INFOGRAPHIC,
    CLOCKFACE,
    AUDIO_VISUAL,
    NOW_PLAYING,
};

inline GradientStops defaultSingleVizStops() {
    return GRADIENT_PRESETS_15.value(0);
}
inline GradientStops defaultTripleVizStops() {
    return GradientStops{
        { 0.f,   QColor("#00b4ff") },
        { 100.f, QColor("#f100ff") }
    };
}
inline QColor defaultDualReading1Color() {
    return QColor(0x7C, 0x3A, 0xED);
}
inline QColor defaultDualReading2Color() {
    return QColor(0xE0, 0x40, 0xFB);
}

inline GradientStops defaultVizStops() {
    return defaultSingleVizStops();
}

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
inline GradientStops defaultClockArcStops() {
    return GradientStops{ {0.f, QColor("#6B00DE")}, {100.f, QColor("#00B4FF")} };
}
inline GradientStops defaultClockDigitalStops() {
    return GradientStops{ {0.f, QColor("#9000ff")}, {100.f, QColor("#ff9f00")} };
}

struct FrameConfig {
    FrameConfig() {
        singleReading.type     = SensorType::CPU_TEMP;
        singleReading.vizStops = defaultSingleVizStops();

        dualReading1.type      = SensorType::CPU_TEMP;
        dualReading1.colorViz  = defaultDualReading1Color();
        dualReading2.type      = SensorType::GPU_TEMP;
        dualReading2.colorViz  = defaultDualReading2Color();

        tripleReading1.type     = SensorType::GPU_TEMP;
        tripleReading1.vizStops = defaultTripleVizStops();
        tripleReading2.type     = SensorType::CPU_TEMP;
        tripleReading3.type     = SensorType::LIQUID_TEMP;

        clockArcStops     = defaultClockArcStops();
        clockDigitalStops = defaultClockDigitalStops();

        audioStops        = defaultVizStops();
    }

    DisplayMode mode      = DisplayMode::SINGLE_INFOGRAPHIC;
    int         rotation  = 0;
    int         brightness = 100;
    QString     gifPath;
    bool        dualVertical = true;

    QColor      bgColor   { Qt::black };

    ModeConfig  single;
    ModeConfig  dual;
    ModeConfig  triple;

    ReadingConfig singleReading;
    ReadingConfig dualReading1, dualReading2;
    ReadingConfig tripleReading1, tripleReading2, tripleReading3;

    int         clockStyle = 0;

    QColor      clockDial     { Qt::white };
    QColor      clockSeconds  { 0xAA, 0x00, 0xFF };

    bool          clockArc24h    = true;
    QColor        clockArcDial   { Qt::white };
    QColor        clockArcText   { Qt::white };
    GradientStops clockArcStops;

    bool          clockDigital24h = false;
    QColor        clockDigitalBg  { Qt::black };
    GradientStops clockDigitalStops;

    GradientStops audioStops;
    bool        audioInvert    = false;
    bool        audioConnected = false;
    bool        audioShowLogo  = true;
    QColor      audioLogoColor { Qt::white };
    float       audioSensitivity = 0.5556f;

    QColor      npBgColor       { Qt::black };
    QColor      npArtistFill    { Qt::white };
    QColor      npArtistOutline { 0, 0, 0, 0 };
    QColor      npTitleFill     { Qt::white };
    QColor      npTitleOutline  { 0, 0, 0, 0 };
    bool        npShowProgress  = true;
};

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
        const float pos = sweepFrac * (1.f - t_offset);
        cg.setColorAt(std::clamp(pos, 0.f, 1.f), s.color);
    }

    if (!sorted.isEmpty()) {
        const QColor firstColor = sorted.first().color;
        const QColor lastColor  = sorted.last().color;
        const float capPad = 0.03f;
        cg.setColorAt(std::clamp(sweepFrac + capPad, 0.f, 0.999f), firstColor);
        cg.setColorAt(std::clamp(1.f - capPad, 0.f, 1.f), lastColor);
        cg.setColorAt(1.f, lastColor);
    }
    return cg;
}

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

class FrameRenderer {
public:
    explicit FrameRenderer(SystemSensors* sensors);
    ~FrameRenderer();

    QImage render(const FrameConfig& cfg);

    static QByteArray toJpeg(const QImage& img, int quality = 90);
    static QByteArray toQ565(const QImage& img);

    void setAudioLevels(const QVector<float>& bands);

    static void setFahrenheit(bool on);
    static bool fahrenheit();

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

    void circularClip(QPainter& p);
    QString formatValue(float v, SensorType t) const;
    QString formatValue(const SensorValue& sv, SensorType t) const;
    QString getShortLabel(SensorType t) const;

    static const ModeConfig& activeMode(const FrameConfig& cfg);

    std::unique_ptr<QMovie> loadGif(const QString& path);
    std::map<std::string, std::unique_ptr<QMovie>> m_gifCache;
    std::map<std::string, QImage>                  m_imageCache;

    QString        m_lastMediaPath;
    QMovie*        m_lastGif       = nullptr;
    const QImage*  m_lastStaticImg = nullptr;

    QPixmap        m_gifScaled;
    int            m_gifScaledNo = -1;

    QVector<float> m_audioBands;
    QMutex         m_audioMutex;

    MediaSnapshot  m_media;
    qint64         m_npScrollAnchorMs = 0;
    QString        m_npScrollKey;
    SystemSensors* m_sensors;
};

} // namespace NZXTKraken
