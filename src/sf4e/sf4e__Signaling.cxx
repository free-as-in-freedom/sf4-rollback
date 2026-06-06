#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "sf4e__Signaling.hxx"

using nlohmann::json;
using sf4e::SignalingClient;

// ---------------------------------------------------------------------------
// Minimal base64 encode/decode — avoids adding another dependency
// ---------------------------------------------------------------------------
static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out += B64_CHARS[(b >> 18) & 0x3f];
        out += B64_CHARS[(b >> 12) & 0x3f];
        out += (i + 1 < len) ? B64_CHARS[(b >> 6) & 0x3f] : '=';
        out += (i + 2 < len) ? B64_CHARS[(b     ) & 0x3f] : '=';
    }
    return out;
}

static std::vector<uint8_t> b64_decode(const std::string& enc) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve((enc.size() / 4) * 3);
    uint32_t buf = 0; int bits = 0;
    for (char c : enc) {
        int v = T[(uint8_t)c];
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(buf >> bits));
            buf &= (1 << bits) - 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
SignalingClient::SignalingClient(const std::string& url, Role role, const std::string& roomCode)
    : _url(url), _role(role), _roomCode(roomCode)
{}

SignalingClient::~SignalingClient() {
    Stop();
}

bool SignalingClient::Start() {
    _ws.setUrl(_url);
    _ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        OnWsMessage(msg);
    });
    _ws.start();
    spdlog::info("Signaling: connecting to {}", _url);
    return true;
}

void SignalingClient::Stop() {
    _ws.stop();
    _wsConnected = false;
}

void SignalingClient::OnWsMessage(const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
        _wsConnected = true;
        const char* roleStr = (_role == Role::Host) ? "host" : "guest";
        json hello = {{"type", "hello"}, {"role", roleStr}, {"room", _roomCode}};
        _ws.send(hello.dump());
        spdlog::info("Signaling: connected, joined room {} as {}", _roomCode, roleStr);
    }
    else if (msg->type == ix::WebSocketMessageType::Close) {
        _wsConnected = false;
        spdlog::info("Signaling: WebSocket closed");
    }
    else if (msg->type == ix::WebSocketMessageType::Error) {
        spdlog::error("Signaling: WebSocket error: {}", msg->errorInfo.reason);
    }
    else if (msg->type == ix::WebSocketMessageType::Message) {
        json j;
        try { j = json::parse(msg->str); }
        catch (...) { return; }

        std::string type = j.value("type", "");
        if (type == "signal") {
            auto blob = b64_decode(j["data"].get<std::string>());
            std::lock_guard<std::mutex> lk(_queueMutex);
            _pendingSignals.push_back(std::move(blob));
        }
        else if (type == "error") {
            spdlog::error("Signaling: server error: {}", j.value("reason", "unknown"));
        }
    }
}

void SignalingClient::Poll(ISteamNetworkingSockets* pInterface) {
    std::vector<std::vector<uint8_t>> signals;
    {
        std::lock_guard<std::mutex> lk(_queueMutex);
        signals.swap(_pendingSignals);
    }
    for (auto& blob : signals) {
        pInterface->ReceivedP2PCustomSignal(blob.data(), (int)blob.size(), this);
    }
}

bool SignalingClient::SendSignal(
    HSteamNetConnection, const SteamNetConnectionInfo_t&,
    const void* pMsg, int cbMsg)
{
    auto encoded = b64_encode((const uint8_t*)pMsg, (size_t)cbMsg);
    json j = {{"type", "signal"}, {"data", encoded}};
    SendJson(j.dump());
    return true;
}

ISteamNetworkingConnectionSignaling* SignalingClient::OnConnectRequest(
    HSteamNetConnection hConn,
    const SteamNetworkingIdentity& /*identityPeer*/,
    int /*nLocalVirtualPort*/)
{
    spdlog::info("Signaling: incoming P2P connection request (conn={})", (int)hConn);
    return this;
}

void SignalingClient::SendRejectionSignal(
    const SteamNetworkingIdentity& /*identityPeer*/,
    const void* pMsg, int cbMsg)
{
    auto encoded = b64_encode((const uint8_t*)pMsg, (size_t)cbMsg);
    json j = {{"type", "signal"}, {"data", encoded}};
    SendJson(j.dump());
}

void SignalingClient::SendJson(const std::string& payload) {
    _ws.send(payload);
}
