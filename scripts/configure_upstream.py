#!/usr/bin/env python3
"""Configure a separate upstream checkout with optional modules disabled."""
import pathlib
import re
import subprocess
import sys
import os

source = pathlib.Path(sys.argv[1]).resolve()
patch = pathlib.Path(__file__).resolve().parent / "compat/sdrpp-resampler-predec.patch"
# Apply only the reviewed compatibility fix; accept an already applied patch.
reverse = subprocess.run(["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if reverse.returncode:
    subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)], check=True)
    subprocess.run(["git", "-C", str(source), "apply", str(patch)], check=True)
options = re.findall(r"option\((OPT_BUILD_\w+)", (source / "CMakeLists.txt").read_text())
# The source selector supplies I/Q only. Keep the radio demodulator and audio
# sink so the runnable SDR++ profile has LSB/USB/AM/FM receiver controls.
enabled = {"OPT_BUILD_RADIO", "OPT_BUILD_AUDIO_SINK"}
generator = os.environ.get("ASTRA_CMAKE_GENERATOR", "Ninja")
subprocess.run(["cmake", "-S", str(source), "-B", str(source / "build"), "-G", generator,
                "-DCMAKE_BUILD_TYPE=Release"] + [f"-D{name}={'ON' if name in enabled else 'OFF'}" for name in options] + sys.argv[2:], check=True)
