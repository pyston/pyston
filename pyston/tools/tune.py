import os
import subprocess
from glob import glob

# Pin the CPU frequency to this while running the benchmarks.
# If this is set to 0 or None, we will instead disable turbo boost
# and the CPU will run at its base frequency.
if "PIN_FREQ" in os.environ:
    PIN_FREQ = int(os.environ["PIN_FREQ"])
elif os.path.exists(os.path.expanduser("~/.pinfreq")):
    PIN_FREQ = int(open(os.path.expanduser("~/.pinfreq")).read())
else:
    raise Exception("Specify the PIN_FREQ env var or write ~/.pinfreq")

def rel(path):
    return os.path.join(os.path.dirname(__file__), path)

PYPERF = rel("pyperf_env/bin/pyperf")

if not os.path.exists(PYPERF):
    subprocess.check_call(["python3", "-m", "venv", rel("pyperf_env")])
    subprocess.check_call([rel("pyperf_env/bin/pip"), "install", "pyperf"])


IS_AMD = "AMD" in open("/proc/cpuinfo").read()

def write_to_sys_file(path, value):
    p = subprocess.Popen(["sudo", "tee", path], stdin=subprocess.PIPE)
    p.communicate(value + b"\n")
    assert p.wait() == 0

def tune():
    ret = subprocess.call(["sudo", PYPERF, "system", "tune"], stdout=open("/dev/null", "w"))

    # 'pyperf system tune' will report an error on AMD systems because it can't do intel specific changes
    # but it will still execute the non intel ones.
    if IS_AMD:
        # Now we have to manually disable turbo boost
        write_to_sys_file("/sys/devices/system/cpu/cpufreq/boost", b"0")
        for f in glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"):
            write_to_sys_file(f, b"performance")
    else:
        if ret != 0:
            ret = subprocess.call(["sudo", PYPERF, "system", "tune"])
            assert 0
        assert ret == 0

    if PIN_FREQ:
        assert not IS_AMD, "on AMD systems we don't support setting a specific frequency"
        write_to_sys_file("/sys/devices/system/cpu/intel_pstate/no_turbo", b"0")

        subprocess.check_call("bash -c 'echo %d | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_{min,max}_freq'" % PIN_FREQ, shell=True)

def untune():
    if PIN_FREQ:
        assert not IS_AMD, "on AMD systems we don't support setting a specific frequency"
        subprocess.check_call("echo 0 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_min_freq", shell=True)
        subprocess.check_call("echo 99999999 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq", shell=True)

    ret = subprocess.call(["sudo", PYPERF, "system", "reset"], stdout=open("/dev/null", "w"))

    # 'pyperf system reset' will report an error on AMD systems because it can't do intel specific changes
    # but it will still execute the non intel ones.
    if IS_AMD:
        # Now we have to manually enable turbo boost
        write_to_sys_file("/sys/devices/system/cpu/cpufreq/boost", b"1")
        for f in glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"):
            write_to_sys_file(f, b"ondemand")
    else:
        assert ret == 0

