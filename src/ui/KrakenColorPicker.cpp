#include "KrakenColorPicker.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QContextMenuEvent>
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QToolButton>
#include <QScreen>
#include <QEvent>
#include <QtMath>
#include <algorithm>
#include "KrakenOpenRGBSettings.h"

namespace NZXTKraken {

// ════════════════════════════════════════════════════════════════════════════
// Patch : Opérateur d'égalité pour GradStop
// ════════════════════════════════════════════════════════════════════════════
inline bool operator==(const GradStop& a, const GradStop& b) {
    return (a.offset == b.offset) && (a.color == b.color);
}

// ════════════════════════════════════════════════════════════════════════════
// Helper pour le thème (OpenRGB Dark/Light)
// ════════════════════════════════════════════════════════════════════════════
namespace {
    struct PopoverPalette {
        QString bg;             // Fond principal du popover
        QString historyBg;      // Fond spécifique (utilisé uniquement pour les onglets Text/Outline)
        QString surface;        // Boutons et champs
        QString border;         // Bordures séparatrices
        QString text;
        QString textMuted;
        QString hover;
        QString tabActiveBorder;// Bordure de l'onglet actif
        bool    dark;
    };

    PopoverPalette resolvePopoverPalette(const QWidget* anchor) {
        QColor base = anchor ? anchor->palette().color(QPalette::Window)
                             : QApplication::palette().color(QPalette::Window);
        const bool dark = base.lightnessF() < 0.5 || base.alpha() == 0;

        if (dark) {
            return {
                "#111113",   // bg
                "#1A1A1E",   // historyBg (uniquement pour le fond des onglets)
                "#1F1F23",   // surface
                "#2A2A2E",   // border
                "#FFFFFF",   // text
                "#666666",   // textMuted
                "#22222A",   // hover
                "#FFFFFF",   // tabActiveBorder (Blanc en mode dark)
                true
            };
        }
        return {
            "#F0F0F2",   // bg
            "#E6E6E9",   // historyBg (uniquement pour le fond des onglets)
            "#EBEBEE",   // surface
            "#D0D0D4",   // border
            "#1A1A1E",   // text
            "#888888",   // textMuted
            "#DDDDE0",   // hover
            "#000000",   // tabActiveBorder (Noir en mode clair)
            false
        };
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Historiques globaux
// ════════════════════════════════════════════════════════════════════════════
static QList<QColor> s_colorHistory;
static void pushHistory(const QColor& c)
{
    if (!c.isValid() || c.alpha() == 0) return;
    s_colorHistory.removeAll(c);
    s_colorHistory.prepend(c);
    while (s_colorHistory.size() > 5) s_colorHistory.removeLast();
}

static QList<GradientStops> s_gradientHistory;
static void pushGradientHistory(const GradientStops& s)
{
    if (s.isEmpty()) return;
    s_gradientHistory.removeAll(s);
    s_gradientHistory.prepend(s);
    while (s_gradientHistory.size() > 5) s_gradientHistory.removeLast();
}

const QList<QColor>& kColorHistory() { return s_colorHistory; }
const QList<GradientStops>& kGradientHistory() { return s_gradientHistory; }
void kSetColorHistory(const QList<QColor>& list) {
    s_colorHistory.clear();
    for (const auto& c : list) {
        if (c.isValid() && c.alpha() != 0) s_colorHistory.append(c);
        if (s_colorHistory.size() >= 5) break;
    }
}
void kSetGradientHistory(const QList<GradientStops>& list) {
    s_gradientHistory.clear();
    for (const auto& g : list) {
        if (!g.isEmpty()) s_gradientHistory.append(g);
        if (s_gradientHistory.size() >= 5) break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ColorSwatchButton
// ════════════════════════════════════════════════════════════════════════════
ColorSwatchButton::ColorSwatchButton(QWidget* parent) : QPushButton(parent)
{
    setFixedSize(32, 24);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet("QPushButton{background:transparent;border:none;padding:0;}");
}
void ColorSwatchButton::setSolid(const QColor& c)
{
    m_kind = Solid; m_fill = c; m_outline = QColor(0,0,0,0);
    m_stops.clear(); update();
}
void ColorSwatchButton::setFillOutline(const QColor& fill, const QColor& outline)
{
    m_kind = FillOutline; m_fill = fill; m_outline = outline;
    m_stops.clear(); update();
}
void ColorSwatchButton::setGradient(const GradientStops& stops)
{
    m_kind = Gradient; m_stops = stops; update();
}
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
void ColorSwatchButton::enterEvent(QEnterEvent* e)
#else
void ColorSwatchButton::enterEvent(QEvent* e)
#endif
{
    QPushButton::enterEvent(e);
    m_hovered = true;
    update();
}
void ColorSwatchButton::leaveEvent(QEvent* e)
{
    QPushButton::leaveEvent(e);
    m_hovered = false;
    update();
}
void ColorSwatchButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    // On désactive l'antialiasing pour avoir des bords ultra-nets (pixel perfect)
    p.setRenderHint(QPainter::Antialiasing, false);

    // CORRECTION : On utilise la palette globale de l'application pour savoir si 
    // l'UI est sombre. Ça évite le bug du bouton transparent.
    QColor globalBg = QApplication::palette().color(QPalette::Window);
    bool dark = globalBg.lightnessF() < 0.5;
    
    // Couleurs d'outline reprises des panels de couleurs (palette popover
    // "border") : un gris foncé et un gris clair, jamais du noir/blanc pur.
    const QColor OUTLINE_DARK  = QColor("#2A2A2E");
    const QColor OUTLINE_LIGHT = QColor("#D0D0D4");

    // Contour des éléments de l'UI générale : gris foncé en thème sombre,
    // gris clair en thème clair (mêmes couleurs que les outlines des panels).
    QColor borderCol = dark ? OUTLINE_DARK : OUTLINE_LIGHT;

    // Pointillés des bordures "invisible" (outline absent) : même principe que
    // dans les panels — gris foncé si le remplissage est clair, gris clair s'il
    // est foncé (pas de noir/blanc pur). Un remplissage transparent est rendu
    // sur fond blanc, donc traité comme clair → pointillés foncés.
    auto dashColorFor = [&](const QColor& fill) -> QColor {
        if (fill.alpha() == 0) return OUTLINE_DARK;
        return fill.lightnessF() >= 0.5 ? OUTLINE_DARK : OUTLINE_LIGHT;
    };

    bool isPopover = property("popoverStyle").toBool();
    QRect r = rect(); // On utilise QRect (entiers) au lieu de QRectF pour un rendu parfait

    // ──────────────────────────────────────────────────────────────────────────
    // ASTUCE : Fonction maison pour dessiner la bordure STRICTEMENT vers l'intérieur.
    // L'extérieur de la case (r) ne bougera jamais, le grossissement se fait en dedans.
    // ──────────────────────────────────────────────────────────────────────────
    auto drawInsideBorder = [&](const QRect& box, const QColor& color, int thickness) {
        p.fillRect(box.x(), box.y(), box.width(), thickness, color); // Haut
        p.fillRect(box.x(), box.y() + box.height() - thickness, box.width(), thickness, color); // Bas
        p.fillRect(box.x(), box.y() + thickness, thickness, box.height() - 2 * thickness, color); // Gauche
        p.fillRect(box.x() + box.width() - thickness, box.y() + thickness, thickness, box.height() - 2 * thickness, color); // Droite
    };

    if (isPopover) {
        // Le contour des swatches reprend la couleur d'outline des boutons
        // (border), centralisée plus haut : OUTLINE_DARK / OUTLINE_LIGHT.
        const QColor popBorderCol = dark ? OUTLINE_DARK : OUTLINE_LIGHT;
        if (m_hovered) {
            // Hover : seul le contour s'élargit (2px) ; la couleur touche la
            // bordure, aucun gap d'une autre couleur n'apparaît.
            drawInsideBorder(r, popBorderCol, 2);
            QRect innerR = r.adjusted(2, 2, -2, -2); // 2px de bordure, pas de gap

            if (m_kind == Gradient) {
                p.fillRect(innerR, makeLinearGradient(innerR, m_stops));
            } else {
                if (m_fill.alpha() == 0) {
                    p.fillRect(innerR, Qt::white);
                    p.setPen(QPen(QColor(255, 0, 0), 1));
                    p.drawLine(innerR.topLeft(), innerR.bottomRight());
                } else {
                    p.fillRect(innerR, m_fill);
                }
            }
        } else {
            // Normal : bordure de 1px, PAS DE GAP (la couleur touche la bordure)
            drawInsideBorder(r, popBorderCol, 1);
            QRect innerR = r.adjusted(1, 1, -1, -1); // 1px d'écart (juste la bordure)

            if (m_kind == Gradient) {
                p.fillRect(innerR, makeLinearGradient(innerR, m_stops));
            } else {
                if (m_fill.alpha() == 0) {
                    p.fillRect(innerR, Qt::white);
                    p.setPen(QPen(QColor(255, 0, 0), 1));
                    p.drawLine(innerR.topLeft(), innerR.bottomRight());
                } else {
                    p.fillRect(innerR, m_fill);
                }
            }
        }
    } else {
        // STYLE UI GENERALE (comme les rectangles de ta page principale)
        // Pas d'effet de grossissement au survol ici (réservé aux popovers).
        int borderW = 1;
        
        if (m_kind == Gradient) {
            p.fillRect(r, makeLinearGradient(r, m_stops));
            drawInsideBorder(r, borderCol, borderW);
        } else if (m_kind == FillOutline) {
            p.fillRect(r, m_fill);
            if (m_outline.alpha() == 0) {
                // Bordure "invisible" : pointillé fin (1px), couleur contrastée
                // selon le remplissage (clair → noir, foncé → blanc).
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(dashColorFor(m_fill), 1, Qt::DashLine));
                p.drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
            } else {
                drawInsideBorder(r, m_outline, borderW + 1); 
            }
        } else {
            // Solid
            if (m_fill.alpha() == 0) {
                // Couleur "transparente" : fond blanc + pointillé contrasté (noir).
                p.fillRect(r, Qt::white);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(dashColorFor(m_fill), 1, Qt::DashLine));
                p.drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
            } else {
                p.fillRect(r, m_fill);
                drawInsideBorder(r, borderCol, borderW);
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SvPad / HueSlider / CustomColorPicker / GradientStopsEditor
// ════════════════════════════════════════════════════════════════════════════
SvPad::SvPad(QWidget* parent) : QWidget(parent) {
    setMinimumSize(190, 110); setCursor(Qt::CrossCursor);
}
void SvPad::setHue(float h) { m_hue = h; update(); }
void SvPad::setSV(float s, float v) { m_sat = s; m_val = v; update(); }
void SvPad::updateFromPos(const QPoint& p) {
    const float s = std::clamp(float(p.x()) / float(width()), 0.f, 1.f);
    const float v = 1.f - std::clamp(float(p.y()) / float(height()), 0.f, 1.f);
    m_sat = s; m_val = v; emit svChanged(s, v); update();
}
void SvPad::mousePressEvent(QMouseEvent* e) { updateFromPos(e->pos()); }
void SvPad::mouseMoveEvent(QMouseEvent* e)  { if (e->buttons() & Qt::LeftButton) updateFromPos(e->pos()); }
void SvPad::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    QLinearGradient h(r.left(), 0, r.right(), 0);
    h.setColorAt(0.0, Qt::white); h.setColorAt(1.0, QColor::fromHsvF(m_hue/360.f, 1.f, 1.f));
    p.fillRect(r, h);
    QLinearGradient v(0, r.top(), 0, r.bottom());
    v.setColorAt(0.0, QColor(0,0,0,0)); v.setColorAt(1.0, Qt::black);
    p.fillRect(r, v);
    p.setPen(QPen(QColor(0x2A,0x2A,0x2E), 1)); p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
    const qreal cx = m_sat * width(); const qreal cy = (1.f - m_val) * height();
    p.setPen(QPen(Qt::white, 2)); p.setBrush(Qt::NoBrush); p.drawEllipse(QPointF(cx, cy), 5, 5);
    p.setPen(QPen(Qt::black, 1)); p.drawEllipse(QPointF(cx, cy), 6, 6);
}

HueSlider::HueSlider(QWidget* parent) : QWidget(parent) {
    setFixedHeight(16); setMinimumWidth(190); setCursor(Qt::PointingHandCursor);
}
void HueSlider::setHue(float h) { m_hue = h; update(); }
void HueSlider::updateFromPos(int x) {
    m_hue = std::clamp(float(x) / float(width()), 0.f, 1.f) * 360.f; emit hueChanged(m_hue); update();
}
void HueSlider::mousePressEvent(QMouseEvent* e) { updateFromPos(e->pos().x()); }
void HueSlider::mouseMoveEvent(QMouseEvent* e)  { if (e->buttons() & Qt::LeftButton) updateFromPos(e->pos().x()); }
void HueSlider::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect().adjusted(0, 1, 0, -1);
    QLinearGradient g(r.left(), 0, r.right(), 0);
    g.setColorAt(0.00, QColor(0xFF,0,0));     g.setColorAt(1.0/6.0, QColor(0xFF,0xFF,0));
    g.setColorAt(2.0/6.0, QColor(0,0xFF,0));  g.setColorAt(3.0/6.0, QColor(0,0xFF,0xFF));
    g.setColorAt(4.0/6.0, QColor(0,0,0xFF));  g.setColorAt(5.0/6.0, QColor(0xFF,0,0xFF));
    g.setColorAt(1.0,     QColor(0xFF,0,0));
    QPainterPath path; path.addRoundedRect(r, r.height()/2, r.height()/2);
    p.fillPath(path, g);
    // Garde la bulle entièrement visible (au-dessus du fond) même aux
    // extrémités : on borne le centre pour que le cercle (rayon 6 + 1px de
    // contour) ne déborde pas hors du widget, où il serait rogné.
    const qreal handleR = 6.0;
    const qreal margin  = handleR + 1.0;
    const qreal cx = std::clamp(qreal(m_hue) / 360.0 * width(), margin, qreal(width()) - margin);
    p.setPen(QPen(Qt::black, 1)); p.setBrush(Qt::white);
    p.drawEllipse(QPointF(cx, height()/2.0), handleR, handleR);
}

CustomColorPicker::CustomColorPicker(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this); lay->setContentsMargins(0,0,0,0); lay->setSpacing(6);
    m_sv = new SvPad(this); m_hueSlider = new HueSlider(this);
    lay->addWidget(m_sv); lay->addWidget(m_hueSlider);
    auto* inputs = new QHBoxLayout; inputs->setSpacing(4);
    // Champs Hex/R/G/B derives de la palette popover (suivent le theme OpenRGB).
    const auto pal = resolvePopoverPalette(this);
    const QString inputBg = pal.dark ? QStringLiteral("#0A0A0C") : QStringLiteral("#FFFFFF");
    auto mkLbl = [pal](const QString& s) { auto* l = new QLabel(s); l->setStyleSheet(QString("color:%1;font-size:10px;").arg(pal.textMuted)); return l; };
    const QString inputCss = QString("QLineEdit, QSpinBox{background:%1;color:%2;border:1px solid %3;border-radius:3px;padding:1px 3px;font-size:10px;}").arg(inputBg, pal.text, pal.border);
    m_hex = new QLineEdit; m_hex->setStyleSheet(inputCss); m_hex->setMaxLength(7); m_hex->setFixedWidth(68);
    m_r = new QSpinBox; m_r->setRange(0,255); m_r->setFixedWidth(46); m_r->setStyleSheet(inputCss);
    m_g = new QSpinBox; m_g->setRange(0,255); m_g->setFixedWidth(46); m_g->setStyleSheet(inputCss);
    m_b = new QSpinBox; m_b->setRange(0,255); m_b->setFixedWidth(46); m_b->setStyleSheet(inputCss);
    inputs->addWidget(mkLbl("Hex")); inputs->addWidget(m_hex);
    inputs->addWidget(mkLbl("R")); inputs->addWidget(m_r);
    inputs->addWidget(mkLbl("G")); inputs->addWidget(m_g);
    inputs->addWidget(mkLbl("B")); inputs->addWidget(m_b);
    inputs->addStretch(); lay->addLayout(inputs);
    setColor(QColor(Qt::white));
    connect(m_sv, &SvPad::svChanged, this, [this](float s, float v) { if (m_updating) return; emitColor(QColor::fromHsvF(m_hueSlider->hue()/360.f, s, v)); });
    connect(m_hueSlider, &HueSlider::hueChanged, this, [this](float h) { if (m_updating) return; m_sv->setHue(h); emitColor(QColor::fromHsvF(h/360.f, m_sv->saturation(), m_sv->value())); });
    connect(m_hex, &QLineEdit::editingFinished, this, [this]() {
        if (m_updating) return;
        QString t = m_hex->text().trimmed();
        if (!t.startsWith('#')) t.prepend('#');
        const bool bgr = (OpenRGBSettings::hexFormat() == QLatin1String("BGR"));
        QColor c;
        if (bgr && t.length() == 7) {
            bool okB = false, okG = false, okR = false;
            const int b = t.mid(1, 2).toInt(&okB, 16);
            const int g = t.mid(3, 2).toInt(&okG, 16);
            const int r = t.mid(5, 2).toInt(&okR, 16);
            if (okB && okG && okR) c = QColor(r, g, b);
        } else {
            c = QColor(t);
        }
        if (c.isValid()) emitColor(c); else syncInputsFromColor();
    });
    auto onRGB = [this]() { if (m_updating) return; emitColor(QColor(m_r->value(), m_g->value(), m_b->value())); };
    connect(m_r, QOverload<int>::of(&QSpinBox::valueChanged), this, onRGB);
    connect(m_g, QOverload<int>::of(&QSpinBox::valueChanged), this, onRGB);
    connect(m_b, QOverload<int>::of(&QSpinBox::valueChanged), this, onRGB);
}
void CustomColorPicker::setColor(const QColor& c) { if (!c.isValid()) return; m_color = c; syncInputsFromColor(); }
void CustomColorPicker::emitColor(const QColor& c) { m_color = c; syncInputsFromColor(); emit colorChanged(c); }
void CustomColorPicker::syncInputsFromColor() {
    m_updating = true; qreal h, s, v; m_color.getHsvF(&h, &s, &v); if (h < 0) h = 0;
    m_hueSlider->setHue(h * 360.f); m_sv->setHue(h * 360.f); m_sv->setSV(s, v);
    {
        const bool bgr = (OpenRGBSettings::hexFormat() == QLatin1String("BGR"));
        m_hex->setText(bgr ? QString::asprintf("#%02X%02X%02X", m_color.blue(), m_color.green(), m_color.red())
                           : m_color.name().toUpper());
    }
    m_r->setValue(m_color.red()); m_g->setValue(m_color.green()); m_b->setValue(m_color.blue());
    m_updating = false;
}

GradientStopsEditor::GradientStopsEditor(QWidget* parent) : QWidget(parent) {
    setFixedHeight(54); setMinimumWidth(220); setMouseTracking(true); setContextMenuPolicy(Qt::PreventContextMenu);
}
void GradientStopsEditor::setStops(const GradientStops& s) { m_stops = s; if (m_selected >= m_stops.size()) m_selected = m_stops.isEmpty() ? -1 : 0; update(); emit selectedChanged(m_selected); }
void GradientStopsEditor::setSelectedIndex(int idx) { if (idx >= 0 && idx < m_stops.size() && m_selected != idx) { m_selected = idx; update(); emit selectedChanged(m_selected); } }
QRectF GradientStopsEditor::barRect() const { return QRectF(8, 24, width() - 16, 22); }
QRectF GradientStopsEditor::handleRect(int idx) const { if (idx < 0 || idx >= m_stops.size()) return QRectF(); const QRectF bar = barRect(); const qreal x = bar.left() + bar.width() * std::clamp(m_stops[idx].offset/100.f, 0.f, 1.f); return QRectF(x - 7, 4, 14, 14); }
int GradientStopsEditor::stopAtPos(const QPoint& p) const { for (int i = m_stops.size()-1; i >= 0; --i) { if (handleRect(i).contains(p)) return i; } return -1; }
void GradientStopsEditor::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); const QRectF bar = barRect();
    p.setBrush(makeLinearGradient(bar, m_stops)); p.setPen(QPen(QColor(0x2A,0x2A,0x2E), 1)); p.drawRoundedRect(bar, 4, 4);
    for (int i = 0; i < m_stops.size(); ++i) { const QRectF hr = handleRect(i); const bool selected = (i == m_selected);
        p.setBrush(m_stops[i].color); p.setPen(QPen(selected ? Qt::white : QColor(0x44,0x44,0x44), 2)); p.drawEllipse(hr); }
}
void GradientStopsEditor::mousePressEvent(QMouseEvent* e) {
    const QRectF bar = barRect();
    if (e->button() == Qt::LeftButton) {            // clic gauche = sélectionner (+ déplacer en glissant)
        int idx = stopAtPos(e->pos());
        if (idx >= 0) { m_selected = idx; m_dragging = idx; emit selectedChanged(idx); update(); }
        else if (bar.contains(e->pos())) {          // barre vide : ajoute un stop puis le sélectionne
            const float pct = std::clamp(float(e->pos().x() - bar.left()) / float(bar.width()), 0.f, 1.f) * 100.f;
            const QColor c = colorAtPercent(m_stops, pct);
            m_stops.append({ pct, c }); m_selected = m_stops.size() - 1; m_dragging = m_selected;
            emit stopsChanged(m_stops); emit selectedChanged(m_selected); update();
        }
        return;
    }
    if (e->button() == Qt::RightButton) {           // clic droit = supprimer le stop sous le curseur
        int idx = stopAtPos(e->pos());
        if (idx >= 0 && m_stops.size() > 1) { m_stops.removeAt(idx); if (m_selected >= m_stops.size()) m_selected = m_stops.size() - 1; emit stopsChanged(m_stops); emit selectedChanged(m_selected); update(); }
        return;
    }
}
void GradientStopsEditor::mouseMoveEvent(QMouseEvent* e) { if (m_dragging < 0 || !(e->buttons() & Qt::LeftButton)) return; const QRectF bar = barRect(); const float pct = std::clamp(float(e->pos().x() - bar.left()) / float(bar.width()), 0.f, 1.f) * 100.f; if (m_dragging < m_stops.size()) { m_stops[m_dragging].offset = pct; emit stopsChanged(m_stops); } update(); }
void GradientStopsEditor::mouseReleaseEvent(QMouseEvent*) { m_dragging = -1; m_markDelete = false; update(); }
void GradientStopsEditor::contextMenuEvent(QContextMenuEvent* e) { e->accept(); }


// ════════════════════════════════════════════════════════════════════════════
// KrakenColorPopover
// ════════════════════════════════════════════════════════════════════════════
KrakenColorPopover::KrakenColorPopover(QWidget* parent) : QFrame(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("KrakenPopover");

    const auto pal = resolvePopoverPalette(parent);

    setStyleSheet(QString(
        "QLabel{color:%1;font-size:11px;background:transparent;border:none;}"
        "QPushButton{background:%2;color:%1;border:1px solid %3;"
        "border-radius:5px;padding:5px 10px;font-size:11px;}"
        "QPushButton:hover{background:%4;}"
    ).arg(pal.text, pal.surface, pal.border, pal.hover));
}

void KrakenColorPopover::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto pal = resolvePopoverPalette(parentWidget());
    
