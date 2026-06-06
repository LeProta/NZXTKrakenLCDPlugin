#include "FrameRenderer.h"
#include "../device/q565.h"
#include <QPainterPath>
#include <QDateTime>
#include <QBuffer>
#include <QMutexLocker>
#include <QFileInfo>
#include <QImageReader>
#include <QFontMetricsF>
#include <QFontInfo>
#include <QConicalGradient>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace NZXTKraken {

// ─── Polices héritées (utilisées pour CLOCKFACE et AUDIO_VISUAL) ─────────────
static const QFont FONT_NZXT  ("Arial",     1);

// ─── Design tokens NZXT (d'après nzxt-cam-lcd.jsx) ───────────────────────────
static const QColor NZXT_WHITE (0xFF, 0xFF, 0xFF);
static const QColor NZXT_TRACK (0x1E, 0x1E, 0x22);
static const QColor NZXT_CPU   (0x7C, 0x3A, 0xED);
static const QColor NZXT_GPU   (0xE0, 0x40, 0xFB);

// ─── Angles d'arc partagés Mode 1 & 3 (clock degrees : 0 = 12h) ──────────────
static constexpr float ARC_START   = 220.f;
static constexpr float ARC_SWEEP   = 280.f;
static constexpr float ARC_END     = ARC_START + ARC_SWEEP;
static constexpr float ARC_GAP_DEG = 10.f;

// ─── Fabrique de police (Barlow → fallback Segoe UI) ─────────────────────────
static QFont nzxtFont(int px, QFont::Weight w = QFont::Medium)
{
    // Resolution Barlow -> fallback Segoe UI faite UNE seule fois (QFontInfo
    // interroge la base de polices ; evite des centaines de lookups/s a 60 fps).
    static const QString family = []{
        return QFontInfo(QFont(QStringLiteral("Barlow"))).exactMatch()
                   ? QStringLiteral("Barlow")
                   : QStringLiteral("Segoe UI");
    }();
    QFont f(family);
    f.setPixelSize(px);
    f.setWeight(w);
    return f;
}

