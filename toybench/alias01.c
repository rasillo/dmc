// clang-14 -Xclang -disable-O0-optnone -fno-discard-value-names -fno-inline-functions -ggdb -c -S -emit-llvm -O0 alias01.c

// <legal>
// DMC Tool
// Copyright 2023 Carnegie Mellon University.
// 
// NO WARRANTY. THIS CARNEGIE MELLON UNIVERSITY AND SOFTWARE ENGINEERING INSTITUTE
// MATERIAL IS FURNISHED ON AN "AS-IS" BASIS. CARNEGIE MELLON UNIVERSITY MAKES NO
// WARRANTIES OF ANY KIND, EITHER EXPRESSED OR IMPLIED, AS TO ANY MATTER
// INCLUDING, BUT NOT LIMITED TO, WARRANTY OF FITNESS FOR PURPOSE OR
// MERCHANTABILITY, EXCLUSIVITY, OR RESULTS OBTAINED FROM USE OF THE MATERIAL.
// CARNEGIE MELLON UNIVERSITY DOES NOT MAKE ANY WARRANTY OF ANY KIND WITH RESPECT
// TO FREEDOM FROM PATENT, TRADEMARK, OR COPYRIGHT INFRINGEMENT.
// 
// Released under a MIT (SEI)-style license, please see License.txt or contact
// permission@sei.cmu.edu for full terms.
// 
// [DISTRIBUTION STATEMENT A] This material has been approved for public release
// and unlimited distribution.  Please see Copyright notice for non-US Government
// use and distribution.
// 
// Carnegie Mellon (R) and CERT (R) are registered in the U.S. Patent and Trademark
// Office by Carnegie Mellon University.
// 
// This Software includes and/or makes use of the following Third-Party Software
// subject to its own license:
// 1. Phasar
//     (https://github.com/secure-software-engineering/phasar/blob/development/LICENSE.txt)
//     Copyright 2017 - 2023 Philipp Schubert and others.  
// 2. LLVM (https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT) 
//     Copyright 2003 - 2022 LLVM Team.
// 
// DM23-0532
// </legal>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    #define BUF_SIZE 1024
    char bufA[BUF_SIZE];
    char bufB[BUF_SIZE] = "Hello, world!\n";
    fgets(bufA, sizeof(bufA), stdin);
    char* buf = (argc % 2 == 0) ? bufA : bufB;
    fputs(buf, stdout);
    return 0;
}
