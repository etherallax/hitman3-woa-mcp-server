#include "MCPBridge.h"

#include "Hooks.h"
#include "Logging.h"

#include <Glacier/ZGameLoopManager.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/SGameUpdateEvent.h>
#include <Glacier/ZSpatialEntity.h>
#include <Glacier/ZHM5CrippleBox.h>
#include <Glacier/ZModule.h>
#include <Glacier/ZItem.h>
#include <Glacier/ZInventory.h>
#include <Glacier/ZActor.h>
#include <Glacier/ZAction.h>
#include <Glacier/ZGameTime.h>
#include <Glacier/ZPrimitives.h>
#include <Glacier/SExternalReferences.h>
#include <Glacier/EntityFactory.h>
#include <Glacier/ZEntityManager.h>
#include <Glacier/CompileReflection.h>
#include <Glacier/ZCollision.h>

#include <simdjson.h>
#include <algorithm>
#include <chrono>
#include <format>

using namespace std::chrono_literals;

static constexpr const char* c_Version = "1.0.0";

MCPBridge::~MCPBridge() {
    m_Unloading = true;
    m_ResultCv.notify_all();

    // The GameLoopManager delegate must be removed before we go away —
    // OnModUnloading doesn't do it for us, and a dangling frame update calls
    // into freed code.
    if (Globals::GameLoopManager) {
        const ZMemberDelegate<MCPBridge, void(const SGameUpdateEvent&)> s_Delegate(
            this, &MCPBridge::OnFrameUpdate
        );
        Globals::GameLoopManager->UnregisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
    }

    m_Server.Stop();
}

void MCPBridge::Init() {
    Hooks::ZEntitySceneContext_ClearScene->AddDetour(this, &MCPBridge::OnClearScene);
    Hooks::ZHM5ItemWeapon_SetBulletsInMagazine->AddDetour(
        this, &MCPBridge::ZHM5ItemWeapon_SetBulletsInMagazine
    );

    // mods/mcpbridge.ini → [server] port = 47847
    m_Port = GetSettingInt("server", "port", 47847);
    if (m_Port < 1024 || m_Port > 65535) {
        Logger::Warn("MCPBridge: invalid port {} in mcpbridge.ini, using 47847", m_Port);
        m_Port = 47847;
    }

    m_Server.Start(static_cast<uint16_t>(m_Port), [this](auto&&... a) { return HandleRequest(a...); });
}

void MCPBridge::OnEngineInitialized() {
    const ZMemberDelegate<MCPBridge, void(const SGameUpdateEvent&)> s_Delegate(
        this, &MCPBridge::OnFrameUpdate
    );
    Globals::GameLoopManager->RegisterFrameUpdate(s_Delegate, 1, EUpdateMode::eUpdatePlayMode);
}

// ---------- HTTP handling (HTTP thread) ----------

std::pair<int, std::string> MCPBridge::HandleRequest(
    const std::string& p_Method, const std::string& p_Path, const std::string& p_Query,
    const std::string& p_Body
) {
    if (p_Path == "/state" && p_Method == "GET") {
        std::string s_Snap;
        {
            std::scoped_lock s_Lock(m_StateMutex);
            s_Snap = m_StateJson;
        }

        // Age of the snapshot: grows whenever the game thread stops ticking
        // (pause menu, console open, focus loss), which is also when queued
        // commands stop draining.
        const auto s_Age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_LastTick
        ).count();

        if (!s_Snap.empty() && s_Snap.back() == '}')
            s_Snap.pop_back();
        return {200, std::format("{},\"stale_ms\":{},\"mod_version\":\"{}\"}}", s_Snap, s_Age, c_Version)};
    }

    if (p_Path == "/cmd" && p_Method == "POST")
        return {200, DispatchOnGameThread(p_Body)};

    // Convenience: GET /items?filter=x also works (queued like a command).
    if (p_Path == "/items" && p_Method == "GET") {
        std::string s_Filter = p_Query;
        if (s_Filter.starts_with("filter="))
            s_Filter = s_Filter.substr(7);
        return {200, DispatchOnGameThread(std::format("{{\"cmd\":\"items\",\"filter\":\"{}\"}}", s_Filter))};
    }

    return {404, R"({"ok":false,"error":"unknown endpoint"})"};
}

