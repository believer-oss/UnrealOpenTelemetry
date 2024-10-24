#!/bin/bash
if [ ! -d "ThirdParty" ]; then
  echo "Copy the OpenSSL and zlib libraries from the Unreal Engine ThirdParty directory into a ThirdParty directory located here."
  exit 1
fi

if [ -d "otel-cpp" ]; then
  rm -rf otel-cpp
fi

docker build -t otel-cpp-build -f Dockerfile .
docker create --name=otel-cpp otel-cpp-build
docker cp otel-cpp:/opt/otel-cpp otel-cpp/
docker rm otel-cpp
rm ../../1.16.1/lib/Linux/Release/*
cp otel-cpp/lib/*.a ../../1.16.1/lib/Linux/Release
