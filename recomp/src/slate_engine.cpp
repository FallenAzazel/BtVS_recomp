#include "slate_engine.h"
#include "xboxkrnl_win32.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

uint32_t IInstance::sm_nSceneArchiveVersion = 0;

bool EqualsI(const TString& a, const TString& b) {
    if (a.m_nLength != b.m_nLength) return false;
    if (!a.m_pData || !b.m_pData) return a.m_pData == b.m_pData;
    return _strnicmp(a.m_pData, b.m_pData, a.m_nLength) == 0;
}

TString& TString::SetLength(int32_t len) {
    if (len + 1 > m_nAllocLength) {
        m_nAllocLength = static_cast<int16_t>(len + 1);
        m_pData = static_cast<char*>(std::realloc(m_pData, m_nAllocLength));
    }
    m_nLength = static_cast<int16_t>(len);
    if (m_pData) m_pData[len] = '\0';
    return *this;
}

TString& TString::operator=(const TString& rhs) {
    if (this == &rhs) return *this;
    SetLength(rhs.m_nLength);
    if (m_pData && rhs.m_pData) {
        std::memcpy(m_pData, rhs.m_pData, rhs.m_nLength + 1);
    }
    return *this;
}

TString& TString::Append(const TString& rhs) {
    int32_t oldLen = m_nLength;
    SetLength(oldLen + rhs.m_nLength);
    if (m_pData && rhs.m_pData) {
        std::memcpy(m_pData + oldLen, rhs.m_pData, rhs.m_nLength + 1);
    }
    return *this;
}

TString TString::Mid(int32_t first, int32_t count) const {
    TString res{nullptr, 0, 0};
    if (first < 0 || first >= m_nLength || count <= 0) return res;
    int32_t actual = std::min(count, m_nLength - first);
    res.SetLength(actual);
    std::memcpy(res.m_pData, m_pData + first, actual);
    res.m_pData[actual] = '\0';
    return res;
}

TString TString::Right(int32_t count) const {
    if (count >= m_nLength) return Mid(0, m_nLength);
    return Mid(m_nLength - count, count);
}

TRGBA::TRGBA(const TColor& c) {
    constexpr float inv255 = 1.0f / 255.0f;
    a = ((c.argb >> 24) & 0xFF) * inv255;
    r = ((c.argb >> 16) & 0xFF) * inv255;
    g = ((c.argb >> 8)  & 0xFF) * inv255;
    b = (c.argb & 0xFF) * inv255;
}

void TColor::Set(const TRGBA& rgba) {
    auto clamp8 = [](float v) -> uint32_t {
        int32_t iv = static_cast<int32_t>(v * 255.0f);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        return static_cast<uint32_t>(iv);
    };
    argb = (clamp8(rgba.a) << 24) | (clamp8(rgba.r) << 16) | (clamp8(rgba.g) << 8) | clamp8(rgba.b);
}

uint8_t TFileInterface::Tag(const char* /*name*/) { return 0; }
bool    TFileInterface::Tag(const char* /*name*/, uint8_t /*val*/) { return true; }

TFileRead::TFileRead(const TFileName& /*fileName*/) {}
bool TFileRead::Open() { return true; }

void TConsoleWindow::SetExtent(int32_t /*w*/, int32_t /*h*/) {}
void TConsole::ScriptShutdown() {}
bool TConsole::GetActive() { return false; }
void TConsole::SetActive(bool /*active*/, bool /*notify*/) {}
void TConsole::printf(const TString& msg) {
    if (msg.m_pData) std::printf("[TConsole] %s\n", msg.m_pData);
}
bool TConsole::ExecFile(char* /*filename*/) { return true; }
void TConsole::RenderWindows() {}

// xmain.obj: ?ShellQueryBaseMemory@@YAJXZ (0x000638E0)
int32_t ShellQueryBaseMemory() {
    xboxkrnl::MM_STATISTICS stats{};
    stats.Length = sizeof(stats);
    xboxkrnl::Shim_MmQueryStatistics(&stats);
    return static_cast<int32_t>(stats.TotalPhysicalPages * 4096);
}

// xmain.obj: ?ShellGetMemory@@YA_NAAJ0@Z (0x000638F0)
bool ShellGetMemory(int32_t& outFree, int32_t& outTotal) {
    xboxkrnl::MM_STATISTICS stats{};
    stats.Length = sizeof(stats);
    xboxkrnl::Shim_MmQueryStatistics(&stats);
    outFree  = static_cast<int32_t>(stats.AvailablePages * 4096);
    outTotal = static_cast<int32_t>(stats.TotalPhysicalPages * 4096);
    return true;
}

