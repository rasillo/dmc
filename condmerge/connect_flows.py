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

import os, sys
import re, json
import argparse
import pdb
from collections import OrderedDict, defaultdict

stop = pdb.set_trace

def read_whole_file(filename, mode_mod=""):
    with open(filename, 'r'+mode_mod) as the_file:
        return the_file.read()

def cleanup_json(s):
    s = re.sub("(^|\n)#[^\n]*", "\n", s)
    s = re.sub("],([\n ]*\\])", "]\\1", s)
    s = re.sub("},([\n ]*\\])", "}\\1", s)
    return s

def read_json_file(filename):
    if filename.endswith(".gz"):
        fn_open = gzip.open
    else:
        fn_open = open
    with fn_open(filename, 'rt') as f:
        try:
            data = json.load(f, object_pairs_hook=OrderedDict)
        except Exception as e:
            raise JSONFileException(
                "Error reading JSON file: {}: {}" .format(filename, e)) from e
    return data


def parse_args():
    parser = argparse.ArgumentParser(description='Connects auxiliary flows')
    parser.add_argument("input_file", nargs="?", default="-", type=str, help="Raw flows, from the LLVM pass")
    parser.add_argument('-o', type=str, dest="out_file", help="Output file")
    parser.add_argument('-b', "--base-dir", type=str, dest="base_dir",
        help="Base directory of the project")
    global cmdline_args
    cmdline_args = parser.parse_args()
    return cmdline_args

def remove_prefix(s, prefix):
    if s.startswith(prefix):
        return s[len(prefix):]
    else:
        return s

def freeze_dict_to_tuple(d, skip_keys):
    if isinstance(d, (str, int)):
        return d
    if isinstance(d, list):
        return tuple([freeze_dict_to_tuple(x, skip_keys) for x in d])
    assert(isinstance(d, dict))
    return tuple((k, freeze_dict_to_tuple(v, skip_keys)) for (k,v) in d.items() if k not in skip_keys)

sink_aux_sources = {}
scrink_ids = {}
cur_scrink_id = 0

def get_scrink_id(froz_scrink):
    global scrink_ids
    global cur_scrink_id
    ret_id = scrink_ids.get(froz_scrink)
    if ret_id:
        return ret_id
    cur_scrink_id += 1
    scrink_ids[froz_scrink] = cur_scrink_id
    return cur_scrink_id

def remove_dups(L):
    hit = set()
    for item in L:
        r = item
        if isinstance(item, dict):
            r = freeze_dict_to_tuple(item, [])
        if r not in hit:
            hit.add(r)
            yield item


def main():
    #input_file = sys.argv[1]
    parse_args()
    global cmdline_args
    global base_dir
    base_dir = cmdline_args.base_dir or ""
    if base_dir and not(base_dir.endswith("/")):
        base_dir += "/"
    input_file = cmdline_args.input_file
    if input_file == "-":
        text = sys.stdin.read()
    else:
        text = read_whole_file(input_file)
    pattern = re.compile(r'\n<flows>(.*?)\n</flows>', re.DOTALL)
    matches = pattern.findall(text)
    flows = []
    for match in matches:
        match = cleanup_json(match.lstrip())
        try:
            data = json.loads(match, object_pairs_hook=OrderedDict)
            flows.extend(data)
        except Exception as e:
            print("Error reading JSON data! %r" % e)
        #print(match)
    outf = sys.stdout
    outf.write("[\n")
    #print(json.dumps(flows, indent=2))
    global sink_aux_sources
    for flow in flows:
        sink = flow["sink"]
        froz_sink = freeze_dict_to_tuple(sink, skip_keys=["arg", "aux"])
        aux_type = sink["aux"]
        if aux_type != "main":
            #print(froz_sink)
            sink_aux_sources.setdefault(froz_sink, {})
            sink_aux_sources[froz_sink].setdefault(aux_type, [])
            sink_aux_sources[froz_sink][aux_type].extend(flow["sources"])
    for froz_sink in list(sink_aux_sources.keys()):
        for (aux_type, sources) in list(sink_aux_sources[froz_sink].items()):
            sink_aux_sources[froz_sink][aux_type] = list(remove_dups(sources))
    is_first = True
    seen_flows = set()
    for flow in flows:
        if flow["sink"]["aux"] != "main":
            continue
        froz_flow = freeze_dict_to_tuple(flow, skip_keys=[])
        if froz_flow in seen_flows:
            continue
        seen_flows.add(froz_flow)
        if (is_first):
            is_first = False
        else:
            outf.write(",\n\n")
        dump_flow(flow, outf, skip_aux=True)
    outf.write("\n]\n")

def dump_flow(flow, outf, skip_aux):
    if skip_aux and flow["sink"]["aux"] != "main":
        return
    outf.write('{"sink": ')
    dump_src_or_sink(flow["sink"], outf, indent=4, wrapper=[], seen=set())
    outf.write(',\n "srcs": [')
    is_first = True
    for src in flow["sources"]:
        if skip_aux and src["aux"] != "main":
            continue
        if is_first:
            is_first = False
        else:
            outf.write(",\n  ")
        dump_src_or_sink(src, outf, indent=4, wrapper=[], seen=set())
    outf.write(']}')

def dump_src_or_sink(src, outf, indent, *, wrapper, seen):
    global sink_aux_sources
    aux_filename = src.get("aux_file")
    if aux_filename:
        outf.write('{"filename":"%s"} ' % (aux_filename,))
        return
    froz_src = freeze_dict_to_tuple(src, skip_keys=[])
    if froz_src in seen:
        outf.write("{\"repeat\":%d}" % get_scrink_id(froz_src))
        return
    seen.add(froz_src)
    funcname = src.get("Func") or src["func"]
    callsite = src["callsite"]
    outf.write('{"func":"%s", ' % (funcname,))
    #outf.write('"aux":"%s", ' % (src["aux"],))
    outf.write('"callsite":["%s","%s",%d,%d]' % (
        remove_prefix(callsite[0], base_dir),  callsite[1], callsite[2], callsite[3]))
    outf.write(', "id":%d' % get_scrink_id(froz_src))
    if src.get("FILE*"):
        outf.write(', "FILE*":"%s"' % (src.get("FILE*"),))
    wrapped = src.get("wrapped")
    if wrapped:
        outf.write(',\n' + (" " * indent) + '"wrapped":')
        froz_wrapper = freeze_dict_to_tuple(src, skip_keys=["arg", "aux", "wrapped"])
        dump_src_or_sink(wrapped, outf, indent+2, wrapper=wrapper+[froz_wrapper], seen=seen)
    froz_src = freeze_dict_to_tuple(src, skip_keys=["arg", "aux"])
    #if funcname == "fopen":
    #    stop()
    keys = [froz_src]
    if wrapper:
        keys.append(wrapper[0] + (("wrapped", froz_src),))
    for link_key in keys:
        if link_key in sink_aux_sources:
            for aux_type in sink_aux_sources[link_key]:
                outf.write(',\n' + (" " * indent) + f'"aux {aux_type}": [')
                is_first = True
                for aux_src in sink_aux_sources[link_key][aux_type]:
                    if (is_first):
                        is_first = False
                    else:
                        outf.write(",\n" + (" " * indent))
                    dump_src_or_sink(aux_src, outf, indent+2, wrapper=wrapper, seen=seen)
                outf.write("]")
                #outf.write("\n" + (" " * indent) + "]\n")
            
    outf.write("}")

main()
