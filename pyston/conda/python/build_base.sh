#!/bin/sh
set -eux

# replace python
ln -sf ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2} ${PREFIX}/bin/python
ln -sf ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2} ${PREFIX}/bin/python3
ln -sf ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2} ${PREFIX}/bin/python${PYTHON_VERSION2}
