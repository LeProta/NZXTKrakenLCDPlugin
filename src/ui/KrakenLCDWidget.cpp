#include "KrakenLCDWidget.h"
#include "KrakenWorker.h"
#include "KrakenMediaCache.h"
#include "KrakenUiPersistence.h"
#include "KrakenOpenRGBSettings.h"
#include "UpdateChecker.h"
#include "../Version.h"
#include "../renderer/FrameRenderer.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QSizePolicy>
#include <QColorDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <QThread>
#include <QPalette>
#include <QPointer>
#include <QEvent>

namespace NZXTKraken {

// ── Constantes ──────────────────────────────────────────────────────────────
static const QString IMAGE_FILTER =
    QStringLiteral("Images (*.gif *.png *.jpg *.jpeg)");

static const QStringList ALLOWED_EXTENSIONS = {
    "gif", "png", "jpg", "jpeg"
};

static constexpr qint64 MAX_FILE_SIZE = 32LL * 1024 * 1024;   // 32 MiB

// Upload/Browse/Remove : boutons natifs (thème OpenRGB), même gabarit que
// Reset/Apply du plugin Pump — plus de stylesheet dédiée.

// Libelles "muets" (statut + aide media) : couleur derivee du theme clair/sombre.
static QString statusLabelStyle(bool dark)
{
    return QString("color:%1;font-size:11px;font-family:monospace;")
           .arg(dark ? "#aaa" : "#666");
}

static QString helperLabelStyle(bool dark)
{
    return QString("color:%1;font-size:11px;")
           .arg(dark ? "#888" : "#777");
}

// ─────────────────────────────────────────────────────────────────────────────
// LCDPreviewWidget
// ─────────────────────────────────────────────────────────────────────────────
LCDPreviewWidget::LCDPreviewWidget(QWidget* p) : QWidget(p) { setFixedSize(220,220); }

void LCDPreviewWidget::setFrame(const QImage& img)
{
    m_frame = img.scaled(208, 208, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    update();
}

void LCDPreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Couleurs derivees de la palette de l'application (OpenRGB sombre/clair) :
    // contour et fond "ecran eteint" suivent le theme automatiquement, Qt
    // repeignant le widget a chaque changement de palette.
    const QColor ring   = palette().color(QPalette::Mid);
    const QColor offScr = palette().color(QPalette::Base);
    QColor       hint   = palette().color(QPalette::WindowText);
    hint.setAlpha(110);

    p.setPen(QPen(ring, 4));
    p.drawEllipse(2, 2, 216, 216);
    QPainterPath clip;
    clip.addEllipse(6, 6, 208, 208);
    p.setClipPath(clip);
    if (!m_frame.isNull()) {
        p.drawImage(6, 6, m_frame);
        // L'aperçu reste à pleine luminosité : le curseur ne pilote que l'écran
        // physique, pas le rendu à l'écran.
    }
    else {
        p.fillRect(6,6,208,208,offScr);
        p.setPen(hint);
        p.setFont(QFont("Segoe UI",11));
        p.drawText(QRectF(6,6,208,208),Qt::AlignCenter,"No\ndevice");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// WASAPI loopback
// ─────────────────────────────────────────────────────────────────────────────
// WASAPI : init/close/readBands deplaces dans KrakenWorker.

// ─────────────────────────────────────────────────────────────────────────────
// Helpers UI (KrakenColorPicker custom popovers)
// ─────────────────────────────────────────────────────────────────────────────
ColorSwatchButton* KrakenLCDWidget::makeColorButton(QColor c)
{
    auto* btn = new ColorSwatchButton;
    btn->setSolid(c);
    btn->setFixedSize(36, 24);  // même taille pour tous les boutons couleur du panel
    return btn;
}

void KrakenLCDWidget::connectColorButton(ColorSwatchButton* btn, QColor& target)
{
    // QPointer : pick() fait tourner une boucle d'événements imbriquée — si le
    // plugin est déchargé pendant que le popover est ouvert, le widget (et
    // `target`, qui pointe dans m_config) est détruit. Sans le garde-fou,
    // `target = c` écrirait dans de la mémoire libérée.
    QPointer<KrakenLCDWidget> self(this);
    connect(btn, &QPushButton::clicked, [=,&target](){
        QColor c = target;
        if (SolidColorPopover::pick(btn, c)) {
            if (!self) return;
            target = c;
            btn->setSolid(c);
            saveConfig();
        }
    });
}

ColorSwatchButton* KrakenLCDWidget::makeTextColorButton(QColor fill, QColor outline)
{
    auto* btn = new ColorSwatchButton;
    btn->setFillOutline(fill, outline);
    btn->setFixedSize(36, 24);  // même taille que makeColorButton
    return btn;
}

void KrakenLCDWidget::connectTextColorButton(ColorSwatchButton* btn,
                                             QColor& fill, QColor& outline)
{
    QPointer<KrakenLCDWidget> self(this);   // cf. connectColorButton
    connect(btn, &QPushButton::clicked, [=,&fill,&outline](){
        QColor f = fill, o = outline;
        if (TextColorPopover::pick(btn, f, o)) {
            if (!self) return;
            fill = f; outline = o;
            btn->setFillOutline(f, o);
            saveConfig();
        }
    });
}

ColorSwatchButton* KrakenLCDWidget::makeGradientButton(const GradientStops& stops)
{
    auto* btn = new ColorSwatchButton;
    btn->setGradient(stops.isEmpty() ? defaultVizStops() : stops);
    btn->setFixedSize(36, 24);  // même taille que makeColorButton (uniformité panel)
    return btn;
}

void KrakenLCDWidget::connectGradientButton(ColorSwatchButton* btn,
                                            GradientStops& target,
                                            const QList<GradientStops>& presets)
{
    QPointer<KrakenLCDWidget> self(this);   // cf. connectColorButton
    connect(btn, &QPushButton::clicked, [=,&target](){
        GradientStops s = target.isEmpty() ? defaultVizStops() : target;
        if (GradientPopover::pick(btn, s, presets)) {
            if (!self) return;
            target = s;
            btn->setGradient(s);
            saveConfig();
        }
    });
}

QComboBox* KrakenLCDWidget::makeSensorCombo(bool includeLiquid)
{
    auto* cb = new QComboBox;
    for (const auto& s : CPU_SENSORS) {
        if (s.type == SensorType::LIQUID_TEMP && !includeLiquid) continue;
        cb->addItem(s.label, int(s.type));
    }
    for (const auto& s : GPU_SENSORS)    cb->addItem(s.label, int(s.type));
    for (const auto& s : MEMORY_SENSORS) cb->addItem(s.label, int(s.type));
    return cb;
}

// Chemin du média de fond du mode courant (ou nullptr si le mode n'en a pas).
QString* KrakenLCDWidget::activeGifPath()
{
    switch (m_config.mode) {
        case DisplayMode::IMAGE_GIF:          return &m_config.gifPath;
        case DisplayMode::SINGLE_INFOGRAPHIC: return &m_config.single.gifPath;
        case DisplayMode::DUAL_INFOGRAPHIC:   return &m_config.dual.gifPath;
        case DisplayMode::TRIPLE_INFOGRAPHIC: return &m_config.triple.gifPath;
        default:                              return nullptr;
    }
}

// Affiche le conteneur média (Upload/Browse) pour Image/GIF, ou pour un mode
// infographic dont le fond GIF est activé.
void KrakenLCDWidget::updateMediaVisibility()
{
    if (!m_mediaContainer) return;
    bool show = false;
    switch (m_config.mode) {
        case DisplayMode::IMAGE_GIF:
        case DisplayMode::SINGLE_INFOGRAPHIC:
        case DisplayMode::DUAL_INFOGRAPHIC:
        case DisplayMode::TRIPLE_INFOGRAPHIC:
            show = true; break;
        default: break;
    }
    m_mediaContainer->setVisible(show);
}

// ─────────────────────────────────────────────────────────────────────────────
// Upload / Browse / Validation / Fade erreur
// ─────────────────────────────────────────────────────────────────────────────
void KrakenLCDWidget::flashErrorBorder(QPushButton* btn)
{
    if (!btn) return;

    const QColor normalBg = m_darkTheme ? QColor(0x1F,0x1F,0x23) : QColor(0xEB,0xEB,0xEE);
    const QColor errBg    = m_darkTheme ? QColor(0xCC,0x22,0x22) : QColor(0xEE,0x33,0x33);
    const QString border  = m_darkTheme ? "#2A2A2E" : "#D0D0D4";
    const QString fg      = m_darkTheme ? "#FFFFFF" : "#1A1A1E";

    // QPointer : le bouton peut mourir pendant l'animation (unload du plugin).
    QPointer<QPushButton> safeBtn(btn);

    // On ne touche QUE le fond du bouton ; la bordure reste celle du thème.
    auto applyBg = [safeBtn, border, fg](const QColor& c) {
        if (!safeBtn) return;
        safeBtn->setStyleSheet(
            QString("QPushButton{background:%1;color:%2;"
                    "border:1px solid %3;border-radius:5px;padding:5px 10px;font-size:11px;}")
            .arg(c.name(), fg, border));
    };

    // Immédiatement → fond rouge
    applyBg(errBg);

    // Après 1 s → fondu du fond rouge vers le fond normal (~500 ms, 10 × 50 ms).
    // Compteur capturé PAR VALEUR (lambda mutable) : rien à libérer si le
    // widget meurt en cours de fondu.
    QTimer::singleShot(1000, this, [this, safeBtn, errBg, normalBg, applyBg]() {
        if (!safeBtn) return;

        auto* fade = new QTimer(this);
        fade->setInterval(50);

        connect(fade, &QTimer::timeout, this,
                [safeBtn, fade, errBg, normalBg, applyBg, step = 0]() mutable {
            if (!safeBtn) { fade->stop(); fade->deleteLater(); return; }
            ++step;
            float t = std::min(1.f, step / 10.f);
            int r = errBg.red()   + int((normalBg.red()   - errBg.red())   * t);
            int g = errBg.green() + int((normalBg.green() - errBg.green()) * t);
            int b = errBg.blue()  + int((normalBg.blue()  - errBg.blue())  * t);
            applyBg(QColor(r, g, b));

            if (step >= 10) {
                fade->stop();
                fade->deleteLater();
                safeBtn->setStyleSheet(QString());   // retour au style natif
            }
        });
        fade->start();
    });
}

bool KrakenLCDWidget::validateAndImport(const QString& path, QPushButton* triggerBtn)
{
    if (path.isEmpty()) return false;

    QFileInfo fi(path);

    if (!fi.exists() || !fi.isFile()) {
        qWarning() << "[KrakenLCD/UI] File not found:" << path;
        flashErrorBorder(triggerBtn);
        return false;
    }

    QString ext = fi.suffix().toLower();
    if (!ALLOWED_EXTENSIONS.contains(ext)) {
        qWarning() << "[KrakenLCD/UI] Extension not allowed:" << ext << "—" << path;
        flashErrorBorder(triggerBtn);
        return false;
    }

    if (fi.size() > MAX_FILE_SIZE) {
        qWarning() << "[KrakenLCD/UI] File too large:" << fi.size()
                   << "bytes (max" << MAX_FILE_SIZE << ") —" << path;
        flashErrorBorder(triggerBtn);
        return false;
    }

    QString imported = KrakenMediaCache::importFile(path);
    if (imported.isEmpty()) {
        qWarning() << "[KrakenLCD/UI] Import failed for" << path;
        flashErrorBorder(triggerBtn);
        return false;
    }

    if (QString* tgt = activeGifPath()) { *tgt = imported; saveConfig(); }
    return true;
}

void KrakenLCDWidget::doUpload()
{
    QString startDir = KrakenMediaCache::downloadsDirectory();
    QString path;
    {
        QFileDialog dlg(this, "Select an image or GIF", startDir, IMAGE_FILTER);
        dlg.setFileMode(QFileDialog::ExistingFile);
        dlg.setOption(QFileDialog::ReadOnly, true);
        if (dlg.exec() != QDialog::Accepted) return;
        QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        path = sel.first();
    }
    validateAndImport(path, m_btnUpload);
}

void KrakenLCDWidget::doBrowse()
{
    QString mediaDir = KrakenMediaCache::mediaDirectory();
    QString path;
    {
        QFileDialog dlg(this, "Select a media file", mediaDir, IMAGE_FILTER);
        dlg.setFileMode(QFileDialog::ExistingFile);
        dlg.setOption(QFileDialog::ReadOnly, true);
        if (dlg.exec() != QDialog::Accepted) return;
        QStringList sel = dlg.selectedFiles();
        if (sel.isEmpty()) return;
        path = sel.first();
    }

    // Depuis media/ → pas de re-copie, juste validation
    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();
    if (!ALLOWED_EXTENSIONS.contains(ext) || fi.size() > MAX_FILE_SIZE) {
        flashErrorBorder(m_btnBrowse);
        return;
    }

    if (QString* tgt = activeGifPath()) { *tgt = path; saveConfig(); }
}

void KrakenLCDWidget::doRemove()
{
    // Retire l'affichage du media du mode courant : on vide juste le chemin,
    // le fichier reste dans le dossier media/. Le worker re-rend sans media.
    if (QString* tgt = activeGifPath()) {
        if (tgt->isEmpty()) return;
        tgt->clear();
        saveConfig();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistence — ne sauvegarde rien tant que le constructeur n'est pas fini
// ─────────────────────────────────────────────────────────────────────────────
void KrakenLCDWidget::saveConfig()
{
    if (!m_ready) return;
    if (m_worker) m_worker->setConfig(m_config);   // pousse la config vers le thread de rendu
    // Écriture disque debouncée (500 ms) : un drag de slider déclenche des
    // dizaines de valueChanged/s — on n'écrit qu'une fois le geste terminé.
    // Flush garanti dans le destructeur.
    if (m_saveTimer) m_saveTimer->start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructeur / destructeur
// ─────────────────────────────────────────────────────────────────────────────
KrakenLCDWidget::KrakenLCDWidget(QWidget* parent) : QWidget(parent)
{
    // m_ready = false par défaut → saveConfig() et onModeChanged() sont no-op

    // Charger la config sauvegardée
    m_config = KrakenUiPersistence::load();
    DisplayMode savedMode = m_config.mode;   // sauvegarder avant que buildUI l'écrase
    int savedBrightness   = m_config.brightness;

    // Thème (clair/sombre) hérité d'OpenRGB via la palette de l'application,
    // détecté AVANT buildUI pour que les boutons Upload/Browse soient stylisés
    // correctement dès leur création.
    m_darkTheme = palette().color(QPalette::Window).lightness() < 128;

    buildUI();
    // Ouverture device + demarrage capteurs : desormais sur le thread du worker.

    // Restaurer le mode dans le combo (sans déclencher onModeChanged prématurément)
    m_modeCombo->blockSignals(true);
    for (int i = 0; i < m_modeCombo->count(); i++) {
        if (m_modeCombo->itemData(i).toInt() == int(savedMode)) {
            m_modeCombo->setCurrentIndex(i);
            break;
        }
    }
    m_modeCombo->blockSignals(false);

    // Restaurer la luminosité (sans déclencher le slot)
    m_brightnessSlider->blockSignals(true);
    m_brightnessSlider->setValue(savedBrightness);
    m_brightnessSlider->blockSignals(false);

    // Restaurer le mode sauvegardé dans la config (buildUI aurait pu l'écraser)
    m_config.mode       = savedMode;
    m_config.brightness = savedBrightness;

    // Debounce de la persistance (créé avant m_ready pour être dispo dès le 1er saveConfig)
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(250);   // fenêtre de perte réduite en cas de kill/crash
    connect(m_saveTimer, &QTimer::timeout, this,
            [this]{ KrakenUiPersistence::save(m_config); });

    // ── Tout est prêt — activer ──────────────────────────────────────────
    m_ready = true;

    // Premier appel réel de onModeChanged (met à jour panels, media container, timers)
    onModeChanged(m_modeCombo->currentIndex());

    // Démarrer les timers (le render timer reprend l'intervalle calculé par
    // onModeChanged ci-dessus, c.-à-d. la cadence de l'écran du modèle détecté).
    // --- Worker de rendu sur thread dedie ---
    m_thread = new QThread(this);
    m_worker = new KrakenWorker(&m_device, &m_sensors);
    m_worker->moveToThread(m_thread);
    m_worker->setConfig(m_config);

    connect(m_thread, &QThread::started,  m_worker, &KrakenWorker::start);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &KrakenWorker::deviceOpened, this,
            [this](const QString& name, bool ok) {
                m_lblDeviceName->setText(name);
                if (!ok) m_lblStatus->setText("Check the USB connection and close NZXT CAM.");
            });
    connect(m_worker, &KrakenWorker::deviceStatus, this,
            [this](const QString& t) { if (m_lblStatus) m_lblStatus->setText(t); });
    connect(m_worker, &KrakenWorker::previewReady, this,
            [this](const QImage& f) { if (m_preview) m_preview->setFrame(f); });

    m_thread->start();

    // Vérification de mise à jour (GitHub releases/latest), asynchrone.
    m_updater = new UpdateChecker(this);
    connect(m_updater, &UpdateChecker::updateAvailable, this,
            [this](const QString& tag, const QString& url) {
        m_updateBanner->setText(QStringLiteral(
            "&#128276; Update available: <b>%1</b> &mdash; "
            "<a href=\"%2\">Download on GitHub</a>")
            .arg(tag.toHtmlEscaped(), url.toHtmlEscaped()));
        m_updateBanner->show();
    });
    m_updater->check(QStringLiteral(NZXT_LCD_VERSION));
}

KrakenLCDWidget::~KrakenLCDWidget()
{
    // Flush direct (pas via le debounce : le timer meurt avec le widget)
    if (m_saveTimer) m_saveTimer->stop();
    if (m_ready) KrakenUiPersistence::save(m_config);
    // Interrompt les attentes en cours de sendFrame (acks 2×5 s, bulk 3×5 s)
    // AVANT l'appel bloquant : sans ça, un device décroché gelait l'UI
    // d'OpenRGB jusqu'à ~15-20 s au unload du plugin.
    m_device.requestAbort();
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Media container (Upload + Browse + helper)
// ─────────────────────────────────────────────────────────────────────────────
void KrakenLCDWidget::buildMediaContainer()
{
    m_mediaContainer = new QWidget;
    auto* lay = new QVBoxLayout(m_mediaContainer);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    auto* btnRow = new QHBoxLayout;
    m_btnUpload = new QPushButton("Upload");
    m_btnUpload->setFixedHeight(22);
    m_btnBrowse = new QPushButton("Browse");
    m_btnBrowse->setFixedHeight(22);
    m_btnRemove = new QPushButton("Remove");
    m_btnRemove->setFixedHeight(22);

    connect(m_btnUpload, &QPushButton::clicked, this, &KrakenLCDWidget::doUpload);
    connect(m_btnBrowse, &QPushButton::clicked, this, &KrakenLCDWidget::doBrowse);
    connect(m_btnRemove, &QPushButton::clicked, this, &KrakenLCDWidget::doRemove);

    btnRow->addWidget(m_btnUpload);
    btnRow->addWidget(m_btnBrowse);
    btnRow->addWidget(m_btnRemove);
    btnRow->addStretch();
    lay->addLayout(btnRow);

    m_lblMediaHelper = new QLabel("Supports GIF, PNG, and JPG. Max 32MiB.");
    m_lblMediaHelper->setStyleSheet(helperLabelStyle(m_darkTheme));
    m_lblMediaHelper->setWordWrap(true);
    lay->addWidget(m_lblMediaHelper);

    // Masqué par défaut
    m_mediaContainer->setVisible(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction de l'interface
// ─────────────────────────────────────────────────────────────────────────────
void KrakenLCDWidget::buildUI()
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(16,12,16,12);
    root->setSpacing(20);

    auto* leftCol = new QVBoxLayout;
    leftCol->setSpacing(8);

    m_lblDeviceName = new QLabel("Detecting…");
    m_lblDeviceName->setStyleSheet("font-weight:600;font-size:13px;");
    m_lblStatus = new QLabel;
    m_lblStatus->setStyleSheet(statusLabelStyle(m_darkTheme));
    leftCol->addWidget(m_lblDeviceName);
    leftCol->addWidget(m_lblStatus);

    // Bannière de mise à jour (masquée tant qu'aucune version plus récente).
    m_updateBanner = new QLabel;
    m_updateBanner->setTextFormat(Qt::RichText);
    m_updateBanner->setOpenExternalLinks(true);
    m_updateBanner->setWordWrap(true);
    m_updateBanner->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;"));
    m_updateBanner->hide();
    leftCol->addWidget(m_updateBanner);

    // Sélecteur de mode — signaux bloqués pendant l'ajout des items
    buildModeSelector();
    leftCol->addWidget(m_modeCombo);

    // Luminosité
    auto* bRow = new QHBoxLayout;
    bRow->addWidget(new QLabel("☀"));
    m_brightnessSlider = new QSlider(Qt::Horizontal);
    m_brightnessSlider->setRange(0,100);
    m_brightnessSlider->setValue(100);
    // Envoi HID debouncé : un drag génère des dizaines de valueChanged/s, et
    // chaque commande est sérialisée avec les frames sur le thread du worker.
    auto* brightnessSend = new QTimer(this);
    brightnessSend->setSingleShot(true);
    brightnessSend->setInterval(150);
    connect(brightnessSend, &QTimer::timeout, this, [this]{
        if (m_worker)
            QMetaObject::invokeMethod(m_worker, "setBrightness", Qt::QueuedConnection,
                                      Q_ARG(int, m_config.brightness));
    });
    connect(m_brightnessSlider, &QSlider::valueChanged, [this, brightnessSend](int v){
        m_config.brightness = v;
        saveConfig();
        brightnessSend->start();   // n'affecte que l'écran physique, pas l'aperçu
    });
    bRow->addWidget(m_brightnessSlider);
    bRow->addWidget(new QLabel("☀"));
    leftCol->addLayout(bRow);

    // Media container (conditionnel)
    buildMediaContainer();
    leftCol->addWidget(m_mediaContainer);

    // Stack des panels
    m_stack = new QStackedWidget;
    buildModePanel_ImageGIF();
    buildModePanel_SingleInfographic();
    buildModePanel_DualInfographic();
    buildModePanel_TripleInfographic();
    buildModePanel_Clockface();
    buildModePanel_AudioVisual();
    buildModePanel_NowPlaying();
    m_stack->addWidget(m_panelImageGIF);
    m_stack->addWidget(m_panelSingle);
    m_stack->addWidget(m_panelDual);
    m_stack->addWidget(m_panelTriple);
    m_stack->addWidget(m_panelClock);
    m_stack->addWidget(m_panelAudio);
    m_stack->addWidget(m_panelNowPlaying);
    leftCol->addWidget(m_stack);
    leftCol->addStretch();

    // Colonne droite : preview + rotate
    auto* rightCol = new QVBoxLayout;
    rightCol->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    rightCol->setSpacing(8);
    auto* rotBtn = new QPushButton("↻  Rotate Display");
    rotBtn->setStyleSheet("color:#AA44FF;background:transparent;border:none;font-size:12px;");
    connect(rotBtn, &QPushButton::clicked, this, &KrakenLCDWidget::onRotateClicked);
    m_preview = new LCDPreviewWidget;
    rightCol->addWidget(rotBtn, 0, Qt::AlignRight);
    rightCol->addWidget(m_preview, 0, Qt::AlignHCenter);

    root->addLayout(leftCol, 1);
    root->addLayout(rightCol);
}

void KrakenLCDWidget::buildModeSelector()
{
    m_modeCombo = new QComboBox;

    // ── Bloquer les signaux pendant l'ajout pour éviter des appels à
    //    onModeChanged() alors que les widgets n'existent pas encore ─────────
    m_modeCombo->blockSignals(true);

    m_modeCombo->addItem("Image/GIF",                   int(DisplayMode::IMAGE_GIF));
    m_modeCombo->addItem("Single Infographic",          int(DisplayMode::SINGLE_INFOGRAPHIC));
    m_modeCombo->addItem("Dual Infographic",            int(DisplayMode::DUAL_INFOGRAPHIC));
    m_modeCombo->addItem("Triple Infographic",          int(DisplayMode::TRIPLE_INFOGRAPHIC));
    m_modeCombo->addItem("Clockface",                   int(DisplayMode::CLOCKFACE));
    m_modeCombo->addItem("Audio Visual",                int(DisplayMode::AUDIO_VISUAL));
    m_modeCombo->addItem("Now Playing",                 int(DisplayMode::NOW_PLAYING));

    m_modeCombo->blockSignals(false);

    // Connecter APRÈS le remplissage
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KrakenLCDWidget::onModeChanged);
}

// ─── Macro capteur : metric + Number (fill+outline) + Text (fill+outline) ───
#define SENSOR_BLOCK(TITLE, RC) \
    if (*(TITLE) != '\0') l->addWidget(new QLabel(TITLE)); \
    { auto* cb = makeSensorCombo(); \
      { const int _idx = cb->findData(int((RC).type)); \
        if (_idx >= 0) { cb->blockSignals(true); cb->setCurrentIndex(_idx); cb->blockSignals(false); } } \
      connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), [=](int i){ \
          (RC).type = SensorType(cb->itemData(i).toInt()); saveConfig(); }); \
      l->addWidget(cb); } \
    { auto* row = new QHBoxLayout; \
      row->addWidget(new QLabel("Number")); \
      auto* bN = makeTextColorButton((RC).numberFill,(RC).numberOutline); \
      connectTextColorButton(bN,(RC).numberFill,(RC).numberOutline); row->addWidget(bN); \
      row->addSpacing(8); row->addWidget(new QLabel("Text")); \
      auto* bT = makeTextColorButton((RC).textFill,(RC).textOutline); \
      connectTextColorButton(bT,(RC).textFill,(RC).textOutline); row->addWidget(bT); \
      row->addStretch(); l->addLayout(row); }

void KrakenLCDWidget::buildModePanel_ImageGIF()
{
    m_panelImageGIF = new QWidget;
    auto* l = new QVBoxLayout(m_panelImageGIF);
    l->setSpacing(10);
    l->addStretch();
}

void KrakenLCDWidget::buildModePanel_SingleInfographic()
{
    m_panelSingle = new QWidget; auto* l = new QVBoxLayout(m_panelSingle); l->setSpacing(8);
    auto bgRow = new QHBoxLayout; bgRow->addWidget(new QLabel("Background"));
    auto* bBg = makeColorButton(m_config.single.bgColor);
    connectColorButton(bBg, m_config.single.bgColor);
    bgRow->addWidget(bBg); bgRow->addStretch(); l->addLayout(bgRow);
    auto lgRow = new QHBoxLayout; auto* ck = new QCheckBox("Logo"); ck->setChecked(m_config.single.showLogo);
    connect(ck,&QCheckBox::toggled,[this](bool b){m_config.single.showLogo=b; saveConfig();});
    lgRow->addWidget(ck);
    auto* bLg = makeColorButton(m_config.single.logoColor);
    connectColorButton(bLg,m_config.single.logoColor);
    lgRow->addWidget(bLg); lgRow->addStretch(); l->addLayout(lgRow);
    auto vzRow = new QHBoxLayout; vzRow->addWidget(new QLabel("Visualization"));
    auto* bVz = makeGradientButton(m_config.singleReading.vizStops);
    connectGradientButton(bVz, m_config.singleReading.vizStops);
    vzRow->addWidget(bVz); vzRow->addStretch(); l->addLayout(vzRow);
    l->addWidget(new QLabel("Primary Reading"));
    SENSOR_BLOCK("", m_config.singleReading);
    l->addStretch();
}

void KrakenLCDWidget::buildModePanel_DualInfographic()
{
    m_panelDual = new QWidget; auto* l = new QVBoxLayout(m_panelDual); l->setSpacing(8);
    
    // ── Display Mode ──
    auto mRow = new QHBoxLayout; mRow->addWidget(new QLabel("Display Mode"));
    auto* dc = new QComboBox; dc->addItem("Vertical"); dc->addItem("Horizontal");
    if (!m_config.dualVertical) dc->setCurrentIndex(1);
    mRow->addWidget(dc); mRow->addStretch(); l->addLayout(mRow);

    // ── Background (Toujours visible et parfaitement aligné) ──
    auto* bgRow = new QHBoxLayout; bgRow->addWidget(new QLabel("Background"));
    auto* bBg = makeColorButton(m_config.dual.bgColor);
    connectColorButton(bBg,m_config.dual.bgColor);
    bgRow->addWidget(bBg); bgRow->addStretch();
    l->addLayout(bgRow);

    // ── Logo (Masqué en Horizontal, parfaitement aligné en Vertical) ──
    auto* lgWrap = new QWidget;
    auto* lgRow = new QHBoxLayout(lgWrap);
    lgRow->setContentsMargins(0, 0, 0, 0); // Élimine les marges invisibles du QWidget pour éviter le décalage !
    
    auto* ck = new QCheckBox("Logo"); ck->setChecked(m_config.dual.showLogo);
    connect(ck, &QCheckBox::toggled, [this](bool b){ m_config.dual.showLogo = b; saveConfig(); });
    lgRow->addWidget(ck);
    auto* bLg = makeColorButton(m_config.dual.logoColor);
    connectColorButton(bLg, m_config.dual.logoColor);
    lgRow->addWidget(bLg); lgRow->addStretch();
    
    l->addWidget(lgWrap);

    // Gestion de la visibilité dynamique du Logo selon le mode (Vertical / Horizontal)
    auto updateLogoVis = [lgWrap, this]() {
        lgWrap->setVisible(m_config.dualVertical);
    };
    updateLogoVis(); // Appel initial au chargement

    connect(dc, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, lgWrap](int i){
        m_config.dualVertical = (i == 0);
        saveConfig();
        lgWrap->setVisible(m_config.dualVertical); // Masque ou affiche instantanément le widget
    });

    // ── Reading 1 ──
    l->addWidget(new QLabel("Reading 1"));
    SENSOR_BLOCK("", m_config.dualReading1);
    { auto vr=new QHBoxLayout; vr->addWidget(new QLabel("Visualization"));
      auto* b=makeColorButton(m_config.dualReading1.colorViz);
      connectColorButton(b,m_config.dualReading1.colorViz);
      vr->addWidget(b); vr->addStretch(); l->addLayout(vr); }

    // ── Reading 2 ──
    l->addWidget(new QLabel("Reading 2"));
    SENSOR_BLOCK("", m_config.dualReading2);
    { auto vr=new QHBoxLayout; vr->addWidget(new QLabel("Visualization"));
      auto* b=makeColorButton(m_config.dualReading2.colorViz);
      connectColorButton(b,m_config.dualReading2.colorViz);
      vr->addWidget(b); vr->addStretch(); l->addLayout(vr); }

    l->addStretch();
}

void KrakenLCDWidget::buildModePanel_TripleInfographic()
{
    m_panelTriple = new QWidget; auto* l = new QVBoxLayout(m_panelTriple); l->setSpacing(8);
    auto bgRow = new QHBoxLayout; bgRow->addWidget(new QLabel("Background"));
    auto* bBg = makeColorButton(m_config.triple.bgColor);
    connectColorButton(bBg,m_config.triple.bgColor);
    bgRow->addWidget(bBg); bgRow->addStretch(); l->addLayout(bgRow);
    auto lgRow = new QHBoxLayout; auto* ck = new QCheckBox("Logo"); ck->setChecked(m_config.triple.showLogo);
    connect(ck,&QCheckBox::toggled,[this](bool b){m_config.triple.showLogo=b; saveConfig();});
    lgRow->addWidget(ck);
    auto* bLg = makeColorButton(m_config.triple.logoColor);
    connectColorButton(bLg,m_config.triple.logoColor);
    lgRow->addWidget(bLg); lgRow->addStretch(); l->addLayout(lgRow);
    auto vzRow = new QHBoxLayout; vzRow->addWidget(new QLabel("Visualization"));
    auto* bVz = makeGradientButton(m_config.tripleReading1.vizStops);
    connectGradientButton(bVz, m_config.tripleReading1.vizStops);
    vzRow->addWidget(bVz); vzRow->addStretch(); l->addLayout(vzRow);
    l->addWidget(new QLabel("Primary Reading"));
    SENSOR_BLOCK("", m_config.tripleReading1);
    l->addWidget(new QLabel("Secondary Readings"));
    SENSOR_BLOCK("", m_config.tripleReading2);
    SENSOR_BLOCK("", m_config.tripleReading3);
    l->addStretch();
}

void KrakenLCDWidget::buildModePanel_Clockface()
{
    m_panelClock = new QWidget; auto* l = new QVBoxLayout(m_panelClock); l->setSpacing(8);

    // Helpers locaux : ajoutent une ligne "label + bouton" à la page donnée.
    auto colorRow = [&](QVBoxLayout* lay, const QString& lbl, QColor& col){
        auto* row = new QHBoxLayout; row->addWidget(new QLabel(lbl));
        auto* b = makeColorButton(col); connectColorButton(b, col);
        row->addWidget(b); row->addStretch(); lay->addLayout(row);
    };
    auto gradientRow = [&](QVBoxLayout* lay, const QString& lbl, GradientStops& stops){
        auto* row = new QHBoxLayout; row->addWidget(new QLabel(lbl));
        auto* b = makeGradientButton(stops);
        connectGradientButton(b, stops, CLOCK_GRADIENT_PRESETS);   // presets propres à l'horloge
        row->addWidget(b); row->addStretch(); lay->addLayout(row);
    };
    auto formatRow = [&](QVBoxLayout* lay, bool& is24){
        lay->addWidget(new QLabel("Format"));
        auto* cb = new QComboBox;
        cb->addItem("12-Hour Clock"); cb->addItem("24-Hour Clock");
        cb->blockSignals(true); cb->setCurrentIndex(is24 ? 1 : 0); cb->blockSignals(false);
        connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                [this, &is24](int i){ is24 = (i == 1); saveConfig(); });
        lay->addWidget(cb);
    };

    // ── Clockface (menu déroulant, façon "Display Mode" du dual) ──
    auto mRow = new QHBoxLayout; mRow->addWidget(new QLabel("Clockface"));
    auto* faceCombo = new QComboBox;
    faceCombo->addItem("Analog"); faceCombo->addItem("Gradient"); faceCombo->addItem("Digital");
    mRow->addWidget(faceCombo); mRow->addStretch(); l->addLayout(mRow);

    // Un conteneur de réglages par clockface, affiché/masqué selon la sélection
    // (comme le logo du dual) : un conteneur masqué ne prend aucune place, donc
    // plus d'espace vide entre les éléments quelle que soit la clockface.
    auto* pageAnalog = new QWidget;  auto* plA = new QVBoxLayout(pageAnalog);
    plA->setContentsMargins(0,0,0,0); plA->setSpacing(8);
    colorRow(plA, "Background", m_config.bgColor);
    colorRow(plA, "Dial",       m_config.clockDial);
    colorRow(plA, "Seconds",    m_config.clockSeconds);

    auto* pageGrad = new QWidget;    auto* plG = new QVBoxLayout(pageGrad);
    plG->setContentsMargins(0,0,0,0); plG->setSpacing(8);
    formatRow(plG, m_config.clockArc24h);
    gradientRow(plG, "Background", m_config.clockArcStops);
    colorRow(plG, "Dial", m_config.clockArcDial);
    colorRow(plG, "Text", m_config.clockArcText);

    auto* pageDigital = new QWidget; auto* plD = new QVBoxLayout(pageDigital);
    plD->setContentsMargins(0,0,0,0); plD->setSpacing(8);
    formatRow(plD, m_config.clockDigital24h);
    colorRow(plD, "Background", m_config.clockDigitalBg);
    gradientRow(plD, "Text", m_config.clockDigitalStops);

    l->addWidget(pageAnalog);
    l->addWidget(pageGrad);
    l->addWidget(pageDigital);
    l->addStretch();

    auto showFace = [pageAnalog, pageGrad, pageDigital](int i){
        pageAnalog->setVisible(i == 0);
        pageGrad->setVisible(i == 1);
        pageDigital->setVisible(i == 2);
    };

    connect(faceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, showFace](int i){
        m_config.clockStyle = i;
        showFace(i);
        saveConfig();
    });

    // État initial : sélection + conteneur correspondant au style sauvegardé.
    const int st = std::clamp(m_config.clockStyle, 0, 2);
    faceCombo->blockSignals(true); faceCombo->setCurrentIndex(st); faceCombo->blockSignals(false);
    showFace(st);
}

void KrakenLCDWidget::buildModePanel_AudioVisual()
{
    m_panelAudio = new QWidget; auto* l = new QVBoxLayout(m_panelAudio); l->setSpacing(8);
    auto* lgRow = new QHBoxLayout;
    auto* ck = new QCheckBox("Logo Display"); ck->setChecked(m_config.audioShowLogo);
    connect(ck,&QCheckBox::toggled,[this](bool b){m_config.audioShowLogo=b; saveConfig();});
    auto* bLogo = makeColorButton(m_config.audioLogoColor);
    connectColorButton(bLogo, m_config.audioLogoColor);
    lgRow->addWidget(ck); lgRow->addWidget(bLogo); lgRow->addStretch(); l->addLayout(lgRow);
    auto* invRow = new QHBoxLayout;
    auto* inv = new QCheckBox("Invert Colors"); inv->setChecked(m_config.audioInvert);
    connect(inv,&QCheckBox::toggled,[this](bool b){m_config.audioInvert=b; saveConfig();});
    invRow->addWidget(inv); invRow->addStretch(); l->addLayout(invRow);
    auto* vzRow = new QHBoxLayout; vzRow->addWidget(new QLabel("Visualization"));
    auto* bVz = makeGradientButton(m_config.audioStops);
    connectGradientButton(bVz, m_config.audioStops);   // mêmes presets que single/triple
    vzRow->addWidget(bVz); vzRow->addStretch(); l->addLayout(vzRow);
    auto* cnRow = new QHBoxLayout;
    auto* cn = new QCheckBox("Connected"); cn->setChecked(m_config.audioConnected);
    connect(cn,&QCheckBox::toggled,[this](bool b){m_config.audioConnected=b; saveConfig();});
    cnRow->addWidget(cn); cnRow->addStretch(); l->addLayout(cnRow);
    // ── Sensibilité exprimée en dB (plancher de bruit) ──
    // Low = -30 dB (à gauche) … High = -120 dB (à droite), par pas de 10 dB.
    auto* sensHdr = new QHBoxLayout;
    sensHdr->addWidget(new QLabel("Sensitivity"));
    sensHdr->addStretch();
    auto* sensVal = new QLabel;
    sensHdr->addWidget(sensVal);
    l->addLayout(sensHdr);

    auto* sens = new QSlider(Qt::Horizontal);
    sens->setRange(-120, -30);                 // dB
    sens->setSingleStep(10);
    sens->setPageStep(10);
    sens->setTickInterval(10);
    sens->setTickPosition(QSlider::TicksBelow);
    sens->setInvertedAppearance(true);         // -30 (Low) à gauche, -120 (High) à droite
    // audioSensitivity 0..1 → dB : -30 - s*90  (donc -30 dB = Low, -120 dB = High)
    sens->setValue(int(std::lround(-30.0 - std::clamp(m_config.audioSensitivity, 0.f, 1.f) * 90.0)));
    auto syncSensLabel = [sensVal](int db){ sensVal->setText(QString("%1 dB").arg(db)); };
    syncSensLabel(sens->value());
    connect(sens,&QSlider::valueChanged,[this,sens,syncSensLabel](int v){
        int snapped = int(std::lround(v / 10.0) * 10);   // verrouille sur des pas de 10 dB
        snapped = std::clamp(snapped, -120, -30);
        if (snapped != v) { sens->blockSignals(true); sens->setValue(snapped); sens->blockSignals(false); }
        syncSensLabel(snapped);
        m_config.audioSensitivity = float((-30 - snapped) / 90.0);   // -30->0 (Low), -120->1 (High)
        saveConfig();
    });
    l->addWidget(sens);
    auto* sensRow = new QHBoxLayout;
    sensRow->addWidget(new QLabel("Low")); sensRow->addStretch(); sensRow->addWidget(new QLabel("High"));
    l->addLayout(sensRow);
    l->addStretch();
}

void KrakenLCDWidget::buildModePanel_NowPlaying()
{
    m_panelNowPlaying = new QWidget;
    auto* l = new QVBoxLayout(m_panelNowPlaying); l->setSpacing(8);

    auto* progRow = new QHBoxLayout;
    auto* progCk = new QCheckBox("Show progress bar and time");
    progCk->setChecked(m_config.npShowProgress);
    connect(progCk, &QCheckBox::toggled, [this](bool b){ m_config.npShowProgress = b; saveConfig(); });
    progRow->addWidget(progCk); progRow->addStretch();
    l->addLayout(progRow);

    auto* bgRow = new QHBoxLayout;
    bgRow->addWidget(new QLabel("Background"));
    // npBgColor : fond propre à Now Playing (avant : partagé avec le
    // Clockface Analog via m_config.bgColor → les deux boutons se pilotaient
    // mutuellement sans rafraîchir leurs swatches).
    auto* bBg = makeColorButton(m_config.npBgColor);
    connectColorButton(bBg, m_config.npBgColor);
    bgRow->addWidget(bBg); bgRow->addStretch();
    l->addLayout(bgRow);

    auto* artRow = new QHBoxLayout;
    artRow->addWidget(new QLabel("Artist"));
    auto* bArt = makeTextColorButton(m_config.npArtistFill, m_config.npArtistOutline);
    connectTextColorButton(bArt, m_config.npArtistFill, m_config.npArtistOutline);
    artRow->addWidget(bArt); artRow->addStretch();
    l->addLayout(artRow);

    auto* titRow = new QHBoxLayout;
    titRow->addWidget(new QLabel("Title"));
    auto* bTit = makeTextColorButton(m_config.npTitleFill, m_config.npTitleOutline);
    connectTextColorButton(bTit, m_config.npTitleFill, m_config.npTitleOutline);
    titRow->addWidget(bTit); titRow->addStretch();
    l->addLayout(titRow);

    l->addStretch();
}

// ─────────────────────────────────────────────────────────────────────────────
// Slots
// ─────────────────────────────────────────────────────────────────────────────
void KrakenLCDWidget::onModeChanged(int /*index*/)
{
    if (!m_ready) return;   // pas encore initialisé

    DisplayMode mode = static_cast<DisplayMode>(m_modeCombo->currentData().toInt());
    m_config.mode = mode;
    saveConfig();

    // Show/hide du conteneur media (selon le mode + le toggle GIF par mode)
    updateMediaVisibility();

    // Panel correspondant
    if (m_stack) {
        const QMap<DisplayMode, QWidget*> panelMap = {
            { DisplayMode::IMAGE_GIF,              m_panelImageGIF },
            { DisplayMode::SINGLE_INFOGRAPHIC,     m_panelSingle   },
            { DisplayMode::DUAL_INFOGRAPHIC,       m_panelDual     },
            { DisplayMode::TRIPLE_INFOGRAPHIC,     m_panelTriple   },
            { DisplayMode::CLOCKFACE,              m_panelClock    },
            { DisplayMode::AUDIO_VISUAL,           m_panelAudio    },
            { DisplayMode::NOW_PLAYING,            m_panelNowPlaying },
        };
        if (auto* p = panelMap.value(mode))
            m_stack->setCurrentWidget(p);
    }

    // Cadence de rendu + WASAPI : geres par le worker (applyCadence),
    // declenche par le setConfig() ci-dessus quand le mode change.
}

// onRenderTick / onSensorTick : deplaces dans KrakenWorker (thread de rendu).

void KrakenLCDWidget::onRotateClicked()
{
    // Rotation purement logicielle : saveConfig() pousse cfg.rotation au
    // worker, le renderer applique la rotation au dessin de chaque frame.
    m_config.rotation = (m_config.rotation + 90) % 360;
    saveConfig();
}

void KrakenLCDWidget::setDarkTheme(bool dark)
{
    m_darkTheme = dark;
    if (m_lblStatus)      m_lblStatus->setStyleSheet(statusLabelStyle(dark));
    if (m_lblMediaHelper) m_lblMediaHelper->setStyleSheet(helperLabelStyle(dark));
    if (m_preview)        m_preview->update();   // repeint l'apercu avec la nouvelle palette
}

void KrakenLCDWidget::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    // OpenRGB applique sa palette (sombre/claire) à toute l'application ; on suit
    // ses changements de thème à chaud pour restyler les boutons Upload/Browse.
    if (e && (e->type() == QEvent::PaletteChange ||
              e->type() == QEvent::ApplicationPaletteChange)) {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        if (dark != m_darkTheme)
            setDarkTheme(dark);
    }
    // OpenRGB change la langue à chaud (installTranslator -> LanguageChange) ;
    // on resuit l'unité de température (°C/°F) selon la locale OpenRGB.
    if (e && e->type() == QEvent::LanguageChange) {
        FrameRenderer::setFahrenheit(OpenRGBSettings::prefersFahrenheit());
    }
}

} // namespace NZXTKraken
