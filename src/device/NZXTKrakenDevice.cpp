#include "NZXTKrakenDevice.h"
#include <hidapi/hidapi.h>
#include <libusb-1.0/libusb.h>
#include <cstring>
#include <algorithm>
#include <QDebug>
#include <QString>
#include <QThread>
#include <QMutexLocker>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <setupapi.h>
#  include <hidsdi.h>
#  pragma comment(lib, "hid.lib")
#  pragma comment(lib, "setupapi.lib")
#endif

namespace NZXTKraken {

// ────────────────────────────────────────────────────────────────────────────
NZXTKrakenDevice::NZXTKrakenDevice()
{
    hid_init();
    libusb_init(&m_usbCtx);
}

NZXTKrakenDevice::~NZXTKrakenDevice()
{
    close();
    if (m_usbCtx) { libusb_exit(m_usbCtx); m_usbCtx = nullptr; }
    // PAS de hid_exit() : l'état hidapi est global au process. Si le plugin
    // partage un jour la même instance hidapi qu'OpenRGB (lien dynamique
    // système, ex. Linux), hid_exit() au unload casserait tous les devices
    // HID d'OpenRGB. Le nettoyage se fait à la fin du process.
}

int NZXTKrakenDevice::hidReportLen() const
{
    if (m_info) return m_info->hidReportLen;
    return 64;
}

// ── Détection / ouverture ────────────────────────────────────────────────────
bool NZXTKrakenDevice::open()
{
    for (const auto& dev : SUPPORTED_DEVICES) {
        if (open(dev.vid, dev.pid)) return true;
    }
    return false;
}

bool NZXTKrakenDevice::open(uint16_t vid, uint16_t pid)
{
    close();
    m_abort.store(false, std::memory_order_relaxed);
    for (const auto& dev : SUPPORTED_DEVICES) {
        if (dev.vid == vid && dev.pid == pid) {
            m_info = &dev;
            break;
        }
    }
    if (!m_info) return false;

    // 1) Tenter HID classique (hidapi)
    if (!openHID(vid, pid)) {
        qWarning() << "[KrakenLCD/HID] interface unavailable for"
                   << QString::fromUtf8(m_info->name)
                   << "— driver was likely replaced by WinUSB (Zadig)."
                      " Falling back to full WinUSB transport.";
    }

#ifdef _WIN32
    // 2) WinHID direct (HidD_SetOutputReport) — contourne le bug d'hidapi 0.14
    openWinHID(vid, pid);
#endif

    // 3) Bulk USB via libusb (interface 0 pour le canal LCD)
    if (!openBulk(vid, pid)) {
        qWarning() << "[KrakenLCD/USB] bulk USB unavailable"
                   << "— LCD screen requires WinUSB (WinUSB driver via Zadig) or libusb (Linux).";
    }

    // Évaluer la disponibilité
    bool hasBulk = (m_usbDev != nullptr);
    bool hasCmd  = (m_hid != nullptr);
#ifdef _WIN32
    hasCmd = hasCmd || (m_winHidHandle != INVALID_HANDLE_VALUE);
#endif

    m_lcdReady = hasBulk && hasCmd;

    if (m_lcdReady) {
        qInfo() << "[KrakenLCD/Device] opened:" << displayName()
                 << QStringLiteral("(%1x%2)").arg(m_info->lcdWidth).arg(m_info->lcdHeight)
                 << "— bulk: libusb  commands:"
                 << (m_hid ? "HID" : "WinHID")
                 << "  hidReportLen:" << hidReportLen();
    } else {
        qWarning() << "[KrakenLCD/Device]" << displayName()
                   << "detected but channel incomplete: bulk=" << hasBulk << "commands=" << hasCmd;
        // Fermer et libérer — ce device n'est pas exploitable
        close();
        // Restaurer m_info à nullptr pour que le prochain open() puisse essayer un autre PID
        return false;
    }
    return true;
}

void NZXTKrakenDevice::close()
{
    if (m_hid) { hid_close(m_hid); m_hid = nullptr; }
    if (m_usbDev) {
        libusb_release_interface(m_usbDev, 0);
        libusb_close(m_usbDev); m_usbDev = nullptr;
    }
#ifdef _WIN32
    closeWinHID();
#endif
    m_info = nullptr;
    m_lcdReady = false;
}

bool NZXTKrakenDevice::isOpen() const { return m_lcdReady || m_hid != nullptr; }

std::string NZXTKrakenDevice::deviceName() const
{
    return m_info ? m_info->name : "Not connected";
}

QString NZXTKrakenDevice::displayName() const
{
    return m_info ? QString::fromUtf8(m_info->name) : QStringLiteral("Not connected");
}

// ── HID classique (hidapi) ───────────────────────────────────────────────────
bool NZXTKrakenDevice::openHID(uint16_t vid, uint16_t pid)
{
    m_hid = hid_open(vid, pid, nullptr);
    if (!m_hid) return false;
    hid_set_nonblocking(m_hid, 1);
    qDebug() << "[KrakenLCD/HID] hid_open OK for"
             << QString::asprintf("VID_%04X&PID_%04X", vid, pid);
    return true;
}

// ─── Envoi de commande HID — adapté à la taille du rapport du device ────────
//
// Le Kraken Elite V2 (PID 0x3012/0x3013) utilise des rapports HID de 512 octets,
// les anciens modèles utilisent 64 octets. Le paquet envoyé doit faire exactement
// reportLen+1 octets (1 byte ReportID 0x00 + reportLen bytes de données).
//
bool NZXTKrakenDevice::sendHIDCommand(const uint8_t* data, size_t len)
{
    if (!data) return false;

    const int reportLen = hidReportLen();

#ifdef _WIN32
    // Plan A : WinHID direct (WriteFile overlapped / HidD_SetOutputReport)
    // C'est la voie la plus fiable sous Windows — elle contourne le bug
    // d'hidapi 0.14 sur WriteFile et utilise directement l'API Windows HID.
    if (m_winHidHandle != INVALID_HANDLE_VALUE) {
        if (winHidWrite(data, len))
            return true;
    }
#endif
    // (Ancien plan B — SET_REPORT via libusb sur wIndex=1 — supprimé :
    // l'interface 1 n'est jamais réclamée, le transfert échouait toujours.)

    // Plan B : hidapi (peut échouer avec le bug ABI 0.14)
    if (m_hid) {
        const int pktSize = reportLen + 1; // +1 pour le ReportID
        std::vector<uint8_t> pkt(pktSize, 0);
        pkt[0] = 0x00; // ReportID
        if (len > (size_t)reportLen) len = reportLen;
        std::memcpy(pkt.data() + 1, data, len);

        int res = hid_write(m_hid, pkt.data(), pktSize);
        if (res == pktSize) return true;

        const wchar_t* err = hid_error(m_hid);
        static int s_hidErr = 0;
        if (s_hidErr++ < 3) {
            qWarning() << "[KrakenLCD/HID] sendHIDCommand hid_write returned" << res
                       << "— cause:" << (err ? QString::fromWCharArray(err) : QStringLiteral("(null)"))
                       << "— cmd[0]="
                       << QString::asprintf("\"0x%02X\"", (unsigned)data[0]);
        }
    }

    return false;
}

bool NZXTKrakenDevice::readHIDResponse(uint8_t* buf, size_t maxLen, int timeoutMs)
{
    // Prioriser hidapi pour la lecture (hid_read_timeout attend un nouveau
    // rapport). Le repli WinHID a la même sémantique depuis le passage à
    // ReadFile overlapped (file de rapports + timeout réel).
    if (m_hid) {
        int r = hid_read_timeout(m_hid, buf, maxLen, timeoutMs);
        return r > 0;
    }
#ifdef _WIN32
    if (m_winHidHandle != INVALID_HANDLE_VALUE) {
        int bytesRead = 0;
        if (winHidRead(buf, maxLen, bytesRead, timeoutMs))
            return bytesRead > 0;
    }
#endif
    return false;
}

// ── Bulk USB — interface 0 ───────────────────────────────────────────────────
bool NZXTKrakenDevice::openBulk(uint16_t vid, uint16_t pid)
{
    // Chercher le device libusb
    m_usbDev = libusb_open_device_with_vid_pid(m_usbCtx, vid, pid);
    if (!m_usbDev) {
        qWarning() << "[KrakenLCD/USB] no libusb device for"
                   << QString::asprintf("VID_%04X&PID_%04X", vid, pid);
        return false;
    }

    // Interface 0 = Vendor Specific = bulk LCD (EP 0x02 OUT)
    // Interface 1 = HID = commandes/capteurs
    if (libusb_kernel_driver_active(m_usbDev, 0) == 1)
        libusb_detach_kernel_driver(m_usbDev, 0);

    int r = libusb_claim_interface(m_usbDev, 0);
    if (r < 0) {
        qWarning() << "[KrakenLCD/USB] claim_interface(0) failed:" << libusb_error_name(r);
        libusb_close(m_usbDev); m_usbDev = nullptr;
        return false;
    }
    qDebug() << "[KrakenLCD/USB] interface 0 claimed (LCD bulk channel).";
    return true;
}

bool NZXTKrakenDevice::sendBulkData(const uint8_t* data, size_t len)
{
    if (!m_usbDev) return false;
    size_t offset = 0;
    int    stalls = 0;
    while (offset < len) {
        if (m_abort.load(std::memory_order_relaxed)) return false;
        size_t chunkLen = std::min(BULK_CHUNK_SIZE, len - offset);
        int transferred = 0;
        int r = libusb_bulk_transfer(m_usbDev,
                                     BULK_ENDPOINT,
                                     const_cast<uint8_t*>(data + offset),
                                     static_cast<int>(chunkLen),
                                     &transferred,
                                     BULK_TIMEOUT_MS);
        if (transferred > 0) {
            // Progrès réel : on avance du nombre d'octets effectivement transmis
            // (même si r<0 a renvoyé un transfert partiel) — évite tout renvoi en
            // double, qui décalerait les données côté device.
            offset += static_cast<size_t>(transferred);
            stalls = 0;
            continue;
        }
        // Aucun octet transmis : stall/erreur. On débloque l'endpoint et on réessaie
        // quelques fois avant d'abandonner la frame.
        if (r == LIBUSB_ERROR_PIPE && m_usbDev)
            libusb_clear_halt(m_usbDev, BULK_ENDPOINT);
        if (++stalls >= 3) {
            static int s_err = 0;
            if (s_err++ < 5)
                qWarning() << "[KrakenLCD/USB] bulk transfer error:" << libusb_error_name(r)
                           << "offset=" << (int)offset << "chunk=" << (int)chunkLen;
            return false;
        }
    }
    return true;
}

// ── Envoi de frame LCD ───────────────────────────────────────────────────────
//
// Protocole Kraken Z/Elite V2 :
//   1. HID cmd 0x36 0x01 ... → prépare le device (taille = hidReportLen)
//   2. Bulk EP 0x02 : header 20 octets + payload JPEG
//   3. HID cmd 0x36 0x02 → finalise
//
bool NZXTKrakenDevice::sendFrame(const QByteArray& imageData, uint8_t streamKindOverride)
{
    if (!m_lcdReady) return false;

    // ── Mutex : un seul sendFrame à la fois ──────────────────────────────────
    QMutexLocker lock(&m_sendMutex);

    // ── Anti-flood : espacement mini = période de l'écran du modèle ──────────
    // 60 Hz (Elite) → ~16 ms, 30 Hz (Kraken/Z) → ~33 ms. Évite de saturer le device
    // tout en autorisant le plein régime des écrans 60 Hz.
    const int minMs = (m_info && m_info->maxFps > 0) ? (1000 / m_info->maxFps) : 33;
    if (m_lastFrameTimeValid) {
        qint64 elapsed = m_lastFrameTime.elapsed();
        if (elapsed < minMs) {
            QThread::msleep(static_cast<unsigned long>(minMs - elapsed));
        }
    }

    const uint32_t sz = static_cast<uint32_t>(imageData.size());

    const uint8_t sk = streamKindOverride ? streamKindOverride
                     : (m_info ? m_info->streamKind : 0x06);

    static int s_frameIdx = 0;
    const int idx = s_frameIdx++;
    const bool log = (idx < 5);

    if (log) qDebug() << "[KrakenLCD/Frame] sendFrame #" << idx
                      << "streamKind=" << (int)sk
                      << "payloadSize=" << sz;

    // ── Vider les messages HID en attente (purge) ────────────────────────
    // Comme le driver Python fait un "clear" avant chaque writeFrame.
    // Plafond d'itérations : garde-fou contre un flux de rapports continu
    // (sans lui, un chemin de lecture qui "réussit toujours" bouclerait à l'infini).
    {
        uint8_t discard[64];
        for (int i = 0; i < 64 && readHIDResponse(discard, sizeof(discard), 1); ++i) {}
    }

    // 1) Commande start HID : [0x36, 0x01, 0x00, 0x01, streamKind]
    // Le driver Python envoie exactement ça pour le Q565 Elite V2.
    {
        uint8_t cmd[5] = { 0x36, 0x01, 0x00, 0x01, sk };
        if (!sendHIDCommand(cmd, sizeof(cmd))) {
            if (log) qWarning() << "[KrakenLCD/Frame] frame #" << idx << "start command failed.";
            return false;
        }
    }

    // 2) Attendre la réponse 0x37 0x01 (start ack) du device
    //    C'est CRITIQUE — sans cette attente, le device n'est pas prêt
    //    et rejette le bulk data, causant le clignotement.
    {
        uint8_t resp[64] = {0};   // resp[14] lu même si le rapport est court
        bool gotAck = false;
        for (int retry = 0; retry < 50 && !m_abort.load(std::memory_order_relaxed); retry++) {
            if (readHIDResponse(resp, sizeof(resp), 100)) {
                if (resp[0] == 0x37 && resp[1] == 0x01) {
                    bool success = (resp[14] == 1);
                    if (log) qDebug() << "[KrakenLCD/Frame] frame #" << idx
                                      << "start ack received, status=" << success;
                    if (!success) return false;
                    gotAck = true;
                    break;
                }
                // Message inattendu — continuer à lire
            }
        }
        if (!gotAck) {
            if (log) qWarning() << "[KrakenLCD/Frame] frame #" << idx
                                << "no start ack received (timeout).";
            return false;
        }
    }

    // 3) Header bulk (20 octets)
    uint8_t hdr[20] = {
        0x12, 0xFA, 0x01, 0xE8,
        0xAB, 0xCD, 0xEF, 0x98,
        0x76, 0x54, 0x32, 0x10,
        sk,   0x00, 0x00, 0x00,
        static_cast<uint8_t>((sz >>  0) & 0xFF),
        static_cast<uint8_t>((sz >>  8) & 0xFF),
        static_cast<uint8_t>((sz >> 16) & 0xFF),
        static_cast<uint8_t>((sz >> 24) & 0xFF),
    };

    // Récupération si un transfert échoue en cours. Le device a été averti (header)
    // qu'il recevrait `sz` octets ; s'il n'en reçoit qu'une partie, il reste en
    // attente du reste et la frame SUIVANTE vient s'y coller → décalage → bandes
    // horizontales persistantes. On le resynchronise : end (vide son buffer de
    // frame), clear-halt d'un éventuel blocage du endpoint, et on invalide
    // l'anti-flood pour que la frame suivante reparte sur une base propre.
    auto resyncDevice = [&]() {
        uint8_t endCmd[3] = { 0x36, 0x02, 0x00 };
        sendHIDCommand(endCmd, sizeof(endCmd));
        if (m_usbDev) libusb_clear_halt(m_usbDev, BULK_ENDPOINT);
        m_lastFrameTimeValid = false;
        static int s_recov = 0;
        if (s_recov++ < 10)
            qWarning() << "[KrakenLCD/Frame] frame #" << idx
                       << "bulk transfer interrupted — resynchronizing device.";
    };

    if (!sendBulkData(hdr, sizeof(hdr))) {
        resyncDevice();
        return false;
    }

    // 4) Payload image
    if (!sendBulkData(reinterpret_cast<const uint8_t*>(imageData.constData()), sz)) {
        resyncDevice();
        return false;
    }

    // 5) Commande end HID : [0x36, 0x02]
    //    Le driver Python envoie TOUJOURS cette commande, même pour le V2 !
    {
        uint8_t cmd[2] = { 0x36, 0x02 };
        sendHIDCommand(cmd, sizeof(cmd));
    }

    // 6) Attendre la réponse 0x37 0x02 (end ack)
    {
        uint8_t resp[64] = {0};
        for (int retry = 0; retry < 50 && !m_abort.load(std::memory_order_relaxed); retry++) {
            if (readHIDResponse(resp, sizeof(resp), 100)) {
                if (resp[0] == 0x37 && resp[1] == 0x02) {
                    if (log) qDebug() << "[KrakenLCD/Frame] frame #" << idx
                                      << "end ack received, status=" << (int)resp[14];
                    break;
                }
            }
        }
    }

    // Enregistrer le timestamp pour l'anti-flood
    m_lastFrameTime.start();
    m_lastFrameTimeValid = true;

    if (log) qDebug() << "[KrakenLCD/Frame] frame #" << idx << "sent successfully.";
    return true;
}

// ── Lecture capteurs ─────────────────────────────────────────────────────────
bool NZXTKrakenDevice::readSensors(DeviceReadings& out)
{
    QMutexLocker lock(&m_sendMutex);   // sérialise avec sendFrame : pas de HID en plein transfert bulk
    const uint8_t req[2] = { 0x74, 0x01 };
    if (!sendHIDCommand(req, sizeof(req)))
        return false;

    const int maxBuf = hidReportLen() + 1;
    std::vector<uint8_t> buf(maxBuf, 0);
    if (!readHIDResponse(buf.data(), buf.size(), 300))
        return false;

    // Chercher le marqueur 0x75 0x01
    int off = 0;
    if (buf[0] == 0x00 && buf[1] == 0x75 && buf[2] == 0x01)
        off = 1; // skip ReportID
    const uint8_t* p = buf.data() + off;

    if (p[0] != 0x75 || p[1] != 0x01)
        return false;

    // Offsets identiques au plugin SignalRGB pour le Elite V2
    out.liquidTemp = static_cast<float>(p[15]) + static_cast<float>(p[16]) / 10.0f;
    out.pumpRPM    = (static_cast<int>(p[18]) << 8) | p[17];
    out.fanRPM     = (static_cast<int>(p[24]) << 8) | p[23];
    return true;
}

// ── Luminosité ───────────────────────────────────────────────────────────────
// Protocole liquidctl (kraken3.py, KrakenZ3.set_screen) :
//   0x30 0x02 0x01 <brightness 0..100> 0x00 0x00 0x01 <orientation 0..3>
// L'ancien 0x36 0x04 n'existe pas dans le firmware (commande ignorée).
// Orientation toujours 0 : la rotation est appliquée au rendu (QPainter),
// sinon elle serait appliquée deux fois.
bool NZXTKrakenDevice::setBrightness(int percent)
{
    QMutexLocker lock(&m_sendMutex);   // sérialise avec sendFrame
    uint8_t val = static_cast<uint8_t>(std::clamp(percent, 0, 100));
    uint8_t cmd[8] = { 0x30, 0x02, 0x01, val, 0x00, 0x00, 0x01, 0x00 };
    return sendHIDCommand(cmd, sizeof(cmd));
}

// ── Sondage de présence (reconnexion à chaud) ────────────────────────────────
bool NZXTKrakenDevice::anySupportedPresent()
{
    hid_device_info* list = hid_enumerate(0x1E71, 0);
    bool found = false;
    for (hid_device_info* p = list; p && !found; p = p->next)
        for (const auto& dev : SUPPORTED_DEVICES)
            if (p->product_id == dev.pid) { found = true; break; }
    hid_free_enumeration(list);
    return found;
}

// =============================================================================
// Windows : WinHID direct (HidD_SetOutputReport / HidD_GetInputReport)
// Contourne le bug d'hidapi 0.14.0 sur WriteFile ERROR_INVALID_PARAMETER
// =============================================================================
#ifdef _WIN32

bool NZXTKrakenDevice::openWinHID(uint16_t vid, uint16_t pid)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA ifData;
    ifData.cbSize = sizeof(ifData);
    int candidates = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); i++) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &reqSize, nullptr);
        std::vector<BYTE> detailBuf(reqSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, reqSize, nullptr, nullptr))
            continue;

        // Vérifier VID/PID dans le chemin
        std::wstring path(detail->DevicePath);
        wchar_t vidStr[16], pidStr[16];
        swprintf(vidStr, 16, L"vid_%04x", vid);
        swprintf(pidStr, 16, L"pid_%04x", pid);
        if (path.find(vidStr) == std::wstring::npos || path.find(pidStr) == std::wstring::npos)
            continue;

        candidates++;

        // FILE_FLAG_OVERLAPPED : requis pour ReadFile avec timeout (winHidRead)
        HANDLE h = CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs;
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(h, &attrs) || attrs.VendorID != vid || attrs.ProductID != pid) {
            CloseHandle(h);
            continue;
        }

        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps;
        if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); continue; }
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
            HidD_FreePreparsedData(pp); CloseHandle(h); continue;
        }
        HidD_FreePreparsedData(pp);

        m_winHidOutputLen = caps.OutputReportByteLength;
        m_winHidInputLen  = caps.InputReportByteLength;
        m_winHidHandle    = h;
        m_winHidReadEvt   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_winHidWriteEvt  = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        // Lire les Value Caps pour découvrir les ReportIDs
        {
            USHORT numOutputCaps = caps.NumberOutputValueCaps;
            USHORT numInputCaps  = caps.NumberInputValueCaps;
            USHORT numOutputBtnCaps = caps.NumberOutputButtonCaps;
            qDebug() << "[KrakenLCD/HID] Caps: OutputValueCaps=" << numOutputCaps
                     << "InputValueCaps=" << numInputCaps
                     << "OutputButtonCaps=" << numOutputBtnCaps
                     << "Usage=" << QString::asprintf("0x%04X", caps.Usage)
                     << "UsagePage=" << QString::asprintf("0x%04X", caps.UsagePage)
                     << "OutputReportByteLength=" << caps.OutputReportByteLength
                     << "InputReportByteLength=" << caps.InputReportByteLength
                     << "FeatureReportByteLength=" << caps.FeatureReportByteLength;
        }

        qDebug() << "[KrakenLCD/HID] HID device opened:"
                 << "OutputReportByteLength=" << m_winHidOutputLen
                 << "InputReportByteLength=" << m_winHidInputLen;

        SetupDiDestroyDeviceInfoList(devInfo);
        return true;
    }

    qWarning() << "[KrakenLCD/HID] No usable HID interface for VID/PID"
               << "(" << candidates << "candidate(s) found).";
    SetupDiDestroyDeviceInfoList(devInfo);
    return false;
}

