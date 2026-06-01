#include <chrono>
#include <memory>

#include <windows.h>
#include <detours/detours.h>

#include <GameNetworkingSockets/steam/steamnetworkingsockets.h>
#include <GameNetworkingSockets/steam/isteamnetworkingutils.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "../session/sf4e__SessionServer.hxx"

#include "sf4e__Game__Battle.hxx"
#include "sf4e__Game__Battle__System.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__GgpoRelay.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__UserApp.hxx"

namespace SessionProtocol = sf4e::SessionProtocol;
using Dimps::App;
using Dimps::Event::EventBase;
using Dimps::Event::EventBaseWithEC;
using Dimps::Event::EventController;
using Dimps::Game::ProgressData;
using Dimps::GameEvents::RootEvent;
using Dimps::Math::FixedPoint;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using rUserApp = Dimps::UserApp;
using fSystem = sf4e::Game::Battle::System;
using fUserApp = sf4e::UserApp;
using fMainMenu = sf4e::GameEvents::MainMenu;
using fVsBattle = sf4e::GameEvents::VsBattle;
using fVsPreBattle = sf4e::GameEvents::VsPreBattle;
using sf4e::Game::Battle::Sound::SoundPlayerManager;
using sf4e::SessionClient;
using sf4e::SessionServer;
using sf4e::SignalingClient;

std::unique_ptr<fUserApp::Session> fUserApp::session;
std::unique_ptr<SessionServer> fUserApp::server;
std::unique_ptr<SignalingClient> fUserApp::serverSignaling;

sf4e::UserApp::Session::Session(
    const SessionClient::Callbacks& callbacks,
    std::string sidecarHash,
    uint16_t ggpoPort,
    std::string& name,
    uint8_t _deviceType,
    uint8_t _deviceIdx,
    uint8_t _delay
):
    client(callbacks, sidecarHash, ggpoPort, name),
    deviceType(_deviceType),
    deviceIdx(_deviceIdx),
    delay(_delay)
{}

void fUserApp::_OnVsBattleTasksRegistered()
{
    // Start the GGPO connection
    bool isPlayer = false;
    for (int i = 0; i < 2; i++) {
        if (session->client._lobbyData.members[i].name == session->client._name) {
            isPlayer = true;
            break;
        }
    }
    if (isPlayer) {
        // Install relay before GGPO binds its socket so Hook_bind fires.
        GgpoRelay::StartSession(&session->client, session->client._ggpoPort);

        GGPOPlayer players[MAX_SF4E_PROTOCOL_USERS];
        for (int i = 0; i < 2 && i < session->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = session->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.size = sizeof(GGPOPlayer);
            player.player_num = i + 1;
            if (session->client._lobbyData.members[i].name == session->client._name) {
                player.type = GGPO_PLAYERTYPE_LOCAL;

                // Inject the chosen device into this player's side
                Dimps::Pad::System* padSys = Dimps::Pad::System::staticMethods.GetSingleton();
                Dimps::Pad::System::__publicMethods& padSysMethods = Dimps::Pad::System::publicMethods;
                (padSys->*padSysMethods.AssociatePlayerAndGamepad)(i, session->deviceIdx);
                (padSys->*padSysMethods.SetDeviceTypeForPlayer)(i, session->deviceType);
                (padSys->*padSysMethods.SetSideHasAssignedController)(i, 1);
                (padSys->*padSysMethods.SetActiveButtonMapping)(Dimps::Pad::System::BUTTON_MAPPING_FIGHT);
            }
            else {
                player.type = GGPO_PLAYERTYPE_REMOTE;
                // Route via the local relay instead of directly to the peer's IP.
                // The relay intercepts GGPO's UDP and forwards it over the GNS
                // session connection, so neither side needs open inbound ports.
                uint16_t relayPort = GgpoRelay::AddSlot(memberData.connId);
                strcpy_s(player.u.remote.ip_address, 32, "127.0.0.1");
                player.u.remote.port = relayPort;
            }
        }
        for (int i = 2; i < session->client._lobbyData.members.size(); i++) {
            SessionProtocol::MemberData& memberData = session->client._lobbyData.members[i];
            GGPOPlayer& player = players[i];
            player.type = GGPO_PLAYERTYPE_SPECTATOR;
            uint16_t relayPort = GgpoRelay::AddSlot(memberData.connId);
            strcpy_s(player.u.remote.ip_address, 32, "127.0.0.1");
            player.u.remote.port = relayPort;
        }
        fSystem::StartGGPO(
            players,
            session->client._lobbyData.members.size(),
            session->client._ggpoPort,
            session->delay,
            session->client._matchData.rngSeed
        );
    }
    else {
        // Always spectate from	P1 for now- the protocol has
        // limited enough players that there's marginal bandwidth
        // differences.	
        // 
        char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
        char* hostIP;
        if (session->client._lobbyData.members[0].ip.empty()) {
            session->client._serverAddr.ToString(szAddr, sizeof(szAddr), false);
            hostIP = szAddr;
        }
        else {
            // Safe-_ish_ removal of const. This gets passed through
            // to an inet_pton() call and never modified.
            hostIP = (char*)session->client._lobbyData.members[0].ip.c_str();
        }

        fSystem::StartSpectating(
            session->client._ggpoPort,
            2,
            hostIP,
            session->client._lobbyData.members[0].port,
            session->client._matchData.rngSeed
        );
    }
}

