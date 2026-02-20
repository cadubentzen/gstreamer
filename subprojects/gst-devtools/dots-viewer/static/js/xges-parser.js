/*
 * Parser for GStreamer Editing Services .xges files.
 * Converts .xges XML into structured JavaScript objects.
 * Supports nested timelines (subprojects).
 */

const NS_PER_SECOND = 1_000_000_000;
const GST_CLOCK_TIME_NONE = 18446744073709551615;

export const TRACK_TYPE_AUDIO = 2;
export const TRACK_TYPE_VIDEO = 4;

export function isClockTimeNone(ns) {
  return Number(ns) >= GST_CLOCK_TIME_NONE - 1000;
}

export function nsToSeconds(ns) {
  if (isClockTimeNone(ns)) return null;
  return Number(ns) / NS_PER_SECOND;
}

export function formatTimecode(totalSeconds) {
  if (totalSeconds == null || isNaN(totalSeconds)) return '\u2014';
  if (totalSeconds === Infinity || totalSeconds < 0) return '\u2014';
  const h = Math.floor(totalSeconds / 3600);
  const m = Math.floor((totalSeconds % 3600) / 60);
  const s = Math.floor(totalSeconds % 60);
  const ms = Math.round((totalSeconds % 1) * 1000);
  if (h > 0) {
    return `${h}:${p2(m)}:${p2(s)}.${p3(ms)}`;
  }
  return `${m}:${p2(s)}.${p3(ms)}`;
}

function p2(n) { return String(n).padStart(2, '0'); }
function p3(n) { return String(n).padStart(3, '0'); }

export function trackTypeToString(t) {
  const parts = [];
  if (t & TRACK_TYPE_VIDEO) parts.push('Video');
  if (t & TRACK_TYPE_AUDIO) parts.push('Audio');
  return parts.join(' + ') || 'Unknown';
}

/* ── GStreamer structure string parser ─────────────────────────────── */

const _emptyStruct = new GstStructure('', new Map());

export function parseGstStructure(str) {
  if (!str || !str.trim()) return _emptyStruct;
  str = str.trim();
  if (str.endsWith(';')) str = str.slice(0, -1).trim();
  try {
    return GstStructure.fromString(str);
  } catch (e) {
    return _emptyStruct;
  }
}

/* ── Binding values parser ────────────────────────────────────────── */

function parseBindingValues(valuesStr) {
  if (!valuesStr) return [];
  return valuesStr.trim().split(/\s+/)
    .filter(s => s.includes(':'))
    .map(pair => {
      const [ts, val] = pair.split(':');
      return { timestamp: nsToSeconds(Number(ts)), value: parseFloat(val) };
    });
}

/* ── Helpers ──────────────────────────────────────────────────────── */

function filenameFromUri(uri) {
  if (!uri) return '';
  const path = uri.startsWith('file://') ? uri.slice(7) : uri;
  return path.split('/').pop() || path;
}

function getClipDisplayName(clip, asset) {
  if (asset && asset.subproject) return `[Nested] ${filenameFromUri(asset.id)}`;
  switch (clip.typeName) {
    case 'GESUriClip':
      return asset ? filenameFromUri(asset.id) : filenameFromUri(clip.assetId);
    case 'GESTitleClip':
      return clip.properties.text || 'Title';
    case 'GESTransitionClip':
      return clip.assetId ? `Transition: ${clip.assetId}` : 'Transition';
    case 'GESTestClip':
      return 'Test Source';
    default:
      return clip.typeName || 'Clip';
  }
}

/* ── Encoding profile parser ──────────────────────────────────────── */

