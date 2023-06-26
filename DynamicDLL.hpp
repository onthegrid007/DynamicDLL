#ifndef DYNAMIC_DLL_
#define DYNAMIC_DLL_

#include "vendor/FileUtilities/vendor/STDExtras/vendor/ThreadPool/vendor/PlatformDetection/PlatformDetection.h"
#include <vector>
#include <thread>
#include "vendor/FileUtilities/vendor/STDExtras/vendor/ThreadPool/vendor/Semaphore/semaphore.h"
#include "vendor/FileUtilities/FileUtilities.hpp"
#include "vendor/FileUtilities/vendor/STDExtras/vendor/ThreadPool/vendor/Semaphore/vendor/Singleton/singleton_container_map.hpp"
extern "C" {
    #include "vendor/libelfmaster/include/libelfmaster.h"
}

#if _BUILD_PLATFORM_WINDOWS
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
    #include <link.h>
    #define DD_EXPORT __attribute__((visibility("default")))
    typedef void* DLLModule;
    #define OPEN_LIB_INIT_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_DEEPBIND | (((m_flags & ReloadFlags::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_GLOBAL : RTLD_GLOBAL)))
    #define OPEN_LIB_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_DEEPBIND | (((m_flags & ReloadFlags::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_LOCAL : RTLD_LOCAL)))
    // #define OPEN_LIB_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_NODELETE | RTLD_GLOBAL)
    // #define OPEN_LIB_COLD_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_GLOBAL)
    #define GET_SYM_BY_STR(handle, str) dlsym(handle, str)
    #define CLOSE_LIB_BY_HANDLE(handle) dlclose(handle)
#endif

inline const uint64_t getEditDistance(const std::string& x, const std::string& y) {
    const uint64_t m = x.length();
    const uint64_t n = y.length();
    uint64_t T[m + 1][n + 1];
    for(uint64_t i = 1; i <= m; i++)
        T[i][0] = i;
    for(uint64_t j = 1; j <= n; j++)
        T[0][j] = j;
    for(uint64_t i = 1; i <= m; i++)
        for (uint64_t j = 1; j <= n; j++)
            T[i][j] = std::min(std::min(T[i-1][j] + 1, T[i][j-1] + 1), T[i-1][j-1] + (x[i - 1] == y[j - 1] ? 0 : 1));
    return T[m][n];
}

inline const long double findStringSimilarity(const std::string& first, const std::string& second) {
    const long double maxL = std::max(first.length(), second.length());
    return ((maxL > 0) ? ((maxL - getEditDistance(first, second)) / maxL) : 1);
}

class DynamicDLL : public SingletonContainerMap<DynamicDLL> {
    public:
    typedef std::string SymNameType;
    #define _DYNAMIC_DLL_FRIEND friend class DynaicDLL; friend class SingletonContainerMap<DynamicDLL>;
    private:
    DD_EXPORT DLLModule m_dll = 0;
    DD_EXPORT Semaphore m_sem;
    DD_EXPORT std::mutex m_mtx;
    DD_EXPORT std::vector<SymNameType> m_dynSymTab;
    DD_EXPORT std::unordered_map<SymNameType, void*> m_addressCache;
    DD_EXPORT std::vector<FileUtilities::ParsedPath> m_libLoadNames;
    DD_EXPORT std::vector<std::string> m_preLoadSymbols;
    DD_EXPORT bool m_isInitLoaded = false;
    DD_EXPORT bool m_isReloading = false;
    
    DD_EXPORT void GenDynSymTab(const std::string& path) {
        elfobj_t obj;
	    elf_error_t error;
        elf_dynsym_iterator_t ds_iter;
        struct elf_symbol symbol;
        if(!elf_open_object(path.c_str(), &obj, ELF_LOAD_F_FORENSICS, &error))
            return;
        elf_dynsym_iterator_init(&obj, &ds_iter);
        m_dynSymTab.clear();
        while(elf_dynsym_iterator_next(&ds_iter, &symbol) == ELF_ITER_OK)
            if(symbol.name)
                m_dynSymTab.emplace_back(symbol.name);
        elf_close_object(&obj);
    }
    
    DD_EXPORT void Unload(bool waitForAFullHalt = false) {
        using namespace std;
        using namespace this_thread;
        m_sem.notify();
        if(waitForAFullHalt) {
            m_sem.wait();
            // m_sem.waitFor([&](std::I64, std::I64){return !(this->m_isReloading); });
        }
        else {
            sleep_for(std::chrono::seconds(1));
        }
        {
            lock_guard<mutex> lock(m_mtx);
            m_addressCache.clear();
        }
        CLOSE_LIB_BY_HANDLE(m_dll);
        m_dll = 0;
    }
    
