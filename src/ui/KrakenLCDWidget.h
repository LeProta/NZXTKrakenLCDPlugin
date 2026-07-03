#pragma once
#include <QWidget>
#include <QTimer>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QSlider>
#include <QCheckBox>
#include <QStackedWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include "../device/NZXTKrakenDevice.h"
#include "../renderer/FrameRenderer.h"
#include "../sensors/SystemSensors.h"
#include "KrakenColorPicker.h"

class QThread;   // fwd (membre pointeur)

namespace NZXTKraken {

class KrakenWorker;   // defini dans KrakenWorker.h

// ── Aperçu circulaire 220×220 ─────────────────────────────────────────────────
class LCDPreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit LCDPreviewWidget(QWidget* parent = nullptr);
    void setFrame(const QImage& img);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage m_frame;
};

// ── Widget principal ──────────────────────────────────────────────────────────
class KrakenLCDWidget : public QWidget {
    Q_OBJECT
public:
    explicit KrakenLCDWidget(QWidget* parent = nullptr);
    ~KrakenLCDWidget();
    void setDarkTheme(bool dark);

protected:
    void changeEvent(QEvent* e) override;   // suit le thème clair/sombre d'OpenRGB

private slots:
    void onModeChanged(int index);
    void onRotateClicked();

private:
    void buildUI();
    void buildModeSelector();
    void buildMediaContainer();
    void buildModePanel_ImageGIF();
    void buildModePanel_SingleInfographic();
    void buildModePanel_DualInfographic();
    void buildModePanel_TripleInfographic();
    void buildModePanel_Clockface();
    void buildModePanel_AudioVisual();
    void buildModePanel_NowPlaying();

    // ── Upload / Browse ─────────────────────────────────────────────────────
    void doUpload();
    void doBrowse();
    void doRemove();   // retire l'affichage du media du mode courant (ne supprime PAS le fichier)
    bool validateAndImport(const QString& path, QPushButton* triggerBtn);
    void flashErrorBorder(QPushButton* btn);

    // ── Helpers ─────────────────────────────────────────────────────────────
    void               updateMediaVisibility();   // affiche le conteneur média selon le mode + useGif
    QString*           activeGifPath();            // chemin GIF du mode courant (ou nullptr)
    ColorSwatchButton* makeColorButton(QColor initial);
    void               connectColorButton(ColorSwatchButton* btn, QColor& target);
    ColorSwatchButton* makeTextColorButton(QColor fill, QColor outline);
    void               connectTextColorButton(ColorSwatchButton* btn,
                                              QColor& fill, QColor& outline);
    ColorSwatchButton* makeGradientButton(const GradientStops& stops);
    void               connectGradientButton(ColorSwatchButton* btn,
                                             GradientStops& target,
                                             const QList<GradientStops>& presets = GRADIENT_PRESETS_15);
    QComboBox*         makeSensorCombo(bool includeLiquid = true);

    // ── Persistence ─────────────────────────────────────────────────────────
    void saveConfig();

    // ── Init guard ──────────────────────────────────────────────────────────
    bool m_ready = false;          // true une fois le constructeur terminé

    // ── Device ──────────────────────────────────────────────────────────────
    NZXTKrakenDevice  m_device;
    QLabel*           m_lblDeviceName = nullptr;
    QLabel*           m_lblStatus     = nullptr;

    // ── Renderer ────────────────────────────────────────────────────────────
    SystemSensors     m_sensors;
    FrameConfig       m_config;
    LCDPreviewWidget* m_preview  = nullptr;

    // ── Mode selector ────────────────────────────────────────────────────────
    QComboBox*        m_modeCombo = nullptr;
    QSlider*          m_brightnessSlider = nullptr;

    // ── Media container (Upload + Browse + helper text) ─────────────────────
    QWidget*          m_mediaContainer = nullptr;
    QPushButton*      m_btnUpload      = nullptr;
    QPushButton*      m_btnBrowse      = nullptr;
    QPushButton*      m_btnRemove      = nullptr;
    QLabel*           m_lblMediaHelper = nullptr;

    // ── Panels ──────────────────────────────────────────────────────────────
    QWidget*          m_panelImageGIF = nullptr;
    QWidget*          m_panelSingle   = nullptr;
    QWidget*          m_panelDual     = nullptr;
    QWidget*          m_panelTriple   = nullptr;
    QWidget*          m_panelClock    = nullptr;
    QWidget*          m_panelAudio    = nullptr;
    QWidget*          m_panelNowPlaying = nullptr;
    QStackedWidget*   m_stack         = nullptr;

    // ── Timers ──────────────────────────────────────────────────────────────
    QThread*          m_thread = nullptr;
    KrakenWorker*     m_worker = nullptr;
    QTimer*           m_saveTimer = nullptr;   // debounce écriture settings.json

    // ── Audio WASAPI loopback ───────────────────────────────────────────────
    // (WASAPI deplace dans KrakenWorker)
    bool m_darkTheme = true;
};

} // namespace NZXTKraken
