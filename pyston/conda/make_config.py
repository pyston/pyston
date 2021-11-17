import os
import sys

def rewrite_config(config_str):
    lines = config_str.split('\n')

    for i in range(len(lines)):
        if lines[i] == "python:":
            lines[i + 1] = "- 3.8.* *_pyston"
            break
    else:
        raise Exception("didn't find 'python' line")

    for i in range(len(lines)):
        if lines[i] == "python_impl:":
            lines[i + 1] = "- pyston"
            break

    return '\n'.join(lines)

def main():
    configs = os.listdir(".ci_support")

    possible_configs = []

    cwd = os.getcwd()

    for c in configs:
        if "aarch64" in c or "ppc" in c:
            continue
        if "win" in c or "osx" in c:
            continue
        if "3." in c and "3.8" not in c:
            continue
        if c in ("migrations", "README"):
            continue
        if "openssl3" in c:
            continue
        if ("mpi4py" in cwd or "h5py" in cwd or "netcdf4" in cwd) and ("openmpi" in c or "nompi" in c):
            continue

        if "cuda_compiler_version" in c:
            assert "11.3" not in c
            if "11.2" not in c:
                continue

        if "pyston" in c:
            continue

        possible_configs.append(c)

    assert len(possible_configs) == 1, possible_configs
    config, = possible_configs

    config_str = open(".ci_support/" + config).read()
    new_config_str = rewrite_config(config_str)

    open(".ci_support/linux-pyston.yaml", 'w').write(new_config_str)
    print("linux-pyston")

if __name__ == "__main__":
    try:
        main()
    except:
        print("make_config.py-failed")
        raise
