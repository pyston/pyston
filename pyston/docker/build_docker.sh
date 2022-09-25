#!/bin/sh
#
# Requires:
# - docker buildx
# - installed qemu emulation for the non native arch
#   e.g.: docker run --privileged --rm tonistiigi/binfmt --install arm64

set -eux

BUILD_NAME=2.3.5
DIR=$(dirname $0)
PLATFORMS=linux/amd64,linux/arm64

docker buildx create --use --name pyston-build-context --node mybuilder0

docker buildx build --push --platform ${PLATFORMS} -t pyston/pyston:latest -t pyston/pyston:${BUILD_NAME} -f ${DIR}/Dockerfile ${DIR}

docker buildx build --push --platform ${PLATFORMS} -t pyston/slim:latest -t pyston/slim:${BUILD_NAME} -f ${DIR}/Dockerfile.slim-bullseye ${DIR}

docker buildx build --push --platform ${PLATFORMS} -t pyston/conda:latest -t pyston/conda:${BUILD_NAME} -f ${DIR}/Dockerfile.conda ${DIR}

docker buildx rm pyston-build-context
