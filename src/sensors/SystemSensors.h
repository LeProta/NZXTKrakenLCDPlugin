#pragma once
#include <QString>
#include <QMap>
#include <QMutex>
#include <QTimer>
#include <string>

namespace NZXTKraken {

enum class SensorType {
    CPU_TEMP, CPU_LOAD, CPU_CLOCK,
    GPU_TEMP, GPU_LOAD, GPU_CLOCK,
    MEM_LOAD,
    LIQUID_TEMP,
};

struct SensorValue {
    float   value     = 0.f;
    QString unit;
    bool    available = false;
};

struct SensorOption {
    SensorType type;
    QString    label;
    QString    shortLabel;
    QString    unit;
};

inline const QList<SensorOption> CPU_SENSORS = {
    { SensorType::CPU_TEMP,    "CPU Temperature",    "CPU",    "°C"  },
    { SensorType::CPU_LOAD,    "CPU Load",           "CPU",    "%"   },
    { SensorType::CPU_CLOCK,   "CPU Clock Speed",    "CPU",    "MHz" },
    { SensorType::LIQUID_TEMP, "Liquid Temperature", "Liquid", "°C"  },
};

inline const QList<SensorOption> GPU_SENSORS = {
    { SensorType::GPU_TEMP,  "GPU Temperature", "GPU", "°C"  },
    { SensorType::GPU_LOAD,  "GPU Load",        "GPU", "%"   },
    { SensorType::GPU_CLOCK, "GPU Clock Speed", "GPU", "MHz" },
};

inline const QList<SensorOption> MEMORY_SENSORS = {
    { SensorType::MEM_LOAD, "RAM Load", "RAM", "%" },
};

class SystemSensors {
public:
     SystemSensors();
    ~SystemSensors();

    void startPolling();
    void stopPolling();
    void shutdown();

    SensorValue read(SensorType type) const;
    void setLiquidTemp(float celsius);

private:
    void pollOnce();
    void buildSensorMap();
    bool                          m_mapBuilt = false;
    QMap<SensorType, std::string> m_ids;
    mutable QMutex                m_mutex;
    QMap<SensorType, SensorValue> m_values;
    QTimer*                       m_timer = nullptr;
};

} // namespace NZXTKraken
