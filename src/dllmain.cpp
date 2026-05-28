#include "mmtTlvDemuxer.h"
#include "bonDriverContext.h"
#include "acasHandler.h"

BonDriverContext g_bonDriverContext;

namespace {

HINSTANCE hModule = nullptr;

std::string getConfigFilePath(void* hModule) {
    char g_IniFilePath[_MAX_PATH];
    GetModuleFileNameA((HMODULE)hModule, g_IniFilePath, _MAX_PATH);

    char drive[_MAX_DRIVE];
    char dir[_MAX_DIR];
    char fname[_MAX_FNAME];
    _splitpath_s(g_IniFilePath, drive, sizeof(drive), dir, sizeof(dir), fname, sizeof(fname), NULL, NULL);
    snprintf(g_IniFilePath, sizeof(g_IniFilePath), "%s%s%s.ini", drive, dir, fname);

    return g_IniFilePath;
}

}

extern "C" __declspec(dllexport) IBonDriver* CreateBonDriver() {
    std::string path = getConfigFilePath(::hModule);
    try {
        config = loadConfig(path);
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return nullptr;
    }

    try {
        std::unique_ptr<AcasHandler> acasHandler = std::make_unique<AcasHandler>();
        std::unique_ptr<ISmartCard> smartCard;
        if (config.casProxyServer.empty()) {
            smartCard = std::make_unique<LocalSmartCard>();
        }
        else {
            auto parsed = casproxy::parseAddress(config.casProxyServer);
            if (!parsed) {
                std::cerr << "Invalid CasProxyServer address" << std::endl;
                return nullptr;
            }
            smartCard = std::make_unique<RemoteSmartCard>(parsed->first, parsed->second);
        }

        smartCard->setSmartCardReaderName(config.smartCardReaderName);
        acasHandler->setSmartCard(std::move(smartCard));
        g_bonDriverContext.demuxer.setCasHandler(std::move(acasHandler));
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return nullptr;
    }

    g_bonDriverContext.handler.setOutputCallback([&](const uint8_t* data, size_t size) {
        if (size == 188) {
            g_bonDriverContext.remuxOutput.insert(g_bonDriverContext.remuxOutput.end(), data, data + size);
        }
    });
    g_bonDriverContext.demuxer.setDemuxerHandler(g_bonDriverContext.handler);

    try {
        if (!g_bonDriverContext.bonTuner.init()) {
            std::cerr << "Failed to initialize BonTuner" << std::endl;
            return nullptr;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in bonTuner.init(): " << e.what() << std::endl;
        return nullptr;
    }
    catch (...) {
        std::cerr << "Unknown exception in bonTuner.init()" << std::endl;
        return nullptr;
    }

    return &g_bonDriverContext.bonTuner;
}


extern "C" __declspec(dllexport) BOOL WINAPI StartMmtsSave(const wchar_t* path, BOOL overwrite) {
    if (path == nullptr || path[0] == L'\0') {
        return FALSE;
    }

    if (!overwrite) {
        DWORD attr = GetFileAttributesW(path);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            return FALSE;
        }
    }

    g_bonDriverContext.demuxer.setDecodedDumpStream(nullptr);
    if (g_bonDriverContext.mmtsDumpFs) {
        g_bonDriverContext.mmtsDumpFs->close();
        g_bonDriverContext.mmtsDumpFs.reset();
    }

    g_bonDriverContext.mmtsDumpFs = std::make_unique<std::ofstream>(path, std::ios::binary);
    if (!g_bonDriverContext.mmtsDumpFs->is_open()) {
        g_bonDriverContext.mmtsDumpFs.reset();
        return FALSE;
    }

    if (config.decodeDump) {
        g_bonDriverContext.demuxer.setDecodedDumpStream(g_bonDriverContext.mmtsDumpFs.get());
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI StopMmtsSave() {
    g_bonDriverContext.demuxer.setDecodedDumpStream(nullptr);
    if (g_bonDriverContext.mmtsDumpFs) {
        g_bonDriverContext.mmtsDumpFs->close();
        g_bonDriverContext.mmtsDumpFs.reset();
    }
}

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        ::hModule = hModule;
        break;
    }

    return true;
}



