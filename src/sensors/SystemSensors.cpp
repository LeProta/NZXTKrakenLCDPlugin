// =============================================================================
//  SystemSensors.cpp — Acquisition des capteurs systeme (Windows uniquement)
//
//  LibreHardwareMonitor embarque via lhwm-cpp-wrapper (lib statique) +
//  lhwm-wrapper.dll (assembly .NET, a placer a cote d'OpenRGB.exe). Aucun
//  OHM/LHM externe. Mapping AUTOMATIQUE par prefixe d'identifiant + type +
//  preference de nom (cf. buildSensorMap()). Liquide : fourni par le Kraken
//  via HID (setLiquidTemp).
//
//  Regle absolue : aucun crash. Toute erreur -> SensorValue.available = false.
// =============================================================================
#include "SystemSensors.h"
#include <QMutexLocker>
#include <QTimer>
#include <QDebug>
#include <QString>
#include <algorithm>
#include <cmath>
#include <vector>
#include <tuple>
#include <initializer_list>
#include <lhwm-cpp-wrapper.h>
#ifdef _WIN32
#include <windows.h>
#endif

// =============================================================================
namespace {
// =============================================================================

// --- Categorie materielle d'apres le prefixe d'identifiant LHM ---------------
//   /amdcpu/...  /intelcpu/...                     -> CPU
//   /gpu-amd/...  /gpu-nvidia/...  /gpu-intel/...   -> GPU
//   /ram/...                                        -> RAM
static bool idIsCpu(const std::string& id) {
    return id.rfind("/amdcpu/", 0) == 0 || id.rfind("/intelcpu/", 0) == 0;
}
static bool idIsGpu(const std::string& id) {
    return id.rfind("/gpu-amd/", 0) == 0
        || id.rfind("/gpu-nvidia/", 0) == 0
        || id.rfind("/gpu-intel/", 0) == 0
        || id.rfind("/gpu/", 0) == 0;   // securite (anciennes nomenclatures)
}
static bool idIsRam(const std::string& id) {
    return id.rfind("/ram/", 0) == 0;
}

} // anonymous namespace

// =============================================================================
namespace NZXTKraken {
// =============================================================================

SystemSensors::SystemSensors()
{
    for (auto t : { SensorType::CPU_TEMP, SensorType::CPU_LOAD, SensorType::CPU_CLOCK,
                    SensorType::GPU_TEMP, SensorType::GPU_LOAD, SensorType::GPU_CLOCK,
                    SensorType::MEM_LOAD, SensorType::LIQUID_TEMP })
        m_values[t] = SensorValue{};
}

SystemSensors::~SystemSensors()
{
    shutdown();
}

void SystemSensors::startPolling()
{
    if (m_timer) return;
    buildSensorMap();
    m_timer = new QTimer;
    m_timer->setInterval(1000);
    QObject::connect(m_timer, &QTimer::timeout, [this]{ pollOnce(); });
    m_timer->start();
    pollOnce();
}

void SystemSensors::stopPolling()
{
    if (m_timer) { m_timer->stop(); delete m_timer; m_timer = nullptr; }
}

void SystemSensors::shutdown()
{
    stopPolling();
    // Rien a liberer : l'assembly .NET (lhwm-wrapper.dll) gere son cycle de vie.
}

SensorValue SystemSensors::read(SensorType type) const
{
    QMutexLocker lk(&m_mutex);
    return m_values.value(type, SensorValue{});
}

void SystemSensors::setLiquidTemp(float celsius)
{
    QMutexLocker lk(&m_mutex);
    bool ok = (celsius > 0.f && celsius < 100.f);
    m_values[SensorType::LIQUID_TEMP] = { celsius, "°C", ok };
}

// --- Resolution des identifiants LHM (executee une seule fois) ---------------
void SystemSensors::buildSensorMap()
{
    if (m_mapBuilt) return;
    m_mapBuilt = true;

#ifdef _WIN32
    // LHM (AmdGpu/ADL) charge le UMD D3D du pilote puis le decharge ; le poll
    // suivant rappelle dedans -> 0xC0000005 dans "<UMD>.DLL_unloaded". On
    // epingle les modules deja charges pour qu'ils ne soient jamais liberes.
    // ponytail: liste fixe des UMD connus, ajouter le nom du pilote si un autre
    // vendeur presente le meme crash.
    auto pinLoaded = [](const wchar_t* name) {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &h);
    };
#endif

    struct Entry { std::string name, type, id; };
    std::vector<Entry> all;

    try {
        // [nomMateriel, [(nomCapteur, typeCapteur, identifiant), ...]]
        auto hsMap = LHWM::GetHardwareSensorMap();
#ifdef _WIN32
        for (const wchar_t* m : { L"amdxn64.dll", L"amdxx64.dll", L"atiadlxx.dll",
                                  L"nvoglv64.dll", L"d3d9.dll" })
            pinLoaded(m);
#endif
        for (const auto& hw : hsMap) {
            for (const auto& s : hw.second) {
                all.push_back({ std::get<0>(s), std::get<1>(s), std::get<2>(s) });
                // Dump diagnostic (une fois, niveau Info) — sert a valider/affiner.
                qInfo().noquote()
                    << "[KrakenLCD/Sensors] LHM" << QString::fromStdString(hw.first)
                    << "|"  << QString::fromStdString(std::get<1>(s))
                    << "|"  << QString::fromStdString(std::get<0>(s))
                    << "->" << QString::fromStdString(std::get<2>(s));
            }
        }
    } catch (...) {
        qWarning() << "[KrakenLCD/Sensors] LHWM::GetHardwareSensorMap threw an exception "
                      "(lhwm-wrapper.dll present but .NET/driver failing?).";
        return;
    }