    // Dessin du fond global
    p.setBrush(QColor(pal.bg));
    p.setPen(QPen(QColor(pal.border), 1));
    p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);
    
    QFrame::paintEvent(e);
}

void KrakenColorPopover::anchorTo(QWidget* widget) {
    if (!widget) return; QPoint global = widget->mapToGlobal(QPoint(0, widget->height())); adjustSize();
    int x = global.x() + widget->width()/2 - width()/2; int y = global.y() + 6;
    QRect avail = QApplication::primaryScreen() ? QApplication::primaryScreen()->availableGeometry() : QRect(0,0,1920,1080);
    if (x < avail.left() + 6) x = avail.left() + 6;
    if (x + width() > avail.right() - 6) x = avail.right() - width() - 6;
    if (y + height() > avail.bottom() - 6) { y = widget->mapToGlobal(QPoint(0,0)).y() - height() - 6; }
    move(x, y);
}
void KrakenColorPopover::showEvent(QShowEvent* e) { QFrame::showEvent(e); qApp->installEventFilter(this); setFocus(); }
void KrakenColorPopover::hideEvent(QHideEvent* e) { qApp->removeEventFilter(this); QFrame::hideEvent(e); }
bool KrakenColorPopover::eventFilter(QObject*, QEvent*) { return false; }
void KrakenColorPopover::keyPressEvent(QKeyEvent* e) { if (e->key() == Qt::Key_Escape) { hide(); return; } QFrame::keyPressEvent(e); }


