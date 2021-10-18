#!/bin/sh

set -eux

BUILD_NAME=2.3.1
DIR=$(dirname $0)

docker build -t pyston/pyston:latest -t pyston/pyston:${BUILD_NAME} -f ${DIR}/Dockerfile ${DIR}
docker push pyston/pyston:latest
docker push pyston/pyston:${BUILD_NAME}

docker build -t pyston/pyston:slim-bullseye -t pyston/pyston:slim -t pyston/pyston:${BUILD_NAME}-slim-bullseye -t pyston/pyston:${BUILD_NAME}-slim -f ${DIR}/Dockerfile.slim-bullseye ${DIR}
docker push pyston/pyston:slim
docker push pyston/pyston:slim-bullseye
docker push pyston/pyston:${BUILD_NAME}-slim-bullseye
docker push pyston/pyston:${BUILD_NAME}-slim
