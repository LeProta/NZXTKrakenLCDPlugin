#pragma once
#include <QFrame>
#include <QWidget>
#include <QPushButton>
#include <QColor>
#include <QList>
#include <QLineEdit>
#include <QSpinBox>
#include "../renderer/FrameRenderer.h"

namespace NZXTKraken {

// ───────────────────────────────────────────────────────────────────────────
// ColorSwatchButton : bouton carré aperçu — Solid / FillOutline / Gradient
// ───────────────────────────────────────────────────────────────────────────
class ColorSwatchButton : public QPushButton {
    Q_OBJECT
public:
    enum Kind { Solid, FillOutline, Gradient };
    explicit ColorSwatchButton(QWidget* parent = nullptr);
    void setSolid(const QColor& c);
    void setFillOutline(const QColor& fill, const QColor& outline);
    void setGradient(const GradientStops& stops);
protected:
    void paintEvent(QPaintEvent*) override;
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
    void enterEvent(QEnterEvent*) override;
#else
    void enterEvent(QEvent*) override;
#endif
    void leaveEvent(QEvent*) override;
private:
    Kind          m_kind = Solid;
    QColor        m_fill { Qt::white };
    QColor        m_outline { 0,0,0,0 };
    GradientStops m_stops;
    bool          m_hovered = false;
};

// ───────────────────────────────────────────────────────────────────────────
// SvPad : pavé 2D Saturation × Value
// ───────────────────────────────────────────────────────────────────────────
class SvPad : public QWidget {
    Q_OBJECT
public:
    explicit SvPad(QWidget* parent = nullptr);
    void setHue(float h);
    void setSV(float s, float v);
    float hue() const { return m_hue; }
    float saturation() const { return m_sat; }
    float value() const { return m_val; }
signals:
    void svChanged(float s, float v);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    float m_hue = 0.f, m_sat = 1.f, m_val = 1.f;
    void updateFromPos(const QPoint& p);
};

// ───────────────────────────────────────────────────────────────────────────
// HueSlider : barre horizontale arc-en-ciel
// ───────────────────────────────────────────────────────────────────────────
class HueSlider : public QWidget {
    Q_OBJECT
public:
    explicit HueSlider(QWidget* parent = nullptr);
    void setHue(float h);
    float hue() const { return m_hue; }
signals:
    void hueChanged(float h);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
private:
    float m_hue = 0.f;
    void updateFromPos(int x);
};

// ───────────────────────────────────────────────────────────────────────────
// CustomColorPicker : SV pad + Hue + inputs Hex/R/G/B (synchronisés)
// ───────────────────────────────────────────────────────────────────────────
class CustomColorPicker : public QWidget {
    Q_OBJECT
public:
    explicit CustomColorPicker(QWidget* parent = nullptr);
    void setColor(const QColor& c);
    QColor color() const { return m_color; }
signals:
    void colorChanged(const QColor& c);
private:
    SvPad*     m_sv = nullptr;
    HueSlider* m_hueSlider = nullptr;
    QLineEdit* m_hex = nullptr;
    QSpinBox*  m_r = nullptr;
    QSpinBox*  m_g = nullptr;
    QSpinBox*  m_b = nullptr;
    QColor     m_color { Qt::white };
    bool       m_updating = false;
    void emitColor(const QColor& c);
    void syncInputsFromColor();
};

// ───────────────────────────────────────────────────────────────────────────
// GradientStopsEditor : barre interactive (drag stops, clic droit ajoute,
// drag bas supprime). Émet stopsChanged en temps réel.
// ───────────────────────────────────────────────────────────────────────────
class GradientStopsEditor : public QWidget {
    Q_OBJECT
public:
    explicit GradientStopsEditor(QWidget* parent = nullptr);
    void setStops(const GradientStops& stops);
    GradientStops stops() const { return m_stops; }
    int selectedIndex() const { return m_selected; }
    void setSelectedIndex(int idx);
signals:
    void stopsChanged(const GradientStops& s);
    void selectedChanged(int index);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    GradientStops m_stops;
    int           m_selected   = 0;
    int           m_dragging   = -1;
    bool          m_markDelete = false;
    QPoint        m_pressPos;
    QRectF barRect() const;
    QRectF handleRect(int idx) const;
    int    stopAtPos(const QPoint& p) const;
};

// ───────────────────────────────────────────────────────────────────────────
// Popover de base : QFrame frameless qui ferme sur clic extérieur + Esc
// ───────────────────────────────────────────────────────────────────────────
class KrakenColorPopover : public QFrame {
    Q_OBJECT
public:
    explicit KrakenColorPopover(QWidget* parent = nullptr);
    void anchorTo(QWidget* widget);
protected:
    void keyPressEvent(QKeyEvent*) override;
    void showEvent(QShowEvent*) override;
    void paintEvent(QPaintEvent*) override;
};

// ───────────────────────────────────────────────────────────────────────────
// Popovers concrets (modal-like, retour de valeur via accept/reject)
// ───────────────────────────────────────────────────────────────────────────
class SolidColorPopover : public KrakenColorPopover {
    Q_OBJECT
public:
    static bool pick(QWidget* anchor, QColor& inOut);
private:
    explicit SolidColorPopover(const QColor& initial, QWidget* parent = nullptr);
    QColor m_color;
    bool   m_accepted = false;
};

class TextColorPopover : public KrakenColorPopover {
    Q_OBJECT
public:
    // Sous-onglets : fill + outline (outline peut être transparent)
    static bool pick(QWidget* anchor, QColor& fill, QColor& outline);
private:
    TextColorPopover(const QColor& fill, const QColor& outline, QWidget* parent = nullptr);
    QColor m_fill, m_outline;
    bool   m_accepted = false;
    int    m_subTab   = 0;   // 0 = Text (fill), 1 = Outline
};

class GradientPopover : public KrakenColorPopover {
    Q_OBJECT
public:
    static bool pick(QWidget* anchor, GradientStops& inOut,
                     const QList<GradientStops>& presets = GRADIENT_PRESETS_15);
private:
    GradientPopover(const GradientStops& initial,
                    const QList<GradientStops>& presets, QWidget* parent = nullptr);
    GradientStops m_stops;
    bool          m_accepted = false;
};

} // namespace NZXTKraken

// ─── Accesseurs d'historique pour la persistance ────────────────────────────
namespace NZXTKraken {
const QList<QColor>& kColorHistory();
const QList<GradientStops>& kGradientHistory();
void kSetColorHistory(const QList<QColor>& list);
void kSetGradientHistory(const QList<GradientStops>& list);
}
