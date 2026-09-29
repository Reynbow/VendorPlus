// VendorPlus: Artifact Formation gets a "Vendor" tab in its title bar. It opens a vendor right there, and
// the vendor screen gets an "Artifact Formation" tab back plus arrows to flip through every vendor in the game.
// vendorplus.dll switches the open shop: the game closes one counter and opens the other, as if you had
// walked over. Purchases and stock are the game's own; each vendor keeps its own zone's level (the DLL).
(function () {
    'use strict';
    if (window.__VendorPlusInstalled) return;
    window.__VendorPlusInstalled = true;

    var C = window.__VendorPlusConfig || {};
    var DIAG = !!C.diagnostics;
    var URL = 'coui://base/__vendorplus__.json';
    var LOG_URL = 'coui://base/__vendorplus_log__.json';
    var TICK_MS = 100, STATUS_MS = 250, SWAP_MAX_MS = 3000;
    var SESSION_END_MS = 500;  // the shop closed this long, outside a switch: the visit is over
    var VENDOR = 0, FORMATION = 1;
    // The other screen's name, from the game's own translations.
    var NAMES = [['ui_loc_MENU_SHOP_VENDOR', 'Vendor'], ['ui_loc_MENU_SHOP_FORGEMASTER', 'Artifact Formation']];
    var ICONS = 'coui://base/uiresources/game/symbols/Icon/';
    var HOVER = ['menu-button--hovered', 'selection-item--hovered'];
    var CSS = '.vp-arrow{display:flex;align-items:center;justify-content:center;min-width:3.7037037037vh}' +
        '.breadcrumbs .vp-count.menu-button{pointer-events:none;width:12.962962963vh !important;min-width:12.962962963vh !important;' +
        'max-width:12.962962963vh !important;flex:0 0 auto;display:flex;justify-content:center;box-sizing:border-box}' +
        '.vp-count .breadcrumbs__item__label{font-variant-numeric:tabular-nums;white-space:nowrap;text-align:center}' +
        '.vp-count__n{opacity:0.6}' +
        '.vp-hidden{display:none}' +
        // A vendor you haven't visited yet: its items covered with a message (the game locks them too).
        '.vp-lock-host{position:relative}' +
        '.vp-lock{position:absolute;left:0;top:0;right:0;bottom:0;display:flex;flex-direction:column;align-items:center;' +
        'justify-content:center;background-color:rgba(13,13,13,0.9);pointer-events:auto;z-index:5}' +
        '.vp-lock__title{font-size:3.3333333333vh;font-weight:700;color:rgb(232,232,232);margin-bottom:1.3888888889vh}' +
        '.vp-lock__text{font-size:2.2222222222vh;color:rgba(232,232,232,0.7)}';
    var TEXT = { locked: 'Visit this vendor to unlock it.', vendor: 'Vendor' };

    function model(k, d) { var m = window[k]; return m && m.value !== undefined ? m.value : d; }
    function loc(k, d) {
        var m = window[k], t = m && (m.translation !== undefined ? m.translation : m.value);
        return typeof t === 'string' && t ? t : d;
    }
    function now() { return Date.now(); }
    function connected(n) { for (var i = 0; n && i < 128; i++, n = n.parentNode) if (n === document.body) return true; return false; }
    function el(tag, cls, text) {
        var n = document.createElement(tag);
        if (cls) n.className = cls;
        if (text !== undefined) n.textContent = text;
        return n;
    }
    function remove(n) { if (n && n.parentNode) n.parentNode.removeChild(n); }
    function name(type) { return type === VENDOR ? 'vendor' : type === FORMATION ? 'formation' : 'none'; }

    var logCount = 0;
    function log(msg, force) {
        if (!DIAG && !force) return;
        if (++logCount > 600) return;
        try {
            var x = new XMLHttpRequest();
            x.open('GET', LOG_URL + '?m=' + encodeURIComponent(String(msg).slice(0, 1400)), true);
            x.send();
        } catch (e) { /* logging must never throw */ }
    }

    function getJson(url, done, timeoutMs) {
        var x;
        try { x = new XMLHttpRequest(); } catch (e) { done(null, String(e)); return; }
        var finished = false, timer = setTimeout(function () {
            if (finished) return; finished = true;
            try { x.abort(); } catch (e) { }
            done(null, 'timeout');
        }, timeoutMs || 1500);
        x.onload = function () {
            if (finished) return; finished = true; clearTimeout(timer);
            var r = null;
            try { if (x.status === 200 || x.status === 0) r = JSON.parse(x.responseText); } catch (e) { r = null; }
            done(r, r ? '' : 'bad response');
        };
        x.onerror = function () {
            if (finished) return; finished = true; clearTimeout(timer);
            done(null, 'request failed');
        };
        try { x.open('GET', url, true); x.send(); } catch (e) { if (!finished) { finished = true; clearTimeout(timer); done(null, String(e)); } }
    }

    var nonce = 0, status = null, statusAt = 0, statusBusy = false;
    function act(query, done) {
        getJson(URL + '?a=' + query + '&n=' + (++nonce), function (r, err) {
            if (r) { status = r; statusAt = now(); }
            if (query.indexOf('status') !== 0) log(query + ' -> ' + (r ? JSON.stringify(r) : err));
            if (done) done(r, err);
        });
    }
    function refreshStatus(force) {
        if (statusBusy || (!force && now() - statusAt < STATUS_MS)) return;
        statusBusy = true;
        act('status', function () { statusBusy = false; });
    }

    function ensureStyle() {
        if (document.getElementById('vp-style')) return;
        var st = el('style');
        st.id = 'vp-style';
        st.textContent = CSS;
        (document.head || document.body).appendChild(st);
    }

    // ---------------------------------------------------------------- the two screens
    // Each shop screen is a .fullscreen-layout whose rmd-visible-if tests ui_shop_shop_type (0 vendor,
    // 1 formation). That element stays; the game swaps the wrapper inside it when it hides and shows (the
    // wrapper comes back as a copy, so our title bar items are checked every tick and rebuilt if they lost
    // their handlers).
    var roots = [null, null];
    function findRoots() {
        for (var t = 0; t < 2; t++) if (roots[t] && !connected(roots[t])) roots[t] = null;
        if (roots[0] && roots[1]) return;
        var all = document.querySelectorAll('.fullscreen-layout');
        for (var i = 0; i < all.length; i++) {
            var m = /ui_shop_shop_type\.value\}\}\s*===\s*(\d)/.exec(all[i].getAttribute('data-bind-rmd-visible-if') || '');
            if (m && (m[1] === '0' || m[1] === '1')) roots[+m[1]] = all[i];
        }
    }

    // ---------------------------------------------------------------- the title bar
    // Breadcrumbs like the screen's own title, unselected: "Artifact Formation  Vendor" on both screens (the
    // open one selected), and on the vendor screen "‹ 3 / 10 ›" to flip through the vendors.
    function crumb(cls, onClick) {
        var n = el('div', 'menu-button breadcrumbs__item vp-el ' + cls);
        n.addEventListener('mouseenter', function () { for (var i = 0; i < HOVER.length; i++) n.classList.add(HOVER[i]); });
        n.addEventListener('mouseleave', function () { for (var i = 0; i < HOVER.length; i++) n.classList.remove(HOVER[i]); });
        n.addEventListener('click', onClick);
        n.__vpLive = true;
        return n;
    }
    function tab(type) {
        var other = 1 - type;
        var n = crumb('vp-tab', function () { requestSwap('to=' + other, name(other)); });
        n.appendChild(el('div', 'menu-button__label breadcrumbs__item__label', loc(NAMES[other][0], NAMES[other][1])));
        return n;
    }
    function arrow(dir) {
        var n = crumb('vp-arrow vp-arrow--' + (dir < 0 ? 'prev' : 'next'), function () { stepVendor(dir); });
        var icon = n.appendChild(el('div', 'icon icon--mask breadcrumbs__item__icon'));
        icon.setAttribute('style', 'mask-image: url(' + ICONS + (dir < 0 ? 'ICN_ARROW_LEFT.svg' : 'ICN_ARROW_RIGHT.svg') + ');');
        return n;
    }
    // "1 / 7": the vendor's place in the list (its zone shows in the level heading next to it).
    function counter() {
        var n = el('div', 'menu-button breadcrumbs__item vp-el vp-count');
        n.appendChild(el('div', 'menu-button__label breadcrumbs__item__label vp-count__n', ''));
        n.__vpLive = true;
        return n;
    }

    function vendorIndex(st) {
        var v = (st && st.vendors) || [];
        for (var i = 0; i < v.length; i++) if (v[i] === st.id) return i;
        return -1;
    }

    // want: show our items on this screen; st: the DLL's status.
    function syncBar(type, want, st) {
        var root = roots[type];
        if (!root) return;
        var crumbs = root.querySelector('.breadcrumbs');
        var mine = crumbs ? crumbs.querySelectorAll('.vp-el') : [];
        var vendors = (st && st.vendors) || [];
        var expect = !want ? 0 : type === FORMATION ? 1 : vendors.length > 1 ? 4 : 1;
        var stale = mine.length !== expect;
        for (var i = 0; i < mine.length && !stale; i++) stale = !mine[i].__vpLive;
        if (stale) {
            for (var j = 0; j < mine.length; j++) remove(mine[j]);
            if (!expect || !crumbs) return;
            var title = null, items = crumbs.querySelectorAll('.breadcrumbs__item');
            if (items.length) title = items[0];
            if (type === FORMATION) {
                crumbs.appendChild(tab(FORMATION));
            } else {
                // Formation first, then the vendor: its tab goes before the title.
                if (title) crumbs.insertBefore(tab(VENDOR), title); else crumbs.appendChild(tab(VENDOR));
                if (expect === 4) {
                    crumbs.appendChild(arrow(-1));
                    crumbs.appendChild(counter());
                    crumbs.appendChild(arrow(1));
                }
            }
            log('title bar items added on the ' + name(type) + ' screen');
        }
        var count = crumbs && crumbs.querySelector('.vp-count');
        if (count) {
            var idx = vendorIndex(st), nText = (idx >= 0 ? idx + 1 : '-') + ' / ' + vendors.length;
            var nEl = count.querySelector('.vp-count__n');
            if (nEl && nEl.textContent !== nText) nEl.textContent = nText;
        }
    }

    // ---------------------------------------------------------------- the vendor's own zone
    // A vendor opened here gets its own zone's level (the DLL), so the "Vendor Level" heading beside the level
    // badge shows that zone's name instead. The game's heading is data-bound: it's hidden and a copy with the
    // name shows in its place.
    function syncZone(zone) {
        var root = roots[VENDOR];
        if (!root) return;
        var orig = null, heads = root.querySelectorAll('.vendor-level__text');
        for (var i = 0; i < heads.length; i++) if (!heads[i].classList.contains('vp-zone')) orig = heads[i];
        var copy = root.querySelector('.vp-zone');
        if (!zone || !orig) {
            remove(copy);
            if (orig) orig.classList.remove('vp-hidden');
            return;
        }
        if (!copy || copy.parentNode !== orig.parentNode) {
            remove(copy);
            copy = el('div', 'vendor-level__text vp-zone');
            orig.parentNode.insertBefore(copy, orig);
        }
        if (copy.textContent !== zone) copy.textContent = zone;
        orig.classList.add('vp-hidden');
    }

    // While the map is open it marks the district you're in (has_player); the DLL knows that district's id.
    // Together they tell the DLL which id each zone has. Sent once per zone per session, when it has settled.
    var DISTRICTS = 7;  // the map's district slots (ui_map_district_images_N_*)
    var mapSent = {}, mapLast = '', mapSince = 0;
    function learnFromMap(st) {
        if (!model('ui_stacks_game_states_map_active', false) || !st || !st.here) { mapLast = ''; return; }
        var zone = '';
        for (var i = 0; i < DISTRICTS; i++)
            if (model('ui_map_district_images_' + i + '_has_player', false) === true) zone = loc('ui_map_district_images_' + i + '_name', '');
        var key = zone + '|' + st.here;
        if (!zone) { mapLast = ''; return; }
        if (key !== mapLast) { mapLast = key; mapSince = now(); return; }
        if (now() - mapSince < 1000 || mapSent[key]) return;
        mapSent[key] = true;
        log('map: you are in "' + zone + '" (district ' + st.here + ')');
        act('here&zone=' + encodeURIComponent(zone));
    }

    // A vendor you haven't opened at its counter yet: the DLL gives it level 0 (the game locks every item), and
    // its items are covered with "<Zone> Vendor / Visit this vendor to unlock it."
    function syncLock(want, zone) {
        var root = roots[VENDOR];
        if (!root) return;
        var layout = root.querySelector('.shop-layout');
        var lock = root.querySelector('.vp-lock');
        if (lock && (!want || !lock.__vpLive || lock.parentNode !== layout)) { remove(lock); lock = null; }
        if (!want || !layout) {
            if (layout) layout.classList.remove('vp-lock-host');
            return;
        }
        var title = zone ? zone + ' ' + loc(NAMES[VENDOR][0], NAMES[VENDOR][1]) : loc(NAMES[VENDOR][0], NAMES[VENDOR][1]);
        if (!lock) {
            lock = el('div', 'vp-lock');
            lock.appendChild(el('div', 'vp-lock__title', title));
            lock.appendChild(el('div', 'vp-lock__text', TEXT.locked));
            lock.__vpLive = true;
            layout.classList.add('vp-lock-host');
            layout.appendChild(lock);
            log('locked vendor: ' + title);
        }
        var t = lock.querySelector('.vp-lock__title');
        if (t && t.textContent !== title) t.textContent = title;
    }

    // ---------------------------------------------------------------- switching
    var busy = false, busyAt = 0, serial0 = -1;
    function requestSwap(query, what) {
        if (busy) return;
        busy = true;
        busyAt = now();
        serial0 = -1;
        log('switch to ' + what);
        act('swap&' + query, function (res) {
            if (res && res.accepted) { serial0 = res.serial; return; }
            log('switch refused: ' + (res ? JSON.stringify(res) : 'no answer'), true);
            busy = false;
        });
    }
    function stepVendor(dir) {
        var st = status, v = (st && st.vendors) || [], i = vendorIndex(st);
        if (!v.length) return;
        // Not in the list (a vendor opened some other way): next is the first, previous the last.
        var next = i < 0 ? v[dir > 0 ? 0 : v.length - 1] : v[(i + dir + v.length) % v.length];
        if (next === st.id) return;
        requestSwap('id=' + next, 'vendor ' + next);
    }

    // ---------------------------------------------------------------- the visit
    // Our title bar items show from the moment Artifact Formation opens until you leave the shops; a vendor
    // opened anywhere else stays as the game made it.
    var visit = false, closedAt = 0, lastInPerson = 0;

    function tick() {
        findRoots();
        var open = !!model('ui_shop_is_open', false);
        var type = model('ui_shop_shop_type', -1);
        // Ask again at once when the screen changed since the last answer, so the items show as it fades in.
        if (open || busy || visit || model('ui_stacks_game_states_map_active', false)) refreshStatus(!!status && (status.open !== open || (open && status.type !== type)));
        var st = status && now() - statusAt < 2000 ? status : null;

        if (busy) {
            if (st && serial0 >= 0 && st.serial !== serial0 && !st.busy) {
                if (st.result !== 'done') log('switch failed: ' + st.result, true);
                busy = false;
            } else if (now() - busyAt > SWAP_MAX_MS) {
                log('switch timed out in the script', true);
                busy = false;
            }
        }

        var paired = !!(open && st && st.installed && st.paired && st.open && st.type === type);
        if (!visit && paired && type === FORMATION) { visit = true; log('visit starts at the formation'); }
        if (visit) {
            if (open) closedAt = 0;
            else if (!busy) {
                if (!closedAt) closedAt = now();
                else if (now() - closedAt > SESSION_END_MS) { visit = false; closedAt = 0; log('visit over'); }
            }
        }
        // Diagnostics: a vendor the player opened in person (not through us), with the map's zone and the level,
        // to build the vendor -> zone table.
        if (DIAG && !visit && open && type === VENDOR && st && st.open && st.type === VENDOR && st.id !== lastInPerson) {
            lastInPerson = st.id;
            log('vendor opened in person: id ' + st.id + ', vendor level ' + model('ui_shop_level', '?'));
        }
        if (!open) lastInPerson = 0;
        learnFromMap(st);
        var show = visit && paired;
        syncBar(VENDOR, show && type === VENDOR, st);
        syncBar(FORMATION, show && type === FORMATION, st);
        var idx = st ? vendorIndex(st) : -1;
        var zoneName = idx >= 0 && st && st.zones ? st.zones[idx] || '' : '';
        var locked = !!(show && type === VENDOR && st.locked);
        syncZone(show && type === VENDOR && !locked && st.homeLevel ? zoneName : '');
        syncLock(locked, zoneName);
    }

    function init() {
        ensureStyle();
        log('VendorPlus ' + C.version + ' script ready (hook ' + C.hook + ')', true);
        setInterval(function () {
            try { tick(); } catch (e) { log('tick error: ' + (e && e.stack || e), true); }
        }, TICK_MS);
    }

    if (document.body) init();
    else document.addEventListener('DOMContentLoaded', init);
})();
