#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>

// ============================================================================
// The Collective's Slate Engine (2002) - Reconstructed Core C++ Types
// Derived directly from final.map MSVC 7.0 mangled signatures (XDK 4432)
// ============================================================================

// Verified 8-byte layout from xmain.asm (TSessionOverview +0..+8, ShellRelaunch movsx eax, word ptr [esi+4])
struct TString {
    char*   m_pData;        // +0x00
    int16_t m_nLength;      // +0x04
    int16_t m_nAllocLength; // +0x06

    // XBE VA: 0x00011020 (?SetLength@TString@@QAEAAU1@J@Z)
    TString& SetLength(int32_t len);
    // XBE VA: 0x000110A0 (??4TString@@QAEAAU0@ABU0@@Z)
    TString& operator=(const TString& rhs);
    // XBE VA: 0x00011130 (?Append@TString@@QAEAAU1@ABU1@@Z)
    TString& Append(const TString& rhs);
    // XBE VA: 0x000120A0 (?Mid@TString@@QBE?AU1@JJ@Z)
    TString  Mid(int32_t first, int32_t count) const;
    // XBE VA: 0x000120F0 (?Right@TString@@QBE?AU1@J@Z)
    TString  Right(int32_t count) const;
};

// XBE VA: 0x00011000 (?EqualsI@@YA_NABUTString@@0@Z)
bool EqualsI(const TString& a, const TString& b);

struct TFileName : public TString {};

template <typename T>
struct TArray {
    T*      m_pData;
    int32_t m_nLength;
    int32_t m_nAllocLength;

    void SetAllocLength(int32_t n) {
        m_nAllocLength = n;
    }
};

template <typename T>
struct T_Vector2 {
    T x, y;
};

template <typename T>
struct T_Vector {
    T x, y, z;
};

template <typename T>
struct T_Vector4 {
    T x, y, z, w;
};

template <typename T>
struct T_Quaternion {
    T x, y, z, w;
};

template <typename T>
struct T_Matrix4 {
    T m[4][4];
};

struct TRectangle {
    int32_t left, top, right, bottom;
};

struct TRGBA;
struct TColor {
    uint32_t argb;
    // XBE VA: 0x000112C0 (?Set@TColor@@QAEXABUTRGBA@@@Z)
    void Set(const TRGBA& rgba);
};

struct TRGBA {
    float r, g, b, a;
    // XBE VA: 0x00011260 (??0TRGBA@@QAE@ABUTColor@@@Z)
    TRGBA(const TColor& c);
};

// ============================================================================
// File & Serialization Subsystem (StdFile.obj / pcStdFile.obj)
// ============================================================================

struct TFileInterface {
    virtual ~TFileInterface() = default;
    // XBE VA: 0x00011190 (?Tag@TFileInterface@@UAEEPBD@Z)
    virtual uint8_t Tag(const char* name);
    // XBE VA: 0x00011220 (?Tag@TFileInterface@@UAE_NPBDE@Z)
    virtual bool    Tag(const char* name, uint8_t val);
    virtual bool    Open() = 0;
};

struct TFileRead : public TFileInterface {
    // XBE VA: 0x00011170 (??0TFileRead@@QAE@ABUTFileName@@@Z)
    explicit TFileRead(const TFileName& fileName);
    // XBE VA: 0x00011250 (?Open@TFileRead@@UAE_NXZ)
    bool Open() override;
};

struct TArchive {
    virtual ~TArchive() = default;
    virtual void Flush() = 0;
    virtual void LabelEnd() = 0;
    virtual void MarkPushLocation() = 0;
    virtual void MarkPopLocation() = 0;
};

// ============================================================================
// Console & CVar Subsystem (console.obj)
// ============================================================================

struct TConsoleWindow {
    // XBE VA: 0x00011350 (?SetExtent@TConsoleWindow@@UAEXJJ@Z)
    virtual void SetExtent(int32_t w, int32_t h);
};