std::string MCPBridge::DispatchOnGameThread(const std::string& p_Body) {
    if (m_Unloading)
        return JsonError("mod is unloading");

    uint64_t s_Id = m_NextId++;
    {
        std::scoped_lock s_Lock(m_QueueMutex);
        m_Pending.emplace_back(s_Id, p_Body);
    }

    // Wait for the game thread to produce a result.
    std::unique_lock s_Lock(m_QueueMutex);
    if (!m_ResultCv.wait_for(s_Lock, 10s, [&] { return m_Results.contains(s_Id) || m_Unloading.load(); }))
        return JsonError(m_Unloading ? "mod is unloading" : "timeout waiting for game thread");

    std::string s_Result = std::move(m_Results[s_Id]);
    m_Results.erase(s_Id);
    return s_Result;
}

// ---------- Game thread ----------

void MCPBridge::OnFrameUpdate(const SGameUpdateEvent& p_UpdateEvent) {
    ++m_FrameCount;
    m_LastTick = std::chrono::steady_clock::now();

    // Refresh the state snapshot.
    {
        std::string s_Json;
        if (auto s_Hitman = SDK()->GetLocalPlayer()) {
            if (const auto s_Spatial = s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>()) {
                const auto s_Matrix = s_Spatial->GetObjectToWorldMatrix();
                const float s_Timescale =
                    Globals::GameTimeManager ? Globals::GameTimeManager->m_fGameTimeMultiplier : 1.f;
                s_Json = std::format(
                    "{{\"in_game\":true,\"pos\":[{:g},{:g},{:g}],"
                    "\"cheats\":{{\"invincible\":{},\"invisible\":{},\"infiniteammo\":{},\"noreload\":{}}},"
                    "\"timescale\":{:g},\"frame\":{}}}",
                    s_Matrix.Trans.x, s_Matrix.Trans.y, s_Matrix.Trans.z,
                    m_Invincible, m_Invisible, m_InfiniteAmmo, m_NoReload,
                    s_Timescale, m_FrameCount
                );
            }
        }
        if (s_Json.empty())
            s_Json = std::format("{{\"in_game\":false,\"frame\":{}}}", m_FrameCount);

        std::scoped_lock s_Lock(m_StateMutex);
        m_StateJson = std::move(s_Json);
    }

    // Drain pending commands.
    for (;;) {
        std::pair<uint64_t, std::string> s_Cmd;
        {
            std::scoped_lock s_Lock(m_QueueMutex);
            if (m_Pending.empty())
                break;
            s_Cmd = std::move(m_Pending.front());
            m_Pending.pop_front();
        }

        std::string s_Result = ExecuteCommand(s_Cmd.second);

        {
            std::scoped_lock s_Lock(m_QueueMutex);
            m_Results.emplace(s_Cmd.first, std::move(s_Result));
        }
        m_ResultCv.notify_all();
    }

    // Expire overlay messages.
    const auto s_Now = std::chrono::steady_clock::now();
    std::erase_if(m_Messages, [&](const Message& p_M) { return p_M.Expires < s_Now; });
}

