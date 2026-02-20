/**
 * Timeline navigation functionality
 *
 * Detects GESTimeline cluster nodes in SVG pipeline views and makes them
 * clickable to open the timeline viewer with the corresponding .xges data.
 */

class TimelineNavigationManager {
    constructor(tooltipManager) {
        this.tooltipManager = tooltipManager;
    }

    /**
     * Finds GESTimeline clusters in the SVG and makes them clickable.
     * Detection is based on the cluster's label text: GESTimeline elements
     * have "GESTimeline" as their first text line (the GObject type name)
     * and the element name as the second line.
     *
     * @param {jQuery} $svg - jQuery object containing the SVG element
     * @param {string} pipelineTitle - Title of the current pipeline dot file,
     *   e.g. "0/0:00:07.852877646-pipeline-snapshot-gespipeline0".
     *   Used to derive the snapshot prefix so we look up xges files from
     *   the same snapshot.
     */
    setupTimelineNavigation($svg, pipelineTitle) {
        // Extract the snapshot prefix from the pipeline title
        // e.g. "0/0:00:07.852877646-pipeline-snapshot-gespipeline0"
        //   -> "0/0:00:07.852877646-pipeline-snapshot-"
        const prefixMatch = pipelineTitle.match(/^(.*pipeline-snapshot-)/);
        if (!prefixMatch) return;
        const snapshotPrefix = prefixMatch[1];

        $svg.find(".cluster").each((index, cluster) => {
            const $cluster = $(cluster);

            const elementName = this.getTimelineElementName($cluster);
            if (!elementName) return;

            // Build the full xges key: same prefix + element name
            const xgesKey = snapshotPrefix + elementName;

            // Only make clickable if xges data is available
            if (!this.hasXgesContent(xgesKey)) return;

            this.makeClusterClickable($cluster, elementName, xgesKey);
        });
    }

    /**
     * Checks whether a cluster represents a GESTimeline element and returns
     * its element name.  In Graphviz SVG output the cluster label is rendered
     * as <text> elements directly inside the cluster <g>.  The first text line
     * is the GObject type ("GESTimeline") and the second is the element name.
     * @param {jQuery} $cluster - jQuery cluster group element
     * @returns {string|null} Element name, or null if not a GESTimeline
     */
    getTimelineElementName($cluster) {
        const texts = $cluster.children("text");
        if (texts.length < 2) return null;

        const typeName = $(texts[0]).text().trim();
        if (typeName !== "GESTimeline") return null;

        const elementName = $(texts[1]).text().trim();
        return elementName || null;
    }

    /**
     * Checks if xges content is available for a given key
     * @param {string} xgesKey - Full xges key to check
     * @returns {boolean} True if xges content exists
     */
    hasXgesContent(xgesKey) {
        const topWindow = window.parent !== window ? window.parent : window;
        const getXges = topWindow.getXgesContent;
        return getXges && !!getXges(xgesKey);
    }

    /**
     * Makes a cluster group visually interactive and clickable
     * @param {jQuery} $cluster - jQuery cluster group element
     * @param {string} elementName - GESTimeline element name (for display)
     * @param {string} xgesKey - Full key to look up xges content
     */
    makeClusterClickable($cluster, elementName, xgesKey) {
        const $shapes = $cluster.children("path, polygon, rect").first();

        $cluster.css('cursor', 'pointer');

        // Store original stroke for hover restore
        const origStroke = $shapes.length ? $shapes.attr('stroke') || '' : '';
        const origStrokeWidth = $shapes.length ? $shapes.attr('stroke-width') || '' : '';

        $cluster.on('mouseenter.timeline-nav', () => {
            $shapes.attr('stroke', '#00bcd4');
            $shapes.attr('stroke-width', '3');
            $shapes.attr('stroke-dasharray', '6,3');

            if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                this.tooltipManager.$tooltip.text('Click to open timeline viewer');
                this.tooltipManager.$tooltip.css({
                    left: ($cluster.offset().left + 20) + 'px',
                    top: ($cluster.offset().top - 30) + 'px'
                }).removeClass('interactive').addClass('show');
            }
        });

        $cluster.on('mouseleave.timeline-nav', () => {
            $shapes.attr('stroke', origStroke);
            $shapes.attr('stroke-width', origStrokeWidth);
            $shapes.removeAttr('stroke-dasharray');

            if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                this.tooltipManager.hideTooltip();
            }
        });

        $cluster.on('click.timeline-nav', (evt) => {
            evt.preventDefault();
            evt.stopPropagation();

            if (this.tooltipManager) {
                this.tooltipManager.hideTooltip();
            }

            this.navigateToTimeline(elementName, xgesKey);
        });
    }

    /**
     * Navigates to the timeline viewer with xges content
     * @param {string} elementName - Element name (for display/URL)
     * @param {string} xgesKey - Full key to look up xges content
     */
    navigateToTimeline(elementName, xgesKey) {
        // Get the xges content from the parent window (index.html)
        const topWindow = window.parent !== window ? window.parent : window;
        const getXges = topWindow.getXgesContent;

        if (!getXges) {
            console.warn('getXgesContent not available on parent window');
            return;
        }

        const xgesContent = getXges(xgesKey);
        if (!xgesContent) {
            console.warn(`No xges content found for key: ${xgesKey}`);
            return;
        }

        // Store in sessionStorage and navigate
        const storageKey = `xges-${elementName}`;
        sessionStorage.setItem(storageKey, xgesContent);

        const baseUrl = topWindow.location.origin;
        topWindow.location.href = `${baseUrl}/timeline.html?timeline=${encodeURIComponent(elementName)}`;
    }
}

// Export for module usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = TimelineNavigationManager;
} else {
    window.TimelineNavigationManager = TimelineNavigationManager;
}
