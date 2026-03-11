/**
 * WASM bundle entry point
 *
 * Bundles d3, d3-graphviz, @hpcc-js/wasm, Fuse, gststructure utilities,
 * all dots-viewer JS modules, and the WASM dots viewer into a single
 * self-contained bundle for use in GStreamer WASM applications.
 *
 * Output: static/dist/gst-dots-wasm-bundle.js
 */

// Core dependencies
import * as d3 from 'd3';
import 'd3-graphviz';
import FuseModule from 'fuse.js';
import { Graphviz } from '@hpcc-js/wasm/graphviz';
import { GstCaps, GstStructure, unwrapValue, valueToStringBare } from 'gststructure';

window.d3 = d3;
window.Fuse = FuseModule;
window.Graphviz = Graphviz;
window.GstCaps = GstCaps;
window.GstStructure = GstStructure;
window.unwrapValue = unwrapValue;
window.valueToStringBare = valueToStringBare;

// Dots-viewer modules — webpack intercepts module.exports so the
// window.X fallback in each file never runs.  Assign explicitly.
window.TooltipManager = require('./static/js/tooltip.js');
window.PipelineNavigationManager = require('./static/js/pipeline-navigation.js');
window.TimelineNavigationManager = require('./static/js/timeline-navigation.js');
window.TextEllipsizerManager = require('./static/js/text-ellipsizer.js');
window.SvgOverlayManager = require('./static/js/svg-overlay-manager.js');

// WASM dots viewer (registers globalThis.__gstDotsWasm)
require('./static/js/gst-dots-wasm.js');