void fUserApp::_OnVsPreBattleTasksRegistered()
{
    size_t charaConditionSize = sizeof(rVsMode::ConfirmedCharaConditions);

    // XXX (adanducci): this is a little fragile- it's technically possible
    // that the pre-battle event is constructed in another context, but
    // practically speaking the VsPreBattle event will always be used in
    // the context of VsMode.
    char* vsModeQuery[] = { "VSMode" };
    rVsMode* mode = (rVsMode*)EventBaseWithEC::FindForegroundEvent(App::GetRootEvent(), vsModeQuery, 1);
    if (!mode) {
        spdlog::error("VsPreBattle tasks registered, but the current foreground event isn't VSMode!");
        return;
    }

    Dimps::Platform::dString* stageName = rVsMode::GetStageName(mode);
    rVsMode::ConfirmedPlayerConditions* conditions = rVsMode::GetConfirmedPlayerConditions(mode);
    for (int i = 0; i < 2; i++) {
        *(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[i])) = session->client._matchData.chara[i].charaID;
        *(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[i])) = 1;
        rVsMode::ConfirmedCharaConditions* charaConditions = rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[i]);
        memcpy_s(charaConditions, charaConditionSize, &session->client._matchData.chara[i], charaConditionSize);
    }

    (stageName->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[session->client._matchData.stageID], 4);
    *(rVsMode::GetStageCode(mode)) = session->client._matchData.stageID;
}