std::string MCPBridge::ExecuteCommand(const std::string& p_Body) {
    simdjson::ondemand::parser s_Parser;
    simdjson::padded_string s_Json(p_Body);
    auto s_Doc = s_Parser.iterate(s_Json);
    if (s_Doc.error())
        return JsonError("invalid json");

    std::string_view s_Cmd;
    if (s_Doc["cmd"].get_string().get(s_Cmd) != simdjson::SUCCESS)
        return JsonError("missing cmd");

    auto s_Num = [&](const char* p_Key, double p_Default = 0.0) {
        double s_V;
        return s_Doc[p_Key].get_double().get(s_V) == simdjson::SUCCESS ? s_V : p_Default;
    };
    auto s_Str = [&](const char* p_Key) -> std::string {
        std::string_view s_V;
        return s_Doc[p_Key].get_string().get(s_V) == simdjson::SUCCESS ? std::string(s_V) : "";
    };
    auto s_Bool = [&](const char* p_Key) {
        bool s_V;
        return s_Doc[p_Key].get_bool().get(s_V) == simdjson::SUCCESS ? s_V : false;
    };

    if (s_Cmd == "ping")
        return R"({"ok":true,"pong":true})";

    if (s_Cmd == "teleport") {
        bool s_Snap = true;
        bool s_SnapV;
        if (s_Doc["snap"].get_bool().get(s_SnapV) == simdjson::SUCCESS)
            s_Snap = s_SnapV;
        return CmdTeleport(s_Num("x"), s_Num("y"), s_Num("z"), s_Bool("relative"), s_Snap);
    }

    if (s_Cmd == "forward")
        return CmdForward(s_Num("distance"));

    if (s_Cmd == "spawn")
        return CmdSpawn(s_Str("id"), s_Str("where") != "world");

    if (s_Cmd == "items")
        return CmdItems(s_Str("filter"), static_cast<int64_t>(s_Num("limit", 50)));

    if (s_Cmd == "cheat")
        return CmdCheat(s_Str("name"), s_Bool("on"));

    if (s_Cmd == "timescale")
        return CmdTimescale(s_Num("value", 1.0));

    if (s_Cmd == "say") {
        double s_Dur = s_Num("duration", 5.0);
        m_Messages.push_back({s_Str("text"), std::chrono::steady_clock::now() + std::chrono::duration_cast<
                                     std::chrono::steady_clock::duration>(std::chrono::duration<double>(s_Dur))});
        return R"({"ok":true})";
    }

    return JsonError(std::format("unknown cmd '{}'", std::string(s_Cmd)));
}

std::string MCPBridge::CmdTeleport(double p_X, double p_Y, double p_Z, bool p_Relative, bool p_Snap) {
    auto s_Hitman = SDK()->GetLocalPlayer();
    if (!s_Hitman)
        return JsonError("no local player");

    const auto s_Spatial = s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>();
    if (!s_Spatial)
        return JsonError("no spatial entity");

    SMatrix s_Matrix = s_Spatial->GetObjectToWorldMatrix();
    if (p_Relative) {
        s_Matrix.Trans.x += static_cast<float>(p_X);
        s_Matrix.Trans.y += static_cast<float>(p_Y);
        s_Matrix.Trans.z += static_cast<float>(p_Z);
    }
    else {
        s_Matrix.Trans.x = static_cast<float>(p_X);
        s_Matrix.Trans.y = static_cast<float>(p_Y);
        s_Matrix.Trans.z = static_cast<float>(p_Z);
    }

    if (p_Snap && !SnapToGround(s_Matrix.Trans))
        return JsonError("no ground beneath target (use snap:false to force)");

    s_Spatial->SetObjectToWorldMatrixFromEditor(s_Matrix);

    return std::format(
        "{{\"ok\":true,\"pos\":[{:g},{:g},{:g}]}}",
        s_Matrix.Trans.x, s_Matrix.Trans.y, s_Matrix.Trans.z
    );
}

bool MCPBridge::SnapToGround(float4& p_Pos) {
    if (!*Globals::CollisionManager)
        return false;

    // Cast from ~2m above the target straight down; land on the first surface.
    ZRayQueryInput s_RayInput {
        .m_vFrom = float4(p_Pos.x, p_Pos.y + 2.f, p_Pos.z, 1.f),
        .m_vTo = float4(p_Pos.x, p_Pos.y - 50.f, p_Pos.z, 1.f),
    };

    ZRayQueryOutput s_RayOutput {};
    if (!(*Globals::CollisionManager)->RayCastClosestHit(s_RayInput, &s_RayOutput))
        return false;

    p_Pos.y = s_RayOutput.m_vPosition.y;
    return true;
}

