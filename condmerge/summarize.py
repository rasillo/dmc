#!/usr/bin/env python3

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

from subprocess import Popen, PIPE, STDOUT
from pathlib import Path
import argparse
import json
import sys
import re
import os
    

def create_parser():
    parser = argparse.ArgumentParser(
        prog='Summarize',
        description='Summerize the DMC json output'
    )
    parser.add_argument('fname', metavar='dmc_json_output', nargs='?', default='-', help='The file-like object containing DMC json-formatted output')
    parser.add_argument('-v', '--verbose', action='count', default=0)
    parser.add_argument('-a', '--aux-file', default=None, help='The whitelist of displayed axuiliary functions')
    return parser

def setup():
    p = create_parser()
    a = p.parse_args()
    if a.fname != '-':
        fname = Path(a.fname)
        if not fname.exists() or not fname.is_file():
            print(f'{fname} is not a file')
            return None
        with open(fname, 'r') as fp:
            data = json.load(fp)
    else:
        data = json.load(sys.stdin) # should be `for line in sys.stdin...` to keep accepting input
    return data, a.verbose, a.aux_file

def find_aux(aux_list, aux):
    for a in aux:
        # print(f"{tab}    aux: {aux}")
        if 'func' in a:
            aux_list.add(a['func'])
        
        if (sub_aux := a.get('aux file')):
            find_aux(aux_list, sub_aux)

def choose_whitelist(aux_list):
    with open('__auxfn_kconfig', 'w') as fp:
        fp.write("menu \"Whitelist Analyzed Auxiliary Functions\"\n\n")
        for fn in aux_list:
            fp.write(f"config {fn.upper()}\n    bool \"{fn}\"\n    default y\n\n")
        fp.write("endmenu\n")

    # os.execve('/usr/bin/kconfig-mconf', ['__auxfn_kconfig'], {})
    # os.execve('/usr/bin/kconfig-mconf', ['/usr/bin/kconfig-mconf', '-h'], {})
    # os.system(f'kconfig-conf __auxfn_kconfig')

def read_new_whitelist():
    aux_list = set()
    with open('.config', 'r') as fp:
        for line in fp.readlines():
            print(line)
            # add more .config parsing here
    return aux_list

def filter_functions(data, aux_file):
    aux_list = set()
    if aux_file is None:
        for x in data:
            sink = x['sink']
            if (aux := sink.get('aux file')):
                find_aux(aux_list, aux)
            for src in x['srcs']:
                if (aux := src.get('aux file')):
                    find_aux(aux_list, aux)
    else:
        fname = Path(aux_file)
        if not fname.exists() or not fname.is_file():
            print(f"Whitelist file {fname} not found")
        else:
            with open(fname.absolute(), 'r') as fp:
                for line in fp.readlines():
                    print(line.strip())
                    aux_list.add(line.strip())

    # for x in data:
    #     sink = x['sink']
    #     if (aux := sink.get('aux file')):
    #         find_aux(aux_list, aux)
    #     for src in x['srcs']:
    #         if (aux := src.get('aux file')):
    #             find_aux(aux_list, aux)

    # print(f"aux_list: {aux_list}")
    # choose_whitelist(aux_list)
    # aux_list = read_new_whitelist()
    return aux_list

def function_str(fn):
    # s.append(f"\t     src: {src['func']} (L:{src['callsite'][2]}, C:{src['callsite'][3]})\n")
    return f"{fn['func']} [{fn['callsite'][0]}|{fn['callsite'][2]},{fn['callsite'][3]}]{' on '+fn['FILE*'] if 'FILE*' in fn else ''}"

def sum_aux(s, aux_fn, whitelist, tab=''):
    for aux in aux_fn:
        # print(f"{tab}    aux: {aux}")
        if 'func' in aux and aux['func'] in whitelist:
            s.append(f"{tab}    with: {function_str(aux)}\n")
            # if aux['func'] in whitelist: 
            #     s.append("in whitelist\n")
        elif 'filename' in aux:
            fname = f" on \"{aux['filename']}\"\n"
            if " on " in s[-1]:
                fname = f", on \"{aux['filename']}\"\n"
            s[-1] = s[-1][:-1] + fname # s[-1][:-1] to remove the new line at the end. s[-1] works bc of the dfs nature of the algorithm
        
        if (sub := aux.get('aux file')):
            sum_aux(s, sub, whitelist, tab+"    ")

def summarize(data, whitelist, verbose):
    s = []
    s.append('Found the following tainted information flows:\n\n')
    if verbose == 0:
        for x in data:
            sink = x['sink']
            # called [] with data from call to []
            s.append(f"Called {function_str(sink)} ")
            if len(x['srcs']) == 0:
                s.append('\n')
            elif len(x['srcs']) == 1:
                s.append(f"with potentially tainted data from call to {function_str(x['srcs'][0])}\n")
            elif len(x['srcs']) > 1:
                s.append('with potentially tainted data from calls to ')
                first = True
                for src in x['srcs']:
                    if first:
                        s.append(function_str(src))
                        first = False
                    else:
                        s.append(f", {function_str(src)}")
                s.append('\n')
            
    elif verbose == 1:
        for x in data:
            sink = x['sink']
            # print(f"sink: {sink}")
            # if len(s) > 1:
            #     s.append("----------\n")
            s.append(f"\nsink: {function_str(sink)}\n")
            if (aux := sink.get('aux file')):
                # print(f"aux: {aux}")
                sum_aux(s, aux, whitelist, "      ")
            for src in x['srcs']:
                # print(f'             {src}')
                s.append(f"      source: {function_str(src)}\n")
                if (aux := src.get('aux file')):
                    sum_aux(s, aux, whitelist, "      ")

    return ''.join(s)

if __name__ == "__main__":
    data,verbose,aux_file = setup()
    if data is None:
        exit(-1)
    whitelist = filter_functions(data, aux_file)
    s = summarize(data, whitelist, verbose)
    print(s)
