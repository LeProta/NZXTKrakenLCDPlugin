#include "KrakenWorker.h"
#include "../device/NZXTKrakenDevice.h"
#include "../sensors/SystemSensors.h"
#include "KrakenOpenRGBSettings.h"
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define INITGUID
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#  include <mmreg.h>
#  include <ks.h>
#  include <ksmedia.h>   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (WAVE_FORMAT_EXTENSIBLE)
#endif

namespace NZXTKraken {

static bool modeUsesMedia(const FrameConfig& cfg)
{
    switch (cfg.mode) {
        case DisplayMode::IMAGE_GIF:          return !cfg.gifPath.isEmpty();
        case DisplayMode::SINGLE_INFOGRAPHIC: return !cfg.single.gifPath.isEmpty();
        case DisplayMode::DUAL_INFOGRAPHIC:   return !cfg.dual.gifPath.isEmpty();
        case DisplayMode::TRIPLE_INFOGRAPHIC: return !cfg.triple.gifPath.isEmpty();
        default:                              return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
KrakenWorker::KrakenWorker(NZXTKrakenDevice* device, SystemSensors* sensors)
    : m_device(device), m_sensors(sensors), m_renderer(sensors)
{
}

KrakenWorker::~KrakenWorker() = default;

FrameConfig KrakenWorker::currentConfig()
{
    QMutexLocker lk(&m_cfgMutex);
    return m_cfg;
}

// Ouverture complète : device + réglages initiaux + signal UI + cadence.
// Utilisé au démarrage ET par la reconnexion à chaud (renderTick).
bool KrakenWorker::tryOpenDevice()
{
    if (!m_device->open()) return false;
    const FrameConfig cfg = currentConfig();
    m_device->setBrightness(cfg.brightness);
    m_device->setRotation(cfg.rotation);
    emit deviceOpened(QString::fromStdString(m_device->deviceName()), true);
    applyCadence();   // reprend la cadence de l'écran du modèle détecté
    m_sendFailCount = 0;
    return true;
}

void KrakenWorker::setConfig(const FrameConfig& cfg)
{
    bool modeChanged;
    {
        QMutexLocker lk(&m_cfgMutex);
        modeChanged = (cfg.mode != m_cfg.mode);
        m_cfg = cfg;
    }
    // La cadence + WASAPI dépendent du mode : on (re)calcule sur le thread du
    // worker quand le mode change (applyCadence touche le QTimer, qui a son
    // affinité sur ce thread).
    if (modeChanged)
        QMetaObject::invokeMethod(this, "applyCadence", Qt::QueuedConnection);
}

// ── Démarrage (sur le thread du worker, via QThread::started) ───────────────
void KrakenWorker::start()
{
#ifdef _WIN32
    // COM pour WMI (capteurs) + WASAPI, sur CE thread. MTA = pas de pompe de
    // messages requise. On n'équilibre par CoUninitialize que si on a réellement
    // initialisé (S_OK) ; S_FALSE/RPC_E_CHANGED_MODE → on ne reprend pas de réf.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_comInit = (hr == S_OK);
#endif

    // Unité de température : suit la locale/langue OpenRGB (°F si region US).
    FrameRenderer::setFahrenheit(OpenRGBSettings::prefersFahrenheit());

    const bool ok = tryOpenDevice();
    if (!ok)
        emit deviceOpened(QStringLiteral("No Kraken LCD device detected"), false);

    // Capteurs demarres MEME sans Kraken : l'apercu UI (infographics) doit
    // afficher CPU/GPU/RAM, pas des "--" partout.
    m_sensors->startPolling();

    m_renderTimer = new QTimer(this);
    connect(m_renderTimer, &QTimer::timeout, this, &KrakenWorker::renderTick);
    applyCadence();
    m_renderTimer->start();

    m_sensorTimer = new QTimer(this);
    connect(m_sensorTimer, &QTimer::timeout, this, &KrakenWorker::sensorTick);
    m_sensorTimer->start(1000);

    qInfo() << "[KrakenLCD/Worker] started \u2014 device" << (ok ? "open" : "not detected")
            << "\u2014 render/sensor loops running";
}

// ── Arrêt (BlockingQueuedConnection depuis le destructeur du widget) ────────
void KrakenWorker::stop()
{
    qInfo() << "[KrakenLCD/Worker] stopping (timers, sensors, device)";
    if (m_renderTimer) { m_renderTimer->stop(); m_renderTimer->deleteLater(); m_renderTimer = nullptr; }
    if (m_sensorTimer) { m_sensorTimer->stop(); m_sensorTimer->deleteLater(); m_sensorTimer = nullptr; }
#ifdef _WIN32
    closeWasapi();
#endif
    if (m_sensors) m_sensors->shutdown();   // stoppe le timer + libère les providers COM, SUR CE thread
    if (m_device)  m_device->close();        // ferme sur le thread qui a ouvert (cohérence libusb/WinHID)
#ifdef _WIN32
    if (m_comInit) { CoUninitialize(); m_comInit = false; }
#endif
}

void KrakenWorker::setBrightness(int percent)
{
    if (m_device) m_device->setBrightness(percent);
}

void KrakenWorker::setRotation(int degrees)
{
    if (m_device) m_device->setRotation(degrees);
}

// ── Cadence de rendu + (dé)activation WASAPI selon le mode ──────────────────
void KrakenWorker::applyCadence()
{
    if (!m_renderTimer) return;
    const FrameConfig cfg = currentConfig();

#ifdef _WIN32
    if (cfg.mode == DisplayMode::AUDIO_VISUAL) initWasapi();
    else                                       closeWasapi();
#endif

    const int devFps   = (m_device->isOpen() && m_device->info() && m_device->info()->maxFps > 0)
                           ? m_device->info()->maxFps : 30;
    const int screenMs = std::max(1, 1000 / devFps);   // période de l'écran du modèle

    int interval;
    if (cfg.mode == DisplayMode::AUDIO_VISUAL) interval = screenMs;            // plein régime (≤ écran)
    else if (modeUsesMedia(cfg)
             || cfg.mode == DisplayMode::NOW_PLAYING
             || cfg.mode == DisplayMode::CLOCKFACE) interval = screenMs;  // GIF/horloge/musique : cadence max de l'ecran
    else                                       interval = std::max(screenMs, 200); // statique ~5 fps

    m_renderTimer->setInterval(interval);
}

// ── Boucle de rendu : rend → (preview) → encode → envoie, tout sur ce thread ─
void KrakenWorker::renderTick()
{
    const FrameConfig cfg = currentConfig();

    // Mode musique : timeline (position/lecture) rafraîchie à chaque frame pour un
    // timestamp fluide qui suit aussi les seeks (appel synchrone léger ; les
    // métadonnées + pochette restent gérées au rythme lent de sensorTick).
    if (cfg.mode == DisplayMode::NOW_PLAYING) {
        MediaSnapshot snap;
        m_media.pollTimeline(snap);
        m_renderer.setMediaInfo(snap);
    }

#ifdef _WIN32
    if (cfg.mode == DisplayMode::AUDIO_VISUAL) {
        // Re-init throttlée (~2 s) si la capture est tombée (changement de
        // périphérique de sortie) : sans ça le visualiseur restait mort
        // jusqu'au prochain changement de mode.
        if (!m_wasapiReady
            && (!m_wasapiRetryClock.isValid() || m_wasapiRetryClock.elapsed() >= 2000)) {
            m_wasapiRetryClock.restart();
            initWasapi();
        }
        if (m_wasapiReady)
            m_renderer.setAudioLevels(readWasapiBands());
    }
#endif

    QImage frame = m_renderer.render(cfg);

    // Apercu UI plafonne a ~30 fps (decouple de la cadence device) : l'UI n'a pas
    // besoin de plus, et ca evite d'inonder le thread UI quand l'ecran tourne a 60 fps.
    if (!m_previewClock.isValid() || m_previewClock.elapsed() >= 33) {
        emit previewReady(frame);   // copie COW vers l'UI (QueuedConnection)
        m_previewClock.restart();
    }

    if (m_device->isOpen() && m_device->info()) {
        QImage target = frame;
        if (m_device->info()->lcdWidth != frame.width())
            target = frame.scaled(m_device->info()->lcdWidth, m_device->info()->lcdHeight,
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        QByteArray payload = (m_device->info()->streamKind == 0x08)
                           ? FrameRenderer::toQ565(target)
                           : FrameRenderer::toJpeg(target, 90);

        if (m_device->sendFrame(payload)) {   // bloquant — mais HORS du thread UI
            m_sendFailCount = 0;
        } else if (++m_sendFailCount >= 3) {
            // Device décroché (câble, reset USB, veille) : on ferme et on
            // repasse par la reconnexion périodique ci-dessous.
            m_sendFailCount = 0;
            m_device->close();
            m_reopenDelayMs = 3000;
            m_reopenClock.restart();
            emit deviceOpened(QStringLiteral("Device lost — reconnecting…"), false);
            qWarning() << "[KrakenLCD/Worker] device unresponsive — closed, will retry every 3 s";
        }
    } else {
        // Reconnexion à chaud : sondage léger (hid_enumerate) ; open() complet
        // seulement si un Kraken est présent. Si présent mais non ouvrable
        // (NZXT CAM lancé, canal occupé), backoff 3 s → 30 s pour ne pas
        // remplir le log de warnings d'open() raté.
        if (!m_reopenClock.isValid() || m_reopenClock.elapsed() >= m_reopenDelayMs) {
            m_reopenClock.restart();
            if (NZXTKrakenDevice::anySupportedPresent()) {
                if (tryOpenDevice()) m_reopenDelayMs = 3000;
                else m_reopenDelayMs = std::min(m_reopenDelayMs * 2, 30000);
            } else {
                m_reopenDelayMs = 3000;   // absent = sondage silencieux, pas de backoff
            }
        }
    }
}

// ── Lecture capteurs HID du Kraken (température liquide / RPM) ───────────────
void KrakenWorker::sensorTick()
{
    // Mode musique : on rafraîchit le "now playing" (SMTC) ~1×/s. Indépendant
    // du device (l'aperçu UI fonctionne aussi sans Kraken branché).
    if (currentConfig().mode == DisplayMode::NOW_PLAYING) {
        MediaSnapshot snap;
        m_media.poll(snap);
        m_renderer.setMediaInfo(snap);
    }

    if (!m_device || !m_device->isOpen()) return;
    DeviceReadings r;
    if (m_device->readSensors(r)) {
        if (m_sensors) m_sensors->setLiquidTemp(r.liquidTemp);   // brut °C (capteurs/arcs)
        const bool fahr  = FrameRenderer::fahrenheit();
        const double liq = fahr ? r.liquidTemp * 9.0 / 5.0 + 32.0 : r.liquidTemp;
        emit deviceStatus(QStringLiteral("Liq. %1\u00b0%2  Pump %3 RPM  Fan %4 RPM")
                          .arg(liq, 0, 'f', 1)
                          .arg(fahr ? QStringLiteral("F") : QStringLiteral("C"))
                          .arg(r.pumpRPM).arg(r.fanRPM));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// WASAPI loopback (déplacé depuis KrakenLCDWidget — capture sur le thread de rendu).
// COM est déjà initialisé (MTA) par start() ; on ne refait PAS CoInitializeEx ici.
// ═════════════════════════════════════════════════════════════════════════════
#ifdef _WIN32

void KrakenWorker::initWasapi()
{
    if (m_wasapiReady) return;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
        return;

    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
        { enumerator->Release(); return; }

    IAudioClient* client = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)))
        { device->Release(); enumerator->Release(); return; }
    device->Release();

    WAVEFORMATEX* fmt = nullptr;
    client->GetMixFormat(&fmt);

    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, fmt, nullptr)))
        { CoTaskMemFree(fmt); client->Release(); enumerator->Release(); return; }

