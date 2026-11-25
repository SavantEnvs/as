#!/usr/bin/env python3
"""Generate module configurations of upstream's bootloader and SOME/IP example with upstream's own generators.

usage: python3 -I mayhem/gen_cfg.py <repo root> <output dir>

tools/generator/__init__.py imports every generator, and some of them need packages that are
not installed (pycrc, ...), so the generator modules are copied into a private package under
<output dir> with an empty __init__.py and only the ones needed are imported. Each JSON file is
copied to <output dir>/<name>/ and generated into <output dir>/<name>/GEN/, as SCons does.
The SOME/IP generator renders jinja2 templates (python3-jinja2, installed by mayhem/Dockerfile).
"""
import glob
import importlib
import os
import shutil
import sys

root, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
pkg = os.path.join(out, "asgen")
os.makedirs(pkg, exist_ok=True)
if os.path.isdir(pkg):
    shutil.rmtree(pkg)
shutil.copytree(os.path.join(root, "tools", "generator"), pkg)  # incl. templates/ for SOME/IP
open(os.path.join(pkg, "__init__.py"), "w").close()
sys.path.insert(0, out)

# (output name, generator module, JSON in the repo)
BL = "app/bootloader/config/"
ADD = "app/examples/someip/add/config/server/"
JOBS = [
    ("LinIf", "LinIf", BL + "LinIf/LinIf.json"),
    ("LinTp", "LinTp", BL + "LinTp/LinTp.json"),
    ("Net", "Net", BL + "Net/Network.json"),
    ("NetMem", "MemCluster", BL + "Net/Mempool.json"),
    ("BLCanTp", "CanTp", BL + "CanTp/CanTp.json"),
    ("BLPduR", "PduR", BL + "PduR/PduR.json"),
    ("BLDcm", "Dcm", BL + "Dcm/Dcm.json"),
    ("Xcp", "Xcp", "app/app/config/Xcp/Xcp.json"),
    ("CanNm", "CanNm", "app/app/config/Com/CanNm.json"),
    ("SomeIpAdd", "Net", ADD + "Network.json"),
    ("SomeIpAdd", "MemCluster", ADD + "Mempool.json"),
]
for name, mod, rel in JOBS:
    d = os.path.join(out, name)
    os.makedirs(d, exist_ok=True)
    dst = os.path.join(d, os.path.basename(rel))
    shutil.copyfile(os.path.join(root, rel), dst)
    importlib.import_module("asgen." + mod).Gen(dst)