// ════════════════════════════════════════════════════════════════════════════
// Helpers UI partagés
// ════════════════════════════════════════════════════════════════════════════
static QWidget* makeHistoryBar(QWidget* parent, const QList<QColor>& history, std::function<void(QColor)> onPick)
{
    const auto pal = resolvePopoverPalette(parent);
    auto* container = new QFrame; container->setObjectName("HistorySection");
    
    // Fond transparent pour s'intégrer uniformément
    container->setStyleSheet("#HistorySection { background:transparent; border:none; }");
    
    auto* lay = new QHBoxLayout(container); lay->setContentsMargins(16, 12, 16, 12); lay->setSpacing(8);

    if (history.isEmpty()) {
        lay->addStretch();
        auto* emptyLbl = new QLabel("No recent");
        emptyLbl->setStyleSheet(QString("color:%1;font-size:11px;font-style:italic;").arg(pal.textMuted));
        emptyLbl->setAlignment(Qt::AlignCenter);
        lay->addWidget(emptyLbl);
        lay->addStretch();
        container->setMinimumHeight(44);
    } else {
        int n = std::min(history.size(), 8);
        for (int i = 0; i < n; ++i) {
            auto* b = new ColorSwatchButton;
            b->setProperty("popoverStyle", true);
            b->setFixedSize(24, 24);
            b->setSolid(history[i]);
            b->setToolTip(history[i].name());
            QColor c = history[i];
            QObject::connect(b, &QPushButton::clicked, b, [c, onPick]() { onPick(c); });
            lay->addWidget(b);
        }
        lay->addStretch();
    }
    return container;
}

