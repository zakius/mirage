create_fooyin_plugin_internal(
    subsonic
    DEPENDS #
        Fooyin::CorePrivate
        Fooyin::Gui
        Qt6::Network
    SOURCES
        rangestreamdevice.cpp
        rangestreamdevice.h
        subsonicapi.cpp
        subsonicapi.h
        subsonicpage.cpp
        subsonicpage.h
        subsonicplugin.cpp
        subsonicplugin.h
        subsonicprovider.cpp
        subsonicprovider.h
)
