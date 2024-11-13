#!/usr/bin/python3

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

import os, sys, time
import openai
import pdb
import re, json
import argparse
from collections import OrderedDict, defaultdict
stop = pdb.set_trace

if not os.environ.get("OPENAI_API_KEY"):
    print("Error: Missing environment variable.  " + API_key_help)
    sys.exit(1)

openai_client = None
if openai_client is None:
    from openai import OpenAI
    openai_client = OpenAI(
        api_key=os.environ.get("OPENAI_API_KEY"),
    )



prompt = (
"""
Consider a static taint analysis (for information flow) that attempts to detect (1) exfiltration of sensitive information and (2) remote-access trojans and similar malicious code that allows an attacker to perform unauthorized operations.
For the POSIX/C (or Windows) API function `$FUNC_NAME`, first indicate the Standard C or POSIX or Windows header file (if known) in which the function is defined and then print out the prototype of the function and briefly explain each parameter and the return value.  Then identify whether each parameter is an 'in', 'out', or 'inout' parameter and whether it may contain potentially sensitive data, using the JSON format below.  In the JSON response, also identify whether the return value may contain sensitive data.
As an example, the contents of a file should be considered to contain potentially sensitive data, but the inode number of the file should not be considered sensitive.  Likewise, data read from a microphone should be considered sensitive, but IRQ numbers should not be considered sensitive.  In general, strings should be considered potentially sensitive, but small integers should not.  However, the `mode` parameter of `fopen`, despite being a string, shouldn’t be considered sensitive, because there are only a small number of valid values of `mode`, and data cannot be exfiltrated by passing it via the `mode` parameter.
Use this JSON format for your answer:
```
{
  "return": {
    "explanation": "(explain why the return value is or isn't potentially sensitive)",
    "sensitive": "true",
    "from_external": "true",
    "special_role": "...",
    "from_params": [...]
  },
  "arg order": ["name_of_parameter_1", ..., "name_of_parameter_n"],
  "name_of_parameter_1": {
    "direction": "in",
    "explanation_sensitive": "(explain why the data is or isn't sensitive)",
    "sensitive": "true",
    "explanation_external": "(explain your answer for the 'to_external'/'from_external' field)",
    "to_external"/"from_external": "true",
    "special_role": "...",
    "from_params": [...]
  },
  ...
}
```
This JSON answer should appear at the *end* of your answer; if you wish to include any explanatory text, please it *before* your JSON answer.
If an 'in' (or 'inout') parameter is sensitive, the `to_external` field should be "true" iff the data is potentially copied or used externally (i.e., outside the program’s address space), such as being written to a file, being supplied to another process, or being stored by the OS.  The `to_external` field should be false if the data is merely transformed and given back to the program.  For example, the data given to the function `fprintf` is copied externally, but the data given to `sprintf` isn't copied externally (it is merely processed and returned to the program).  As another example, if data is passed to a new or existing process, `to_external` should be "true".  In general, if the data may have an observable effect outside the program, the `to_external` field should be "true".  If the parameter is not sensitive, this field may be omitted.
If an 'out' (or 'inout') parameter or return value is sensitive, the `from_external` field should be "true" if the API function directly copies data from an external source.  For example, `scanf` copies data from an external source, but `sscanf` doesn't (it uses only data supplied by the program).   If the parameter or return value is not sensitive, this field may be omitted.
If potentially sensitive data is copied from an IN/INOUT parameter to the return value or to an OUT/INOUT parameter, indicate that using the `from_params` field.  For example, for `char* strcpy(char* dest, char* src)`, the `from_params` field of `return` and `dest` should both be `["src"]`, because the string pointed to by `src` is copied to `dest` and a pointer to `dest` is the return value.
For variadic function such as `printf`, use "..." for the name of the variadic parameter.
If an argument or return value is a file identifier (e.g., a FILE* pointer, a numeric file descriptor, or the name of a file), then set the `special_role` field to "file"; otherwise, omit the `special_role` field.  For the purpose of this field, sockets, pipes, etc. are all considered files.

If you do not recognize the asked-about function, then instead of answering as above, say
```
{"error": "unknown function"}
```
"""
)


def read_whole_file(filename):
    with open(filename, 'r') as the_file:
        return the_file.read()

def write_file(filename, content):
    with open(filename, 'w') as file:
        file.write(content)


def print_progress(msg):
    elapsed_time = time.time() - program_start_time
    print("[%6.2f sec] %s" % (elapsed_time, msg))

def parse_args():
    parser = argparse.ArgumentParser(description='Asks GPT about sources and sinks')
    parser.add_argument("func_list", type=str, help="List of functions (either the name of a JSON file or a space-separated list of functions)")
    parser.add_argument("out_dir", type=str, help="Directory to write output files")
    cmdline_args = parser.parse_args()
    return cmdline_args

def main():
    global cmdline_args
    cmdline_args = parse_args()
    func_list = None
    if "." in cmdline_args.func_list:
        func_list = json.loads(read_whole_file(cmdline_args.func_list))
    else:
        func_list = cmdline_args.func_list.split()
    for func_name in func_list:
        print(func_name)
        ask_func(func_name)
    print_progress("Done!")

def ask_func(func_name):
    global cmdline_args
    filename = cmdline_args.out_dir + "/" + func_name + ".txt"
    if os.path.isfile(filename):
        print("Answer file already exists for %s!" % func_name)
        return
    messages = [
        {"role": "system", "content": "You are ChatGPT, a large language model trained by OpenAI, based on the GPT-4 architecture.\n" + 
                                      "Knowledge cutoff: 2021-09\n" + "Current date: 2023-09-09"},
        {"role": "user", "content": prompt.replace("$FUNC_NAME", func_name)},
    ]
    print_progress("Asking about %s..." % func_name)

    chat_completion = openai_client.chat.completions.create(
        model="gpt-4-turbo-preview",
        messages=messages)

    print(chat_completion)
    print("="*78)
    answer = chat_completion.choices[0].message.content
    write_file(filename=filename, content=answer+"\n")


program_start_time = time.time()
main()

