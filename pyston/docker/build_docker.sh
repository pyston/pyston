set -eux

BUILD_NAME=2.3.1

docker build -t pyston/pyston:latest -t pyston/pyston:${BUILD_NAME} $(dirname $0)
docker push pyston/pyston:latest
docker push pyston/pyston:${BUILD_NAME}
