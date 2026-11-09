#pragma once
#include <QObject>

namespace NZXTKraken {

class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject* parent = nullptr);
    void check(const QString& currentVersion);

signals:
    void updateAvailable(const QString& latestTag, const QString& releaseUrl);
};

} // namespace NZXTKraken
