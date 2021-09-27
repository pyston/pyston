#!/bin/sh
set -eux

# replace python
ln -sf ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/python
ln -sf ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/python3
