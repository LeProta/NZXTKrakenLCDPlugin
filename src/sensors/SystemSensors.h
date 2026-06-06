#pragma once
#include <QString>
#include <QMap>
#include <QMutex>
#include <QTimer>
#include <string>

namespace NZXTKraken {

// ─── Types de capteurs ───────────────────────────────────────────────────────
// CPU/GPU/MEM = capteurs système (sélectionnables par l'utilisateur)
// LIQUID_TEMP = capteur du Kraken lui-même (alimenté par setLiquidTemp)
enum class SensorType {
    CPU_TEMP, CPU_LOAD, CPU_CLOCK,
    GPU_TEMP, GPU_LOAD, GPU_CLOCK,
    MEM_LOAD,
    LIQUID_TEMP,
};

struct SensorValue {
    float   value     = 0.f;
    QString unit;
    bool    available = false;  // false → afficher "N/A"
};

struct SensorOption {
    SensorType type;
    QString    label;
    QString    shortLabel;
    QString    unit;
};

// ─── Listes pour les ComboBox UI ─────────────────────────────────────────────
static const QList<SensorOption> CPU_SENSORS = {
    { SensorType::CPU_TEMP,    "CPU Temperature",    "CPU",    "°C"  },
    { SensorType::CPU_LOAD,    "CPU Load",           "CPU",    "%"   },
    { SensorType::CPU_CLOCK,   "CPU Clock Speed",    "CPU",    "MHz" },
    { SensorType::LIQUID_TEMP, "Liquid Temperature", "Liquid", "°C"  },
};

static const QList<SensorOption> GPU_SENSORS = {
    { SensorType::GPU_TEMP,  "GPU Temperature", "GPU", "°C"  },
    { SensorType::GPU_LOAD,  "GPU Load",        "GPU", "%"   },
    { SensorType::GPU_CLOCK, "GPU Clock Speed", "GPU", "MHz" },
};

static const QList<SensorOption> MEMORY_SENSORS = {
    { SensorType::MEM_LOAD, "RAM Load", "RAM", "%" },
};

// =============================================================================
//  SystemSensors
//  ---------------------------------------------------------------------------
//  Windows : lecture via LibreHardwareMonitor embarqué (lhwm-cpp-wrapper.lib, lib statique + lhwm-wrapper.dll, l'assembly .NET à placer à côté d'OpenRGB.exe). Aucun OHM/LHM externe requis.
//    - startPolling() appelle buildSensorMap() : LHWM::GetHardwareSensorMap() est récupérée UNE fois, puis chaque capteur est résolu automatiquement par préfixe d'identifiant (/amdcpu/ /intelcpu/ → CPU ; /gpu-amd/ /gpu-nvidia/ /gpu-intel/ → GPU ; /ram/ → RAM) + type (Temperature / Load / Clock) + préférence de nom. Le résultat est l'identifiant exact.
//    - pollOnce() lit chaque valeur via LHWM::GetSensorValue(identifiant).
//    - Capteur non résolu ou introuvable → SensorValue.available = false → "N/A".
//  La température du liquide vient du Kraken (HID, via setLiquidTemp), pas de LHM.
//
//  Pré-requis : lhwm-wrapper.dll présent (vérifié en amont par le plugin) et pour les capteurs CPU / carte mère, OpenRGB lancé en administrateur (driver ring0 de LHM). Toute erreur → available = false. Jamais de crash.
// =============================================================================
class SystemSensors {
public:
     SystemSensors();
    ~SystemSensors();

    void startPolling();
    void stopPolling();
    void shutdown();   // arrêt complet (timer) ; idempotent ; appelé sur le thread du worker

    SensorValue read(SensorType type) const;
    void setLiquidTemp(float celsius);

private:
    void pollOnce();
    void buildSensorMap();                   // résout 1× : SensorType → identifiant LHM
    bool                          m_mapBuilt = false;
    QMap<SensorType, std::string> m_ids;      // identifiant LHM par capteur (vide = non résolu)
    mutable QMutex                m_mutex;
    QMap<SensorType, SensorValue> m_values;
    QTimer*                       m_timer = nullptr;
};

} // namespace NZXTKraken
