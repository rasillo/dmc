#!/usr/bin/python3

# Run this script on the files produced by ask_gpt.py

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

import os, sys, re, json
from collections import OrderedDict, defaultdict
import pdb, pprint
stop = pdb.set_trace


def read_whole_file(filename):
    with open(filename, 'r') as the_file:
        return the_file.read()

def parse_ans(gpt_ans, filename):
    if re.search('(^|\n){[ \n]*"error": "unknown function"[ \n]*}(\n|$)', gpt_ans):
        return None
    m = re.search("```json(.*?return.*?)```", gpt_ans, re.DOTALL | re.IGNORECASE)
    if not m:
        m = re.search("```\n([{].*?return.*?)```", gpt_ans, re.DOTALL | re.IGNORECASE)
    if not m:
        print("Error finding JSON answer in file %s!" % filename)
        return None
    try:
        json_answer = m.group(1)
        json_answer = re.sub("//.*\n", "\n", json_answer)
        json_answer = re.sub("],([\n ]+\\])", "]\\1", json_answer)
        json_answer = re.sub(',([\n ]+})', '\\1', json_answer)
        return json.loads(json_answer, object_pairs_hook=OrderedDict)
    except:
        print("Error parsing JSON answer in file %s!" % filename)
        return None

check_proto = False
func_info_map = {}
for filename in sys.argv[1:]:
    if filename == "--check-proto":
        check_proto = True
        continue
    gpt_ans = read_whole_file(filename)
    json_ans = parse_ans(gpt_ans, filename)
    if json_ans != None:
        if json_ans.get("error") == "unknown function":
            continue
        func_name = os.path.splitext(os.path.basename(filename))[0]
        for (param, attribs) in json_ans.items():
            if not isinstance(attribs, dict):
                if param != "arg order":
                    print("Error in %r, param %r" % (filename, param))
                continue
            if param == "return" and ("direction" not in attribs):
                attribs["direction"] = "out"
            if attribs.get("sensitive") == "true":
                if attribs.get("to_external") == "true":
                    if attribs.get("direction") == "out":
                        sys.stderr.write("Error: (%s, %s)\n" % (func_name, param))
                    else:
                        json_ans[param]["is_sink"] = "true"
                if attribs.get("from_external") == "true":
                    if attribs.get("direction") == "in":
                        sys.stderr.write("Error: (%s, %s)\n" % (func_name, param))
                    else:
                        json_ans[param]["is_source"] = "true"
        keys = list(json_ans.keys())
        args = [x for x in keys[1:] if x not in  ["return", "arg order"]]
        json_ans["arg list"] = ", ".join(args)
        if check_proto:
            import print_proto
            prototype = print_proto.prototype_of_func(func_name)
        else:
            prototype = None
        is_variadic = False
        if not prototype:
            if check_proto:
                sys.stderr.write("Function %r not found in man pages!\n\n" % func_name)
        else:
            if re.match("^.*[.][.][.][^,]*$", prototype, re.DOTALL):
                is_variadic = True
            m = re.match(".*" + ".*,.*".join(args) + ".*", prototype, re.DOTALL)
            if not m:
                sys.stderr.write(prototype + "\n" + str(args) + "\n\n")
        if is_variadic:
            json_ans["is variadic"] = True
        func_info_map[func_name] = json_ans

print(json.dumps(func_info_map, indent=2) + "\n")
