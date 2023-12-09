/*
*   BSD 3-Clause License, see file labled 'LICENSE' for the full License.
*   Copyright (c) 2023, Peter Ferranti
*   All rights reserved.
*/

#ifndef DYNAMIC_DLL_
#define DYNAMIC_DLL_

#include "vendor/FileUtilities/vendor/STDExtras/vendor/ThreadPool/vendor/Semaphore/semaphore.hpp"
#include "vendor/FileUtilities/vendor/STDExtras/STDExtras.hpp"
#include "vendor/FileUtilities/FileUtilities.hpp"
extern "C" {
    #include "vendor/libelfmaster/include/libelfmaster.h"
}
#include <thread>

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
    #define DD_IMPORT
    typedef void* DLLModule;
    #define OPEN_LIB_INIT_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_DEEPBIND | (((m_flags & ReloadFlag::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_GLOBAL : RTLD_GLOBAL)))
    #define OPEN_LIB_BY_CSTR(str) dlopen(str, RTLD_NOW | RTLD_DEEPBIND | (((m_flags & ReloadFlag::HOT_RELOADABLE) ? RTLD_NODELETE | RTLD_LOCAL : RTLD_LOCAL)))
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

class DynamicDLL {
     public:
    enum ReloadFlag : bool {
        COLD_RELOADABLE = false,
        HOT_RELOADABLE = true
    };
    
    private:    
    DLLModule m_dll{0};
    Semaphore m_sem{0};
    std::mutex m_mtx;
    std::vector<std::string> m_dynSymTab;
    std::unordered_map<std::string, void*> m_addressCache;
    std::vector<FileUtilities::ParsedPath> m_libLoadNames;
    bool m_isInitLoaded{false};
    bool m_isReloading{false};
    ReloadFlag m_flags;
    inline static std::unordered_map<std::string, DynamicDLL*> _Instances{};
    inline static std::mutex _SMTX{};
    
    void GenDynSymTab(const std::string& path) {
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
    
    void Unload(bool waitForAFullHalt = false) {
        using namespace std;
        using namespace this_thread;
        m_sem.spinAll();
        if(waitForAFullHalt) {
            m_sem.waitForInit();
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
    
    const bool Load() {
        for(auto& lib : m_libLoadNames) {
            const std::string currentLib{lib};
            m_dll = (m_isInitLoaded ? OPEN_LIB_INIT_BY_CSTR(currentLib.c_str()) : OPEN_LIB_BY_CSTR(currentLib.c_str()));
            if(m_dll) {
                GenDynSymTab(currentLib);
                m_isInitLoaded = true;
                return true;
            }
        }
        m_dll = 0;
        return false;
    }
    
    bool isSymAddressCached(const std::string key) {
        return ((m_addressCache.find(key) != m_addressCache.end()) && !m_addressCache.empty());
    }
    
    void Reload(bool waitForAFullHalt = false) {
        if(!m_isInitLoaded || m_isReloading) return;
        m_isReloading = true;
        Unload(waitForAFullHalt);
        Load();
        m_isReloading = false;
        m_sem.spinAll();
    }
    
    void* GetSymAddress(const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false) {
        using namespace std;
        if(!m_isInitLoaded) return 0;
        m_sem.waitFor([&](int64_t, int64_t){return !(this->m_isReloading); });
        lock_guard<mutex> lock(m_mtx);
        std::string lookupName{needsDemangle ? std::move(abi::__cxa_demangle(symname.c_str(), NULL, NULL, NULL)) : symname};
        try {
            std::for_each(m_dynSymTab.begin(), m_dynSymTab.end(), [&](const std::string& s) {
                const std::string cmp = (cmpAsMangled ? s : abi::__cxa_demangle(s.c_str(), NULL, NULL, NULL));
               if(findStringSimilarity(lookupName, cmp) > 0.7) {
                    lookupName = s;
                    throw;
                }
            });
        }
        catch(...) {}
        if(!isSymAddressCached(lookupName)) {
            m_addressCache[lookupName] = (void*)GET_SYM_BY_STR(m_dll, lookupName.c_str());
        }
        return m_addressCache[lookupName];
    }
    
    DynamicDLL(std::vector<FileUtilities::ParsedPath> libloadnames, ReloadFlag flags) :
        m_libLoadNames(libloadnames),
        m_flags(flags) {
        if(!Load())
            std::cout << m_libLoadNames[0].getPath(FileUtilities::PathType::NameOnly) << ": DynamicDLL Load Failed!" << std::endl;
    }
    
    DynamicDLL() {}
    ~DynamicDLL() {}
    
    public:
    static const bool InstanceExists(const std::string key) {
        return ((_Instances.find(key) != _Instances.end()) && !_Instances.empty());
    }
    
    static DynamicDLL& BeginInstance(const std::string key, std::vector<FileUtilities::ParsedPath> libloadnames, ReloadFlag flags = ReloadFlag::COLD_RELOADABLE) {
        std::ThreadLock lock(_SMTX);
        if(InstanceExists(key))
            return *(_Instances[key]);
        std::cout << "here" << std::endl;
        return *(_Instances[key] = std::move(new DynamicDLL(libloadnames, flags)));
    }
    
    static DynamicDLL& GetInstance(const std::string key) {
        if(InstanceExists(key))
            return *(_Instances[key]);
        return *(_Instances[key] = std::move(new DynamicDLL()));
    }    
    
    template<typename T, typename ... Args>
    std::invoke_result_t<T, Args...> CallSym(const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false, Args... args) {
        return (*(T*)GetSymAddress(symname, needsDemangle, cmpAsMangled))(args...);
    }
    
    template<typename T>
    T& GetSym(const std::string symname, bool needsDemangle = false,  bool cmpAsMangled = false) {
        return *((T*)GetSymAddress(symname, needsDemangle, cmpAsMangled));
    }
};


#endif