#!/bin/sh

set -eux

BUILD_NAME=2.3.1
DIR=$(dirname $0)

docker build -t pyston/pyston:latest -t pyston/pyston:${BUILD_NAME} -f ${DIR}/Dockerfile ${DIR}
docker push pyston/pyston:latest
docker push pyston/pyston:${BUILD_NAME}

docker build -t pyston/slim:latest -t pyston/slim:${BUILD_NAME} -f ${DIR}/Dockerfile.slim-bullseye ${DIR}
docker push pyston/slim:latest
docker push pyston/slim:${BUILD_NAME}
