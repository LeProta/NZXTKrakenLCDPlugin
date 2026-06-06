#pragma once
#include <QString>

namespace NZXTKraken {

// Gère le dossier médias :
//   %APPDATA%/OpenRGB/NZXTKrakenLCD/media
//
// Copie les fichiers uploadés dedans et fournit le chemin racine pour le bouton "Browse".
class KrakenMediaCache {
public:
    // Retourne le dossier racine "media" (le crée s'il n'existe pas)
    static QString mediaDirectory();

    // Retourne le dossier racine "NZXTKrakenLCD"
    static QString rootDirectory();

    // Copie un fichier source dans media/ et retourne le nouveau chemin.
    // Si le fichier existe déjà (même nom), il est écrasé.
    // Retourne une chaîne vide en cas d'erreur.
    static QString importFile(const QString& sourcePath);

    // Retourne le dossier Téléchargements Windows de l'utilisateur
    static QString downloadsDirectory();
};

} // namespace NZXTKraken