static QWidget* makeGradientHistoryBar(QWidget* parent, const QList<GradientStops>& history, std::function<void(GradientStops)> onPick)
{
    const auto pal = resolvePopoverPalette(parent);
    auto* container = new QFrame; container->setObjectName("GradientHistorySection");
    
    // Fond transparent pour s'intégrer uniformément
    container->setStyleSheet("#GradientHistorySection { background:transparent; border:none; }");
    
    auto* lay = new QHBoxLayout(container); lay->setContentsMargins(16, 12, 16, 12); lay->setSpacing(8);

    if (history.isEmpty()) {
        lay->addStretch();
        auto* emptyLbl = new QLabel("No recent");
        emptyLbl->setStyleSheet(QString("color:%1;font-size:11px;font-style:italic;").arg(pal.textMuted));
        emptyLbl->setAlignment(Qt::AlignCenter); lay->addWidget(emptyLbl); lay->addStretch();
        container->setMinimumHeight(44);
    } else {
        int n = std::min(history.size(), 8);
        for (int i = 0; i < n; ++i) {
            auto* b = new ColorSwatchButton;
            b->setProperty("popoverStyle", true);
            b->setFixedSize(24, 24);
            b->setGradient(history[i]);
            GradientStops s = history[i];
            QObject::connect(b, &QPushButton::clicked, b, [s, onPick]() { onPick(s); });
            lay->addWidget(b);
        }
        lay->addStretch();
    }
    return container;
}

