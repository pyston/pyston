FROM ubuntu:20.04

ENV PYSTON_VERSION='2.3.1'
ENV UBUNTU_VERSION='20.04'

RUN set -eux; \
    apt-get update; \
    apt-get install -y wget; \
    wget https://github.com/pyston/pyston/releases/download/pyston_${PYSTON_VERSION}/pyston_${PYSTON_VERSION}_${UBUNTU_VERSION}.deb; \
    apt install -y ./pyston_${PYSTON_VERSION}_${UBUNTU_VERSION}.deb; \
    rm pyston_${PYSTON_VERSION}_${UBUNTU_VERSION}.deb; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    ln -sf /usr/bin/pyston /usr/bin/python3.8; \
    ln -sf /usr/bin/pyston /usr/bin/python3; \
    ln -sf /usr/bin/pyston /usr/bin/python; \
    ln -sf /usr/bin/pip-pyston /usr/bin/pip

CMD ["python3.8"]