// xinstall.obj: ?QueryVersionData@@YA_NAAK0@Z (0x00062B90)
bool QueryVersionData(uint32_t& outVerA, uint32_t& outVerB) {
    outVerA = 0x3D35C92D; // PE TimeDateStamp from final.map
    outVerB = 4432;       // XDK Build 4432
    return true;
}

void RunInstaller(bool forceInstall) {
    char installDatPath[512]{};
    xboxkrnl::TranslateXboxPath("D:\\install.dat", installDatPath, sizeof(installDatPath));

    FILE* f = std::fopen(installDatPath, "rb");
    if (!f) {
        LogPrintf("[xinstall] WARNING: Could not open '%s'\n", installDatPath);
        return;
    }

    // Decompiled from TLogo::FileCopyThreadProc (xinstall.obj @ 0x00062830):
    // 0x1000-byte header: uint32_t numFiles at +0x00, followed by 64-byte entries at +0x40
    uint8_t hdr[0x1000]{};
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        std::fclose(f);
        return;
    }

    uint32_t numFiles = *reinterpret_cast<uint32_t*>(&hdr[0]);
    LogPrintf("[xinstall] RunInstaller(force=%d): Parsing D:\\install.dat (%u cached files)...\n",
              forceInstall ? 1 : 0, numFiles);

    uint64_t dataOffset = 0x1000;
    uint32_t extractedCount = 0;

    for (uint32_t i = 0; i < numFiles && (0x40 + (i + 1) * 0x40) <= sizeof(hdr); ++i) {
        const uint8_t* entry = &hdr[0x40 + i * 0x40];
        uint32_t actualSize  = *reinterpret_cast<const uint32_t*>(&entry[0]);
        uint32_t paddedSize  = *reinterpret_cast<const uint32_t*>(&entry[4]);
        char relName[57]{};
        std::memcpy(relName, &entry[8], 56);
        relName[56] = '\0';

        // Extract ALL cached files from D:\install.dat into Z:\ (recomp/cache/) if missing or wrong size
        std::string xboxDest = std::string("Z:\\") + relName;
        char hostDest[512]{};
        xboxkrnl::TranslateXboxPath(xboxDest.c_str(), hostDest, sizeof(hostDest));

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        bool alreadyPresent = !forceInstall &&
                              GetFileAttributesExA(hostDest, GetFileExInfoStandard, &fad) &&
                              fad.nFileSizeHigh == 0 && fad.nFileSizeLow == actualSize;

        if (!alreadyPresent) {
            // Ensure parent directory exists
            char dirBuf[512]{};
            std::strncpy(dirBuf, hostDest, sizeof(dirBuf) - 1);
            for (char* p = dirBuf; *p; ++p) {
                if (*p == '/' || *p == '\\') {
                    char saved = *p;
                    *p = '\0';
                    CreateDirectoryA(dirBuf, nullptr);
                    *p = saved;
                }
            }

            FILE* outF = std::fopen(hostDest, "wb");
            if (outF) {
                std::fseek(f, static_cast<long>(dataOffset), SEEK_SET);
                std::vector<uint8_t> buf(actualSize);
                if (actualSize > 0 && std::fread(buf.data(), 1, actualSize, f) == actualSize) {
                    std::fwrite(buf.data(), 1, actualSize, outF);
                }
                std::fclose(outF);
                ++extractedCount;
                LogPrintf("  [Z:\\%s] Unpacked (%u bytes -> %s)\n", relName, actualSize, hostDest);
            }
        } else {
            LogPrintf("  [Z:\\%s] Cached OK (%u bytes)\n", relName, actualSize);
        }

        dataOffset += paddedSize;
    }

    std::fclose(f);
    LogPrintf("[xinstall] Completed install.dat pass (%u files unpacked to Z:\\ cache).\n", extractedCount);
}

ISlayer::ISlayer() = default;
uint32_t    ISlayer::GetScriptID() { return 0x534C4159; }
TScriptDef* ISlayer::GetScriptDef() { return nullptr; }
uint32_t    ISlayer::GetScriptSize() { return sizeof(ISlayer); }
const char* ISlayer::GetScriptName() const { return "Slayer"; }
bool        ISlayer::IsEditorType() { return false; }
void        ISlayer::Event_KilledEnemy(ICharacter* /*enemy*/) {}
void        ISlayer::AutoKnockDownCylinder(const TScrSequence* /*seq*/) {}
int32_t     ISlayer::GetExpressionIndexFromAnimationCategory() { return 0; }
void        ISlayer::Power_Change(float /*delta*/) {}
void        ISlayer::InformEnteredSequence(const TScrSequence* /*seq*/) {}