static QFrame* wrapInPresetSection(QWidget*, QLayout* gridLayout) {
    auto* container = new QFrame; container->setObjectName("PresetSection");
    container->setStyleSheet("#PresetSection { background:transparent; border:none; }");
    auto* lay = new QVBoxLayout(container); lay->setContentsMargins(16, 10, 16, 10); lay->addLayout(gridLayout);
    return container;
}

static QGridLayout* makeSwatchGrid(QWidget* parent, const QList<QColor>& colors, bool firstIsTransparent, std::function<void(QColor)> onPick)
{
    auto* grid = new QGridLayout; grid->setSpacing(10);
    for (int i = 0; i < colors.size(); ++i) {
        const QColor c = colors[i];
        const bool transp = firstIsTransparent && (i == 0);
        auto* b = new ColorSwatchButton;
        b->setProperty("popoverStyle", true);
        b->setFixedSize(24, 24);
        b->setSolid(transp ? QColor(0,0,0,0) : c);
        b->setToolTip(transp ? "Transparent (no outline)" : c.name());
        QObject::connect(b, &QPushButton::clicked, b, [c, transp, onPick]() { onPick(transp ? QColor(0,0,0,0) : c); });
        grid->addWidget(b, i / 5, i % 5);
    }
    return grid;
}

