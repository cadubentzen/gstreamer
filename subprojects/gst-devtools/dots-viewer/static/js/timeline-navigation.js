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
     *
     * @param {Element} svg - SVG element
     * @param {string} pipelineTitle - Title of the current pipeline dot file
     */
    setupTimelineNavigation(svg, pipelineTitle) {
        const prefixMatch = pipelineTitle.match(/^(.*pipeline-snapshot-)/);
        if (!prefixMatch) return;
        const snapshotPrefix = prefixMatch[1];

        svg.querySelectorAll('.cluster').forEach(cluster => {
            const elementName = this.getTimelineElementName(cluster);
            if (!elementName) return;

            const xgesKey = snapshotPrefix + elementName;

            if (!this.hasXgesContent(xgesKey)) return;

            this.makeClusterClickable(cluster, elementName, xgesKey);
        });
    }

    /**
     * Checks whether a cluster represents a GESTimeline element and returns
     * its element name.
     * @param {Element} cluster - Cluster group element
     * @returns {string|null} Element name, or null if not a GESTimeline
     */
    getTimelineElementName(cluster) {
        const texts = Array.from(cluster.children).filter(el => el.tagName === 'text');
        if (texts.length < 2) return null;

        const typeName = texts[0].textContent.trim();
        if (typeName !== "GESTimeline") return null;

        const elementName = texts[1].textContent.trim();
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
     * Makes the GESTimeline label texts inside a cluster into a visible link
     * that opens the timeline viewer. Only the type/name text labels are
     * interactive — not the entire cluster background — so the intent is clear.
     * @param {Element} cluster - Cluster group element
     * @param {string} elementName - GESTimeline element name (for display)
     * @param {string} xgesKey - Full key to look up xges content
     */
    makeClusterClickable(cluster, elementName, xgesKey) {
        /* The cluster's first two direct <text> children are the type label
         * ("GESTimeline") and the instance name. Only those are interactive —
         * any further texts (properties, state) are left alone. */
        const allTexts = Array.from(cluster.children).filter(el => el.tagName === 'text');
        const labels = allTexts.slice(0, 2);
        if (!labels.length) return;

        labels.forEach(label => {
            label.style.cursor = 'pointer';
            label.style.fill = '#1a73e8';
            label.style.textDecoration = 'underline';
            label.style.fontWeight = 'bold';

            label.addEventListener('mouseenter', (evt) => {
                label.style.fill = '#0d47a1';
                if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                    this.tooltipManager.tooltipEl.textContent = `Open GES timeline viewer for "${elementName}"`;
                    const rect = label.getBoundingClientRect();
                    this.tooltipManager.tooltipEl.style.left = (rect.left + window.scrollX) + 'px';
                    this.tooltipManager.tooltipEl.style.top = (rect.bottom + window.scrollY + 6) + 'px';
                    this.tooltipManager.tooltipEl.classList.remove('interactive');
                    this.tooltipManager.tooltipEl.classList.add('show');
                }
            });

            label.addEventListener('mouseleave', () => {
                label.style.fill = '#1a73e8';
                if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                    this.tooltipManager.hideTooltip();
                }
            });

            label.addEventListener('click', (evt) => {
                evt.preventDefault();
                evt.stopPropagation();
                if (this.tooltipManager) {
                    this.tooltipManager.hideTooltip();
                }
                this.navigateToTimeline(elementName, xgesKey);
            });
        });
    }

    /**
     * Navigates to the timeline viewer with xges content
     * @param {string} elementName - Element name (for display/URL)
     * @param {string} xgesKey - Full key to look up xges content
     */
    navigateToTimeline(elementName, xgesKey) {
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
