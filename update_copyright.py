#!/usr/bin/env python

# <legal>
# DMC Tool
# Copyright 2023 Carnegie Mellon University.
# 
# NO WARRANTY. THIS CARNEGIE MELLON UNIVERSITY AND SOFTWARE ENGINEERING INSTITUTE
# MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO
# WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED, AS TO ANY MATTER
# INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR PURPOSE OR
# MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF THE MATERIAL.
# CARNEGIE MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
# TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
# 
# Released under a MIT (SEI)-style license, please see License.txt or contact
# permission@sei.cmu.edu for full terms.
# 
# [DISTRIBUTION STATEMENT A] This material has been approved for public release
# and unlimited distribution.  Please see Copyright notice for non-US Government
# use and distribution.
# 
# Carnegie Mellon (R) and CERT (R) are registered in the U.S. Patent and Trademark
# Office by Carnegie Mellon University.
# 
# This Software includes and/or makes use of the following Third-Party Software
# subject to its own license:
# 1. Phasar
#     (https://github.com/secure-software-engineering/phasar/blob/development/LICENSE.txt)
#     Copyright 2017 - 2023 Philipp Schubert and others.  
# 2. LLVM (https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT) 
#     Copyright 2003 - 2022 LLVM Team.
# 
# DM23-0532
# </legal>

# How to use:
# 1. Make sure update_copyright.py is up-to-date, and fix other decompilation bugs
# 2. Run update_copyright.py -w , update any file with <legal> tags that should have them.
# 3. Run update_copyright.py, verify that README looks good (perhaps view it as a pdf or html)
# 4. Build container. test container
# 5. Build & release tarball

import fnmatch
import json
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib
import pdb
stop = pdb.set_trace

VERSION_MAGIC_STR = '{{BASP_VERSION}}'

COPYRIGHT_BLACKLIST = [
    "./.git",
    "./.ccls-cache",
    "./.pytest_cache",
    "./gpt/out",
    "./License.txt",
]

COPYRIGHT_FILENAME_MAP = {
    ".gitignore": r'# \1',
    "Vagrantfile": r'# \1',
    "Dockerfile": r'# \1',
    "Makefile": r'# \1',
    "README": r'\1',
    "CMakeLists.txt": r'# \1',
    ".gitkeep": None,
    "ABOUT": None,
}
COPYRIGHT_EXTENSION_MAP = {
    ".sh": r'# \1',
    ".py": r'# \1',
    ".rb": r'# \1',
    ".mk": r'# \1',
    ".properties": r'# \1',
    ".yml": r'# \1',
    ".yaml": r'# \1',

    ".java": r'// \1',
    ".c": r'// \1',
    ".cpp": r'// \1',
    ".h": r'// \1',
    ".C": r'// \1',
    ".H": r'// \1',
    ".js": r'// \1',

    ".erb": r'<!-- \1 -->',
    ".sql": r'-- \1',
    ".ll": r'; \1',

    # These file extensions need no comments and can be ignored
    ".md":  r'\1  ',
    ".html": r'<p>\1',
    ".txt": r'\1',

    # These file extensions are binary and can be ignored
    ".jpg": None,
    ".png": None,
    ".gif": None,
    ".pdf": None,
    ".docx": None,
    ".xlsx": None,
    ".pptx": None,
    ".pyc": None,
    ".zip": None,
    ".class": None,
    ".so": None,
    ".o": None,
    ".elf": None,

    # These file extensions have no commenting mechanism so can be ignored
    ".json": None,
    ".csv": None,
    ".tsv": None,
}

VERSION_FIX_FILES = ['License.txt']


def read_configuration(cfg_filename):
    '''
        Reads the ABOUT file, parses it as json, and returns the parsed object.
    '''
    with open(cfg_filename) as f:
        return json.load(f)


def update_copyright(cfg):
    '''
        Updating copyright info in each file that contains it
    '''
    warnings = args.warnings
    for dirpath, _, files in os.walk('.'):
        if any([dirpath.startswith(x) for x in COPYRIGHT_BLACKLIST]):
            continue

        for filename in files:
            full_path = os.path.join(dirpath, filename)
            ext = os.path.splitext(full_path)[-1]
            regex = None
            if dirpath == "./bin":
                regex = COPYRIGHT_EXTENSION_MAP[".sh"]
            elif filename in COPYRIGHT_FILENAME_MAP:
                regex = COPYRIGHT_FILENAME_MAP[filename]
            elif ext in COPYRIGHT_EXTENSION_MAP:
                regex = COPYRIGHT_EXTENSION_MAP[ext]
            else:
                if warnings:
                    print("WARNING: Not checking copyright in " + full_path)

            if not regex:
                continue

            with io.open(full_path, 'r+', encoding="utf-8") as fp:
                contents = fp.read()
                # This matches all lines where the <legal> tags
                # live. So anything on these lines outside the <legal>
                # tags will get erased. IOW this presumes that the
                # lines with <legal> tags have nothing else of
                # importance.
                match = re.search(r'(?im)^.*?<legal>(.|\n)*?</legal>.*?$',
                                  contents)
                if not match:
                    if warnings:
                        print("WARNING: No copyright for " + full_path)
                    continue
                if not warnings:
                    new_contents = contents
                    cui_line = ""
                    if args.cui:
                        # Assumption: If the CUI footer is already present, then so is the CUI header.
                        if not re.search("(\n|\r)[^A-Za-z0-9]*CUI[^A-Za-z0-9]*(\n|\r)*$", new_contents):
                            cui_line = "CUI\n\n"
                            if not new_contents.endswith("\n"):
                                new_contents += "\n"
                            new_contents += "\n" + re.sub(r'(?m)(^.*$)', regex, "CUI") + "\n"
                    new_legal = cui_line + "<legal>\n" + "\n".join(cfg['legal']) + "\n</legal>"
                    prot_new_legal = re.sub(r'(?m)(^.*$)', regex, new_legal)
                    new_contents = (new_contents[:match.start(0)] +
                                    prot_new_legal +
                                    new_contents[match.end(0):])
                    fp.seek(0)
                    fp.write(new_contents)
                    fp.truncate()

    return 'DONE'


def adjust_version_numbers(cfg):
    '''
        Replacing version numbers with the version from the ABOUT file.
    '''
    for filename in VERSION_FIX_FILES:
        with io.open(filename, 'r', encoding="utf-8") as input_f:
            new_str = input_f.read().replace(VERSION_MAGIC_STR, cfg['version'])
        with io.open(filename, 'w', encoding="utf-8") as output_f:
            output_f.write(new_str)
    return 'DONE'


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description="Script to update copyright info")
    p.add_argument('-w', '--warnings', action='store_true',
                   help="Print warnings, but don't update legal info in source files")
    cui_group = p.add_mutually_exclusive_group()
    cui_group.add_argument('--cui', action='store_true', help="Adds CUI footer")
    cui_group.add_argument('--no-cui', action='store_true', help="Don't add CUI footer")
    args = p.parse_args()

    if (not args.cui) and (not args.no_cui) and (not args.warnings):
        p.print_usage()
        print("Error: exactly one of {'--cui', '--no-cui'} must be specified.")
        exit()

    cfg = read_configuration('ABOUT')

    # Setup the local directory for packaging
    update_copyright(cfg)
    adjust_version_numbers(cfg)
