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
     * Makes a cluster group visually interactive and clickable
     * @param {Element} cluster - Cluster group element
     * @param {string} elementName - GESTimeline element name (for display)
     * @param {string} xgesKey - Full key to look up xges content
     */
    makeClusterClickable(cluster, elementName, xgesKey) {
        const shape = cluster.querySelector('path, polygon, rect');

        cluster.style.cursor = 'pointer';

        const origStroke = shape ? (shape.getAttribute('stroke') || '') : '';
        const origStrokeWidth = shape ? (shape.getAttribute('stroke-width') || '') : '';

        cluster.addEventListener('mouseenter', () => {
            if (shape) {
                shape.setAttribute('stroke', '#00bcd4');
                shape.setAttribute('stroke-width', '3');
                shape.setAttribute('stroke-dasharray', '6,3');
            }

            if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                this.tooltipManager.tooltipEl.textContent = 'Click to open timeline viewer';
                const clusterRect = cluster.getBoundingClientRect();
                this.tooltipManager.tooltipEl.style.left = (clusterRect.left + window.scrollX + 20) + 'px';
                this.tooltipManager.tooltipEl.style.top = (clusterRect.top + window.scrollY - 30) + 'px';
                this.tooltipManager.tooltipEl.classList.remove('interactive');
                this.tooltipManager.tooltipEl.classList.add('show');
            }
        });

        cluster.addEventListener('mouseleave', () => {
            if (shape) {
                shape.setAttribute('stroke', origStroke);
                shape.setAttribute('stroke-width', origStrokeWidth);
                shape.removeAttribute('stroke-dasharray');
            }

            if (this.tooltipManager && !this.tooltipManager.isInteractive()) {
                this.tooltipManager.hideTooltip();
            }
        });

        cluster.addEventListener('click', (evt) => {
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
