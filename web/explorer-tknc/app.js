(function () {
    const API_BASE = '/explorer/api';
    let modalHistory = [];

    const I18N_DICT = {};
    ['en', 'zh', 'ja', 'ko', 'fr', 'de', 'es', 'pt', 'ru', 'ar', 'hi', 'th', 'vi'].forEach(function (code) {
        var holder = window['LANG_' + code];
        if (holder && holder[code]) I18N_DICT[code] = holder[code];
    });

    const DEFAULT_LANG = 'en';
    let CURRENT_LANG = localStorage.getItem('tknc_explorer_lang');
    if (!CURRENT_LANG || !I18N_DICT[CURRENT_LANG]) {
        CURRENT_LANG = DEFAULT_LANG;
    }

    function t(key) {
        const dict = I18N_DICT[CURRENT_LANG];
        return (dict && dict[key]) ? dict[key] : (I18N_DICT[DEFAULT_LANG] && I18N_DICT[DEFAULT_LANG][key]) || key;
    }

    function applyTranslations() {
        document.querySelectorAll('[data-i18n]').forEach(function (el) {
            const key = el.getAttribute('data-i18n');
            el.textContent = t(key);
        });
        document.querySelectorAll('[data-i18n-placeholder]').forEach(function (el) {
            const key = el.getAttribute('data-i18n-placeholder');
            el.placeholder = t(key);
        });
        const sel = document.getElementById('langSelector');
        if (sel) sel.value = CURRENT_LANG;
        document.documentElement.lang = CURRENT_LANG;
        
        // RTL support for Arabic
        document.documentElement.dir = (CURRENT_LANG === 'ar') ? 'rtl' : 'ltr';
    }

    function setLang(lang) {
        if (!I18N_DICT[lang]) return;
        CURRENT_LANG = lang;
        localStorage.setItem('tknc_explorer_lang', lang);
        applyTranslations();
        initTableLabels();
    }

    window.setLang = setLang;

    function formatNumber(num) {
        if (num >= 1e18) return (num / 1e18).toFixed(2) + ' EH/s';
        if (num >= 1e15) return (num / 1e15).toFixed(2) + ' PH/s';
        if (num >= 1e12) return (num / 1e12).toFixed(2) + ' TH/s';
        if (num >= 1e9) return (num / 1e9).toFixed(2) + ' GH/s';
        if (num >= 1e6) return (num / 1e6).toFixed(2) + ' MH/s';
        if (num >= 1e3) return (num / 1e3).toFixed(2) + ' KH/s';
        return num.toFixed(2) + ' H/s';
    }

    function formatTKN(amount) {
        return (amount / 1e8).toFixed(8) + ' TKNC';
    }

    function formatBytes(bytes) {
        if (bytes >= 1e6) return (bytes / 1e6).toFixed(2) + ' MB';
        if (bytes >= 1e3) return (bytes / 1e3).toFixed(2) + ' KB';
        return bytes + ' B';
    }

    function formatTime(timestamp) {
        const date = new Date(timestamp * 1000);
        return date.toLocaleString();
    }

    function shortHash(hash) {
        if (!hash) return '';
        if (hash.length <= 16) return hash;
        return hash.substring(0, 8) + '...' + hash.substring(hash.length - 8);
    }

    async function apiCall(path) {
        const resp = await fetch(API_BASE + path);
        if (!resp.ok) {
            const err = await resp.json().catch(() => ({}));
            throw new Error(err.error || 'API request failed');
        }
        return resp.json();
    }

    function openModal(title, contentHtml, pushHistory) {
        const modal = document.getElementById('modal');
        const modalTitle = document.getElementById('modalTitle');
        const modalBody = document.getElementById('modalBody');
        const backBtn = document.getElementById('modalBackBtn');

        if (pushHistory !== false) {
            modalHistory.push({ title: modalTitle.textContent, content: modalBody.innerHTML });
        }

        modalTitle.textContent = title;
        modalBody.innerHTML = contentHtml;
        modal.classList.remove('hidden');
        document.body.style.overflow = 'hidden';

        backBtn.style.display = modalHistory.length > 0 ? 'block' : 'none';
    }

    function closeModal() {
        document.getElementById('modal').classList.add('hidden');
        document.body.style.overflow = '';
        modalHistory = [];
        document.getElementById('modalBackBtn').style.display = 'none';
    }

    function modalBack() {
        if (modalHistory.length === 0) return;
        const prev = modalHistory.pop();
        document.getElementById('modalTitle').textContent = prev.title;
        document.getElementById('modalBody').innerHTML = prev.content;
        document.getElementById('modalBackBtn').style.display = modalHistory.length > 0 ? 'block' : 'none';
    }

    window.closeModal = closeModal;
    window.modalBack = modalBack;

    document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape') closeModal();
    });

    async function loadStats() {
        try {
            const info = await apiCall('/blockchain/info');
            if (info && info.blocks !== undefined) {
                document.getElementById('blockHeight').textContent = Number(info.blocks).toLocaleString();
            }
            if (info && info.networkhashps !== undefined) {
                document.getElementById('hashrate').textContent = formatNumber(Number(info.networkhashps));
            }
            if (info && info.difficulty !== undefined) {
                const diff = Number(info.difficulty);
                document.getElementById('difficulty').textContent = diff < 0.01 ? diff.toFixed(6) : diff.toLocaleString();
            }
        } catch (e) {
            console.error('Failed to load stats:', e);
        }

        try {
            const mempool = await apiCall('/mempool/info');
            const count = mempool.count !== undefined ? mempool.count : mempool.size;
            document.getElementById('mempoolSize').textContent = count ? Number(count).toLocaleString() + ' tx' : '0 tx';
        } catch (e) {
            console.error('Failed to load mempool:', e);
        }
    }

    async function loadBlocks() {
        try {
            const blocks = await apiCall('/blocks?limit=10');
            const tbody = document.getElementById('blocksBody');
            tbody.innerHTML = '';
            blocks.forEach(block => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td><a href="#" class="hash" onclick="showBlock(${block.height}); return false;">${block.height}</a></td>
                    <td><a href="#" class="hash" onclick="showBlock('${block.hash}'); return false;">${shortHash(block.hash)}</a></td>
                    <td>${formatTime(block.time)}</td>
                    <td>${block.tx_count || 'N/A'}</td>
                    <td class="mono">${block.size ? formatBytes(block.size) : 'N/A'}</td>
                    <td class="mono">${block.weight ? (block.weight / 1000).toFixed(1) + ' KW' : 'N/A'}</td>
                `;
                tbody.appendChild(tr);
            });
            initTableLabels();
        } catch (e) {
            document.getElementById('blocksBody').innerHTML = '<tr><td colspan="6" class="loading">' + t('loadingBlocks') + '</td></tr>';
            console.error(e);
        }
    }

    async function loadRecentTxs() {
        try {
            const txs = await apiCall('/transactions/recent?limit=10');
            const tbody = document.getElementById('recentTxsBody');
            tbody.innerHTML = '';
            if (!txs || txs.length === 0) {
                tbody.innerHTML = '<tr><td colspan="6" class="loading">' + t('noTxs') + '</td></tr>';
                return;
            }
            txs.forEach(tx => {
                const tr = document.createElement('tr');
                const coinbaseLabel = tx.is_coinbase ? ' [C]' : '';
                tr.innerHTML = `
                    <td><a href="#" class="hash" onclick="showTx('${tx.txid}'); return false;">${shortHash(tx.txid)}${coinbaseLabel}</a></td>
                    <td><a href="#" class="hash" onclick="showBlock(${tx.block_height}); return false;">#${tx.block_height}</a></td>
                    <td>${formatTime(tx.block_time)}</td>
                    <td class="mono">${tx.input_count}</td>
                    <td class="mono">${tx.output_count}</td>
                    <td class="mono" style="color: var(--green);">${formatTKN(tx.total_out)}</td>
                `;
                tbody.appendChild(tr);
            });
            initTableLabels();
        } catch (e) {
            document.getElementById('recentTxsBody').innerHTML = '<tr><td colspan="6" class="loading">' + t('loadingTxs') + '</td></tr>';
            console.error(e);
        }
    }

    async function showBlock(query) {
        // Check if modal is already open (with real content, not loading state)
        const modal = document.getElementById('modal');
        const isAlreadyOpen = !modal.classList.contains('hidden');
        const isLoading = document.getElementById('modalBody').innerHTML.includes('loadingBlock') || 
                          document.getElementById('modalTitle').textContent.indexOf(t('loading')) >= 0;
        
        // Only push history if navigating from an existing detail view (not from home/loading)
        if (isAlreadyOpen && !isLoading) {
            openModal(t('loadingBlock'), '<div class="detail-card"><h2>' + t('loadingBlock') + '</h2></div>');
        } else {
            openModal(t('loadingBlock'), '<div class="detail-card"><h2>' + t('loadingBlock') + '</h2></div>', false);
        }
        
        try {
            const block = await apiCall('/block/' + query);
            // Prefer full tx objects (block.txs) for richer display; fall back to txid strings (block.tx)
            const txsFull = block.txs || [];
            const txStrings = block.tx || [];
            let txHtml = '';
            if (txsFull.length > 0 && txsFull[0] && txsFull[0].is_coinbase !== undefined) {
                // Full tx objects available - show coinbase marker, I/O counts, and total output
                txsFull.forEach((tx, i) => {
                    const cb = tx.is_coinbase ? ' [C]' : '';
                    const total = tx.total_out ? formatTKN(tx.total_out) : '';
                    const meta = total ? `<span style="color: var(--green);">${total}</span>` : '';
                    txHtml += `<div class="tx-io">
                        <span class="addr" onclick="showTx('${tx.txid}')">#${i} ${shortHash(tx.txid)}${cb}</span>
                        <span>${meta}</span>
                    </div>`;
                });
            } else {
                // Only txid strings - basic display
                txStrings.forEach((txid, i) => {
                    txHtml += `<div class="tx-io">
                        <span class="addr" onclick="showTx('${txid}')">#${i} ${shortHash(txid)}</span>
                    </div>`;
                });
            }

            const prevHash = block.prev_block_hash || block.previousblockhash;
            const nextHash = block.next_block_hash;

            const content = `
                <div class="block-nav" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem;">
                    <button class="btn" onclick="${prevHash ? `showBlock('${prevHash}')` : ''}" ${prevHash ? '' : 'disabled style="opacity: 0.4; cursor: not-allowed;"'}>
                        &larr; ${t('prevBlock')}
                    </button>
                    <span style="font-weight: 600;">${t('blockTitle')} #${block.height}</span>
                    <button class="btn" onclick="${nextHash ? `showBlock('${nextHash}')` : ''}" ${nextHash ? '' : 'disabled style="opacity: 0.4; cursor: not-allowed;"'}>
                        ${t('nextBlock')} &rarr;
                    </button>
                </div>
                <div class="detail-card">
                    <div class="detail-grid">
                        <div class="detail-item" style="grid-column: 1 / -1;">
                            <div class="detail-label">${t('detailHash')}</div>
                            <div class="detail-value mono" style="font-size: 0.85rem; word-break: break-all;">${block.hash}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailHeight')}</div>
                            <div class="detail-value">#${block.height.toLocaleString()}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailConfirmations')}</div>
                            <div class="detail-value">${block.confirmations ? block.confirmations.toLocaleString() : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailTime')}</div>
                            <div class="detail-value">${formatTime(block.time)}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailTransactions')}</div>
                            <div class="detail-value">${(block.tx_count || block.nTx || txsFull.length || txStrings.length || 0).toLocaleString()}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailSize')}</div>
                            <div class="detail-value">${block.size ? formatBytes(block.size) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailWeight')}</div>
                            <div class="detail-value">${block.weight ? block.weight.toLocaleString() + ' WU' : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailDifficulty')}</div>
                            <div class="detail-value">${block.difficulty ? (Number(block.difficulty) < 0.01 ? Number(block.difficulty).toFixed(6) : Number(block.difficulty).toLocaleString()) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailReward')}</div>
                            <div class="detail-value" style="color: var(--green); font-weight: 600;">${block.reward ? formatTKN(block.reward) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailMaturity') || 'Maturity'}</div>
                            <div class="detail-value" style="color: ${block.is_mature ? 'var(--green)' : 'var(--orange)'}; font-weight: 600;">
                                ${block.is_mature
                                    ? '&#10003; ' + (t('matureOk') || 'Mature (spendable)')
                                    : '&#9203; ' + (t('maturePending') || 'Immature') + ' (' + block.maturity_remaining + ' ' + (t('maturityBlocksLeft') || 'blocks left') + ')'}
                            </div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailFees')}</div>
                            <div class="detail-value" style="color: var(--yellow);">${block.fees ? formatTKN(block.fees) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailVersion')}</div>
                            <div class="detail-value">${block.version || 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailNonce')}</div>
                            <div class="detail-value">${block.nonce != null ? block.nonce.toLocaleString() : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailBits')}</div>
                            <div class="detail-value">${block.bits || 'N/A'}</div>
                        </div>
                        <div class="detail-item" style="grid-column: 1 / -1;">
                            <div class="detail-label">${t('detailMerkleRoot')}</div>
                            <div class="detail-value mono" style="font-size: 0.85rem; word-break: break-all;">${block.merkleroot || block.merkle_root || 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailPrevBlock')}</div>
                            <div class="detail-value">${prevHash ? `<a href="#" class="hash" onclick="showBlock('${prevHash}'); return false;">${shortHash(prevHash)}</a>` : t('genesisBlock')}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailNextBlock')}</div>
                            <div class="detail-value">${nextHash ? `<a href="#" class="hash" onclick="showBlock('${nextHash}'); return false;">${shortHash(nextHash)}</a>` : t('latestBlock')}</div>
                        </div>
                    </div>
                </div>
                <div class="detail-card">
                    <h2>${t('detailTransactions')} (${block.tx_count || txsFull.length || txStrings.length || 0})</h2>
                    <div class="tx-outputs">${txHtml || '<p style="color: var(--text-muted)">' + t('noTxs') + '</p>'}</div>
                </div>
            `;

            openModal(t('blockTitle') + ' #' + block.height, content, false);
        } catch (e) {
            openModal(t('blockNotFound'), '<div class="detail-card"><h2>' + t('blockNotFound') + '</h2><p style="color: var(--text-muted)">' + t('blockNotFoundDesc') + '</p></div>');
        }
    }

    window.showBlock = showBlock;

    async function showTx(txid) {
        // Check if modal is already open (with real content, not loading state)
        const modal = document.getElementById('modal');
        const isAlreadyOpen = !modal.classList.contains('hidden');
        const isLoading = document.getElementById('modalBody').innerHTML.includes('loadingTx') || 
                          document.getElementById('modalTitle').textContent.indexOf(t('loading')) >= 0;
        
        // Only push history if navigating from an existing detail view
        if (isAlreadyOpen && !isLoading) {
            openModal(t('loadingTx'), '<div class="detail-card"><h2>' + t('loadingTx') + '</h2></div>');
        } else {
            openModal(t('loadingTx'), '<div class="detail-card"><h2>' + t('loadingTx') + '</h2></div>', false);
        }
        
        try {
            const tx = await apiCall('/tx/' + txid);
            const vin = tx.vin || [];
            const vout = tx.vout || [];

            let vinHtml = '';
            vin.forEach((input, i) => {
                if (input.is_coinbase) {
                    vinHtml += `<div class="tx-io">
                        <span><span style="display:inline-block;padding:2px 8px;background:rgba(251,191,36,0.15);color:#fbbf24;border-radius:4px;font-size:0.75rem;font-weight:600;margin-right:8px;">${'\u53d1\u9001\u65b9'}</span>[C] <span style="color: var(--yellow); font-weight: 600;">${t('txMiner')}</span></span>
                        <span class="amount" style="color: var(--green);">${t('txReward')}</span>
                    </div>`;
                } else {
                    const addr = input.prevout && input.prevout.scriptpubkey_address ? input.prevout.scriptpubkey_address : 'Unknown';
                    const val = input.prevout ? formatTKN(input.prevout.value) : 'N/A';
                    vinHtml += `<div class="tx-io">
                        <span><span style="display:inline-block;padding:2px 8px;background:rgba(251,191,36,0.15);color:#fbbf24;border-radius:4px;font-size:0.75rem;font-weight:600;margin-right:8px;">${'\u53d1\u9001\u65b9'}</span><span class="addr" onclick="showAddress('${addr}')">${addr}</span></span>
                        <span class="amount">${val}</span>
                    </div>`;
                }
            });

            let voutHtml = '';
            const isCoinbase = vin.length > 0 && vin[0].is_coinbase;
            vout.forEach((output, i) => {
                let addr = output.scriptpubkey_address || t('txNoAddress');
                let addrClick = output.scriptpubkey_address ? `showAddress('${addr}')` : '';
                if (isCoinbase && !output.scriptpubkey_address) {
                    addr = '[C] ' + t('txMiner');
                    addrClick = '';
                }
                voutHtml += `<div class="tx-io">
                    <span><span style="display:inline-block;padding:2px 8px;background:rgba(34,197,94,0.15);color:#22c55e;border-radius:4px;font-size:0.75rem;font-weight:600;margin-right:8px;">${'\u63a5\u6536\u65b9'}</span><span class="addr" onclick="${addrClick}">${addr}</span></span>
                    <span class="amount">${formatTKN(output.value)}</span>
                </div>`;
            });

            let totalIn = 0, totalOut = 0;
            vin.forEach(i => { if (i.prevout && i.prevout.value) totalIn += i.prevout.value; });
            vout.forEach(o => { totalOut += o.value; });
            const fee = tx.fee || (totalIn > 0 && totalOut > 0 ? totalIn - totalOut : 0);
            const feeRate = tx.fee_rate || 0;

            const isConfirmed = tx.status && tx.status.confirmed;
            const confirmations = tx.confirmations || 0;

            const content = `
                <div class="detail-card">
                    <div class="detail-grid">
                        <div class="detail-item" style="grid-column: 1 / -1;">
                            <div class="detail-label">${t('detailHash')}</div>
                            <div class="detail-value mono" style="font-size: 0.85rem; word-break: break-all;">${tx.txid || txid}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('txStatus')}</div>
                            <div class="detail-value" style="color: ${isConfirmed ? 'var(--green)' : 'var(--orange)'}; font-weight: 600;">
                                ${isConfirmed ? '&#10003; ' + t('txConfirmed') : '&#9203; ' + t('txUnconfirmed')}
                            </div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailConfirmations')}</div>
                            <div class="detail-value">${isConfirmed ? confirmations.toLocaleString() : '0 (' + t('txMempool') + ')'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('txBlock')}</div>
                            <div class="detail-value">${isConfirmed && tx.status.block_height ? `<a href="#" class="hash" onclick="showBlock(${tx.status.block_height}); return false;">#${tx.status.block_height}</a>` : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailTime')}</div>
                            <div class="detail-value">${tx.status && tx.status.block_time ? formatTime(tx.status.block_time) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailSize')}</div>
                            <div class="detail-value">${tx.size ? formatBytes(tx.size) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailWeight')}</div>
                            <div class="detail-value">${tx.weight ? tx.weight.toLocaleString() + ' WU' : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('txFee')}</div>
                            <div class="detail-value">${fee > 0 ? formatTKN(fee) : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('txFeeRate')}</div>
                            <div class="detail-value">${feeRate > 0 ? feeRate.toLocaleString() + ' sat/vB' : 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('detailVersion')}</div>
                            <div class="detail-value">${tx.version || 'N/A'}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('txLocktime')}</div>
                            <div class="detail-value">${tx.locktime || 0}</div>
                        </div>
                    </div>
                </div>
                <div class="detail-card">
                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 1rem;">
                        <h2 style="margin: 0;">${'\u27f0'} ${t('txInputs')} (${vin.length})</h2>
                        <span style="color: var(--text-muted); font-size: 0.9rem;">${t('txTotal')}: ${totalIn > 0 ? formatTKN(totalIn) : 'N/A'}</span>
                    </div>
                    <div class="tx-inputs">${vinHtml}</div>
                    <div class="tx-arrow">&#8595;</div>
                    <div style="display: flex; justify-content: space-between; align-items: center; margin: 1rem 0 0.5rem 0;">
                        <h2 style="margin: 0;">${'\u27fe'} ${t('txOutputs')} (${vout.length})</h2>
                        <span style="color: var(--text-muted); font-size: 0.9rem;">${t('txTotal')}: ${formatTKN(totalOut)}</span>
                    </div>
                    <div class="tx-outputs">${voutHtml}</div>
                </div>
            `;

            openModal(t('txTitle'), content, false);
        } catch (e) {
            openModal(t('txNotFound'), '<div class="detail-card"><h2>' + t('txNotFound') + '</h2><p style="color: var(--text-muted)">' + t('txNotFoundDesc') + '</p></div>');
        }
    }

    window.showTx = showTx;

    async function showAddress(address) {
        // Check if modal is already open (with real content, not loading state)
        const modal = document.getElementById('modal');
        const isAlreadyOpen = !modal.classList.contains('hidden');
        const isLoading = document.getElementById('modalBody').innerHTML.includes('loadingAddress') || 
                          document.getElementById('modalTitle').textContent.indexOf(t('loading')) >= 0;
        
        // Only push history if navigating from an existing detail view
        if (isAlreadyOpen && !isLoading) {
            openModal(t('loadingAddress'), '<div class="detail-card"><h2>' + t('loadingAddress') + '</h2></div>');
        } else {
            openModal(t('loadingAddress'), '<div class="detail-card"><h2>' + t('loadingAddress') + '</h2></div>', false);
        }
        
        try {
            const addrInfo = await apiCall('/address/' + address);
            const txs = addrInfo.transactions || [];
            const utxos = addrInfo.utxos || [];

            const balance = (addrInfo.chain_stats ? addrInfo.chain_stats.funded_txo_sum - addrInfo.chain_stats.spent_txo_sum : 0);
            const totalReceived = addrInfo.chain_stats ? addrInfo.chain_stats.funded_txo_sum : 0;
            const totalSpent = addrInfo.chain_stats ? addrInfo.chain_stats.spent_txo_sum : 0;
            const txCount = addrInfo.chain_stats ? addrInfo.chain_stats.tx_count : txs.length;

            let utxoHtml = '';
            const displayUtxos = utxos.slice(0, 10);
            displayUtxos.forEach((u, i) => {
                utxoHtml += `<div class="tx-io">
                    <span><a href="#" class="hash" onclick="showTx('${u.txid}'); return false;">${shortHash(u.txid)}:${u.vout}</a></span>
                    <span class="amount" style="color: var(--green);">${formatTKN(u.value || u.amount_sat || 0)}</span>
                </div>`;
            });

            let txHtml = '';
            const displayTxs = txs.slice(0, 10);
            displayTxs.forEach(txid => {
                txHtml += `<div class="tx-io">
                    <span class="addr" onclick="showTx('${txid}')">${shortHash(txid)}</span>
                </div>`;
            });

            const content = `
                <div class="detail-card">
                    <h2>${t('addressTitle')}</h2>
                    <div class="detail-grid">
                        <div class="detail-item" style="grid-column: 1 / -1;">
                            <div class="detail-label">${t('detailHash')}</div>
                            <div class="detail-value" style="font-size: 0.95rem; word-break: break-all;">${address}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('addrFinalBalance')}</div>
                            <div class="detail-value" style="color: var(--green); font-size: 1.2rem; font-weight: 700;">${formatTKN(balance)}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('addrTotalReceived')}</div>
                            <div class="detail-value">${formatTKN(totalReceived)}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('addrTotalSent')}</div>
                            <div class="detail-value">${formatTKN(totalSpent)}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('addrTxCount')}</div>
                            <div class="detail-value">${txCount.toLocaleString()}</div>
                        </div>
                        <div class="detail-item">
                            <div class="detail-label">${t('addrUtxoCount')}</div>
                            <div class="detail-value">${utxos.length.toLocaleString()}</div>
                        </div>
                    </div>
                </div>
                <div class="detail-card">
                    <h2>${t('addrUtxos')} (${Math.min(utxos.length, 10)}/${utxos.length})</h2>
                    <div class="tx-outputs">${utxoHtml || '<p style="color: var(--text-muted)">' + t('addrNoUtxos') + '</p>'}</div>
                </div>
                ${txs.length > 0 ? `
                <div class="detail-card">
                    <h2>${t('addrTxCount')} (${Math.min(txs.length, 10)}/${txs.length})</h2>
                    <div class="tx-outputs">${txHtml || '<p style="color: var(--text-muted)">' + t('addrNoTxs') + '</p>'}</div>
                </div>` : ''}
            `;

            openModal(t('addressTitle'), content, false);
        } catch (e) {
            openModal(t('addressNotFound'), '<div class="detail-card"><h2>' + t('addressNotFound') + '</h2><p style="color: var(--text-muted)">' + t('addressNotFoundDesc') + '</p></div>');
        }
    }

    window.showAddress = showAddress;

    function showBroadcast() {
        const content = `
            <div class="detail-card">
                <h2>${t('broadcastHeading')}</h2>
                <p style="color: var(--text-muted); margin-bottom: 1rem;">${t('broadcastDesc')}</p>
                <textarea id="broadcastHex" class="broadcast-textarea" placeholder="${t('broadcastPlaceholder')}"></textarea>
                <div style="margin-top: 1rem; display: flex; gap: 1rem; align-items: center;">
                    <button class="btn" onclick="broadcastTx()">${t('broadcastBtn')}</button>
                    <span id="broadcastResult" style="font-size: 0.9rem;"></span>
                </div>
            </div>
        `;
        openModal(t('broadcastTitle'), content);
    }

    window.showBroadcast = showBroadcast;

    async function broadcastTx() {
        const hex = document.getElementById('broadcastHex').value.trim();
        const resultEl = document.getElementById('broadcastResult');
        if (!hex) {
            resultEl.style.color = 'var(--red)';
            resultEl.textContent = t('broadcastEmpty');
            return;
        }
        resultEl.style.color = 'var(--text-muted)';
        resultEl.textContent = t('broadcasting');
        try {
            const resp = await fetch(API_BASE + '/tx/broadcast', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ hex: hex })
            });
            const data = await resp.json();
            if (data.success && data.txid) {
                resultEl.style.color = 'var(--green)';
                resultEl.innerHTML = t('broadcastSuccess') + ' TxID: <a href="#" class="hash" onclick="showTx(\'' + data.txid + '\'); return false;">' + shortHash(data.txid) + '</a>';
            } else {
                resultEl.style.color = 'var(--red)';
                resultEl.textContent = t('broadcastError') + ' ' + (data.error || 'Unknown error');
            }
        } catch (e) {
            resultEl.style.color = 'var(--red)';
            resultEl.textContent = t('broadcastError') + ' ' + e.message;
        }
    }

    window.broadcastTx = broadcastTx;

    function showHome() {
        closeModal();
    }

    window.showHome = showHome;

    async function doSearch() {
        const input = document.getElementById('searchInput');
        const query = input.value.trim();
        if (!query) return;

        if (/^\d+$/.test(query)) {
            showBlock(query);
        } else if (query.length === 64 && /^[0-9a-fA-F]+$/.test(query)) {
            try {
                await apiCall('/block/' + query);
                showBlock(query);
            } catch (e) {
                showTx(query);
            }
        } else if (query.startsWith('token1')) {
            showAddress(query);
        } else {
            showTx(query);
        }
    }

    function init() {
        applyTranslations();

        var sel = document.getElementById('langSelector');
        if (sel) {
            sel.value = CURRENT_LANG;
            sel.addEventListener('change', function () {
                setLang(sel.value);
            });
        }

        loadStats();
        loadBlocks();
        loadRecentTxs();

        var searchBtn = document.getElementById('searchBtn');
        if (searchBtn) searchBtn.addEventListener('click', doSearch);
        var searchInput = document.getElementById('searchInput');
        if (searchInput) searchInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') doSearch();
        });
        var refreshBlocksBtn = document.getElementById('refreshBlocks');
        if (refreshBlocksBtn) refreshBlocksBtn.addEventListener('click', function () {
            loadStats();
            loadBlocks();
            loadRecentTxs();
        });
        const refreshTxsBtn = document.getElementById('refreshTxs');
        if (refreshTxsBtn) {
            refreshTxsBtn.addEventListener('click', loadRecentTxs);
        }

        initNavDropdown();

        setInterval(loadStats, 30000);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

    function initNavDropdown() {
        var dropdown = document.querySelector('.nav-dropdown');
        var menu = document.querySelector('.nav-dropdown-menu');
        if (!dropdown || !menu) return;

        function closeDropdown() { dropdown.classList.remove('open'); }

        window.toggleExplorerDropdown = function (e) {
            e.stopPropagation();
            dropdown.classList.toggle('open');
        };

        document.addEventListener('click', function (e) {
            if (!dropdown.contains(e.target)) { closeDropdown(); }
        });
        menu.querySelectorAll('a').forEach(function (a) {
            a.addEventListener('click', function () { closeDropdown(); });
        });
    }

    function initTableLabels() {
        document.querySelectorAll('.data-table').forEach(function (table) {
            var headers = [];
            table.querySelectorAll('thead th').forEach(function (th) {
                headers.push(th.textContent.trim());
            });
            table.querySelectorAll('tbody tr').forEach(function (tr) {
                tr.querySelectorAll('td').forEach(function (td, i) {
                    td.setAttribute('data-label', headers[i] || '');
                });
            });
        });
    }

    window.initTableLabels = initTableLabels;
})();
