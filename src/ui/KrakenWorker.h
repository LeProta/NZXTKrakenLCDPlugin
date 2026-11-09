#pragma once
#include <QObject>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <QElapsedTimer>
#include "../renderer/FrameRenderer.h"
#include "../sensors/MediaSession.h"

class QTimer;

namespace NZXTKraken {

class NZXTKrakenDevice;

class KrakenWorker : public QObject {
    Q_OBJECT
public:
    KrakenWorker(NZXTKrakenDevice* device, SystemSensors* sensors);
    ~KrakenWorker() override;

    void setConfig(const FrameConfig& cfg);

public slots:
    void start();
    void stop();
    void setBrightness(int percent);

signals:
    void deviceOpened(const QString& name, bool ok);
    void deviceStatus(const QString& text);
    void previewReady(const QImage& frame);

private slots:
    void renderTick();
    void sensorTick();
    void applyCadence();

private:
    FrameConfig currentConfig();
    bool tryOpenDevice();

#ifdef _WIN32
    void initWasapi();
    void closeWasapi();
    QVector<float> readWasapiBands();
#endif

    NZXTKrakenDevice* m_device  = nullptr;
    SystemSensors*    m_sensors = nullptr;
    FrameRenderer     m_renderer;
    MediaSession      m_media;               // "now playing" via SMTC (mode NOW_PLAYING)

    QTimer* m_renderTimer = nullptr;
    QTimer* m_sensorTimer = nullptr;

    QElapsedTimer m_previewClock;

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
    QVector<float> m_wasapiAcc;
    QElapsedTimer  m_wasapiRetryClock;
#endif
};

} // namespace NZXTKraken
