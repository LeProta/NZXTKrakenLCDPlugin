#pragma once
#include <QtPlugin>
#include <QLabel>
#include <QMenu>
#include <QImage>
#include <string>

#define OpenRGBPluginInterface_IID  "com.OpenRGBPluginInterface"
#define OPENRGB_PLUGIN_API_VERSION  4

enum
{
    OPENRGB_PLUGIN_LOCATION_TOP         = 0,
    OPENRGB_PLUGIN_LOCATION_DEVICES     = 1,
    OPENRGB_PLUGIN_LOCATION_INFORMATION = 2,
    OPENRGB_PLUGIN_LOCATION_SETTINGS    = 3,
};

struct OpenRGBPluginInfo
{
    std::string     Name;
    std::string     Description;
    std::string     Version;
    std::string     Commit;
    std::string     URL;
    QImage          Icon;           // 64×64

    unsigned int    Location;
    std::string     Label;
    std::string     TabIconString;
    QImage          TabIcon;        // 16×16
};

class ResourceManagerInterface;    // forward — défini dans OpenRGB-source

class OpenRGBPluginInterface
{
public:
    virtual                    ~OpenRGBPluginInterface() {}
    virtual OpenRGBPluginInfo   GetPluginInfo()                                         = 0;
    virtual unsigned int        GetPluginAPIVersion()                                   = 0;
    virtual void                Load(ResourceManagerInterface* resource_manager_ptr)    = 0;
    virtual QWidget*            GetWidget()                                             = 0;
    virtual QMenu*              GetTrayMenu()                                           = 0;  // pas de paramètre
    virtual void                Unload()                                                = 0;
};

Q_DECLARE_INTERFACE(OpenRGBPluginInterface, OpenRGBPluginInterface_IID)

// ─── infos plugin ─────────────────────────────────────────────────────────────
namespace NZXTKraken { class KrakenLCDWidget; }

class NZXTKrakenLCDPlugin
    : public QObject
    , public OpenRGBPluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID OpenRGBPluginInterface_IID)
    Q_INTERFACES(OpenRGBPluginInterface)

public:
    NZXTKrakenLCDPlugin()  = default;
    ~NZXTKrakenLCDPlugin() override;

    OpenRGBPluginInfo   GetPluginInfo()                                     override;
    unsigned int        GetPluginAPIVersion()                               override;
    void                Load(ResourceManagerInterface* resource_manager_ptr) override;
    QWidget*            GetWidget()                                         override;
    QMenu*              GetTrayMenu()                                       override;
    void                Unload()                                            override;

private:
    NZXTKraken::KrakenLCDWidget* m_widget = nullptr;
};
