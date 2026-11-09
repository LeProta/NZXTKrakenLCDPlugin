#pragma once
#include <QString>

namespace NZXTKraken {

class KrakenMediaCache {
public:
    static QString mediaDirectory();

    static QString rootDirectory();

    static QString importFile(const QString& sourcePath);

    static QString downloadsDirectory();
};

} // namespace NZXTKraken