    IAudioCaptureClient* capture = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture)))
        { CoTaskMemFree(fmt); client->Release(); enumerator->Release(); return; }

    client->Start();

    m_wasapiEnum    = enumerator;
    m_wasapiClient  = client;
    m_wasapiCapture = capture;
    m_wasapiFormat  = fmt;
    m_wasapiCh      = fmt->nChannels;
    m_wasapiAcc.fill(0.f, 64);
    m_wasapiReady   = true;
    qDebug() << "[KrakenLCD/Audio] WASAPI loopback initialized \u2014"
             << fmt->nSamplesPerSec << "Hz," << fmt->nChannels << "ch";
}

void KrakenWorker::closeWasapi()
{
    if (!m_wasapiReady) return;
    auto* client  = (IAudioClient*)m_wasapiClient;
    auto* capture = (IAudioCaptureClient*)m_wasapiCapture;
    auto* enumer  = (IMMDeviceEnumerator*)m_wasapiEnum;
    if (client)  client->Stop();
    if (capture) capture->Release();
    if (client)  client->Release();
    if (enumer)  enumer->Release();
    if (m_wasapiFormat) CoTaskMemFree(m_wasapiFormat);
    m_wasapiClient = m_wasapiCapture = m_wasapiEnum = m_wasapiFormat = nullptr;
    m_wasapiReady = false;
}