std::string MCPBridge::CmdForward(double p_Distance) {
    auto s_Hitman = SDK()->GetLocalPlayer();
    if (!s_Hitman)
        return JsonError("no local player");

    const auto s_Spatial = s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>();
    if (!s_Spatial)
        return JsonError("no spatial entity");

    SMatrix s_Matrix = s_Spatial->GetObjectToWorldMatrix();
    // ZAxis is the backward vector in Glacier space.
    s_Matrix.Trans.x -= s_Matrix.ZAxis.x * static_cast<float>(p_Distance);
    s_Matrix.Trans.z -= s_Matrix.ZAxis.z * static_cast<float>(p_Distance);
    s_Spatial->SetObjectToWorldMatrixFromEditor(s_Matrix);

    return std::format(
        "{{\"ok\":true,\"pos\":[{:g},{:g},{:g}]}}",
        s_Matrix.Trans.x, s_Matrix.Trans.y, s_Matrix.Trans.z
    );
}

std::string MCPBridge::CmdCheat(const std::string& p_Name, bool p_On) {
    if (p_Name == "infiniteammo") {
        if (p_On) {
            if (!EnsureCrippleBoxEntity())
                return JsonError("cripple box unavailable (not in a scene?)");
            auto s_Hitman = SDK()->GetLocalPlayer();
            if (!s_Hitman)
                return JsonError("no local player");
            auto* s_CrippleBox = m_HM5CrippleBoxEntity.QueryInterface<ZHM5CrippleBox>();
            s_CrippleBox->m_bActivateOnStart = true;
            s_CrippleBox->m_rHitmanCharacter = s_Hitman;
            s_CrippleBox->m_bLimitedAmmo = false;
            s_CrippleBox->Activate(0);
        }
        else if (m_HM5CrippleBoxEntity) {
            Functions::ZEntityManager_DeleteEntity->Call(
                Globals::EntityManager, m_HM5CrippleBoxEntity, {}
            );
            m_HM5CrippleBoxEntity = {};
        }
        m_InfiniteAmmo = p_On;
        return R"({"ok":true})";
    }

    if (p_Name == "invincible" || p_Name == "invisible") {
        if (p_On && !EnsureAICrippleEntity())
            return JsonError("cripple entity unavailable (not in a scene?)");
        if (!m_AICrippleEntity)
            return JsonError("cripple entity unavailable");

        const char* s_Pin = p_Name == "invincible"
            ? (p_On ? "SetHeroInvincible" : "SetHeroVulnerable")
            : (p_On ? "SetHeroHidden" : "SetHeroVisible");
        m_AICrippleEntity.SignalInputPin(ZString(std::string_view(s_Pin)));

        if (p_Name == "invincible") m_Invincible = p_On;
        else m_Invisible = p_On;
        return R"({"ok":true})";
    }

    if (p_Name == "noreload") {
        m_NoReload = p_On;
        return R"({"ok":true})";
    }

    return JsonError("unknown cheat " + p_Name);
}

std::string MCPBridge::CmdTimescale(double p_Value) {
    if (!Globals::GameTimeManager)
        return JsonError("no game time manager");
    Globals::GameTimeManager->m_fGameTimeMultiplier = static_cast<float>(p_Value);
    return R"({"ok":true})";
}

