#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os
import sys

config_map = {}

def map_config(source_dir, site_config):
    global config_map
    source_dir = os.path.realpath(source_dir)
    source_dir = os.path.normcase(source_dir)
    site_config = os.path.normpath(site_config)
    config_map[source_dir] = site_config

# Variables configured at build time.
llvm_source_root = "C:/Users/Krystian/src/llvm-project/llvm"
llvm_obj_root = "C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug"

# Make sure we can find the lit package.
sys.path.insert(0, os.path.join(llvm_source_root, 'utils', 'lit'))

# Set up some builtin parameters, so that by default the LLVM test suite
# configuration file knows how to find the object tree.
builtin_parameters = { 'build_mode' : "." }


map_config('C:/Users/Krystian/src/llvm-project/llvm/utils/lit/tests/lit.cfg', 'C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/utils/lit/lit.site.cfg')
map_config('C:/Users/Krystian/src/llvm-project/llvm/test/lit.cfg.py', 'C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/test/lit.site.cfg.py')
map_config('C:/Users/Krystian/src/llvm-project/llvm/test/Unit/lit.cfg.py', 'C:/Users/Krystian/src/llvm-project/llvm/out/build/x64-Debug/test/Unit/lit.site.cfg.py')

builtin_parameters['config_map'] = config_map

if __name__=='__main__':
    from lit.main import main
    main(builtin_parameters)
