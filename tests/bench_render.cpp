// Banc de mesure du pipeline LCD sans matériel : rendu QPainter, mise à
// l'échelle 640 -> 480 (Z / Elite 360 / 2023), encodage Q565 (Elite V2) / JPEG.
// Même enchaînement que KrakenWorker::renderTick, envoi USB exclu.
//   bench_render.exe [frames] [image.png]
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <cstdio>
#include "renderer/FrameRenderer.h"

using namespace NZXTKraken;

struct Stat { double render = 0, scale = 0, q565 = 0, jpeg = 0; qint64 q565Bytes = 0, jpegBytes = 0; };

static Stat run(FrameRenderer& r, const FrameConfig& cfg, int frames)
{
    Stat s;
    QElapsedTimer t;
    for (int i = 0; i < frames; ++i) {
        t.start();
        const QImage frame = r.render(cfg);
        s.render += t.nsecsElapsed() / 1e6;

        t.start();
        const QByteArray q = FrameRenderer::toQ565(frame);
        s.q565 += t.nsecsElapsed() / 1e6;
        s.q565Bytes += q.size();

        t.start();
        const QImage small = frame.scaled(480, 480, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        s.scale += t.nsecsElapsed() / 1e6;

        t.start();
        const QByteArray j = FrameRenderer::toJpeg(small, 90);
        s.jpeg += t.nsecsElapsed() / 1e6;
        s.jpegBytes += j.size();
    }
    s.render /= frames; s.scale /= frames; s.q565 /= frames; s.jpeg /= frames;
    s.q565Bytes /= frames; s.jpegBytes /= frames;
    return s;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const int frames = argc > 1 ? atoi(argv[1]) : 200;

    // Image de test (photo-like : dégradés + bruit) si aucune fournie
    QTemporaryDir tmp;
    QString img = argc > 2 ? QString::fromLocal8Bit(argv[2]) : tmp.filePath("test.png");
    if (argc <= 2) {
        QImage im(1200, 1200, QImage::Format_RGB32);
        for (int y = 0; y < im.height(); ++y)
            for (int x = 0; x < im.width(); ++x)
                im.setPixel(x, y, qRgb((x * 255) / 1200, (y * 255) / 1200, ((x ^ y) * 37) & 0xFF));
        im.save(img);
    }

    SystemSensors sensors;   // valeurs par défaut, pas de polling LHM
    FrameRenderer r(&sensors);

    struct Case { const char* name; FrameConfig cfg; };
    QVector<Case> cases;
    FrameConfig c;
    c.mode = DisplayMode::SINGLE_INFOGRAPHIC; cases.append({ "single infographic", c });
    c.mode = DisplayMode::TRIPLE_INFOGRAPHIC; cases.append({ "triple infographic", c });
    c.mode = DisplayMode::CLOCKFACE;          cases.append({ "clock", c });
    c.mode = DisplayMode::AUDIO_VISUAL;       cases.append({ "audio visual", c });
    c.mode = DisplayMode::IMAGE_GIF; c.gifPath = img; cases.append({ "static image", c });

    std::printf("%-20s %9s %9s %9s %9s %11s %11s\n", "mode (ms/frame)", "render", "q565", "scale480", "jpeg480",
                "q565 bytes", "jpeg bytes");
    for (const Case& k : cases) {
        run(r, k.cfg, 5);   // chauffe (polices, caches)
        const Stat s = run(r, k.cfg, frames);
        std::printf("%-20s %9.2f %9.2f %9.2f %9.2f %11lld %11lld\n", k.name, s.render, s.q565,
                    s.scale, s.jpeg, s.q565Bytes, s.jpegBytes);
    }
    return 0;
}