static QGridLayout* makeGradientSwatchGrid(QWidget* parent, const QList<GradientStops>& presets, std::function<void(GradientStops)> onPick)
{
    auto* grid = new QGridLayout; grid->setSpacing(10);
    for (int i = 0; i < presets.size(); ++i) {
        auto* b = new ColorSwatchButton;
        b->setProperty("popoverStyle", true);
        b->setFixedSize(24, 24);
        b->setGradient(presets[i]);
        QObject::connect(b, &QPushButton::clicked, b, [=]() { onPick(presets[i]); });
        grid->addWidget(b, i / 5, i % 5);
    }
    return grid;
}


// ════════════════════════════════════════════════════════════════════════════
// SolidColorPopover
// ════════════════════════════════════════════════════════════════════════════
SolidColorPopover::SolidColorPopover(const QColor& initial, QWidget* parent)
    : KrakenColorPopover(parent), m_color(initial)
{
    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(0, 0, 0, 12);
    root->setSpacing(0);

    auto onPick = [this](QColor c) { m_color = c; pushHistory(c); m_accepted = true; hide(); };

    auto* stack = new QStackedWidget;
    const auto pal = resolvePopoverPalette(this);

    // Page 0 : Swatches
    auto* swatchPage = new QWidget;
    auto* swatchLay = new QVBoxLayout(swatchPage);
    swatchLay->setContentsMargins(0,0,0,0); swatchLay->setSpacing(4);

    swatchLay->addWidget(makeHistoryBar(this, s_colorHistory, onPick));
    
    auto* sep0 = new QFrame; sep0->setFrameShape(QFrame::HLine); sep0->setStyleSheet(QString("background:%1;border:none; margin: 0 16px;").arg(pal.border)); sep0->setFixedHeight(1);
    swatchLay->addWidget(sep0);
    
    swatchLay->addWidget(wrapInPresetSection(this, makeSwatchGrid(this, SWATCH_FILL_15, false, onPick)));

    auto* swRow = new QHBoxLayout;
    swRow->setContentsMargins(16, 4, 16, 0);
    auto* btnCustom = new QPushButton("Custom"); auto* btnCancel0 = new QPushButton("Cancel");
    swRow->addWidget(btnCustom); swRow->addWidget(btnCancel0); swatchLay->addLayout(swRow);
    stack->addWidget(swatchPage);

    // Page 1 : Custom
    auto* customPage = new QWidget;
    auto* customLay = new QVBoxLayout(customPage);
    customLay->setContentsMargins(16, 16, 16, 0); customLay->setSpacing(10);

    auto* picker = new CustomColorPicker; picker->setColor(initial); customLay->addWidget(picker);
    auto* cRow = new QHBoxLayout;
    auto* btnSwatch = new QPushButton("Swatch"); auto* btnCancel1 = new QPushButton("Cancel");
    cRow->addWidget(btnSwatch); cRow->addWidget(btnCancel1); customLay->addLayout(cRow);
    stack->addWidget(customPage);

    root->addWidget(stack);

    auto updateStackHeight = [stack](int index) {
        for (int i = 0; i < stack->count(); ++i) stack->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        stack->widget(index)->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum); stack->adjustSize();
    };
    connect(stack, &QStackedWidget::currentChanged, updateStackHeight);
    updateStackHeight(stack->currentIndex());

    connect(btnCustom, &QPushButton::clicked, this, [=]() { stack->setCurrentIndex(1); btnSwatch->setAttribute(Qt::WA_UnderMouse, false); btnSwatch->update(); });
    connect(btnCancel0, &QPushButton::clicked, this, [this]() { hide(); });
    connect(btnSwatch, &QPushButton::clicked, this, [this, picker]() { m_color = picker->color(); pushHistory(m_color); m_accepted = true; hide(); });
    connect(picker, &CustomColorPicker::colorChanged, this, [this](const QColor& c){ m_color = c; });
    connect(btnCancel1, &QPushButton::clicked, this, [this]() { hide(); });
}
bool SolidColorPopover::pick(QWidget* anchor, QColor& inOut) {
    SolidColorPopover pop(inOut); pop.anchorTo(anchor); pop.show();
    QEventLoop loop; QObject::connect(&pop, &QObject::destroyed, &loop, &QEventLoop::quit);
    while (pop.isVisible()) QApplication::processEvents(QEventLoop::WaitForMoreEvents);
    if (pop.m_accepted) { inOut = pop.m_color; return true; } return false;
}

