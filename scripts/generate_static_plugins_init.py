#!/usr/bin/env python3
"""Generate static-plugins-init.c for test builds.

Produces a minimal gst_init_static_plugins() that registers the given
plugins via GST_PLUGIN_STATIC_DECLARE / GST_PLUGIN_STATIC_REGISTER.
Unlike generate_init_static_plugins.py this does NOT require the
gst-full config.h macros and is intended for linking into test
executables in static (Emscripten) builds.
"""

import argparse

TEMPLATE = """\
/* Auto-generated — do not edit */
#include <gst/gstplugin.h>

{declarations}

void
gst_init_static_plugins (void)
{{
{registrations}
}}
"""

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-o', '--output', required=True)
    parser.add_argument('-p', '--plugins', required=True,
                        help='Semicolon-separated plugin names')
    args = parser.parse_args()

    names = [n.strip() for n in args.plugins.split(';') if n.strip()]

    decls = '\n'.join(
        f'GST_PLUGIN_STATIC_DECLARE ({n});' for n in names)
    regs = '\n'.join(
        f'  GST_PLUGIN_STATIC_REGISTER ({n});' for n in names)

    with open(args.output, 'w') as f:
        f.write(TEMPLATE.format(declarations=decls, registrations=regs))


if __name__ == '__main__':
    main()
