// Quick test: simulate a host and guest connecting to the Railway signaling server.
// Verifies the server accepts connections, assigns roles, and relays signals.
// Usage: node test.js

const WebSocket = require('ws');

const URL = 'wss://sf4-rollback-production.up.railway.app';
const ROOM = 'test-room-' + Date.now();

function connect(role) {
    return new Promise((resolve, reject) => {
        const ws = new WebSocket(URL);
        ws.on('open', () => {
            console.log(`[${role}] connected`);
            ws.send(JSON.stringify({ type: 'hello', role, room: ROOM }));
        });
        ws.on('message', (raw) => {
            const msg = JSON.parse(raw);
            console.log(`[${role}] received:`, msg);
            resolve({ ws, msg });
        });
        ws.on('error', (err) => {
            console.error(`[${role}] error:`, err.message);
            reject(err);
        });
    });
}

async function run() {
    console.log(`Connecting to ${URL}`);
    console.log(`Room: ${ROOM}\n`);

    // Connect host and wait for 'ready'
    const host = await connect('host');
    if (host.msg.type !== 'ready') {
        console.error('FAIL: host did not get ready');
        process.exit(1);
    }
    console.log('[host] got ready ✓\n');

    // Connect guest and wait for 'ready'
    const guest = await connect('guest');
    if (guest.msg.type !== 'ready') {
        console.error('FAIL: guest did not get ready');
        process.exit(1);
    }
    console.log('[guest] got ready ✓\n');

    // Guest sends a signal to host
    console.log('[guest] sending test signal to host...');
    guest.ws.send(JSON.stringify({ type: 'signal', data: 'dGVzdA==' }));

    // Host should receive it
    await new Promise((resolve, reject) => {
        host.ws.once('message', (raw) => {
            const msg = JSON.parse(raw);
            console.log('[host] received signal:', msg);
            if (msg.type === 'signal' && msg.data === 'dGVzdA==') {
                console.log('\nSIGNALING SERVER OK — relay is working ✓');
                resolve();
            } else {
                console.error('FAIL: unexpected message', msg);
                reject();
            }
        });
        setTimeout(() => reject(new Error('timeout waiting for signal relay')), 5000);
    });

    host.ws.close();
    guest.ws.close();
}

run().catch((err) => {
    console.error('FAIL:', err?.message || err);
    process.exit(1);
});
