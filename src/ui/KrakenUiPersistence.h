#pragma once
#include <QString>
#include "../renderer/FrameRenderer.h"

namespace NZXTKraken {

class KrakenUiPersistence {
public:
    static QString settingsFilePath();

    static FrameConfig load();

    static bool save(const FrameConfig& cfg);
};

} // namespace NZXTKraken
