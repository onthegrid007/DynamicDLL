#ifndef DYNAMIC_DLL_
#define DYNAMIC_DLL_

#include "../vendor/PlatformDetection/PlatformDetection.h"
#include "../vendor/STDExtras/vendor/Singleton/singleton_container_map.hpp"

#if _BUILD_PLATFORM_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
       #define WIN32_LEAN_AND_MEAN 1
    #endif
    #include <windows.h>
    #include <commdlg.h>
    #define DD_EXPORT __declspec(dllexport)
#else
    #include <dlfcn.h>
    #define DD_EXPORT __attribute__((visibility("default")))
#endif

class DynamicDLL : public SingletonContainerMap<DynamicDLL> {
    public:
    private:
    _SCM_CHILD_DECLORATIONS(DynamicDLL)
    DD_EXPORT DynamicDLL() {}
    DD_EXPORT ~DynamicDLL() {}
};
_SCM_CHILD_DEFINITIONS(DynamicDLL)

#endif