#cp ../../../embedded/src/bert/* $1/huffman_demo/src
#cp ../../../docs/tutorials/huffman/sw_huffman/* $1/huffman_demo/src

import os.path
import pathlib
import argparse
import glob
import shutil
import sys

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("workDir", help='Directory which tutorial refers to as WORK and where provided files were copied to.  Example: /home/steven/myProject.  Assumes design_1_wrapper is subdirectory of this.')

    args = parser.parse_args()

    bertDir = pathlib.Path(sys.path[0]).resolve().parent.parent.parent
    assert os.path.isdir(str(bertDir))
    bertBspDir = bertDir / "embedded" / "libsrc" / "xilfpga_v5_1" / "src"
    assert os.path.isdir(str(bertBspDir))

    workDir = pathlib.Path(args.workDir).resolve()
    assert os.path.isdir(str(workDir)) 
    sdkDir  = workDir / "design_1_wrapper"
    assert os.path.isdir(str(sdkDir)) 
    appBspDir = sdkDir / "psu_cortexa53_0" / "standalone_domain" / "bsp" / "psu_cortexa53_0" / "libsrc" / "xilfpga_v5_1" / "src"
    assert os.path.isdir(str(appBspDir))

    print("Copying files from '{}' to \n                           '{}'".format(bertBspDir, appBspDir))
    for name in glob.glob(str(bertBspDir / '*')):
        print("    Copying: {}".format(name))
        shutil.copy2(name, str(appBspDir))
    