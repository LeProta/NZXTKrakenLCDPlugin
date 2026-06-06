#pragma once
#include <QString>
#include "../renderer/FrameRenderer.h"

namespace NZXTKraken {

// Charge / sauvegarde la config dans :
//   %APPDATA%/OpenRGB/NZXTKrakenLCD/settings.json
//
// Chaque modification (mode, couleur, capteur, brightness, image…) doit appeler save() pour persister immédiatement.
class KrakenUiPersistence {
public:
    static QString settingsFilePath();

    // Charge la config depuis le fichier. Retourne les défauts si le fichier n'existe pas ou est corrompu.
    static FrameConfig load();

    // Sauvegarde la config sur disque (crée le dossier si nécessaire).
    static bool save(const FrameConfig& cfg);
};

} // namespace NZXTKraken