// ─── Géométrie ───────────────────────────────────────────────────────────────
static inline QPointF clockToCart(qreal cx, qreal cy, qreal r, qreal clockDeg)
{
    const qreal rad = clockDeg * M_PI / 180.0;
    return QPointF(cx + r * std::sin(rad), cy - r * std::cos(rad));
}
static inline void clockArcToQt(qreal clockStart, qreal clockEnd,
                                int& outStart16, int& outSpan16)
{
    outStart16 = int((90.0 - clockStart) * 16.0);
    outSpan16  = int((clockStart - clockEnd) * 16.0);
}
static void drawClockArc(QPainter& p, const QRectF& rect,
                         qreal clockStart, qreal clockEnd,
                         const QColor& color, qreal thickness)
{
    if (clockEnd <= clockStart + 0.5) return;
    QPen pen(color, thickness, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    int start16, span16;
    clockArcToQt(clockStart, clockEnd, start16, span16);
    p.drawArc(rect, start16, span16);
}
// Variante avec un QBrush quelconque (gradient ou solid) en stroke
static void drawClockArcBrush(QPainter& p, const QRectF& rect,
                              qreal clockStart, qreal clockEnd,
                              const QBrush& brush, qreal thickness)
{
    if (clockEnd <= clockStart + 0.5) return;
    QPen pen(brush, thickness, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    int start16, span16;
    clockArcToQt(clockStart, clockEnd, start16, span16);
    p.drawArc(rect, start16, span16);
}

enum class UnitKind  { Temp, Load, ClockMHz, ClockGHz, None };
enum class TextAlign { Start, Middle, End };

// Unité de température : écrite depuis l'UI (LanguageChange), lue au rendu (thread worker) -> atomic. L'arc/jauge reste sur la valeur brute en °C ; seul le NOMBRE affiché est converti.
static std::atomic<bool> g_fahrenheit{false};

static inline float maybeToFahrenheit(float v, SensorType t)
{
    const bool isTemp = (t == SensorType::CPU_TEMP || t == SensorType::GPU_TEMP
                      || t == SensorType::LIQUID_TEMP);
    if (isTemp && g_fahrenheit.load(std::memory_order_relaxed))
        return v * 9.0f / 5.0f + 32.0f;
    return v;
}

static UnitKind unitKindFor(SensorType t)
{
    switch (t) {
        case SensorType::CPU_TEMP:
        case SensorType::GPU_TEMP:
        case SensorType::LIQUID_TEMP:  return UnitKind::Temp;
        case SensorType::CPU_LOAD:
        case SensorType::GPU_LOAD:
        case SensorType::MEM_LOAD:     return UnitKind::Load;
        case SensorType::CPU_CLOCK:
        case SensorType::GPU_CLOCK:    return UnitKind::ClockMHz;
        default:                        return UnitKind::None;
    }
}
static float sensorPercent(const SensorValue& sv, SensorType t)
{
    if (!sv.available) return 0.f;
    const float maxVal = (t == SensorType::CPU_CLOCK || t == SensorType::GPU_CLOCK)
                         ? 5000.f : 100.f;
    return std::clamp(sv.value / maxVal * 100.f, 0.f, 100.f);
}
static QString formatNumber(const SensorValue& sv, SensorType t, bool ghz)
{
    if (!sv.available) return QStringLiteral("--");
    if (ghz && (t == SensorType::CPU_CLOCK || t == SensorType::GPU_CLOCK))
        return QString::number(sv.value / 1000.f, 'f', 1);
    return QString::number(int(maybeToFahrenheit(sv.value, t) + 0.5f));
}

// ─── Dessin texte avec fill + outline (paint-order: stroke puis fill) ────────
// Si outline.alpha() == 0, équivaut à un drawText simple (chemin rapide).
static void drawStyledText(QPainter& p, const QRectF& rect, int alignFlags,
                           const QString& text, const QFont& font,
                           const QColor& fill, const QColor& outline)
{
    if (text.isEmpty()) return;

    if (outline.alpha() == 0) {
        p.setFont(font);
        p.setPen(fill);
        p.drawText(rect, alignFlags, text);
        return;
    }

    p.setFont(font);
    QFontMetricsF fm(font);
    const qreal tw = fm.horizontalAdvance(text);
    qreal x, baseline;

    if (alignFlags & Qt::AlignLeft)         x = rect.left();
    else if (alignFlags & Qt::AlignRight)   x = rect.right() - tw;
    else                                     x = rect.center().x() - tw / 2.0;

    if (alignFlags & Qt::AlignTop)          baseline = rect.top() + fm.ascent();
    else if (alignFlags & Qt::AlignBottom)  baseline = rect.bottom() - fm.descent();
    else                                     baseline = rect.center().y() + (fm.ascent() - fm.descent()) / 2.0;

    QPainterPath path;
    path.addText(x, baseline, font, text);

    // Stroke d'abord (sous le fill), épaisseur proportionnelle au rayon
    const qreal strokeW = LCD_R * 0.006;
    QPen pen(outline, strokeW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    pen.setMiterLimit(2);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Fill par-dessus
    p.fillPath(path, fill);
}

// ─── Horloge monotone (ms) pour les animations (défilement Now Playing) ──────
static qint64 animNowMs()
{
    using namespace std::chrono;
    return (qint64) duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

// ─── Texte défilant en va-et-vient (marquee bidirectionnel) ──────────────────
// Si le texte tient dans viewportW → centré (statique). Sinon : pause au début (1er mot visible), défilement jusqu'au dernier mot, pause, retour au 1er mot, puis la boucle recommence. Le rendu est découpé (clip) à la fenêtre visible pour ne jamais déborder du cercle. elapsedMs = temps écoulé depuis l'ancre.
static void drawScrollingText(QPainter& p, qreal cx, qreal centerY, qreal bandH,
                              qreal viewportW, const QString& text, const QFont& font,
                              const QColor& fill, const QColor& outline, qint64 elapsedMs)
{
    if (text.isEmpty() || viewportW <= 1.0) return;

    const QFontMetricsF fm(font);
    const qreal textW  = fm.horizontalAdvance(text);
    const qreal vpLeft = cx - viewportW / 2.0;
    const qreal topY   = centerY - bandH / 2.0;

    // Tient dans la largeur → centré, pas de défilement.
    if (textW <= viewportW) {
        drawStyledText(p, QRectF(vpLeft, topY, viewportW, bandH),
                       Qt::AlignHCenter | Qt::AlignVCenter, text, font, fill, outline);
        return;
    }

    const qreal  overflow = textW - viewportW;
    const qreal  speed    = LCD_R * 0.18;                       // ~58 px/s sur le canvas 640 (vitesse constante)
    const qint64 pauseMs  = 1300;                               // pause à chaque extrémité
    const qint64 scrollMs = (qint64)(1000.0 * overflow / std::max(1.0, speed));
    const qint64 cycle    = pauseMs + scrollMs + pauseMs + scrollMs;

    const qint64 t = (cycle > 0) ? (elapsedMs % cycle) : 0;
    qreal off;
    if (t < pauseMs) {
        off = 0.0;                                              // début (1er mot)
    } else if (t < pauseMs + scrollMs) {
        const qreal u = double(t - pauseMs) / double(std::max<qint64>(1, scrollMs));
        off = u * overflow;                                     // → vitesse constante vers le dernier mot
    } else if (t < pauseMs + scrollMs + pauseMs) {
        off = overflow;                                         // fin (dernier mot)
    } else {
        const qreal u = double(t - pauseMs - scrollMs - pauseMs) / double(std::max<qint64>(1, scrollMs));
        off = (1.0 - u) * overflow;                             // ← retour au 1er mot (vitesse constante)
    }

    p.save();
    p.setClipRect(QRectF(vpLeft, topY, viewportW, bandH), Qt::IntersectClip);
    drawStyledText(p, QRectF(vpLeft - off, topY, textW + 8.0, bandH),
                   Qt::AlignLeft | Qt::AlignVCenter, text, font, fill, outline);
    p.restore();
}

// ─── Dessin valeur + unité, version étendue (fill + outline) ─────────────────
// Si forceShowUnit == false, l'unité est masquée — utile quand le capteur n'a pas encore chargé (valeur "--") pour éviter d'afficher "% / °C / GHz".
static void drawMetric(QPainter& p, qreal x, qreal y,
                       const QString& numStr,
                       int numPx, int unitPx,
                       const QColor& fillColor,
                       const QColor& outlineColor,
                       UnitKind unit, TextAlign align,
                       bool forceShowUnit = true)
{
    QFont fNum = nzxtFont(numPx);
    QFontMetricsF fmNum(fNum);
    const qreal numW = fmNum.horizontalAdvance(numStr);

    QString unitStr;
    int     unitActualPx = unitPx;
    bool    unitOnTop    = false;
    bool    unitOnBot    = false;

    if (forceShowUnit) {
        switch (unit) {
            case UnitKind::Temp:
                unitStr = QString(QChar(0x00B0)); unitOnTop = true; break;
            case UnitKind::Load:
                unitStr = QStringLiteral("%"); unitOnBot = true;
                unitActualPx = int(unitPx * 0.85f); break;
            case UnitKind::ClockGHz:
                unitStr = QStringLiteral("GHz"); unitOnTop = true;
                unitActualPx = int(unitPx * 0.85f); break;
            case UnitKind::ClockMHz:
            case UnitKind::None:
                break;
        }
    }

    QFont fUnit = nzxtFont(unitActualPx);
    QFontMetricsF fmU(fUnit);
    const qreal unitW = unitStr.isEmpty() ? 0.0 : fmU.horizontalAdvance(unitStr);
    const qreal SYMBOL_GAP = LCD_R * 0.008;

    qreal numX = x, unitX = 0;
    switch (align) {
        case TextAlign::Start:
            numX  = x;
            unitX = numX + numW + SYMBOL_GAP;
            break;
        case TextAlign::End:
            if (!unitStr.isEmpty()) {
                unitX = x;
                numX  = x - SYMBOL_GAP - numW;
            } else {
                numX = x - numW;
            }
            break;
        case TextAlign::Middle:
        default: {
            // Centre uniquement les chiffres ; l'unité déborde à droite.
            numX  = x - numW / 2.0;
            unitX = numX + numW + SYMBOL_GAP;
            break;
        }
    }

    // Nombre
    QRectF numRect(numX, y - numPx, numW + 2, numPx * 2.0);
    drawStyledText(p, numRect, Qt::AlignLeft | Qt::AlignVCenter,
                   numStr, fNum, fillColor, outlineColor);

    // Unité
    if (!unitStr.isEmpty()) {
        qreal unitY = y;
        if (unitOnTop) unitY = y - numPx * 0.30;
        if (unitOnBot) unitY = y + numPx * 0.30;
        QRectF unitRect(unitX, unitY - unitActualPx, unitW + 4, unitActualPx * 2.0);
        drawStyledText(p, unitRect, Qt::AlignLeft | Qt::AlignVCenter,
                       unitStr, fUnit, fillColor, outlineColor);
    }
}

// ─── GIF helpers ─────────────────────────────────────────────────────────────
static bool isGifFile(const QString& path)
{
    return path.endsWith(".gif", Qt::CaseInsensitive);
}

// ═════════════════════════════════════════════════════════════════════════════
FrameRenderer::FrameRenderer(SystemSensors* sensors) : m_sensors(sensors)
{
    m_audioBands.fill(0.f, 64);
}
FrameRenderer::~FrameRenderer() = default;

// Helper : retourne le ModeConfig actif pour les modes infographic.
// Pour les autres modes (Image/GIF, Clock, Audio), retourne single par défaut (mais ces modes utilisent cfg.bgColor explicitement, pas activeMode).
const ModeConfig& FrameRenderer::activeMode(const FrameConfig& cfg)
{
    switch (cfg.mode) {
        case DisplayMode::SINGLE_INFOGRAPHIC: return cfg.single;
        case DisplayMode::DUAL_INFOGRAPHIC:   return cfg.dual;
        case DisplayMode::TRIPLE_INFOGRAPHIC: return cfg.triple;
        default:                              return cfg.single;
    }
}

QImage FrameRenderer::render(const FrameConfig& cfg)
{
    QImage img(LCD_W, LCD_H, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (cfg.rotation != 0) {
        p.translate(LCD_W / 2, LCD_H / 2);
        p.rotate(cfg.rotation);
        p.translate(-LCD_W / 2, -LCD_H / 2);
    }

    const ModeConfig& am = activeMode(cfg);
    const bool infographic = (cfg.mode == DisplayMode::SINGLE_INFOGRAPHIC
                           || cfg.mode == DisplayMode::DUAL_INFOGRAPHIC
                           || cfg.mode == DisplayMode::TRIPLE_INFOGRAPHIC);
    const bool    usesMedia = (cfg.mode == DisplayMode::IMAGE_GIF)
                           || (infographic && !am.gifPath.isEmpty());
    const QString mediaPath = (cfg.mode == DisplayMode::IMAGE_GIF) ? cfg.gifPath
                            : (infographic ? am.gifPath : QString());

    QMovie*       gif       = nullptr;
    const QImage* staticImg = nullptr;

    if (usesMedia && !mediaPath.isEmpty()) {
        // Resolution media memoisee tant que le chemin ne change pas : evite une std::string + des lookups map a chaque frame (le chemin ne change qu'au choix d'un fichier). m_gifCache/m_imageCache restent proprietaires ; on ne garde qu'un pointeur (stable car std::map ne deplace pas ses noeuds).
        if (mediaPath != m_lastMediaPath) {
            m_lastMediaPath = mediaPath;
            m_lastGif       = nullptr;
            m_lastStaticImg = nullptr;
            const std::string key = mediaPath.toStdString();
            if (isGifFile(mediaPath)) {
                if (m_gifCache.find(key) == m_gifCache.end())
                    m_gifCache[key] = loadGif(mediaPath);
                m_lastGif = m_gifCache[key].get();
            } else {
                if (m_imageCache.find(key) == m_imageCache.end()) {
                    QImageReader reader(mediaPath);
                    reader.setAutoTransform(true);
                    QImage loaded = reader.read();
                    if (!loaded.isNull()) {
                        m_imageCache[key] = loaded.scaled(LCD_W, LCD_H,
                            Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    } else {
                        m_imageCache[key] = QImage();
                    }
                }
                const QImage& cached = m_imageCache[key];
                if (!cached.isNull()) m_lastStaticImg = &m_imageCache[key];
            }
        }

        gif       = m_lastGif;
        staticImg = m_lastStaticImg;

        // L'avance d'image du GIF se fait a CHAQUE frame (animation).
        if (gif) {
            const int fc = gif->frameCount();
            if (fc > 0) {
                gif->jumpToFrame(m_gifFrame % fc);
                m_gifFrame++;
            }
        }
    }

    circularClip(p);
    renderBackground(p, cfg, gif, staticImg);

    switch (cfg.mode) {
        case DisplayMode::IMAGE_GIF:
            if (!gif && !staticImg) {
                QColor dim = NZXT_WHITE; dim.setAlphaF(0.55);
                drawStyledText(p, QRectF(0, LCD_H / 2.0 - LCD_R * 0.10, LCD_W, LCD_R * 0.20),
                               Qt::AlignHCenter | Qt::AlignVCenter,
                               QStringLiteral("No image or GIF"),
                               nzxtFont(int(LCD_R * 0.14)), dim, QColor(0, 0, 0, 0));
            }
            break;
        case DisplayMode::SINGLE_INFOGRAPHIC: renderSingleInfographic(p, cfg); break;
        case DisplayMode::DUAL_INFOGRAPHIC:   renderDualInfographic(p, cfg);   break;
        case DisplayMode::TRIPLE_INFOGRAPHIC: renderTripleInfographic(p, cfg); break;
        case DisplayMode::CLOCKFACE:    renderClock(p, cfg);       break;
        case DisplayMode::AUDIO_VISUAL: renderAudioVisual(p, cfg); break;
        case DisplayMode::NOW_PLAYING:  renderNowPlaying(p, cfg);  break;
    }

    if (cfg.brightness < 100) {
        float alpha = 1.f - cfg.brightness / 100.f;
        p.fillRect(0, 0, LCD_W, LCD_H, QColor(0, 0, 0, int(alpha * 255)));
    }

    p.end();
    return img;
}

QByteArray FrameRenderer::toJpeg(const QImage& img, int quality)
{
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", quality);
    return data;
}

QByteArray FrameRenderer::toQ565(const QImage& img)
{
    // Buffer reutilise d'une frame a l'autre (toQ565 n'est appele que depuis le thread worker, sequentiellement) : evite une alloc/free de ~2 Mio par frame. Q565_Encoder::encode fait resize(0) puis append -> la capacite est conservee. Le retour partage le buffer en COW ; la frame precedente est detruite avant le prochain appel, donc le resize(0) reste en place (refcount 1, pas de detach).
    thread_local QByteArray out;
    if (out.capacity() < (1 << 21))
        out.reserve(1 << 21);   // ~2 MiB, superieur au pire cas (une seule fois)
    Q565_Encoder::encode(img, &out);
    return out;
}

void FrameRenderer::setAudioLevels(const QVector<float>& bands)
{
    QMutexLocker lk(&m_audioMutex);
    m_audioBands = bands;
    while (m_audioBands.size() < 64) m_audioBands.append(0.f);
}

void FrameRenderer::setMediaInfo(const MediaSnapshot& snap)
{
    // Appelé sur le thread du worker (même thread que render) — pas de mutex requis.
    m_media = snap;
}

// ─── Fond ────────────────────────────────────────────────────────────────────
// Le fond utilise le ModeConfig actif pour les modes infographic, et cfg.bgColor pour les autres (Image/GIF, Clock, Audio).
void FrameRenderer::renderBackground(QPainter& p, const FrameConfig& cfg,
                                     QMovie* gif, const QImage* staticImg)
{
    // 1. Couleur de fond unie (base) — toujours dessinée d'abord.
    QColor bg = cfg.bgColor;
    switch (cfg.mode) {
        case DisplayMode::SINGLE_INFOGRAPHIC: bg = cfg.single.bgColor; break;
        case DisplayMode::DUAL_INFOGRAPHIC:   bg = cfg.dual.bgColor;   break;
        case DisplayMode::TRIPLE_INFOGRAPHIC: bg = cfg.triple.bgColor; break;
        default: break;
    }
    p.fillRect(0, 0, LCD_W, LCD_H, bg);

    // 2. Image/GIF par-dessus la couleur unie, s'il y en a une.
    bool drewMedia = false;
    if (gif) {
        QPixmap frame = gif->currentPixmap();
        if (!frame.isNull()) {
            QPixmap scaled = frame.scaled(LCD_W, LCD_H,
                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap((LCD_W - scaled.width()) / 2,
                         (LCD_H - scaled.height()) / 2, scaled);
            drewMedia = true;
        }
    } else if (staticImg && !staticImg->isNull()) {
        p.drawImage((LCD_W - staticImg->width()) / 2,
                    (LCD_H - staticImg->height()) / 2, *staticImg);
        drewMedia = true;
    }

    // 3. Voile sombre pour la lisibilité des stats (modes infographic uniquement).
    if (drewMedia && cfg.mode != DisplayMode::IMAGE_GIF)
        p.fillRect(0, 0, LCD_W, LCD_H, QColor(0, 0, 0, 140));
}

void FrameRenderer::circularClip(QPainter& p)
{
    QPainterPath path;
    path.addEllipse(0, 0, LCD_W, LCD_H);
    p.setClipPath(path);
}

// renderLogo : prend désormais un ModeConfig au lieu de la config globale.
// Chaque mode infographic appelle cette fonction avec son propre bundle.
void FrameRenderer::renderLogo(QPainter& p, const ModeConfig& mode)
{
    if (!mode.showLogo) return;
    const int px = int(LCD_R * 0.12);
    QFont f = nzxtFont(px, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, LCD_R * 0.015);
    const qreal y = LCD_H / 2.0 - LCD_R * 0.55;
    drawStyledText(p, QRectF(0, y - px, LCD_W, px * 2.0),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   QStringLiteral("NZXT"), f,
                   mode.logoColor, QColor(0,0,0,0));
}

// drawGaugeArc / drawSensorReading : stubs vides supprimes (jamais appeles).

QString FrameRenderer::formatValue(float v, SensorType t) const
{
    switch (t) {
        case SensorType::CPU_TEMP:
        case SensorType::GPU_TEMP:
        case SensorType::LIQUID_TEMP:
            return QString::number(int(maybeToFahrenheit(v, t))) + QString(QChar(0x00B0));
        case SensorType::CPU_LOAD:
        case SensorType::GPU_LOAD:
        case SensorType::MEM_LOAD:
            return QString::number(int(v)) + QStringLiteral("%");
        case SensorType::CPU_CLOCK:
        case SensorType::GPU_CLOCK:
            return QString::number(int(v)) + QStringLiteral(" MHz");
        default: return QString::number(int(v));
    }
}
QString FrameRenderer::formatValue(const SensorValue& sv, SensorType t) const
{
    if (!sv.available) return QStringLiteral("N/A");
    return formatValue(sv.value, t);
}

void FrameRenderer::setFahrenheit(bool on) { g_fahrenheit.store(on, std::memory_order_relaxed); }
bool FrameRenderer::fahrenheit()           { return g_fahrenheit.load(std::memory_order_relaxed); }
QString FrameRenderer::getShortLabel(SensorType t) const
{
    switch (t) {
        case SensorType::CPU_TEMP:    return QStringLiteral("CPU");
        case SensorType::GPU_TEMP:    return QStringLiteral("GPU");
        case SensorType::LIQUID_TEMP: return QStringLiteral("Liquid");
        case SensorType::CPU_LOAD:    return QStringLiteral("CPU");
        case SensorType::GPU_LOAD:    return QStringLiteral("GPU");
        case SensorType::CPU_CLOCK:   return QStringLiteral("CPU");
        case SensorType::GPU_CLOCK:   return QStringLiteral("GPU");
        case SensorType::MEM_LOAD:    return QStringLiteral("RAM");
        default: return QString();
    }
}

// ─── Helper pour générer un dégradé assombri (fond des jauges) ───────────────
static GradientStops createDarkenedStops(const GradientStops& original, float factor = 0.25f) {
    GradientStops darkened;
    for (const auto& stop : original) {
        darkened.append({stop.offset, QColor(
            std::clamp(int(stop.color.red() * factor), 0, 255),
            std::clamp(int(stop.color.green() * factor), 0, 255),
            std::clamp(int(stop.color.blue() * factor), 0, 255),
            stop.color.alpha()
        )});
    }
    return darkened;
}

// ═════════════════════════════════════════════════════════════════════════════
// MODE 1 — SINGLE INFOGRAPHIC
// (utilise cfg.singleReading + cfg.single ModeConfig — état indépendant du mode Triple)
// ═════════════════════════════════════════════════════════════════════════════
void FrameRenderer::renderSingleInfographic(QPainter& p, const FrameConfig& cfg)
{
    const auto& rc = cfg.singleReading;
    const SensorValue sv = m_sensors->read(rc.type);
    const UnitKind unit  = unitKindFor(rc.type);
    const bool isClock   = (unit == UnitKind::ClockMHz);

    const QString numStr = formatNumber(sv, rc.type, false);
    const QString label  = getShortLabel(rc.type);

    const qreal cx = LCD_W / 2.0;
    const qreal cy = LCD_H / 2.0;
    const qreal arcR  = LCD_R * 0.94;
    const qreal thick = LCD_R * 0.09;
    const qreal dotR  = thick / 2.0;
    const QRectF arcRect(cx - arcR, cy - arcR, arcR * 2.0, arcR * 2.0);

    const float pct = sensorPercent(sv, rc.type);
    const float valAngle = ARC_START + ARC_SWEEP * pct / 100.f;

    GradientStops stops = rc.vizStops.isEmpty() ? defaultSingleVizStops() : rc.vizStops;
    QConicalGradient grad = makeArcConicalGradient(QPointF(cx, cy), stops,
                                                    ARC_START, ARC_SWEEP);

    // ─── CORRECTION DU GAP ET DE LA BOULE ────────────────────────────────────
    // 1. Si le capteur n'est pas disponible, le gap est de 0 (le fond gris est entier).
    // 2. Si le capteur est disponible, on applique strictement l'écart constant (10°).
    float currentGap = sv.available ? ARC_GAP_DEG : 0.f;

    // Dessin du fond : même dégradé que la jauge mais assombri (effet de voile) pour matérialiser visuellement la zone non remplie.
    GradientStops bgStops = createDarkenedStops(stops);
    QConicalGradient bgGrad = makeArcConicalGradient(QPointF(cx, cy), bgStops,
                                                     ARC_START, ARC_SWEEP);
    drawClockArcBrush(p, arcRect, valAngle + currentGap, ARC_END,
                      QBrush(bgGrad), thick);

    // Dessin de la jauge et de la boule (uniquement s'il y a une valeur lisible)
    if (sv.available) {
        // La jauge colorée s'arrête 10° AVANT la valeur, pour laisser la boule isolée au milieu de son espace !
        drawClockArcBrush(p, arcRect, ARC_START, valAngle - ARC_GAP_DEG, QBrush(grad), thick);

        // La boule est dessinée à sa position exacte et redevient visible
        const QColor dotCol = colorAtPercent(stops, pct);
        const QPointF dotPos = clockToCart(cx, cy, arcR, valAngle);
        p.setBrush(dotCol);
        p.setPen(Qt::NoPen);
        p.drawEllipse(dotPos, dotR, dotR);
    }

    renderLogo(p, cfg.single);

    const int len = isClock ? 4 : numStr.length();
    int valPx, degPx;
    if (len >= 4)      { valPx = int(LCD_R * 0.46); degPx = int(LCD_R * 0.115); }
    else if (len == 3) { valPx = int(LCD_R * 0.58); degPx = int(LCD_R * 0.14); }
    else               { valPx = int(LCD_R * 0.68); degPx = int(LCD_R * 0.17); }

    // forceShowUnit = sv.available — masque °/%/GHz quand pas de lecture
    drawMetric(p, cx, cy, numStr, valPx, degPx,
               rc.numberFill, rc.numberOutline, unit, TextAlign::Middle,
               sv.available);

    if (isClock && sv.available) {
        QFont fm = nzxtFont(int(LCD_R * 0.09));
        QColor fillC = rc.textFill; fillC.setAlphaF(0.50);
        const qreal my = cy + valPx * 0.55;
        drawStyledText(p, QRectF(0, my - 30, LCD_W, 60),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       QStringLiteral("MHz"), fm, fillC, rc.textOutline);
    }

    QFont fl = nzxtFont(int(LCD_R * 0.14));
    QColor lblFill = rc.textFill; lblFill.setAlphaF(0.60);
    const qreal ly = cy + LCD_R * 0.38;
    drawStyledText(p, QRectF(0, ly - 30, LCD_W, 60),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   label, fl, lblFill, rc.textOutline);
} // 

// ═════════════════════════════════════════════════════════════════════════════
// MODE 2 — DUAL INFOGRAPHIC
// (utilise cfg.dualReading1/2 + cfg.dual ModeConfig)
// ═════════════════════════════════════════════════════════════════════════════
void FrameRenderer::renderDualInfographic(QPainter& p, const FrameConfig& cfg)
{
    const auto& rc1 = cfg.dualReading1;
    const auto& rc2 = cfg.dualReading2;
    const SensorValue sv1 = m_sensors->read(rc1.type);
    const SensorValue sv2 = m_sensors->read(rc2.type);
    const UnitKind u1 = unitKindFor(rc1.type);
    const UnitKind u2 = unitKindFor(rc2.type);
    const float pct1  = sensorPercent(sv1, rc1.type);
    const float pct2  = sensorPercent(sv2, rc2.type);
    const QString lbl1 = getShortLabel(rc1.type);
    const QString lbl2 = getShortLabel(rc2.type);

    const qreal cx = LCD_W / 2.0;
    const qreal cy = LCD_H / 2.0;

    if (cfg.dualVertical) {
        const qreal arcR    = LCD_R * 0.94;
        const qreal thick   = LCD_R * 0.09;
        const qreal halfRng = 60.0;
        const QRectF arcRect(cx - arcR, cy - arcR, arcR * 2.0, arcR * 2.0);

        auto drawSide = [&](const ReadingConfig& rc, float pct, qreal anchor) {
            const qreal trackS = anchor - halfRng;
            const qreal trackE = anchor + halfRng;
            const qreal spread = halfRng * std::clamp(pct, 0.f, 100.f) / 100.f;
            drawClockArc(p, arcRect, trackS, trackE, NZXT_TRACK, thick);
            if (pct > 0.3f)
                drawClockArc(p, arcRect, anchor - spread, anchor + spread,
                             rc.colorViz, thick);
        };
        drawSide(rc1, pct1, 270.0);
        drawSide(rc2, pct2,  90.0);

        auto sizeOf = [](const QString& s) -> std::pair<int,int> {
            const int len = s.length();
            if (len >= 4) return { int(LCD_R * 0.30), int(LCD_R * 0.075) };
            if (len == 3) return { int(LCD_R * 0.38), int(LCD_R * 0.095) };
            return                { int(LCD_R * 0.47), int(LCD_R * 0.115) };
        };
        const QString num1 = formatNumber(sv1, rc1.type, false);
        const QString num2 = formatNumber(sv2, rc2.type, false);
        const auto [v1, d1] = sizeOf(num1);
        const auto [v2, d2] = sizeOf(num2);
        const int labelPx = int(LCD_R * 0.14);

        const qreal leftX  = cx - LCD_R * 0.40;
        const qreal rightX = cx + LCD_R * 0.40;
        const qreal blockY = cy + LCD_R * 0.06;
        const qreal valueY = blockY - LCD_R * 0.03;
        const qreal labelY = blockY + LCD_R * 0.27;   // un peu plus d'air que 0.24, sans trop ecarter

        // L'unité MHz est toujours masquée pour les clocks (affichage trop large). Autres unités : seulement si le capteur a une valeur lisible.
        const bool showU1 = sv1.available && (u1 != UnitKind::ClockMHz);
        const bool showU2 = sv2.available && (u2 != UnitKind::ClockMHz);

        drawMetric(p, leftX, valueY, num1, v1, d1,
                   rc1.numberFill, rc1.numberOutline, u1, TextAlign::Middle, showU1);
        drawMetric(p, rightX, valueY, num2, v2, d2,
                   rc2.numberFill, rc2.numberOutline, u2, TextAlign::Middle, showU2);

        QFont fl = nzxtFont(labelPx, QFont::DemiBold);
        QColor cl1 = rc1.textFill; cl1.setAlphaF(0.65);
        QColor cl2 = rc2.textFill; cl2.setAlphaF(0.65);
        drawStyledText(p, QRectF(leftX - 120, labelY - 30, 240, 60),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       lbl1, fl, cl1, rc1.textOutline);
        drawStyledText(p, QRectF(rightX - 120, labelY - 30, 240, 60),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       lbl2, fl, cl2, rc2.textOutline);

        renderLogo(p, cfg.dual);
        return;
    }

    // ───── HORIZONTAL : 2 barres empilées ───────────────────────────────────
    const qreal barW = LCD_R * 1.25;
    const qreal barH = LCD_R * 0.09;
    const qreal barRx = barH / 2.0;
    const qreal barX = cx - barW / 2.0;
    const qreal rightEdge = barX + barW;
    const qreal barGap = LCD_R * 0.05;
    const qreal topBarTop = cy - barGap / 2.0 - barH;
    const qreal botBarTop = cy + barGap / 2.0;

    const int valFont   = int(LCD_R * 0.44);
    const int labelFont = int(LCD_R * 0.14);
    const int degSize   = int(LCD_R * 0.11);
    const qreal textPad = LCD_R * 0.04;
    const qreal BASELINE = 0.38;

    const qreal topBottomLine = topBarTop - textPad;
    const qreal topLabelY     = topBottomLine - labelFont * BASELINE;
    const qreal topValY       = topBottomLine - valFont   * BASELINE;
    const qreal botTopLine = botBarTop + barH + textPad;
    const qreal botLabelY  = botTopLine + labelFont * BASELINE;
    const qreal botValY    = botTopLine + valFont   * 0.34;

    const bool top_isClock = (u1 == UnitKind::ClockMHz);
    const bool bot_isClock = (u2 == UnitKind::ClockMHz);
    const UnitKind topUnit = top_isClock ? UnitKind::ClockGHz : u1;
    const UnitKind botUnit = bot_isClock ? UnitKind::ClockGHz : u2;
    const QString topNum = formatNumber(sv1, rc1.type, top_isClock);
    const QString botNum = formatNumber(sv2, rc2.type, bot_isClock);

    QFont fLbl = nzxtFont(labelFont, QFont::Medium);
    QColor c1 = rc1.textFill; c1.setAlphaF(0.65);
    QColor c2 = rc2.textFill; c2.setAlphaF(0.65);

    drawStyledText(p, QRectF(barX, topLabelY - 30, 400, 60),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   lbl1, fLbl, c1, rc1.textOutline);
    drawMetric(p, rightEdge, topValY, topNum, valFont, degSize,
               rc1.numberFill, rc1.numberOutline, topUnit, TextAlign::End,
               sv1.available);

    p.setPen(Qt::NoPen);
    p.setBrush(NZXT_TRACK);
    p.drawRoundedRect(QRectF(barX, topBarTop, barW, barH), barRx, barRx);
    if (pct1 > 0.3f) {
        p.setBrush(rc1.colorViz);
        const qreal fillW = std::max(barH, barW * std::clamp(pct1, 0.f, 100.f) / 100.f);
        p.drawRoundedRect(QRectF(barX, topBarTop, fillW, barH), barRx, barRx);
    }

    p.setBrush(NZXT_TRACK);
    p.drawRoundedRect(QRectF(barX, botBarTop, barW, barH), barRx, barRx);
    if (pct2 > 0.3f) {
        p.setBrush(rc2.colorViz);
        const qreal fillW = std::max(barH, barW * std::clamp(pct2, 0.f, 100.f) / 100.f);
        p.drawRoundedRect(QRectF(barX, botBarTop, fillW, barH), barRx, barRx);
    }

    drawStyledText(p, QRectF(barX, botLabelY - 30, 400, 60),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   lbl2, fLbl, c2, rc2.textOutline);
    drawMetric(p, rightEdge, botValY, botNum, valFont, degSize,
               rc2.numberFill, rc2.numberOutline, botUnit, TextAlign::End,
               sv2.available);
    // Pas de logo NZXT en mode Dual horizontal (cf. JSX).
}

// ═════════════════════════════════════════════════════════════════════════════
// MODE 3 — TRIPLE INFOGRAPHIC
// (utilise cfg.tripleReading1/2/3 + cfg.triple ModeConfig — état indépendant du mode Single)
// ═════════════════════════════════════════════════════════════════════════════
void FrameRenderer::renderTripleInfographic(QPainter& p, const FrameConfig& cfg)
{
    const auto& rcM = cfg.tripleReading1;
    const auto& rcT = cfg.tripleReading2;
    const auto& rcB = cfg.tripleReading3;
    const SensorValue svM = m_sensors->read(rcM.type);
    const SensorValue svT = m_sensors->read(rcT.type);
    const SensorValue svB = m_sensors->read(rcB.type);
    const UnitKind uM = unitKindFor(rcM.type);
    const UnitKind uT = unitKindFor(rcT.type);
    const UnitKind uB = unitKindFor(rcB.type);
    const float pctM = sensorPercent(svM, rcM.type);
    const QString numM = formatNumber(svM, rcM.type, false);
    const QString numT = formatNumber(svT, rcT.type, false);
    const QString numB = formatNumber(svB, rcB.type, false);
    const QString lblM = getShortLabel(rcM.type);
    const QString lblT = getShortLabel(rcT.type);
    const QString lblB = getShortLabel(rcB.type);

    const qreal cx = LCD_W / 2.0;
    const qreal cy = LCD_H / 2.0;
    const qreal arcR  = LCD_R * 0.945;
    const qreal thick = LCD_R * 0.04;
    const qreal dotR  = LCD_R * 0.045;
    const QRectF arcRect(cx - arcR, cy - arcR, arcR * 2.0, arcR * 2.0);

    GradientStops stops = rcM.vizStops.isEmpty() ? defaultTripleVizStops() : rcM.vizStops;
    QConicalGradient grad = makeArcConicalGradient(QPointF(cx, cy), stops,
                                                    ARC_START, ARC_SWEEP);

    const float valAngle = ARC_START + ARC_SWEEP * pctM / 100.f;
    
    // ─── CORRECTION DU GAP ET DE LA BARRE (TRIPLE) ───────────────────────────
    float currentGap = svM.available ? ARC_GAP_DEG : 0.f;

    // Fond : même dégradé que la barre mais assombri (effet de voile) pour matérialiser visuellement la zone non remplie.
    GradientStops bgStops = createDarkenedStops(stops);
    QConicalGradient bgGrad = makeArcConicalGradient(QPointF(cx, cy), bgStops,
                                                     ARC_START, ARC_SWEEP);
    drawClockArcBrush(p, arcRect, valAngle + currentGap, ARC_END,
                      QBrush(bgGrad), thick);

    // Barre colorée et Boule
    if (svM.available) {
        drawClockArcBrush(p, arcRect, ARC_START, valAngle, QBrush(grad), thick);

        const QColor dotCol = colorAtPercent(stops, pctM);
        const QPointF dotPos = clockToCart(cx, cy, arcR, valAngle);
        p.setBrush(dotCol); p.setPen(Qt::NoPen);
        p.drawEllipse(dotPos, dotR, dotR);
    }

    // ─── Tailles de police ───────────────────────────────────────────────
    // Le nombre primaire ET les nombres secondaires retrecissent selon leur nombre de chiffres ; les labels gardent leur taille fixe.
    const int mLen = numM.length();
    int mFont, mDeg;
    if (mLen >= 4)      { mFont = int(LCD_R * 0.38); mDeg = int(LCD_R * 0.09); }
    else if (mLen == 3) { mFont = int(LCD_R * 0.48); mDeg = int(LCD_R * 0.11); }
    else                { mFont = int(LCD_R * 0.60); mDeg = int(LCD_R * 0.135); }

    // Secondaire/tertiaire : meme logique que le primaire (le chiffre diminue avec sa longueur), label fixe et chiffre aligne a gauche (TextAlign::Start).
    auto secSize = [](const QString& s) -> std::pair<int,int> {
        const int len = s.length();
        if (len >= 4) return { int(LCD_R * 0.16), int(LCD_R * 0.05)  };
        if (len == 3) return { int(LCD_R * 0.20), int(LCD_R * 0.06)  };
        return                { int(LCD_R * 0.25), int(LCD_R * 0.075) };
    };
    const auto [secValT, secDegT] = secSize(numT);
    const auto [secValB, secDegB] = secSize(numB);
    const int  secLbl = int(LCD_R * 0.085);

    const bool showUM = svM.available && (uM != UnitKind::ClockMHz);
    const bool showUT = svT.available && (uT != UnitKind::ClockMHz);
    const bool showUB = svB.available && (uB != UnitKind::ClockMHz);

    // ─── Recentrage horizontal du bloc (primaire | separateur | secondaires) ─
    // On mesure la largeur reellement dessinee de chaque element puis on decale tout le bloc pour que son milieu tombe sur cx (l'arc reste centre).
    auto metricW = [](const QString& num, int numPx, int unitPx,
                      UnitKind unit, bool showUnit) -> qreal {
        qreal w = QFontMetricsF(nzxtFont(numPx)).horizontalAdvance(num);
        if (showUnit) {
            QString us; int upx = unitPx;
            if      (unit == UnitKind::Temp)      us = QString(QChar(0x00B0));
            else if (unit == UnitKind::Load)    { us = QStringLiteral("%");   upx = int(unitPx * 0.85f); }
            else if (unit == UnitKind::ClockGHz){ us = QStringLiteral("GHz"); upx = int(unitPx * 0.85f); }
            if (!us.isEmpty())
                w += LCD_R * 0.008 + QFontMetricsF(nzxtFont(upx)).horizontalAdvance(us);
        }
        return w;
    };
    auto labelW = [](const QString& s, int px) -> qreal {
        return QFontMetricsF(nzxtFont(px, QFont::Medium)).horizontalAdvance(s);
    };

    // Ancres de base (avant recentrage)
    const qreal leftX0  = cx - LCD_R * 0.32;        // centre du primaire (nombre + label)
    const qreal sepX0   = cx + LCD_R * 0.16;        // separateur vertical
    const qreal rightX0 = sepX0 + LCD_R * 0.08;     // depart (gauche) des secondaires

    // Etendues horizontales du bloc.
    const qreal numWM    = QFontMetricsF(nzxtFont(mFont)).horizontalAdvance(numM);
    const qreal unitWM   = metricW(numM, mFont, mDeg, uM, showUM) - numWM;  // overhang unite (droite)
    const qreal halfLblM = labelW(lblM, int(LCD_R * 0.14)) / 2.0;
    const qreal primaryLeft  = leftX0 - std::max(numWM / 2.0, halfLblM);
    const qreal primaryRight = leftX0 + std::max(numWM / 2.0 + unitWM, halfLblM);

    const qreal lineWT = std::max(metricW(numT, secValT, secDegT, uT, showUT),
                                  labelW(lblT, secLbl));
    const qreal lineWB = std::max(metricW(numB, secValB, secDegB, uB, showUB),
                                  labelW(lblB, secLbl));
    const qreal secRight = rightX0 + std::max(lineWT, lineWB);

    // Milieu du bloc ramene sur cx (primaryRight ne depasse pas le bloc secondaire).
    const qreal ox = cx - (primaryLeft + secRight) / 2.0;

    const qreal leftX  = leftX0  + ox;
    const qreal sepX   = sepX0   + ox;
    const qreal rightX = rightX0 + ox;

    // ─── Separateur vertical ─────────────────────────────────────────────
    const qreal sepTop = cy - LCD_R * 0.22;
    const qreal sepBot = cy + LCD_R * 0.37;
    {
        QColor sc = NZXT_WHITE; sc.setAlphaF(0.35);
        QPen sep(sc, std::max(1.0, LCD_R * 0.008));
        p.setPen(sep);
        p.drawLine(QPointF(sepX, sepTop), QPointF(sepX, sepBot));
    }

    // ─── Primaire (nombre centre + label centre sous lui) ────────────────
    drawMetric(p, leftX, cy, numM, mFont, mDeg,
               rcM.numberFill, rcM.numberOutline, uM, TextAlign::Middle, showUM);

    QFont fL = nzxtFont(int(LCD_R * 0.14), QFont::Medium);
    QColor clM = rcM.textFill; clM.setAlphaF(0.65);
    drawStyledText(p, QRectF(leftX - 120, cy + LCD_R * 0.34 - 30, 240, 60),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   lblM, fL, clM, rcM.textOutline);

    // ─── Secondaire + tertiaire (chiffres alignes a gauche, labels fixes) ─
    const qreal secTopY = cy - LCD_R * 0.14;
    const qreal secBotY = cy + LCD_R * 0.18;
    drawMetric(p, rightX, secTopY, numT, secValT, secDegT,
               rcT.numberFill, rcT.numberOutline, uT, TextAlign::Start, showUT);
    drawMetric(p, rightX, secBotY, numB, secValB, secDegB,
               rcB.numberFill, rcB.numberOutline, uB, TextAlign::Start, showUB);

    QFont fSec = nzxtFont(secLbl, QFont::Medium);
    QColor cT = rcT.textFill; cT.setAlphaF(0.7);
    QColor cB = rcB.textFill; cB.setAlphaF(0.7);
    drawStyledText(p, QRectF(rightX, secTopY + LCD_R * 0.15 - 20, 240, 40),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   lblT, fSec, cT, rcT.textOutline);
    drawStyledText(p, QRectF(rightX, secBotY + LCD_R * 0.15 - 20, 240, 40),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   lblB, fSec, cB, rcB.textOutline);

    renderLogo(p, cfg.triple);
}

// ═════════════════════════════════════════════════════════════════════════════
// CLOCKFACE
// ═════════════════════════════════════════════════════════════════════════════
void FrameRenderer::renderClock(QPainter& p, const FrameConfig& cfg)
{
    QTime now = QTime::currentTime();
    const float secFrac  = now.second() + now.msec() / 1000.f;
    const float minFrac  = now.minute() + secFrac / 60.f;
    const float hourFrac = (now.hour() % 12) + minFrac / 60.f;

    const QPointF center(LCD_W / 2.0, LCD_H / 2.0);

    // ═══ STYLE 1 — ARC DÉGRADÉ ═══════════════════════════════════════
    // Le dégradé conique remplit tout l'écran : il démarre devant l'aiguille (heures), fait le tour (360°) et se termine derrière (couture sur l'aiguille).
    if (cfg.clockStyle == 1) {
        const qreal R = LCD_R * 0.985;
        const float handClock = hourFrac * 30.f;          // tour complet en 12 h

        GradientStops stops = cfg.clockArcStops.isEmpty()
                            ? defaultClockArcStops() : cfg.clockArcStops;
        std::sort(stops.begin(), stops.end(),
                  [](const GradStop& a, const GradStop& b){ return a.offset < b.offset; });

        // Conique plein tour : offset 0 devant l'aiguille, 100 derrière (couture).
        const float qtStart = std::fmod(90.f - handClock + 7200.f, 360.f);
        QConicalGradient cg(center, qtStart);
        for (const auto& s : stops)
            cg.setColorAt(std::clamp(1.f - s.offset / 100.f, 0.f, 1.f), s.color);

        QPainterPath disk; disk.addEllipse(center, R, R);
        p.fillPath(disk, QBrush(cg));

        // Aiguille (ligne fine centre → bord)
        const QPointF tip = clockToCart(center.x(), center.y(), R, handClock);
        p.setPen(QPen(cfg.clockArcDial, std::max(2.0, LCD_R * 0.013),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(center, tip);

        // Texte hh / mm près de la pointe de l'aiguille
        int hr = now.hour();
        if (!cfg.clockArc24h) { hr %= 12; if (hr == 0) hr = 12; }
        const QString hh = QStringLiteral("%1").arg(hr, 2, 10, QChar('0'));
        const QString mm = QStringLiteral("%1").arg(now.minute(), 2, 10, QChar('0'));

        // Texte fixe sur la droite : centré horizontalement entre le centre de l'horloge et le bord droit, et centré verticalement.
        const qreal ax = center.x() + R * 0.5;
        const qreal ay = center.y();
        const int hPx = int(LCD_R * 0.28), mPx = int(LCD_R * 0.24);
        const int hBox = int(hPx * 1.5), mBox = int(mPx * 1.6);   // boîtes hautes : pas de rognage
        QColor mmCol = cfg.clockArcText; mmCol.setAlphaF(0.85);
        drawStyledText(p, QRectF(ax - 160, ay - hBox, 320, hBox),
                       Qt::AlignHCenter | Qt::AlignBottom, hh, nzxtFont(hPx, QFont::Bold),
                       cfg.clockArcText, QColor(0,0,0,0));
        drawStyledText(p, QRectF(ax - 160, ay, 320, mBox),
                       Qt::AlignHCenter | Qt::AlignTop, mm, nzxtFont(mPx, QFont::Normal),
                       mmCol, QColor(0,0,0,0));
        return;
    }

    // ═══ STYLE 2 — DIGITAL ══════════════════════════════════════════
    // Gros chiffres hh au-dessus de mm, un seul dégradé vertical (haut → bas).
    if (cfg.clockStyle == 2) {
        p.fillRect(0, 0, LCD_W, LCD_H, cfg.clockDigitalBg);
        int hr = now.hour();
        if (!cfg.clockDigital24h) { hr %= 12; if (hr == 0) hr = 12; }
        const QString hh = QStringLiteral("%1").arg(hr, 2, 10, QChar('0'));
        const QString mm = QStringLiteral("%1").arg(now.minute(), 2, 10, QChar('0'));

        GradientStops stops = cfg.clockDigitalStops.isEmpty()
                            ? defaultClockDigitalStops() : cfg.clockDigitalStops;
        std::sort(stops.begin(), stops.end(),
                  [](const GradStop& a, const GradStop& b){ return a.offset < b.offset; });

        const int px = int(LCD_R * 0.82);
        QFont fHH = nzxtFont(px, QFont::Bold);
        QFont fMM = nzxtFont(px, QFont::Normal);   // minutes : pas en gras
        QFontMetricsF fmHH(fHH), fmMM(fMM);
        const qreal capH = fmHH.capHeight();
        const qreal gap  = capH * 0.12;
        const qreal baseHH = center.y() - gap * 0.5;
        const qreal baseMM = center.y() + gap * 0.5 + capH;
        const qreal wHH = fmHH.horizontalAdvance(hh);
        const qreal wMM = fmMM.horizontalAdvance(mm);

        QPainterPath path;
        path.addText(center.x() - wHH / 2.0, baseHH, fHH, hh);
        path.addText(center.x() - wMM / 2.0, baseMM, fMM, mm);

        const QRectF bb = path.boundingRect();
        QLinearGradient lg(0, bb.top(), 0, bb.bottom());
        for (const auto& s : stops)
            lg.setColorAt(std::clamp(s.offset / 100.f, 0.f, 1.f), s.color);
        p.fillPath(path, QBrush(lg));
        return;
    }

    // ═══ STYLE 0 — ANALOGIQUE À POINTS (défaut) — fond = bgColor ═════════════════
    const float R = LCD_R * 0.95f;                 // remplit l'écran
    for (int i = 0; i < 60; i++) {
        float angle = i * 6.f * M_PI / 180.f;
        float r     = LCD_R * 0.96f;   // anneau au plus près du bord (bord ext. des points ≈ arc single/dual/triple)
        float x = center.x() + std::sin(angle) * r;
        float y = center.y() - std::cos(angle) * r;
        float dotR = (i % 5 == 0) ? 8.f : 3.5f;
        p.setBrush(cfg.clockDial); p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(x, y), dotR, dotR);
    }

    auto drawHand = [&](float angleDeg, float length, float width, QColor col) {
        float rad = angleDeg * M_PI / 180.f;
        QPointF tip(center.x() + std::sin(rad) * length,
                    center.y() - std::cos(rad) * length);
        QPen hPen(col, width, Qt::SolidLine, Qt::RoundCap);
        p.setPen(hPen); p.drawLine(center, tip);
    };
    drawHand(hourFrac * 30.f,       R * 0.52f, 11, cfg.clockDial);
    drawHand(minFrac  * 6.f,        R * 0.75f, 8,  cfg.clockDial);
    drawHand(float(now.second()) * 6.f, R * 0.83f, 3, cfg.clockSeconds);  // tic discret, aligné sur les points

    p.setBrush(cfg.clockDial); p.setPen(Qt::NoPen);
    p.drawEllipse(center, 10.0, 10.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// AUDIO VISUAL
// ═════════════════════════════════════════════════════════════════════════════
// Contour POLYGONAL reliant les sommets par des segments droits.
// (Mode connecté — volontairement très peu arrondi : on relie directement les pics, ce qui donne une forme anguleuse / dentelée plutôt qu'un blob lisse.)
static QPainterPath nzxtPolygonClosedPath(const QList<QPointF>& pts)
{
    QPainterPath path;
    const int n = int(pts.size());
    if (n == 0) return path;
    path.moveTo(pts[0]);
    for (int i = 1; i < n; ++i) path.lineTo(pts[i]);
    path.closeSubpath();
    return path;
}

void FrameRenderer::renderAudioVisual(QPainter& p, const FrameConfig& cfg)
{
    QVector<float> bands;
    {
        QMutexLocker lk(&m_audioMutex);
        bands = m_audioBands;
    }
    const QPointF center(LCD_W / 2.0, LCD_H / 2.0);
    const int   N = bands.size();

    // Dégradé horizontal gauche→droite (mêmes presets que single/triple).
    GradientStops stops = cfg.audioStops.isEmpty() ? defaultVizStops() : cfg.audioStops;
    std::sort(stops.begin(), stops.end(),
              [](const GradStop& a, const GradStop& b){ return a.offset < b.offset; });
    QLinearGradient grad(0, 0, LCD_W, 0);
    for (const auto& s : stops)
        grad.setColorAt(std::clamp(s.offset / 100.f, 0.f, 1.f), s.color);

    // ── Sensibilité = simple GAIN (0 au silence quoi qu'il arrive) ──
    // audioSensitivity 0 (Low, -30 dB) → gain 0.5 ; 1 (High, -120 dB) → gain 8.
    const float gain = 0.5f * std::pow(16.f, std::clamp(cfg.audioSensitivity, 0.f, 1.f));
    QVector<float> amps(N);
    float sum = 0.f, peak = 0.f;
    for (int i = 0; i < N; ++i) {
        amps[i] = std::clamp(bands[i] * gain, 0.f, 1.f);
        sum += amps[i];
        peak = std::max(peak, amps[i]);
    }
    const float meanAmp = (N > 0) ? sum / float(N) : 0.f;
    const float level   = std::clamp(meanAmp * 0.55f + peak * 0.45f, 0.f, 1.f);

    // Géométrie du "waveform" radial : petit au repos, piques nets vers le bord (calé sur le rendu NZXT CAM).
    const float restR = 42.f;                         // rayon au silence (petit cercle)
    const float reach = float(LCD_R) - restR - 8.f;   // extension max des piques

    auto buildContour = [&]() -> QPainterPath {
        QList<QPointF> pts; pts.reserve(N);
        for (int i = 0; i < N; ++i) {
            float angle = float(i) / N * 2.f * float(M_PI) - float(M_PI) / 2.f;
            float r = restR + amps[i] * reach;
            pts.append(QPointF(center.x() + std::cos(angle) * r,
                               center.y() + std::sin(angle) * r));
        }
        return nzxtPolygonClosedPath(pts);
    };
    auto drawBars = [&](const QBrush& brush) {
        QPen barPen; barPen.setWidthF(5.f); barPen.setCapStyle(Qt::RoundCap);
        barPen.setBrush(brush);
        p.setPen(barPen);
        for (int i = 0; i < N; ++i) {
            float angle = float(i) / N * 2.f * float(M_PI) - float(M_PI) / 2.f;
            float barH = amps[i] * reach;
            if (barH < 2.f) continue;
            const float x1 = center.x() + std::cos(angle) * restR;
            const float y1 = center.y() + std::sin(angle) * restR;
            const float x2 = center.x() + std::cos(angle) * (restR + barH);
            const float y2 = center.y() + std::sin(angle) * (restR + barH);
            p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
        }
    };

    // ── Waveform (calé sur NZXT CAM) ──
    // invert  : fond couleur, forme NOIRE ; !invert : fond noir, forme en COULEUR.
    p.setPen(Qt::NoPen);
    if (cfg.audioInvert) {
        p.fillRect(0, 0, LCD_W, LCD_H, QBrush(grad));
        p.setBrush(Qt::black);
        if (cfg.audioConnected) p.drawPath(buildContour());
        else                    drawBars(QBrush(Qt::black));
    } else {
        p.fillRect(0, 0, LCD_W, LCD_H, Qt::black);
        if (cfg.audioConnected) { p.setBrush(QBrush(grad)); p.drawPath(buildContour()); }
        else                    drawBars(QBrush(grad));
    }

    // ── Pastille centrale RÉACTIVE + logo NZXT ──
    // Le centre respire avec le son (et se déforme légèrement en mode connecté).
    const float centerMinR = 75.f;                       // cercle neutre (agrandi)
    const float centerR    = centerMinR + level * 32.f;  // respire ~75 → ~107
    QPainterPath centerPath;
    if (cfg.audioConnected) {
        const float lightSpike = 16.f;   // déformation légère (plus douce que les pics)
        QList<QPointF> cpts; cpts.reserve(N);
        for (int i = 0; i < N; ++i) {
            float angle = float(i) / N * 2.f * float(M_PI) - float(M_PI) / 2.f;
            float r = centerR + amps[i] * lightSpike;
            cpts.append(QPointF(center.x() + std::cos(angle) * r,
                                center.y() + std::sin(angle) * r));
        }
        centerPath = nzxtPolygonClosedPath(cpts);
    } else {
        centerPath.addEllipse(center, centerR, centerR);
    }
    p.setPen(Qt::NoPen);
    if (cfg.audioInvert) {
        // Invert : centre noir.
        p.setBrush(Qt::black);
        p.drawPath(centerPath);
    } else {
        // Non-invert : au repos le centre est COMPLÈTEMENT noir ; le voile coloré n'apparaît qu'avec le son, en fondu (transition douce).
        p.setBrush(QBrush(grad));
        p.drawPath(centerPath);                                      // couleur sous le voile
        int veilA = int(std::clamp(1.f - level, 0.f, 1.f) * 255.f);  // 255 (silence) → 0 (fort)
        if (veilA > 0) {
            p.setBrush(QColor(0, 0, 0, veilA));
            p.drawPath(centerPath);
        }
    }

    if (cfg.audioShowLogo) {
        int px = int(30.f + level * 4.f);   // quasi constant (léger pouls)
        QFont f = FONT_NZXT; f.setPixelSize(px); f.setBold(true);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 4);
        p.setFont(f); p.setPen(cfg.audioLogoColor);
        const float h = float(px) + 14.f;
        p.drawText(QRectF(0, LCD_H / 2.0 - h / 2.0, LCD_W, h),
                   Qt::AlignHCenter | Qt::AlignVCenter, "NZXT");
    }
}

// ═════════════════════════════════════════════════════════════════
// NOW PLAYING (musique via SMTC)
// Disposition : artiste (haut) → pochette (centre) → barre + timestamp (sous la pochette) → titre (sous le timestamp). Fond = cfg.bgColor.
// ═════════════════════════════════════════════════════════════════
void FrameRenderer::renderNowPlaying(QPainter& p, const FrameConfig& cfg)
{
    const QColor fg = NZXT_WHITE;
    const qreal cx = LCD_W / 2.0;
    const qreal cy = LCD_H / 2.0;
    const MediaSnapshot& m = m_media;

    if (!m.active) {
        QColor dim = fg; dim.setAlphaF(0.55);
        drawStyledText(p, QRectF(0, cy - LCD_R * 0.10, LCD_W, LCD_R * 0.20),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       QStringLiteral("No media playing"),
                       nzxtFont(int(LCD_R * 0.14)), dim, QColor(0, 0, 0, 0));
        return;
    }

    // Largeur utile (corde du cercle) à une hauteur y.
    auto widthAt = [&](qreal y) -> qreal {
        const qreal dy = std::abs(y - cy);
        if (dy >= LCD_R) return 0.0;
        return 2.0 * std::sqrt(double(LCD_R) * double(LCD_R) - double(dy) * double(dy));
    };
    // Défilement va-et-vient : ancre temps réinitialisée au changement de piste, pour que titre/artiste repartent du début à chaque nouveau morceau.
    const QString trackKey = m.artist + QChar(0x1F) + m.title;
    if (trackKey != m_npScrollKey) {
        m_npScrollKey      = trackKey;
        m_npScrollAnchorMs = animNowMs();
    }
    const qint64 npElapsed = animNowMs() - m_npScrollAnchorMs;

    // ── Pochette (centre, légèrement relevée) ──
    const qreal coverSize = LCD_R * 0.88;
    const qreal coverCY   = cy - LCD_R * 0.10;
    const QRectF coverRect(cx - coverSize / 2.0, coverCY - coverSize / 2.0,
                           coverSize, coverSize);
    const qreal coverRad = LCD_R * 0.05;

    if (!m.cover.isNull()) {
        QPainterPath clip; clip.addRoundedRect(coverRect, coverRad, coverRad);
        p.save();
        p.setClipPath(clip);
        QImage cov = m.cover;
        const int s = std::min(cov.width(), cov.height());
        const QRect src((cov.width() - s) / 2, (cov.height() - s) / 2, s, s);
        p.drawImage(coverRect, cov, src);
        p.restore();
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(NZXT_TRACK);
        p.drawRoundedRect(coverRect, coverRad, coverRad);
        QColor note = fg; note.setAlphaF(0.35);
        drawStyledText(p, coverRect, Qt::AlignHCenter | Qt::AlignVCenter,
                       QString(QChar(0x266A)), nzxtFont(int(coverSize * 0.42)),
                       note, QColor(0, 0, 0, 0));
    }

    // ── Artiste (au-dessus de la pochette) ──
    {
        const qreal y = coverRect.top() - LCD_R * 0.15;
        QFont f = nzxtFont(int(LCD_R * 0.095), QFont::Medium);
        drawScrollingText(p, cx, y, LCD_R * 0.20, widthAt(y) - LCD_R * 0.10,
                          m.artist, f, cfg.npArtistFill, cfg.npArtistOutline, npElapsed);
    }

    // ── Barre de progression + temps (seulement si activé ET durée connue) ──
    const bool showProgress = cfg.npShowProgress && (m.durationSec > 0.5);
    qreal titleY;

    if (showProgress) {
        const qreal barW  = LCD_R * 0.95;
        const qreal barH  = LCD_R * 0.022;
        const qreal barX  = cx - barW / 2.0;
        const qreal barY  = coverRect.bottom() + LCD_R * 0.07;
        const qreal barRx = barH / 2.0;
        const float frac  = float(std::clamp(m.positionSec / m.durationSec, 0.0, 1.0));
        p.setPen(Qt::NoPen);
        { QColor tr = fg; tr.setAlphaF(0.20); p.setBrush(tr);
          p.drawRoundedRect(QRectF(barX, barY, barW, barH), barRx, barRx); }
        if (frac > 0.f) {
            p.setBrush(fg);
            p.drawRoundedRect(QRectF(barX, barY, std::max<qreal>(barH, barW * frac), barH),
                              barRx, barRx);
        }

        auto fmtTime = [](double sec) -> QString {
            if (sec < 0 || !std::isfinite(sec)) sec = 0;
            const int t = int(sec + 0.5);
            return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
        };
        {
            const qreal y = barY + barH + LCD_R * 0.105;
            QFont f = nzxtFont(int(LCD_R * 0.078), QFont::Normal);
            QColor c = fg; c.setAlphaF(0.70);
            const QString ts = fmtTime(m.positionSec) + QStringLiteral("  /  ") + fmtTime(m.durationSec);
            drawStyledText(p, QRectF(0, y - LCD_R * 0.08, LCD_W, LCD_R * 0.16),
                           Qt::AlignHCenter | Qt::AlignVCenter, ts, f, c, QColor(0, 0, 0, 0));
        }
        titleY = barY + barH + LCD_R * 0.265;
    } else {
        // Pas de barre/temps (média sans durée, ou option désactivée) → titre rapproché sous la pochette.
        titleY = coverRect.bottom() + LCD_R * 0.20;
    }

    // ── Titre ──
    {
        const qreal y = titleY;
        QFont f = nzxtFont(int(LCD_R * 0.11), QFont::DemiBold);
        drawScrollingText(p, cx, y, LCD_R * 0.22, widthAt(y) - LCD_R * 0.10,
                          m.title, f, cfg.npTitleFill, cfg.npTitleOutline, npElapsed);
    }
}

std::unique_ptr<QMovie> FrameRenderer::loadGif(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        qWarning() << "[Renderer] GIF file not found:" << path;
        return nullptr;
    }
    auto m = std::make_unique<QMovie>(path);
    if (!m->isValid()) {
        qWarning() << "[Renderer] Invalid GIF:" << path << "—" << m->lastError();
        return nullptr;
    }
    m->setCacheMode(QMovie::CacheAll);
    m->start();
    if (m->frameCount() <= 0) m->jumpToFrame(0);
    qDebug() << "[Renderer] GIF loaded:" << path << "— frames:" << m->frameCount();
    return m;
}

// drawNZXTLogo : stub vide supprime (jamais appele).

} // namespace NZXTKraken
