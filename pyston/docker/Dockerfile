FROM ubuntu:20.04

RUN apt-get update
RUN apt-get install -y wget

RUN set -eux; \
    wget https://github.com/pyston/pyston/releases/download/v2.3/pyston_2.3_20.04.deb; \
    apt install -y ./pyston_2.3_20.04.deb; \
    rm pyston_2.3_20.04.deb

RUN set -eux; \
    ln -sf /usr/bin/python3.8-pyston2.3 /usr/bin/python3.8; \
    ln -sf /usr/bin/python3.8-pyston2.3 /usr/bin/python3; \
    ln -sf /usr/bin/pip-pyston2.3 /usr/bin/pip

