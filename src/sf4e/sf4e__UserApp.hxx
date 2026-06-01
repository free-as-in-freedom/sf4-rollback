#pragma once

#include <memory>
#include <string>

#include <windows.h>

#include "../Dimps/Dimps__Math.hxx"
#include "../Dimps/Dimps__UserApp.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include "sf4e__Signaling.hxx"

namespace sf4e {
    struct UserApp : Dimps::UserApp
    {
        struct Session {
            Session(
                const SessionClient::Callbacks& callbacks,
                std::string sidecarHash,
                uint16_t ggpoPort,
                std::string& name,
                uint8_t _deviceType,
                uint8_t _deviceIdx,
                uint8_t _delay
            );

            std::unique_ptr<SignalingClient> signaling; // non-null for P2P sessions
            SessionClient client;
            uint8_t deviceType;
            uint8_t deviceIdx;
            uint8_t delay;
        };

        static std::unique_ptr<Session> session;
        static std::unique_ptr<SessionServer> server;
        static std::unique_ptr<SignalingClient> serverSignaling;

        static void Install();
        static void Steam_PostUpdate();
        static void StartSession(char* joinAddr, uint16_t port, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay);
        static void StartSessionP2P(const std::string& signalingUrl, const std::string& roomCode, uint16_t ggpoPort, std::string& sidecarHash, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay);
        static void StartServer(uint16 hostPort, std::string& identity, std::string& sidecarHash, bool editionSelect, int roundCount, Dimps::Math::FixedPoint roundTime);
        static void StartServerP2P(const std::string& signalingUrl, const std::string& roomCode, uint16 hostPort, std::string& sidecarHash, bool editionSelect, int roundCount, Dimps::Math::FixedPoint roundTime);
        static std::string GenerateRoomCode();
        static void _OnVsPreBattleTasksRegistered();
        static void _OnVsBattleTasksRegistered();
    };
}