QVector<float> KrakenWorker::readWasapiBands()
{
    constexpr int N = 64;
    QVector<float> bands(N, 0.f);
    if (!m_wasapiReady) return bands;

    auto* capture = (IAudioCaptureClient*)m_wasapiCapture;
    auto* fmt     = (WAVEFORMATEX*)m_wasapiFormat;

    if (m_wasapiAcc.size() < N) m_wasapiAcc.fill(0.f, N);

    // Detection de format robuste : EXTENSIBLE -> SubFormat, sinon wFormatTag.
    // Format non float et non PCM 16 bits -> ignore (silence) plutot que
    // d'interpreter les octets n'importe comment (garbage a l'ecran).
    bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    bool isS16   = (fmt->wFormatTag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        isS16   = !isFloat && IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)
                           && fmt->wBitsPerSample == 16;
    }

    UINT32 packetSize = 0;
    HRESULT hr = capture->GetNextPacketSize(&packetSize);
    if (FAILED(hr)) {
        // Peripherique par defaut change/invalide (AUDCLNT_E_DEVICE_INVALIDATED) :
        // on ferme ; renderTick re-initialisera (throttle ~2 s) sur le nouveau
        // device par defaut. Pas d'initWasapi() ici : pendant la transition
        // il echouerait, et le visualiseur resterait mort.
        qWarning() << "[KrakenLCD/Audio] capture lost (" << Qt::hex << (quint32)hr
                   << ") — will reinitialize WASAPI on default device";
        closeWasapi();
        return bands;
    }
    while (packetSize > 0) {
        BYTE* data; UINT32 frames; DWORD flags;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames > 0 && (isFloat || isS16)) {
            int ch = m_wasapiCh;
            int perBand = std::max(1u, frames / (UINT32)N);

            for (int b = 0; b < N; b++) {
                float sum = 0.f;
                for (int j = 0; j < perBand; j++) {
                    int idx = (b * perBand + j) * ch;
                    float s = 0.f;
                    if (isFloat) {
                        auto* f = (float*)data;
                        if (idx < (int)(frames * ch)) s = std::abs(f[idx]);
                    } else {
                        auto* i16 = (int16_t*)data;
                        if (idx < (int)(frames * ch)) s = std::abs(i16[idx]) / 32768.f;
                    }
                    sum += s;
                }
                m_wasapiAcc[b] = std::max(m_wasapiAcc[b], sum / perBand);
            }
        }
        capture->ReleaseBuffer(frames);
        if (FAILED(capture->GetNextPacketSize(&packetSize))) break;
    }

    for (int b = 0; b < N; b++) {
        bands[b]        = m_wasapiAcc[b];
        m_wasapiAcc[b] *= 0.82f;
    }
    return bands;
}

#endif // _WIN32

} // namespace NZXTKraken