struct TConsole {
    // XBE VA: 0x00011460 (?ScriptShutdown@TConsole@@UAEXXZ)
    virtual void ScriptShutdown();
    // XBE VA: 0x00011470 (?GetActive@TConsole@@UAE_NXZ)
    virtual bool GetActive();
    // XBE VA: 0x00011480 (?SetActive@TConsole@@UAEX_N0@Z)
    virtual void SetActive(bool active, bool notify);
    // XBE VA: 0x00011900 (?printf@TConsole@@UAEXABUTString@@@Z)
    virtual void printf(const TString& msg);
    // XBE VA: 0x000125E0 (?ExecFile@TConsole@@UAE_NPAD@Z)
    virtual bool ExecFile(char* filename);
    // XBE VA: 0x00012B70 (?RenderWindows@TConsole@@UAEXXZ)
    virtual void RenderWindows();
};

// ============================================================================
// Game Object & Character Hierarchy (IObject -> IInstance -> IEntity -> ICharacter -> ISlayer)
// ============================================================================

struct TScriptDef;
struct TScrSequence;

struct IObject {
    virtual ~IObject() = default;
};

struct IInstance : public IObject {
    static uint32_t sm_nSceneArchiveVersion; // XBE VA: 0x007D09CC (000b:0007fb2c)
    virtual uint32_t    GetScriptID() = 0;
    virtual TScriptDef* GetScriptDef() = 0;
    virtual uint32_t    GetScriptSize() = 0;
    virtual const char* GetScriptName() const = 0;
    virtual bool        IsEditorType() = 0;
};

struct IEntity : public IInstance {};

struct ICharacter : public IEntity {};

struct ISlayer : public ICharacter {
    // XBE VA: 0x0020E0A0 (??0ISlayer@@QAE@XZ)
    ISlayer();
    // XBE VA: 0x0020E1C0 (?GetScriptID@ISlayer@@UAEKXZ)
    uint32_t    GetScriptID() override;
    TScriptDef* GetScriptDef() override;
    // XBE VA: 0x0020E1D0 (?GetScriptSize@ISlayer@@UAEKXZ)
    uint32_t    GetScriptSize() override;
    // XBE VA: 0x0020E1E0 (?GetScriptName@ISlayer@@UBEPBDXZ)
    const char* GetScriptName() const override;
    // XBE VA: 0x00117B40 (?IsEditorType@ISlayer@@UAE_NXZ)
    bool        IsEditorType() override;

    // XBE VA: 0x0020E1F0 (?Event_KilledEnemy@ISlayer@@UAEXPAUICharacter@@@Z)
    virtual void    Event_KilledEnemy(ICharacter* enemy);
    // XBE VA: 0x0020E220 (?AutoKnockDownCylinder@ISlayer@@UAEXPBUTScrSequence@@@Z)
    virtual void    AutoKnockDownCylinder(const TScrSequence* seq);
    // XBE VA: 0x0020E250 (?GetExpressionIndexFromAnimationCategory@ISlayer@@UAEJXZ)
    virtual int32_t GetExpressionIndexFromAnimationCategory();
    // XBE VA: 0x0020E2C0 (?Power_Change@ISlayer@@UAEXM@Z)
    virtual void    Power_Change(float delta);
    // XBE VA: 0x0020E300 (?InformEnteredSequence@ISlayer@@UAEXPBUTScrSequence@@@Z)
    virtual void    InformEnteredSequence(const TScrSequence* seq);
};

// ============================================================================
// Platform Shell & Installer Entry Points (xmain.obj / xinstall.obj)
// ============================================================================

void LogPrintf(const char* fmt, ...);

// XBE VA: 0x00062B90 (?QueryVersionData@@YA_NAAK0@Z)
bool QueryVersionData(uint32_t& outVerA, uint32_t& outVerB);
// XBE VA: 0x00063250 (?RunInstaller@@YAX_N@Z) & 0x00062830 (?FileCopyThreadProc@TLogo@@SGKPAX@Z)
void RunInstaller(bool forceInstall);
// XBE VA: 0x000638E0 (?ShellQueryBaseMemory@@YAJXZ)
int32_t ShellQueryBaseMemory();
// XBE VA: 0x000638F0 (?ShellGetMemory@@YA_NAAJ0@Z)
bool ShellGetMemory(int32_t& outFree, int32_t& outTotal);
