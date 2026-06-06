// sf4e signaling server
// Relays GNS ICE signals between a host and guest in a named room.
// Neither player needs an open port; this server only handles connection setup.
// Game traffic flows P2P after ICE negotiation completes.
//
// Usage: PORT=9000 node index.js

const WebSocket = require('ws');

const PORT = process.env.PORT || 9000;
const server = new WebSocket.Server({ port: PORT });

// rooms: roomCode -> { host: ws|null, guest: ws|null }
const rooms = new Map();

function getOrCreateRoom(code) {
    if (!rooms.has(code)) rooms.set(code, { host: null, guest: null });
    return rooms.get(code);
}

function cleanRoom(code) {
    const room = rooms.get(code);
    if (room && !room.host && !room.guest) rooms.delete(code);
}

server.on('connection', (ws) => {
    let role = null;
    let roomCode = null;

    ws.on('message', (raw) => {
        let msg;
        try { msg = JSON.parse(raw); } catch { return; }

        if (msg.type === 'hello') {
            role = msg.role;        // 'host' or 'guest'
            roomCode = msg.room;

            const room = getOrCreateRoom(roomCode);
            if (room[role] && room[role] !== ws) {
                // Slot already taken
                ws.send(JSON.stringify({ type: 'error', reason: 'slot_taken' }));
                return;
            }
            room[role] = ws;
            ws.send(JSON.stringify({ type: 'ready' }));
            console.log(`[${roomCode}] ${role} joined`);
            return;
        }

        if (msg.type === 'signal') {
            const room = rooms.get(roomCode);
            if (!room) return;
            const peerRole = role === 'host' ? 'guest' : 'host';
            const peer = room[peerRole];
            if (peer && peer.readyState === WebSocket.OPEN) {
                peer.send(JSON.stringify({ type: 'signal', data: msg.data }));
            }
        }
    });

    ws.on('close', () => {
        if (!roomCode || !role) return;
        const room = rooms.get(roomCode);
        if (room && room[role] === ws) {
            room[role] = null;
            console.log(`[${roomCode}] ${role} left`);
            cleanRoom(roomCode);
        }
    });
});

console.log(`sf4e signaling server listening on port ${PORT}`);
