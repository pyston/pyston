set -eu
set -x

rm -rfv /tmp/pip_patch
mkdir /tmp/pip_patch

FN=$(realpath python/cpython/Lib/ensurepip/_bundled/pip*.whl)

unzip $FN -d /tmp/pip_patch

patch /tmp/pip_patch/pip/_vendor/distlib/scripts.py <<EOF
164c164
<                             'python%s' % sysconfig.get_config_var('EXE'))
---
>                             'pyston%s' % sysconfig.get_config_var('EXE'))
168c168
<                'python%s%s' % (sysconfig.get_config_var('VERSION'),
---
>                'pyston%s%s' % (sysconfig.get_config_var('VERSION'),
EOF

rm $FN
(cd /tmp/pip_patch; zip -r $FN -b /tmp/pip_patch *)

rm -rfv /tmp/pip_patch