void OnReady(sf4e::SessionClient* const client, const sf4e::SessionClient::Callbacks& c) {
    // Since handling a request forces the process to load into a battle,
    // handling the request can only reasonably be done if the process is
    // currently on the main menu.
    RootEvent* root = App::GetRootEvent();
    char* mainMenuQuery[1] = { "MainMenu" };
    rMainMenu* mainMenu = (rMainMenu*)EventBaseWithEC::FindForegroundEvent(
        root,
        mainMenuQuery,
        1
    );
    if (!mainMenu) {
        spdlog::info("Client: ignoring that both clients are ready because we're not on the main menu");
        return;
    }

    ProgressData* progressData = *RootEvent::GetProgressData(root);
    ProgressData::BattleTypeSettings* BattleTypeSettings = &(ProgressData::GetBattleTypeSettings(progressData)[ProgressData::NBT_PVP]);
    *ProgressData::GetNextBattleType(progressData) = ProgressData::NBT_PVP;
    BattleTypeSettings->editionSelect = client->_lobbyData.editionSelect;
    BattleTypeSettings->rounds = client->_lobbyData.roundCount;
    BattleTypeSettings->timeLimit = client->_lobbyData.roundTime;
    fVsPreBattle::bSkipToVersus = true;
    fVsPreBattle::OnTasksRegistered = fUserApp::_OnVsPreBattleTasksRegistered;
    fVsBattle::OnTasksRegistered = fUserApp::_OnVsBattleTasksRegistered;
    (rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
}

void OnBattleSynced(SessionClient* const client, const sf4e::SessionClient::Callbacks& callbacks) {
    fVsBattle::bSessionSynced = true;
}

void OnGgpoData(const sf4e::SessionProtocol::ConnectionID& src, const std::vector<uint8_t>& payload, sf4e::SessionClient* const, const sf4e::SessionClient::Callbacks&) {
    GgpoRelay::InjectFromCid(src, payload.data(), (int)payload.size());
}

sf4e::SessionClient::Callbacks clientCallbacks = {
    nullptr,
    sf4e::Overlay::OnClientError,
    OnReady,
    OnBattleSynced,
    OnGgpoData,
};

void fUserApp::Install() {
    GgpoRelay::InstallHooks();
    DetourAttach((PVOID*)&rUserApp::staticMethods.Steam_PostUpdate, Steam_PostUpdate);
}

void fUserApp::StartSession(char* joinAddr, uint16_t port, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay) {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.ParseString(joinAddr);
    session.reset(new Session(
        clientCallbacks,
        sidecarHash,
        port,
        name,
        deviceType,
        deviceIdx,
        delay
    ));
    session->client.Connect(addr);
}

void fUserApp::StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) {
    server.reset(new SessionServer(identity, sidecarHash, editionSelect, roundCount, roundTime));
    server->Listen(hostPort);
}

void fUserApp::StartServerP2P(const std::string& signalingUrl, const std::string& roomCode, uint16 hostPort, std::string& sidecarHash, bool editionSelect, int roundCount, FixedPoint roundTime) {
    // Configure STUN so GNS can traverse NAT without any open ports.
    SteamNetworkingUtils()->SetGlobalConfigValueString(
        k_ESteamNetworkingConfig_P2P_STUN_ServerList,
        "stun.l.google.com:19302"
    );
    server.reset(new SessionServer(roomCode, sidecarHash, editionSelect, roundCount, roundTime));
    // Direct-IP socket for the host's own local client (no port forwarding needed,
    // it only listens on loopback).
    server->Listen(hostPort);
    // P2P socket for remote joiners via the signaling relay.
    serverSignaling.reset(new SignalingClient(signalingUrl, SignalingClient::Role::Host, roomCode));
    serverSignaling->Start();
    server->ListenP2P(serverSignaling.get());
}

void fUserApp::StartSessionP2P(const std::string& signalingUrl, const std::string& roomCode, uint16_t ggpoPort, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay) {
    SteamNetworkingUtils()->SetGlobalConfigValueString(
        k_ESteamNetworkingConfig_P2P_STUN_ServerList,
        "stun.l.google.com:19302"
    );
    auto signaling = std::unique_ptr<SignalingClient>(
        new SignalingClient(signalingUrl, SignalingClient::Role::Guest, roomCode)
    );
    signaling->Start();

    session.reset(new Session(clientCallbacks, sidecarHash, ggpoPort, name, deviceType, deviceIdx, delay));
    // Store signaling client inside the session so it lives as long as the connection.
    session->signaling = std::move(signaling);
    session->client.ConnectP2P(session->signaling.get());
}

std::string fUserApp::GenerateRoomCode() {
    static const char CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string code(6, ' ');
    for (char& c : code) {
        c = CHARS[sf4e::localRand() % (sizeof(CHARS) - 1)];
    }
    return code;
}

void fUserApp::Steam_PostUpdate() {
    if (session) {
        session->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }
    SteamNetworkingSockets()->RunCallbacks();

    if (session) {
        if (session->client.Step()) {
            GgpoRelay::EndSession();
            delete session.release();
        }
    }

    if (server) {
        if (server->Step()) {
            delete server.release();
            serverSignaling.reset();
        }
    }

    if (fSystem::ggpo) {
        ggpo_idle(fSystem::ggpo, 1);
    }

    rUserApp::staticMethods.Steam_PostUpdate();
}