std::string MCPBridge::CmdSpawn(const std::string& p_RepoId, bool p_ToInventory) {
    auto s_Hitman = SDK()->GetLocalPlayer();
    if (!s_Hitman)
        return JsonError("no local player");

    const ZString s_RepoIdStr{std::string_view(p_RepoId)};
    const ZRepositoryID s_RepoId(s_RepoIdStr);

    if (p_ToInventory) {
        const auto s_Character = s_Hitman.m_pInterfaceRef->m_pCharacter.m_pInterfaceRef;
        const auto s_Controllers =
            &s_Character->m_rSubcontrollerContainer.m_pInterfaceRef->m_aReferencedControllers;
        const auto s_Inventory =
            static_cast<ZCharacterSubcontrollerInventory*>((*s_Controllers)[6].m_pInterfaceRef);

        Functions::ZCharacterSubcontrollerInventory_CreateItem->Call(
            s_Inventory,
            s_RepoId,
            "",
            {},
            ZCharacterSubcontrollerInventory::ECreateItemType::ECIT_ContractItem
        );

        return R"({"ok":true,"where":"inventory"})";
    }

    // World spawn: item spawner + repository key entity at the player's position.
    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;
    if (!s_Scene)
        return JsonError("scene not loaded");

    const auto s_SpawnerId = ResId<"[modules:/zitemspawner.class].pc_entitytype">;
    const auto s_KeyId = ResId<"[modules:/zitemrepositorykeyentity.class].pc_entitytype">;

    TResourcePtr<ZTemplateEntityFactory> s_SpawnerRes, s_KeyRes;
    Globals::ResourceManager->GetResourcePtr(s_SpawnerRes, s_SpawnerId, 0);
    Globals::ResourceManager->GetResourcePtr(s_KeyRes, s_KeyId, 0);

    if (!s_SpawnerRes || !s_KeyRes)
        return JsonError("spawner resources not loaded");

    ZEntityRef s_Spawner, s_Key;
    SExternalReferences s_ExternalRefs;

    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, s_Spawner, "", s_SpawnerRes, s_Scene.m_entityRef, s_ExternalRefs, -1
    );
    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, s_Key, "", s_KeyRes, s_Scene.m_entityRef, s_ExternalRefs, -1
    );

    if (!s_Spawner || !s_Key)
        return JsonError("failed to spawn entities");

    const auto s_HitmanSpatial = s_Hitman.m_entityRef.QueryInterface<ZSpatialEntity>();
    const auto s_ItemSpawner = s_Spawner.QueryInterface<ZItemSpawner>();

    s_ItemSpawner->m_ePhysicsMode = ZItemSpawner::EPhysicsMode::EPM_KINEMATIC;
    s_ItemSpawner->m_rMainItemKey.m_entityRef = s_Key;
    s_ItemSpawner->m_rMainItemKey.m_pInterfaceRef = s_Key.QueryInterface<ZItemRepositoryKeyEntity>();
    s_ItemSpawner->m_rMainItemKey.m_pInterfaceRef->m_RepositoryId = s_RepoId;
    s_ItemSpawner->m_bUsePlacementAttach = false;
    s_ItemSpawner->SetObjectToWorldMatrixFromEditor(s_HitmanSpatial->GetObjectToWorldMatrix());

    Functions::ZItemSpawner_RequestContentLoad->Call(s_ItemSpawner);

    return R"({"ok":true,"where":"world"})";
}

std::string MCPBridge::CmdItems(const std::string& p_Filter, int64_t p_Limit) {
    if (!m_RepoLoaded) {
        LoadRepositoryProps();
        m_RepoLoaded = true;
    }

    std::string s_FilterLower = p_Filter;
    std::transform(s_FilterLower.begin(), s_FilterLower.end(), s_FilterLower.begin(), ::tolower);

    std::string s_Out = "{\"ok\":true,\"items\":[";
    int64_t s_Count = 0;
    for (const auto& [s_Id, s_Name] : m_RepoProps) {
        std::string s_NameLower = s_Name;
        std::transform(s_NameLower.begin(), s_NameLower.end(), s_NameLower.begin(), ::tolower);
        if (!s_FilterLower.empty() && s_NameLower.find(s_FilterLower) == std::string::npos)
            continue;

        if (s_Count > 0)
            s_Out += ',';
        s_Out += std::format("{{\"id\":\"{}\",\"name\":\"{}\"}}", JsonEscape(s_Id), JsonEscape(s_Name));
        if (++s_Count >= p_Limit)
            break;
    }
    s_Out += std::format("],\"total\":{}}}", m_RepoProps.size());
    return s_Out;
}