void NZXTKrakenDevice::closeWinHID()
{
    if (m_winHidHandle != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_winHidHandle, nullptr);
        CloseHandle(m_winHidHandle);
        m_winHidHandle = INVALID_HANDLE_VALUE;
    }
    if (m_winHidReadEvt)  { CloseHandle(m_winHidReadEvt);  m_winHidReadEvt  = nullptr; }
    if (m_winHidWriteEvt) { CloseHandle(m_winHidWriteEvt); m_winHidWriteEvt = nullptr; }
    m_winHidOutputLen = 0;
    m_winHidInputLen  = 0;
}

bool NZXTKrakenDevice::winHidWrite(const uint8_t* data, size_t len)
{
    if (m_winHidHandle == INVALID_HANDLE_VALUE || !data || m_winHidOutputLen <= 0)
        return false;

    // Le Kraken Elite V2 utilise des rapports HID de 512 octets.
    // SignalRGB envoie device.write([0x36, ...], 512) — c'est un WriteFile
    // de 512 octets où data[0] est la commande (pas un ReportID).
    //
    // Sur ce device, les rapports n'ont PAS de ReportID numéroté
    // (UsagePage=0xFF00, Vendor Defined). Le buffer WriteFile doit faire
    // exactement OutputReportByteLength octets.

    // === Stratégie 1: WriteFile direct (comme SignalRGB) ===
    // Pas de ReportID préfixé — les données commencent directement par la commande.
    // Handle ouvert en OVERLAPPED : le write doit l'être aussi.
    {
        std::vector<BYTE> buf(m_winHidOutputLen, 0);
        const size_t toCopy = std::min(len, buf.size());
        std::memcpy(buf.data(), data, toCopy);  // data[0] = 0x36 etc.

        OVERLAPPED ov{};
        ov.hEvent = m_winHidWriteEvt;
        ResetEvent(m_winHidWriteEvt);
        DWORD written = 0;
        BOOL ok = WriteFile(m_winHidHandle, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(m_winHidWriteEvt, 1000) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(m_winHidHandle, &ov, &written, FALSE);
            } else {
                CancelIoEx(m_winHidHandle, &ov);
                GetOverlappedResult(m_winHidHandle, &ov, &written, TRUE);
                ok = FALSE;
            }
        } else if (ok) {
            GetOverlappedResult(m_winHidHandle, &ov, &written, TRUE);
        }
        if (ok) {
            static int s_okCount = 0;
            if (s_okCount++ < 3)
                qDebug() << "[KrakenLCD/HID] WriteFile OK" << written << "bytes, cmd[0]="
                         << QString::asprintf("0x%02X", (unsigned)data[0]);
            return true;
        }
        DWORD err1 = GetLastError();
        static int s_wfErr = 0;
        if (s_wfErr++ < 3)
            qWarning() << "[KrakenLCD/HID] WriteFile failed, GetLastError=" << err1
                       << "size=" << m_winHidOutputLen;
    }

    // === Stratégie 2: HidD_SetOutputReport avec ReportID=0 préfixé ===
    {
        std::vector<BYTE> buf(m_winHidOutputLen, 0);
        const size_t toCopy = std::min(len, buf.size() - 1);
        std::memcpy(buf.data() + 1, data, toCopy);
        if (HidD_SetOutputReport(m_winHidHandle, buf.data(), static_cast<ULONG>(buf.size())))
            return true;
    }

    // === Stratégie 3: HidD_SetOutputReport avec data[0] comme ReportID ===
    {
        std::vector<BYTE> buf(m_winHidOutputLen, 0);
        if (len <= buf.size()) {
            std::memcpy(buf.data(), data, len);
            if (HidD_SetOutputReport(m_winHidHandle, buf.data(), static_cast<ULONG>(buf.size())))
                return true;
        }
    }

    const DWORD err = GetLastError();
    static int s_errCount = 0;
    if (s_errCount++ < 3) {
        qWarning() << "[KrakenLCD/HID] All strategies failed, last GetLastError=" << err
                   << "cmd[0]=" << QString::asprintf("0x%02X", (unsigned)data[0]);
    }
    return false;
}

