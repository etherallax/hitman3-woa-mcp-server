#pragma once

#include "IPluginInterface.h"
#include "HttpServer.h"

#include <Glacier/ZEntity.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZResource.h>
#include <Glacier/ZMath.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ZRepositoryID;
class ZTemplateEntityFactory;
struct SGameUpdateEvent;
class ZEntitySceneContext;
class IFirearm;

class MCPBridge : public IPluginInterface {
public:
    ~MCPBridge() override;

    void Init() override;
    void OnEngineInitialized() override;
    void OnDrawUI(bool p_HasFocus) override;
    void OnDrawMenu() override;

private:
    void OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent);

    // HTTP
    std::pair<int, std::string> HandleRequest(
        const std::string& p_Method, const std::string& p_Path, const std::string& p_Query,
        const std::string& p_Body
    );
    std::string DispatchOnGameThread(const std::string& p_Body);

    // Game-thread command implementations. All return JSON strings.
    std::string ExecuteCommand(const std::string& p_Body);
    std::string CmdTeleport(double p_X, double p_Y, double p_Z, bool p_Relative, bool p_Snap);
    std::string CmdForward(double p_Distance);
    std::string CmdSpawn(const std::string& p_RepoId, bool p_ToInventory);
    std::string CmdItems(const std::string& p_Filter, int64_t p_Limit);
    std::string CmdCheat(const std::string& p_Name, bool p_On);
    std::string CmdTimescale(double p_Value);

    // Cheat helpers (game thread only).
    bool EnsureAICrippleEntity();
    bool EnsureCrippleBoxEntity();

    // Raycast down from above the target to find the floor. Game thread only.
    bool SnapToGround(float4& p_Pos);

    void LoadRepositoryProps();

    static std::string JsonError(const std::string& p_Message);
    static std::string JsonEscape(const std::string& p_Str);

    DECLARE_PLUGIN_DETOUR(MCPBridge, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene);
    DECLARE_PLUGIN_DETOUR(MCPBridge, void, ZHM5ItemWeapon_SetBulletsInMagazine, IFirearm* th, int32_t nBullets);

private:
    HttpServer m_Server;

    // Set in the destructor (runs on the game thread during unload/reload).
    // Makes in-flight dispatches bail immediately instead of waiting for a
    // game thread that is blocked joining the HTTP thread.
    std::atomic<bool> m_Unloading{false};

    // Command queue: HTTP thread -> game thread.
    std::mutex m_QueueMutex;
    std::condition_variable m_QueueCv;
    std::deque<std::pair<uint64_t, std::string>> m_Pending;
    std::unordered_map<uint64_t, std::string> m_Results;
    std::condition_variable m_ResultCv;
    std::atomic<uint64_t> m_NextId{1};

    // State snapshot written on the game thread, read by HTTP threads.
    std::mutex m_StateMutex;
    std::string m_StateJson{"{\"in_game\":false}"};

    // Overlay messages.
    struct Message {
        std::string Text;
        std::chrono::steady_clock::time_point Expires;
    };
    std::vector<Message> m_Messages;

    // Cheat state.
    ZEntityRef m_AICrippleEntity;
    ZEntityRef m_HM5CrippleBoxEntity;
    bool m_Invincible = false;
    bool m_Invisible = false;
    bool m_InfiniteAmmo = false;
    bool m_NoReload = false;

    // Repository prop cache.
    bool m_RepoLoaded = false;
    TResourcePtr<ZTemplateEntityFactory> m_RepositoryResource;
    std::vector<std::pair<std::string, std::string>> m_RepoProps; // id, display name

    uint64_t m_FrameCount = 0;
    int64_t m_Port = 47847;
    std::chrono::steady_clock::time_point m_LastTick{};
};

DECLARE_ZHM_PLUGIN(MCPBridge)
