#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <steam/isteamnetworkingsockets.h>
#include <steamnetworkingcustomsignaling.h>
#include <steam/isteamnetworkingutils.h>

namespace sf4e {

// SignalingClient connects to the lightweight WebSocket signaling relay and
// implements both GNS signaling interfaces so it can serve both the host
// (ISteamNetworkingSignalingRecvContext — accepts incoming P2P connections)
// and the joiner (ISteamNetworkingConnectionCustomSignaling — sends signals
// on an outgoing P2P connection).
//
// ICE candidate blobs are base64-encoded and sent as JSON over WebSocket.
// The WebSocket connection is only needed during ICE negotiation; it can be
// closed once the GNS connection reaches the Connected state.
class SignalingClient
    : public ISteamNetworkingConnectionSignaling
    , public ISteamNetworkingSignalingRecvContext
{
public:
    enum class Role { Host, Guest };

    SignalingClient(const std::string& url, Role role, const std::string& roomCode);
    ~SignalingClient();

    // Connect WebSocket to signaling server. Returns true if the connection
    // was initiated (it may not be fully established yet).
    bool Start();
    void Stop();
    bool IsConnected() const { return _wsConnected; }

    // Call once per frame from the game thread to dispatch queued signals
    // into GNS via ReceivedP2PCustomSignal.
    void Poll(ISteamNetworkingSockets* pInterface);

    // ISteamNetworkingConnectionCustomSignaling
    // Called by GNS (on game thread) to send an ICE signal to the peer.
    bool SendSignal(HSteamNetConnection hConn, const SteamNetConnectionInfo_t& info,
                    const void* pMsg, int cbMsg) override;
    // GNS calls Release() when done with this signaling object. We manage our
    // own lifetime so this is intentionally a no-op.
    void Release() override {}

    // ISteamNetworkingSignalingRecvContext
    // Called by GNS when an incoming P2P connection request arrives (host side).
    // Returns this so the same WebSocket channel is used for the response.
    ISteamNetworkingConnectionSignaling* OnConnectRequest(
        HSteamNetConnection hConn,
        const SteamNetworkingIdentity& identityPeer,
        int nLocalVirtualPort) override;
    void SendRejectionSignal(const SteamNetworkingIdentity& identityPeer,
                             const void* pMsg, int cbMsg) override;

private:
    std::string _url;
    Role _role;
    std::string _roomCode;

    ix::WebSocket _ws;
    bool _wsConnected = false;

    std::mutex _queueMutex;
    std::vector<std::vector<uint8_t>> _pendingSignals;

    void OnWsMessage(const ix::WebSocketMessagePtr& msg);
    void SendJson(const std::string& payload);
};

} // namespace sf4e
