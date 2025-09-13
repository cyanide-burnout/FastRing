* Clone h2o from master

git clone https://github.com/h2o/h2o.git
cd h2o
git submodule update --init --recursive

* Check service

grpcurl -v -plaintext \
  -proto gRPCTest.proto -import-path . \
  -d '{"text":"one"}' \
  10.211.55.9:8080 demo.Echoer/UnaryEcho
