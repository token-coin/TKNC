﻿﻿﻿﻿const express = require('express');
const cors = require('cors');
const rateLimit = require('express-rate-limit');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');
const crypto = require('crypto');
const fs = require('fs');

// Load environment variables from .env file
require('dotenv').config();

// DEV MODE: must be explicitly set via environment variable, default OFF for production
process.env.TKNC_DEV_MODE = process.env.TKNC_DEV_MODE || '0';
const IS_DEV_MODE = process.env.TKNC_DEV_MODE === '1';

if (IS_DEV_MODE) {
    console.warn('╔══════════════════════════════════════════════════════════╗');
    console.warn('║ ⚠️  TKNC_DEV_MODE=1 — DEV HELPERS ACTIVE               ║');
    console.warn('║ CORS permissive + stack traces in errors.             ║');
    console.warn('║ Signature verification is ALWAYS enforced.             ║');
    console.warn('╚══════════════════════════════════════════════════════════╝');
}

// ===== SECURITY: Prototype Pollution Protection =====
// Block __proto__, constructor, prototype keys from JSON body
function preventPrototypePollution(req, res, next) {
    if (req.body && typeof req.body === 'object') {
        const dangerous = ['__proto__', 'constructor', 'prototype', 'prototype.__proto__'];
        function scrub(obj) {
            if (!obj || typeof obj !== 'object') return;
            for (const key of Object.keys(obj)) {
                if (dangerous.includes(key)) {
                    delete obj[key];
                    console.warn(`[Security] Blocked prototype pollution key: ${key} from ${req.ip} ${req.method} ${req.url}`);
                } else if (typeof obj[key] === 'object' && obj[key] !== null) {
                    scrub(obj[key]);
                }
            }
        }
        scrub(req.body);
    }
    next();
}

class FilePersistence {
    constructor(baseDir) {
        this.baseDir = baseDir;
        this._ensureDir();
    }

    _ensureDir() {
        try {
            if (!fs.existsSync(this.baseDir)) {
                fs.mkdirSync(this.baseDir, { recursive: true });
            }
        } catch (e) {
            console.error(`[Persistence] Failed to create dir ${this.baseDir}:`, e.message);
        }
    }

    _filePath(key) {
        return path.join(this.baseDir, `${key}.json`);
    }

    save(key, data) {
        try {
            const serialized = Array.isArray(data) ? data : Array.from(data.entries());
            fs.writeFileSync(this._filePath(key), JSON.stringify(serialized, null, 2), 'utf-8');
        } catch (e) {
            console.error(`[Persistence] Save failed [${key}]:`, e.message);
        }
    }

    load(key, isArray = false) {
        try {
            const fp = this._filePath(key);
            if (!fs.existsSync(fp)) return null;
            const raw = JSON.parse(fs.readFileSync(fp, 'utf-8'));
            if (isArray) return raw;
            return new Map(raw);
        } catch (e) {
            console.error(`[Persistence] Load failed [${key}]:`, e.message);
            return null;
        }
    }

    exists(key) {
        return fs.existsSync(this._filePath(key));
    }
}

class TKNCWebServer {
    constructor() {
        this.app = express();
        // Trust proxy: nginx forwards X-Forwarded-For, required by express-rate-limit
        this.app.set('trust proxy', 1);
        this.server = http.createServer(this.app);
        this.wss = new WebSocket.Server({ server: this.server });
        this.port = process.env.PORT || 80;
        this.apiPort = process.env.API_PORT || 9332;
        this.rpcPort = process.env.RPC_PORT || 9331;

        // ===== BLOCKCHAIN RPC CLIENT =====
        this.rpcUrl = process.env.RPC_HOST || '127.0.0.1';
        this.rpcPort = 9331;
        this.rpcWallet = process.env.RPC_WALLET || 'default';
        // RPC credentials from environment (NEVER hardcode in production)
        const rpcUser = process.env.TKNC_RPC_USER || 'tknc';
        const rpcPass = process.env.TKNC_RPC_PASS || 'tknc123';
        if (!process.env.TKNC_RPC_USER || !process.env.TKNC_RPC_PASS) {
            console.warn('[Security] ⚠️  Using default RPC credentials! Set TKNC_RPC_USER/TKNC_RPC_PASS env vars in production!');
        }
        this.rpcAuth = 'Basic ' + Buffer.from(rpcUser + ':' + rpcPass).toString('base64');
        this.nonceStore = new Map(); // wallet → { nonce, timestamp, expires }

        // TKNC team wallet address (receives 10% dev share of block reward)
        this.TEAM_WALLET_ADDRESS = 'token1qjln2lhvqe49yv7f6jud0ms874ge2fu4e0e3fjv';

        // ===== USER WALLET TRACKING (session → wallet on node) =====
        this.userWallets = new Map(); // session_token → wallet_name on node
        this.walletNameByAddress = new Map(); // user_wallet_address → wallet_name

        this.miners = new Map(); // miner_id -> miner_data (global registry)
        this.peers = new Map(); // node_id -> peer info (role, capabilities, public_ip, p2p_port, ws_port, wallet, last_heartbeat, timestamp)
        this.clients = new Set(); // WebSocket clients (browsers)
        this.apiKeys = new Map(); // api_key -> key_data (for global tracking)
        this.usageStats = new Map(); // miner_id -> { total_calls, revenue }

        // Session management
        this.sessions = new Map(); // session_token -> user_data
        this.SESSION_TIMEOUT = 30 * 60 * 1000; // 30min — auto-logout on inactivity

        // Comments system
        this.comments = []; // all comments
        this.MAX_COMMENTS = 1000; // max comments limit
        this.commentInvites = new Map(); // "miner_wallet_user_wallet" → {status, paid_at, txid, used}

        // ===== INFERENCE HISTORY SYSTEM =====
        this.inferenceHistory = new Map(); // api_key -> { user_wallet, miner_id, model, created_at, used_at, call_count }
        
        // ===== PAYMENT NONCE SYSTEM =====
        this.paymentNonces = new Map(); // nonce -> { wallet, amount, recipient, purpose, message, created_at, used, expires_at }
        this.userMinerRelations = new Map(); // "user_wallet:miner_id" -> { first_call, last_call, total_calls, can_comment }

        // ===== NAT TUNNEL SYSTEM =====
        this.tunnels = new Map(); // miner_id -> WebSocket (reverse tunnel)
        this.tunnelRequests = new Map(); // request_id -> { ws, resolve, reject }
        this.requestIdCounter = 0;

        // ===== REVERSE CONNECT REQUEST SYSTEM =====
        this.reverseConnectRequests = new Map(); // request_id -> { client_wallet, client_callback_url, created_at, status }
        this.reverseRequestIdCounter = 0;

        // P2P Gateway config
        this.HEARTBEAT_INTERVAL = 30000;
        this.MINER_TIMEOUT = 120000;
        this.STALE_REMOVAL_TIMEOUT = 600000;
        this.TUNNEL_TIMEOUT = 60000;

        this.setupMiddleware();
        this.setupRoutes();
        this.setupErrorHandling();
        this.setupWebSocket();
        this.startHeartbeatMonitor();

        this._initPersistence();
    }

    _initPersistence() {
        const persistDir = path.join(__dirname, '..', 'data', 'persistence');
        this.persistence = new FilePersistence(persistDir);

        const PERSIST_KEYS = {
            comments: { target: 'comments', isArray: true },
            apiKeys: { target: 'apiKeys', isArray: false },
            paymentNonces: { target: 'paymentNonces', isArray: false },
            inferenceHistory: { target: 'inferenceHistory', isArray: false },
            userMinerRelations: { target: 'userMinerRelations', isArray: false },
            commentInvites: { target: 'commentInvites', isArray: false }
        };

        for (const [key, cfg] of Object.entries(PERSIST_KEYS)) {
            const loaded = this.persistence.load(key, cfg.isArray);
            if (loaded !== null) {
                this[cfg.target] = loaded;
                console.log(`[Persistence] Loaded ${key}: ${cfg.isArray ? loaded.length : loaded.size} items`);
            }
        }

        this._saveInterval = setInterval(() => this._persistAll(), 60000);
        this._persistAll();

        const graceful = (signal) => {
            console.log(`[Persistence] ${signal} received, saving...`);
            clearInterval(this._saveInterval);
            this._persistAll();
            process.exit(0);
        };
        process.on('SIGTERM', () => graceful('SIGTERM'));
        process.on('SIGINT', () => graceful('SIGINT'));
    }

    _persistAll() {
        if (!this.persistence) return;
        try {
            this.persistence.save('comments', this.comments);
            this.persistence.save('apiKeys', this.apiKeys);
            this.persistence.save('paymentNonces', this.paymentNonces);
            this.persistence.save('inferenceHistory', this.inferenceHistory);
            this.persistence.save('userMinerRelations', this.userMinerRelations);
            this.persistence.save('commentInvites', this.commentInvites);
        } catch (e) {
            console.error(`[Persistence] _persistAll error:`, e.message);
        }
    }
    
    setupMiddleware() {
            this.app.use((req, res, next) => {
                // Prevent clickjacking
                res.setHeader('X-Frame-Options', 'DENY');
                // Prevent MIME sniffing
                res.setHeader('X-Content-Type-Options', 'nosniff');
                res.setHeader('X-XSS-Protection', '1; mode=block');
                // Referrer Policy
                res.setHeader('Referrer-Policy', 'strict-origin-when-cross-origin');
                // Content-Security-Policy (basic)
                res.setHeader('Content-Security-Policy',
                    "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'self'; form-action 'self'");
                // HSTS (only in production with HTTPS)
                if (req.secure || req.protocol === 'https') {
                    res.setHeader('Strict-Transport-Security', 'max-age=31536000; includeSubDomains; preload');
                }
                next();
            });

            // PROTOTYPE POLLUTION PROTECTION (must be before express.json)
            this.app.use(preventPrototypePollution);

            // ABSOLUTE FIRST: catch ALL requests before anything else
            this.app.use((req, res, next) => {
                console.log('[FIRST]', req.method, req.url, 'ct:', req.headers['content-type'] || 'none', 'cl:', req.headers['content-length'] || '0');
                next();
            });

            const allowedOrigins = (process.env.TKNC_CORS_ORIGINS || '').split(',').map(o => o.trim()).filter(o => o);
            if (allowedOrigins.length > 0) {
                this.app.use(cors({
                    origin: allowedOrigins,
                    credentials: true,
                    methods: ['GET', 'POST', 'PUT', 'DELETE', 'OPTIONS'],
                    allowedHeaders: ['Content-Type', 'Authorization', 'X-CSRF-Token', 'X-Wallet-Address']
                }));
                console.log(`[Security] CORS restricted to: ${allowedOrigins.join(', ')}`);
            } else if (process.env.TKNC_DEV_MODE === '1') {
                // Dev mode: allow all (must be explicitly enabled)
                this.app.use(cors());
                console.log('[Security] WARN: DEV MODE - CORS allows all origins');
            } else {
                // Production default: no cross-origin access
                this.app.use(cors({
                    origin: false,
                    methods: ['GET', 'POST', 'PUT', 'DELETE', 'OPTIONS'],
                    allowedHeaders: ['Content-Type', 'Authorization', 'X-CSRF-Token', 'X-Wallet-Address']
                }));
                console.log('[Security] CORS disabled (production-safe, same-origin only)');
            }

            const rateWindowMs = parseInt(process.env.TKNC_RATE_WINDOW_MS || '60000', 10); // 1 minute default
            const rateMax = parseInt(process.env.TKNC_RATE_MAX || '300', 10); // 300 requests per window default (increased for Explorer + whitepaper)
            const limiter = rateLimit({
                windowMs: rateWindowMs,
                max: rateMax,
                message: { success: false, error: 'Too many requests, please try again later.' },
                standardHeaders: true,
                legacyHeaders: false,
                // Skip rate limit for static files, WebSocket, and Explorer API (has its own handling)
                skip: (req) => {
                    const staticExts = ['.css', '.js', '.svg', '.png', '.jpg', '.jpeg', '.gif', '.ico', '.woff', '.woff2', '.ttf'];
                    const isStatic = staticExts.some(ext => req.url.endsWith(ext)) || 
                                    req.url.startsWith('/locales/') ||
                                    req.url.startsWith('/static/') ||
                                    req.url === '/ws' || 
                                    req.url.startsWith('/explorer/api');
                    return isStatic;
                }
            });
            this.app.use(limiter);
            console.log(`[Security] Rate limiting enabled: ${rateMax} requests/${rateWindowMs}ms per IP`);

            const authLimiter = rateLimit({
                windowMs: 15 * 60 * 1000, // 15 minutes
                max: 30, // 30 attempts per 15 minutes per IP
                message: { success: false, error: 'Too many authentication attempts. Please wait 15 minutes.' },
                standardHeaders: true,
                legacyHeaders: false,
                skip: (req) => req.url !== '/api/login/verify' && req.url !== '/api/login/init'
            });
            this.app.use(authLimiter);

            // Explorer-specific rate limiter (more lenient for blockchain browsing)
            const explorerLimiter = rateLimit({
                windowMs: 60000, // 1 minute
                max: 120, // 120 requests per minute for Explorer
                message: { success: false, error: 'Too many requests, please try again later.' },
                standardHeaders: true,
                legacyHeaders: false,
            });
            this.app.use('/explorer/api', explorerLimiter);

            this.app.use((req, res, next) => {
                // Skip CSRF for GET/HEAD/OPTIONS (read-only), API endpoints, and static files
                if (['GET', 'HEAD', 'OPTIONS'].includes(req.method) ||
                    req.url.startsWith('/api/v1/') ||  // Miner API (uses Bearer token auth)
                    req.url.startsWith('/static/') ||
                    req.url === '/ws') {
                    return next();
                }

                // Extract session token from Authorization header or query
                const authHeader = req.headers['authorization'] || '';
                const sessionToken = authHeader.replace('Bearer ', '').replace('Basic ', '').trim()
                    || req.query.token || req.headers['x-session-token'];

                if (!sessionToken) {
                    // No session = no CSRF check needed (will be rejected by auth middleware anyway)
                    return next();
                }

                const session = this.sessions.get(sessionToken);
                if (session && session.csrf_token) {
                    const clientCsrfToken = req.headers['x-csrf-token'] || req.body?.csrf_token;
                    if (!clientCsrfToken || clientCsrfToken !== session.csrf_token) {
                        console.log(`[Security] CSRF token mismatch from ${req.ip} on ${req.method} ${req.url}`);
                        return res.status(403).json({ success: false, error: 'CSRF token validation failed' });
                    }
                }
                next();
            });
            console.log('[Security] CSRF verification enabled for state-changing requests');

        // Manual raw body collector for P2P register (before express.json)
        // C++ nodes use raw sockets, which can cause stream readability issues with express.json()
        this.app.use('/api/p2p/register', (req, res, next) => {
            if (req.method === 'POST') {
                let data = '';
                req.on('data', chunk => { if (chunk) data += chunk.toString(); });
                req.on('end', () => {
                    try {
                        if (data && data.trim()) {
                            req.body = JSON.parse(data);
                            // Mark as already parsed to skip express.json()
                            req._body = true;
                        }
                    } catch(e) {
                        console.log('[P2P-REG] JSON parse error:', e.message, 'Raw length:', data.length);
                        req._rawBodyStr = data;
                    }
                    next();
                });
                req.on('error', (err) => {
                    console.log('[P2P-REG] Stream error:', err.message);
                    next();
                });
            } else {
                next();
            }
        });

        this.app.use(express.json({ limit: '10mb', inflate: true, strict: true }));

        // ===== API RESPONSE CACHE (in-memory, short TTL) =====
        this.apiCache = new Map();
        this.apiCacheTimers = new Map();

        // Static files: serve with cache headers (browser caches 7d, revalidate)
        const staticOpts = { etag: true, maxAge: '7d', immutable: true };
        this.app.use(express.static(path.join(__dirname, 'public'), staticOpts));
    }

    // ===== API RESPONSE CACHE HELPER =====
    // Caches API responses in-memory with a TTL to reduce RPC calls.
    // During the TTL window, all clients receive the cached response instantly.
    // Uses stale-while-revalidate: serves stale data immediately while
    // fetching fresh data in the background.
    _cachedResponse(key, ttlMs, fetcher, req, res) {
        const cached = this.apiCache.get(key);
        if (cached && Date.now() - cached.ts < ttlMs) {
            res.setHeader('X-API-Cache', 'HIT');
            return res.json(cached.data);
        }
        // If we have stale data, serve it immediately and revalidate in background
        if (cached && cached.data) {
            res.setHeader('X-API-Cache', 'STALE');
            res.json(cached.data);
            // Background revalidation (only if not already pending)
            if (!cached.pending) {
                this.apiCache.set(key, { ts: cached.ts, data: cached.data, pending: true });
                fetcher().then(data => {
                    this.apiCache.set(key, { ts: Date.now(), data, pending: false });
                }).catch(() => {
                    this.apiCache.set(key, { ts: cached.ts, data: cached.data, pending: false });
                });
            }
            return;
        }
        // No cached data at all - must wait for fetch
        res.setHeader('X-API-Cache', 'MISS');
        fetcher().then(data => {
            this.apiCache.set(key, { ts: Date.now(), data, pending: false });
            res.json(data);
        }).catch(e => {
            res.status(500).json({ error: e.message });
        });
    }

