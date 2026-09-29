// VendorPlus: Artifact Formation gets a tab row like the game menu's (Artifact Formation, Vendor, Wardrobe; LB / RB
// or a click to switch), and the vendor screen gets arrows (and the secondary pair of buttons) to flip through
// every vendor in the game. vendorplus.dll switches the open screen: the game closes one counter and opens the
// other, as if you had walked over; the wardrobe is the game's own wardrobe screen. Purchases and stock are the
// game's own; each vendor keeps its own zone's level (the DLL).
(function () {
    'use strict';
    if (window.__VendorPlusInstalled) return;
    window.__VendorPlusInstalled = true;

    var C = window.__VendorPlusConfig || {};
    var DIAG = !!C.diagnostics;
    var URL = 'coui://base/__vendorplus__.json';
    var LOG_URL = 'coui://base/__vendorplus_log__.json';
    var TICK_MS = 100, STATUS_MS = 250, STATUS_VISIT_MS = 100, SWAP_MAX_MS = 3000;
    var SESSION_END_MS = 500;  // no screen of ours open this long, outside a switch: the visit is over
    var VENDOR = 0, FORMATION = 1, WARDROBE = 2;  // VENDOR and FORMATION are the game's shop types
    var TABS = [FORMATION, VENDOR, WARDROBE];      // the tab row, left to right
    // The screens' names, from the game's own translations.
    var NAMES = [['ui_loc_MENU_SHOP_VENDOR', 'Vendor'], ['ui_loc_MENU_SHOP_FORGEMASTER', 'Artifact Formation'],
        ['', 'Wardrobe']];  // the game calls its screen "Skins"; the tab says what it is
    // The menu actions the game's own tab rows use (LB / RB on a pad), and the pair the vendor arrows use.
    var KEY_PREV = 'MENU_PREV', KEY_NEXT = 'MENU_NEXT', KEY_PREV2 = 'MENU_PREV_SECONDARY', KEY_NEXT2 = 'MENU_NEXT_SECONDARY';
    // The vendor's own amount keys ("[Q] x1 [E]", LT / RT on a pad): some of those same keys.
    var AMOUNT_DOWN = 'MENU_SHOP_DECREASE_QUANTITY', AMOUNT_UP = 'MENU_SHOP_INCREASE_QUANTITY';
    var STICK = 'MENU_SCROLL_RIGHT_STICK_X';  // the right stick, sideways: the vendor arrows on a pad
    var ICONS = 'coui://base/uiresources/game/symbols/Icon/';
    var HOVER = ['menu-button--hovered', 'selection-item--hovered'];
    var CSS = '.vp-arrow{display:flex;align-items:center;justify-content:center;min-width:3.7037037037vh}' +
        '.breadcrumbs .vp-count.menu-button{pointer-events:none;width:12.962962963vh !important;min-width:12.962962963vh !important;' +
        'max-width:12.962962963vh !important;flex:0 0 auto;display:flex;justify-content:center;box-sizing:border-box}' +
        '.vp-count .breadcrumbs__item__label{font-variant-numeric:tabular-nums;white-space:nowrap;text-align:center}' +
        '.vp-count__n{opacity:0.6}' +
        '.breadcrumbs .vp-vkey{align-self:center;cursor:pointer}' +
        // The tab row: as tall as its buttons (a menu-header-layout shares the free height with flex:1, which
        // would squash the screen's own header), its layer in the flow instead of absolute.
        '.menu-header-layout.vp-tabs{flex:0 0 auto;margin-bottom:1.8518518519vh}' +
        '.vp-tabs .menu-header-layout__main{position:relative;height:auto}' +
        '.vp-tabs .vp-key{cursor:pointer}' +
        // The screen's close button ("x B") moves to the front of the tab row.
        '.vp-tabs .breadcrumbs__start{margin-right:1.8518518519vh}' +
        // The wardrobe's own title bar (an item's slot, "Hair") follows the tabs in the row.
        '.vp-tabs .breadcrumbs{margin-left:3.7037037037vh}' +
        '.vp-hidden{display:none !important}' +
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
    function setClass(n, cls, on) {
        if (!n || n.classList.contains(cls) === !!on) return;
        if (on) n.classList.add(cls); else n.classList.remove(cls);
    }
    function name(type) { return type === VENDOR ? 'vendor' : type === FORMATION ? 'formation' : type === WARDROBE ? 'wardrobe' : 'none'; }

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
    function refreshStatus(force, every) {
        if (statusBusy || (!force && now() - statusAt < (every || STATUS_MS))) return;
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

    // ---------------------------------------------------------------- the three screens
    // Each shop screen is a .fullscreen-layout whose rmd-visible-if tests ui_shop_shop_type (0 vendor,
    // 1 formation); the wardrobe's tests hud_player_outfit_open. That element stays; the game swaps the wrapper
    // inside it when it hides and shows (the wrapper comes back as a copy, so our items are checked every tick
    // and rebuilt if they lost their handlers).
    var roots = [null, null, null];
    function findRoots() {
        for (var t = 0; t < 3; t++) if (roots[t] && !connected(roots[t])) roots[t] = null;
        if (roots[0] && roots[1] && roots[2]) return;
        var all = document.querySelectorAll('.fullscreen-layout');
        for (var i = 0; i < all.length; i++) {
            var vis = all[i].getAttribute('data-bind-rmd-visible-if') || '';
            var m = /ui_shop_shop_type\.value\}\}\s*===\s*(\d)/.exec(vis);
            if (m && (m[1] === '0' || m[1] === '1')) roots[+m[1]] = all[i];
            else if (/^\s*\{\{hud_player_outfit_open\.value\}\}\s*$/.test(vis)) roots[WARDROBE] = all[i];
        }
    }

    function hover(n) {
        n.addEventListener('mouseenter', function () { for (var i = 0; i < HOVER.length; i++) n.classList.add(HOVER[i]); });
        n.addEventListener('mouseleave', function () { for (var i = 0; i < HOVER.length; i++) n.classList.remove(HOVER[i]); });
    }

    // A button prompt drawn the game's way: the action's icon (the pad button, or the key's picture), or the
    // key's name as text for keys that have no picture. The game keeps ui_action_keys_<ACTION>_* up to date.
    function actionKey(cls, onClick) {
        var n = el('div', 'action-key action-key--visible vp-key ' + cls);
        n.appendChild(el('div', 'action-key__icon'));
        n.appendChild(el('span', 'action-key__text', ''));
        if (onClick) n.addEventListener('click', onClick);
        return n;
    }
    // off: the key does something else here (the game's own "unavailable" look).
    function syncKey(n, action, off) {
        if (!n) return;
        var k = 'ui_action_keys_' + action + '_';
        var path = model(k + 'path', '') || '', asText = !!model(k + 'text_show', false);
        var text = String(model(k + 'text_value', '') || ''), wide = !!model(k + 'text_wide', false);
        var icon = n.firstChild, label = n.lastChild;
        var bg = path ? 'url("' + path + '")' : '';
        if (icon.__vpBg !== bg) { icon.__vpBg = bg; icon.style.backgroundImage = bg; }
        setClass(icon, 'action-key__icon--hide', asText);
        setClass(label, 'action-key__text--show', asText);
        if (label.textContent !== text) label.textContent = text;
        setClass(n, 'action-key--KB-wide', asText && wide);
        setClass(n, 'action-key--disabled', !!off);
    }
    // The right stick's prompt: the game's own for that axis, or its right stick icon for the pad in use (named like
    // the pad's other prompts: XB_... or PS_...).
    function syncStick(n) {
        if (!n) return;
        var path = model('ui_action_keys_' + STICK + '_path', '') || '';
        if (!path) {
            var other = String(model('ui_action_keys_' + KEY_PREV + '_path', '') || '');
            var m = /^(.*\/)(XB|PS)_[^\/]*$/.exec(other);
            path = (m ? m[1] + m[2] : 'coui://base/uiresources/game/symbols/Controls/XB') + '_RIGHT_STICK_X.svg';
        }
        var icon = n.firstChild, label = n.lastChild, bg = 'url("' + path + '")';
        if (icon.__vpBg !== bg) { icon.__vpBg = bg; icon.style.backgroundImage = bg; }
        setClass(icon, 'action-key__icon--hide', false);
        setClass(label, 'action-key__text--show', false);
        if (label.textContent !== '') label.textContent = '';
        setClass(n, 'action-key--KB-wide', false);
        setClass(n, 'action-key--disabled', false);
    }
    function padInUse() { var d = model('ui_input_active_controller_device', -1); return d === 0 || d === -1; }
    // An action on the same key as one of the vendor's amount keys, on the device in use (Q / E, or LT / RT).
    function keyOf(action) {
        var k = 'ui_action_keys_' + action + '_';
        return String(model(k + 'path', '') || '') + '|' + String(model(k + 'text_value', '') || '');
    }
    function amountKey(action) {
        var key = keyOf(action);
        return key !== '|' && (key === keyOf(AMOUNT_DOWN) || key === keyOf(AMOUNT_UP));
    }
    // The keys as the game shows them on the vendor screen, logged when they change (which ones are shared).
    var keysLogged = '';
    function logKeys(st) {
        function k(a) { return keyOf(a).replace(/^[^|]*\//, ''); }
        var line = 'keys (device ' + model('ui_input_active_controller_device', -1) + '): tabs ' + k(KEY_PREV) + ' ' +
            k(KEY_NEXT) + ', arrows ' + k(KEY_PREV2) + ' ' + k(KEY_NEXT2) + ', amount ' + k(AMOUNT_DOWN) + ' ' +
            k(AMOUNT_UP) + ', stick ' + (String(model('ui_action_keys_' + STICK + '_path', '') || '-').replace(/^.*\//, '')) +
            (st && st.stickOk ? '' : ' (no stick reader)');
        if (line !== keysLogged) { keysLogged = line; log(line, true); }
    }

    // ---------------------------------------------------------------- the tab row
    // The game menu's own tab row (Map, Loadout, Inventory, ...): "[LB] Artifact Formation  Vendor  Wardrobe [RB]",
    // above the screen's header, the open screen selected.
    function tabRow(st) {
        var host = el('div', 'menu-header-layout vp-tabs');
        var main = host.appendChild(el('div', 'menu-header-layout__main menu-header-layout__main--vertical'));
        var row = main.appendChild(el('div', 'tab-buttons'));
        row.appendChild(actionKey('tab-buttons__element tab-buttons__key tab-buttons__key--left vp-key--prev',
            function () { stepTab(-1); }));
        for (var i = 0; i < TABS.length; i++) {
            (function (screen) {
                var b = el('div', 'menu-button tab-buttons-button tab-buttons__element vp-tab');
                b.appendChild(el('div', 'menu-button__label tab-buttons-button__label', loc(NAMES[screen][0], NAMES[screen][1])));
                b.__vpScreen = screen;
                hover(b);
                b.addEventListener('click', function () { goTo(screen); });
                row.appendChild(b);
            })(TABS[i]);
        }
        row.appendChild(actionKey('tab-buttons__element tab-buttons__key tab-buttons__key--right vp-key--next',
            function () { stepTab(1); }));
        host.__vpLive = true;
        return host;
    }
    function availableTabs(st) {
        var out = [];
        for (var i = 0; i < TABS.length; i++) if (TABS[i] !== WARDROBE || (st && st.canWardrobe)) out.push(TABS[i]);
        return out;
    }
    function syncTabs(screen, want, st) {
        var root = roots[screen];
        if (!root) return;
        var host = root.querySelector('.vp-tabs');
        var content = root.querySelector('.fullscreen-layout__content');
        var header = null;
        if (content) for (var c = content.firstChild; c; c = c.nextSibling)
            if (c.classList && c.classList.contains('menu-header-layout') && !c.classList.contains('vp-tabs')) { header = c; break; }
        var main = header && header.querySelector('.menu-header-layout__main');
        var crumbs = (header && header.querySelector('.breadcrumbs')) || (host && host.querySelector('.breadcrumbs'));
        if (host && (!want || !host.__vpLive || host.parentNode !== content)) {
            // What moved into the row goes home first (also out of a stale copy of the row, into that copy's
            // header): the wardrobe's title bar back to the front of its header, then the close button into it.
            var moved = host.querySelector('.breadcrumbs');
            if (moved && main) {
                main.insertBefore(moved, main.firstChild);
                var mt = moved.querySelector('.breadcrumbs__item');
                if (mt) setClass(mt, 'vp-hidden', false);
            }
            var back = host.querySelector('.breadcrumbs__start');
            if (back && crumbs) crumbs.insertBefore(back, crumbs.firstChild);
            remove(host);
            host = null;
        }
        // The formation's title bar would only repeat its tab once the close button has moved: hidden, so
        // "Sort by" keeps its place.
        setClass(crumbs, 'vp-hidden', !!(want && screen === FORMATION && content && header));
        if (!want || !content || !header) return;
        if (!host) {
            host = tabRow(st);
            content.insertBefore(host, header);
            log('tab row added on the ' + name(screen) + ' screen');
        }
        var row = host.querySelector('.tab-buttons'), start = crumbs && crumbs.querySelector('.breadcrumbs__start');
        if (start && row) row.insertBefore(start, row.firstChild);
        // The wardrobe's title bar joins the row after the tabs (its header has no room under the row); its own
        // title ("Skins") is hidden: the tab says it. An item's slot ("Hair") shows there while you browse.
        if (screen === WARDROBE && crumbs && row) {
            if (crumbs.parentNode !== row) row.appendChild(crumbs);
            var title = crumbs.querySelector('.breadcrumbs__item');
            if (title) setClass(title, 'vp-hidden', true);
        }
        var tabs = availableTabs(st), buttons = host.querySelectorAll('.vp-tab');
        for (var i = 0; i < buttons.length; i++) {
            var b = buttons[i], on = b.__vpScreen === screen;
            setClass(b, 'vp-hidden', tabs.indexOf(b.__vpScreen) < 0);
            setClass(b, 'menu-button--selected', on);
            setClass(b, 'selection-item--selected', on);
        }
        // On the vendor screen, keys that are its amount keys too (Q / E) change the amount: shown unavailable.
        var off = screen === VENDOR && (amountKey(KEY_PREV) || amountKey(KEY_NEXT));
        syncKey(host.querySelector('.vp-key--prev'), KEY_PREV, off);
        syncKey(host.querySelector('.vp-key--next'), KEY_NEXT, off);
    }

    // ---------------------------------------------------------------- the vendor switcher
    // On the vendor screen's title bar: "[LT] ‹ 3 / 7 › [RT]" to flip through the vendors.
    function crumb(cls, onClick) {
        var n = el('div', 'menu-button breadcrumbs__item vp-el ' + cls);
        hover(n);
        n.addEventListener('click', onClick);
        n.__vpLive = true;
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

    // want: show the switcher on the vendor screen; st: the DLL's status.
    function syncBar(want, st) {
        var root = roots[VENDOR];
        if (!root) return;
        var crumbs = root.querySelector('.breadcrumbs');
        var mine = crumbs ? crumbs.querySelectorAll('.vp-el') : [];
        // The game's own "Vendor" title: the tab row says it, so it's hidden while the row shows.
        var items = crumbs ? crumbs.querySelectorAll('.breadcrumbs__item') : [];
        for (var t = 0; t < items.length; t++) if (!items[t].classList.contains('vp-el')) { setClass(items[t], 'vp-hidden', !!want); break; }
        var vendors = (st && st.vendors) || [];
        var expect = want && vendors.length > 1 ? 5 : 0;
        var stale = mine.length !== expect;
        for (var i = 0; i < mine.length && !stale; i++) stale = !mine[i].__vpLive;
        if (stale) {
            for (var j = 0; j < mine.length; j++) remove(mine[j]);
            if (!expect || !crumbs) return;
            var prevKey = actionKey('vp-el vp-vkey vp-vkey--prev', function () { stepVendor(-1); });
            var nextKey = actionKey('vp-el vp-vkey vp-vkey--next', function () { stepVendor(1); });
            prevKey.__vpLive = nextKey.__vpLive = true;
            crumbs.appendChild(prevKey);
            crumbs.appendChild(arrow(-1));
            crumbs.appendChild(counter());
            crumbs.appendChild(arrow(1));
            crumbs.appendChild(nextKey);
            log('vendor switcher added');
        }
        // The arrows' keys; when they're the vendor's amount keys too (LT / RT on a pad), the right stick instead.
        var prevKey = crumbs && crumbs.querySelector('.vp-vkey--prev'), nextKey = crumbs && crumbs.querySelector('.vp-vkey--next');
        var shared = amountKey(KEY_PREV2) || amountKey(KEY_NEXT2);
        var stick = shared && !!(st && st.stickOk) && padInUse();
        if (stick) syncStick(prevKey); else syncKey(prevKey, KEY_PREV2, shared);
        syncKey(nextKey, KEY_NEXT2, shared);
        setClass(nextKey, 'vp-hidden', stick);
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
    var screenNow = -1;  // the screen of ours that's open (VENDOR, FORMATION, WARDROBE), -1 = none
    function goTo(screen) {
        if (screen === screenNow) return;
        requestSwap('to=' + screen, name(screen));
    }
    // LB / RB: the tab to the left / right, round the ends as in the game menu.
    function stepTab(dir) {
        var tabs = availableTabs(status), i = tabs.indexOf(screenNow);
        if (i < 0 || tabs.length < 2) return;
        goTo(tabs[(i + dir + tabs.length) % tabs.length]);
    }

    // The DLL counts presses of the menu actions (LB / RB and the secondary pair); act on the ones since the
    // last status while our tabs show. The wardrobe uses LB / RB itself for an item's materials, so there they
    // only switch tabs from its top level. Presses that were the vendor's amount keys too (Q / E, LT / RT) come
    // apart (padAmt): on the vendor screen they're its amount, elsewhere they switch as usual. There a flick of the
    // right stick (stick: right, left) flips the vendors on a pad.
    var padSeen = null, amtSeen = null, stickSeen = null;
    function counts(a, n) {
        if (!a || a.length !== n) { a = []; for (var i = 0; i < n; i++) a.push(0); }
        return a.slice();
    }
    function handlePad(st, show) {
        var p = st && st.pad;
        if (!p || p.length < 4) return;
        var a = counts(st.padAmt, 4), s = counts(st.stick, 2);
        var seen = padSeen, seenA = amtSeen, seenS = stickSeen;
        padSeen = p.slice();
        amtSeen = a;
        stickSeen = s;
        if (!seen || !show || busy) return;
        var vendor = screenNow === VENDOR, d = [];
        for (var i = 0; i < 4; i++) d.push(p[i] !== seen[i] || (!vendor && a[i] !== seenA[i]));
        if (screenNow === WARDROBE && model('ui_stacks_game_states_player_outfit_browser_current', false)) return;
        if (d[0] !== d[1]) { stepTab(d[0] ? 1 : -1); return; }
        if (!vendor) return;
        if (d[2] !== d[3]) { stepVendor(d[2] ? 1 : -1); return; }
        var right = s[0] !== seenS[0], left = s[1] !== seenS[1];
        if (right !== left && padInUse()) stepVendor(right ? 1 : -1);
    }

    // The wardrobe's slot whose items are open (its place in the grid: 0 Top Layer, 1 Hair, 2 Base Layer, 3 Facial
    // Hair, 4 Accessories, 5 Body and Face), -1 = none: the DLL gives face slots the wardrobe's face camera.
    var SLOTS = 6, slotSent = null;
    function syncSlot(wardrobe) {
        var n = -1;
        if (wardrobe && model('ui_stacks_game_states_player_outfit_browser_current', false)) {
            var cur = model('ui_menu_navigation_player_outfit_current', -1);
            for (var i = 0; i < SLOTS; i++) if (model('hud_player_outfit_slots_' + i + '_id', -2) === cur) { n = i; break; }
        }
        if (n === slotSent) return;
        slotSent = n;
        act('slot&slot=' + n);
    }

    // ---------------------------------------------------------------- the visit
    // Our items show from the moment Artifact Formation opens until you leave its screens (the formation, the
    // vendors and the wardrobe); a vendor or wardrobe opened anywhere else stays as the game made it.
    var visit = false, closedAt = 0, lastInPerson = 0;

    function tick() {
        findRoots();
        var open = !!model('ui_shop_is_open', false);
        var type = model('ui_shop_shop_type', -1);
        var wardrobe = !!model('hud_player_outfit_open', false);
        // Ask again at once when the screen changed since the last answer, so the items show as it fades in.
        if (open || wardrobe || busy || visit || model('ui_stacks_game_states_map_active', false))
            refreshStatus(!!status && (status.open !== open || (open && status.type !== type)), visit ? STATUS_VISIT_MS : STATUS_MS);
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
            if (open || wardrobe) closedAt = 0;
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
        screenNow = wardrobe ? WARDROBE : paired ? type : -1;
        var show = !!(visit && st && st.installed && screenNow >= 0);
        for (var s = 0; s < 3; s++) syncTabs(s, show && screenNow === s, st);
        syncBar(show && screenNow === VENDOR, st);
        if (show && screenNow === VENDOR) logKeys(st);
        handlePad(st, show);
        syncSlot(wardrobe);
        var idx = st ? vendorIndex(st) : -1;
        var zoneName = idx >= 0 && st && st.zones ? st.zones[idx] || '' : '';
        var locked = !!(show && screenNow === VENDOR && st.locked);
        syncZone(show && screenNow === VENDOR && !locked && st.homeLevel ? zoneName : '');
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