    if (all.empty()) {
        qWarning() << "[KrakenLCD/Sensors] LHWM: empty sensor map. DLL present but no sensors "
                      "— Memory Integrity (HVCI) blocking the driver, or OpenRGB "
                      "not running as administrator?";
        return;
    }

    // Selecteur : parmi les entrees (categorie + type), prend la 1re dont le nom
    // correspond a une preference (egalite exacte puis prefixe), sinon la 1re ;
    // en excluant les noms contenant un motif d'exclusion.
    auto pick = [&](bool(*cat)(const std::string&), const char* type,
                    std::initializer_list<const char*> prefs,
                    std::initializer_list<const char*> excludes) -> std::string {
        std::vector<const Entry*> cand;
        for (const auto& e : all) {
            if (!cat(e.id)) continue;
            if (e.type != type) continue;
            bool ex = false;
            for (const char* x : excludes)
                if (e.name.find(x) != std::string::npos) { ex = true; break; }
            if (!ex) cand.push_back(&e);
        }
        if (cand.empty()) return std::string();
        for (const char* p : prefs)                  // egalite exacte
            for (const Entry* e : cand)
                if (e->name == p) return e->id;
        for (const char* p : prefs) {                 // sinon prefixe
            const std::string ps(p);
            for (const Entry* e : cand)
                if (e->name.rfind(ps, 0) == 0) return e->id;
        }
        return cand.front()->id;                       // defaut : 1re entree
    };

    m_ids[SensorType::CPU_TEMP]  = pick(idIsCpu, "Temperature",
        { "Core (Tctl/Tdie)", "Core (Tctl)", "CPU Package", "Package", "CPU Cores" }, {});
    m_ids[SensorType::CPU_LOAD]  = pick(idIsCpu, "Load",
        { "CPU Total", "Total" }, {});
    m_ids[SensorType::CPU_CLOCK] = pick(idIsCpu, "Clock",
        { "Core #1", "CPU Core #1", "Core" }, { "Bus" });
    m_ids[SensorType::GPU_TEMP]  = pick(idIsGpu, "Temperature",
        { "GPU Core", "GPU Hot Spot", "GPU Package", "GPU" }, {});
    m_ids[SensorType::GPU_LOAD]  = pick(idIsGpu, "Load",
        { "GPU Core", "D3D 3D", "GPU" }, { "Memory", "Mem" });
    m_ids[SensorType::GPU_CLOCK] = pick(idIsGpu, "Clock",
        { "GPU Core" }, { "Memory", "Mem", "Shader" });
    m_ids[SensorType::MEM_LOAD]  = pick(idIsRam, "Load",
        { "Memory", "Memory Used" }, { "Virtual" });

    auto logId = [&](const char* lbl, SensorType t) {
        const std::string id = m_ids.value(t, std::string());
        qInfo().noquote() << "[KrakenLCD/Sensors] resolved" << lbl << "->"
            << (id.empty() ? QStringLiteral("(none)") : QString::fromStdString(id));
    };
    logId("CPU temp ", SensorType::CPU_TEMP);
    logId("CPU load ", SensorType::CPU_LOAD);
    logId("CPU clock", SensorType::CPU_CLOCK);
    logId("GPU temp ", SensorType::GPU_TEMP);
    logId("GPU load ", SensorType::GPU_LOAD);
    logId("GPU clock", SensorType::GPU_CLOCK);
    logId("RAM load ", SensorType::MEM_LOAD);
}

void SystemSensors::pollOnce()
{
    auto readId = [&](SensorType t, const char* unit, bool clampPct) -> SensorValue {
        const std::string id = m_ids.value(t, std::string());
        if (id.empty()) return SensorValue{};
        float v;
        try { v = LHWM::GetSensorValue(id); }
        catch (...) { return SensorValue{}; }
        if (std::isnan(v)) return SensorValue{};
        if (clampPct) v = std::clamp(v, 0.f, 100.f);
        return SensorValue{ v, unit, true };
    };

    SensorValue cpuT = readId(SensorType::CPU_TEMP,  "°C",  false);
    if (cpuT.available && (cpuT.value <= 0.f || cpuT.value >= 150.f)) cpuT = SensorValue{};  // 0 = pas de donnee -> N/A
    SensorValue cpuL = readId(SensorType::CPU_LOAD,  "%",   true);
    SensorValue cpuC = readId(SensorType::CPU_CLOCK, "MHz", false);
    if (cpuC.available && cpuC.value <= 0.f) cpuC = SensorValue{};
    SensorValue gpuT = readId(SensorType::GPU_TEMP,  "°C",  false);
    if (gpuT.available && (gpuT.value <= 0.f || gpuT.value >= 150.f)) gpuT = SensorValue{};  // 0 = pas de donnee -> N/A
    SensorValue gpuL = readId(SensorType::GPU_LOAD,  "%",   true);
    SensorValue gpuC = readId(SensorType::GPU_CLOCK, "MHz", false);
    if (gpuC.available && gpuC.value <= 0.f) gpuC = SensorValue{};
    SensorValue memL = readId(SensorType::MEM_LOAD,  "%",   true);

    {
        QMutexLocker lk(&m_mutex);
        m_values[SensorType::CPU_TEMP]  = cpuT;
        m_values[SensorType::CPU_LOAD]  = cpuL;
        m_values[SensorType::CPU_CLOCK] = cpuC;
        m_values[SensorType::GPU_TEMP]  = gpuT;
        m_values[SensorType::GPU_LOAD]  = gpuL;
        m_values[SensorType::GPU_CLOCK] = gpuC;
        m_values[SensorType::MEM_LOAD]  = memL;
        // LIQUID_TEMP : laisse tel quel (alimente par setLiquidTemp via HID).
    }
}

} // namespace NZXTKraken