    DD_EXPORT void Preload() {
        for(const auto& symname : m_preLoadSymbols) {
            // to-do
            // m_addressCache[symname] = (void*)GET_SYM_BY_STR(m_dll, symname.c_str());
        }
        m_isInitLoaded = true;
    }
    
    DD_EXPORT bool Load(bool initLoad = false) {
        if(initLoad) {
            for(auto& lib : m_libLoadNames) {
                std::string currentLib = lib;
                GenDynSymTab(currentLib);
                m_dll = OPEN_LIB_INIT_BY_CSTR(currentLib.c_str());
                if(m_dll) {
                    Preload();
                    return true;
                }
            }
        }
        else {
            for(auto& lib : m_libLoadNames) {
                std::string currentLib = lib;
                GenDynSymTab(currentLib);
                m_dll = OPEN_LIB_BY_CSTR(currentLib.c_str());
                if(m_dll) {
                    Preload();
                    return true;
                }
            }
        }
        m_dll = 0;
        return false;
    }
    
    public:
    enum ReloadFlags : char {
        COLD_RELOADABLE = 0,
        HOT_RELOADABLE = 1
    };
    
    DD_EXPORT bool isSymAddressCached(const std::string key, bool blocking = true) {
        using namespace std;
        if(blocking) {
            lock_guard<mutex> lock(m_mtx);
            // BADLOGV(std::toString<bool>((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty()))
            return ((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty());
        } else {
            // BADLOGV(std::toString<bool>((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty()))
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
 
    template<typename T, typename ... Args>
    DD_EXPORT auto& CallSym(T* _T, const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false, Args... args) {
        return (*(T*)GetSymAddress(symname, needsDemangle, cmpAsMangled))(args...);
    }
    
    template<typename T>
    DD_EXPORT T& GetSymAs(const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false) {
        return *((T*)GetSymAddress(symname, needsDemangle, cmpAsMangled));
    }
    
    private:
    DD_EXPORT ReloadFlags m_flags;
    DD_EXPORT void* GetSymAddress(const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false) {
        using namespace std;
        if(!m_isInitLoaded) return 0;
        m_sem.waitFor([&](int64_t, int64_t){return !(this->m_isReloading); });
        lock_guard<mutex> lock(m_mtx);
        std::string lookupName;
        lookupName = needsDemangle ? std::move(abi::__cxa_demangle(symname.c_str(), NULL, NULL, NULL)) : symname;
        try {
            std::for_each(m_dynSymTab.begin(), m_dynSymTab.end(), [&](std::string& s) {
                const std::string cmp = (cmpAsMangled ? s : abi::__cxa_demangle(s.c_str(), NULL, NULL, NULL));
                // BADLOG("cmp: " << cmp << std::endl << "lookupName: " << lookupName << std::endl << "actual sym: " << s << std::endl << "percent: " << std::findStringSimilarity(lookupName, cmp) * std::LD(100) << std::endl)
                if(findStringSimilarity(lookupName, cmp) > 0.7) {
                    // BADLOGV(s);
                    lookupName = s;
                    throw std::exception();
                }
            });
        }
        catch(std::exception& e) {}
        if(!isSymAddressCached(lookupName, false)) {
            m_addressCache[lookupName] = (void*)GET_SYM_BY_STR(m_dll, lookupName.c_str());
        }
        // BADLOGV(m_addressCache[lookupName])
        return m_addressCache[lookupName];
    }
    
    _SCM_CHILD_DECLORATIONS(DynamicDLL)
    DD_EXPORT DynamicDLL(std::vector<FileUtilities::ParsedPath> libloadnames, std::vector<std::string> preloadsymbols, ReloadFlags flags = ReloadFlags::COLD_RELOADABLE) :
        m_libLoadNames(libloadnames),
        m_preLoadSymbols(preloadsymbols),
        m_flags(flags) {
        if(!Load(true))
            std::cout << m_libLoadNames[0].getPath(FileUtilities::PathType::NameOnly) << ": DynamicDLL Load Failed!" << std::endl;
    }
    DD_EXPORT ~DynamicDLL() {}
};
_SCM_CHILD_DEFINITIONS(DynamicDLL)

#endif