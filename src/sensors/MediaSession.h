#pragma once
#include <QString>
#include <QImage>
#include <memory>

namespace NZXTKraken {

// ─────────────────────────────────────────────────────────────────────────────
//  Instantané de la lecture média en cours (now playing).
//  Rempli par MediaSession via l'API Windows SMTC. Tous les champs sont neutres
//  (active=false) si rien ne joue ou si l'API échoue.
// ─────────────────────────────────────────────────────────────────────────────
struct MediaSnapshot {
    bool    active       = false;  // true si une session média existe
    bool    playing      = false;  // true si en lecture (vs pause)
    QString title;                 // titre du morceau
    QString artist;                // artiste / interprète
    QString album;                 // album
    double  positionSec  = 0.0;    // position courante (s) — extrapolée si en lecture
    double  durationSec  = 0.0;    // durée totale (s ; 0 si inconnue)
    double  playbackRate = 1.0;    // vitesse de lecture (1.0 par défaut)
    QImage  cover;                 // pochette (peut être nulle)
};

// ─────────────────────────────────────────────────────────────────────────────
//  MediaSession
//   Lit le « Now Playing » de Windows via GlobalSystemMediaTransportControls
//   (SMTC / Windows.Media.Control). Agrège n'importe quelle app média
//   (Spotify, navigateur, Apple Music, Tidal…). Détails WinRT confinés au .cpp
//   (PIMPL) pour ne pas polluer les en-têtes.
//
//   IMPORTANT — SMTC ne pousse Position qu'au play/pause/seek (pas en continu).
//   On l'extrapole donc via LastUpdatedTime quand ça joue, sinon le timestamp
//   resterait figé pendant la lecture.
//
//   Deux niveaux de rafraîchissement (à appeler depuis un thread COM/MTA) :
//     • poll()         : COMPLET (métadonnées + pochette + timeline). Appel lent
//                        (~1 Hz) car TryGetMediaPropertiesAsync est asynchrone.
//     • pollTimeline() : LÉGER (position / lecture uniquement, 100 % synchrone),
//                        à appeler à la cadence de rendu pour un timestamp fluide
//                        qui suit aussi les seeks. Réutilise les métadonnées et la
//                        pochette du dernier poll() complet.
// ─────────────────────────────────────────────────────────────────────────────
class MediaSession {
public:
    MediaSession();
    ~MediaSession();

    MediaSession(const MediaSession&)            = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    // Complet : métadonnées + pochette + timeline. Pochette re-décodée seulement
    // au changement de morceau (cache interne).
    bool poll(MediaSnapshot& out);

    // Léger : rafraîchit position/durée/lecture/vitesse (extrapolées), en
    // réutilisant titre/artiste/album/pochette du dernier poll() complet.
    bool pollTimeline(MediaSnapshot& out);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace NZXTKraken
