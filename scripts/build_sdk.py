#!/usr/bin/env python3
"""Build against a matching, already-built SDR++ SDK on any desktop OS.

SDK/astra-sdk.json names paths relative to SDK (or absolute paths):
  {"source":"SDRPlusPlus", "core":"lib/sdrpp_core.lib",
   "include":["include"], "libraries":["lib/volk.lib"],
   "cmake":["-DCMAKE_TOOLCHAIN_FILE=..."]}
Use source and core built for the same executable/compiler/architecture.
"""
import argparse
import json
import pathlib
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('sdk',type=pathlib.Path)
p.add_argument('--build',type=pathlib.Path,default=pathlib.Path('build-sdk'))
args=p.parse_args(); sdk=args.sdk.resolve()
manifest=json.loads((sdk/'astra-sdk.json').read_text())
def paths(key):
    return ';'.join(str((sdk/f).resolve()) for f in manifest.get(key,[]))
command=['cmake','-S',str(pathlib.Path(__file__).resolve().parents[1]),'-B',str(args.build),
    '-DCMAKE_BUILD_TYPE=Release','-DASTRA918_BUILD_MODULE=ON',
    '-DCMAKE_PREFIX_PATH='+str(sdk),
    '-DSDRPP_SOURCE_DIR='+str((sdk/manifest['source']).resolve()),
    '-DSDRPP_CORE_LIBRARY='+str((sdk/manifest['core']).resolve()),
    '-DSDRPP_SDK_INCLUDE_DIRS='+paths('include'),'-DSDRPP_SDK_LIBRARIES='+paths('libraries')]
subprocess.run(command+manifest.get('cmake',[]),check=True)
subprocess.run(['cmake','--build',str(args.build),'--config','Release','--parallel','4'],check=True)
subprocess.run(['ctest','--test-dir',str(args.build),'-C','Release','--output-on-failure'],check=True)
