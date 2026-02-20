/*
 * D3.js-based timeline renderer for parsed .xges data.
 * Supports nested timeline drill-down and per-layer height resizing.
 */
import * as d3 from 'd3';
import {
  formatTimecode, nsToSeconds, trackTypeToString,
  TRACK_TYPE_AUDIO, TRACK_TYPE_VIDEO,
} from './xges-parser.js';

const RULER_H      = 34;
const DEFAULT_LH   = 72;
const MIN_LH       = 40;
const CLIP_PAD     = 10;   /* vertical padding around clip rect inside a layer */
const LABEL_W      = 90;
const MIN_W_TEXT   = 40;
const MIN_W_BADGE  = 60;
const HANDLE_H     = 6;    /* height of the resize grab area */

/* distinct colours for keyframe binding lines */
const _KF_COLORS = [
  'rgba(255,200,80,.7)',   /* amber  */
  'rgba(100,200,255,.7)',  /* cyan   */
  'rgba(255,120,180,.7)',  /* pink   */
  'rgba(120,255,160,.7)',  /* green  */
  'rgba(180,140,255,.7)',  /* purple */
  'rgba(255,160,100,.7)',  /* orange */
];

/* ── helpers ──────────────────────────────────────────────────────── */


function esc(s) {
  if (s == null) return '';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function fmtVal(v) {
  if (v == null) return '';
  if (typeof v === 'object') {
    if (v.numerator !== undefined) return `${v.numerator}/${v.denominator}`;
    return JSON.stringify(v);
  }
  return String(v);
}

function clipCssClass(clip) {
  if (clip.isNestedTimeline) return 'clip-nested';
  switch (clip.typeName) {
    case 'GESUriClip':        return 'clip-uri';
    case 'GESTitleClip':      return 'clip-title';
    case 'GESTransitionClip': return 'clip-transition';
    case 'GESTestClip':       return 'clip-test';
    case 'GESOverlayClip':    return 'clip-overlay';
    default:                  return 'clip-other';
  }
}

/* ── TimelineViewer ───────────────────────────────────────────────── */

export class TimelineViewer {
  constructor({ timelineEl, detailEl, infoEl, breadcrumbEl }) {
    this.timelineEl    = timelineEl;
    this.detailEl      = detailEl;
    this.infoEl        = infoEl;
    this.breadcrumbEl  = breadcrumbEl;
    this.rootData      = null;
    this.data          = null;
    this.selectedClip  = null;
    this.currentTransform = d3.zoomIdentity;
    /* navigation stack for nested timelines */
    this.navStack = [];
    /* per-layer heights (initialised in renderTimeline) */
    this.layerHeights = [];
  }

  /* ── layout helpers ─────────────────────────────────────────────── */

  _layerY(i) {
    let y = RULER_H;
    for (let j = 0; j < i; j++) y += this.layerHeights[j];
    return y;
  }

  _totalH() {
    let h = RULER_H;
    for (const lh of this.layerHeights) h += lh;
    return h;
  }

  _clipH(li)      { return Math.max(20, this.layerHeights[li] - CLIP_PAD * 2); }
  _clipMargin()    { return CLIP_PAD; }

  /* ── public API ──────────────────────────────────────────────────── */

  render(data) {
    this.rootData = data;
    this.data = data;
    this.navStack = [];
    this.selectedClip = null;
    this._updateBreadcrumb();
    this.renderInfoPanel();
    this.clearDetailPanel();
    this.renderTimeline();
  }

  /* ── nested timeline navigation ──────────────────────────────────── */

  _drillDown(clip) {
    const asset = this.data.project.assetMap.get(clip.assetId);
    if (!asset || !asset.subproject) return;

    /* push current state */
    this.navStack.push({
      data: this.data,
      selectedClip: this.selectedClip,
      transform: this.currentTransform,
      layerHeights: this.layerHeights.slice(),
    });

    /* build a data object that mirrors the top-level shape */
    const sub = asset.subproject;
    this.data = {
      version: this.rootData.version,
      project: sub,
      timeline: sub.timeline,
    };
    this.selectedClip = null;
    this.currentTransform = d3.zoomIdentity;
    this.layerHeights = [];

    this._updateBreadcrumb();
    this.renderInfoPanel();
    this.clearDetailPanel();
    this.renderTimeline();
  }

  _navigateUp(index) {
    if (index < 0 || index >= this.navStack.length) return;
    const entry = this.navStack[index];
    this.navStack = this.navStack.slice(0, index);
    this.data = entry.data;
    this.selectedClip = null;
    this.currentTransform = entry.transform;
    this.layerHeights = entry.layerHeights || [];

    this._updateBreadcrumb();
    this.renderInfoPanel();
    this.clearDetailPanel();
    this.renderTimeline();
  }

  _updateBreadcrumb() {
    if (!this.breadcrumbEl) return;
    if (!this.navStack.length) {
      this.breadcrumbEl.classList.add('hidden');
      return;
    }
    this.breadcrumbEl.classList.remove('hidden');
    this.breadcrumbEl.innerHTML = '';

    /* root crumb */
    const rootA = document.createElement('a');
    rootA.href = '#';
    rootA.textContent = 'Root Timeline';
    rootA.addEventListener('click', e => { e.preventDefault(); this._navigateUp(0); });
    this.breadcrumbEl.appendChild(rootA);

    /* intermediate crumbs */
    for (let i = 1; i < this.navStack.length; i++) {
      const sep = document.createElement('span');
      sep.textContent = ' \u203a ';
      sep.className = 'breadcrumb-sep';
      this.breadcrumbEl.appendChild(sep);
      const a = document.createElement('a');
      a.href = '#';
      a.textContent = this.navStack[i].data.project.metadatas.get('name')
                    || `Sub-timeline ${i}`;
      const idx = i;
      a.addEventListener('click', e => { e.preventDefault(); this._navigateUp(idx); });
      this.breadcrumbEl.appendChild(a);
    }

    /* current (non-link) */
    const sep = document.createElement('span');
    sep.textContent = ' \u203a ';
    sep.className = 'breadcrumb-sep';
    this.breadcrumbEl.appendChild(sep);
    const cur = document.createElement('span');
    cur.className = 'breadcrumb-current';
    cur.textContent = this.data.project.metadatas.get('name') || 'Sub-timeline';
    this.breadcrumbEl.appendChild(cur);
  }

  /* ── info sidebar ────────────────────────────────────────────────── */

  renderInfoPanel() {
    const el = this.infoEl;
    if (!el) return;
    const d = this.data;
    let h = '';

    /* project */
    h += '<div class="info-section"><h3>Project</h3>';
    const pm = d.project.metadatas;
    const pmName = pm.get('name'), pmAuthor = pm.get('author'), pmScale = pm.get('render-scale');
    if (pmName)   h += `<div class="info-item"><span class="info-label">Name:</span> ${esc(pmName)}</div>`;
    if (pmAuthor) h += `<div class="info-item"><span class="info-label">Author:</span> ${esc(pmAuthor)}</div>`;
    if (pmScale)  h += `<div class="info-item"><span class="info-label">Render Scale:</span> ${pmScale}%</div>`;
    h += `<div class="info-item"><span class="info-label">Version:</span> ${esc(d.version)}</div>`;
    if (this.navStack.length)
      h += `<div class="info-item"><span class="info-label">Depth:</span> nested level ${this.navStack.length}</div>`;
    h += '</div>';

    /* timeline */
    if (d.timeline) {
      const tl = d.timeline, tp = tl.properties;
      h += '<div class="info-section"><h3>Timeline</h3>';
      if (tl.durationSeconds) h += `<div class="info-item"><span class="info-label">Duration:</span> ${formatTimecode(tl.durationSeconds)}</div>`;
      const tpAT = tp.get('auto-transition'), tpSnap = tp.get('snapping-distance');
      if (tpAT != null) h += `<div class="info-item"><span class="info-label">Auto-transition:</span> ${tpAT}</div>`;
      if (tpSnap)       h += `<div class="info-item"><span class="info-label">Snapping:</span> ${formatTimecode(nsToSeconds(tpSnap))}</div>`;
      h += `<div class="info-item"><span class="info-label">Layers:</span> ${tl.layers.length}</div>`;

      if (tl.tracks.length) {
        h += '<h4>Tracks</h4>';
        for (const t of tl.tracks) {
          const cls = t.trackType === TRACK_TYPE_VIDEO ? 'track-video' : 'track-audio';
          h += `<div class="info-track"><span class="track-badge ${cls}">${trackTypeToString(t.trackType)}</span>`;
          h += ` <span class="info-muted">ID ${t.trackId}</span>`;
          const rcHtml = renderCapsHtml(t.properties.get('restriction-caps'));
          if (rcHtml) h += rcHtml;
          h += '</div>';
        }
      }

      if (tl.groups.length) {
        h += '<h4>Groups</h4>';
        for (const g of tl.groups) {
          const gn = g.properties.get('name') || `Group ${g.id}`;
          h += `<div class="info-group"><span class="info-label">${esc(gn)}</span> (${g.children.length} children)</div>`;
        }
      }
      h += '</div>';
    }

    /* assets — filter out GESTimeline duplicates (the GESUriClip entry is preferred) */
    const displayAssets = d.project.assets.filter(a => {
      if (a.extractableTypeName !== 'GESTimeline') return true;
      return !d.project.assets.some(
        b => b.id === a.id && b.extractableTypeName !== 'GESTimeline'
      );
    });
    if (displayAssets.length) {
      h += `<div class="info-section"><h3>Assets (${displayAssets.length})</h3>`;
      for (const a of displayAssets) {
        h += '<div class="info-asset">';
        h += `<div class="asset-name">${esc(a.displayName)}</div>`;
        h += `<div class="asset-type">${esc(a.extractableTypeName)}</div>`;
        if (a.subproject) {
          const stl = a.subproject.timeline;
          if (stl) {
            const layers = stl.layers.length;
            let clips = 0;
            for (const l of stl.layers) clips += l.clips.length;
            h += `<div class="asset-duration">${layers} layer${layers !== 1 ? 's' : ''}, ${clips} clip${clips !== 1 ? 's' : ''}`;
            if (stl.durationSeconds) h += ` \u2014 ${formatTimecode(stl.durationSeconds)}`;
            h += '</div>';
          }
        } else if (a.properties.get('duration')) {
          h += `<div class="asset-duration">${formatTimecode(nsToSeconds(a.properties.get('duration')))}</div>`;
        }
        h += '</div>';
      }
      h += '</div>';
    }

    /* encoding profiles */
    if (d.project.encodingProfiles.length) {
      h += '<div class="info-section"><h3>Encoding Profiles</h3>';
      for (const ep of d.project.encodingProfiles) {
        h += `<div class="info-encoding"><div class="encoding-name">${esc(ep.name || 'Unnamed')}</div>`;
        if (ep.description && ep.description !== '(null)') h += `<div class="info-muted">${esc(ep.description)}</div>`;
        if (ep.format) h += `<div class="info-item"><span class="info-label">Container:</span> ${esc(ep.format)}</div>`;
        for (const s of ep.streams) {
          const cls = s.type === 'video' ? 'track-video' : 'track-audio';
          h += `<div class="info-stream"><span class="track-badge ${cls}">${s.type}</span> ${esc(s.format || '')}`;
          if (s.presetName) h += ` <span class="info-muted">(${esc(s.presetName)})</span>`;
          if (s.restriction) { const rHtml = renderCapsHtml(s.restriction); h += rHtml || `<div class="info-caps">${esc(s.restriction)}</div>`; }
          h += '</div>';
        }
        h += '</div>';
      }
      h += '</div>';
    }

    el.innerHTML = h;
  }

  /* ── detail sidebar ──────────────────────────────────────────────── */

  clearDetailPanel() {
    if (this.detailEl) this.detailEl.innerHTML = '<div class="detail-empty">Click a clip to view details</div>';
  }

  showClipDetail(clip) {
    if (!this.detailEl) return;
    const asset = this.data.project.assetMap.get(clip.assetId);
    let h = '<div class="detail-content">';
    h += `<h3 class="detail-title">${esc(clip.displayName)}</h3>`;
    h += `<div class="detail-type">${esc(clip.typeName)}</div>`;

    /* nested timeline drill-down button */
    if (clip.isNestedTimeline) {
      h += '<div class="detail-section">';
      h += '<button class="nested-drilldown" id="drilldown-btn">';
      h += 'Open nested timeline \u25b6</button>';
      h += '</div>';
    }

    /* timing */
    h += '<div class="detail-section"><h4>Timing</h4>';
    h += tbl([
      ['Start',       formatTimecode(clip.start)],
      ['Duration',    formatTimecode(clip.duration)],
      ['End',         formatTimecode(clip.start + clip.duration)],
      ['In-point',    formatTimecode(clip.inpoint)],
      ['Track Types', trackTypeToString(clip.sourceTrackTypes || clip.trackTypes)],
    ]);
    if (clip.rate) h += tbl([['Rate', clip.rate]]);
    h += '</div>';

    /* raw ns */
    h += '<div class="detail-section"><h4>Raw Timing (ns)</h4>';
    h += tbl([
      ['Start',    clip.startNs.toLocaleString()],
      ['Duration', clip.durationNs.toLocaleString()],
      ['In-point', clip.inpointNs.toLocaleString()],
    ]);
    h += '</div>';

    /* asset */
    if (asset) {
      h += '<div class="detail-section"><h4>Asset</h4>';
      const rows = [['ID', asset.id], ['Type', asset.extractableTypeName]];
      const aDuration = asset.properties.get('duration');
      if (aDuration)
        rows.push(['Source Duration', formatTimecode(nsToSeconds(aDuration))]);
      const aFormats = asset.properties.get('supported-formats');
      if (aFormats)
        rows.push(['Formats', trackTypeToString(aFormats)]);
      h += tbl(rows, true);

      /* nested timeline summary */
      if (asset.subproject && asset.subproject.timeline) {
        const stl = asset.subproject.timeline;
        h += '<h4>Nested Timeline</h4>';
        const nrows = [];
        if (stl.durationSeconds) nrows.push(['Duration', formatTimecode(stl.durationSeconds)]);
        nrows.push(['Layers', stl.layers.length]);
        let totalClips = 0;
        for (const l of stl.layers) totalClips += l.clips.length;
        nrows.push(['Clips', totalClips]);
        nrows.push(['Tracks', stl.tracks.length]);
        h += tbl(nrows);
      }

      const mk = [...asset.metadatas.keys()];
      if (mk.length) {
        h += '<h4>Asset Metadata</h4>';
        h += tbl(mk.map(k => [k, fmtVal(asset.metadatas.get(k))]), true);
      }
      h += '</div>';
    }

    /* properties */
    h += propsSection('Properties', clip.properties);
    h += propsSection('Children Properties', clip.childrenProperties);

    /* effects */
    if (clip.effects.length) {
      h += `<div class="detail-section"><h4>Effects (${clip.effects.length})</h4>`;
      for (const fx of clip.effects) {
        h += `<div class="detail-effect"><div class="effect-name">${esc(fx.assetId)}</div>`;
        h += tbl([
          ['Track Type', trackTypeToString(fx.trackType)],
          ['Track ID',   fx.trackId],
          ['Active',     fx.properties.get('active') ?? ''],
          ['Priority',   fx.properties.get('priority') ?? ''],
        ]);
        const cpk = [...fx.childrenProperties.keys()];
        if (cpk.length) {
          h += '<div class="effect-params">Parameters:</div>';
          h += tbl(cpk.map(k => [k, fmtVal(fx.childrenProperties.get(k))]));
        }
        h += '</div>';
      }
      h += '</div>';
    }

    /* sources */
    if (clip.sources.length) {
      h += `<div class="detail-section"><h4>Track Sources (${clip.sources.length})</h4>`;
      for (const src of clip.sources) {
        h += `<div class="detail-source"><div class="source-track">Track ID: ${esc(src.trackId)}</div>`;
        const sk = [...src.childrenProperties.keys()];
        if (sk.length) h += tbl(sk.map(k => [k, fmtVal(src.childrenProperties.get(k))]));
        for (const b of src.bindings) {
          h += `<div class="detail-binding"><div class="binding-header">Keyframes: ${esc(b.property)}</div>`;
          h += tbl([['Type', b.type], ['Source', b.sourceType], ['Mode', b.mode]]);
          if (b.values.length) {
            h += `<details class="kf-details"><summary>Values (${b.values.length})</summary>`;
            h += '<table class="detail-table keyframe-table"><tr><th>Time</th><th>Value</th></tr>';
            for (const kf of b.values) h += `<tr><td>${formatTimecode(kf.timestamp)}</td><td>${kf.value.toFixed(4)}</td></tr>`;
            h += '</table></details>';
          }
          h += '</div>';
        }
        h += '</div>';
      }
      h += '</div>';
    }

    /* metadata */
    h += propsSection('Metadata', clip.metadatas);
    h += '</div>';
    this.detailEl.innerHTML = h;

    /* wire up drill-down button */
    if (clip.isNestedTimeline) {
      const btn = this.detailEl.querySelector('#drilldown-btn');
      if (btn) btn.addEventListener('click', () => this._drillDown(clip));
    }
  }

  /* ── SVG timeline ────────────────────────────────────────────────── */

  renderTimeline() {
    this.timelineEl.innerHTML = '';
    const tl = this.data.timeline;
    if (!tl || !tl.layers.length) {
      this.timelineEl.innerHTML = '<div class="detail-empty" style="padding-top:4em">No timeline data found</div>';
      return;
    }

    const layers = tl.layers;

    /* initialise per-layer heights if needed */
    if (this.layerHeights.length !== layers.length)
      this.layerHeights = layers.map(() => DEFAULT_LH);

    let maxEnd = 0;
    for (const l of layers)
      for (const c of l.clips)
        maxEnd = Math.max(maxEnd, c.start + c.duration);
    if (tl.durationSeconds > maxEnd) maxEnd = tl.durationSeconds;
    this.timelineDuration = maxEnd * 1.05 || 10;

    const rect = this.timelineEl.getBoundingClientRect();
    this.w = rect.width || 800;
    this.h = this._totalH();

    this.baseX = d3.scaleLinear()
      .domain([0, this.timelineDuration])
      .range([LABEL_W, this.w]);
    this.currentTransform = d3.zoomIdentity;

    this.svg = d3.select(this.timelineEl).append('svg')
      .attr('width', '100%').attr('height', this.h)
      .attr('class', 'timeline-svg');

    /* nested timeline stripe pattern */
    const defs = this.svg.append('defs');
    defs.append('clipPath').attr('id','tl-clip')
      .append('rect')
      .attr('x', LABEL_W).attr('y', 0)
      .attr('width', this.w - LABEL_W).attr('height', this.h + 20);

    const pat = defs.append('pattern')
      .attr('id', 'nested-stripes')
      .attr('width', 8).attr('height', 8)
      .attr('patternUnits', 'userSpaceOnUse')
      .attr('patternTransform', 'rotate(45)');
    pat.append('rect').attr('width', 8).attr('height', 8).attr('fill', 'rgba(0,0,0,0)');
    pat.append('line').attr('x1',0).attr('y1',0).attr('x2',0).attr('y2',8)
      .attr('stroke','rgba(255,255,255,0.1)').attr('stroke-width',4);

    this.bgG     = this.svg.append('g').attr('class','bg-group');
    this.clipG   = this.svg.append('g').attr('class','clip-group').attr('clip-path','url(#tl-clip)');
    this.rulerG  = this.svg.append('g').attr('class','ruler-group').attr('clip-path','url(#tl-clip)');
    this.labelG  = this.svg.append('g').attr('class','label-group');
    this.handleG = this.svg.append('g').attr('class','handle-group');

    this._drawBgs(layers);
    this._drawLabels(layers);
    this._drawHandles(layers);
    this._updateView();
    this._setupZoom();
  }

  /* ── refresh after resize ──────────────────────────────────────── */

  _refreshLayout() {
    this.h = this._totalH();
    this.svg.attr('height', this.h);
    /* update clip-path to new height */
    this.svg.select('defs #tl-clip rect')
      .attr('height', this.h + 20);

    const layers = this.data.timeline.layers;
    this.bgG.selectAll('*').remove();
    this.labelG.selectAll('*').remove();
    this.handleG.selectAll('*').remove();
    this._drawBgs(layers);
    this._drawLabels(layers);
    this._drawHandles(layers);
    this._updateView();
  }

  /* backgrounds & labels */

  _drawBgs(layers) {
    layers.forEach((_, i) => {
      const y = this._layerY(i);
      const lh = this.layerHeights[i];
      this.bgG.append('rect')
        .attr('class', `layer-bg ${i % 2 ? 'odd' : 'even'}`)
        .attr('x', 0).attr('y', y)
        .attr('width', '100%').attr('height', lh);
      if (i > 0)
        this.bgG.append('line').attr('class','layer-separator')
          .attr('x1',0).attr('x2','100%').attr('y1',y).attr('y2',y);
    });
  }

  _drawLabels(layers) {
    this.labelG.append('rect').attr('class','label-bg ruler-label-bg')
      .attr('width', LABEL_W).attr('height', RULER_H);

    layers.forEach((layer, i) => {
      const y = this._layerY(i);
      const lh = this.layerHeights[i];
      const g = this.labelG.append('g').attr('transform',`translate(0,${y})`);
      g.append('rect').attr('class','label-bg').attr('width', LABEL_W).attr('height', lh);
      const hasAT = layer.properties.get('auto-transition');
      g.append('text').attr('class','label-text')
        .attr('x', LABEL_W/2).attr('y', lh/2 - (hasAT ? 6 : 0))
        .attr('text-anchor','middle').attr('dominant-baseline','middle')
        .text(`Layer ${layer.priority}`);
      if (hasAT)
        g.append('text').attr('class','label-subtitle')
          .attr('x', LABEL_W/2).attr('y', lh/2 + 10)
          .attr('text-anchor','middle').attr('dominant-baseline','middle')
          .text('auto-trans.');
    });
  }

  /* ── resize handles ─────────────────────────────────────────────── */

  _drawHandles(layers) {
    const me = this;
    layers.forEach((_, i) => {
      const y = this._layerY(i) + this.layerHeights[i] - HANDLE_H / 2;
      const handle = this.handleG.append('rect')
        .attr('class', 'layer-resize-handle')
        .attr('x', 0).attr('y', y)
        .attr('width', '100%').attr('height', HANDLE_H)
        .attr('cursor', 'ns-resize');

      const drag = d3.drag()
        .on('start', function(event) {
          d3.select(this).classed('active', true);
          event.sourceEvent.stopPropagation();
        })
        .on('drag', function(event) {
          const newH = Math.max(MIN_LH, me.layerHeights[i] + event.dy);
          me.layerHeights[i] = newH;
          me._refreshLayout();
        })
        .on('end', function() {
          d3.select(this).classed('active', false);
        });

      handle.call(drag);
    });
  }

  /* view update (called on zoom) */

  _getX() { return this.currentTransform.rescaleX(this.baseX); }

  _updateView() {
    this._drawRuler();
    this._drawClips();
  }

  _drawRuler() {
    this.rulerG.selectAll('*').remove();
    const x = this._getX();
    this.rulerG.append('rect').attr('class','ruler-bg')
      .attr('x', LABEL_W).attr('y',0)
      .attr('width', this.w).attr('height', RULER_H);
    const axis = d3.axisTop(x).tickFormat(d => formatTimecode(d)).tickSizeOuter(0).tickPadding(6);
    this.rulerG.append('g').attr('class','ruler-axis')
      .attr('transform',`translate(0,${RULER_H - 1})`)
      .call(axis);
  }

  _drawClips() {
    /* remove stale clip-paths from defs */
    this.svg.select('defs').selectAll('.cmask').remove();
    this.clipG.selectAll('*').remove();

    const x  = this._getX();
    const me = this;

    this.data.timeline.layers.forEach((layer, li) => {
      const ly  = me._layerY(li);
      const lh  = me.layerHeights[li];
      const ch  = me._clipH(li);
      const cm  = CLIP_PAD;

      layer.clips.forEach((clip, ci) => {
        const cx = x(clip.start);
        const cw = Math.max(2, x(clip.start + clip.duration) - cx);
        const cy = ly + cm;
        const sel = me.selectedClip === clip;

        const g = me.clipG.append('g')
          .attr('class', `clip ${clipCssClass(clip)}${sel ? ' selected' : ''}`)
          .attr('cursor', 'pointer')
          .on('click', event => { event.stopPropagation(); me._select(clip, g); });

        /* double-click to drill into nested timelines */
        if (clip.isNestedTimeline) {
          g.on('dblclick', event => { event.stopPropagation(); me._drillDown(clip); });
        }

        /* bg rect */
        g.append('rect').attr('class','clip-bg')
          .attr('x',cx).attr('y',cy).attr('width',cw).attr('height',ch)
          .attr('rx',5).attr('ry',5);

        /* nested timeline stripe overlay */
        if (clip.isNestedTimeline) {
          g.append('rect')
            .attr('x',cx).attr('y',cy).attr('width',cw).attr('height',ch)
            .attr('rx',5).attr('ry',5)
            .attr('fill', 'url(#nested-stripes)')
            .attr('pointer-events', 'none');
        }

        /* inpoint indicator */
        if (clip.inpoint > 0 && cw > 20)
          g.append('line').attr('class','clip-inpoint')
            .attr('x1',cx+3).attr('y1',cy+2).attr('x2',cx+3).attr('y2',cy+ch-2);

        /* text (clipped to rect) */
        if (cw > MIN_W_TEXT) {
          const mid = `cmask-${li}-${ci}`;
          me.svg.select('defs').append('clipPath').attr('id',mid).attr('class','cmask')
            .append('rect')
            .attr('x',cx+4).attr('y',cy)
            .attr('width', cw - 8 - (cw > MIN_W_BADGE ? 42 : 0))
            .attr('height', ch);

          const tg = g.append('g').attr('clip-path',`url(#${mid})`);
          tg.append('text').attr('class','clip-name')
            .attr('x',cx+8).attr('y',cy+20).text(clip.displayName);
          if (cw > 120 && ch > 40)
            tg.append('text').attr('class','clip-time-label')
              .attr('x',cx+8).attr('y',cy+36)
              .text(`${formatTimecode(clip.start)} \u2014 ${formatTimecode(clip.start + clip.duration)}`);
          /* hint for nested */
          if (clip.isNestedTimeline && cw > 160 && ch > 55)
            tg.append('text').attr('class','clip-time-label')
              .attr('x',cx+8).attr('y',cy+ch-6)
              .text('double-click to open');
        }

        /* badges — use actual source track types when available */
        const badgeTypes = clip.sourceTrackTypes || clip.trackTypes;
        if (cw > MIN_W_BADGE) {
          let bx = cx + cw - 22;
          if (badgeTypes & TRACK_TYPE_AUDIO) {
            g.append('rect').attr('class','badge badge-audio')
              .attr('x',bx).attr('y',cy+4).attr('width',18).attr('height',14).attr('rx',3);
            g.append('text').attr('class','badge-label')
              .attr('x',bx+9).attr('y',cy+14).attr('text-anchor','middle').text('A');
            bx -= 20;
          }
          if (badgeTypes & TRACK_TYPE_VIDEO) {
            g.append('rect').attr('class','badge badge-video')
              .attr('x',bx).attr('y',cy+4).attr('width',18).attr('height',14).attr('rx',3);
            g.append('text').attr('class','badge-label')
              .attr('x',bx+9).attr('y',cy+14).attr('text-anchor','middle').text('V');
          }
        }

        /* effect indicator */
        if (clip.effects.length && cw > 40) {
          g.append('circle').attr('class','effect-indicator')
            .attr('cx',cx+cw-14).attr('cy',cy+ch-14).attr('r',8);
          g.append('text').attr('class','effect-indicator-text')
            .attr('x',cx+cw-14).attr('y',cy+ch-10)
            .attr('text-anchor','middle')
            .text('fx' + (clip.effects.length > 1 ? clip.effects.length : ''));
        }

        /* keyframe polylines */
        if (cw > 30) {
          let bIdx = 0;
          const kfH = Math.max(8, ch - 38);
          for (const src of clip.sources) {
            for (const b of src.bindings) {
              if (b.values.length < 2) { bIdx++; continue; }
              const kfBot = cy + ch - 4;
              /* normalise values to each binding's own range */
              let vMin = Infinity, vMax = -Infinity;
              for (const kf of b.values) {
                if (kf.value < vMin) vMin = kf.value;
                if (kf.value > vMax) vMax = kf.value;
              }
              const vRange = vMax - vMin || 1;
              const inpt = clip.inpoint || 0;
              const pts = b.values
                .filter(kf => {
                  return kf.timestamp >= inpt
                      && kf.timestamp <= inpt + clip.duration;
                })
                .map(kf => {
                  const px = x(clip.start + kf.timestamp - inpt);
                  const norm = (kf.value - vMin) / vRange;
                  const py = kfBot - norm * kfH;
                  return `${px},${py}`;
                }).join(' ');
              if (pts) {
                const line = g.append('polyline')
                  .attr('class','keyframe-line')
                  .attr('stroke', _KF_COLORS[bIdx % _KF_COLORS.length])
                  .attr('points', pts);
                line.append('title').text(b.property);
              }
              bIdx++;
            }
          }
        }
      });
    });
  }

  /* zoom */

  _setupZoom() {
    const me = this;
    const zoom = d3.zoom()
      .scaleExtent([0.1, 500])
      .filter(e => e.type === 'wheel' || e.type === 'mousedown' || e.type === 'touchstart')
      .on('zoom', event => {
        const t = event.transform;
        me.currentTransform = d3.zoomIdentity.translate(t.x, 0).scale(t.k);
        me._updateView();
      });
    this.svg.call(zoom);
    this.svg.on('dblclick.zoom', null);
    this.svg.on('click.desel', () => me._deselect());

    /* prevent browser swipe-back navigation on horizontal scroll */
    this.svg.node().addEventListener('wheel', e => e.preventDefault(), { passive: false });
    this.svg.node().addEventListener('touchmove', e => e.preventDefault(), { passive: false });
  }

  /* selection */

  _select(clip, gSel) {
    this.selectedClip = clip;
    this.clipG.selectAll('.clip').classed('selected', false);
    gSel.classed('selected', true);
    this.showClipDetail(clip);
  }

  _deselect() {
    this.selectedClip = null;
    this.clipG.selectAll('.clip').classed('selected', false);
    this.clearDetailPanel();
  }
}

/* ── caps rendering ───────────────────────────────────────────────── */

function renderCapsHtml(str) {
  if (!str || str === 'NULL') return null;
  try {
    const caps = GstCaps.fromString(str.replace(/\\(.)/g, '$1'));
    if (caps.isAny)   return '<div class="caps-block caps-block-name">ANY</div>';
    if (caps.isEmpty) return '<div class="caps-block caps-block-name">EMPTY</div>';
    let h = '<div class="caps-block">';
    for (let i = 0; i < caps.length; i++) {
      const s = caps[i];
      const features = caps.getFeatures(i);
      if (i > 0) h += '<div class="caps-block-sep"></div>';
      let header = s.name;
      if (features.length) header += `(${features.join(',')})`;
      h += `<div class="caps-block-name">${esc(header)}</div>`;
      const keys = [...s.keys()];
      if (keys.length) {
        const keyW = Math.max(...keys.map(k => k.length)) + 2;
        for (const k of keys)
          h += `<div class="caps-block-field" style="--key-w:${keyW}ch">` +
               `<span class="caps-block-key">${esc(k)}:</span>` +
               `${esc(valueToStringBare(s.getTyped(k)))}` +
               '</div>';
      }
    }
    return h + '</div>';
  } catch (e) {
    return `<div class="info-caps">${esc(str)}</div>`;
  }
}

/* ── tiny helpers for detail HTML ─────────────────────────────────── */

function tbl(rows, uriClass) {
  let h = '<table class="detail-table">';
  for (const [k, v] of rows)
    h += `<tr><td>${esc(k)}</td><td${uriClass ? ' class="detail-uri"' : ''}>${esc(fmtVal(v))}</td></tr>`;
  return h + '</table>';
}

function propsSection(title, obj) {
  const keys = [...obj.keys()];
  if (!keys.length) return '';
  let h = `<div class="detail-section"><h4>${title}</h4>`;
  h += tbl(keys.map(k => [k, fmtVal(obj.get(k))]));
  return h + '</div>';
}