// ════════════════════════════════════════════════════════════════════════════
// TextColorPopover 
// ════════════════════════════════════════════════════════════════════════════
TextColorPopover::TextColorPopover(const QColor& fill, const QColor& outline, QWidget* parent)
    : KrakenColorPopover(parent), m_fill(fill), m_outline(outline)
{
    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(0, 0, 0, 12);
    root->setSpacing(0);

    const auto pal = resolvePopoverPalette(this);
    auto* rootStack = new QStackedWidget;

    // ──────────────── PAGE 0 : VUE SWATCH (Avec Onglets Text / Outline) ────────────────
    auto* pageSwatch = new QWidget;
    auto* laySwatch = new QVBoxLayout(pageSwatch);
    laySwatch->setContentsMargins(0,0,0,0); laySwatch->setSpacing(4);

    // Onglets Centrés (Leur conteneur garde la couleur de fond spécifique historyBg)
    auto* tabsContainer = new QFrame;
    tabsContainer->setStyleSheet(QString(
        "background:%1; border:none; border-top-left-radius: 9px; border-top-right-radius: 9px;"
    ).arg(pal.historyBg));
    auto* tabsRow = new QHBoxLayout(tabsContainer);
    tabsRow->setContentsMargins(16, 8, 16, 0);
    tabsRow->addStretch();
    auto* tabText = new QPushButton("Text");
    auto* tabOutl = new QPushButton("Outline");

    auto styleTab = [pal](QPushButton* b, bool active) {
        b->setCheckable(true);
        b->setChecked(active);
        b->setStyleSheet(QString(
            "QPushButton{background:transparent;color:%1;"
            "border:none; border-bottom:2px solid %2;"
            "border-radius:0;padding:4px 12px;"
            "font-size:12px;font-weight:%3;}"
            "QPushButton:hover{color:%4;}")
            .arg(active ? pal.text : pal.textMuted,
                 active ? pal.tabActiveBorder : "transparent", // Utilisation de la nouvelle palette
                 active ? QStringLiteral("600") : QStringLiteral("400"),
                 pal.text));
    };
    styleTab(tabText, true); styleTab(tabOutl, false);
    tabsRow->addWidget(tabText); tabsRow->addWidget(tabOutl);
    tabsRow->addStretch();
    laySwatch->addWidget(tabsContainer);

    auto* gridStack = new QStackedWidget;

    auto onPickText = [this](QColor c) { m_fill = c; pushHistory(c); m_accepted = true; hide(); };
    auto onPickOutl = [this](QColor c) { m_outline = c; pushHistory(c); m_accepted = true; hide(); };

    // --- Grille Text ---
    auto* gridText = new QWidget; auto* lText = new QVBoxLayout(gridText);
    lText->setContentsMargins(0,0,0,0); lText->setSpacing(4);
    
    auto* histBoxT = makeHistoryBar(this, s_colorHistory, onPickText);
    lText->addWidget(histBoxT);
    
    auto* sep1 = new QFrame; sep1->setFrameShape(QFrame::HLine); sep1->setStyleSheet(QString("background:%1;border:none; margin: 0 16px;").arg(pal.border)); sep1->setFixedHeight(1);
    lText->addWidget(sep1); lText->addWidget(wrapInPresetSection(this, makeSwatchGrid(this, SWATCH_FILL_15, false, onPickText)));
    gridStack->addWidget(gridText);

    // --- Grille Outline ---
    auto* gridOutl = new QWidget; auto* lOutl = new QVBoxLayout(gridOutl);
    lOutl->setContentsMargins(0,0,0,0); lOutl->setSpacing(4);
    
    auto* histBoxO = makeHistoryBar(this, s_colorHistory, onPickOutl);
    lOutl->addWidget(histBoxO);
    
    auto* sep2 = new QFrame; sep2->setFrameShape(QFrame::HLine); sep2->setStyleSheet(QString("background:%1;border:none; margin: 0 16px;").arg(pal.border)); sep2->setFixedHeight(1);
    lOutl->addWidget(sep2); lOutl->addWidget(wrapInPresetSection(this, makeSwatchGrid(this, SWATCH_OUTLINE_15, true, onPickOutl)));
    gridStack->addWidget(gridOutl);

    laySwatch->addWidget(gridStack);

    // Boutons
    auto* swR = new QHBoxLayout;
    swR->setContentsMargins(16, 4, 16, 0);
    auto* btnCustom = new QPushButton("Custom"); auto* btnCancel1 = new QPushButton("Cancel");
    swR->addWidget(btnCustom); swR->addWidget(btnCancel1);
    laySwatch->addLayout(swR);

    rootStack->addWidget(pageSwatch);

    // ──────────────── PAGE 1 : VUE CUSTOM ────────────────
    auto* pageCustom = new QWidget;
    auto* layCustom = new QVBoxLayout(pageCustom);
    layCustom->setContentsMargins(16, 16, 16, 0); layCustom->setSpacing(10);

    auto* picker = new CustomColorPicker;
    layCustom->addWidget(picker);

    auto* cuR = new QHBoxLayout;
    auto* btnSwatch = new QPushButton("Swatch"); auto* btnCancel2 = new QPushButton("Cancel");
    cuR->addWidget(btnSwatch); cuR->addWidget(btnCancel2);
    layCustom->addLayout(cuR);

    rootStack->addWidget(pageCustom);
    root->addWidget(rootStack);

    // ──────────────── CÂBLAGES ────────────────
    auto updateRootStackHeight = [rootStack](int index) {
        for (int i = 0; i < rootStack->count(); ++i) rootStack->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        rootStack->widget(index)->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum); rootStack->adjustSize();
    };
    connect(rootStack, &QStackedWidget::currentChanged, updateRootStackHeight);
    updateRootStackHeight(rootStack->currentIndex());

    auto updateGridStackHeight = [gridStack](int index) {
        for (int i = 0; i < gridStack->count(); ++i) gridStack->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        gridStack->widget(index)->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum); gridStack->adjustSize();
    };
    connect(gridStack, &QStackedWidget::currentChanged, updateGridStackHeight);
    updateGridStackHeight(gridStack->currentIndex());

    connect(tabText, &QPushButton::clicked, this, [=]() { styleTab(tabText, true); styleTab(tabOutl, false); gridStack->setCurrentIndex(0); m_subTab = 0; });
    connect(tabOutl, &QPushButton::clicked, this, [=]() { styleTab(tabText, false); styleTab(tabOutl, true); gridStack->setCurrentIndex(1); m_subTab = 1; });

    connect(btnCustom, &QPushButton::clicked, this, [=]() {
        QColor initial = (m_subTab == 0) ? m_fill : m_outline;
        if (initial.alpha() == 0) initial = QColor(Qt::white);
        picker->setColor(initial);
        rootStack->setCurrentIndex(1);
        btnSwatch->setAttribute(Qt::WA_UnderMouse, false); btnSwatch->update();
    });

    connect(btnSwatch, &QPushButton::clicked, this, [this, picker]() {
        QColor c = picker->color();
        if (m_subTab == 0) { m_fill = c; pushHistory(c); } else { m_outline = c; pushHistory(c); }
        m_accepted = true; hide();
    });

    connect(picker, &CustomColorPicker::colorChanged, this, [this](const QColor& c) {
        if (m_subTab == 0) m_fill = c; else m_outline = c;
    });

    connect(btnCancel1, &QPushButton::clicked, this, [this]() { hide(); });
    connect(btnCancel2, &QPushButton::clicked, this, [this]() { hide(); });
}
bool TextColorPopover::pick(QWidget* anchor, QColor& fill, QColor& outline) {
    TextColorPopover pop(fill, outline); pop.anchorTo(anchor); pop.show();
    while (pop.isVisible()) QApplication::processEvents(QEventLoop::WaitForMoreEvents);
    if (pop.m_accepted) { fill = pop.m_fill; outline = pop.m_outline; return true; } return false;
}

