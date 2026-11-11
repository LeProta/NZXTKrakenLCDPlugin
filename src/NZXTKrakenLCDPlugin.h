#pragma once
#include "OpenRGBPluginInterface.h"

namespace NZXTKraken { class KrakenLCDWidget; }

class NZXTKrakenLCDPlugin
    : public QObject
    , public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID FILE "NZXTKrakenLCDPlugin.json")
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    NZXTKrakenLCDPlugin()  = default;
    ~NZXTKrakenLCDPlugin() override;

    OpenRGBPluginInfo   GetPluginInfo()                                     override;
    unsigned int        GetPluginAPIVersion()                               override;
    void                Load(OpenRGBPluginAPIInterface* plugin_api_ptr)    override;
    QWidget*            GetWidget()                                         override;
    QMenu*              GetTrayMenu()                                       override;
    void                Unload()                                            override;
    void                OnProfileAboutToLoad()                              override {}
    void                OnProfileLoad(nlohmann::json)                       override {}
    nlohmann::json      OnProfileSave()                                     override { return {}; }
    unsigned char*      OnSDKCommand(unsigned int, unsigned char*, unsigned int*) override { return nullptr; }
    void                ProfileManagerUpdated(unsigned int)                 override {}
    void                ResourceManagerUpdated(unsigned int)                override {}
    void                SettingsManagerUpdated(unsigned int)                override;

private:
    NZXTKraken::KrakenLCDWidget* m_widget = nullptr;
};