function parseEncodingProfiles(projectEl) {
  const encodingProfiles = [];
  for (const ep of projectEl.querySelectorAll('encoding-profiles > encoding-profile')) {
    const profile = {
      name: ep.getAttribute('name'),
      description: ep.getAttribute('description'),
      type: ep.getAttribute('type'),
      format: ep.getAttribute('format'),
      presetName: ep.getAttribute('preset-name'),
      streams: [],
    };
    for (const sp of ep.querySelectorAll('stream-profile')) {
      profile.streams.push({
        parent: sp.getAttribute('parent'),
        id: sp.getAttribute('id'),
        type: sp.getAttribute('type'),
        format: sp.getAttribute('format'),
        presetName: sp.getAttribute('preset-name'),
        presence: sp.getAttribute('presence'),
        restriction: sp.getAttribute('restriction'),
        pass: sp.getAttribute('pass'),
        variableFramerate: sp.getAttribute('variableframerate'),
        enabled: sp.getAttribute('enabled'),
      });
    }
    encodingProfiles.push(profile);
  }
  return encodingProfiles;
}

/* ── Asset parser (handles nested timelines recursively) ──────────── */

function parseAssets(projectEl) {
  const assets = [];
  const resEl = projectEl.querySelector(':scope > resources')
             || projectEl.querySelector(':scope > ressources');
  if (!resEl) return assets;

  for (const a of resEl.querySelectorAll(':scope > asset')) {
    const asset = {
      id: a.getAttribute('id'),
      extractableTypeName: a.getAttribute('extractable-type-name'),
      properties: parseGstStructure(a.getAttribute('properties') || ''),
      metadatas: parseGstStructure(a.getAttribute('metadatas') || ''),
      proxyId: a.getAttribute('proxy-id'),
      displayName: filenameFromUri(a.getAttribute('id')),
      subproject: null,
    };

    /* nested timeline: asset contains an inline project */
    if (asset.extractableTypeName === 'GESTimeline') {
      asset.subproject = parseProjectElement(a);
      asset.displayName = `[Nested] ${asset.displayName}`;
    }

    assets.push(asset);
  }
  return assets;
}

/* ── Timeline parser ──────────────────────────────────────────────── */

