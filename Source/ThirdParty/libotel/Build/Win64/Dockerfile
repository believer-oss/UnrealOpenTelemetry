FROM ghcr.io/believer-oss/ue-builder:ltsc2022-vs2022 AS build

SHELL ["powershell", "-Command", "$ErrorActionPreference = 'Stop';$ProgressPreference='silentlyContinue';"]

ENV CORES=32
ARG OTEL_GIT_TAG=v1.16.1
ARG GRPC_GIT_TAG=v1.65.5
ARG BUILD_TYPE=Release

WORKDIR C:\\work

RUN git clone \
  -j $env:CORES \
  --depth=1 \
  -b $env:GRPC_GIT_TAG \
  --recurse-submodules \
  --shallow-submodules \
  https://github.com/grpc/grpc.git

RUN git clone \
  -j $env:CORES \
  --depth=1 \
  -b $env:OTEL_GIT_TAG \
  --recurse-submodules \
  --shallow-submodules \
  https://github.com/open-telemetry/opentelemetry-cpp.git

COPY ThirdParty C:\\work\\ThirdParty

RUN Invoke-WebRequest -Uri https://github.com/Kitware/CMake/releases/download/v3.30.3/cmake-3.30.3-windows-x86_64.msi -OutFile cmake.msi
RUN msiexec /i cmake.msi /quiet /passive

RUN Set-Content -Path .\\toolchain.cmake -Value ''; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(UE_THIRDPARTY "C:\\work\\ThirdParty")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(UE_OPENSSL "${UE_THIRDPARTY}\\OpenSSL\\1.1.1t")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'include_directories( AFTER "${UE_OPENSSL}\\include\\Win64\\VS2015")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(CMAKE_CXX_STANDARD "17")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(SSL_ROOT_DIR "${UE_OPENSSL}")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(OPENSSL_INCLUDE_DIR "${UE_OPENSSL}\\include\\Win64\\VS2015")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(OPENSSL_SSL_LIBRARY "${UE_OPENSSL}\\lib\\Win64\\VS2015\\Release\\libssl.lib")'; \
  Add-Content -Path .\\toolchain.cmake -Value 'set(OPENSSL_CRYPTO_LIBRARY "${UE_OPENSSL}\\lib\\Win64\\VS2015\\Release\\libcrypto.lib)'

# Build abseil-cpp
WORKDIR C:\\work\\grpc\\third_party\\abseil-cpp\\cmake\\build
RUN cmake \
    -A "x64" \
    -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
    -DABSL_PROPAGATE_CXX_STD=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
    ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# Build protobuf
WORKDIR C:\\work\\grpc\\third_party\\protobuf\\cmake\\build
RUN cmake \
  -A "x64" \
  -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -Dprotobuf_ABSL_PROVIDER=package \
  -Dabsl_DIR="C:\\work\\otel-cpp" \
  -Dprotobuf_BUILD_TESTS=false \
  -Dprotobuf_WITH_ZLIB=false \
  -Dprotobuf_BUILD_EXAMPLES=false \
  -Dprotobuf_MSVC_STATIC_RUNTIME=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PREFIX_PATH="C:\\work\\otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
  ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# Build re2
WORKDIR C:\\work\\grpc\\third_party\\re2\\cmake\\build
RUN cmake \
    -A "x64" \
    -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
    -DRE2_BUILD_TESTING=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
    ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# Build zlib
WORKDIR C:\\work\\grpc\\third_party\\zlib\\cmake\\build
RUN cmake \
  -A "x64" \
  -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
  ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# Build cares
WORKDIR C:\\work\\grpc\\third_party\\cares\\cares\\cmake\\build
RUN cmake \
  -A "x64" \
  -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DCARES_STATIC=ON \
  -DCARES_SHARED=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
  ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# move OpenSSL libs into cmake-compatible directory format
WORKDIR C:\\work
RUN mkdir OpenSSL; \
  Copy-Item -Recurse -Path C:\\work\\ThirdParty\\OpenSSL\\1.1.1t\\include\\Win64\\VS2015 C:\\work\\OpenSSL\\include; \
  Copy-Item -Recurse -Path C:\\work\\ThirdParty\\OpenSSL\\1.1.1t\\lib\\Win64\\VS2015\\Release C:\\work\\OpenSSL\\lib

# Build grpc
WORKDIR C:\\work\\grpc\\cmake\\build
RUN cmake \
  -A "x64" \
  -DgRPC_INSTALL=ON \
  -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
  -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
  -DgRPC_ABSL_PROVIDER=package \
  -Dabsl_DIR="C:\\work\\otel-cpp" \
  -DgRPC_RE2_PROVIDER=package \
  -Dre2_DIR="C:\\work\\otel-cpp/" \
  -DgRPC_PROTOBUF_PROVIDER=package \
  -DgRPC_ZLIB_PROVIDER=package \
  -DgRPC_CARES_PROVIDER=package \
  -Dc-ares_DIR="C:\\work\\otel-cpp/" \
  -DgRPC_SSL_PROVIDER=package \
  -DOPENSSL_ROOT_DIR="C:\\work\\OpenSSL" \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PREFIX_PATH="C:\\work\\otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
  -DgRPC_BUILD_CODEGEN=ON \
  ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

# Build opentelemetry-cpp
WORKDIR C:\\work\\opentelemetry-cpp\\cmake\\build
RUN cmake \
  -A "x64" \
  -DCMAKE_TOOLCHAIN_FILE="C:\\work\\toolchain.cmake" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE  \
  -DWITH_STL=CXX17 \
  -DWITH_ABSEIL=ON \
  -DWITH_NO_DEPRECATED_CODE=ON \
  -DWITH_DEPRECATED_SDK_FACTORY=OFF \
  -DOPENTELEMETRY_INSTALL=ON \
  -DBUILD_TESTING=OFF \
  -DWITH_BENCHMARK=OFF \
  -DWITH_EXAMPLES=OFF \
  -DWITH_FUNC_TESTS=OFF \
  -Dabsl_DIR="C:\\work\\otel-cpp" \
  -Dopentelemetry_gRPC_PROVIDER=package \
  -DgRPC_DIR="C:\\work\\otel-cpp" \
  -DCMAKE_INSTALL_PREFIX=C:\\work\\otel-cpp \
  -DWITH_OTLP_GRPC=ON \
  -DWITH_OTLP_HTTP=OFF \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DOPENSSL_ROOT_DIR="C:\\work\\OpenSSL" \
  -DCMAKE_PREFIX_PATH="C:\\work\\otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="C:\\work\\otel-cpp" \
  ..\\..
RUN cmake --build . -j $env:CORES --config "$env:BUILD_TYPE" --target install

CMD [""]
