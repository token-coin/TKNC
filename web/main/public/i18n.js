(function () {
    // Merge all language dictionaries
    var I18N_DICT = {};
    ['zh', 'en', 'ja', 'ko', 'fr', 'de', 'es', 'pt', 'ru', 'ar', 'hi', 'th', 'vi'].forEach(function (code) {
        var holder = window['LANG_' + code];
        if (holder && holder[code]) I18N_DICT[code] = holder[code];
    });

    var DEFAULT_LANG = 'en';
    var CURRENT_LANG = localStorage.getItem('tknc_wp_lang');
    
    // Migrate: if stored lang is not in available dict, reset to default
    if (!CURRENT_LANG || !I18N_DICT[CURRENT_LANG]) {
        CURRENT_LANG = DEFAULT_LANG;
        localStorage.setItem('tknc_wp_lang', DEFAULT_LANG);
    }

    function t(key) {
        if (I18N_DICT[CURRENT_LANG] && I18N_DICT[CURRENT_LANG].hasOwnProperty(key)) {
            return I18N_DICT[CURRENT_LANG][key];
        }
        if (I18N_DICT['en'] && I18N_DICT['en'].hasOwnProperty(key)) {
            return I18N_DICT['en'][key];
        }
        if (I18N_DICT['zh'] && I18N_DICT['zh'].hasOwnProperty(key)) {
            return I18N_DICT['zh'][key];
        }
        return key;
    }

    function applyLang(lang) {
        CURRENT_LANG = lang;
        localStorage.setItem('tknc_wp_lang', lang);
        document.documentElement.lang = lang;
        document.documentElement.dir = 'ltr';

        document.querySelectorAll('[data-i18n]').forEach(function (el) {
            var key = el.getAttribute('data-i18n');
            var val = t(key);
            if (val !== undefined && val !== null) el.textContent = val;
        });

        document.querySelectorAll('[data-i18n-html]').forEach(function (el) {
            var key = el.getAttribute('data-i18n-html');
            var val = t(key);
            if (val) el.innerHTML = val;
        });

        var sel = document.getElementById('langSelector');
        if (sel) sel.value = lang;

        // refresh dual-engine panel content
        renderEngine(currentEngine);

        // refresh compare table labels for mobile stacked layout
        initCompareTableLabels();
    }

    window.t = t;
    window.applyLang = applyLang;

    // ===== Dual-engine switcher =====
    var currentEngine = '1';
    var engineData = {
        '1': {
            titleKey: 'dualEng1Title',
            tagKey: 'dualEng1Tag',
            bulletKeys: ['dualEng1_b1', 'dualEng1_b2', 'dualEng1_b3', 'dualEng1_b4']
        },
        '2': {
            titleKey: 'dualEng2Title',
            tagKey: 'dualEng2Tag',
            bulletKeys: ['dualEng2_b1', 'dualEng2_b2', 'dualEng2_b3']
        }
    };

    function renderEngine(num) {
        currentEngine = num;
        var data = engineData[num];
        if (!data) return;

        var titleEl = document.getElementById('engineTitle');
        var tagEl = document.getElementById('engineTag');
        var listEl = document.getElementById('engineList');
        if (titleEl) titleEl.textContent = t(data.titleKey);
        if (tagEl) tagEl.textContent = t(data.tagKey);
        if (listEl) {
            listEl.innerHTML = '';
            data.bulletKeys.forEach(function (k) {
                var li = document.createElement('li');
                li.textContent = t(k);
                listEl.appendChild(li);
            });
        }

        document.querySelectorAll('.engine-btn').forEach(function (b) {
            b.classList.toggle('active', b.getAttribute('data-engine') === num);
        });
        var sw = document.querySelector('.dual-switch');
        if (sw) sw.setAttribute('data-active', num);

        // panel animation
        var panel = document.getElementById('enginePanel');
        if (panel) {
            panel.style.opacity = '0';
            panel.style.transform = 'translateY(10px)';
            requestAnimationFrame(function () {
                panel.style.transition = 'all .4s';
                panel.style.opacity = '1';
                panel.style.transform = 'none';
            });
        }
    }

    window.renderEngine = renderEngine;

    // ===== Scroll reveal =====
    function initReveal() {
        var els = document.querySelectorAll('.reveal');
        if (!('IntersectionObserver' in window)) {
            els.forEach(function (e) { e.classList.add('visible'); });
            return;
        }
        var io = new IntersectionObserver(function (entries) {
            entries.forEach(function (entry) {
                if (entry.isIntersecting) {
                    entry.target.classList.add('visible');
                    io.unobserve(entry.target);
                }
            });
        }, { threshold: 0.12, rootMargin: '0px 0px -40px 0px' });
        els.forEach(function (e) { io.observe(e); });
    }

    // ===== Tilt on pointer move =====
    function initTilt() {
        document.querySelectorAll('.tilt').forEach(function (card) {
            card.addEventListener('mousemove', function (e) {
                var r = card.getBoundingClientRect();
                var x = (e.clientX - r.left) / r.width - 0.5;
                var y = (e.clientY - r.top) / r.height - 0.5;
                card.style.transform = 'perspective(900px) rotateX(' + (-y * 6) + 'deg) rotateY(' + (x * 6) + 'deg) translateY(-4px)';
            });
            card.addEventListener('mouseleave', function () {
                card.style.transform = '';
            });
        });
    }

    // ===== Init =====
    function init() {
        var sel = document.getElementById('langSelector');
        if (sel) {
            sel.value = CURRENT_LANG;
            sel.addEventListener('change', function () { applyLang(sel.value); });
        }
        applyLang(CURRENT_LANG);

        document.querySelectorAll('.engine-btn').forEach(function (b) {
            b.addEventListener('click', function () { renderEngine(b.getAttribute('data-engine')); });
        });

        initReveal();
        initTilt();
        initNavDropdown();
        initCompareTableLabels();
        initIdeTabs();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }

    function initIdeTabs() {
        var tabs = document.querySelectorAll('.ide-tab');
        var panels = document.querySelectorAll('.ide-panel');
        if (!tabs.length || !panels.length) return;
        tabs.forEach(function (tab) {
            tab.addEventListener('click', function () {
                var target = tab.getAttribute('data-ide-tab');
                tabs.forEach(function (t) { t.classList.toggle('active', t === tab); });
                panels.forEach(function (p) {
                    var pid = p.getAttribute('data-ide-panel');
                    if (pid === target) {
                        p.style.display = 'flex';
                        p.style.opacity = '0';
                        p.style.transform = 'translateY(8px)';
                        requestAnimationFrame(function () {
                            p.style.transition = 'all .3s';
                            p.style.opacity = '1';
                            p.style.transform = 'none';
                        });
                    } else {
                        p.style.display = 'none';
                    }
                });
            });
        });
    }

    function initCompareTableLabels() {
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

    function initNavDropdown() {
        var dropdown = document.querySelector('.nav-dropdown');
        var menu = document.querySelector('.nav-dropdown-menu');
        if (!dropdown || !menu) return;

        function closeDropdown() {
            dropdown.classList.remove('open');
        }

        window.toggleNavDropdown = function (e) {
            e.stopPropagation();
            dropdown.classList.toggle('open');
        };

        document.addEventListener('click', function (e) {
            if (!dropdown.contains(e.target)) {
                closeDropdown();
            }
        });

        menu.querySelectorAll('a').forEach(function (a) {
            a.addEventListener('click', function () {
                closeDropdown();
            });
        });
    }
})();
