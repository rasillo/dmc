# Detection of Malicious Code (DMC) using Information Flow Analysis

## Legal Markings
<legal>  
DMC Tool  
Copyright 2023 Carnegie Mellon University.  
  
NO WARRANTY. THIS CARNEGIE MELLON UNIVERSITY AND SOFTWARE ENGINEERING INSTITUTE  
MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO  
WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED, AS TO ANY MATTER  
INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR PURPOSE OR  
MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF THE MATERIAL.  
CARNEGIE MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT  
TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.  
  
Released under a MIT (SEI)-style license, please see License.txt or contact  
permission@sei.cmu.edu for full terms.  
  
[DISTRIBUTION STATEMENT A] This material has been approved for public release  
and unlimited distribution.  Please see Copyright notice for non-US Government  
use and distribution.  
  
Carnegie Mellon (R) and CERT (R) are registered in the U.S. Patent and Trademark  
Office by Carnegie Mellon University.  
  
This Software includes and/or makes use of the following Third-Party Software  
subject to its own license:  
1. Phasar  
    (https://github.com/secure-software-engineering/phasar/blob/development/LICENSE.txt)  
    Copyright 2017 - 2023 Philipp Schubert and others.    
2. LLVM (https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT)   
    Copyright 2003 - 2022 LLVM Team.  
  
DM23-0532  
</legal>  

## Building and running the docker container

The DMC tool uses ".ll" files produced by Clang.  The ".ll" files produced by one version of Clang are not necessarily compatible with other versions of Clang.  DMC supports multiple versions of Clang, but when you build the DMC container, it is tied to a specific version of Clang.  Below, we use Dockerfile.clang15.  There is also Dockerfile.clang14.  We will support additional versions of Clang in the future.

To build:
```bash
time docker build -f Dockerfile.clang15 -t dmc-clang15 . --no-cache # "--no-cache" can usually be omitted
```

To run:
```
dmc_dir=<absolute pathname of directory where you extracted DMC; this is the directory with Dockerfile.clang15>
data_dir=<directory with code to analyze>
docker run -it --rm -v $dmc_dir:/host_dmc [-v $data_dir:/data] dmc-clang15 bash
```
For the `mal-client-2` demo, you can omit `[-v $data_dir:/data]`.  If there is a problem mounting shared volumes, you can also omit `-v $dmc_dir:/host_dmc` and use `/incl_dmc` instead of `/host_dmc` in the rest of these instructions (or do `ln -s /incl_dmc /host_dmc`).


## Building the LLVM taint pass

```
cd /host_dmc/condmerge
cmake -DCMAKE_BUILD_TYPE=Debug . # Don't leave out the period.
make Taint
```

## How to generate an ".ll" file from a single ".c" file

Given a single C file `file.c`, you can generate a suitable ".ll" file via:

`clang-$CLANGVER -Xclang -disable-O0-optnone -fno-discard-value-names -fno-inline-functions -ggdb -c -S -emit-llvm -O0 file.c`

For example, for `mal-client-2.c`:
```
cd /host_dmc/toybench
clang-$CLANGVER -Xclang -disable-O0-optnone -fno-discard-value-names -fno-inline-functions -ggdb -c -S -emit-llvm -O0 mal-client-2.c
```


## How to run the analysis on an ".ll" file

For file `input.ll` and an optional wrappers file `wrappers.txt`, the DMC analysis is run as follows:

`/host_dmc/run_taint_pass.sh input.ll --sources-and-sinks /host_dmc/gpt/func_taint3.txt --taint-copiers /host_dmc/taint_copiers.txt [--wrappers wrappers.txt] | python3 /host_dmc/condmerge/connect_flows.py`

Example for `mal-client-2.ll`:

`/host_dmc/run_taint_pass.sh /host_dmc/toybench/mal-client-2.ll  --sources-and-sinks ../gpt/func_taint3.txt --taint-copiers /host_dmc/taint_copiers.txt  --wrappers /host_dmc/toybench/mc2.wrappers.txt  | python3 /host_dmc/condmerge/connect_flows.py`

The expected output is:
```
Round 1 (35 functions in worklist) 
Round 2 (0 functions in worklist) 
[
{"sink": {"func":"write", "callsite":["mal-client-2.c","main",162,21], "id":1,
    "aux file": [{"func":"socket", "callsite":["mal-client-2.c","main",96,18], "id":2}]},
 "srcs": [{"func":"read_from_file", "callsite":["mal-client-2.c","main",160,32], "id":3,
    "wrapped":{"func":"fread", "callsite":["mal-client-2.c","read_from_file",75,13], "id":4,
      "aux file": [{"func":"fopen", "callsite":["mal-client-2.c","read_from_file",68,10], "id":5,
        "aux file": [{"func":"getline", "callsite":["mal-client-2.c","main",137,26], "id":6, "FILE*":"stdin"},
        {"func":"getline", "callsite":["mal-client-2.c","main",148,30], "id":7, "FILE*":"stdin"}]}]}}]},

{"sink": {"func":"write", "callsite":["mal-client-2.c","main",168,17], "id":8,
    "aux file": [{"func":"socket", "callsite":["mal-client-2.c","main",96,18], "id":2}]},
 "srcs": [{"func":"getline", "callsite":["mal-client-2.c","main",137,26], "id":6, "FILE*":"stdin"},
  {"func":"getline", "callsite":["mal-client-2.c","main",148,30], "id":7, "FILE*":"stdin"}]},

{"sink": {"func":"write", "callsite":["mal-client-2.c","main",183,29], "id":9,
    "aux file": [{"func":"socket", "callsite":["mal-client-2.c","main",96,18], "id":2}]},
 "srcs": [{"func":"read_from_file", "callsite":["mal-client-2.c","main",180,40], "id":10,
    "wrapped":{"func":"fread", "callsite":["mal-client-2.c","read_from_file",75,13], "id":4,
      "aux file": [{"func":"fopen", "callsite":["mal-client-2.c","read_from_file",68,10], "id":5,
        "aux file": [{"filename":"secrets.txt"} ]}]}}]}
]
```

## How to generate ".ll" files for a multi-file codebase

For a POSIX codebase with a makefile, you can use `make_run_clang.py`, as follows:

1. Run `bear` (included in the Docker image; https://github.com/rizsotto/Bear) to generate `compile_commands.json`, like this:
```bash
bear -- make
```

2. Run the `make_run_clang.sh` script, like so:
```bash
/host_dmc/make_run_clang.py -c compile_commands.json -o clang_ll.sh
```
This produces a shell script for generating ".ll" files.

3. Run the generated `clang_ll.sh` script.  This generates generates an ".ll" file for each translation unit.

4. Link the produced ".ll" files into a single combined ".ll" file:
```
llvm-link-$CLANGVER -S -o combined.ll file_1.raw.ll ... file_N.raw.ll
```

For binaries, GhiLift or RetDec can be used, but the results aren't perfect.


## File for specifying sources and sinks

Each line specifies a single function.
The format of each line is `FuncName arg_1_cat arg_2_cat ... arg_N_cat -> ret_cat`
where each `arg_i_cat` indicates the taint category of argument `i`, and 
`ret_cat` indicates the taint category of the function return value.

Taint categories are: `Src`, `Sink`, `SrcAndSink`,
`FileSource`, `FileSink`, `FileSrcAndSink`, `none`.

If a function has no sources or sinks, the shorter format `FuncName -` can be used. 

The DMC tool will print out a list of external functions that appear in the codebase but don't appear in the Sources-And-Sinks file: `Unrecognized external functions: [ func_1 ... func_n ]`

## File for specifying wrapper functions

A list of wrapper functions, one per line.  We hope to mostly automate this soon.
More context sensitivity is provided for functions listed in this file.
Do not put any recursive functions (incl. mutually recursive functions) here; otherwise, the analysis may get stuck in an infinite loop.