// Lecture d'un NOUVEAU rapport d'entrée via ReadFile overlapped + timeout.
// ReadFile consomme la file de rapports du driver HID (comme hidapi) : pas de
// re-lecture d'un ack périmé, et un timeout réel quand la file est vide —
// contrairement à HidD_GetInputReport (poll synchrone du dernier rapport),
// qui produisait de faux acks et des purges infinies.
bool NZXTKrakenDevice::winHidRead(uint8_t* data, size_t maxLen, int& bytesRead, int timeoutMs)
{
    bytesRead = 0;
    if (m_winHidHandle == INVALID_HANDLE_VALUE || !data || m_winHidInputLen <= 0)
        return false;

    std::vector<BYTE> buf(m_winHidInputLen, 0);

    OVERLAPPED ov{};
    ov.hEvent = m_winHidReadEvt;
    ResetEvent(m_winHidReadEvt);
    DWORD read = 0;
    BOOL ok = ReadFile(m_winHidHandle, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING)
            return false;
        if (WaitForSingleObject(m_winHidReadEvt, timeoutMs > 0 ? DWORD(timeoutMs) : 0) != WAIT_OBJECT_0) {
            CancelIoEx(m_winHidHandle, &ov);
            GetOverlappedResult(m_winHidHandle, &ov, &read, TRUE);   // attend la fin de l'annulation
            return false;
        }
    }
    if (!GetOverlappedResult(m_winHidHandle, &ov, &read, TRUE) || read == 0)
        return false;

    // Skip le ReportID (byte 0)
    const size_t toCopy = std::min<size_t>(read > 0 ? size_t(read) - 1 : 0, maxLen);
    std::memcpy(data, buf.data() + 1, toCopy);
    bytesRead = static_cast<int>(toCopy);
    return true;
}

#endif // _WIN32

} // namespace NZXTKraken
