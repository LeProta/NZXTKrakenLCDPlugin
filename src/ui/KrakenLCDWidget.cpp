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
#include "OpenRGBPluginInterface.h"

namespace NZXTKraken {

static const QString IMAGE_FILTER =
    QStringLiteral("Images (*.gif *.png *.jpg *.jpeg)");

static const QStringList ALLOWED_EXTENSIONS = {
    "gif", "png", "jpg", "jpeg"
};

static constexpr qint64 MAX_FILE_SIZE = 32LL * 1024 * 1024;   // 32 MiB

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
    }
    else {
        p.fillRect(6,6,208,208,offScr);
        p.setPen(hint);
        p.setFont(QFont("Segoe UI",11));
        p.drawText(QRectF(6,6,208,208),Qt::AlignCenter,"No\ndevice");
    }
}

ColorSwatchButton* KrakenLCDWidget::makeColorButton(QColor c)
{
    auto* btn = new ColorSwatchButton;
    btn->setSolid(c);
    btn->setFixedSize(36, 24);
    return btn;
}

void KrakenLCDWidget::connectColorButton(ColorSwatchButton* btn, QColor& target)
{
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
    btn->setFixedSize(36, 24);
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
    btn->setFixedSize(36, 24);
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

void KrakenLCDWidget::flashErrorBorder(QPushButton* btn)
{
    if (!btn) return;

    const QColor normalBg = m_darkTheme ? QColor(0x1F,0x1F,0x23) : QColor(0xEB,0xEB,0xEE);
    const QColor errBg    = m_darkTheme ? QColor(0xCC,0x22,0x22) : QColor(0xEE,0x33,0x33);
    const QString border  = m_darkTheme ? "#2A2A2E" : "#D0D0D4";
    const QString fg      = m_darkTheme ? "#FFFFFF" : "#1A1A1E";

    QPointer<QPushButton> safeBtn(btn);

    auto applyBg = [safeBtn, border, fg](const QColor& c) {
        if (!safeBtn) return;
        safeBtn->setStyleSheet(
            QString("QPushButton{background:%1;color:%2;"
                    "border:1px solid %3;border-radius:5px;padding:5px 10px;font-size:11px;}")
            .arg(c.name(), fg, border));
    };

    applyBg(errBg);

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
                safeBtn->setStyleSheet(QString());
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
    if (QString* tgt = activeGifPath()) {
        if (tgt->isEmpty()) return;
        tgt->clear();
        saveConfig();
    }
}

void KrakenLCDWidget::saveConfig()
{
    if (!m_ready) return;
    if (m_worker) m_worker->setConfig(m_config);
    if (m_saveTimer) m_saveTimer->start();
}

static bool isKrakenController(RGBControllerInterface* c)
{
    const QString loc = QString::fromStdString(c->GetLocation());
    for (const auto& dev : SUPPORTED_DEVICES)
        if (loc.contains(QString::asprintf("VID_%04X&PID_%04X", dev.vid, dev.pid),
                         Qt::CaseInsensitive))
            return true;
    return false;
}

// OpenRGB writes the Kraken ring colors only once (profile load, which at boot races with
// this plugin opening the device), so a lost packet leaves LEDs dark. SignalRGB instead
// re-sends every frame: do the same with OpenRGB's own colors, every 2 s.
void KrakenLCDWidget::reapplyOpenRGBLighting()
{
    // Detection (startup, rescan, 1.1 hotplug) creates/deletes controllers: wait for it.
    // shortcut: a Kraken unplugged during this loop can still race, guard via ResourceManagerUpdated if it ever crashes.
    if (!m_rm || m_rm->GetDetectionPercent() < 100) return;
    for (RGBControllerInterface* c : m_rm->GetRGBControllers()) {
        if (!c || !isKrakenController(c)) continue;
        const int mode = c->GetActiveMode();
        if (mode < 0 || unsigned(mode) >= c->GetModeCount()
            || c->GetModeColorMode(mode) != MODE_COLORS_PER_LED)
            continue;   // hardware effect: re-sending would restart its animation
        bool anyLit = false;
        for (unsigned i = 0; i < c->GetLEDCount() && !anyLit; ++i) anyLit = c->GetColor(i) != 0;
        if (!anyLit)
            continue;   // no profile loaded yet: keep whatever the AIO shows
        c->UpdateMode();
    }
}

KrakenLCDWidget::KrakenLCDWidget(OpenRGBPluginAPIInterface* rm, QWidget* parent)
    : QWidget(parent), m_rm(rm)
{

    m_config = KrakenUiPersistence::load();
    DisplayMode savedMode = m_config.mode;
    int savedBrightness   = m_config.brightness;

    m_darkTheme = palette().color(QPalette::Window).lightness() < 128;

    buildUI();

    m_modeCombo->blockSignals(true);
    for (int i = 0; i < m_modeCombo->count(); i++) {
        if (m_modeCombo->itemData(i).toInt() == int(savedMode)) {
            m_modeCombo->setCurrentIndex(i);
            break;
        }
    }
    m_modeCombo->blockSignals(false);

    m_brightnessSlider->blockSignals(true);
    m_brightnessSlider->setValue(savedBrightness);
    m_brightnessSlider->blockSignals(false);

    m_config.mode       = savedMode;
    m_config.brightness = savedBrightness;

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(250);
    connect(m_saveTimer, &QTimer::timeout, this,
            [this]{ KrakenUiPersistence::save(m_config); });

    m_ready = true;

    onModeChanged(m_modeCombo->currentIndex());

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
                if (ok && !m_rgbTimer) {
                    m_rgbTimer = new QTimer(this);
                    connect(m_rgbTimer, &QTimer::timeout, this,
                            &KrakenLCDWidget::reapplyOpenRGBLighting);
                    m_rgbTimer->start(2000);
                }
            });
    connect(m_worker, &KrakenWorker::deviceStatus, this,
            [this](const QString& t) { if (m_lblStatus) m_lblStatus->setText(t); });
    connect(m_worker, &KrakenWorker::previewReady, this,
            [this](const QImage& f) { if (m_preview) m_preview->setFrame(f); });

    m_thread->start();

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
    if (m_saveTimer) m_saveTimer->stop();
    if (m_ready) KrakenUiPersistence::save(m_config);
    m_device.requestAbort();
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

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

    m_mediaContainer->setVisible(false);
}

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

    m_updateBanner = new QLabel;
    m_updateBanner->setTextFormat(Qt::RichText);
    m_updateBanner->setOpenExternalLinks(true);
    m_updateBanner->setWordWrap(true);
    m_updateBanner->setStyleSheet(QStringLiteral("font-size:12px;font-weight:600;"));
    m_updateBanner->hide();
    leftCol->addWidget(m_updateBanner);

    buildModeSelector();
    leftCol->addWidget(m_modeCombo);

    auto* bRow = new QHBoxLayout;
    bRow->addWidget(new QLabel("☀"));
    m_brightnessSlider = new QSlider(Qt::Horizontal);
    m_brightnessSlider->setRange(0,100);
    m_brightnessSlider->setValue(100);
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
        brightnessSend->start();
    });
    bRow->addWidget(m_brightnessSlider);
    bRow->addWidget(new QLabel("☀"));
    leftCol->addLayout(bRow);

    buildMediaContainer();
    leftCol->addWidget(m_mediaContainer);

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

    m_modeCombo->blockSignals(true);

    m_modeCombo->addItem("Image/GIF",                   int(DisplayMode::IMAGE_GIF));
    m_modeCombo->addItem("Single Infographic",          int(DisplayMode::SINGLE_INFOGRAPHIC));
    m_modeCombo->addItem("Dual Infographic",            int(DisplayMode::DUAL_INFOGRAPHIC));
    m_modeCombo->addItem("Triple Infographic",          int(DisplayMode::TRIPLE_INFOGRAPHIC));
    m_modeCombo->addItem("Clockface",                   int(DisplayMode::CLOCKFACE));
    m_modeCombo->addItem("Audio Visual",                int(DisplayMode::AUDIO_VISUAL));
    m_modeCombo->addItem("Now Playing",                 int(DisplayMode::NOW_PLAYING));

    m_modeCombo->blockSignals(false);

    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KrakenLCDWidget::onModeChanged);
}

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
    
    auto mRow = new QHBoxLayout; mRow->addWidget(new QLabel("Display Mode"));
    auto* dc = new QComboBox; dc->addItem("Vertical"); dc->addItem("Horizontal");
    if (!m_config.dualVertical) dc->setCurrentIndex(1);
    mRow->addWidget(dc); mRow->addStretch(); l->addLayout(mRow);

    auto* bgRow = new QHBoxLayout; bgRow->addWidget(new QLabel("Background"));
    auto* bBg = makeColorButton(m_config.dual.bgColor);
    connectColorButton(bBg,m_config.dual.bgColor);
    bgRow->addWidget(bBg); bgRow->addStretch();
    l->addLayout(bgRow);

    auto* lgWrap = new QWidget;
    auto* lgRow = new QHBoxLayout(lgWrap);
    lgRow->setContentsMargins(0, 0, 0, 0);
    
    auto* ck = new QCheckBox("Logo"); ck->setChecked(m_config.dual.showLogo);
    connect(ck, &QCheckBox::toggled, [this](bool b){ m_config.dual.showLogo = b; saveConfig(); });
    lgRow->addWidget(ck);
    auto* bLg = makeColorButton(m_config.dual.logoColor);
    connectColorButton(bLg, m_config.dual.logoColor);
    lgRow->addWidget(bLg); lgRow->addStretch();
    
    l->addWidget(lgWrap);

    auto updateLogoVis = [lgWrap, this]() {
        lgWrap->setVisible(m_config.dualVertical);
    };
    updateLogoVis();

    connect(dc, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, lgWrap](int i){
        m_config.dualVertical = (i == 0);
        saveConfig();
        lgWrap->setVisible(m_config.dualVertical);
    });

    l->addWidget(new QLabel("Reading 1"));
    SENSOR_BLOCK("", m_config.dualReading1);
    { auto vr=new QHBoxLayout; vr->addWidget(new QLabel("Visualization"));
      auto* b=makeColorButton(m_config.dualReading1.colorViz);
      connectColorButton(b,m_config.dualReading1.colorViz);
      vr->addWidget(b); vr->addStretch(); l->addLayout(vr); }

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

    auto colorRow = [&](QVBoxLayout* lay, const QString& lbl, QColor& col){
        auto* row = new QHBoxLayout; row->addWidget(new QLabel(lbl));
        auto* b = makeColorButton(col); connectColorButton(b, col);
        row->addWidget(b); row->addStretch(); lay->addLayout(row);
    };
    auto gradientRow = [&](QVBoxLayout* lay, const QString& lbl, GradientStops& stops){
        auto* row = new QHBoxLayout; row->addWidget(new QLabel(lbl));
        auto* b = makeGradientButton(stops);
        connectGradientButton(b, stops, CLOCK_GRADIENT_PRESETS);
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

    auto mRow = new QHBoxLayout; mRow->addWidget(new QLabel("Clockface"));
    auto* faceCombo = new QComboBox;
    faceCombo->addItem("Analog"); faceCombo->addItem("Gradient"); faceCombo->addItem("Digital");
    mRow->addWidget(faceCombo); mRow->addStretch(); l->addLayout(mRow);

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
    connectGradientButton(bVz, m_config.audioStops);
    vzRow->addWidget(bVz); vzRow->addStretch(); l->addLayout(vzRow);
    auto* cnRow = new QHBoxLayout;
    auto* cn = new QCheckBox("Connected"); cn->setChecked(m_config.audioConnected);
    connect(cn,&QCheckBox::toggled,[this](bool b){m_config.audioConnected=b; saveConfig();});
    cnRow->addWidget(cn); cnRow->addStretch(); l->addLayout(cnRow);
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
    sens->setInvertedAppearance(true);
    sens->setValue(int(std::lround(-30.0 - std::clamp(m_config.audioSensitivity, 0.f, 1.f) * 90.0)));
    auto syncSensLabel = [sensVal](int db){ sensVal->setText(QString("%1 dB").arg(db)); };
    syncSensLabel(sens->value());
    connect(sens,&QSlider::valueChanged,[this,sens,syncSensLabel](int v){
        int snapped = int(std::lround(v / 10.0) * 10);
        snapped = std::clamp(snapped, -120, -30);
        if (snapped != v) { sens->blockSignals(true); sens->setValue(snapped); sens->blockSignals(false); }
        syncSensLabel(snapped);
        m_config.audioSensitivity = float((-30 - snapped) / 90.0);
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

void KrakenLCDWidget::onModeChanged(int )
{
    if (!m_ready) return;

    DisplayMode mode = static_cast<DisplayMode>(m_modeCombo->currentData().toInt());
    m_config.mode = mode;
    saveConfig();

    updateMediaVisibility();

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

}

void KrakenLCDWidget::onRotateClicked()
{
    m_config.rotation = (m_config.rotation + 90) % 360;
    saveConfig();
}

void KrakenLCDWidget::setDarkTheme(bool dark)
{
    m_darkTheme = dark;
    if (m_lblStatus)      m_lblStatus->setStyleSheet(statusLabelStyle(dark));
    if (m_lblMediaHelper) m_lblMediaHelper->setStyleSheet(helperLabelStyle(dark));
    if (m_preview)        m_preview->update();
}

void KrakenLCDWidget::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e && (e->type() == QEvent::PaletteChange ||
              e->type() == QEvent::ApplicationPaletteChange)) {
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        if (dark != m_darkTheme)
            setDarkTheme(dark);
    }
    if (e && e->type() == QEvent::LanguageChange) {
        FrameRenderer::setFahrenheit(OpenRGBSettings::prefersFahrenheit());
    }
}

} // namespace NZXTKraken
