#!/usr/bin/env python3
"""Run matching SDR++ binaries in an Astra-only profile (never normal settings)."""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdrpp-source',type=pathlib.Path,required=True)
p.add_argument('--sdrpp-build',type=pathlib.Path,required=True)
p.add_argument('--module',type=pathlib.Path)
p.add_argument('--simulator')
args = p.parse_args()
source, build = args.sdrpp_source.resolve(), args.sdrpp_build.resolve()
ext = '.dll' if sys.platform=='win32' else '.dylib' if sys.platform=='darwin' else '.so'
exe = build/('sdrpp.exe' if sys.platform=='win32' else 'sdrpp')
module = args.module or root/'build'/('astra918_source'+ext)
profile = root/'build/profile'
modules = profile/'modules'; modules.mkdir(parents=True,exist_ok=True)
for file in [module,build/'decoder_modules/radio'/('radio'+ext),build/'sink_modules/audio_sink'/('audio_sink'+ext)]:
    if not file.is_file(): p.error(f'Missing matching module: {file}; use --module for a multi-config build')
    shutil.copy2(file,modules/file.name)
config_path = profile/'config.json'
config = json.loads(config_path.read_text()) if config_path.exists() else {}
config.update(resourcesDirectory=str(source/'root/res'), modulesDirectory=str(modules),
              moduleInstances={'Astra':{'module':'astra918_source','enabled':True},
                               'Radio':{'module':'radio','enabled':True},
                               'Audio Sink':{'module':'audio_sink','enabled':True}},
              source='Astra918 Astra')
config.setdefault('frequency',14200000)
config_path.write_text(json.dumps(config,indent=2)+'\n')
env = dict(os.environ)
if args.simulator: env['ASTRA918_SIMULATOR']=args.simulator
raise SystemExit(subprocess.call([str(exe),'-r',str(profile)],env=env))
