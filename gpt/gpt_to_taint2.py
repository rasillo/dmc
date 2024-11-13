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

"""GPT func_info.json to taint policy converter

Usage:
  gpt_to_taint.py [<func_info_file>] [<out_file>]

Options:
  -h --help     Show this screen.
"""

import docopt
import json
import sys


def get_cat_code(d):
    if not isinstance(d, dict):
        return "Error: not a dict"
    is_source = d.get('is_source', False)
    is_sink = d.get('is_sink', False)
    ret = "none"
    if is_source and is_sink:
        ret = "SrcAndSink"
    elif is_source:
        ret = "Src"
    elif is_sink:
        ret = "Sink"
    if d.get("special_role") == "file":
        if d.get("direction") == "in":
            if (ret == "Src"):
                return "Error: 'direction' inconsistent with being a sink"
            ret = "FileSink"
        elif d.get("direction") == "out":
            if (ret == "Sink"):
                return "Error: 'direction' inconsistent with being a source"
            ret = "FileSrc"
        elif d.get("direction") == "inout":
            ret = "FileSrcAndSink"
        else:
            ret = "Error: bad 'direction' field"
    return ret


def do_fun(fun, fun_json):
    arglist = fun_json['arg list'].split(', ')
    if arglist == ['']:
        arglist = []

    ret = []

    for idx, arg in enumerate(arglist):
        assert arg in fun_json, f"arg {arg} not in {fun_json}"
        cur = get_cat_code(fun_json[arg])
        if cur.startswith("Error"):
            print("%s, func %r, param %r" % (cur, fun, arg))
            cur = "Error"
        ret.append(cur)

    ret.append("->")
    cur = get_cat_code(fun_json["return"])
    if cur.startswith("Error"):
        print("%s, func %r, param %r" % (cur, fun, "return"))
        cur = "Error"
    ret.append(cur)

    return ret
    

    

def convert(in_json, outf):
    #funs = [do_fun(fun_name, fun_json) for fun_name, fun_json in in_json.items()]
    outfile = outf
    for fun_name, fun_json in in_json.items():
        cats = do_fun(fun_name, fun_json) 
        # Skip funcs without any sources/sinks and also
        # skip funcs like 'ftell' that have only aux sinks.
        if (("Src" in " ".join(cats)) or ("Sink" in cats)):
            outfile.write(f"{fun_name:11}" + " " + " ".join(cats) + "\n")
        else:
            outfile.write(f"{fun_name:11}" + " -\n")
        #if sources:
        #    outfile.write("source " + fun_name + " " + " ".join(str(x) for x in sources) + "\n")
        #if sinks:
        #    outfile.write("sink " + fun_name + " " + " ".join(str(x) for x in sinks) + "\n")
        

if __name__ == '__main__':
    arguments = docopt.docopt(__doc__)
    #print(arguments)

    inputf = open(arguments['<func_info_file>'], 'r') if arguments['<func_info_file>'] is not None else sys.stdin
    outf = open(arguments['<out_file>'], 'w') if arguments['<out_file>'] is not None else sys.stdout
    
    convert(json.load(inputf), outf)