void MCPBridge::LoadRepositoryProps() {
    m_RepoProps.clear();

    if (m_RepositoryResource.m_nResourceIndex.val == -1) {
        const auto s_ID = ResId<"[assembly:/repository/pro.repo].pc_repo">;
        Globals::ResourceManager->GetResourcePtr(m_RepositoryResource, s_ID, 0);
    }

    if (m_RepositoryResource.GetResourceInfo().status != RESOURCE_STATUS_VALID)
        return;

    const auto s_RepositoryData = static_cast<THashMap<
        ZRepositoryID, ZDynamicObject, TDefaultHashMapPolicy<ZRepositoryID>>*>(
        m_RepositoryResource.GetResourceData()
    );

    for (const auto& [s_RepositoryID, s_DynamicObject] : *s_RepositoryData) {
        TArray<SDynamicObjectKeyValuePair>* s_Entries =
            s_DynamicObject.As<TArray<SDynamicObjectKeyValuePair>>();

        ZString s_Id, s_Title, s_CommonName, s_Name;
        bool s_IsItem = false;

        for (auto& s_Entry : *s_Entries) {
            if (s_Entry.sKey == "ID_") s_Id = *s_Entry.value.As<ZString>();
            else if (s_Entry.sKey == "Title") s_Title = *s_Entry.value.As<ZString>();
            else if (s_Entry.sKey == "CommonName") s_CommonName = *s_Entry.value.As<ZString>();
            else if (s_Entry.sKey == "Name") s_Name = *s_Entry.value.As<ZString>();
            else if (s_Entry.sKey == "ItemType") s_IsItem = true;
        }

        if (s_Id.IsEmpty() || !s_IsItem)
            continue;

        std::string s_FinalName;
        if (!s_Title.IsEmpty())
            s_FinalName = s_Title.c_str();
        else if (!s_CommonName.IsEmpty())
            s_FinalName = s_CommonName.c_str();
        else if (!s_Name.IsEmpty())
            s_FinalName = s_Name.c_str();
        else
            s_FinalName = "<unnamed>";

        m_RepoProps.emplace_back(s_Id.c_str(), std::move(s_FinalName));
    }
}

// ---------- Cheat helpers ----------

bool MCPBridge::EnsureAICrippleEntity() {
    if (m_AICrippleEntity)
        return true;

    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;
    if (!s_Scene)
        return false;

    constexpr auto s_FactoryId = ResId<"[modules:/zaicrippleentity.class].pc_entitytype">;
    TResourcePtr<ZTemplateEntityFactory> s_Factory;
    Globals::ResourceManager->GetResourcePtr(s_Factory, s_FactoryId, 0);
    if (!s_Factory)
        return false;

    SExternalReferences s_ExternalRefs;
    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, m_AICrippleEntity, "", s_Factory, s_Scene.m_entityRef,
        s_ExternalRefs, -1
    );

    return static_cast<bool>(m_AICrippleEntity);
}

bool MCPBridge::EnsureCrippleBoxEntity() {
    if (m_HM5CrippleBoxEntity)
        return true;

    const auto s_Scene = Globals::Hitman5Module->m_pEntitySceneContext->m_pScene;
    if (!s_Scene)
        return false;

    constexpr auto s_FactoryId = ResId<"[modules:/zhm5cripplebox.class].pc_entitytype">;
    TResourcePtr<ZTemplateEntityFactory> s_Factory;
    Globals::ResourceManager->GetResourcePtr(s_Factory, s_FactoryId, 0);
    if (!s_Factory)
        return false;

    SExternalReferences s_ExternalRefs;
    Functions::ZEntityManager_NewEntity->Call(
        Globals::EntityManager, m_HM5CrippleBoxEntity, "", s_Factory, s_Scene.m_entityRef,
        s_ExternalRefs, -1
    );

    return static_cast<bool>(m_HM5CrippleBoxEntity);
}

// ---------- Overlay ----------

void MCPBridge::OnDrawMenu() {
    ImGui::Text("MCPBridge v%s on 127.0.0.1:%lld", c_Version, m_Port);
}