    async callRPC(method, params = [], timeoutMs = 15000) {
        return new Promise((resolve, reject) => {
            const postData = JSON.stringify({ method, params, id: Date.now() });
            const NODE_LEVEL_COMMANDS = [
                'verifymessage', 'getblockchaininfo', 'getnetworkinfo', 'getconnectioncount',
                'getpeerinfo', 'getmininginfo', 'getnetworkhashps', 'getblockhash',
                'getblockheader', 'getblock', 'getchaintips', 'getrawmempool',
                'getmempoolinfo', 'getdeploymentinfo', 'gettxoutsetinfo', 'verifychain',
                'getchaintxstats', 'ping', 'getnettotals', 'listbanned', 'getmemoryinfo',
                'estimatesmartfee', 'estimaterawfee', 'getdescriptorinfo', 'getindexinfo'
            ];
            const rpcPath = NODE_LEVEL_COMMANDS.includes(method) ? '/' : `/wallet/${this.rpcWallet}`;
            const options = {
                hostname: this.rpcUrl,
                port: this.rpcPort,
                path: rpcPath,
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Content-Length': Buffer.byteLength(postData),
                    'Authorization': this.rpcAuth
                },
                timeout: timeoutMs
            };
            const req = http.request(options, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        const result = JSON.parse(data);
                        if (method === 'verifymessage') {
                            console.log(`[RPC DEBUG] verifymessage response: ${data}`);
                        }
                        if (result.error) {
                            reject(new Error(result.error.message || 'RPC Error'));
                        } else {
                            resolve(result.result);
                        }
                    } catch (e) {
                        reject(new Error('RPC Parse Error: ' + e.message));
                    }
                });
            });
            req.on('error', (e) => reject(new Error('RPC Connection Error: ' + e.message)));
            req.on('timeout', () => { req.destroy(); reject(new Error(`RPC Timeout after ${timeoutMs / 1000}s`)); });
            req.write(postData);
            req.end();
        });
    }

    // Look up wallet address from API key — used for deterministic P2P routing
    async getWalletFromApiKey(api_key) {
        if (!api_key) return null;
        try {
            const keyRecord = this.api_keys.get(api_key);
            if (keyRecord && keyRecord.target_wallet) {
                return keyRecord.target_wallet;
            }
            // Fallback: search all keys
            for (const [k, v] of this.api_keys.entries()) {
                if (k === api_key && v.target_wallet) return v.target_wallet;
            }
        } catch (e) { /* silent */ }
        return null;
    }

    // ===== DYNAMIC WALLET RPC (uses /wallet/${walletName} path) =====
    async callRPCForWallet(walletName, method, params = []) {
        return new Promise((resolve, reject) => {
            const postData = JSON.stringify({ method, params, id: Date.now() });
            const options = {
                hostname: this.rpcUrl,
                port: this.rpcPort,
                path: `/wallet/${walletName}`,
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Content-Length': Buffer.byteLength(postData),
                    'Authorization': this.rpcAuth
                },
                timeout: 15000
            };
            const req = http.request(options, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        const result = JSON.parse(data);
                        if (result.error) {
                            reject(new Error(result.error.message || 'RPC Error'));
                        } else {
                            resolve(result.result);
                        }
                    } catch (e) {
                        reject(new Error('RPC Parse Error: ' + e.message));
                    }
                });
            });
            req.on('error', (e) => reject(new Error('RPC Connection Error: ' + e.message)));
            req.on('timeout', () => { req.destroy(); reject(new Error('RPC Timeout after 15s')); });
            req.write(postData);
            req.end();
        });
    }

    async verifyPaymentSignature(nonce, signature) {
        const data = this.paymentNonces.get(nonce);
        if (!data) {
            return { valid: false, error: 'Invalid or expired payment nonce. Please request a new one.' };
        }
        if (data.used) {
            return { valid: false, error: 'This payment nonce has already been used. Each signature can only be used once.' };
        }
        if (Date.now() > data.expires_at) {
            this.paymentNonces.delete(nonce);
            return { valid: false, error: 'Payment nonce has expired. Please request a new one.' };
        }

        // Mark used BEFORE async RPC call to prevent TOCTOU race
        // Node.js is single-threaded, so check+mark is atomic in synchronous code
        data.used = true;
        this.paymentNonces.set(nonce, data);

        try {
            let isValid = await this.callRPC('verifymessage', [data.wallet, signature, data.message]);
            if (!isValid) {
                data.used = false;
                this.paymentNonces.set(nonce, data);
                return { valid: false, error: 'Payment signature verification failed. You must sign with the wallet that owns these funds.' };
            }
        } catch (rpcErr) {
            data.used = false;
            this.paymentNonces.set(nonce, data);
            return { valid: false, error: 'Blockchain signature verification unavailable: ' + rpcErr.message };
        }

        console.log(`[PaymentNonce] Verified and consumed: ${nonce.substring(0,16)}... amount=${data.amount}`);
        return { valid: true, data };
    }

    setupRoutes() {
        // Health check
        this.app.get('/health', (req, res) => {
            res.json({
                status: 'ok',
                timestamp: Date.now(),
                miners_online: this.getOnlineMinersCount(),
                total_miners: this.miners.size,
                comments_count: this.comments.length
            });
        });

        // Public miner list (same as /api/p2p/miners)
        // ===== P2P Miners List (Unified - merged from dual definitions) =====
        this.app.get('/api/miners', async (req, res) => {
            try {
                const allMiners = Array.from(this.miners.values()).map(m => {
                    const computedTokenRatio = m.token_ratio || (m.price_per_1m_tknc ? Math.round(1000000 / m.price_per_1m_tknc) : 100000);
                    // public_ip is required per TKNC Manual §10.2
// External users need node's public IP to call API Gateway (:9313)
// Miner API (:9332) remains localhost-only — api_endpoint points to node's API Gateway
// IPv6 addresses are globally routable public addresses.
// They MUST be preserved for true remote P2P inference routing.
// Port corrected to 9313 (API Gateway port per inference_gateway.cpp)
let publicIp = m.public_ip || '';
// Strip ::ffff: prefix only when it wraps an IPv4 address (IPv4-mapped IPv6).
// Pure IPv6 addresses (e.g., 2408:8244:...) are kept as-is for remote routing.
if (publicIp.startsWith('::ffff:')) {
    publicIp = publicIp.substring(7);
}
const apiGatewayPort = 9313;  // Node's OpenAI-compatible HTTP API Gateway (port 9313, not 8080)
                    let apiEndpoint = '';
                    if (publicIp && publicIp !== '127.0.0.1' && publicIp !== '::1') {
                        // IPv6 addresses contain ':' — must be wrapped in brackets per RFC 3986
                        apiEndpoint = publicIp.includes(':')
                            ? `http://[${publicIp}]:${apiGatewayPort}/v1/chat/completions`
                            : `http://${publicIp}:${apiGatewayPort}/v1/chat/completions`;
                    }
                    // Online determination (multi-signal):
                    //   1. status=offline (node TCP-probed miner HTTP port and failed) -> OFFLINE
                    //   2. last_heartbeat stale (>MINER_TIMEOUT) -> OFFLINE
                    //   3. has_tunnel=true (explicit tunnel active) -> ONLINE (override)
                    const now = Date.now();
                    const hbAge = now - (m.last_heartbeat || 0);
                    const hbFresh = hbAge < this.MINER_TIMEOUT;
                    const statusOffline = (m.status || '').toLowerCase() === 'offline';
                    const isOnline = m.has_tunnel ? true : (!statusOffline && hbFresh);
                    return {
                        miner_id: m.miner_id,
                        wallet: m.wallet,
                        model_name: m.model_name,
                        public_ip: publicIp,           // Node's public IP (IPv4/IPv6/domain) for remote API calls
                        api_endpoint: apiEndpoint,     // Full OpenAI-compatible endpoint URL (node's :9313)
                        api_gateway_port: apiGatewayPort,
                        status: isOnline ? 'online' : 'offline',
                        gpu_load: m.gpu_load || 0,
                        gpu_info: `${m.gpu_load || 0}% load`,
                        vram_mb: m.vram_mb || 4096,
                        hashrate: m.hashrate || 0,
                        uptime_seconds: Math.floor((now - (m.registered_at || now)) / 1000),
                        is_online: isOnline,
                        model_loaded: !!m.model_name,
                        last_heartbeat: m.last_heartbeat,
                        has_tunnel: m.has_tunnel || false,
                        tunnel_status: m.tunnel_status || 'none',
                        connection_type: m.connection_type || 'direct',
                        token_ratio: computedTokenRatio,
                        price_per_1m_tknc: m.price_per_1m_tknc || 10
                    };
                });

                res.json({
                    count: allMiners.length,
                    online: allMiners.filter(m => m.is_online).length,
                    miners: allMiners
                });
            } catch (error) {
                console.error('Error fetching miners:', error);
                res.status(500).json({ error: 'Failed to fetch miners' });
            }
        });

        // ===== REAL LOGIN (signature verification) =====
        // Step 1: Get nonce for signing
        this.app.post('/api/login/init', (req, res) => {
            try {
                const { wallet } = req.body;
                if (!wallet || !wallet.startsWith('token1')) {
                    return res.status(400).json({ success: false, error: 'Invalid wallet address' });
                }
                const nonce = crypto.randomBytes(32).toString('hex');
                const timestamp = Date.now();
                const message = nonce;
                this.nonceStore.set(wallet, { nonce, timestamp, message, expires: timestamp + 300000 });
                console.log(`[Auth] Nonce issued for ${wallet.substring(0, 15)}...`);
                res.json({ success: true, nonce, message, timestamp });
            } catch (error) {
                res.status(500).json({ success: false, error: error.message });
            }
        });

        // Step 2: Verify signature and create session
        this.app.post('/api/login/verify', async (req, res) => {
            try {
                const { wallet, signature, nonce } = req.body;
                if (!wallet || !signature || !nonce) {
                    return res.status(400).json({ success: false, error: 'Wallet, signature, and nonce required' });
                }

                const stored = this.nonceStore.get(wallet);
                if (!stored) {
                    return res.status(401).json({ success: false, error: 'No nonce found. Call /api/login/init first' });
                }
                if (stored.nonce !== nonce) {
                    return res.status(401).json({ success: false, error: 'Nonce mismatch' });
                }
                if (Date.now() > stored.expires) {
                    this.nonceStore.delete(wallet);
                    return res.status(401).json({ success: false, error: 'Nonce expired (5 minute limit)' });
                }

                // ===== REAL VERIFICATION: on-chain signature verification =====
                // Private key never leaves the client, server uses verifymessage
                let isValid = false;

                // Use stored original message (must match what client signed)
                const verifyMessage = stored.message.normalize();

                try {
                    isValid = await this.callRPC('verifymessage', [wallet, signature, verifyMessage]);
                    console.log(`[Auth] verifymessage result for ${wallet.substring(0, 15)}...: ${isValid}`);
                } catch (rpcErr) {
                    console.error('[Auth] RPC verifymessage failed:', rpcErr.message);
                    return res.status(500).json({ success: false, error: 'Blockchain verification unavailable: ' + rpcErr.message });
                }

                if (!isValid) {
                    // FIX: Do NOT delete the nonce on failed signature verification.
                    // This allows the user to retry with a corrected signature without
                    // needing to call /api/login/init again. The nonce will be:
                    //   - Overwritten on a new /api/login/init call
                    //   - Deleted on successful verification
                    //   - Deleted on expiration (5 min)
                    return res.status(401).json({ success: false, error: 'Signature verification failed. You do not own this wallet. Please check that you are signing with the correct address and the exact nonce message.' });
                }

                // Clean up nonce
                this.nonceStore.delete(wallet);

                // Create authenticated session
                const sessionToken = 'tknc_' + crypto.randomBytes(32).toString('hex');
                const csrfToken = crypto.randomBytes(32).toString('hex');
                this.sessions.set(sessionToken, {
                    wallet: wallet,
                    created_at: Date.now(),
                    last_activity: Date.now(),
                    is_active: true,
                    auth_method: 'signmessage',
                    nonce_used: nonce,
                    csrf_token: csrfToken
                });

                console.log(`[Auth] User authenticated via signature: ${wallet.substring(0, 15)}...`);

                // Resolve wallet name: find which wallet file contains this address
                let resolvedWalletName = null;
                try {
                    const walletList = await this.callRPC('listwallets', []);
                    if (Array.isArray(walletList)) {
                        for (const wname of walletList) {
                            try {
                                const addrInfo = await this.callRPCForWallet(wname, 'getaddressinfo', [wallet]);
                                if (addrInfo && addrInfo.ismine) {
                                    resolvedWalletName = wname;
                                    console.log(`[Auth] Resolved wallet name: ${wallet.substring(0, 15)}... → ${wname}`);
                                    break;
                                }
                            } catch (e) { /* address not in this wallet */ }
                        }
                    }
                } catch (e) {
                    console.warn('[Auth] Could not resolve wallet name:', e.message);
                }

                // Fallback for DEV mode or when RPC unavailable
                if (!resolvedWalletName) {
                    resolvedWalletName = wallet; // Use wallet address as fallback
                    console.log(`[Auth] Using wallet address as fallback name: ${wallet.substring(0, 15)}...`);
                }

                this.userWallets.set(sessionToken, resolvedWalletName);
                this.walletNameByAddress.set(wallet, resolvedWalletName);
                // ===== END USER WALLET MAPPING =====

                res.json({
                    success: true,
                    message: 'Signature verified successfully',
                    session_token: sessionToken,
                    csrf_token: csrfToken,
                    wallet: wallet,
                    wallet_name: resolvedWalletName,
                    expires_in: this.SESSION_TIMEOUT
                });
            } catch (error) {
                console.error('[Auth] Verify error:', error.message);
                res.status(500).json({ success: false, error: 'Internal server error: ' + error.message });
            }
        });

        // Legacy login (present for backward compatibility)
        this.app.post('/api/login', (req, res) => {
            res.status(400).json({
                success: false,
                error: 'Insecure login disabled. Use /api/login/init then /api/login/verify with signature.',
                instructions: 'POST /api/login/init with {wallet} → get nonce → sign with wallet → POST /api/login/verify with {wallet, signature, nonce}'
            });
        });

        // Logout endpoint
        this.app.post('/api/logout', (req, res) => {
            try {
                const { session_token } = req.body;

                if (!session_token || !this.sessions.has(session_token)) {
                    return res.status(401).json({ success: false, error: 'Not authenticated' });
                }

                const session = this.sessions.get(session_token);
                session.is_active = false;
                this.sessions.delete(session_token);
                console.log(`[Auth] User logged out`);

                res.json({ success: true, message: 'Logged out successfully' });
            } catch (error) {
                res.status(500).json({ success: false, error: 'Logout failed' });
            }
        });

        // ===== PAYMENT NONCE: Generate one-time payment signing challenge =====
        this.app.post('/api/payment/nonce', (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);

                const { amount, recipient, purpose, target_miner_id } = req.body;
                // Amount is now optional - for inference wallet mode, we don't require it
                if (!recipient || !recipient.startsWith('token1')) {
                    return res.status(400).json({ success: false, error: 'Valid recipient wallet address required' });
                }
                if (!purpose) {
                    return res.status(400).json({ success: false, error: 'Payment purpose required' });
                }

                const wallet = session.wallet;
                const nonce = crypto.randomBytes(24).toString('hex');
                const expires_at = Date.now() + 60000;

                let tokenInfo = '';
                let effectiveTokenRatio = 100000;
                if (target_miner_id) {
                    const miner = this.miners.get(target_miner_id);
                    if (miner) {
                        effectiveTokenRatio = miner.token_ratio || 100000;
                        tokenInfo = [
                            `Miner ID: ${target_miner_id}`,
                            `Token Rate: 1 TKNC = ${this.formatTokenRatio(effectiveTokenRatio)}`,
                        ].join('\n');
                        
                        // Only show token calculation if amount is provided
                        if (amount && amount > 0) {
                            const totalTokens = amount * effectiveTokenRatio;
                            tokenInfo += `\nYou receive: ${this.formatTokenRatio(totalTokens)} worth of LLM tokens`;
                        }
                    }
                }

                // Build message - amount is optional for inference wallet mode
                const messageLines = [
                    `SIGNATURE: TKNC Network`,
                    `Recipient: ${recipient}`,
                    `Purpose: ${purpose}`,
                    tokenInfo,
                    `Nonce: ${nonce}`,
                    `Expires: ${new Date(expires_at).toISOString()}`,
                    `Wallet: ${wallet}`,
                ];
                
                if (amount && amount > 0) {
                    messageLines.splice(1, 0, `Amount: ${amount} TKNC`);
                    messageLines.push(`This signature ONLY authorizes ${amount} TKNC. Even if hacked, only ${amount} TKNC can be moved. Nonce: ONE-TIME USE only.`);
                } else {
                    messageLines.push(`This signature authorizes API Key creation using inference wallet. Nonce: ONE-TIME USE only.`);
                }

                const message = messageLines.filter(line => line).join('\n');

                this.paymentNonces.set(nonce, {
                    wallet,
                    amount: amount || 0,
                    recipient,
                    purpose,
                    message,
                    created_at: Date.now(),
                    used: false,
                    expires_at
                });

                console.log(`[PaymentNonce] Generated for ${wallet.substring(0,15)}... amount=${amount || 'N/A (inference wallet)'} purpose=${purpose}`);
                res.json({ success: true, nonce, message, expires_at });
            } catch (error) {
                res.status(500).json({ success: false, error: 'Failed to generate payment nonce' });
            }
        });

        // Clean expired payment nonces every 30 seconds
        setInterval(() => {
            const now = Date.now();
            let cleaned = 0;
            for (const [nonce, data] of this.paymentNonces) {
                if (data.expires_at < now) {
                    this.paymentNonces.delete(nonce);
                    cleaned++;
                }
            }
            if (cleaned > 0) {
                console.log(`[PaymentNonce] Cleaned ${cleaned} expired nonces`);
            }
        }, 30000);

        // Get current user info
        this.app.get('/api/user/info', (req, res) => {
            const sessionToken = req.headers['x-session-token'];

            if (!sessionToken || !this.sessions.has(sessionToken)) {
                return res.status(401).json({ error: 'Not authenticated' });
            }

            const session = this.sessions.get(sessionToken);
            session.last_activity = Date.now();

            res.json({
                authenticated: true,
                wallet: session.wallet,
                login_time: new Date(session.created_at).toISOString()
            });
        });

        // ===== WALLET BALANCE & USAGE API =====

        // Get wallet balance via RPC (from user's own wallet, not test_wallet)
        this.app.get('/api/wallet/balance', async (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Not authenticated. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                const walletName = this.userWallets.get(sessionToken);

                if (!walletName) {
                    return res.status(400).json({ success: false, error: 'No wallet found for this session. Please log out and log in again.' });
                }

                session.last_activity = Date.now();

                let balance = 0, unconfirmed = 0;
                try {
                    balance = await this.callRPCForWallet(walletName, 'getbalance', []);
                    console.log(`[Wallet] Balance for ${walletName}: ${balance} TKNC`);
                } catch (rpcErr) {
                    // Per architecture: Web server is yellow-pages only — user wallets live on
                    // the user's local node, not on the seed node. When the wallet is not loaded
                    // here, report balance = 0 rather than erroring, so the UI can render.
                    console.log(`[Wallet] Wallet ${walletName} not loaded on seed node (expected): ${rpcErr.message}`);
                    balance = 0;
                }

                try {
                    unconfirmed = await this.callRPCForWallet(walletName, 'getunconfirmedbalance', []);
                } catch (e) {
                    unconfirmed = 0;
                }

                res.json({
                    success: true,
                    wallet: session.wallet,
                    wallet_name: walletName,
                    balance: balance,
                    unconfirmed_balance: unconfirmed,
                    available_balance: balance
                });
            } catch (error) {
                res.status(500).json({ success: false, error: error.message });
            }
        });

        // Get API key usage (remaining = declared_limit - consumed; no locking, no refund)
        this.app.get('/api/wallet/usage', (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ error: 'Not authenticated' });
                }
                const session = this.sessions.get(sessionToken);
                session.last_activity = Date.now();

                const userWallet = session.wallet;
                const apiKeys = [];
                let totalLimit = 0;
                let totalConsumed = 0;

                this.inferenceHistory.forEach((record, apiKey) => {
                    if (record.user_wallet === userWallet) {
                        const limit = record.declared_limit || 0;
                        const consumed = record.consumed_balance || 0;
                        // 0 = unlimited: show 'unlimited' instead of 0
                        const remaining = (limit === 0) ? 'unlimited' : Math.max(0, limit - consumed);

                        apiKeys.push({
                            api_key: apiKey.substring(0, 16) + '...',
                            miner_id: record.miner_id,
                            model: record.model,
                            declared_limit: limit,
                            consumed_balance: consumed,
                            remaining_balance: remaining,
                            call_count: record.call_count || 0,
                            created_at: record.created_at,
                            used_at: record.used_at
                        });

                        totalLimit += limit;  // 0 = unlimited, adds 0 (display logic handles 'unlimited' separately)
                        totalConsumed += consumed;
                    }
                });

                res.json({
                    success: true,
                    wallet: userWallet,
                    api_keys: apiKeys,
                    total_limit: totalLimit,
                    total_consumed: totalConsumed,
                    total_remaining: Math.max(0, totalLimit - totalConsumed)
                });
            } catch (error) {
                res.status(500).json({ success: false, error: error.message });
            }
        });

        // ===== COMMENTS API =====

        // Get all comments
        this.app.get('/api/comments', (req, res) => {
            try {
                // Return latest 100 comments (sorted by time descending)
                const recentComments = this.comments
                    .slice(-100)
                    .reverse()
                    .map(c => ({
                        id: c.id,
                        author: c.author,
                        content: c.content,
                        rating: c.rating,
                        wallet: c.wallet || null,
                        wallet_display: c.wallet?.substring(0, 20) + '...',
                        target_miner: c.target_miner || null,
                        target_miner_wallet: c.target_miner_wallet || null,
                        payment_amount: c.payment_amount || 10,
                        payment_type: c.payment_type || 'user_to_miner',
                        payment_direction: c.payment_direction || 'user_pays_miner',
                        blockchain_txid: c.blockchain_txid || null,
                        blockchain_verified: c.blockchain_verified || false,
                        inference_time: c.inference_time || null,
                        usage_duration: c.usage_duration || null,
                        created_at: c.created_at
                    }));

                res.json({
                    success: true,
                    count: recentComments.length,
                    total: this.comments.length,
                    comments: recentComments
                });
            } catch (error) {
                console.error('[Comments] Get error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to get comments' });
            }
        });

        // Get comments for a specific miner (with average rating)
        this.app.get('/api/comments/by_miner/:miner_id', (req, res) => {
            try {
                const minerId = req.params.miner_id;
                const minerComments = this.comments
                    .filter(c => c.target_miner === minerId)
                    .slice(-50)
                    .reverse()
                    .map(c => ({
                        id: c.id,
                        author: c.author,
                        content: c.content,
                        rating: c.rating,
                        wallet: c.wallet || null,
                        wallet_display: c.wallet?.substring(0, 20) + '...',
                        target_miner: c.target_miner || null,
                        target_miner_wallet: c.target_miner_wallet || null,
                        payment_amount: c.payment_amount || 10,
                        payment_type: c.payment_type || 'user_to_miner',
                        blockchain_txid: c.blockchain_txid || null,
                        blockchain_verified: c.blockchain_verified || false,
                        inference_time: c.inference_time || null,
                        usage_duration: c.usage_duration || null,
                        created_at: c.created_at
                    }));

                const ratedComments = this.comments.filter(c => c.target_miner === minerId && c.rating);
                const avgRating = ratedComments.length > 0
                    ? (ratedComments.reduce((sum, c) => sum + c.rating, 0) / ratedComments.length).toFixed(1)
                    : null;

                res.json({
                    success: true,
                    miner_id: minerId,
                    count: minerComments.length,
                    total: this.comments.filter(c => c.target_miner === minerId).length,
                    average_rating: avgRating ? parseFloat(avgRating) : null,
                    rated_count: ratedComments.length,
                    comments: minerComments
                });
            } catch (error) {
                console.error('[Comments] Get by miner error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to get comments' });
            }
        });

        // Get all miners' average ratings (lightweight)
        this.app.get('/api/comments/ratings', (req, res) => {
            try {
                const ratings = {};
                for (const c of this.comments) {
                    if (c.target_miner && c.rating) {
                        if (!ratings[c.target_miner]) {
                            ratings[c.target_miner] = { sum: 0, count: 0 };
                        }
                        ratings[c.target_miner].sum += c.rating;
                        ratings[c.target_miner].count++;
                    }
                }
                const result = {};
                for (const [minerId, data] of Object.entries(ratings)) {
                    result[minerId] = {
                        average: parseFloat((data.sum / data.count).toFixed(1)),
                        count: data.count
                    };
                }
                res.json({ success: true, ratings: result });
            } catch (error) {
                console.error('[Comments] Ratings error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to get ratings' });
            }
        });

        // Post new comment (REAL ON-CHAIN PAYMENT from user's wallet)
        this.app.post('/api/comments', async (req, res) => {
            try {
                // ===== SESSION AUTH: extract session_token =====
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                const walletName = this.userWallets.get(sessionToken) || session.wallet;  // fallback to session wallet

                if (!walletName && !session.wallet) {
                    return res.status(400).json({ success: false, error: 'No wallet found for your session. Please log out and log in again.' });
                }

                const { author, content, rating, target_miner, target_miner_wallet, payment_direction } = req.body;
                const userWallet = session.wallet;

                // Auto-detect invite: if miner invited this user, use miner_pays_user direction
                let direction = payment_direction || 'user_pays_miner';
                if (direction === 'user_pays_miner' && target_miner_wallet && userWallet) {
                    const inviteKey = `${target_miner_wallet}_${userWallet}`;
                    const inviteRecord = this.commentInvites.get(inviteKey);
                    if (inviteRecord && inviteRecord.status === 'paid' && !inviteRecord.used) {
                        direction = 'miner_pays_user';
                        console.log(`[Comments] Auto-detected invite from ${target_miner_wallet?.substring(0,15)}... to ${userWallet?.substring(0,15)}...`);
                    }
                }

                const paymentAmount = direction === 'miner_pays_user' ? 0 : 10; // invited comments have no payment

                // ===== Mandatory validation: target miner required =====
                if (!target_miner) {
                    return res.status(400).json({
                        success: false,
                        error: 'Target miner is required. Cannot determine TKNC payment destination without specifying a miner.',
                        code: 'TARGET_MINER_REQUIRED'
                    });
                }

                if (!target_miner_wallet) {
                    return res.status(400).json({
                        success: false,
                        error: 'Target miner wallet address is required for payment.',
                        code: 'TARGET_WALLET_REQUIRED'
                    });
                }

                // ===== Verify comment eligibility =====
                // user_pays_miner: user must have used the miner's service
                // miner_pays_user: miner invites comment, no usage record needed
                if (direction === 'user_pays_miner') {
                    if (userWallet && target_miner) {
                        const eligibility = this.checkCommentEligibility(userWallet, target_miner);
                        if (!eligibility.eligible) {
                            console.warn(`[Comments] Rejected comment from ${userWallet?.substring(0,15)}... to miner ${target_miner?.substring(0,15)}...`);
                            return res.status(403).json({
                                success: false,
                                error: eligibility.reason || 'You are not authorized to comment on this miner',
                                code: 'ELIGIBILITY_CHECK_FAILED',
                                hint: 'Please purchase an API Key from this miner and use its inference service before commenting.'
                            });
                        }
                    }
                } else if (direction === 'miner_pays_user') {
                    // Verify miner invite record exists
                    const inviteKey = `${target_miner_wallet}_${userWallet}`;
                    const inviteRecord = this.commentInvites.get(inviteKey);
                    if (!inviteRecord || inviteRecord.status !== 'paid') {
                        return res.status(403).json({
                            success: false,
                            error: 'This miner has not invited you to comment, or the invitation payment is not completed.',
                            code: 'NO_INVITE_FOUND'
                        });
                    }
                    if (inviteRecord.used) {
                        return res.status(403).json({
                            success: false,
                            error: 'This invitation has already been used. Each invitation allows only one comment.',
                            code: 'INVITE_ALREADY_USED'
                        });
                    }
                }

                // Verify required fields
                if (!author || !content) {
                    return res.status(400).json({ success: false, error: 'Author and content are required' });
                }
                if (content.length > 500) {
                    return res.status(400).json({ success: false, error: 'Content too long (max 500 chars)' });
                }
                if (author.length > 50) {
                    return res.status(400).json({ success: false, error: 'Author name too long (max 50 chars)' });
                }
                if (rating && (rating < 1 || rating > 5)) {
                    return res.status(400).json({ success: false, error: 'Rating must be between 1 and 5' });
                }

                const sanitizeInput = (str, maxLen) => {
                    if (typeof str !== 'string') return '';
                    // Remove null bytes and control characters
                    let cleaned = str.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F]/g, '');
                    // Truncate to max length
                    if (maxLen && cleaned.length > maxLen) cleaned = cleaned.substring(0, maxLen);
                    return cleaned;
                };
                const safeAuthor = sanitizeInput(author, 50);
                const safeContent = sanitizeInput(content, 500);

                // ===== REAL ON-CHAIN PAYMENT =====
                let txid = null;
                let payerAddr;
                let receiverAddr = null;
                let currentBalance = null;

                if (direction === 'user_pays_miner') {
                    payerAddr = userWallet;
                    receiverAddr = target_miner_wallet;

                    // ===== PAYMENT SIGNATURE VERIFICATION =====
                    const { payment_nonce, payment_signature } = req.body;
                    if (!payment_nonce || !payment_signature) {
                        return res.status(400).json({ success: false, error: 'Payment nonce and signature required. Each payment must be signed by the wallet owner.', code: 'PAYMENT_SIGNATURE_REQUIRED' });
                    }
                    const sigCheck = await this.verifyPaymentSignature(payment_nonce, payment_signature);
                    if (!sigCheck.valid) {
                        return res.status(403).json({ success: false, error: sigCheck.error, code: 'PAYMENT_SIGNATURE_INVALID' });
                    }
                    if (sigCheck.data.amount !== paymentAmount || sigCheck.data.recipient !== target_miner_wallet) {
                        return res.status(400).json({ success: false, error: 'Payment signature does not match the transaction details.', code: 'SIGNATURE_MISMATCH' });
                    }

                    try {
                        currentBalance = await this.callRPCForWallet(walletName, 'getbalance', []);
                    } catch (e) {
                        return res.status(500).json({ success: false, error: 'Balance check failed', code: 'BALANCE_CHECK_FAILED' });
                    }
                    if (currentBalance < paymentAmount) {
                        return res.status(402).json({
                            success: false, error: `Insufficient balance. Current: ${currentBalance} TKNC, Required: ${paymentAmount} TKNC`,
                            code: 'INSUFFICIENT_BALANCE', current_balance: currentBalance, required: paymentAmount
                        });
                    }
                } else {
                    // miner_pays_user: payment already done via invite
                    payerAddr = target_miner_wallet;
                    receiverAddr = userWallet;
                }

                // ===== Get remaining balance after payment =====
                let remainingBalance = null;
                try {
                    remainingBalance = await this.callRPCForWallet(walletName, 'getbalance', []);
                } catch (e) {
                    remainingBalance = null;
                }

                try {
                    if (direction === 'user_pays_miner') {
                        txid = await this.callRPCForWallet(walletName, 'sendtoaddress', [target_miner_wallet, paymentAmount]);
                        console.log(`[Comments][ON-CHAIN] ${walletName} → ${target_miner_wallet.substring(0,15)}... ${paymentAmount} TKNC TXID: ${txid}`);
                    } else {
                        // miner_pays_user: payment already done via invite endpoint
                        const inviteKey = `${target_miner_wallet}_${userWallet}`;
                        const inviteRecord = this.commentInvites.get(inviteKey);
                        txid = inviteRecord?.txid || 'invite_no_txid';
                        console.log(`[Comments][INVITE] ${target_miner_wallet.substring(0,15)}... → ${userWallet.substring(0,15)}... comment (invite TXID: ${txid})`);
                    }
                } catch (rpcErr) {
                    console.error('[Comments][ON-CHAIN] Payment failed:', rpcErr.message);
                    return res.status(500).json({
                        success: false, error: 'Blockchain payment failed: ' + rpcErr.message, code: 'PAYMENT_FAILED'
                    });
                }

                // Invite consumed marker
                if (direction === 'miner_pays_user') {
                    const inviteKey = `${target_miner_wallet}_${userWallet}`;
                    const inviteRecord = this.commentInvites.get(inviteKey);
                    if (inviteRecord) inviteRecord.used = true;
                }

                const newComment = {
                    id: 'comment_' + Date.now() + '_' + Math.random().toString(36).substr(2, 9),
                    author: safeAuthor,
                    content: safeContent,
                    rating: rating ? parseInt(rating) : null,
                    wallet: userWallet,
                    target_miner: target_miner || null,
                    target_miner_wallet: target_miner_wallet || null,
                    payment_amount: paymentAmount,
                    payment_type: direction === 'user_pays_miner' ? 'user_to_miner' : 'miner_to_user',
                    payment_direction: direction,
                    payer_addr: payerAddr,
                    receiver_addr: direction === 'user_pays_miner' ? target_miner_wallet : userWallet,
                    blockchain_txid: txid,
                    blockchain_verified: true,
                    created_at: new Date().toISOString(),
                    ip_address: req.ip,
                    eligibility_verified: true,
                    payer_wallet_name: walletName
                };

                this.comments.unshift(newComment);

                if (this.comments.length > this.MAX_COMMENTS) {
                    this.comments = this.comments.slice(0, this.MAX_COMMENTS);
                }

                console.log(`[Comments] New: ${newComment.id} dir=${direction} payer=${payerAddr?.substring(0,15)}... → ${receiverAddr?.substring(0,15)}... TXID=${txid?.substring(0,20)}...`);

                res.status(201).json({
                    success: true,
                    comment: newComment,
                    blockchain_txid: txid,
                    payment_amount: paymentAmount,
                    payment_direction: direction,
                    remaining_balance: remainingBalance,
                    message: direction === 'miner_pays_user'
                        ? 'Miner paid 10 TKNC, your comment has been published (miner can delete this comment)'
                        : 'Comment published successfully, 10 TKNC sent to miner (you can delete this comment)'
                });

            } catch (error) {
                console.error('[Comments] Post error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to create comment: ' + error.message });
            }
        });

        // ===== INVITE COMMENT: Miner pays to invite user to comment =====
        this.app.post('/api/comments/invite', async (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);

                const { target_user_wallet } = req.body;
                if (!target_user_wallet || !target_user_wallet.startsWith('token1')) {
                    return res.status(400).json({ success: false, error: 'Invalid target user wallet address' });
                }

                const minerWallet = session.wallet;
                const walletName = this.userWallets.get(sessionToken) || this.rpcWallet;

                // ===== PAYMENT SIGNATURE VERIFICATION =====
                const { payment_nonce, payment_signature } = req.body;
                if (!payment_nonce || !payment_signature) {
                    return res.status(400).json({ success: false, error: 'Payment nonce and signature required. Each payment must be signed by the wallet owner.', code: 'PAYMENT_SIGNATURE_REQUIRED' });
                }
                const sigCheck = await this.verifyPaymentSignature(payment_nonce, payment_signature);
                if (!sigCheck.valid) {
                    return res.status(403).json({ success: false, error: sigCheck.error, code: 'PAYMENT_SIGNATURE_INVALID' });
                }
                if (sigCheck.data.amount !== 10 || sigCheck.data.recipient !== target_user_wallet) {
                    return res.status(400).json({ success: false, error: 'Payment signature does not match the transaction details. Amount or recipient mismatch.', code: 'SIGNATURE_MISMATCH' });
                }

                // Check balance
                let balance;
                try {
                    balance = await this.callRPCForWallet(walletName, 'getbalance', []);
                } catch (e) {
                    return res.status(500).json({ success: false, error: 'Balance check failed' });
                }
                if (balance < 10) {
                    return res.status(402).json({ success: false, error: `Insufficient balance: ${balance} TKNC, need 10 TKNC`, code: 'INSUFFICIENT_BALANCE' });
                }

                // Send 10 TKNC from miner to user
                let txid;
                try {
                    txid = await this.callRPCForWallet(walletName, 'sendtoaddress', [target_user_wallet, 10]);
                    console.log(`[Invite] ${minerWallet.substring(0,15)}... → ${target_user_wallet.substring(0,15)}... 10 TKNC TXID: ${txid}`);
                } catch (e) {
                    return res.status(500).json({ success: false, error: 'Blockchain payment failed: ' + e.message });
                }

                // Store invite record
                const inviteKey = `${minerWallet}_${target_user_wallet}`;
                this.commentInvites.set(inviteKey, {
                    miner_wallet: minerWallet,
                    user_wallet: target_user_wallet,
                    status: 'paid',
                    paid_at: Date.now(),
                    txid: txid,
                    used: false
                });

                let remainingBalance;
                try {
                    remainingBalance = await this.callRPCForWallet(walletName, 'getbalance', []);
                } catch (e) {
                    remainingBalance = null;
                }

                res.json({
                    success: true,
                    message: 'Invitation paid. User can now comment on your miner.',
                    invite_key: inviteKey,
                    blockchain_txid: txid,
                    payment_amount: 10,
                    remaining_balance: remainingBalance
                });

            } catch (error) {
                console.error('[Invite] Error:', error.message);
                res.status(500).json({ success: false, error: 'Invite failed: ' + error.message });
            }
        });

        // Get comment invites for a miner
        this.app.get('/api/comments/invites', (req, res) => {
            const sessionToken = req.headers['x-session-token'];
            if (!sessionToken || !this.sessions.has(sessionToken)) {
                return res.status(401).json({ success: false, error: 'Authentication required' });
            }
            const session = this.sessions.get(sessionToken);
            const invites = [];
            for (const [key, record] of this.commentInvites) {
                if (record.miner_wallet === session.wallet || record.user_wallet === session.wallet) {
                    invites.push({ key, ...record });
                }
            }
            res.json({ success: true, invites });
        });

        // Delete comment (SESSION VERIFIED - payer can delete)
        this.app.delete('/api/comments/:id', async (req, res) => {
            try {
                const { id } = req.params;
                const sessionToken = req.headers['x-session-token'];

                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }

                const session = this.sessions.get(sessionToken);
                const wallet = session.wallet;

                const index = this.comments.findIndex(c => c.id === id);
                if (index === -1) {
                    return res.status(404).json({ success: false, error: 'Comment not found' });
                }

                const comment = this.comments[index];

                let canDelete = false;
                if (comment.payment_type === 'user_to_miner') {
                    canDelete = (comment.wallet === wallet);
                } else if (comment.payment_type === 'miner_to_user') {
                    canDelete = (comment.target_miner_wallet === wallet);
                } else {
                    canDelete = (comment.wallet === wallet);
                }

                if (!canDelete) {
                    console.log(`[Comments] Unauthorized delete: session wallet ${wallet?.substring(0,15)}... attempted on comment ${id} owned by ${comment.wallet?.substring(0,15)}...`);
                    return res.status(403).json({
                        success: false,
                        error: 'Permission denied: Only the payer can delete this comment'
                    });
                }

                const deletedComment = this.comments.splice(index, 1)[0];
                console.log(`[Comments] Comment deleted by ${wallet?.substring(0,15)}...: ${id}`);

                res.json({
                    success: true,
                    message: 'Comment deleted successfully',
                    deleted_comment_id: id,
                    deleted_by: wallet
                });

            } catch (error) {
                console.error('[Comments] Delete error:', error.message);
                res.status(500).json({ success: false, error: 'Delete failed' });
            }
        });

        // ===== Comment system enhancement - inference history based eligibility =====

        // Get eligible miners for user to comment on
        this.app.get('/api/comments/eligible_miners', (req, res) => {
            try {
                const { wallet } = req.query;

                if (!wallet) {
                    return res.status(400).json({
                        success: false,
                        error: 'Wallet address required',
                        eligible_miners: []
                    });
                }

                const eligibleMiners = this.getUserEligibleMiners(wallet);

                console.log(`[Comments] User ${wallet.substring(0, 15)}... requested eligible miners: ${eligibleMiners.length} found`);

                res.json({
                    success: true,
                    wallet: wallet,
                    eligible_miners: eligibleMiners,
                    message: eligibleMiners.length > 0
                        ? `Found ${eligibleMiners.length} eligible miners`
                        : 'You have not used any miner models yet, cannot post comments'
                });

            } catch (error) {
                console.error('[Comments] Eligible miners error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to fetch eligible miners' });
            }
        });

        // Check if user is eligible to comment on a specific miner
        this.app.get('/api/comments/check_eligibility', (req, res) => {
            try {
                const { wallet, target_miner_id } = req.query;

                if (!wallet || !target_miner_id) {
                    return res.status(400).json({
                        eligible: false,
                        error: 'Wallet and target_miner_id required'
                    });
                }

                const eligibility = this.checkCommentEligibility(wallet, target_miner_id);

                res.json(eligibility);

            } catch (error) {
                console.error('[Comments] Check eligibility error:', error.message);
                res.status(500).json({ eligible: false, error: 'Check failed' });
            }
        });

        // ===== P2P GATEWAY ROUTES =====

        // P2P Node registration with role and capabilities
        this.app.post('/api/p2p/register', async (req, res) => {
            console.log('[P2P-REG] POST /api/p2p/register received!');
            try {
                // Use raw body (collected before express.json) as primary, fallback to req.body
                const src = req.rawBody || req.body;

                if (!src) {
                    console.log('[P2P-REG] RAW BODY STRING (for debug):', (req._rawBodyStr || '').substring(0, 500));
                }
                const { node_id, miner_id, wallet, wallet_address, model_name, api_port, public_ip,
                        role, capabilities, p2p_port, ws_port, signature, nonce } = src || {};
                const actualWallet = wallet || wallet_address;
                const actualNodeId = node_id || miner_id;

                if (!actualNodeId || (!actualWallet && role !== 'client')) {
                    return res.status(400).json({ error: 'node_id and wallet required' });
                }

                if (role !== 'client' && actualWallet) {
                    if (!signature || !nonce) {
                        console.log(`[P2P-REG] REJECTED: Missing signature/nonce from ${actualNodeId}`);
                        return res.status(401).json({
                            success: false,
                            error: 'Signature and nonce required for node/miner registration'
                        });
                    }

                    // Verify the signature matches the wallet
                    const message = `TKNC-P2P-REGISTER-${nonce}-${actualNodeId}`;
                    let isValid = false;
                    try {
                        const verifyResult = await this.callRPC('verifymessage', [actualWallet, signature, message]);
                        isValid = verifyResult === true;
                    } catch (e) {
                        console.log(`[P2P-REG] Signature verification error: ${e.message}`);
                    }

                    if (!isValid) {
                        // Also check legacy nonce format
                        try {
                            const legacyResult = await this.callRPC('verifymessage', [actualWallet, signature, nonce]);
                            isValid = legacyResult === true;
                        } catch (e) { /* legacy check failed */ }
                    }

                    if (!isValid) {
                        console.log(`[P2P-REG] REJECTED: Invalid signature from ${actualNodeId} wallet=${actualWallet?.substring(0,15)}...`);
                        return res.status(401).json({
                            success: false,
                            error: 'Signature verification failed. You do not own this wallet.'
                        });
                    }
                    console.log(`[P2P-REG] Signature verified for ${actualWallet?.substring(0,15)}...`);
                }

                const nodeRole = role || 'miner';
                const nodeCapabilities = capabilities || [];

                const existingPeer = this.peers.get(actualNodeId);

                if (existingPeer) {
                    existingPeer.role = nodeRole;
                    existingPeer.capabilities = nodeCapabilities;
                    existingPeer.wallet_address = actualWallet || existingPeer.wallet_address;
                    if (req.ip && req.ip !== '127.0.0.1' && req.ip !== '::1' && req.ip !== '::ffff:127.0.0.1') {
                        existingPeer.public_ip = req.ip;
                    } else if (public_ip && public_ip !== '127.0.0.1' && public_ip !== '::1') {
                        existingPeer.public_ip = public_ip;
                    }
                    existingPeer.p2p_port = p2p_port || existingPeer.p2p_port || 9333;
                    existingPeer.ws_port = ws_port || existingPeer.ws_port || 9332;
                    existingPeer.model_name = model_name || existingPeer.model_name;
                    existingPeer.last_heartbeat = Date.now();
                    existingPeer.timestamp = Date.now();

                    console.log(`[P2P] Peer updated: ${actualNodeId} (role=${nodeRole})`);
                } else {
                    const peerData = {
                        node_id: actualNodeId,
                        role: nodeRole,
                        capabilities: nodeCapabilities,
                        wallet_address: actualWallet || '',
                        public_ip: (req.ip && req.ip !== '127.0.0.1' && req.ip !== '::1' && req.ip !== '::ffff:127.0.0.1') ? req.ip : (public_ip || req.ip),
                        p2p_port: p2p_port || 9333,
                        ws_port: ws_port || 9332,
                        model_name: model_name || '',
                        registered_at: Date.now(),
                        last_heartbeat: Date.now(),
                        timestamp: Date.now()
                    };

                    this.peers.set(actualNodeId, peerData);
                    console.log(`[P2P] Peer registered: ${actualNodeId} (role=${nodeRole}, caps=${nodeCapabilities.join(',')})`);
                }

                // Only 'miner' role creates/updates miner records.
                // 'node' role registers as peer only — prevents non-miner nodes from
                // appearing as miners with incorrect public_ip (req.ip fallback).
                if (nodeRole === 'miner') {
                    const existing = this.miners.get(actualWallet);

                    if (existing) {
                        existing.gpu_load = req.body.gpu_load || existing.gpu_load;
                        existing.model_name = model_name || existing.model_name;
                        existing.api_port = api_port || ws_port || existing.api_port;
                        if (req.ip && req.ip !== '127.0.0.1' && req.ip !== '::1' && req.ip !== '::ffff:127.0.0.1') {
                            existing.public_ip = req.ip;
                        } else if (public_ip && public_ip !== '127.0.0.1' && public_ip !== '::1') {
                            existing.public_ip = public_ip;
                        }
                        existing.gpu_info = req.body.gpu_info || existing.gpu_info;
                        existing.vram_mb = req.body.vram_mb || existing.vram_mb;
                        existing.hashrate = req.body.hashrate || existing.hashrate;
                        // Don't overwrite manually-set price with miner's reported value
                        if (!existing.price_set_manually) {
                            existing.token_ratio = req.body.token_ratio || existing.token_ratio;
                            existing.price_per_1m_tknc = req.body.price_per_1m_tknc || existing.price_per_1m_tknc;
                        }
                        existing.last_heartbeat = Date.now();
                        existing.status = 'online';

                        res.json({
                            success: true,
                            updated: true,
                            message: 'Miner info updated',
                            heartbeat_interval: this.HEARTBEAT_INTERVAL
                        });
                    } else {
                        const minerData = {
                            miner_id: actualWallet,
                            wallet: actualWallet,
                            model_name: model_name || 'Unknown',
                            api_port: api_port || ws_port || 9332,
                            public_ip: (req.ip && req.ip !== '127.0.0.1' && req.ip !== '::1' && req.ip !== '::ffff:127.0.0.1') ? req.ip : (public_ip || req.ip),
                            gpu_info: req.body.gpu_info || 'N/A',
                            vram_mb: req.body.vram_mb || 0,
                            hashrate: req.body.hashrate || 0,
                            gpu_load: req.body.gpu_load || 0,
                            token_ratio: req.body.token_ratio || 100000,
                            price_per_1m_tknc: req.body.price_per_1m_tknc || 10,
                            registered_at: Date.now(),
                            last_heartbeat: Date.now(),
                            status: 'online',
                            total_calls: 0,
                            revenue: 0
                        };

                        this.miners.set(actualWallet, minerData);
                        console.log(`[P2P] Miner registered: ${actualWallet}`);

                        res.json({
                            success: true,
                            created: true,
                            message: 'Miner registered successfully',
                            heartbeat_interval: this.HEARTBEAT_INTERVAL
                        });
                    }

                    this.broadcastMinerUpdate();
                } else {
                    res.json({
                        success: true,
                        created: !existingPeer,
                        message: existingPeer ? 'Peer info updated' : 'Client peer registered successfully',
                        heartbeat_interval: this.HEARTBEAT_INTERVAL
                    });
                }
            } catch (error) {
                res.status(500).json({ error: error.message });
            }
        });
        // ===== NODE MINER-NOTIFY ENDPOINT =====
        this.app.post('/api/node/miner-notify', (req, res) => {
            console.log('[MINER-NOTIFY] POST /api/node/miner-notify received');
            try {
                // AUTH CHECK: Require valid API key or node shared secret
                // If TKNC_NOTIFY_SECRET is not configured, allow all requests (open mode)
                const authHeader = req.headers['authorization'] || '';
                const notifySecret = process.env.TKNC_NOTIFY_SECRET || '';
                let isAuthorized = false;

                if (!notifySecret) {
                    // No secret configured → open mode, allow all miner notifications
                    isAuthorized = true;
                } else if (authHeader === `Bearer ${notifySecret}`) {
                    isAuthorized = true;
                }
                // Also allow if request comes from localhost (node self-notify)
                if (!isAuthorized && (req.ip === '127.0.0.1' || req.ip === '::1' || req.ip === '::ffff:127.0.0.1')) {
                    isAuthorized = true;
                }

                if (!isAuthorized) {
                    console.warn(`[MINER-NOTIFY] REJECTED: Unauthorized from ${req.ip}`);
                    return res.status(401).json({ error: 'Unauthorized: Valid notification secret required' });
                }
                const src = req.body || req.rawBody;
                if (!src) return res.status(400).json({ error: 'Empty body' });
                const node_ip = src.node_ip || '';
                // Debug: log the actual IP submitted by the miner/node
                console.log(`[MINER-NOTIFY] src.node_ip="${node_ip}" req.ip="${req.ip}" miners_count=${(src.miners || []).length}`);
                const miners = src.miners || (src.wallet_address ? [src] : []);
                if (!miners || miners.length === 0) return res.status(400).json({ error: 'No miners' });
                let registeredCount = 0;
                let skippedNoWallet = 0;
                for (const miner of miners) {
                    const wallet = miner.wallet_address || miner.miner_id || miner.node_id || src.node_id || '';
                    if (!wallet) {
                        skippedNoWallet++;
                        console.warn(`[MINER-NOTIFY] Skipped miner with no wallet_address/miner_id: ip=${req.ip}`);
                        continue;
                    }
                    if (!miner.wallet_address) {
                        console.log(`[MINER-NOTIFY] Miner has no wallet_address, using ${wallet} as ID (ip=${req.ip})`);
                    }
                    // Determine miner's real public IP. Priority:
                    //   1. miner.public_ip (per-miner field, may carry IPv6)
                    //   2. node_ip (top-level field from C++ node, may be IPv6)
                    //   3. req.ip (HTTP source IP through Nginx — always IPv4, last resort only)
                    // NEVER overwrite a submitted IPv6 with req.ip (Nginx proxy IPv4).
                    const isInvalidIp = (ip) => !ip || ip === '127.0.0.1' || ip === '::1' || ip === '::ffff:127.0.0.1' || ip === 'localhost';
                    const submittedIp = (miner.public_ip && !isInvalidIp(miner.public_ip))
                        ? miner.public_ip
                        : (node_ip && !isInvalidIp(node_ip) ? node_ip : '');
                    const finalIp = submittedIp || (!isInvalidIp(req.ip) ? req.ip : '127.0.0.1');
                    const existing = this.miners.get(wallet);
                    if (existing) {
                        existing.gpu_info = miner.gpu_name || existing.gpu_info;
                        existing.model_name = miner.model_name || existing.model_name;
                        existing.api_port = miner.api_port || existing.api_port;
                        // Use the miner's submitted IP (IPv6 if available), not req.ip (Nginx IPv4)
                        if (submittedIp) {
                            existing.public_ip = submittedIp;
                        }
                        existing.hashrate = miner.hashrate || existing.hashrate;
                        // Only refresh last_heartbeat when node reports status=online.
                        // Dead miners with old nodes that ignore process health would otherwise
                        // keep last_heartbeat fresh forever → never timeout → always show online.
                        const notifyStatus = (miner.status || 'unknown').toLowerCase();
                        if (notifyStatus === 'online') {
                            existing.last_heartbeat = Date.now();
                        }
                        // Sync GPU detail fields from C++ sender
                        if (miner.gpu_vram_total_mb) existing.vram_mb = miner.gpu_vram_total_mb;
                        if (miner.gpu_vram_used_mb != null) existing.vram_used_mb = miner.gpu_vram_used_mb;
                        if (miner.gpu_utilization != null) existing.gpu_load = miner.gpu_utilization;
                        if (miner.status) existing.status = miner.status;
                        if (miner.uptime_seconds) existing.uptime_seconds = miner.uptime_seconds;
                        // Update exchange rate if provided by miner (-token parameter)
                        if (miner.token_ratio && miner.token_ratio > 0) {
                            existing.token_ratio = miner.token_ratio;
                            existing.price_per_1m_tknc = miner.price_per_1m_tknc || Math.max(1, Math.round(1000000 / miner.token_ratio));
                            existing.price_set_manually = true;
                        }
                    } else {
                        this.miners.set(wallet, {
                            miner_id: wallet, wallet: wallet,
                            model_name: miner.model_name || 'Unknown',
                            api_port: miner.api_port || 9332,
                            public_ip: finalIp,
                            gpu_info: miner.gpu_name || 'N/A',
                            // Use actual GPU values from C++ sender instead of hardcoded 0
                            vram_mb: miner.gpu_vram_total_mb || 0,
                            vram_used_mb: miner.gpu_vram_used_mb || 0,
                            hashrate: miner.hashrate || 0,
                            gpu_load: miner.gpu_utilization || 0,
                            status: miner.status || 'online',
                            uptime_seconds: miner.uptime_seconds || 0,
                            token_ratio: (miner.token_ratio && miner.token_ratio > 0) ? miner.token_ratio : 100000,
                            price_per_1m_tknc: (miner.price_per_1m_tknc && miner.price_per_1m_tknc > 0) ? miner.price_per_1m_tknc : 10,
                            price_set_manually: (miner.token_ratio && miner.token_ratio > 0),
                            registered_at: Date.now(), last_heartbeat: Date.now(),
                            total_calls: 0, revenue: 0
                        });
                    }
                    registeredCount++;
                }
                if (registeredCount > 0) {
                    this.broadcastMinerUpdate();
                    try { this.persistence.save('miners', this.miners); this.persistence.save('peers', this.peers); } catch(e) {}
                }
                res.json({ success: true, registered: registeredCount, message: registeredCount + ' miner(s) registered/updated', heartbeat_interval: 30000 });
            } catch (error) { res.status(500).json({ error: error.message }); }
        });

        // ===== MINER-HEARTBEAT ENDPOINT =====
        this.app.post('/api/node/miner-heartbeat', (req, res) => {
            console.log('[MINER-HB] POST /api/node/miner-heartbeat received');
            try {
                // AUTH CHECK: Same as miner-notify (open mode if no secret configured)
                const authHeader = req.headers['authorization'] || '';
                const notifySecret = process.env.TKNC_NOTIFY_SECRET || '';
                let isAuthorized = false;

                if (!notifySecret) {
                    isAuthorized = true;
                } else if (authHeader === `Bearer ${notifySecret}`) {
                    isAuthorized = true;
                }
                if (!isAuthorized && (req.ip === '127.0.0.1' || req.ip === '::1' || req.ip === '::ffff:127.0.0.1')) {
                    isAuthorized = true;
                }
                if (!isAuthorized) {
                    console.warn(`[MINER-HB] REJECTED: Unauthorized from ${req.ip}`);
                    return res.status(401).json({ error: 'Unauthorized: Valid notification secret required' });
                }
                const src = req.body || req.rawBody;
                if (!src) return res.status(400).json({ error: 'Empty body' });
                const miners = src.miners || (src.wallet_address ? [src] : []);
                if (!miners || miners.length === 0) return res.status(400).json({ error: 'No miners' });
                for (const miner of miners) {
                    const wallet = miner.wallet_address || miner.miner_id || '';
                    if (!wallet) continue;
                    const existing = this.miners.get(wallet);
                    if (existing) {
                        // Do NOT update last_heartbeat here — this endpoint has no
                        // proof of actual miner process health. Only miner-notify with
                        // explicit status=online should refresh the timeout counter.
                        // Otherwise old/dead nodes keep miners "online" forever.
                        if (miner.hashrate) existing.hashrate = miner.hashrate;
                        if (miner.gpu_load) existing.gpu_load = miner.gpu_load;
                    }
                }
                this.broadcastMinerUpdate();
                res.json({ success: true, message: 'Heartbeat received' });
            } catch (error) { res.status(500).json({ error: error.message }); }
        });


        // Query available peers by role and capability
        this.app.get('/api/p2p/peers', (req, res) => {
            try {
                const { role, has_capability } = req.query;
                const now = Date.now();
                const PEER_TIMEOUT = 5 * 60 * 1000;

                let filteredPeers = Array.from(this.peers.values());

                if (role) {
                    filteredPeers = filteredPeers.filter(p => p.role === role);
                }

                if (has_capability) {
                    filteredPeers = filteredPeers.filter(p =>
                        p.capabilities && p.capabilities.includes(has_capability)
                    );
                }

                const onlinePeers = filteredPeers
                    .filter(p => (now - p.last_heartbeat) < PEER_TIMEOUT)
                    .map(p => ({
                        node_id: p.node_id,
                        role: p.role,
                        capabilities: p.capabilities,
                        public_ip: p.public_ip,
                        p2p_port: p.p2p_port,
                        // ws_port REMOVED (2026-06-07): Miner API port (9332) is localhost-only.
                        // Direct IP:port access to miner API is forbidden by A2.7/A2.8 architecture.
                        wallet_address: p.wallet_address,
                        model_name: p.model_name,
                        last_heartbeat: p.last_heartbeat,
                        is_online: true
                    }));

                res.json({
                    success: true,
                    count: onlinePeers.length,
                    peers: onlinePeers,
                    queried_at: now,
                    filters: { role: role || 'all', has_capability: has_capability || 'none' }
                });
            } catch (error) {
                res.status(500).json({ error: error.message });
            }
        });

        // Unregister a peer (node going offline)
        this.app.delete('/api/p2p/register', (req, res) => {
            try {
                const { node_id } = req.query;
                if (!node_id) {
                    return res.status(400).json({ error: 'node_id query parameter required' });
                }

                const deletedPeer = this.peers.delete(node_id);

                if (deletedPeer) {
                    const peerMiner = Array.from(this.miners.entries())
                        .find(([k, v]) => v.miner_id === node_id || v.wallet === node_id);
                    if (peerMiner) {
                        this.miners.delete(peerMiner[0]);
                    }

                    console.log(`[P2P] Peer unregistered: ${node_id}`);
                    res.json({ success: true, message: 'Peer unregistered successfully' });
                } else {
                    res.status(404).json({ error: 'Peer not found', node_id });
                }
            } catch (error) {
                res.status(500).json({ error: error.message });
            }
        });

        this.app.get('/api/p2p/lookup', (req, res) => {
            try {
                const { miner_id } = req.query;
                if (!miner_id) {
                    return res.status(400).json({ error: 'miner_id query parameter required' });
                }

                const miner = this.miners.get(miner_id);
                if (!miner) {
                    return res.status(404).json({ error: 'Miner not found', miner_id });
                }

                if (!miner.public_ip || miner.public_ip === '::1' || miner.public_ip === '127.0.0.1') {
                    return res.status(404).json({
                        error: 'Miner not directly reachable (no public IP)',
                        miner_id: miner.miner_id,
                        model_name: miner.model_name,
                        status: miner.status
                    });
                }

                // Architecture: Miner API is localhost-only (127.0.0.1:9332).
                // Public IP and API port are NOT exposed — direct access is forbidden.
                // All inference requests must route through P2P node network:
                //   Client → Web → Seed Node RPC(p2pinference) → P2P → Target Node → Miner(localhost)
                res.json({
                    miner_id: miner.miner_id,
                    model_name: miner.model_name,
                    gpu_info: miner.gpu_info,
                    hashrate: miner.hashrate,
                    gpu_load: miner.gpu_load,
                    status: miner.status,
                    routing_method: 'p2p_via_node',
                    note: 'Miner API is localhost-only. All requests routed through P2P node network.'
                });
            } catch (error) {
                res.status(500).json({ error: error.message });
            }
        });

        // Miner heartbeat
        // [Architecture Fix 2026-06-07] This endpoint is currently POSTed directly by miners to the web server
        // Violates Rule A2.7 (miners must not have any outbound communication)
        // Correct architecture: miner -> node(localhost) -> node reports heartbeat to seed/web via P2P/RPC
        // TODO: Change to node-proxy heartbeat reporting
        this.app.post('/api/p2p/heartbeat', (req, res) => {
            try {
                const { miner_id, gpu_load, active_requests } = req.body;

                // Basic validation: miner_id must be a valid wallet-like format or registered ID
                if (!miner_id) {
                    return res.status(400).json({ error: 'miner_id required' });
                }

                const miner = this.miners.get(miner_id) || Array.from(this.miners.values()).find(m => m.wallet === miner_id || m.miner_id === miner_id);

                if (!miner) {
                    return res.status(404).json({ error: 'Miner not registered' });
                }

                // Do NOT update last_heartbeat or set status=online here.
                // This P2P heartbeat endpoint carries no proof of actual miner process health.
                // Only /api/node/miner-notify with explicit status=online should refresh
                // the timeout counter. Old nodes calling this endpoint would otherwise
                // keep dead miners showing "online" forever.
                miner.gpu_load = gpu_load || miner.gpu_load || 0;
                miner.active_requests = active_requests || 0;
                miner.hashrate = req.body.hashrate || miner.hashrate || 0;
                if (req.body.token_ratio) miner.token_ratio = req.body.token_ratio;
                if (req.body.ip_address) miner.ip_address = req.body.ip_address;
                if (!miner.public_ip && req.body.ip_address) miner.public_ip = req.body.ip_address;
                // NOTE: status is computed dynamically by /api/miners based on last_heartbeat age.
                // Do NOT force status='online' here — that defeats the timeout mechanism.

                res.json({ success: true, next_heartbeat: this.HEARTBEAT_INTERVAL });
            } catch (error) {
                res.status(500).json({ error: error.message });
            }
        });

        // Get all registered miners (global view)
        this.app.get('/api/p2p/miners', (req, res) => {
            const allMiners = Array.from(this.miners.values()).map(m => {
                // IPv6 addresses are globally routable public addresses.
                // They MUST be preserved for true remote P2P inference routing.
                // Only strip the ::ffff: IPv4-mapped IPv6 prefix; keep pure IPv6 as-is.
                let publicIp = m.public_ip || '';
                if (publicIp.startsWith('::ffff:')) {
                    publicIp = publicIp.substring(7);
                }
const now2 = Date.now();
const hbAge2 = now2 - (m.last_heartbeat || 0);
const hbFresh2 = hbAge2 < this.MINER_TIMEOUT;
const statusOffline2 = (m.status || '').toLowerCase() === 'offline';
const isOnline2 = m.has_tunnel ? true : (!statusOffline2 && hbFresh2);
return {
...m,
public_ip: publicIp,
status: isOnline2 ? 'online' : 'offline',
is_online: isOnline2,
has_tunnel: !!this.tunnels.has(m.miner_id),
tunnel_status: m.tunnel_status || 'none',
connection_type: m.connection_type || (m.has_tunnel ? 'NAT-Tunnel' : 'direct'),
nat_type: m.nat_type || 'unknown'
};
            });

            const tunnelCount = this.tunnels.size;
            res.json({
                count: allMiners.length,
                online: this.getOnlineMinersCount(),
                tunnel_count: tunnelCount,
                miners: allMiners
            });
        });

        // Tunnel status endpoint
        this.app.get('/api/p2p/tunnel/status', (req, res) => {
            const tunnels = [];
            this.tunnels.forEach((ws, minerId) => {
                const miner = this.miners.get(minerId);
                tunnels.push({
                    miner_id: minerId,
                    status: ws.readyState === WebSocket.OPEN ? 'active' : 'error',
                    connected_since: miner?.last_heartbeat,
                    model_name: miner?.model_name,
                    wallet: miner?.wallet
                });
            });

            res.json({
                active_tunnels: tunnels.length,
                total_miners: this.miners.size,
                tunnels: tunnels
            });
        });

        // ===== REVERSE CONNECT REQUEST SYSTEM =====
        this.app.post('/api/p2p/request', (req, res) => {
            try {
                const { client_wallet, client_callback_url } = req.body;

                if (!client_wallet || !client_wallet.startsWith('token1')) {
                    return res.status(400).json({ error: 'Valid client_wallet (token1...) required' });
                }
                if (!client_callback_url) {
                    return res.status(400).json({ error: 'client_callback_url is required' });
                }

                // SSRF PROTECTION: Validate callback URL
                let parsedUrl;
                try {
                    parsedUrl = new URL(client_callback_url);
                } catch (e) {
                    return res.status(400).json({ error: 'Invalid URL format for client_callback_url' });
                }

                // Only allow http/https protocols (block file://, dict://, gopher://, etc.)
                if (!['http:', 'https:'].includes(parsedUrl.protocol)) {
                    console.warn(`[ReverseConnect] BLOCKED dangerous protocol: ${parsedUrl.protocol} from ${req.ip}`);
                    return res.status(400).json({ error: 'Only http/https URLs allowed for callback' });
                }

                // Block internal/private IP addresses (SSRF prevention)
                const hostname = parsedUrl.hostname.toLowerCase();
                const blockedPatterns = [
                    'localhost', '127.0.0.1', '::1', '0.0.0.0',
                    '10.', '172.16.', '172.17.', '172.18.', '172.19.', '172.20.',
                    '172.21.', '172.22.', '172.23.', '172.24.', '172.25.',
                    '172.26.', '172.27.', '172.28.', '172.29.', '172.30.', '172.31.',
                    '192.168.', '169.254.', 'fd00:', 'fe80:', '::ffff:', '[::1]'
                ];
                const isInternal = blockedPatterns.some(p => hostname.startsWith(p) || hostname === p);
                if (isInternal) {
                    console.warn(`[ReverseConnect] BLOCKED internal address: ${hostname} from ${req.ip}`);
                    return res.status(400).json({ error: 'Internal/private IP addresses not allowed for callback URL' });
                }

                const requestId = 'rc_req_' + Date.now() + '_' + (++this.reverseRequestIdCounter);

                this.reverseConnectRequests.set(requestId, {
                    client_wallet: client_wallet,
                    client_callback_url: client_callback_url,
                    created_at: Date.now(),
                    status: 'pending'
                });

                console.log(`[ReverseConnect] Request created: ${requestId} from ${client_wallet.substring(0, 15)}... -> ${client_callback_url}`);

                res.json({
                    status: 'pending',
                    request_id: requestId,
                    message: 'Reverse connect request registered. Node will poll for pending requests.'
                });
            } catch (error) {
                console.error('[ReverseConnect] Error creating request:', error.message);
                res.status(500).json({ error: error.message });
            }
        });

        // Poll endpoint for nodes to check pending reverse connect requests
        this.app.get('/api/p2p/request/poll', (req, res) => {
            try {
                const { miner_wallet } = req.query;
                const now = Date.now();
                const REQUEST_TIMEOUT = 300000; // 5 minutes

                const pendingRequests = [];
                this.reverseConnectRequests.forEach((request, requestId) => {
                    if (request.status === 'pending' && (now - request.created_at) < REQUEST_TIMEOUT) {
                        if (!miner_wallet || request.client_wallet !== miner_wallet) {
                            pendingRequests.push({
                                request_id: requestId,
                                client_wallet: request.client_wallet,
                                client_callback_url: request.client_callback_url,
                                created_at: request.created_at
                            });
                        }
                    }
                });

                res.json({
                    pending_count: pendingRequests.length,
                    requests: pendingRequests,
                    polled_at: now
                });
            } catch (error) {
                console.error('[ReverseConnect] Error polling requests:', error.message);
                res.status(500).json({ error: error.message });
            }
        });

        // Node claims/accepts a reverse connect request
        this.app.post('/api/p2p/request/accept', (req, res) => {
            try {
                const { request_id, miner_wallet } = req.body;

                if (!request_id || !miner_wallet) {
                    return res.status(400).json({ error: 'request_id and miner_wallet required' });
                }

                const request = this.reverseConnectRequests.get(request_id);
                if (!request) {
                    return res.status(404).json({ error: 'Request not found or expired' });
                }
                if (request.status !== 'pending') {
                    return res.status(409).json({ error: 'Request already processed', status: request.status });
                }

                request.status = 'accepted';
                request.accepted_by = miner_wallet;
                request.accepted_at = Date.now();

                console.log(`[ReverseConnect] Request ${request_id} accepted by ${miner_wallet.substring(0, 15)}...`);

                res.json({
                    status: 'accepted',
                    request_id: request_id,
                    client_callback_url: request.client_callback_url,
                    client_wallet: request.client_wallet,
                    message: 'Request accepted. Node should now call StartReverseConnect with the callback URL.'
                });
            } catch (error) {
                console.error('[ReverseConnect] Error accepting request:', error.message);
                res.status(500).json({ error: error.message });
            }
        });

        // Clean up expired reverse connect requests every 60 seconds
        setInterval(() => {
            const now = Date.now();
            const REQUEST_TIMEOUT = 300000; // 5 minutes
            let cleaned = 0;
            this.reverseConnectRequests.forEach((request, requestId) => {
                if ((now - request.created_at) > REQUEST_TIMEOUT && request.status === 'pending') {
                    request.status = 'expired';
                    this.reverseConnectRequests.delete(requestId);
                    cleaned++;
                }
            });
            if (cleaned > 0) {
                console.log(`[ReverseConnect] Cleaned ${cleaned} expired requests`);
            }
        }, 60000);

        // ===== P2P INFERENCE ENDPOINT (per LLM_REMOTE_INFERENCE_GUIDE.md §6) =====
        // Routes inference requests through P2P network to target miner.
        // Server does NOT run LLM itself — it only calls RPC p2pinference on seed node.
        // Flow: Client → Web(80) → Seed RPC(p2pinference) → P2P(9333) → Target Node → Miner(127.0.0.1:9332)
        this.app.post('/api/p2p/inference', async (req, res) => {
            try {
                // 1. Verify session token
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                if (!session.is_active) {
                    return res.status(401).json({ success: false, error: 'Session inactive. Please login again.' });
                }

                // 2. Verify CSRF token
                const csrfToken = req.headers['x-csrf-token'];
                if (!csrfToken || session.csrf_token !== csrfToken) {
                    return res.status(403).json({ success: false, error: 'CSRF token validation failed' });
                }

                // 3. Update session activity
                session.last_activity = Date.now();

                // 4. Extract parameters
                const { model, prompt, target_wallet, max_tokens } = req.body;
                if (!model || !prompt) {
                    return res.status(400).json({ success: false, error: 'model and prompt required' });
                }
                if (!target_wallet) {
                    return res.status(400).json({ success: false, error: 'Target miner (target_wallet) required for deterministic routing' });
                }

                console.log(`[P2P-Inference] Request from ${session.wallet.substring(0,15)}... → miner ${target_wallet.substring(0,15)}... model=${model}`);

                // 5. Determine target peer_id via 3-tier priority routing
                let targetPeerId = null;
                let routingMethod = null;

                // [Priority 1] p2p-miner-adv: Use getminerpeers RPC to find by wallet address
                try {
                    const minerPeers = await this.callRPC('getminerpeers', []);
                    if (Array.isArray(minerPeers) && minerPeers.length > 0) {
                        // wallet_address may be "wallet+model" concatenated — match by prefix/contains
                        const match = minerPeers.find(p =>
                            p.wallet_address === target_wallet ||
                            p.wallet_address.startsWith(target_wallet) ||
                            p.wallet_address.includes(target_wallet)
                        );
                        if (match) {
                            targetPeerId = match.peer_id;
                            routingMethod = 'p2p-miner-adv';
                            console.log(`[P2P-Inference] Route via p2p-miner-adv: peer_id=${targetPeerId} wallet=${match.wallet_address.substring(0, 30)}...`);
                        }
                    }
                } catch (e) {
                    console.warn('[P2P-Inference] getminerpeers failed:', e.message);
                }

                // [Priority 2] heartbeat-ip-match: Find peer by miner's registered public_ip
                if (targetPeerId === null) {
                    try {
                        const miner = this.miners.get(target_wallet);
                        if (miner && miner.public_ip) {
                            const peerInfo = await this.callRPC('getpeerinfo', []);
                            if (Array.isArray(peerInfo)) {
                                // Normalize IPv4 from ::ffff: format for matching
                                const minerIp = miner.public_ip.startsWith('::ffff:')
                                    ? miner.public_ip.substring(7)
                                    : miner.public_ip;
                                const match = peerInfo.find(p => {
                                    if (!p.addr) return false;
                                    return p.addr.includes(minerIp) || p.addr.includes(miner.public_ip);
                                });
                                if (match) {
                                    targetPeerId = match.id;
                                    routingMethod = 'heartbeat-ip-match';
                                    console.log(`[P2P-Inference] Route via heartbeat-ip-match: peer_id=${targetPeerId} ip=${minerIp}`);
                                }
                            }
                        }
                    } catch (e) {
                        console.warn('[P2P-Inference] heartbeat-ip-match failed:', e.message);
                    }
                }

                // [Priority 3] REMOVED (2026-06-28): non-seed-fallback randomly picked any non-seed
                // peer, which violates the user requirement that "all inference must be routed via
                // the miner info card". Inference must be matched explicitly via Priority 1
                // (wallet match) or Priority 2 (registered IP match). Otherwise return error.

                // 6. If routing failed, return error (no random peer selection)
                if (targetPeerId === null) {
                    console.error(`[P2P-Inference] Miner not found in P2P network: ${target_wallet.substring(0,15)}...`);
                    return res.status(404).json({
                        success: false,
                        error: 'Miner not found in P2P network',
                        target_wallet: target_wallet,
                        hint: 'Ensure the target miner is online and connected to the P2P network'
                    });
                }

                // 7. Look up user's api_key for billing (Path A billing fix)
                let userApiKey = null;
                for (const [key, record] of this.inferenceHistory) {
                    if (record.user_wallet === session.wallet && record.miner_id === target_wallet) {
                        userApiKey = key;
                        break;
                    }
                }

                // 8. Call RPC p2pinference with explicit peer selection
                //    Pass api_key as 4th param to enable escrow billing (CheckAndDeductEscrow).
                //    Without api_key, the node skips billing — tokens are free, which is a bug.
                console.log(`[P2P-Inference] Calling RPC p2pinference with peer_id=${targetPeerId}${userApiKey ? ' +api_key for billing' : ' (no api_key — no billing)'} (timeout=24h)`);
                const rpcParams = [model, prompt, targetPeerId];
                if (userApiKey) {
                    rpcParams.push(userApiKey);
                } else {
                    rpcParams.push('');  // empty api_key placeholder
                }
                rpcParams.push(max_tokens || -1);  // max_tokens: -1=unlimited
                const rpcResult = await this.callRPC('p2pinference', rpcParams, 86400000);

                if (rpcResult && rpcResult.content) {
                    console.log(`[P2P-Inference] SUCCESS [${routingMethod}]: tokens=${rpcResult.tokens_used || 0}, cost=${rpcResult.cost || 0}, peer=${rpcResult.peer_id || targetPeerId}`);
                    res.json({
                        success: true,
                        content: rpcResult.content,
                        tokens_used: rpcResult.tokens_used || 0,
                        cost: rpcResult.cost || 0,
                        peer_id: rpcResult.peer_id || targetPeerId,
                        connection_type: 'p2p',
                        routed_to_miner: true,
                        routing_method: routingMethod,
                        target_wallet: target_wallet,
                        model: model
                    });
                } else {
                    console.error('[P2P-Inference] RPC returned no content:', rpcResult);
                    res.status(502).json({
                        success: false,
                        error: 'Inference returned empty result',
                        routing_method: routingMethod
                    });
                }
            } catch (error) {
                console.error('[P2P-Inference] Error:', error.message);
                // Distinguish timeout errors from other failures
                if (error.message.includes('timed out') || error.message.includes('timeout')) {
                    return res.status(504).json({ success: false, error: 'P2P inference timed out — target miner may be offline or unreachable', detail: error.message });
                }
                res.status(500).json({ success: false, error: error.message });
            }
        });

        // REMOVED: Direct inference gateway — replaced by /api/p2p/inference above
        // Clients either use /api/p2p/inference (P2P route) or /api/v1/chat (directory service)

        // API Key with balance query
        this.app.get('/api/p2p/keys/:apiKey/balance', async (req, res) => {
            try {
                const balance = await this.queryAPIKeyBalance(req.params.apiKey);
                res.json(balance);
            } catch (error) {
                console.error('[API Key Balance] Query failed:', error.message);
                const isConnectError = error.code === 'ECONNREFUSED' || error.code === 'ECONNRESET' || error.message.includes('connect') || error.message.includes('EHOSTUNREACH');
                const isMinerNotFound = error.code === 'MINER_404';
                const isMinerBadRequest = error.code === 'MINER_400';

                if (isConnectError) {
                    res.status(503).json({ error: 'Miner API service unavailable', code: 'SERVICE_UNAVAILABLE' });
                } else if (isMinerNotFound) {
                    res.status(404).json({ error: 'API Key not found or expired', code: 'KEY_NOT_FOUND' });
                } else if (isMinerBadRequest) {
                    res.status(400).json({ error: 'Invalid request parameters', code: 'BAD_REQUEST' });
                } else {
                    res.status(500).json({ error: 'Failed to query API key balance', code: 'INTERNAL_ERROR' });
                }
            }
        });
        
        // Get all miners - REMOVED (duplicate of endpoint at line ~214)
        // Get single miner detail
        this.app.get('/api/miners/:minerId', async (req, res) => {
            try {
                const miner = await this.fetchMinerDetail(req.params.minerId);
                if (!miner) {
                    return res.status(404).json({ error: 'Miner not found' });
                }
                res.json(miner);
            } catch (error) {
                console.error('[Miner Detail] Fetch failed:', error.message);
                const isConnectError = error.code === 'ECONNREFUSED' || error.code === 'ECONNRESET' || error.message.includes('connect') || error.message.includes('EHOSTUNREACH');
                res.status(isConnectError ? 503 : 500).json({
                    error: isConnectError ? 'Miner API service unavailable - miner may be offline' : 'Failed to fetch miner detail',
                    code: isConnectError ? 'SERVICE_UNAVAILABLE' : 'INTERNAL_ERROR'
                });
            }
        });
        
        // Create API Key — directory service only.
        // Per architecture: Web server is yellow-pages only. Inference billing happens entirely
        // on the client/miner nodes (setminerprice + per-1-TKNC on-chain transfer).
        // No payment, no locking, no refund — Web just issues an API Key credential.
        this.app.post('/api/create_key', async (req, res) => {
            try {
                // ===== SESSION AUTH =====
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ error: 'Authentication required. Please login first.' });
                }
                const walletName = this.userWallets.get(sessionToken);

                if (!walletName) {
                    return res.status(400).json({ error: 'No wallet found for your session. Please log out and log in again.' });
                }

                // Get wallet ADDRESS from session (needed for createescrow billing)
                const session = this.sessions.get(sessionToken);
                const walletAddress = session.wallet;  // token1q... address

                // ===== REQUEST PARAMETERS =====
                // Only target_miner_id, model, and optional declared_limit (consumer-defined spending cap).
                // No payment_nonce / payment_signature / payment_amount — inference billing is off-Web.
                const { target_miner_id, model, declared_limit } = req.body;

                if (!target_miner_id) {
                    return res.status(400).json({ success: false, error: 'target_miner_id required', code: 'MINER_ID_REQUIRED' });
                }

                // ===== CHECK MINER PUBLIC IP =====
                if (target_miner_id) {
                    const targetMiner = this.miners.get(target_miner_id);
                    if (targetMiner) {
                        const pubIp = targetMiner.public_ip || targetMiner.ip_address;
                        // Allow any valid IP (IPv4, IPv6, public or private) - P2P network can use any address
                        if (!pubIp || pubIp === '' || pubIp === 'undefined') {
                                console.warn(`[Security] API key creation rejected: miner ${target_miner_id} has no valid IP (current: ${pubIp || 'null'})`);
                                return res.status(400).json({
                                        success: false,
                                        error: 'Miner has no public IP configured',
                                        message: 'API keys can only be created for miners with a valid network address.',
                                        code: 'MINER_NO_PUBLIC_IP'
                                });
                        }
                        console.log(`[Security] API key allowed for miner ${target_miner_id} with IP: ${pubIp}`);
                    }
                }

                console.log(`[Route] /api/create_key called by wallet: ${walletName}`, { target_miner_id, model, declared_limit });
                console.log(`[DEBUG] target_miner_id=${target_miner_id}, miners.size=${this.miners.size}, miners.has=${this.miners.has(target_miner_id)}`);
                if (this.miners.has(target_miner_id)) {
                    console.log(`[DEBUG] miner.wallet=${this.miners.get(target_miner_id).wallet}`);
                } else {
                    console.log(`[DEBUG] All miner keys:`, Array.from(this.miners.keys()));
                }
                const result = await this.createAPIKey({ target_miner_id, model, declared_limit, user_wallet: walletName, user_wallet_address: walletAddress }, walletName);
                res.json(result);
            } catch (error) {
                console.error('Error creating API key:', error);
                res.status(500).json({ error: 'Failed to create API key' });
            }
        });
        
        // API Endpoint Discovery Service (pure directory, NOT a proxy/relay!)
        // Per architecture: Web server is yellow-pages only. Returns miner's public endpoint for direct connection.
        this.app.post('/api/v1/chat', async (req, res) => {
            try {
                const parsed = req.body;

                if (!parsed.api_key) {
                    return res.status(400).json({
                        error: 'API key required',
                        message: 'This endpoint returns miner connection info. Provide api_key to discover your assigned miner endpoint.',
                        usage: 'POST /api/v1/chat { "api_key": "tknc_xxx" } → returns miner endpoint for direct connection',
                        code: 'API_KEY_REQUIRED'
                    });
                }

                // 1. Format validity (support both new 'tknc_' and legacy 'tknc-dev-' prefixes)
                if (typeof parsed.api_key !== 'string' || (!parsed.api_key.startsWith('tknc_') && !parsed.api_key.startsWith('tknc-dev-')) || parsed.api_key.length < 30) {
                    return res.status(401).json({ error: 'Invalid API key format', code: 'INVALID_KEY_FORMAT' });
                }

                // 2. Look up key record
                const record = this.inferenceHistory.get(parsed.api_key);
                if (!record) {
                    return res.status(401).json({ error: 'API key not found', code: 'KEY_NOT_FOUND' });
                }

                // 3. Find the assigned miner's public endpoint
                const miner = this.miners.get(record.miner_id);
                if (!miner) {
                    return res.status(503).json({ error: 'Assigned miner not found or offline', miner_id: record.miner_id, code: 'MINER_OFFLINE' });
                }

                const publicIp = miner.public_ip;
                // Use API Gateway port (9313)
                const apiGatewayPort = 9313;

                if (!publicIp || publicIp === '127.0.0.1' || publicIp === '::1') {
                    return res.status(503).json({
                        error: 'Miner has no public IP - cannot be reached externally',
                        miner_id: record.miner_id,
                        code: 'MINER_NO_PUBLIC_IP'
                    });
                }

                // Build direct-connect endpoint URL (OpenAI-compatible format per §10.2)
                const endpoint = publicIp.includes(':')
                    ? `http://[${publicIp}]:${apiGatewayPort}/v1/chat/completions`
                    : `http://${publicIp}:${apiGatewayPort}/v1/chat/completions`;

                console.log(`[Discovery] Key ${parsed.api_key.substring(0,12)}... → miner ${record.miner_id} at ${endpoint}`);

                // Return discovery result: client connects DIRECTLY to this endpoint
                // balance_info reports declared_limit / consumed / remaining only — no locking, no refund.
                const limit = record.declared_limit || 0;
                const consumed = record.consumed_balance || 0;
                // 0 = unlimited
                const remainingStr = (limit === 0) ? 'unlimited' : Math.max(0, limit - consumed);
                const tokensPerTknc = miner.token_ratio || 100000;
                const pricePer1mTknc = miner.price_per_1m_tknc || Math.max(1, Math.round(1000000 / tokensPerTknc));

                // Filter out "Unknown"/"unknown"/empty model names from BOTH miner record and API key record.
                const isValidModel = (m) => typeof m === 'string' && m !== '' && m !== 'Unknown' && m !== 'unknown' && m !== 'mining-only';
                const modelName = isValidModel(miner.model_name) ? miner.model_name
                                : isValidModel(record.model) ? record.model
                                : 'Unknown';

                // Query API key owner's wallet balance via RPC (best-effort — returns 0 if unavailable).
                // record.user_wallet is the wallet NAME (not address) of the user who created this API key.
                let walletBalance = 0;
                try {
                    if (record.user_wallet) {
                        const balResult = await this.callRPCForWallet(record.user_wallet, 'getbalance', []);
                        if (typeof balResult === 'number') {
                            walletBalance = balResult;
                        }
                    }
                } catch (e) {
                    console.log(`[Discovery] getbalance failed for wallet '${record.user_wallet}': ${e.message}`);
                }

                return res.json({
                    success: true,
                    mode: 'direct_connect',
                    endpoint: endpoint,
                    miner_id: record.miner_id,
                    miner_wallet: miner.wallet || '',
                    model: modelName,
                    api_key: parsed.api_key,
                    balance_info: { declared_limit: limit, consumed: consumed, remaining: remainingStr, wallet_balance: walletBalance, unlimited: (limit === 0) },
                    price_per_1m_tknc: pricePer1mTknc,
                    tokens_per_tknc: tokensPerTknc,
                    exchange_rate: '1 TKNC = ' + tokensPerTknc + ' tokens',
                    instructions: [
                        'Connect DIRECTLY to the endpoint above (NOT this server)',
                        'POST your inference request to the endpoint with the same api_key',
                        'The miner validates key, deducts balance, executes inference locally',
                        'This server does NOT relay or forward any inference requests'
                    ],
                    code: 'USE_DIRECT_CONNECTION'
                });

            } catch (e) {
                console.error('Error in endpoint discovery:', e);
                res.status(500).json({ error: 'Discovery service failed' });
            }
        });
        
        // Network stats
        this.app.get('/api/network/stats', async (req, res) => {
            try {
                const stats = await this.getNetworkStats();
                res.json(stats);
            } catch (error) {
                console.error('Error getting network stats:', error);
                res.status(500).json({ error: 'Failed to get network stats' });
            }
        });

        // Available models list - aggregated from registered miners
        this.app.get('/api/models', (req, res) => {
            try {
                const modelMap = new Map();
                for (const miner of this.miners.values()) {
                    const name = miner.model_name;
                    if (typeof name === 'string' && name !== '' && name !== 'Unknown' && name !== 'unknown' && name !== 'mining-only') {
                        if (!modelMap.has(name)) {
                            modelMap.set(name, {
                                id: name,
                                name: name,
                                description: miner.description || ('Model served by miner ' + (miner.miner_id || '').substring(0, 16)),
                                format: 'GGUF',
                                status: 'available',
                                miner_count: 1
                            });
                        } else {
                            modelMap.get(name).miner_count++;
                        }
                    }
                }
                const models = Array.from(modelMap.values());
                res.json({
                    success: true,
                    count: models.length,
                    models: models
                });
            } catch (error) {
                console.error('Error getting models:', error);
                res.status(500).json({ error: 'Failed to get models' });
            }
        });

        // Pricing configuration (ERR-MISSING-063 fix)
        this.app.get('/api/pricing', (req, res) => {
            try {
                const pricing = {
                    token_name: 'TKNC',
                    price_per_1m_tknc: {
                        min: 1,
                        max: 1000,
                        default: 10,
                        description: 'Tokens per TKNC (higher = cheaper for customer)'
                    },
                    api_key_min_payment: 100,
                    comment_cost: 10,
                    invite_cost: 10,
                    formula: '1 TKNC = 1,000,000 / price_per_1m_tknc tokens',
                    examples: [
                        { price: 10, tokens_per_tknc: '100K tokens' },
                        { price: 100, tokens_per_tknc: '10K tokens' },
                        { price: 1, tokens_per_tknc: '1M tokens' }
                    ]
                };
                res.json({
                    success: true,
                    pricing: pricing
                });
            } catch (error) {
                console.error('Error getting pricing:', error);
                res.status(500).json({ error: 'Failed to get pricing config' });
            }
        });

        // ===== MINER PRICE SETTING =====
        this.app.post('/api/miners/set-price', async (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                const walletName = this.userWallets.get(sessionToken);

                const { miner_id, tokens_per_tknc } = req.body;
                if (!miner_id) {
                    return res.status(400).json({ success: false, error: 'miner_id required' });
                }
                if (!tokens_per_tknc || tokens_per_tknc < 1) {
                    return res.status(400).json({ success: false, error: 'tokens_per_tknc must be >= 1' });
                }
                // Calculate price_per_1m_tknc for C++ miner API compatibility
                const price_per_1m_tknc = Math.max(1, Math.round(1000000 / tokens_per_tknc));

                // Verify the logged-in user owns the miner
                const miner = this.miners.get(miner_id);
                if (!miner) {
                    return res.status(404).json({ success: false, error: 'Miner not found' });
                }
                const userWallet = session.wallet;
                const ownsMiner = miner.wallet === userWallet;
                if (!ownsMiner) {
                    // Also check wallet_name associations
                    let found = false;
                    for (const [addr, name] of this.walletNameByAddress) {
                        if (addr === miner.wallet && name === walletName) {
                            found = true; break;
                        }
                    }
                    if (!found) {
                        return res.status(403).json({ success: false, error: 'You do not own this miner. Your wallet: ' + userWallet + ', miner wallet: ' + miner.wallet });
                    }
                }

                // Forward the price update to the miner's C++ API with signature verification
                let apiSuccess = false;
                let apiMessage = '';
                const cppPort = miner.api_port || 9332;

                // Try to get nonce + sign via node RPC, then forward to C++
                try {
                    const noncePostData = JSON.stringify({
                        miner_id: miner_id,
                        price_per_1m_tknc: parseInt(price_per_1m_tknc)
                    });
                    const nonceResult = await new Promise((resolve) => {
                        const opts = {
                            hostname: '127.0.0.1', port: cppPort,
                            path: '/api/v1/miners/price_nonce', method: 'POST',
                            headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(noncePostData) },
                            timeout: 5000
                        };
                        const req2 = http.request(opts, (res2) => {
                            let d = ''; res2.on('data', chunk => d += chunk);
                            res2.on('end', () => { try { resolve(JSON.parse(d)); } catch(e) { resolve(null); } });
                        });
                        req2.on('error', () => resolve(null));
                        req2.on('timeout', () => { req2.destroy(); resolve(null); });
                        req2.write(noncePostData); req2.end();
                    });

                    if (nonceResult && nonceResult.nonce) {
                        let signature = '';
                        try {
                            const signPost = JSON.stringify({
                                method: 'signmessage', params: [session.wallet, nonceResult.message], id: Date.now()
                            });
                            const signResult = await new Promise((resolve, reject) => {
                                const opts = {
                                    hostname: this.rpcUrl, port: this.rpcPort,
                                    path: `/wallet/${walletName || this.rpcWallet}`, method: 'POST',
                                    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(signPost),
                                        'Authorization': this.rpcAuth }, timeout: 10000
                                };
                                const reqS = http.request(opts, (resS) => {
                                    let d2 = ''; resS.on('data', chunk => d2 += chunk);
                                    resS.on('end', () => {
                                        try {
                                            const r = JSON.parse(d2);
                                            if (r.error) reject(new Error(r.error.message));
                                            else resolve(r.result);
                                        } catch(e) { reject(e); }
                                    });
                                });
                                reqS.on('error', (e) => reject(e));
                                reqS.on('timeout', () => { reqS.destroy(); reject(new Error('timeout')); });
                                reqS.write(signPost); reqS.end();
                            });
                            signature = signResult;
                            console.log(`[SetPrice] Signed via RPC wallet=${walletName}: ${signature.substring(0,20)}...`);
                        } catch (signErr) {
                            console.log('[SetPrice] signmessage failed:', signErr.message);
                        }

                        if (signature) {
                            const setPostData = JSON.stringify({
                                miner_id, price_per_1m_tknc: parseInt(price_per_1m_tknc),
                                wallet_address: session.wallet, signature, nonce: nonceResult.nonce
                            });
                            apiSuccess = await new Promise((resolve) => {
                                const opts3 = {
                                    hostname: '127.0.0.1', port: cppPort,
                                    path: '/api/v1/miners/set_price', method: 'POST',
                                    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(setPostData) },
                                    timeout: 5000
                                };
                                const req4 = http.request(opts3, (res4) => {
                                    let d3 = ''; res4.on('data', chunk => d3 += chunk);
                                    res4.on('end', () => {
                                        try { const r = JSON.parse(d3); resolve(r.status === 'success'); } catch(e) { resolve(false); }
                                    });
                                });
                                req4.on('error', () => resolve(false));
                                req4.on('timeout', () => { req4.destroy(); resolve(false); });
                                req4.write(setPostData); req4.end();
                            });
                            if (!apiSuccess) apiMessage = 'C++ signature verification failed';
                        } else {
                            apiMessage = 'Wallet private key unavailable for signing';
                        }
                    } else {
                        apiMessage = 'C++ price nonce unavailable';
                    }
                } catch (e) {
                    console.log('[SetPrice] Flow error:', e.message);
                    apiMessage = e.message;
                }

                // Always update web registry so display and discovery reflect the user's setting.
                // For remote miners the C++ API (127.0.0.1:9332) is unreachable from the server,
                // so apiSuccess tracks whether the miner confirmed — but the web price is authoritative.
                const p = parseInt(price_per_1m_tknc);
                miner.price_per_1m_tknc = p;
                miner.price_set_manually = true;
                miner.token_ratio = parseInt(tokens_per_tknc);
                if (apiSuccess) {
                    console.log(`[SetPrice] Miner ${miner_id}: tokens_per_tknc=${tokens_per_tknc}, price_per_1m_tknc=${p} (1 TKNC = ${tokens_per_tknc} tokens) — C++ confirmed`);
                } else {
                    console.warn(`[SetPrice] Miner ${miner_id}: tokens_per_tknc=${tokens_per_tknc} updated on web only (C++ unreachable: ${apiMessage})`);
                }

                res.json({
                    success: true,
                    miner_id: miner_id,
                    price_per_1m_tknc: p,
                    token_ratio: parseInt(tokens_per_tknc),
                    display: `1 TKNC = ${this.formatTokenRatio(parseInt(tokens_per_tknc))}`,
                    api_updated: apiSuccess,
                    message: apiSuccess
                        ? 'Price updated on miner and web gateway'
                        : 'Price updated on web gateway (miner will sync on next heartbeat)'
                });
            } catch (error) {
                console.error('[SetPrice] Error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to set price: ' + error.message });
            }
        });

        // Set public IP for miner (web-based, with signature verification)
        this.app.post('/api/miners/set-public-ip', async (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                const walletName = this.userWallets.get(sessionToken);

                const { miner_id, public_ip } = req.body;
                if (!miner_id) {
                    return res.status(400).json({ success: false, error: 'miner_id required' });
                }
                if (!public_ip || public_ip.trim() === '') {
                    return res.status(400).json({ success: false, error: 'public_ip required' });
                }
                const trimmedIp = public_ip.trim();
                if (trimmedIp === '127.0.0.1' || trimmedIp === '::1' || trimmedIp === 'localhost') {
                    return res.status(400).json({ success: false, error: 'Cannot set localhost as public IP. Use an actual public IPv4/IPv6 address or domain name.' });
                }

                const miner = this.miners.get(miner_id);
                if (!miner) {
                    return res.status(404).json({ success: false, error: 'Miner not found' });
                }
                const userWallet = session.wallet;
                const ownsMiner = miner.wallet === userWallet;
                if (!ownsMiner) {
                    let found = false;
                    for (const [addr, name] of this.walletNameByAddress) {
                        if (addr === miner.wallet && name === walletName) {
                            found = true; break;
                        }
                    }
                    if (!found) {
                        return res.status(403).json({ success: false, error: 'You do not own this miner.' });
                    }
                }

                let apiSuccess = false;
                let apiMessage = '';
                const cppPort = miner.api_port || 9332;

                try {
                    const nonceUrl = `http://127.0.0.1:${cppPort}/api/v1/miners/public_ip_nonce?miner_id=${encodeURIComponent(miner_id)}`;
                    const nonceResult = await new Promise((resolve) => {
                        http.get(nonceUrl, (res2) => {
                            let d = ''; res2.on('data', chunk => d += chunk);
                            res2.on('end', () => { try { resolve(JSON.parse(d)); } catch(e) { resolve(null); } });
                        }).on('error', () => resolve(null)).setTimeout(5000, function() { this.destroy(); resolve(null); });
                    });

                    if (nonceResult && nonceResult.nonce) {
                        let signature = '';
                        try {
                            const signPost = JSON.stringify({
                                method: 'signmessage', params: [session.wallet, nonceResult.message], id: Date.now()
                            });
                            const signResult = await new Promise((resolve, reject) => {
                                const opts = {
                                    hostname: this.rpcUrl, port: this.rpcPort,
                                    path: `/wallet/${walletName || this.rpcWallet}`, method: 'POST',
                                    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(signPost),
                                        'Authorization': this.rpcAuth }, timeout: 10000
                                };
                                const reqS = http.request(opts, (resS) => {
                                    let d2 = ''; resS.on('data', chunk => d2 += chunk);
                                    resS.on('end', () => {
                                        try { const r = JSON.parse(d2); if (r.error) reject(new Error(r.error.message)); else resolve(r.result); } catch(e) { reject(e); }
                                    });
                                });
                                reqS.on('error', (e) => reject(e));
                                reqS.on('timeout', () => { reqS.destroy(); reject(new Error('timeout')); });
                                reqS.write(signPost); reqS.end();
                            });
                            signature = signResult;
                        } catch (signErr) {
                            console.log('[SetPublicIP] Sign failed:', signErr.message);
                        }

                        if (signature) {
                            const setPostData = JSON.stringify({
                                miner_id, public_ip: trimmedIp,
                                wallet_address: session.wallet, signature, nonce: nonceResult.nonce
                            });
                            apiSuccess = await new Promise((resolve) => {
                                const opts3 = {
                                    hostname: '127.0.0.1', port: cppPort,
                                    path: '/api/v1/miners/set_public_ip', method: 'POST',
                                    headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(setPostData) },
                                    timeout: 5000
                                };
                                const req4 = http.request(opts3, (res4) => {
                                    let d3 = ''; res4.on('data', chunk => d3 += chunk);
                                    res4.on('end', () => {
                                        try { const r = JSON.parse(d3); resolve(r.status === 'success'); } catch(e) { resolve(false); }
                                    });
                                });
                                req4.on('error', () => resolve(false));
                                req4.on('timeout', () => { req4.destroy(); resolve(false); });
                                req4.write(setPostData); req4.end();
                            });
                            if (!apiSuccess) apiMessage = 'C++ signature verification failed';
                        } else {
                            apiMessage = 'Wallet private key unavailable for signing';
                        }
                    } else {
                        apiMessage = 'C++ public IP nonce unavailable';
                    }
                } catch (e) {
                    console.log('[SetPublicIP] Flow error:', e.message);
                    apiMessage = e.message;
                }

                if (apiSuccess) {
                    miner.public_ip = trimmedIp;
                    miner.ip_address = trimmedIp;
                    console.log(`[SetPublicIP] Miner ${miner_id}: public_ip=${trimmedIp}`);
                } else {
                    console.warn(`[SetPublicIP] C++ verification failed for ${miner_id}, web IP NOT updated: ${apiMessage}`);
                }

                res.json({
                    success: true,
                    miner_id: miner_id,
                    public_ip: trimmedIp,
                    api_updated: apiSuccess,
                    message: apiSuccess
                        ? 'Public IP updated on miner and web gateway'
                        : 'Public IP updated on web gateway (miner API verification pending)'
                });
            } catch (error) {
                console.error('[SetPublicIP] Error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to set public IP: ' + error.message });
            }
        });

        // ===== MINER DESCRIPTION SETTING =====
        this.app.post('/api/miners/set-description', async (req, res) => {
            try {
                const sessionToken = req.headers['x-session-token'];
                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    return res.status(401).json({ success: false, error: 'Authentication required. Please login first.' });
                }
                const session = this.sessions.get(sessionToken);
                const walletName = this.userWallets.get(sessionToken);

                const { miner_id, description } = req.body;
                if (!miner_id) {
                    return res.status(400).json({ success: false, error: 'miner_id required' });
                }
                if (description === undefined || description === null) {
                    return res.status(400).json({ success: false, error: 'description required' });
                }

                // Validate description length
                const desc = String(description).trim();
                if (desc.length > 500) {
                    return res.status(400).json({ success: false, error: 'Description too long (max 500 chars)' });
                }

                const miner = this.miners.get(miner_id);
                if (!miner) {
                    return res.status(404).json({ success: false, error: 'Miner not found' });
                }

                // Verify ownership: the logged-in wallet must own this miner
                const userWallet = session.wallet;
                if (miner.wallet !== userWallet) {
                    // Also check wallet name mapping
                    let found = false;
                    for (const [addr, name] of this.walletNameByAddress) {
                        if (addr === miner.wallet && name === walletName) {
                            found = true; break;
                        }
                    }
                    if (!found) {
                        return res.status(403).json({ success: false, error: 'You do not own this miner' });
                    }
                }

                miner.description = desc;
                try { this.persistence.save('miners', this.miners); } catch(e) {}
                this.broadcastMinerUpdate();

                console.log(`[SetDescription] Miner ${miner_id}: description updated by ${userWallet.substring(0,15)}...`);
                res.json({
                    success: true,
                    miner_id: miner_id,
                    description: desc,
                    message: 'Description updated successfully'
                });
            } catch (error) {
                console.error('[SetDescription] Error:', error.message);
                res.status(500).json({ success: false, error: 'Failed to set description: ' + error.message });
            }
        });

        // Serve main page
        this.app.get('/', (req, res) => {
            res.sendFile(path.join(__dirname, 'public', 'index.html'));
        });

        // AI Model Marketplace - serves /ai/* from ../AI/public
        const aiPublicPath = path.join(__dirname, '..', 'AI', 'public');
        this.app.use('/ai', express.static(aiPublicPath, { etag: true, maxAge: '7d', immutable: true }));
        this.app.get('/ai', (req, res) => {
            res.sendFile(path.join(aiPublicPath, 'index.html'));
        });
        this.app.get('/ai/', (req, res) => {
            res.sendFile(path.join(aiPublicPath, 'index.html'));
        });

        // Explorer - serves /explorer/* from ../explorer-tknc
        const explorerPath = path.join(__dirname, '..', 'explorer-tknc');
        this.app.use('/explorer', express.static(explorerPath, { etag: true, maxAge: '7d', immutable: true }));
        this.app.get('/explorer', (req, res) => {
            res.sendFile(path.join(explorerPath, 'index.html'));
        });
        this.app.get('/explorer/', (req, res) => {
            res.sendFile(path.join(explorerPath, 'index.html'));
        });

        // ===== Explorer API endpoints (proxied to node RPC) =====
        // Blockchain info: returns blocks, networkhashps, difficulty
        // Cached for 10 seconds to reduce RPC load during rapid page loads
        this.app.get('/explorer/api/blockchain/info', async (req, res) => {
            this._cachedResponse('blockchain_info', 10000, async () => {
                const info = await this.callRPC('getblockchaininfo', []);
                const hashps = await this.callRPC('getnetworkhashps', [20]);
                return {
                    blocks: info.blocks,
                    headers: info.headers,
                    difficulty: info.difficulty,
                    networkhashps: hashps,
                    chain: info.chain,
                    bestblockhash: info.bestblockhash
                };
            }, req, res);
        });

        // Mempool info - cached 10s
        this.app.get('/explorer/api/mempool/info', async (req, res) => {
            this._cachedResponse('mempool_info', 10000, async () => {
                const info = await this.callRPC('getmempoolinfo', []);
                return {
                    count: info.size,
                    size: info.size,
                    bytes: info.bytes,
                    usage: info.usage
                };
            }, req, res);
        });

        // Recent blocks: /blocks?limit=N - cached 15s
        this.app.get('/explorer/api/blocks', async (req, res) => {
            const limit = Math.min(parseInt(req.query.limit) || 10, 100);
            const cacheKey = 'blocks_' + limit;
            this._cachedResponse(cacheKey, 15000, async () => {
                const info = await this.callRPC('getblockchaininfo', []);
                const blocks = [];
                let height = info.blocks;
                let hash = info.bestblockhash;
                for (let i = 0; i < limit && height >= 0; i++) {
                    const block = await this.callRPC('getblock', [hash, 1]);
                    blocks.push({
                        height: block.height,
                        hash: block.hash,
                        time: block.time,
                        tx_count: block.tx ? block.tx.length : (block.nTx || 0),
                        size: block.size,
                        weight: block.weight
                    });
                    if (block.height === 0) break;
                    hash = block.previousblockhash;
                    height = block.height - 1;
                }
                return blocks;
            }, req, res);
        });

        // Recent transactions: /transactions/recent?limit=N
        // Returns recent transactions from both mempool and latest blocks.
        // Cached 15s to reduce RPC load.
        this.app.get('/explorer/api/transactions/recent', async (req, res) => {
            const limit = Math.min(parseInt(req.query.limit) || 10, 50);
            const cacheKey = 'recent_txs_' + limit;
            this._cachedResponse(cacheKey, 15000, async () => {
                const txs = [];

                // 1) Mempool transactions (unconfirmed)
                try {
                    const mempool = await this.callRPC('getrawmempool', [false]);
                    for (const txid of mempool.slice(0, limit)) {
                        try {
                            const raw = await this.callRPC('getrawtransaction', [txid, 1]);
                            const isCoinbase = raw.vin && raw.vin.length > 0 && raw.vin[0].coinbase;
                            let totalOut = 0;
                            if (raw.vout) {
                                for (const v of raw.vout) {
                                    // For coinbase txs, exclude team wallet output (10% dev share)
                                    if (isCoinbase && v.scriptPubKey && v.scriptPubKey.address === this.TEAM_WALLET_ADDRESS) continue;
                                    totalOut += (v.value || 0);
                                }
                            }
                            txs.push({
                                txid: raw.txid,
                                is_coinbase: !!isCoinbase,
                                block_height: -1,
                                block_time: raw.time || 0,
                                time: raw.time || 0,
                                input_count: raw.vin ? raw.vin.length : 0,
                                output_count: raw.vout ? raw.vout.length : 0,
                                total_out: Math.round(totalOut * 1e8),
                                size: raw.size || 0
                            });
                        } catch (e) {
                            // skip individual tx errors
                        }
                    }
                } catch (e) {
                    // mempool query failed — continue to block scan
                }

                // 2) Recent block transactions (confirmed, including coinbase)
                //    Walk back from the tip until we have `limit` transactions.
                if (txs.length < limit) {
                    try {
                        const info = await this.callRPC('getblockchaininfo', []);
                        let height = info.blocks;
                        let hash = info.bestblockhash;
                        const needed = limit - txs.length;
                        let scanned = 0;
                        // Scan at most `needed` blocks (most blocks have 1 coinbase tx)
                        const MAX_BLOCKS_TO_SCAN = Math.max(needed, 30);
                        while (height >= 0 && txs.length < limit && scanned < MAX_BLOCKS_TO_SCAN) {
                            const block = await this.callRPC('getblock', [hash, 2]);
                            const blockTxs = block.tx || [];
                            for (const tx of blockTxs) {
                                if (txs.length >= limit) break;
                                if (!tx || typeof tx !== 'object') continue;
                                const isCoinbase = tx.vin && tx.vin.length > 0 && tx.vin[0].coinbase;
                                let totalOut = 0;
                                if (tx.vout) {
                                    for (const v of tx.vout) {
                                        // For coinbase txs, exclude team wallet output (10% dev share)
                                        if (isCoinbase && v.scriptPubKey && v.scriptPubKey.address === this.TEAM_WALLET_ADDRESS) continue;
                                        totalOut += (v.value || 0);
                                    }
                                }
                                txs.push({
                                    txid: tx.txid,
                                    is_coinbase: !!isCoinbase,
                                    block_height: block.height,
                                    block_time: block.time,
                                    time: block.time,
                                    input_count: tx.vin ? tx.vin.length : 0,
                                    output_count: tx.vout ? tx.vout.length : 0,
                                    total_out: Math.round(totalOut * 1e8),
                                    size: tx.size || 0
                                });
                            }
                            scanned++;
                            if (block.height === 0 || !block.previousblockhash) break;
                            hash = block.previousblockhash;
                            height = block.height - 1;
                        }
                    } catch (e) {
                        // block scan failed — return what we have
                    }
                }

                return txs;
            }, req, res);
        });

        // Block detail: /block/:hashOrHeight
        this.app.get('/explorer/api/block/:query', async (req, res) => {
            try {
                const query = req.params.query;
                let hash = query;
                if (/^\d+$/.test(query)) {
                    hash = await this.callRPC('getblockhash', [parseInt(query)]);
                }
                // Use verbosity=2 to get full transaction data (vin/vout)
                // so the block detail page can display coinbase reward and outputs
                const block = await this.callRPC('getblock', [hash, 2]);
                let txStrings = [];
                let txsFull = [];
                if (block.tx && block.tx.length > 0) {
                    for (const t of block.tx.slice(0, 25)) {
                        if (typeof t === 'string') {
                            txStrings.push(t);
                            txsFull.push({ txid: t });
                        } else {
                            txStrings.push(t.txid);
                            const isCoinbase = t.vin && t.vin.length > 0 && !!t.vin[0].coinbase;
                            let totalOut = 0;
                            let teamReward = 0;
                            if (t.vout) {
                                for (const v of t.vout) {
                                    const val = v.value || 0;
                                    if (isCoinbase && v.scriptPubKey && v.scriptPubKey.address === this.TEAM_WALLET_ADDRESS) {
                                        teamReward += val;
                                    } else {
                                        totalOut += val;
                                    }
                                }
                            }
                            txsFull.push({
                                txid: t.txid,
                                is_coinbase: isCoinbase,
                                size: t.size || 0,
                                input_count: t.vin ? t.vin.length : 0,
                                output_count: t.vout ? t.vout.length : 0,
                                total_out: Math.round(totalOut * 1e8),
                                team_reward: Math.round(teamReward * 1e8),
                                block_height: block.height,
                                block_time: block.time,
                                time: block.time
                            });
                        }
                    }
                }
                // Calculate block reward (miner's share only = 90%, excluding 10% team share)
                let blockReward = 0;
                let blockTeamReward = 0;
                if (txsFull.length > 0 && txsFull[0].is_coinbase) {
                    blockReward = txsFull[0].total_out || 0;
                    blockTeamReward = txsFull[0].team_reward || 0;
                }
                const chainInfo = await this.callRPC('getblockchaininfo', []);
                const confirmations = chainInfo.blocks - block.height + 1;
                // TKNC coinbase maturity: outputs become spendable after COINBASE_MATURITY confirmations
                // Defined in src/consensus/consensus.h: static const int COINBASE_MATURITY = 60;
                const COINBASE_MATURITY = 60;
                const isMature = confirmations >= COINBASE_MATURITY;
                const maturityRemaining = Math.max(0, COINBASE_MATURITY - confirmations);

                res.json({
                    height: block.height,
                    hash: block.hash,
                    time: block.time,
                    size: block.size,
                    weight: block.weight,
                    version: block.version,
                    nonce: block.nonce,
                    bits: block.bits,
                    difficulty: block.difficulty,
                    merkleroot: block.merkleroot || block.merkle_root || '',
                    previousblockhash: block.previousblockhash,
                    nextblockhash: block.nextblockhash,
                    confirmations: confirmations,
                    tx_count: block.tx ? block.tx.length : (block.nTx || 0),
                    tx: txStrings,      // txid string array (backward compat for frontend)
                    txs: txsFull,       // full tx objects (with is_coinbase, total_out)
                    reward: blockReward,       // miner reward in satoshis (90%, excludes team share)
                    team_reward: blockTeamReward, // team/dev reward in satoshis (10%)
                    coinbase_maturity: COINBASE_MATURITY,
                    is_mature: isMature,
                    maturity_remaining: maturityRemaining
                });
            } catch (e) {
                res.status(500).json({ error: e.message });
            }
        });

        // Transaction detail: /tx/:txid
        // Node does not have txindex enabled, so getrawtransaction only works
        // for mempool txs. For confirmed txs we walk back recent blocks
        // (verbosity=2 returns full tx data) and look up the requested txid.
        // Response is converted to esplora-compatible format expected by frontend:
        //   vin[i].is_coinbase, vin[i].prevout.{scriptpubkey_address,value}
        //   vout[i].scriptpubkey_address, vout[i].value (in satoshis)
        //   tx.status.{confirmed,block_height,block_time}
        this.app.get('/explorer/api/tx/:txid', async (req, res) => {
            try {
                const txid = req.params.txid;
                let rawTx = null;
                let blockHeight = -1;
                let blockTime = 0;
                let confirmations = 0;

                // 1) Try mempool first (fast path)
                try {
                    rawTx = await this.callRPC('getrawtransaction', [txid, 1]);
                } catch (e) {
                    rawTx = null;
                }

                // 2) If not in mempool, walk back recent blocks (verbosity=2)
                if (!rawTx) {
                    const info = await this.callRPC('getblockchaininfo', []);
                    let height = info.blocks;
                    let hash = info.bestblockhash;
                    const MAX_BLOCKS_TO_SCAN = 200;
                    for (let i = 0; i < MAX_BLOCKS_TO_SCAN && height >= 0; i++) {
                        const block = await this.callRPC('getblock', [hash, 2]);
                        const txs = block.tx || [];
                        for (const t of txs) {
                            if (t && t.txid === txid) {
                                rawTx = t;
                                blockHeight = block.height;
                                blockTime = block.time;
                                confirmations = info.blocks - block.height + 1;
                                break;
                            }
                        }
                        if (rawTx) break;
                        if (block.height === 0 || !block.previousblockhash) break;
                        hash = block.previousblockhash;
                        height = block.height - 1;
                    }
                }

                if (!rawTx) {
                    return res.status(404).json({ error: 'Transaction not found in mempool or recent blocks' });
                }

                // Convert Bitcoin Core format → esplora format expected by frontend
                const isCoinbase = rawTx.vin && rawTx.vin.length > 0 && !!rawTx.vin[0].coinbase;
                const vin = (rawTx.vin || []).map(v => {
                    if (v.coinbase) {
                        return { is_coinbase: true, prevout: null };
                    }
                    const prevout = v.prevout ? {
                        scriptpubkey_address: v.prevout.scriptPubKey && v.prevout.scriptPubKey.address
                            ? v.prevout.scriptPubKey.address
                            : (v.prevout.scriptPubKey && v.prevout.scriptPubKey.addresses
                                ? v.prevout.scriptPubKey.addresses[0] : ''),
                        value: v.prevout.value ? Math.round(v.prevout.value * 1e8) : 0
                    } : null;
                    return {
                        txid: v.txid,
                        vout: v.vout,
                        prevout: prevout
                    };
                });
                const vout = (rawTx.vout || []).map(o => {
                    const spk = o.scriptPubKey || {};
                    return {
                        scriptpubkey_address: spk.address || (spk.addresses && spk.addresses.length > 0 ? spk.addresses[0] : ''),
                        value: Math.round((o.value || 0) * 1e8)
                    };
                });

                res.json({
                    txid: rawTx.txid,
                    version: rawTx.version,
                    locktime: rawTx.locktime,
                    size: rawTx.size,
                    weight: rawTx.weight,
                    fee: rawTx.fee ? Math.round(rawTx.fee * 1e8) : 0,
                    status: {
                        confirmed: blockHeight >= 0,
                        block_height: blockHeight,
                        block_time: blockTime
                    },
                    confirmations: confirmations,
                    vin: vin,
                    vout: vout
                });
            } catch (e) {
                res.status(500).json({ error: e.message });
            }
        });

        // Address detail: /address/:addr
        // Returns data in mempool.space/esplora-compatible format expected by frontend:
        //   chain_stats.funded_txo_sum / spent_txo_sum (in satoshis)
        //   utxos[] with txid, vout, value (in satoshis)
        //   transactions[] list of txids (empty — UTXO index only provides UTXOs)
        //
        // Primary: uses getaddressutxos RPC (O(k), requires -addressindex)
        // Fallback: uses scantxoutset RPC (O(n), for nodes without -addressindex)
        this.app.get('/explorer/api/address/:addr', async (req, res) => {
            try {
                const addr = req.params.addr;

                // Try the fast address index first.
                let utxos = [];
                let totalSat = 0;

                try {
                    const result = await this.callRPC('getaddressutxos', [addr]);
                    const rpcUtxos = result.utxos || [];
                    totalSat = Math.round((result.total_amount || 0) * 1e8);
                    utxos = rpcUtxos.map(u => ({
                        txid: u.txid,
                        vout: u.vout,
                        value: Math.round((u.amount || 0) * 1e8),
                        amount_sat: Math.round((u.amount || 0) * 1e8),
                        height: u.height,
                        coinbase: u.coinbase
                    }));
                } catch (indexErr) {
                    // Address index not enabled or error — fall back to scantxoutset.
                    console.warn('[Explorer] getaddressutxos failed, falling back to scantxoutset:', indexErr.message);
                    const result = await this.callRPC('scantxoutset', ['start', [{ 'desc': 'addr(' + addr + ')' }]]);
                    const unspents = result.unspents || [];
                    totalSat = Math.round((result.total_amount || 0) * 1e8);
                    utxos = unspents.map(u => ({
                        txid: u.txid,
                        vout: u.vout,
                        value: Math.round((u.amount || 0) * 1e8),
                        amount_sat: Math.round((u.amount || 0) * 1e8),
                        height: u.height,
                        coinbase: u.coinbase
                    }));
                }

                res.json({
                    address: addr,
                    chain_stats: {
                        funded_txo_sum: totalSat,
                        spent_txo_sum: 0,
                        tx_count: utxos.length
                    },
                    utxos: utxos,
                    transactions: []
                });
            } catch (e) {
                res.status(500).json({ error: e.message });
            }
        });

        // Broadcast transaction: /tx/broadcast (POST)
        this.app.post('/explorer/api/tx/broadcast', async (req, res) => {
            try {
                const rawTx = req.body.rawtx || req.body.hex || req.body.raw_tx;
                if (!rawTx) return res.status(400).json({ error: 'Missing rawtx' });
                const txid = await this.callRPC('sendrawtransaction', [rawTx]);
                res.json({ txid: txid, success: true });
            } catch (e) {
                res.status(500).json({ error: e.message });
            }
        });
    }
    
    setupWebSocket() {
        this.wss.on('connection', (ws, req) => {
            const clientType = req.url?.includes('/tunnel') ? 'tunnel' : 'client';

            if (clientType === 'tunnel') {
                // ===== MINER TUNNEL CONNECTION =====
                console.log('[Tunnel] Miner attempting to connect...');
                ws.isTunnel = true;

                ws.on('message', (data) => {
                    this.handleTunnelMessage(ws, data);
                });

                // Register tunnel on first auth message (handled in handleTunnelMessage)

            } else {
                // ===== BROWSER CLIENT CONNECTION =====
                const url = new URL(req.url || '/', `http://${req.headers.host}`);
                const sessionToken = url.searchParams.get('token') || req.headers['sec-websocket-protocol'];

                if (!sessionToken || !this.sessions.has(sessionToken)) {
                    // Allow unauthenticated connection for public data (miner list, stats)
                    // But mark as unauthenticated for sensitive operations
                    ws.isAuthenticated = false;
                    console.log('[WS] Unauthenticated browser client connected (public access only)');
                } else {
                    ws.isAuthenticated = true;
                    ws.sessionToken = sessionToken;
                    ws.wallet = this.sessions.get(sessionToken)?.wallet;
                    console.log(`[WS] Authenticated client connected: ${ws.wallet?.substring(0, 15)}...`);
                }

                this.clients.add(ws);

                ws.on('close', () => {
                    this.clients.delete(ws);
                });

                ws.on('message', (data) => {
                    this.handleClientMessage(ws, data);
                });

                this.broadcastMinerUpdate();
            }

            ws.on('close', () => {
                if (ws.isTunnel && ws.minerId) {
                    console.log(`[Tunnel] Miner ${ws.minerId} disconnected`);
                    this.tunnels.delete(ws.minerId);

                    const miner = this.miners.get(ws.minerId);
                    if (miner) {
                        miner.has_tunnel = false;
                        miner.tunnel_status = 'disconnected';
                    }
                    this.broadcastMinerUpdate();
                }
            });
        });

        console.log(`[Tunnel] WebSocket server configured - clients: /, tunnels: /ws/tunnel`);
    }

    handleTunnelMessage(ws, data) {
        try {
            const message = JSON.parse(data);

            switch (message.type) {
                case 'tunnel_register':
                    // Miners send: { type: "tunnel_register", miner_id: "xxx", ... }
                    const { miner_id, wallet, model_name } = message;

                    const walletKey = wallet || miner_id;

                    if (!walletKey) {
                        return ws.send(JSON.stringify({ type: 'error', message: 'wallet or miner_id required' }));
                    }

                    ws.minerId = walletKey;
                    this.tunnels.set(walletKey, ws);

                    let miner = this.miners.get(walletKey);
                    if (!miner) {
                        miner = {
                            miner_id: walletKey,
                            wallet: walletKey,
                            model_name: model_name || 'Unknown',
                            registered_at: Date.now(),
                            total_calls: 0,
                            revenue: 0
                        };
                        this.miners.set(walletKey, miner);
                    }

                    miner.last_heartbeat = Date.now();
                    miner.has_tunnel = true;
                    miner.tunnel_status = 'connected';
                    miner.connection_type = 'NAT-Tunnel';

                    console.log(`[Tunnel] ✓ Miner ${walletKey} connected via NAT tunnel`);

                    ws.send(JSON.stringify({
                        type: 'tunnel_registered',
                        miner_id: walletKey,
                        heartbeat_interval: this.HEARTBEAT_INTERVAL,
                        message: 'Tunnel established successfully'
                    }));

                    this.broadcastMinerUpdate();
                    break;

                case 'tunnel_heartbeat':
                    // Keep tunnel alive
                    if (ws.minerId) {
                        const m = this.miners.get(ws.minerId);
                        if (m) {
                            m.last_heartbeat = Date.now();
                            m.gpu_load = message.gpu_load || 0;
                            m.active_requests = message.active_requests || 0;
                        }
                        ws.send(JSON.stringify({ type: 'tunnel_heartbeat_ack', timestamp: Date.now() }));
                    }
                    break;

                case 'tunnel_response':
                    // Response from miner to forwarded request
                    const { request_id, response } = message;
                    const pendingRequest = this.tunnelRequests.get(request_id);

                    if (pendingRequest) {
                        clearTimeout(pendingRequest.timer);
                        pendingRequest.resolve(response);
                        this.tunnelRequests.delete(request_id);
                        console.log(`[Tunnel] Response received for request ${request_id}`);
                    }
                    break;

                default:
                    console.log(`[Tunnel] Unknown message type: ${message.type}`);
            }
        } catch (error) {
            console.error('[Tunnel] Message parse error:', error.message);
        }
    }

    handleClientMessage(ws, data) {
        try {
            const message = JSON.parse(data);
            switch (message.type) {
                case 'get_miners':
                    const minersList = Array.from(this.miners.values()).map(m => ({
                        ...m,
                        is_online: (Date.now() - m.last_heartbeat) < this.MINER_TIMEOUT || m.has_tunnel,
                        has_tunnel: !!m.has_tunnel,
                        connection_type: m.connection_type || 'direct'
                    }));
                    ws.send(JSON.stringify({ type: 'miners_list', data: minersList }));
                    break;
                case 'subscribe_miner':
                    ws.subscribedMiner = message.miner_id;
                    break;
            }
        } catch (error) {
            console.error('[WS] Client message error:', error.message);
        }
    }
    
    broadcastMinerUpdate() {
        const update = {
            type: 'miner_update',
            timestamp: Date.now(),
            online_count: this.getOnlineMinersCount(),
            total_count: this.miners.size
        };
        this.clients.forEach(client => {
            if (client.readyState === WebSocket.OPEN) {
                client.send(JSON.stringify(update));
            }
        });
    }

    // ===== P2P GATEWAY METHODS =====

    getOnlineMinersCount() {
        let count = 0;
        const now = Date.now();
        this.miners.forEach(m => {
            if ((now - m.last_heartbeat) < this.MINER_TIMEOUT) count++;
        });
        return count;
    }

    formatTokenRatio(ratio) {
        if (!ratio || ratio <= 0) return 'N/A';
        if (ratio >= 1000000) return (ratio / 1000000).toFixed(1) + 'M tokens';
        if (ratio >= 1000) return (ratio / 1000).toFixed(0) + 'K tokens';
        return ratio + ' tokens';
    }

    selectBestMiner() {
        const now = Date.now();
        let bestMiner = null;
        let bestScore = -1;

        this.miners.forEach(miner => {
            if ((now - miner.last_heartbeat) >= this.MINER_TIMEOUT) return;

            // Score based on: low load + few active requests
            const loadScore = 100 - (miner.gpu_load || 0);
            const requestScore = 10 - (Math.min(miner.active_requests || 0, 10));
            const score = loadScore + requestScore;

            if (score > bestScore) {
                bestScore = score;
                bestMiner = miner;
            }
        });

        return bestMiner;
    }

    // ===== ARCHITECTURE COMPLIANCE (2026-06-07) =====
    // A2.7/A2.8: Miners listen ONLY on 127.0.0.1:9332 (localhost).
    // External clients MUST NOT have direct IP:port access to miners.
    // REMOVED: routeInferenceRequest and sendViaTunnel functions
    // Server does NOT relay inference requests - clients connect DIRECTLY to miner endpoint

    async queryAPIKeyBalance(apiKey) {
        return new Promise((resolve, reject) => {
            http.get(`http://127.0.0.1:${this.apiPort}/api/v1/keys/balance?apikey=${encodeURIComponent(apiKey)}`, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    if (res.statusCode === 200) {
                        try { resolve(JSON.parse(data)); } catch (e) { reject(e); }
                    } else {
                        const error = new Error(`Miner API returned ${res.statusCode}`);
                        error.code = `MINER_${res.statusCode}`;
                        error.statusCode = res.statusCode;
                        try { error.body = JSON.parse(data); } catch (e2) { error.body = data; }
                        reject(error);
                    }
                });
            }).on('error', reject);
        });
    }

    startHeartbeatMonitor() {
        setInterval(() => {
            const now = Date.now();
            const PEER_TIMEOUT = 5 * 60 * 1000;
            const toRemove = [];
            this.miners.forEach((miner, id) => {
                if ((now - miner.last_heartbeat) >= this.MINER_TIMEOUT && miner.status !== 'offline') {
                    console.log(`[P2P] Miner ${id} went offline`);
                    miner.status = 'offline';
                    this.broadcastMinerUpdate();
                }
                if (miner.status === 'offline' && (now - miner.last_heartbeat) >= this.STALE_REMOVAL_TIMEOUT && !miner.discovered) {
                    toRemove.push(id);
                }
            });
            for (const id of toRemove) {
                console.log(`[P2P] Removing stale miner: ${id} (offline > ${this.STALE_REMOVAL_TIMEOUT/1000}s)`);
                this.miners.delete(id);
            }
            if (toRemove.length > 0) {
                this.broadcastMinerUpdate();
            }

            const stalePeers = [];
            this.peers.forEach((peer, nodeId) => {
                if ((now - peer.last_heartbeat) >= PEER_TIMEOUT) {
                    stalePeers.push(nodeId);
                }
            });
            for (const nodeId of stalePeers) {
                console.log(`[P2P] Removing stale peer: ${nodeId} (no heartbeat > ${(PEER_TIMEOUT/1000)}s)`);
                this.peers.delete(nodeId);
            }
        }, this.HEARTBEAT_INTERVAL);

        console.log(`[P2P] Heartbeat monitor started (${this.HEARTBEAT_INTERVAL}ms interval, ${this.MINER_TIMEOUT}ms timeout)`);

        // Immediate cleanup of any existing stale miners
        const now = Date.now();
        const toRemoveNow = [];
        this.miners.forEach((miner, id) => {
            if ((now - miner.last_heartbeat) >= this.STALE_REMOVAL_TIMEOUT && !miner.discovered) {
                toRemoveNow.push(id);
            }
        });
        for (const id of toRemoveNow) {
            console.log(`[P2P] Startup cleanup: removing stale miner ${id}`);
            this.miners.delete(id);
        }
        if (toRemoveNow.length > 0) {
            console.log(`[P2P] Startup cleanup: removed ${toRemoveNow.length} stale miners`);
            this.broadcastMinerUpdate();
        }

        const PEER_TIMEOUT = 5 * 60 * 1000;
        const stalePeerIds = [];
        this.peers.forEach((peer, nodeId) => {
            if ((now - peer.last_heartbeat) >= PEER_TIMEOUT) {
                stalePeerIds.push(nodeId);
            }
        });
        for (const nodeId of stalePeerIds) {
            console.log(`[P2P] Startup cleanup: removing stale peer ${nodeId}`);
            this.peers.delete(nodeId);
        }
        if (stalePeerIds.length > 0) {
            console.log(`[P2P] Startup cleanup: removed ${stalePeerIds.length} stale peers`);
        }

        // Network miner discovery: run every 15 seconds to auto-discover miners
        this.discoverNetworkMiners();
        setInterval(() => {
            this.discoverNetworkMiners();
        }, 15000);
        console.log('[Discovery] Network miner auto-discovery started (15s interval)');

        // Session cleanup (every hour)
        setInterval(() => {
            const now = Date.now();
            this.sessions.forEach((session, token) => {
                if ((now - session.last_activity) > this.SESSION_TIMEOUT) {
                    this.sessions.delete(token);
                    console.log(`[Auth] Session expired: ${token}`);
                }
            });
        }, 3600000); // 1 hour

        console.log(`[Auth] Session manager started (timeout: ${this.SESSION_TIMEOUT}ms)`);
    }
    
    async fetchMinersFromAPI() {
        return new Promise((resolve, reject) => {
            http.get(`http://127.0.0.1:${this.apiPort}/api/v1/miners`, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        resolve(JSON.parse(data));
                    } catch (e) {
                        reject(e);
                    }
                });
            }).on('error', reject);
        });
    }
    
    async fetchMinerDetail(minerId) {
        return new Promise((resolve, reject) => {
            const postData = JSON.stringify({ miner_id: minerId });
            const options = {
                hostname: '127.0.0.1',
                port: this.apiPort,
                path: '/api/v1/miners/detail',
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(postData) }
            };
            
            const req = http.request(options, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        const result = JSON.parse(data);
                        resolve(result);
                    } catch (e) {
                        reject(e);
                    }
                });
            });
            
            req.on('error', (e) => {
                console.error(`[API Key] Request error:`, e.message);
                reject(e);
            });

            // Timeout: destroy connection if no response within 10s
            req.on('timeout', () => {
                console.warn('[API Key] Request timeout after 10s, destroying connection');
                req.destroy();
                reject(new Error('API Key creation timeout - miner API not responding'));
            });

            req.write(postData);
            req.end();
        });
    }
    
    async createAPIKey(keyData, walletName) {
        const { target_miner_id, declared_limit, model, user_wallet, user_wallet_address } = keyData;

        // declared_limit = 0 means unlimited (bounded only by wallet balance).
        // Default is 0 (unlimited) if not specified by the user.
        // Web server does not handle inference billing, but must create escrow so the node
        // can track consumption and trigger on-chain transfers every 1 TKNC.
        const limit = Number(declared_limit) || 0;

        console.log(`[API Key] Creating key for miner: ${target_miner_id}, declared_limit: ${limit}, from wallet: ${walletName}`);

        const miner = this.miners.get(target_miner_id);
        if (!miner || !miner.wallet) {
            console.error(`[API Key] Miner ${target_miner_id} not found in registry`);
            return { error: 'Miner not found or has no wallet registered' };
        }
        const minerWallet = miner.wallet;
        console.log(`[API Key] Target miner wallet: ${minerWallet.substring(0,15)}...`);

        // ===== 1. Check miner online status =====
        const minerAvailable = await this.checkMinerAvailability(target_miner_id);
        if (!minerAvailable) {
            console.error(`[API Key] Miner ${target_miner_id} is offline, refusing key creation`);
            return {
                success: false,
                error: 'Miner is currently offline, cannot create API Key.',
                code: 'MINER_OFFLINE',
                hint: 'Please wait for the miner to come back online before trying again'
            };
        }
        console.log(`[API Key] Miner ${target_miner_id} is online, issuing credential...`);

        // ===== 2. Issue API Key via seed node RPC tknc_createapikey =====
        // The key is written to seed node's LevelDB and P2P-broadcast to all connected nodes.
        // Miners on any node can validate the key via local LevelDB lookup.
        // Per architecture: this RPC ONLY issues a credential — no on-chain payment, no locking.
        try {
            const rpcResult = await this.callRPC('tknc_createapikey', [limit, model || 'all', 365]);
            console.log(`[API Key] RPC tknc_createapikey success: key=${rpcResult.api_key}, synced_peers=${rpcResult.synced_peers}`);

            this.recordApiKeyCreation(rpcResult.api_key, {
                target_miner_id: target_miner_id,
                declared_limit: limit,
                model: model || 'Unknown',
                user_wallet: user_wallet || walletName
            });

            // ===== 2.5. Create escrow (spending limit) for pay-as-you-go billing =====
            // Without an escrow, CheckAndDeductEscrow returns false and billing never happens.
            // The escrow tracks: total_tknc (spending cap), consumed_tknc, miner_wallet for transfers.
            // Every 1 TKNC consumed → CheckAndDeductEscrow triggers TransferFromWallet to miner.
            try {
                // Calculate exchange rate from miner's price info.
                // price_per_1m_tknc = X TKNC per 1M tokens → rate_tokens_per_tknc = 1M / X
                const pricePer1m = miner.price_per_1m_tknc || 10;
                const rateTokensPerTknc = Math.max(1, Math.round(1000000 / pricePer1m));

                const escrowResult = await this.callRPC('createescrow', [
                    user_wallet_address || user_wallet,  // user_wallet (ADDRESS, not name)
                    minerWallet,                         // miner_wallet
                    limit,                               // amount_tknc (spending cap)
                    rateTokensPerTknc,                   // rate_tokens_per_tknc
                    model || 'all',                      // model_name
                    '',                                  // model_hash (optional)
                    rpcResult.api_key                    // api_key
                ]);
                console.log(`[API Key] Escrow created: id=${escrowResult.escrow_id}, total=${limit} TKNC, rate=${rateTokensPerTknc} tokens/TKNC`);
            } catch (escrowErr) {
                // Escrow creation failure is non-fatal — API key is still valid.
                // Inference will work but billing won't be tracked. Log warning.
                console.warn(`[API Key] createescrow failed (billing will not work): ${escrowErr.message}`);
            }

            // ===== 3. Build direct-connect endpoint (yellow-pages directory) =====
            // Client connects DIRECTLY to miner's node API Gateway for inference.
            // Web server does NOT relay or forward any inference requests.
            let publicIp = miner?.public_ip;
            const apiGwPort = 9313;  // Node's OpenAI-compatible API Gateway (port 9313, not 8080)

            // Fallback: If miner's public_ip is missing/local, query node RPC
            if (!publicIp || publicIp === '127.0.0.1' || publicIp === '::1' || publicIp === 'localhost') {
                try {
                    const nodeInfo = await this.callRPC('getpublicip', []);
                    if (nodeInfo && nodeInfo.public_ip && nodeInfo.public_ip !== '') {
                        publicIp = nodeInfo.public_ip;
                        console.log(`[API Key] Using node RPC public_ip fallback: ${publicIp}`);
                    }
                } catch (rpcErr) {
                    console.warn(`[API Key] Node RPC getpublicip failed: ${rpcErr.message}`);
                }
            }

            const endpoint = (publicIp && publicIp !== '127.0.0.1' && publicIp !== '::1' && publicIp !== 'localhost')
                ? (publicIp.includes(':')
                    ? `http://[${publicIp}]:${apiGwPort}/v1/chat/completions`
                    : `http://${publicIp}:${apiGwPort}/v1/chat/completions`)
                : null;

            return {
                api_key: rpcResult.api_key,
                declared_limit: limit,
                consumed_balance: 0,
                remaining_balance: limit,
                endpoint: endpoint,
                miner_id: target_miner_id,
                miner_wallet: minerWallet,
                model: model || 'Unknown',
                p2p_route: {
                    rpc_command: 'p2pinference',
                    model: model || 'qwen2.5-0.5b-instruct',
                    target_wallet: minerWallet
                },
                synced_peers: rpcResult.synced_peers || 0,
                mode: 'direct_connect',
                connection_type: 'direct',
                instructions: 'Connect DIRECTLY to the endpoint above with this api_key for inference. This server does NOT relay requests.',
                code: 'USE_DIRECT_CONNECTION'
            };
        } catch (rpcErr) {
            console.error(`[API Key] RPC tknc_createapikey failed:`, rpcErr.message);
            return {
                success: false,
                error: 'API Key creation failed: seed node RPC tknc_createapikey failed: ' + rpcErr.message,
                code: 'RPC_CREATE_APIKEY_FAILED'
            };
        }
    }

    // ===== Inference history tracking system =====

    async checkMinerAvailability(minerId) {
        // P2P Architecture: Check both miners and peers maps
        // Miners can register via /api/p2p/register (stored in peers) or /api/miners/register (stored in miners)
        let miner = this.miners.get(minerId);

        // Fallback: search in peers map if not found in miners
        if (!miner) {
            miner = Array.from(this.peers.values()).find(p =>
                p.wallet_address === minerId || p.node_id === minerId
            );
        }

        if (!miner) {
            console.log(`[Miner Health] ${minerId}: not found in miners/peers → offline`);
            return false;
        }

        // Check timestamp from either data source
        const lastSeen = new Date(miner.timestamp || miner.last_heartbeat || miner.last_seen || 0);
        const now = new Date();
        const diffSeconds = (now - lastSeen) / 1000;

        // Consider online if seen within last 5 minutes (P2P heartbeat interval)
        const isOnline = diffSeconds < 300;

        console.log(`[Miner Health] ${minerId}: age=${diffSeconds.toFixed(0)}s, status=${isOnline ? 'ONLINE' : 'OFFLINE'}, source=${this.miners.has(minerId) ? 'miners' : 'peers'}`);
        return isOnline;
    }

    // ===== NETWORK MINER DISCOVERY =====
    async discoverNetworkMiners() {
        console.log('[Discovery] Scanning for network miners...');
        try {
            // Method 1: Query the C++ miner API at default port (9332) for its known miners
            const minerApiUrl = `http://127.0.0.1:${this.apiPort}/api/v1/miners`;
            await new Promise((resolve) => {
                const req = http.get(minerApiUrl, (res) => {
                    let data = '';
                    res.on('data', chunk => data += chunk);
                    res.on('end', () => {
                        try {
                            const result = JSON.parse(data);
                            if (Array.isArray(result)) {
                                this.mergeDiscoveredMiners(result);
                            } else if (result.miners) {
                                this.mergeDiscoveredMiners(result.miners);
                            }
                        } catch (e) {
                            console.log('[Discovery] Failed to parse miner API response:', e.message);
                        }
                        resolve();
                    });
                });
                req.on('error', (e) => {
                    console.log('[Discovery] Miner API not reachable on port', this.apiPort, ':', e.message);
                    resolve();
                });
                req.setTimeout(5000, () => { req.destroy(); resolve(); });
            });

            // Method 2: Try the node RPC for additional miner info (getpeerinfo)
            try {
                const peerData = await this.callRPC('getpeerinfo', []);
                if (Array.isArray(peerData)) {
                    for (const peer of peerData) {
                        const peerAddr = peer.addr?.split(':')[0];
                        if (peerAddr && peerAddr !== '0.0.0.0') {
                            await this.probeMinerEndpoint(peerAddr, peer.miner_id || 'peer_' + peer.id);
                        }
                    }
                }
            } catch (e) {
                console.log('[Discovery] RPC getpeerinfo failed (expected if no peers):', e.message);
            }
        } catch (e) {
            console.log('[Discovery] Network scan error:', e.message);
        }
    }

    mergeDiscoveredMiners(minerList) {
        let addedCount = 0;
        let updatedCount = 0;
        for (const miner of minerList) {
            const walletKey = miner.wallet_address || miner.wallet || miner.miner_id;
            if (!walletKey) continue;

            const existing = this.miners.get(walletKey);

            if (existing && existing.last_heartbeat > 0) {
                existing.gpu_load = miner.gpu_utilization || existing.gpu_load;
                existing.hashrate = miner.hashrate || existing.hashrate;
                existing.vram_mb = miner.gpu_vram_total_mb || existing.vram_mb;
                if (miner.price_per_1m_tknc && !existing.price_set_manually) {
                    existing.price_per_1m_tknc = miner.price_per_1m_tknc;
                    existing.token_ratio = Math.round(1000000 / miner.price_per_1m_tknc);
                }
                updatedCount++;
            } else if (!existing) {
                const apiPort = miner.api_port || 9332;
                const apiHost = miner.ip_address || '127.0.0.1';
                fetch(`http://${apiHost}:${apiPort}/api/v1/miners`, { timeout: 2000 })
                    .then(res => {
                        if (res.ok) {
                            const tokenRatio = miner.price_per_1m_tknc
                                ? Math.round(1000000 / miner.price_per_1m_tknc)
                                : 100000;
                            const rawIp = miner.ip_address || miner.public_ip || null;
                            const publicIp = (rawIp && rawIp !== '127.0.0.1' && rawIp !== '::1' && rawIp !== 'localhost')
                                ? rawIp : null;
                            this.miners.set(walletKey, {
                                miner_id: walletKey,
                                wallet: walletKey,
                                model_name: miner.model_name || 'Unknown',
                                api_port: apiPort,
                                public_ip: publicIp,
                                ip_address: rawIp,
                                token_ratio: tokenRatio,
                                price_per_1m_tknc: miner.price_per_1m_tknc || 10,
                                vram_mb: miner.gpu_vram_total_mb || 0,
                                hashrate: miner.hashrate || 0,
                                gpu_load: miner.gpu_utilization || 0,
                                status: 'online',
                                last_heartbeat: Date.now(),
                                registered_at: Date.now(),
                                discovered: true,
                                total_calls: miner.total_inference_requests || 0,
                                revenue: miner.total_earned || 0
                            });
                            console.log(`[Discovery] ✓ Verified & added miner: ${walletKey} (${miner.model_name || 'Unknown'})`);
                            this.broadcastMinerUpdate();
                        }
                    })
                    .catch(() => {});
            }
        }
        if (updatedCount > 0) {
            console.log(`[Discovery] Updated ${updatedCount} live miners`);
        }
    }

    async probeMinerEndpoint(host, possibleMinerId) {
        return new Promise((resolve) => {
            const url = `http://${host}:${this.apiPort}/api/v1/miners`;
            const req = http.get(url, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        const result = JSON.parse(data);
                        if (Array.isArray(result)) {
                            this.mergeDiscoveredMiners(result);
                        } else if (result.miners) {
                            this.mergeDiscoveredMiners(result.miners);
                        }
                    } catch (e) {}
                    resolve();
                });
            });
            req.on('error', () => resolve());
            req.setTimeout(3000, () => { req.destroy(); resolve(); });
        });
    }

    recordApiKeyCreation(apiKey, keyData) {
        const { target_miner_id, declared_limit, model, user_wallet } = keyData;

        const record = {
            api_key: apiKey,
            miner_id: target_miner_id,
            model: model || 'Unknown',
            declared_limit: declared_limit || 0,    // Consumer-declared spending cap (display only, no locking)
            consumed_balance: 0,                     // TKNC already consumed (tracked for display)
            created_at: Date.now(),
            used_at: null,
            call_count: 0,
            user_wallet: user_wallet || null         // Creator wallet, set during key creation
        };

        this.inferenceHistory.set(apiKey, record);
        console.log(`[Inference History] API Key recorded: ${apiKey.substring(0, 12)}... → miner: ${target_miner_id}, declared_limit: ${declared_limit} TKNC`);
    }

    markApiKeyAsUsed(apiKey, userWallet, tokensUsed = 0) {
        if (this.inferenceHistory.has(apiKey)) {
            const record = this.inferenceHistory.get(apiKey);
            record.used_at = Date.now();
            record.call_count += 1;
            record.user_wallet = userWallet;

            // Track consumed balance using miner's token_ratio (display only — actual billing
            // happens on the client/miner nodes via per-1-TKNC on-chain transferTo).
            const miner = this.miners.get(record.miner_id);
            const ratio = miner?.token_ratio || 1000;
            const consumedTKNC = tokensUsed > 0 ? tokensUsed / ratio : 1;
            record.consumed_balance = (record.consumed_balance || 0) + consumedTKNC;

            // Update user-miner relation
            const relationKey = `${userWallet}:${record.miner_id}`;
            if (this.userMinerRelations.has(relationKey)) {
                const relation = this.userMinerRelations.get(relationKey);
                relation.last_call = Date.now();
                relation.total_calls += 1;
                relation.can_comment = true;  // Can comment after using model
            } else {
                this.userMinerRelations.set(relationKey, {
                    first_call: Date.now(),
                    last_call: Date.now(),
                    total_calls: 1,
                    can_comment: true,
                    miner_id: record.miner_id,
                    model: record.model
                });
            }

            console.log(`[Inference History] API Key used: ${apiKey.substring(0, 12)}... by user: ${userWallet?.substring(0, 15)}...`);
            return true;
        }
        return false;
    }

    getUserEligibleMiners(userWallet) {
        const eligibleMiners = [];

        this.userMinerRelations.forEach((relation, key) => {
            const [wallet, minerId] = key.split(':');
            if (wallet === userWallet && relation.can_comment) {
                // Find miner wallet address
                const miner = this.miners.get(relation.miner_id);
                const minerWallet = miner ? miner.wallet : null;
                const modelName = relation.model || (miner ? miner.model_name : 'Unknown');

                eligibleMiners.push({
                    miner_id: relation.miner_id,
                    wallet: minerWallet,
                    model_name: modelName,
                    first_call: relation.first_call,
                    last_call: relation.last_call,
                    total_calls: relation.total_calls,
                    interaction_duration: relation.last_call - relation.first_call
                });
            }
        });

        return eligibleMiners.sort((a, b) => b.last_call - a.last_call);  // Sort by most recent usage
    }

    checkCommentEligibility(userWallet, targetMinerId) {
        console.log(`[Eligibility] Checking: ${userWallet} → ${targetMinerId}`);
        
        const relationKey = `${userWallet}:${targetMinerId}`;

        if (!this.userMinerRelations.has(relationKey)) {
            return {
                eligible: false,
                reason: 'You have not used this miner model yet, cannot post comments. Please purchase an API Key and use the miner inference service first.'
            };
        }

        const relation = this.userMinerRelations.get(relationKey);

        if (!relation.can_comment) {
            return {
                eligible: false,
                reason: 'Your usage record does not meet the comment eligibility criteria.'
            };
        }

        return {
            eligible: true,
            relation: relation
        };
    }

    // REMOVED: forwardInferenceRequest and forwardInferenceRequestWithBody functions
    // Server does NOT relay inference requests - clients connect DIRECTLY to miner endpoint

    async getNetworkStats() {
        try {
            const cppStats = await this.fetchMinerStats();
            let blockHeight = cppStats.block_height ?? 0;
            if (!blockHeight || blockHeight === 0) {
                try {
                    const chainInfo = await this.callRPC('getblockchaininfo', []);
                    blockHeight = chainInfo?.blocks ?? 0;
                } catch (e) { /* keep cpp value */ }
            }
            const onlineMiners = this.getOnlineMinersCount();
            const totalMiners = this.miners.size;
            const models = new Set(Array.from(this.miners.values()).map(m => m.model_name)).size;
            return {
                total_miners: totalMiners,
                online_miners: onlineMiners,
                total_models: models,
                total_requests: Array.from(this.miners.values()).reduce((sum, m) => sum + (m.total_calls || 0), 0),
                block_height: blockHeight,
                current_difficulty: cppStats.current_difficulty ?? 0,
                source: 'miner_api'
            };
        } catch (e) {
            console.log('[Stats] Miner API unavailable, using node RPC fallback');
            return await this.getFallbackStats();
        }
    }

    fetchMinerStats() {
        return new Promise((resolve, reject) => {
            http.get(`http://127.0.0.1:${this.apiPort}/api/v1/network/stats`, (res) => {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => {
                    try {
                        const parsed = JSON.parse(data);
                        resolve(parsed.stats || parsed);
                    } catch (e) {
                        reject(e);
                    }
                });
            }).on('error', reject);
        });
    }

    async getFallbackStats() {
        const onlineMiners = this.getOnlineMinersCount();
        const totalMiners = this.miners.size;
        const models = new Set(Array.from(this.miners.values()).map(m => m.model_name)).size;
        let blockHeight = 0;
        let difficulty = 0;
        try {
            const chainInfo = await this.callRPC('getblockchaininfo', []);
            blockHeight = chainInfo?.blocks || 0;
            difficulty = chainInfo?.difficulty || 0;
            console.log(`[Stats] Fallback: Got real data from node RPC - height=${blockHeight}, diff=${difficulty}`);
        } catch (e) {
            console.error('[Stats] Fallback: Node RPC failed, using 0:', e.message);
        }
        return {
            total_miners: totalMiners,
            online_miners: onlineMiners,
            total_models: models,
            total_requests: Array.from(this.miners.values()).reduce((sum, m) => sum + (m.total_calls || 0), 0),
            block_height: blockHeight,
            current_difficulty: difficulty,
            tunnel_count: this.tunnels.size,
            source: 'node_rpc_fallback'
        };
    }

    setupErrorHandling() {
        // Catch-all for unhandled route errors
        this.app.use((err, req, res, next) => {
            // Don't leak stack traces, internal paths, or system details
            const isDev = process.env.TKNC_DEV_MODE === '1';
            const errorResponse = {
                success: false,
                error: err.message || 'Internal server error',
                ...(isDev ? { stack: err.stack } : {}) // Only expose stack in dev mode
            };

            // Log full error server-side but sanitize client response
            console.error(`[Error] ${req.method} ${req.url}: ${err.message}`);
            if (err.stack) console.error(`[Error-Stack] ${err.stack.substring(0, 500)}`);

            res.status(err.status || 500).json(errorResponse);
        });

        // 404 handler - don't reveal server structure
        this.app.use((req, res) => {
            res.status(404).json({ success: false, error: 'Resource not found' });
        });
        console.log('[Security] Error sanitization enabled (stack traces hidden in production)');
    }
    
    start() {
        this.server.listen(this.port, () => {
            console.log(`========================================`);
            console.log(`TKNC P2P Gateway Server Started`);
            console.log(`========================================`);

            // SECURITY: Warn loudly if DEV_MODE is enabled
            if (process.env.TKNC_DEV_MODE === '1') {
                console.log(`\n⚠️  ⚠️  ⚠️  DEV MODE WARNING ⚠️  ⚠️  ⚠️`);
                console.log(`DEV_MODE IS ENABLED — dev helpers active:`);
                console.log(`  - CORS: permissive (all origins allowed)`);
                console.log(`  - Error responses: include stack traces`);
                console.log(`  - Signature verification: ALWAYS enforced (not bypassable)`);
                console.log(`Avoid running with TKNC_DEV_MODE=1 in production.`);
                console.log(`⚠️  ⚠️  ⚠️  END WARNING ⚠️  ⚠️  ⚠️\n`);
            }

            console.log(`HTTP:     http://localhost:${this.port}`);
            console.log(`API Proxy: http://localhost:${this.port}/api`);
            console.log(`WebSocket: ws://localhost:${this.port}`);
            console.log(`Miner Reg:  /api/p2p/register`);
            console.log(`Heartbeat:  ${this.HEARTBEAT_INTERVAL}ms`);
            console.log(`Timeout:    ${this.MINER_TIMEOUT}ms`);
            console.log(`========================================`);
        });
    }
}

if (require.main === module) {
    const server = new TKNCWebServer();
    server.start();
}

module.exports = TKNCWebServer;
