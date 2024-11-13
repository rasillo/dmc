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

# https://chat.openai.com/share/8418def4-a196-4549-b411-c78fb21b1551

import subprocess
import sys
import re

def get_man_page(function_name):
    """ Fetch the man page for a given function. """
    try:
        # Run the 'man' command and get the output
        output = subprocess.check_output(['man', function_name], text=True)
        return output
    except subprocess.CalledProcessError:
        sys.stderr.write(f"Man page for '{function_name}' not found.")
        return None

def extract_prototype(man_page, function_name):
    """ Extract the function prototype from the man page. """
    # Regular expression to match the prototype
    # Adjust the regex according to the standard formatting of man pages
    regex = re.compile(rf'\b{function_name}\b\(.*?\);', re.DOTALL)
    match = regex.search(man_page)
    if match:
        return match.group()
    else:
        return None

def prototype_of_func(function_name):
    man_page = get_man_page(function_name)
    if man_page is None:
        return None
    prototype = extract_prototype(man_page, function_name)
    return prototype

def main():
    if len(sys.argv) != 2:
        print("Usage: python3 print_proto.py [function_name]")
        sys.exit(1)

    function_name = sys.argv[1]
    prototype = prototype_of_func(function_name)

    if prototype:
        print(prototype)
    else:
        print(f"Prototype for '{function_name}' not found.")

if __name__ == "__main__":
    main()
