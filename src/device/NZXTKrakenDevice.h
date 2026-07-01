#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <QByteArray>
#include <QMutex>
#include <QElapsedTimer>

// hidapi forward
struct hid_device_;
typedef hid_device_ hid_device;

// libusb forward
struct libusb_context;
struct libusb_device_handle;

#ifdef _WIN32
#  include <windows.h>
#endif

namespace NZXTKraken {

// ─── Périphérique connu ──────────────────────────────────────────────────────
struct DeviceInfo {
    uint16_t    vid;
    uint16_t    pid;
    const char* name;
    int         lcdWidth;
    int         lcdHeight;
    int         hidReportLen;  // Taille du rapport HID (64 ou 512)
    uint8_t     streamKind;    // 0x06 = JPEG 480, 0x08 = JPEG Q565 640
    int         maxFps;        // Hz max de l'écran LCD (cadence de rendu cible)
};

// Table de tous les Krakens avec LCD (inline C++17 : une seule instance,
// pas une copie par TU comme avec `static`)
inline const std::vector<DeviceInfo> SUPPORTED_DEVICES = {
    //                                                  W    H    HID  stream  fps
    { 0x1E71, 0x3008, "NZXT Kraken Z73",          480, 480,  64, 0x06,  30 },
    { 0x1E71, 0x3009, "NZXT Kraken Z53",          480, 480,  64, 0x06,  30 },
    { 0x1E71, 0x300A, "NZXT Kraken Z63",          480, 480,  64, 0x06,  30 },
    { 0x1E71, 0x300C, "NZXT Kraken Elite 360",    480, 480,  64, 0x06,  60 },
    { 0x1E71, 0x300E, "NZXT Kraken 2023",         480, 480,  64, 0x06,  30 },
    { 0x1E71, 0x3012, "NZXT Kraken Elite V2",     640, 640, 512, 0x08,  60 },
    { 0x1E71, 0x3013, "NZXT Kraken Elite V2 B",   640, 640, 512, 0x08,  60 },
};

// ─── Lectures capteurs lues depuis HID ──────────────────────────────────────
struct DeviceReadings {
    float liquidTemp    = 0.f; // °C
    int   pumpRPM       = 0;
    int   fanRPM        = 0;
    int   firmwareMajor = 0;
    int   firmwareMinor = 0;
};

// ─── Classe de communication ─────────────────────────────────────────────────
class NZXTKrakenDevice {
public:
    NZXTKrakenDevice();
    ~NZXTKrakenDevice();

    // Détecte et ouvre le premier Kraken compatible trouvé
    bool open();
    // Ouvre un modèle spécifique par VID/PID
    bool open(uint16_t vid, uint16_t pid);
    void close();
    bool isOpen() const;

    const DeviceInfo* info() const { return m_info; }
    std::string       deviceName() const;
    QString           displayName() const;

    // Envoie une frame vers l'écran LCD
    // Le format dépend du modèle : RGB565 brut pour Elite V2, JPEG pour les anciens
    bool sendFrame(const QByteArray& imageData, uint8_t streamKindOverride = 0);

    // Lit la température du liquide et les RPM via HID
    bool readSensors(DeviceReadings& out);

    // Rotation de l'écran (0, 90, 180, 270)
    bool setRotation(int degrees);

    // Luminosité (0–100)
    bool setBrightness(int percent);

    // Interrompt les boucles d'attente de sendFrame (acks, bulk) depuis un
    // autre thread. Utilisé au unload pour ne pas geler l'UI d'OpenRGB
    // pendant les retries/timeouts si le device décroche.
    void requestAbort() { m_abort.store(true, std::memory_order_relaxed); }

    // Sondage léger (hid_enumerate) : un Kraken supporté est-il branché ?
    // Sans ouvrir le device ni générer les warnings d'un open() complet —
    // utilisé par la reconnexion à chaud.
    static bool anySupportedPresent();

private:
    // HID (commandes + lecture capteurs)
    bool openHID(uint16_t vid, uint16_t pid);
    bool sendHIDCommand(const uint8_t* data, size_t len);
    bool readHIDResponse(uint8_t* buf, size_t maxLen, int timeoutMs = 300);

    // Bulk USB (envoi frames LCD)
    bool openBulk(uint16_t vid, uint16_t pid);
    bool sendBulkData(const uint8_t* data, size_t len);

    // Taille dynamique du rapport HID
    int hidReportLen() const;

#ifdef _WIN32
    // WinHID direct — contourne le bug d'hidapi 0.14.0.
    // Handle ouvert en OVERLAPPED : la lecture passe par ReadFile (file de
    // rapports du driver HID = sémantique "prochain rapport" avec timeout),
    // PAS par HidD_GetInputReport qui ne fait que poller le dernier rapport
    // (acks périmés + boucles infinies de purge).
    bool openWinHID(uint16_t vid, uint16_t pid);
    void closeWinHID();
    bool winHidWrite(const uint8_t* data, size_t len);
    bool winHidRead(uint8_t* data, size_t maxLen, int& bytesRead, int timeoutMs);
    HANDLE  m_winHidHandle   = INVALID_HANDLE_VALUE;
    HANDLE  m_winHidReadEvt  = nullptr;
    HANDLE  m_winHidWriteEvt = nullptr;
    int     m_winHidOutputLen = 0;
    int     m_winHidInputLen  = 0;
#endif

    hid_device*            m_hid    = nullptr;
    libusb_context*        m_usbCtx = nullptr;
    libusb_device_handle*  m_usbDev = nullptr;
    const DeviceInfo*      m_info   = nullptr;
    bool                   m_lcdReady = false;
    mutable QMutex         m_sendMutex;          // protège sendFrame contre les appels concurrents
    QElapsedTimer          m_lastFrameTime;       // anti-flood inter-frame
    bool                   m_lastFrameTimeValid = false;
    std::atomic<bool>      m_abort{false};        // interruption des attentes (unload)

    static constexpr size_t BULK_CHUNK_SIZE  = 16384; // 16 KB chunks pour transferts rapides
    static constexpr uint8_t BULK_ENDPOINT   = 0x02;
    static constexpr int    BULK_TIMEOUT_MS  = 5000;  // 5 sec pour les gros payloads RGB565
};

} // namespace NZXTKraken