function parseTimeline(timelineEl, assetMap) {
  const tracks = [];
  for (const t of timelineEl.querySelectorAll(':scope > track')) {
    tracks.push({
      trackType: parseInt(t.getAttribute('track-type')) || 0,
      trackId: t.getAttribute('track-id'),
      caps: t.getAttribute('caps'),
      properties: parseGstStructure(t.getAttribute('properties') || ''),
      metadatas: parseGstStructure(t.getAttribute('metadatas') || ''),
    });
  }

  /* track-id → track-type lookup */
  const trackTypeById = new Map();
  for (const t of tracks) trackTypeById.set(t.trackId, t.trackType);

  const layers = [];
  for (const layerEl of timelineEl.querySelectorAll(':scope > layer')) {
    const layer = {
      priority: parseInt(layerEl.getAttribute('priority')) || 0,
      properties: parseGstStructure(layerEl.getAttribute('properties') || ''),
      metadatas: parseGstStructure(layerEl.getAttribute('metadatas') || ''),
      clips: [],
    };

    for (const clipEl of layerEl.querySelectorAll(':scope > clip')) {
      const clip = {
        id: clipEl.getAttribute('id'),
        assetId: clipEl.getAttribute('asset-id'),
        typeName: clipEl.getAttribute('type-name'),
        layerPriority: parseInt(clipEl.getAttribute('layer-priority')) || layer.priority,
        trackTypes: parseInt(clipEl.getAttribute('track-types')) || 0,
        startNs: Number(clipEl.getAttribute('start') || 0),
        durationNs: Number(clipEl.getAttribute('duration') || 0),
        inpointNs: Number(clipEl.getAttribute('inpoint') || 0),
        rate: parseFloat(clipEl.getAttribute('rate')) || 0,
        start: nsToSeconds(Number(clipEl.getAttribute('start') || 0)),
        duration: nsToSeconds(Number(clipEl.getAttribute('duration') || 0)),
        inpoint: nsToSeconds(Number(clipEl.getAttribute('inpoint') || 0)),
        properties: parseGstStructure(clipEl.getAttribute('properties') || ''),
        childrenProperties: parseGstStructure(clipEl.getAttribute('children-properties') || ''),
        metadatas: parseGstStructure(clipEl.getAttribute('metadatas') || ''),
        effects: [],
        sources: [],
        isNestedTimeline: false,
      };

      const asset = assetMap.get(clip.assetId);
      clip.displayName = getClipDisplayName(clip, asset);
      clip.isNestedTimeline = !!(asset && asset.subproject);

      for (const fx of clipEl.querySelectorAll(':scope > effect')) {
        clip.effects.push({
          assetId: fx.getAttribute('asset-id'),
          clipId: fx.getAttribute('clip-id'),
          typeName: fx.getAttribute('type-name'),
          trackType: parseInt(fx.getAttribute('track-type')) || 0,
          trackId: fx.getAttribute('track-id'),
          properties: parseGstStructure(fx.getAttribute('properties') || ''),
          childrenProperties: parseGstStructure(fx.getAttribute('children-properties') || ''),
          metadatas: parseGstStructure(fx.getAttribute('metadatas') || ''),
        });
      }

      for (const srcEl of clipEl.querySelectorAll(':scope > source')) {
        const source = {
          trackId: srcEl.getAttribute('track-id'),
          properties: parseGstStructure(srcEl.getAttribute('properties') || ''),
          childrenProperties: parseGstStructure(srcEl.getAttribute('children-properties') || ''),
          metadatas: parseGstStructure(srcEl.getAttribute('metadatas') || ''),
          bindings: [],
        };
        for (const b of srcEl.querySelectorAll(':scope > binding')) {
          source.bindings.push({
            type: b.getAttribute('type'),
            sourceType: b.getAttribute('source_type'),
            property: b.getAttribute('property'),
            mode: parseInt(b.getAttribute('mode')) || 0,
            trackId: b.getAttribute('track_id'),
            values: parseBindingValues(b.getAttribute('values')),
          });
        }
        clip.sources.push(source);
      }

      /* compute actual track types from sources */
      let srcTrackTypes = 0;
      for (const src of clip.sources) {
        const tt = src.properties['track-type']
                || trackTypeById.get(src.trackId)
                || 0;
        srcTrackTypes |= tt;
      }
      clip.sourceTrackTypes = srcTrackTypes;

      layer.clips.push(clip);
    }
    layers.push(layer);
  }
  layers.sort((a, b) => a.priority - b.priority);

  const groups = [];
  const groupsEl = timelineEl.querySelector(':scope > groups');
  if (groupsEl) {
    for (const gEl of groupsEl.querySelectorAll(':scope > group')) {
      const group = {
        id: gEl.getAttribute('id'),
        properties: parseGstStructure(gEl.getAttribute('properties') || ''),
        children: [],
      };
      for (const c of gEl.querySelectorAll(':scope > child')) {
        group.children.push({ id: c.getAttribute('id'), name: c.getAttribute('name') });
      }
      groups.push(group);
    }
  }

  const tlProps = parseGstStructure(timelineEl.getAttribute('properties') || '');
  const tlMeta = parseGstStructure(timelineEl.getAttribute('metadatas') || '');

  return {
    properties: tlProps,
    metadatas: tlMeta,
    tracks,
    layers,
    groups,
    durationSeconds: tlMeta.duration ? nsToSeconds(tlMeta.duration) : null,
  };
}

/* ── Project parser (reused for top-level and nested subprojects) ── */

