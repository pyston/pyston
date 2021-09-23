set -eux

BUILD_NAME=2.3

docker build -t pyston/pyston:latest -t pyston/pyston:${BUILD_NAME} .
docker push pyston/pyston:latest
docker push pyston/pyston:${BUILD_NAME}
