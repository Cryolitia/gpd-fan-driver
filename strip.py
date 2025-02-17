#! /usr/bin/env nix-shell
#! nix-shell -i python3 -p "python3.withPackages (ps: with ps; [ ])"

import re
import sys

if __name__ == "__main__":
    status = []
    with open(sys.argv[1]) as f:
        content = f.read()
        content = re.sub(r'#define OUT_OF_TREE', '', content)
        content = re.sub(r'#if LINUX_VERSION_CODE.*?#endif\n', '', content, flags = re.S)
        content = re.sub(r'#ifdef((?!#endif).)*?#else\n(.*?)#endif\n', r'\2', content, flags = re.S)
        content = re.sub(r'#ifdef.*?#endif\n', '', content, flags = re.S)
        content = re.sub(r'\n{2,}', '\n\n', content)
        print(content, end='')