function parseProjectElement(containerEl) {
  /*
   * containerEl can be:
   *  - a <ges> element (top-level — contains <project>)
   *  - an <asset> element with extractable-type-name="GESTimeline"
   *    which may contain either:
   *      a) inline children directly (<encoding-profiles>, <ressources>, <timeline>)
   *      b) a <ges><project>...</project></ges> wrapper (real GES files)
   */
  const projectEl = containerEl.querySelector(':scope > project')
                 || containerEl.querySelector(':scope > ges > project')
                 || containerEl;

  const encodingProfiles = parseEncodingProfiles(projectEl);
  const assets = parseAssets(projectEl);

  /*
   * Real GES files often have two <asset> elements with the same id:
   *   1. extractable-type-name="GESTimeline" — carries the inline subproject
   *   2. extractable-type-name="GESUriClip"  — referenced by clips
   * Build the map so the GESUriClip entry inherits the subproject from
   * its GESTimeline sibling.
   */
  const subprojectById = new Map();
  for (const a of assets) {
    if (a.subproject) subprojectById.set(a.id, a.subproject);
  }
  const assetMap = new Map();
  for (const a of assets) {
    if (assetMap.has(a.id)) {
      /* prefer the GESUriClip entry (the one clips actually reference) */
      const existing = assetMap.get(a.id);
      if (a.extractableTypeName !== 'GESTimeline') {
        a.subproject = a.subproject || subprojectById.get(a.id) || null;
        a.isNestedTimeline = !!a.subproject;
        if (a.subproject) a.displayName = `[Nested] ${filenameFromUri(a.id)}`;
        assetMap.set(a.id, a);
      }
    } else {
      a.subproject = a.subproject || subprojectById.get(a.id) || null;
      a.isNestedTimeline = !!a.subproject;
      if (a.subproject && a.extractableTypeName !== 'GESTimeline')
        a.displayName = `[Nested] ${filenameFromUri(a.id)}`;
      assetMap.set(a.id, a);
    }
  }

  const timelineEl = projectEl.querySelector(':scope > timeline');
  const timeline = timelineEl ? parseTimeline(timelineEl, assetMap) : null;

  return {
    properties: parseGstStructure(projectEl.getAttribute('properties') || ''),
    metadatas: parseGstStructure(projectEl.getAttribute('metadatas') || ''),
    encodingProfiles,
    assets,
    assetMap,
    timeline,
  };
}

/* ── Subproject propagation for deeply nested timelines ────────────── */

/*
 * GES files declare ALL nested timeline assets at the root <ressources>
 * level as extractable-type-name="GESTimeline".  Nested projects reference
 * them via GESUriClip entries with the same id.  After the recursive parse,
 * collect every known subproject globally and link them into nested projects
 * whose GESUriClip assets share the same id.
 */

function _collectSubprojects(project, map) {
  for (const [id, asset] of project.assetMap) {
    if (asset.subproject && !map.has(id)) map.set(id, asset.subproject);
  }
  for (const [, asset] of project.assetMap) {
    if (asset.subproject) _collectSubprojects(asset.subproject, map);
  }
}

function _applySubprojects(project, globalSubs) {
  let changed = false;
  for (const [id, asset] of project.assetMap) {
    if (!asset.subproject && globalSubs.has(id)) {
      asset.subproject = globalSubs.get(id);
      asset.isNestedTimeline = true;
      asset.displayName = `[Nested] ${filenameFromUri(id)}`;
      changed = true;
    }
  }
  if (changed && project.timeline) {
    for (const layer of project.timeline.layers) {
      for (const clip of layer.clips) {
        const asset = project.assetMap.get(clip.assetId);
        if (asset && asset.subproject && !clip.isNestedTimeline) {
          clip.isNestedTimeline = true;
          clip.displayName = getClipDisplayName(clip, asset);
        }
      }
    }
  }
  for (const [, asset] of project.assetMap) {
    if (asset.subproject) _applySubprojects(asset.subproject, globalSubs);
  }
}

/* ── Main entry point ─────────────────────────────────────────────── */

export function parseXges(xmlString) {
  const doc = new DOMParser().parseFromString(xmlString, 'text/xml');
  const err = doc.querySelector('parsererror');
  if (err) throw new Error('XML parse error: ' + err.textContent);

  const gesEl = doc.querySelector('ges');
  if (!gesEl) throw new Error('Missing <ges> root element');
  const projectEl = gesEl.querySelector('project');
  if (!projectEl) throw new Error('Missing <project> element');

  const project = parseProjectElement(gesEl);

  /* propagate subprojects from root-level GESTimeline assets into
     nested projects that reference them as GESUriClip */
  const globalSubs = new Map();
  _collectSubprojects(project, globalSubs);
  _applySubprojects(project, globalSubs);

  return {
    version: gesEl.getAttribute('version') || '0.1',
    project,
    timeline: project.timeline,
  };
}
