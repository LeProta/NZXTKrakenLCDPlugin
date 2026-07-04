#pragma once
#include <QObject>

namespace NZXTKraken {

// Vérifie GitHub (releases/latest) une fois par lancement. Requête HTTPS via
// WinHTTP sur un thread détaché (PAS Qt5Network : OpenRGB ship un Qt5Core dont
// la version diffère du Qt de build -> mélanger Qt5Network crashe). Émet
// updateAvailable si le tag distant > version courante. Silencieux sinon.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    void check(const QString& currentVersion);

signals:
    void updateAvailable(const QString& latestTag, const QString& releaseUrl);
};

} // namespace NZXTKraken
