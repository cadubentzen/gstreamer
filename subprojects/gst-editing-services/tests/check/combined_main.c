/* GES combined test binary main
 *
 * When building with GST_CHECK_COMBINED_BUILD, all test suites register
 * themselves via constructors. This file provides the main() that
 * dispatches to the requested suite based on argv[1].
 */
#include <gst/check/gstcheck.h>

int
main (int argc, char **argv)
{
  return gst_check_combined_main (argc, argv);
}
