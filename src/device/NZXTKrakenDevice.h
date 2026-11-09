#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <QByteArray>
#include <QMutex>
#include <QElapsedTimer>

struct hid_device_;
typedef hid_device_ hid_device;

struct libusb_context;
struct libusb_device_handle;

#ifdef _WIN32
#  include <windows.h>
#endif

namespace NZXTKraken {

struct DeviceInfo {
    uint16_t    vid;
    uint16_t    pid;
    const char* name;
    int         lcdWidth;
    int         lcdHeight;
    int         hidReportLen;
    uint8_t     streamKind;
    int         maxFps;
};

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

struct DeviceReadings {
    float liquidTemp    = 0.f; // °C
    int   pumpRPM       = 0;
    int   fanRPM        = 0;
    int   firmwareMajor = 0;
    int   firmwareMinor = 0;
};

class NZXTKrakenDevice {
public:
    NZXTKrakenDevice();
    ~NZXTKrakenDevice();

    bool open();
    bool open(uint16_t vid, uint16_t pid);
    void close();
    bool isOpen() const;

    const DeviceInfo* info() const { return m_info; }
    std::string       deviceName() const;
    QString           displayName() const;

    bool sendFrame(const QByteArray& imageData, uint8_t streamKindOverride = 0);

    bool readSensors(DeviceReadings& out);

    bool setBrightness(int percent);

    void requestAbort() { m_abort.store(true, std::memory_order_relaxed); }

    static bool anySupportedPresent();

private:
    bool openHID(uint16_t vid, uint16_t pid);
    bool sendHIDCommand(const uint8_t* data, size_t len);
    bool readHIDResponse(uint8_t* buf, size_t maxLen, int timeoutMs = 300);

    bool openBulk(uint16_t vid, uint16_t pid);
    bool sendBulkData(const uint8_t* data, size_t len);

    int hidReportLen() const;

#ifdef _WIN32
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
    mutable QMutex         m_sendMutex;
    QElapsedTimer          m_lastFrameTime;
    bool                   m_lastFrameTimeValid = false;
    std::atomic<bool>      m_abort{false};

    static constexpr size_t BULK_CHUNK_SIZE  = 16384;
    static constexpr uint8_t BULK_ENDPOINT   = 0x02;
    static constexpr int    BULK_TIMEOUT_MS  = 5000;
};

} // namespace NZXTKraken