void MCPBridge::OnDrawUI(bool p_HasFocus) {
    if (m_Messages.empty())
        return;

    const ImGuiIO& s_IO = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(s_IO.DisplaySize.x * 0.5f, 60.f), ImGuiCond_Always, ImVec2(0.5f, 0.f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin(
        "##mcpbridge_msgs", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove
    );

    for (const auto& s_Msg : m_Messages)
        ImGui::TextColored(ImVec4(0.35f, 0.9f, 1.f, 1.f), "devin: %s", s_Msg.Text.c_str());

    ImGui::End();
}

// ---------- Detours ----------

DEFINE_PLUGIN_DETOUR(MCPBridge, void, OnClearScene, ZEntitySceneContext* th, bool p_FullyUnloadScene) {
    m_AICrippleEntity = {};
    m_HM5CrippleBoxEntity = {};
    m_Invincible = m_Invisible = m_InfiniteAmmo = false;
    m_RepoLoaded = false;

    return { HookAction::Continue() };
}

DEFINE_PLUGIN_DETOUR(
    MCPBridge, void, ZHM5ItemWeapon_SetBulletsInMagazine, IFirearm* th, int32_t nBullets
) {
    if (!m_NoReload)
        return { HookAction::Continue() };

    const auto s_LocalHitman = SDK()->GetLocalPlayer();
    if (!s_LocalHitman)
        return { HookAction::Continue() };

    ZHM5ItemWeapon* s_Weapon = static_cast<ZHM5ItemWeapon*>(th);
    if (s_Weapon->m_pOwner != s_LocalHitman.m_entityRef)
        return { HookAction::Continue() };

    if (s_Weapon->m_nBulletsFired == s_Weapon->m_nBulletsToFire)
        s_Weapon->m_nBulletsFired = 0;

    if (nBullets != 0)
        return { HookAction::Continue() };

    if (!s_LocalHitman.m_pInterfaceRef->IsInfiniteAmmoEnabled()) {
        auto s_Character = s_LocalHitman.m_pInterfaceRef->m_pCharacter.m_pInterfaceRef;
        auto s_Controllers =
            &s_Character->m_rSubcontrollerContainer.m_pInterfaceRef->m_aReferencedControllers;
        auto s_Inventory =
            static_cast<ZCharacterSubcontrollerInventory*>((*s_Controllers)[6].m_pInterfaceRef);

        const eAmmoType s_AmmoType = th->GetAmmoType();
        uint32 s_AmmoInPocket = Functions::ZCharacterSubcontrollerInventory_GetAmmoInPocketForType->Call(
            s_Inventory, s_AmmoType
        );

        if (s_AmmoInPocket > 0) {
            s_AmmoInPocket -= s_Weapon->GetMagazineCapacity();
            s_Inventory->m_nAmmoInPocket[static_cast<size_t>(s_AmmoType)] = s_AmmoInPocket;
            nBullets = s_Weapon->GetMagazineCapacity();
        }
    }
    else {
        nBullets = s_Weapon->GetMagazineCapacity();
    }

    p_Hook->CallOriginal(th, nBullets);
    return { HookAction::Return() };
}

// ---------- Misc ----------

std::string MCPBridge::JsonError(const std::string& p_Message) {
    return std::format("{{\"ok\":false,\"error\":\"{}\"}}", JsonEscape(p_Message));
}

std::string MCPBridge::JsonEscape(const std::string& p_Str) {
    std::string s_Out;
    s_Out.reserve(p_Str.size() + 8);
    for (char c : p_Str) {
        switch (c) {
        case '"': s_Out += "\\\""; break;
        case '\\': s_Out += "\\\\"; break;
        case '\n': s_Out += "\\n"; break;
        case '\r': s_Out += "\\r"; break;
        case '\t': s_Out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                s_Out += std::format("\\u{:04x}", c);
            else
                s_Out += c;
        }
    }
    return s_Out;
}

DEFINE_ZHM_PLUGIN(MCPBridge);
