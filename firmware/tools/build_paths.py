"""Remove workstation-specific paths from __FILE__ and debug data in shipped images."""
from pathlib import Path
Import("env")
flags=[]
for source,target in [(Path.home(),"/toolchain-user"),(Path(env["PROJECT_DIR"]),"/btclone/firmware")]:
    variants={str(source),source.as_posix(),"/"+source.as_posix()}
    for name in sorted(variants,key=len,reverse=True):
        flags.append("-ffile-prefix-map="+name+"="+target)
env.Append(CCFLAGS=flags)