// ════════════════════════════════════════════════════════════════════════════
// GradientPopover 
// ════════════════════════════════════════════════════════════════════════════
GradientPopover::GradientPopover(const GradientStops& initial, const QList<GradientStops>& presets, QWidget* parent)
    : KrakenColorPopover(parent), m_stops(initial.isEmpty() ? defaultVizStops() : initial)
{
    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(0, 0, 0, 12);
    root->setSpacing(0);

    const auto pal = resolvePopoverPalette(this);
    auto* stack = new QStackedWidget;

    // Page 0 : Swatches
    auto* page0 = new QWidget;
    auto* lay0 = new QVBoxLayout(page0);
    lay0->setContentsMargins(0,0,0,0); lay0->setSpacing(4);

    auto onPickPreset = [this](GradientStops s) { m_stops = s; pushGradientHistory(s); m_accepted = true; hide(); };
    lay0->addWidget(makeGradientHistoryBar(this, s_gradientHistory, onPickPreset));
    
    auto* sepG = new QFrame; sepG->setFrameShape(QFrame::HLine); sepG->setStyleSheet(QString("background:%1;border:none; margin: 0 16px;").arg(pal.border)); sepG->setFixedHeight(1);
    lay0->addWidget(sepG);
    
    lay0->addWidget(wrapInPresetSection(this, makeGradientSwatchGrid(this, presets, onPickPreset)));

    auto* row0 = new QHBoxLayout;
    row0->setContentsMargins(16, 4, 16, 0);
    auto* btnCustom0 = new QPushButton("Custom"); auto* btnCancel0 = new QPushButton("Cancel");
    row0->addWidget(btnCustom0); row0->addWidget(btnCancel0); lay0->addLayout(row0);
    stack->addWidget(page0);

    // Page 1 : Custom
    auto* page1 = new QWidget;
    auto* lay1 = new QVBoxLayout(page1);
    lay1->setContentsMargins(16, 16, 16, 0); lay1->setSpacing(10);

    auto* editor = new GradientStopsEditor; editor->setStops(m_stops); lay1->addWidget(editor);
    auto* picker = new CustomColorPicker; if (!m_stops.isEmpty()) picker->setColor(m_stops[0].color); lay1->addWidget(picker);

    auto* row1 = new QHBoxLayout;
    auto* btnSwatch1 = new QPushButton("Swatch"); auto* btnCancel1 = new QPushButton("Cancel");
    row1->addWidget(btnSwatch1); row1->addWidget(btnCancel1); lay1->addLayout(row1);
    stack->addWidget(page1);

    root->addWidget(stack);

    auto updateStackHeight = [stack](int index) {
        for (int i = 0; i < stack->count(); ++i) stack->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        stack->widget(index)->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum); stack->adjustSize();
    };
    connect(stack, &QStackedWidget::currentChanged, updateStackHeight);
    updateStackHeight(stack->currentIndex());

    connect(btnCustom0, &QPushButton::clicked, this, [=]() { stack->setCurrentIndex(1); btnSwatch1->setAttribute(Qt::WA_UnderMouse, false); btnSwatch1->update(); });
    connect(btnCancel0, &QPushButton::clicked, this, [this]() { hide(); });
    connect(btnSwatch1, &QPushButton::clicked, this, [this, editor]() { m_stops = editor->stops(); pushGradientHistory(m_stops); m_accepted = true; hide(); });
    connect(btnCancel1, &QPushButton::clicked, this, [this]() { hide(); });

    connect(editor, &GradientStopsEditor::stopsChanged, this, [this](const GradientStops& s) { m_stops = s; });
    connect(editor, &GradientStopsEditor::selectedChanged, this, [this, picker, editor](int idx) { if (idx >= 0 && idx < editor->stops().size()) picker->setColor(editor->stops()[idx].color); });
    connect(picker, &CustomColorPicker::colorChanged, this, [this, editor](const QColor& c) { const int idx = editor->selectedIndex(); GradientStops s = editor->stops(); if (idx >= 0 && idx < s.size()) { s[idx].color = c; editor->setStops(s); editor->setSelectedIndex(idx); m_stops = s; } });
}
bool GradientPopover::pick(QWidget* anchor, GradientStops& inOut, const QList<GradientStops>& presets) {
    GradientPopover pop(inOut, presets); pop.anchorTo(anchor); pop.show();
    while (pop.isVisible()) QApplication::processEvents(QEventLoop::WaitForMoreEvents);
    if (pop.m_accepted) { inOut = pop.m_stops; return true; } return false;
}

} // namespace NZXTKraken