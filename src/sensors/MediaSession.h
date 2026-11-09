#pragma once
#include <QString>
#include <QImage>
#include <memory>

namespace NZXTKraken {

struct MediaSnapshot {
    bool    active       = false;
    bool    playing      = false;
    QString title;
    QString artist;
    QString album;
    double  positionSec  = 0.0;
    double  durationSec  = 0.0;
    double  playbackRate = 1.0;
    QImage  cover;
};

class MediaSession {
public:
    MediaSession();
    ~MediaSession();

    MediaSession(const MediaSession&)            = delete;
    MediaSession& operator=(const MediaSession&) = delete;

    bool poll(MediaSnapshot& out);

    bool pollTimeline(MediaSnapshot& out);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace NZXTKraken
