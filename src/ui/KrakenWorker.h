#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include "../renderer/FrameRenderer.h"   // FrameRenderer, FrameConfig, DisplayMode (inclut SystemSensors.h)
#include "../sensors/MediaSession.h"     // MediaSession, MediaSnapshot (mode NOW_PLAYING)

class QTimer;

namespace NZXTKraken {

class NZXTKrakenDevice;   // emprunté (possédé par le widget)

// ─────────────────────────────────────────────────────────────────────────────
// KrakenWorker
//   Vit sur son propre QThread. Possède TOUTE l'I/O lourde qui causait les
//   micro-saccades quand elle tournait sur le thread UI :
//     • rendu QPainter + encodage (Q565 / JPEG) ;
//     • envoi USB (sendFrame, bloquant) — un seul thread, donc PAS de backlog :
//       chaque tick rend la config la plus fraîche puis l'envoie
//       (latest-frame-wins implicite, aucune file qui gonfle) ;
//     • polling capteurs système (WMI/PDH/D3DKMT) + lecture HID du Kraken ;
//     • capture audio WASAPI loopback.
//   Le device et les capteurs restent possédés par le widget ; le worker ne fait
//   que les piloter, EXCLUSIVEMENT depuis ce thread (aucune I/O sur l'UI).
// ─────────────────────────────────────────────────────────────────────────────
class KrakenWorker : public QObject {
    Q_OBJECT
public:
    KrakenWorker(NZXTKrakenDevice* device, SystemSensors* sensors);
    ~KrakenWorker() override;

    // Thread-safe (mutex interne). Appelé depuis l'UI à chaque changement de config.
    void setConfig(const FrameConfig& cfg);

public slots:
    void start();                  // QueuedConnection depuis QThread::started
    void stop();                   // BlockingQueuedConnection depuis le destructeur du widget
    void setBrightness(int percent);

signals:
    void deviceOpened(const QString& name, bool ok);
    void deviceStatus(const QString& text);
    void previewReady(const QImage& frame);

private slots:
    void renderTick();
    void sensorTick();
    void applyCadence();           // (re)règle l'intervalle de rendu + init/arrêt WASAPI selon le mode

private:
    FrameConfig currentConfig();
    bool tryOpenDevice();          // open + brightness/rotation + signal + cadence

#ifdef _WIN32
    void initWasapi();
    void closeWasapi();
    QVector<float> readWasapiBands();
#endif

    NZXTKrakenDevice* m_device  = nullptr;   // emprunté
    SystemSensors*    m_sensors = nullptr;   // emprunté
    FrameRenderer     m_renderer;            // possédé (créé sur ce thread)
    MediaSession      m_media;               // "now playing" via SMTC (mode NOW_PLAYING)

    QTimer* m_renderTimer = nullptr;
    QTimer* m_sensorTimer = nullptr;

    QElapsedTimer m_previewClock;   // throttle apercu UI (~30 fps, decouple du device)

    // Reconnexion a chaud : compteur d'echecs sendFrame + throttle du sondage.
    // Backoff 3 s -> 30 s quand le device est present mais non ouvrable
    // (ex. NZXT CAM lance) : evite le spam de warnings d'open() rate.
    int           m_sendFailCount  = 0;
    int           m_reopenDelayMs  = 3000;
    QElapsedTimer m_reopenClock;

    QMutex      m_cfgMutex;
    FrameConfig m_cfg;

    bool m_comInit = false;

#ifdef _WIN32
    void* m_wasapiEnum    = nullptr;
    void* m_wasapiClient  = nullptr;
    void* m_wasapiCapture = nullptr;
    void* m_wasapiFormat  = nullptr;
    bool  m_wasapiReady   = false;
    int   m_wasapiCh      = 2;
    QVector<float> m_wasapiAcc;      // enveloppe par bande (etait un static de fonction)
    QElapsedTimer  m_wasapiRetryClock; // throttle des tentatives de re-init (~2 s)
#endif
};

} // namespace NZXTKraken
