/**
 * GStreamer WASM Dots Viewer
 *
 * Auto-injects a floating panel for viewing pipeline DOT graphs when
 * GStreamer runs in a WASM environment with GST_TRACERS=dots.
 *
 * Registers globalThis.__gstDotsWasm which receives DOT data from the
 * pipeline-snapshot tracer via Emscripten FFI.
 *
 * Uses SvgOverlayManager (from the dots-viewer) for interactive SVG
 * rendering with node highlighting, zoom/pan, tooltips, and text
 * ellipsizing. All dependencies are bundled — no CDN loads.
 */

(function () {
    'use strict';

    let panelElement = null;
    let pipelinesDiv = null;
    let toggleButton = null;
    let searchInput = null;
    let dotCount = 0;
    let pendingObservers = [];
    let observerDebounceTimer = null;
    const OBSERVER_DEBOUNCE_MS = 300;
    let gvInstance = null;
    let gvLoading = null;

    /**
     * Returns a Graphviz WASM instance (from @hpcc-js/wasm, bundled).
     */
    function getGraphviz() {
        if (gvInstance) return Promise.resolve(gvInstance);
        if (gvLoading) return gvLoading;
        gvLoading = Graphviz.load().then(gv => { gvInstance = gv; return gv; });
        return gvLoading;
    }

    /**
     * Renders DOT content to an SVG DOM element using the bundled
     * @hpcc-js/wasm Graphviz instance.
     */
    function renderDotToSvg(dotContent) {
        return getGraphviz().then(gv => {
            const svgString = gv.layout(dotContent, 'svg', 'dot');
            const parser = new DOMParser();
            const doc = parser.parseFromString(svgString, 'image/svg+xml');
            return doc.documentElement;
        });
    }

    function injectStyles() {
        if (document.getElementById('gst-dots-wasm-styles')) return;

        const style = document.createElement('style');
        style.id = 'gst-dots-wasm-styles';
        style.textContent = `
            /* ── Reset for WASM dots panel ─────────────────────────────────── */
            :root {
                --gst-dots-bg: #1a1a1a;
                --gst-dots-surface: #222;
                --gst-dots-border: #3a3a3a;
                --gst-dots-text: #e0e0e0;
                --gst-dots-text-dim: #888;
                --gst-dots-accent: #4a9eff;
            }

            #gst-dots-toggle {
                position: fixed;
                bottom: 16px;
                right: 16px;
                z-index: 99998;
                width: 48px;
                height: 48px;
                border-radius: 50%;
                background: var(--gst-dots-accent);
                color: #fff;
                border: none;
                cursor: pointer;
                font-size: 20px;
                display: flex;
                align-items: center;
                justify-content: center;
                box-shadow: 0 2px 8px rgba(0,0,0,.4);
                transition: background .15s;
            }
            #gst-dots-toggle:hover { background: #3888e8; }
            #gst-dots-toggle .badge {
                position: absolute;
                top: -4px;
                right: -4px;
                background: #ef5350;
                color: #fff;
                font-size: 11px;
                min-width: 18px;
                height: 18px;
                border-radius: 9px;
                display: flex;
                align-items: center;
                justify-content: center;
                padding: 0 4px;
            }

            #gst-dots-panel {
                position: fixed;
                bottom: 76px;
                right: 16px;
                z-index: 99997;
                width: 420px;
                max-height: 70vh;
                background: var(--gst-dots-bg);
                border: 1px solid var(--gst-dots-border);
                border-radius: 12px;
                box-shadow: 0 4px 24px rgba(0,0,0,.5);
                display: none;
                flex-direction: column;
                font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
                color: var(--gst-dots-text);
                overflow: hidden;
            }
            #gst-dots-panel.visible { display: flex; }

            #gst-dots-panel .panel-header {
                display: flex;
                align-items: center;
                justify-content: space-between;
                padding: 12px 16px;
                border-bottom: 1px solid var(--gst-dots-border);
                background: var(--gst-dots-surface);
                flex-shrink: 0;
            }
            #gst-dots-panel .panel-header h3 {
                margin: 0;
                font-size: 14px;
                color: #fff;
                font-weight: 600;
            }

            #gst-dots-panel .panel-actions {
                display: flex;
                gap: 8px;
            }

            #gst-dots-panel .panel-actions button {
                background: var(--gst-dots-surface);
                color: var(--gst-dots-text);
                border: 1px solid var(--gst-dots-border);
                padding: 4px 12px;
                border-radius: 4px;
                cursor: pointer;
                font-size: 12px;
            }
            #gst-dots-panel .panel-actions button:hover {
                background: #333;
                border-color: #555;
            }
            #gst-dots-panel .panel-actions button.primary {
                background: var(--gst-dots-accent);
                color: #fff;
                border-color: var(--gst-dots-accent);
            }
            #gst-dots-panel .panel-actions button.primary:hover {
                background: #3888e8;
            }

            #gst-dots-panel .panel-search {
                padding: 8px 16px;
                border-bottom: 1px solid var(--gst-dots-border);
                flex-shrink: 0;
            }
            #gst-dots-panel .panel-search input {
                width: 100%;
                background: var(--gst-dots-surface);
                color: var(--gst-dots-text);
                border: 1px solid var(--gst-dots-border);
                border-radius: 4px;
                padding: 6px 10px;
                font-size: 12px;
                outline: none;
            }
            #gst-dots-panel .panel-search input:focus {
                border-color: var(--gst-dots-accent);
            }

            #gst-dots-panel .panel-content {
                flex: 1;
                overflow-y: auto;
                padding: 8px 16px;
                min-height: 100px;
            }

            #gst-dots-panel .pipeline-card {
                background: var(--gst-dots-surface);
                border: 1px solid var(--gst-dots-border);
                border-radius: 8px;
                margin-bottom: 8px;
                cursor: pointer;
                transition: border-color .15s;
                overflow: hidden;
            }
            #gst-dots-panel .pipeline-card:hover {
                border-color: var(--gst-dots-accent);
            }
            #gst-dots-panel .pipeline-card h4 {
                margin: 0;
                padding: 10px 12px;
                font-size: 12px;
                font-weight: 500;
                color: var(--gst-dots-text);
                border-bottom: 1px solid var(--gst-dots-border);
            }
            #gst-dots-panel .pipeline-card .preview {
                height: 120px;
                overflow: hidden;
                padding: 4px;
                pointer-events: none;
            }
            #gst-dots-panel .pipeline-card .preview svg {
                width: 100%;
                height: 100%;
            }

            #gst-dots-panel .empty-state {
                text-align: center;
                color: var(--gst-dots-text-dim);
                padding: 32px 16px;
                font-size: 13px;
            }

            /* ── Overlay (used by SvgOverlayManager) ────────────────── */
            .gst-dots-overlay {
                position: fixed;
                top: 0; left: 0; right: 0; bottom: 0;
                z-index: 99999;
                background: var(--gst-dots-bg, #1a1a1a);
                display: flex;
                flex-direction: column;
            }
            .gst-dots-overlay .overlay-header {
                display: flex;
                align-items: center;
                justify-content: space-between;
                padding: 8px 16px;
                background: var(--gst-dots-surface, #222);
                border-bottom: 1px solid var(--gst-dots-border, #3a3a3a);
                flex-shrink: 0;
            }
            .gst-dots-overlay .overlay-header h2 {
                margin: 0;
                font-size: 14px;
                color: #fff;
                font-weight: 600;
            }
            .gst-dots-overlay .closebtn {
                color: #aaa;
                font-size: 28px;
                cursor: pointer;
                line-height: 1;
                text-decoration: none;
            }
            .gst-dots-overlay .closebtn:hover { color: #fff; }
            .gst-dots-overlay .overlay-content {
                flex: 1;
                overflow: hidden;
                position: relative;
            }
            .gst-dots-overlay #graph {
                width: 100%;
                height: 100%;
            }
            .gst-dots-overlay .overlay-instructions {
                padding: 4px 16px;
                font-size: 11px;
                color: var(--gst-dots-text-dim, #888);
                background: var(--gst-dots-surface, #222);
                border-top: 1px solid var(--gst-dots-border, #3a3a3a);
                text-align: center;
            }
            .gst-dots-overlay .overlay-actions {
                padding: 6px 16px;
                background: var(--gst-dots-surface, #222);
                border-top: 1px solid var(--gst-dots-border, #3a3a3a);
                display: flex;
                gap: 8px;
            }
            .gst-dots-overlay .overlay-actions button {
                background: var(--gst-dots-surface, #222);
                color: var(--gst-dots-text, #e0e0e0);
                border: 1px solid var(--gst-dots-border, #3a3a3a);
                padding: 4px 12px;
                border-radius: 4px;
                cursor: pointer;
                font-size: 12px;
            }
            .gst-dots-overlay .overlay-actions button:hover {
                background: #333;
            }

            /* Custom tooltip (same as dots-viewer) */
            .custom-tooltip {
                position: absolute;
                background-color: #333;
                color: white;
                padding: 8px 12px;
                border-radius: 4px;
                font-size: 12px;
                z-index: 100001;
                pointer-events: none;
                opacity: 0;
                transition: opacity 0.2s;
                max-width: 600px;
                word-break: break-word;
                white-space: pre-wrap;
                font-family: monospace;
                text-align: left;
                user-select: text;
                -webkit-user-select: text;
            }
            .custom-tooltip.show { opacity: 1; }
            .custom-tooltip.interactive {
                pointer-events: auto;
                cursor: text;
                border: 2px solid #555;
            }

            /* Caps tooltip formatting */
            .caps-tooltip { font-family: monospace; font-size: 12px; }
            .caps-tooltip-sep { border-top: 1px solid #555; margin: 4px 0; }
            .caps-tooltip-field { display: flex; gap: 4px; }
            .caps-tooltip-key { color: #8cf; min-width: var(--key-w, 10ch); }

            /* Scrollbar styling for panel */
            #gst-dots-panel ::-webkit-scrollbar { width: 6px; }
            #gst-dots-panel ::-webkit-scrollbar-track { background: transparent; }
            #gst-dots-panel ::-webkit-scrollbar-thumb {
                background: var(--gst-dots-border);
                border-radius: 3px;
            }
        `;
        document.head.appendChild(style);
    }

    function injectUI() {
        if (toggleButton) return;

        injectStyles();

        // Toggle button
        toggleButton = document.createElement('button');
        toggleButton.id = 'gst-dots-toggle';
        toggleButton.innerHTML = '<svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"/></svg>';
        toggleButton.title = 'GStreamer Pipeline Viewer';
        toggleButton.addEventListener('click', () => {
            panelElement.classList.toggle('visible');
        });
        document.body.appendChild(toggleButton);

        // Panel
        panelElement = document.createElement('div');
        panelElement.id = 'gst-dots-panel';
        panelElement.innerHTML = `
            <div class="panel-header">
                <h3>Pipeline Viewer</h3>
                <div class="panel-actions">
                    <button class="primary" id="gst-dots-dump-btn">Dump Pipelines</button>
                    <button id="gst-dots-clear-btn">Clear</button>
                </div>
            </div>
            <div class="panel-search">
                <input type="text" placeholder="Search pipelines..." id="gst-dots-search" />
            </div>
            <div class="panel-content" id="gst-dots-pipelines">
                <div class="empty-state">Click "Dump Pipelines" to capture pipeline graphs</div>
            </div>
        `;
        document.body.appendChild(panelElement);

        // Wire up buttons
        document.getElementById('gst-dots-dump-btn').addEventListener('click', () => {
            if (typeof Module !== 'undefined' && Module._gst_dots_snapshot) {
                Module._gst_dots_snapshot();
            }
        });

        document.getElementById('gst-dots-clear-btn').addEventListener('click', () => {
            clearPipelines();
        });

        searchInput = document.getElementById('gst-dots-search');
        searchInput.addEventListener('input', updateSearch);

        pipelinesDiv = document.getElementById('gst-dots-pipelines');
    }

    function clearPipelines() {
        if (!pipelinesDiv) return;
        pipelinesDiv.innerHTML = '<div class="empty-state">Click "Dump Pipelines" to capture pipeline graphs</div>';
        dotCount = 0;
        updateBadge();
    }

    function updateBadge() {
        if (!toggleButton) return;
        let badge = toggleButton.querySelector('.badge');
        if (dotCount > 0) {
            if (!badge) {
                badge = document.createElement('span');
                badge.className = 'badge';
                toggleButton.appendChild(badge);
            }
            badge.textContent = dotCount;
        } else if (badge) {
            badge.remove();
        }
    }

    function activatePendingObservers() {
        for (const { observer, target } of pendingObservers) {
            observer.observe(target);
        }
        pendingObservers = [];
    }

    function scheduleObserverActivation() {
        if (observerDebounceTimer) {
            clearTimeout(observerDebounceTimer);
        }
        observerDebounceTimer = setTimeout(activatePendingObservers, OBSERVER_DEBOUNCE_MS);
    }

    function generatePreviewSvg(container) {
        if (container.rendered) return;
        container.rendered = true;

        renderDotToSvg(container.dotContent).then(svgEl => {
            svgEl.style.width = '100%';
            svgEl.style.height = '100%';
            container.innerHTML = '';
            container.appendChild(svgEl);
        }).catch(error => {
            console.error('Failed rendering preview SVG:', error);
            container.textContent = 'SVG render failed';
        });
    }

    function addPipelineCard(name, content) {
        if (!pipelinesDiv) return;

        // Remove empty state
        const emptyState = pipelinesDiv.querySelector('.empty-state');
        if (emptyState) emptyState.remove();

        // Check if already exists, update if so
        const existingId = 'gst-dot-' + name.replace(/[^a-zA-Z0-9_-]/g, '_');
        const existing = document.getElementById(existingId);
        if (existing) {
            existing.remove();
            dotCount--;
        }

        const card = document.createElement('div');
        card.className = 'pipeline-card';
        card.id = existingId;

        const title = document.createElement('h4');
        title.textContent = name;
        card.appendChild(title);

        const previewDiv = document.createElement('div');
        previewDiv.className = 'preview';
        previewDiv.dotContent = content;
        card.appendChild(previewDiv);

        // Lazy render preview
        const observer = new IntersectionObserver((entries, obs) => {
            for (const entry of entries) {
                if (entry.isIntersecting) {
                    generatePreviewSvg(previewDiv);
                    obs.unobserve(entry.target);
                }
            }
        }, { rootMargin: '100px' });
        pendingObservers.push({ observer, target: previewDiv });
        scheduleObserverActivation();

        card.addEventListener('click', () => {
            openOverlay(name, content);
        });

        if (pipelinesDiv.firstChild) {
            pipelinesDiv.insertBefore(card, pipelinesDiv.firstChild);
        } else {
            pipelinesDiv.appendChild(card);
        }

        dotCount++;
        updateBadge();
    }

    /**
     * Opens a full-screen interactive overlay for a pipeline DOT graph.
     * Uses SvgOverlayManager from the dots-viewer for node highlighting,
     * zoom/pan, tooltips, text ellipsizing, and cluster drill-down.
     */
    function openOverlay(name, content) {
        removeOverlay();

        const overlayDiv = document.createElement('div');
        overlayDiv.id = 'overlay';
        overlayDiv.className = 'gst-dots-overlay';

        // Header
        const header = document.createElement('div');
        header.className = 'overlay-header';
        const title = document.createElement('h2');
        title.id = 'title';
        title.textContent = name;
        header.appendChild(title);
        const closeButton = document.createElement('a');
        closeButton.className = 'closebtn';
        closeButton.innerHTML = '&times;';
        closeButton.onclick = (e) => { removeOverlay(); e.stopPropagation(); };
        header.appendChild(closeButton);
        overlayDiv.appendChild(header);

        // Content — #graph is where SvgOverlayManager renders via d3-graphviz
        const contentDiv = document.createElement('div');
        contentDiv.className = 'overlay-content';
        const graphDiv = document.createElement('div');
        graphDiv.id = 'graph';
        contentDiv.appendChild(graphDiv);
        overlayDiv.appendChild(contentDiv);

        // Instructions
        const instructions = document.createElement('div');
        instructions.className = 'overlay-instructions';
        instructions.textContent = 'Click node to highlight connections | Double-click cluster to zoom | Ctrl+scroll to zoom | Esc to close';
        overlayDiv.appendChild(instructions);

        // Actions
        const actions = document.createElement('div');
        actions.className = 'overlay-actions';
        const saveBtn = document.createElement('button');
        saveBtn.id = 'save-svg';
        saveBtn.textContent = 'Save SVG';
        actions.appendChild(saveBtn);
        overlayDiv.appendChild(actions);

        document.body.appendChild(overlayDiv);

        // Use SvgOverlayManager for interactive rendering
        const manager = new SvgOverlayManager();
        manager.init(content, name).catch(error => {
            console.error('SvgOverlayManager init failed:', error);
            graphDiv.textContent = 'Failed to render: ' + error.message;
        });

        // Keyboard close handler (Esc — works with SvgOverlayManager's
        // two-press Esc which consumes the first press for unhighlight)
        const keyHandler = (evt) => {
            if (evt.key === 'Escape' && document.getElementById('overlay')) {
                removeOverlay();
                document.removeEventListener('keyup', keyHandler);
            }
        };
        document.addEventListener('keyup', keyHandler);
    }

    function removeOverlay() {
        const overlay = document.getElementById('overlay');
        if (overlay) overlay.remove();
        // Clean up any orphaned tooltips
        document.querySelectorAll('.custom-tooltip').forEach(t => t.remove());
    }

    function updateSearch() {
        if (!searchInput || !pipelinesDiv) return;

        const query = searchInput.value.trim();
        const cards = pipelinesDiv.querySelectorAll('.pipeline-card');

        if (!query) {
            cards.forEach(card => { card.style.display = ''; });
            return;
        }

        if (typeof Fuse !== 'undefined') {
            const list = Array.from(cards).map(card => ({
                id: card.id,
                title: card.querySelector('h4').textContent
            }));

            const fuse = new Fuse(list, {
                includeScore: true,
                threshold: 0.6,
                keys: ['title']
            });
            const results = fuse.search(query);
            const matchedIds = new Set(results.map(r => r.item.id));

            cards.forEach(card => {
                card.style.display = matchedIds.has(card.id) ? '' : 'none';
            });
        } else {
            // Simple fallback filter
            const lower = query.toLowerCase();
            cards.forEach(card => {
                const title = card.querySelector('h4').textContent.toLowerCase();
                card.style.display = title.includes(lower) ? '' : 'none';
            });
        }
    }

    // Register global callback
    globalThis.__gstDotsWasm = {
        onNewDot: function (name, content) {
            // Ensure UI is injected (may be first DOT received)
            if (!panelElement) {
                if (document.readyState === 'loading') {
                    document.addEventListener('DOMContentLoaded', () => {
                        injectUI();
                        addPipelineCard(name, content);
                    });
                    return;
                }
                injectUI();
            }
            addPipelineCard(name, content);
        }
    };

    // Auto-inject UI when DOM is ready (even before first DOT)
    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', injectUI);
    } else {
        injectUI();
    }

})();
