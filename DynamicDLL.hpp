#ifndef DYNAMIC_DLL_
#define DYNAMIC_DLL_

#include "vendor/FileUtilities/vendor/PlatformDetection/PlatformDetection.h"
#include "vendor/FileUtilities/vendor/STDExtras/vendor/Singleton/singleton_container_map.hpp"
#include "vendor/FileUtilities/FileUtilities.hpp"
#include "vendor/FileUtilities/vendor/STDExtras/Semaphore/semaphore.h"
#include "vendor/FileUtilities/vendor/STDExtras/STDExtras.hpp"

// #define DD_FORCE_HOT_RELOADABILITY 1

#if _BUILD_PLATFORM_WINDOWS & !DD_FORCE_HOT_RELOADABILITY
    #ifndef WIN32_LEAN_AND_MEAN
       #define WIN32_LEAN_AND_MEAN 1
    #endif
    #include <windows.h>
    #include <commdlg.h>
    #define DD_EXPORT __declspec(dllexport)
    #define DD_IMPORT __declspec(dllimport)
    typedef HMODULE DLLModule;
    DD_IMPORT HMODULE __stdcall LoadLibraryA(LPCSTR);
    DD_IMPORT FARPROC __stdcall GetProcAddress(DLLModule, LPCSTR);
    DD_IMPORT bool __stdcall FreeLibrary(DLLModule);
    #define OPEN_LIB_INIT_BY_CSTR(str) LoadLibraryA(str)
    #define OPEN_LIB_BY_CSTR(str) LoadLibraryA(str)
    #define GET_SYM_BY_STR(handle, str) GetProcAddress(handle, str)
    #define CLOSE_LIB_BY_HANDLE(handle) FreeLibrary(handle);
    
#else
    #include <dlfcn.h>
    #define DD_EXPORT __attribute__((visibility("default")))
    typedef void* DLLModule;
    #define OPEN_LIB_INIT_BY_CSTR(str) dlopen(str, RTLD_NOW | ((BIT_CHECK(m_flags, ReloadFlags::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_GLOBAL : RTLD_GLOBAL)))
    #define OPEN_LIB_BY_CSTR(str) dlopen(str, RTLD_NOW | ((BIT_CHECK(m_flags, ReloadFlags::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_LOCAL : RTLD_LOCAL)))
    // #define OPEN_LIB_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL)
    // #define OPEN_LIB_COLD_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_GLOBAL)
    #define GET_SYM_BY_STR(handle, str) dlsym(handle, str)
    #define CLOSE_LIB_BY_HANDLE(handle) dlclose(handle)
#endif

class DynamicDLL : public SingletonContainerMap<DynamicDLL> {
    private:
    DD_EXPORT DLLModule m_dll = 0;
    DD_EXPORT Semaphore m_sem;
    DD_EXPORT std::mutex m_mtx;
    DD_EXPORT std::unordered_map<std::string, void*> m_addressCache;
    DD_EXPORT std::vector<FileUtilities::ParsedPath> m_libLoadNames;
    DD_EXPORT std::vector<std::string> m_preLoadSymbols;
    DD_EXPORT bool m_isInitLoaded = false;
    DD_EXPORT bool m_isReloading = false;
    DD_EXPORT void Unload(bool waitForAFullHalt = false) {
        m_sem.notify();
        if(waitForAFullHalt) {
            m_sem.wait();
            // m_sem.waitFor([&](std::I64, std::I64){return !(this->m_isReloading); });
        }
        else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        {
            std::ThreadLock lock(m_mtx);
            m_addressCache.clear();
        }
        CLOSE_LIB_BY_HANDLE(m_dll);
        m_dll = 0;
    }
    DD_EXPORT void Load(bool initLoad = false) {
        if(initLoad) {
            for(auto& lib : m_libLoadNames) {
                m_dll = OPEN_LIB_INIT_BY_CSTR(std::string(lib).c_str());
                if(m_dll) goto PRELOAD;
            }
        }
        else {
            for(auto& lib : m_libLoadNames) {
            m_dll = OPEN_LIB_BY_CSTR(std::string(lib).c_str());
            if(m_dll) goto PRELOAD;
        }
        }
        m_dll = 0;
        return;
        PRELOAD:
        for(const auto& symname : m_preLoadSymbols)
            m_addressCache[symname] = (void*)GET_SYM_BY_STR(m_dll, symname.c_str());
        m_isInitLoaded = true;
    }
    
    public:
    enum ReloadFlags : char {
        COLD_RELOADABLE = BIT(0),
        HOT_RELOADABLE = BIT(1)
    };
    
    DD_EXPORT bool isSymAddressCached(const std::string key, bool blocking = true) {
        if(blocking) {
            std::ThreadLock lock(m_mtx);
            return ((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty());
        } else {
            return ((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty());
        }
    }
    
    DD_EXPORT void Reload(bool waitForAFullHalt = false) {
        if(!m_isInitLoaded) return;
        m_isReloading = true;
        Unload(waitForAFullHalt);
        Load(false);
        m_isReloading = false;
        m_sem.notify();
    }
    
    private:
    DD_EXPORT ReloadFlags m_flags;
    DD_EXPORT void* GetSymAddress(std::string symname) {
        if(!m_isInitLoaded) return 0;
        m_sem.waitFor([&](std::I64, std::I64){return !(this->m_isReloading); });
        std::ThreadLock lock(m_mtx);
        if(!isSymAddressCached(symname, false)) {
            m_addressCache[symname] = (void*)GET_SYM_BY_STR(m_dll, symname.c_str());
        }
        return m_addressCache[symname];
    }
    
    template<typename T, typename RT, typename ... Args>
    DD_EXPORT RT CallSym(std::string symname, Args&&... args) {
        return (*(T*)GetSymAddress(symname))(args...);
    }
    
    template<typename T>
    DD_EXPORT T& GetSymAs(std::string symname) {
        return *(T*)GetSymAddress(symname);
    }
    
    _SCM_CHILD_DECLORATIONS(DynamicDLL)
    DD_EXPORT DynamicDLL(std::vector<FileUtilities::ParsedPath> libloadnames, std::vector<std::string> preloadsymbols, ReloadFlags flags = ReloadFlags::COLD_RELOADABLE) :
        m_libLoadNames(libloadnames),
        m_preLoadSymbols(preloadsymbols),
        m_flags(flags) {
        Load(true);
    }
    DD_EXPORT ~DynamicDLL() {}
};
_SCM_CHILD_DEFINITIONS(DynamicDLL)

#endif