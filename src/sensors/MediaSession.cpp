#include "MediaSession.h"

#ifdef _WIN32
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.Control.h>
#include <QByteArray>
#include <chrono>
#include <cmath>
#endif

namespace NZXTKraken {

#ifdef _WIN32

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

// hstring (UTF-16) -> QString
static QString hstr(winrt::hstring const& h)
{
    return QString::fromWCharArray(h.c_str(), static_cast<int>(h.size()));
}

// TimeSpan (ticks 100 ns) -> secondes
static double secs(winrt::Windows::Foundation::TimeSpan ts)
{
    return std::chrono::duration<double>(ts).count();
}

// Valeurs BRUTES extraites de la session, sans logique de filtrage.
struct RawTimeline {
    bool   timelineValid = false;  // GetTimelineProperties a réussi
    bool   playing       = false;
    double rate          = 1.0;
    double posRaw        = 0.0;    // Position - StartTime
    double duration      = 0.0;    // EndTime - StartTime
};

// Lecture 100 % synchrone (aucun appel asynchrone) → utilisable à chaque frame.
static RawTimeline readRawTimeline(GlobalSystemMediaTransportControlsSession const& session)
{
    RawTimeline rt;
    try {
        auto pinfo = session.GetPlaybackInfo();
        rt.playing = (pinfo.PlaybackStatus() ==
            GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
        auto r = pinfo.PlaybackRate();           // IReference<double> (nullable)
        if (r) { const double rv = r.Value(); if (rv > 0.01) rt.rate = rv; }
    } catch (...) {}

    try {
        auto tl = session.GetTimelineProperties();
        const double start = secs(tl.StartTime());
        rt.posRaw        = secs(tl.Position()) - start;
        rt.duration      = secs(tl.EndTime())  - start;
        rt.timelineValid = true;
    } catch (...) {}
    return rt;
}

struct MediaSession::Impl {
    GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
    QString       lastKey;       // identité du morceau courant (titre|artiste|album)
    MediaSnapshot last;          // dernier instantané COMPLET (métadonnées + pochette)

    // Horloge interne de position (par morceau).
    double                                goodDuration = 0.0;        // dernière durée fiable
    double                                dispPos      = 0.0;        // position affichée courante
    std::chrono::steady_clock::time_point dispStamp{};               // instant de dispPos
    double                                lastPosRaw   = 0.0;        // dernière Position brute vue
    bool                                  haveSample   = false;

    bool ensureManager()
    {
        if (mgr) return true;
        try {
            mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            return static_cast<bool>(mgr);
        } catch (...) {
            mgr = nullptr;
            return false;
        }
    }

    void resetClock()   // à appeler au changement de morceau
    {
        goodDuration = 0.0;
        dispPos      = 0.0;
        lastPosRaw   = 0.0;
        haveSample   = false;
    }

    // Position via une horloge monotone interne, recalée sur l'app au besoin.
    //
    // On fait avancer la position NOUS-MÊMES tant que ça joue (free-run) et on ne
    // se recale sur la valeur de l'app que lorsqu'elle publie une NOUVELLE position
    // (≠ précédente) — soit un saut net (seek), soit l'app a pris de l'avance.
    // Conséquence : plus de gel (apps qui ne rafraîchissent pas Position après un
    // seek, ou qui ne la fournissent pas comme Cider) ni de reculs (petit retard
    // de l'app). La durée, elle, est maintenue (hold last-good) à travers les
    // rapports dégénérés (durée qui retombe à ~0 juste après un seek).
    void applyTimeline(const RawTimeline& rt, MediaSnapshot& out)
    {
        const auto now = std::chrono::steady_clock::now();
        double elapsed = haveSample
            ? std::chrono::duration<double>(now - dispStamp).count() : 0.0;
        if (elapsed < 0.0 || elapsed > 3600.0) elapsed = 0.0;   // garde-fou (veille…)

        out.playing      = rt.playing;
        out.playbackRate = rt.rate;

        const bool degenerate = rt.timelineValid && (rt.duration <= 0.5) && (goodDuration > 0.5);
        if (rt.timelineValid && rt.duration > 0.5) goodDuration = rt.duration;

        double disp = dispPos + (rt.playing ? elapsed * rt.rate : 0.0);   // free-run
        if (rt.timelineValid && !degenerate) {
            const double app = rt.posRaw;
            if (!haveSample) {
                disp = app;                                    // 1ère valeur → ancrage
            } else if (std::fabs(app - lastPosRaw) > 0.05) {   // l'app a publié du neuf
                const bool seek     = std::fabs(app - disp) > 3.0;
                const bool appAhead = app > disp + 0.25;
                if (seek || appAhead) disp = app;              // sinon on garde le free-run (pas de recul)
            }
        }

        if (disp < 0.0) disp = 0.0;
        if (goodDuration > 0.5 && disp > goodDuration) disp = goodDuration;

        out.positionSec = disp;
        out.durationSec = (goodDuration > 0.5) ? goodDuration : 0.0;

        dispPos   = disp;
        dispStamp = now;
        if (rt.timelineValid) lastPosRaw = rt.posRaw;
        haveSample = true;
    }
};

MediaSession::MediaSession()  : d(std::make_unique<Impl>()) {}
MediaSession::~MediaSession() = default;

bool MediaSession::poll(MediaSnapshot& out)
{
    out = MediaSnapshot{};
    try {
        if (!d->ensureManager())
            return false;

        auto session = d->mgr.GetCurrentSession();
        if (!session) {
            out.active = false;     // rien ne joue
            d->last    = out;
            return true;
        }

        // Métadonnées (titre / artiste / album) + pochette — appel asynchrone.
        // Attente BORNÉE (400 ms) : poll() tourne sur le thread de rendu ;
        // un .get() non borné pouvait bloquer la frame plusieurs centaines de
        // ms quand l'app média est lente à répondre. En cas de dépassement on
        // ressert le dernier instantané complet et on réessaiera au tick suivant.
        auto propsOp = session.TryGetMediaPropertiesAsync();
        if (propsOp.wait_for(std::chrono::milliseconds(400))
                != winrt::Windows::Foundation::AsyncStatus::Completed) {
            propsOp.Cancel();
            out = d->last;
            if (d->last.active)
                d->applyTimeline(readRawTimeline(session), out);
            return d->last.active;
        }
        auto props = propsOp.GetResults();
        out.title  = hstr(props.Title());
        out.artist = hstr(props.Artist());
        out.album  = hstr(props.AlbumTitle());

        // Changement de morceau : reset de l'horloge interne + re-décodage pochette.
        const QString key = out.title + QChar(0x1F) + out.artist + QChar(0x1F) + out.album;
        if (key != d->lastKey) {
            d->resetClock();
            d->last.cover = QImage();
            try {
                auto thumbRef = props.Thumbnail();
                if (thumbRef) {
                    auto stream = thumbRef.OpenReadAsync().get();
                    const uint32_t size = static_cast<uint32_t>(stream.Size());
                    if (size > 0) {
                        DataReader reader(stream);
                        reader.LoadAsync(size).get();
                        QByteArray bytes;
                        bytes.resize(static_cast<int>(size));
                        auto* p = reinterpret_cast<uint8_t*>(bytes.data());
                        reader.ReadBytes(winrt::array_view<uint8_t>(p, p + size));
                        QImage img;
                        if (img.loadFromData(reinterpret_cast<const uchar*>(bytes.constData()),
                                             static_cast<int>(size)))
                            d->last.cover = img;
                    }
                }
            } catch (...) {
                d->last.cover = QImage();
            }
            d->lastKey = key;
        }

        d->applyTimeline(readRawTimeline(session), out);

        out.cover  = d->last.cover;
        out.active = true;

        d->last = out;   // mémorise l'instantané complet (base pour pollTimeline)
        return true;
    } catch (...) {
        out.active = false;
        return false;
    }
}

bool MediaSession::pollTimeline(MediaSnapshot& out)
{
    out = MediaSnapshot{};
    try {
        if (!d->mgr)
            return false;                         // pas encore initialisé → attend poll()

        auto session = d->mgr.GetCurrentSession();
        if (!session) {
            out.active     = false;               // lecture arrêtée
            d->last.active = false;
            return true;
        }
        if (!d->last.active) {
            out = d->last;                        // métadonnées pas encore connues → attend poll()
            return true;
        }

        out = d->last;                            // métadonnées + pochette en cache
        d->applyTimeline(readRawTimeline(session), out);

        d->last.positionSec  = out.positionSec;   // base fraîche
        d->last.durationSec  = out.durationSec;
        d->last.playing      = out.playing;
        d->last.playbackRate = out.playbackRate;
        return true;
    } catch (...) {
        out.active = false;
        return false;
    }
}

#else  // ── Non-Windows : implémentation neutre ──────────────────────────────

struct MediaSession::Impl {};
MediaSession::MediaSession()  : d(std::make_unique<Impl>()) {}
MediaSession::~MediaSession() = default;
bool MediaSession::poll(MediaSnapshot& out)         { out = MediaSnapshot{}; return false; }
bool MediaSession::pollTimeline(MediaSnapshot& out) { out = MediaSnapshot{}; return false; }

#endif

} // namespace NZXTKraken
