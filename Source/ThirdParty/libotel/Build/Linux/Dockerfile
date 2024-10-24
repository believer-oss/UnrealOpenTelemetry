FROM ghcr.io/believer-oss/ue-builder:cuda12.1.1-ubuntu22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

ENV CORES=32
ENV OTEL_GIT_TAG=v1.16.1
ARG GRPC_GIT_TAG=v1.65.5
ARG UE_REPO=https://github.com/EpicGames/UnrealEngine.git
ARG UE_BRANCH=release

ARG CMAKE_PREFIX_PATH="/usr/local:/opt/otel-cpp"
ARG CPATH="/work/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu/usr/include"

USER root
RUN apt-get update && \
  apt-get install --no-install-recommends --no-install-suggests -y \
  autoconf \
  build-essential \
  ca-certificates \
  cmake \
  curl \
  git \
  iwyu \
  lcov \
  libbenchmark-dev \
  libgmock-dev \
  libgtest-dev \
  libtool \
  pkg-config \
  wget 

WORKDIR /work/

# Clone grpc
RUN git clone \
  -j${CORES} \
  --depth=1 \
  -b ${GRPC_GIT_TAG} \
  --recurse-submodules \
  --shallow-submodules \
  https://github.com/grpc/grpc

# Clone opentelemetry-cpp
RUN git clone \
  -j${CORES} \
  --depth=1 \
  -b ${OTEL_GIT_TAG} \
  --recurse-submodules \
  --shallow-submodules \
  https://github.com/open-telemetry/opentelemetry-cpp.git

# Setup UE 5.4-compatible clang toolchain
RUN wget https://cdn.unrealengine.com/Toolchain_Linux/native-linux-v22_clang-16.0.6-centos7.tar.gz && \
  tar xvf native-linux-v22_clang-16.0.6-centos7.tar.gz

COPY ThirdParty /work/ThirdParty 

COPY <<-"EOF" toolchain.cmake
include(CMakePrintHelpers)

set(UE_THIRDPARTY "/work/ThirdParty")
set(UE_CLANG_TOOLCHAIN "/work/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu")
#set(UE_CLANG_TOOLCHAIN "${UE_ROOTDIR}/Engine/Extras/ThirdPartyNotUE/SDKs/HostLinux/Linux_x64/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu")
set(UE_CLANG_TOOLCHAIN_BIN "${UE_CLANG_TOOLCHAIN}/bin")

set(CMAKE_C_COMPILER	"${UE_CLANG_TOOLCHAIN_BIN}/clang"				CACHE PATH "compiler" FORCE) 
set(CMAKE_CXX_COMPILER	"${UE_CLANG_TOOLCHAIN_BIN}/clang++"				CACHE PATH "" FORCE) 
set(CMAKE_ASM_COMPILER	"${CMAKE_C_COMPILER}"						CACHE STRING "" FORCE)
set(CMAKE_AR 		"${UE_CLANG_TOOLCHAIN_BIN}/llvm-ar"		CACHE PATH "archive")
set(CMAKE_RANLIB	"${UE_CLANG_TOOLCHAIN_BIN}/x86_64-unknown-linux-gnu-ranlib"	CACHE PATH "ranlib")
#set(CMAKE_LINKER 	"${UE_CLANG_TOOLCHAIN_BIN}/x86_64-unknown-linux-gnu-ld"		CACHE PATH "linker")
set(CMAKE_LINKER 	"${UE_CLANG_TOOLCHAIN_BIN}/ld.ldd"		CACHE PATH "linker")
#set(CMAKE_LINKER 	"${UE_CLANG_TOOLCHAIN_BIN}/ldd"		CACHE PATH "linker")
set(CMAKE_NM 		"${UE_CLANG_TOOLCHAIN_BIN}/x86_64-unknown-linux-gnu-gcc-nm"	CACHE PATH "nm")
set(CMAKE_OBJCOPY 	"${UE_CLANG_TOOLCHAIN_BIN}/llvm-objcopy"	CACHE PATH "objcopy")
set(CMAKE_OBJDUMP 	"${UE_CLANG_TOOLCHAIN_BIN}/x86_64-unknown-linux-gnu-objdump"	CACHE PATH "objdump")

set(UE_OPENSSL "${UE_THIRDPARTY}/OpenSSL/1.1.1t")

set(CMAKE_SYSTEM_INCLUDE_PATH "")
set(CMAKE_INCLUDE_PATH	"")
include_directories( BEFORE SYSTEM "${UE_CLANG_TOOLCHAIN}/usr/include")
include_directories( AFTER "${UE_THIRDPARTY}/Unix/LibCxx/include/c++/v1")
include_directories( AFTER "${UE_OPENSSL}/include/Unix")

#set(CLANG_LINK_DIRS "${UE_THIRDPARTY}/Unix/LibCxx/lib/Unix/x86_64-unknown-linux-gnu ${UE_CLANG_TOOLCHAIN}/lib ${UE_CLANG_TOOLCHAIN}/lib64 ${UE_CLANG_TOOLCHAIN}/usr/lib ${UE_CLANG_TOOLCHAIN}/usr/lib64")
#link_directories( "${CLANG_LINK_DIRS}")
link_directories( "${UE_THIRDPARTY}/Unix/LibCxx/lib/Unix/x86_64-unknown-linux-gnu")
link_directories( "${UE_OPENSSL}/lib/Unix/x86_64-unknown-linux-gnu")
link_directories( "${UE_CLANG_TOOLCHAIN}/lib")
link_directories( "${UE_CLANG_TOOLCHAIN}/lib64")
link_directories( "${UE_CLANG_TOOLCHAIN}/usr/lib")
link_directories( "${UE_CLANG_TOOLCHAIN}/usr/lib64")
#cmake_print_variables(LINK_DIRECTORIES)

set(CURL_LIBRARY "/work/ThirdParty/libcurl/8.4.0/lib/Unix/x86_64-unknown-linux-gnu/Release")
set(CURL_INCLUDE_DIR "/work/ThirdParty/libcurl/8.4.0/include")

set(SSL_ROOT_DIR "${UE_OPENSSL}")
set(OPENSSL_INCLUDE_DIR "${UE_OPENSSL}/include/Unix")
set(OPENSSL_SSL_LIBRARY "${UE_OPENSSL}/lib/Unix/x86_64-unknown-linux-gnu/libssl.a")
set(OPENSSL_CRYPTO_LIBRARY "${UE_OPENSSL}/lib/Unix/x86_64-unknown-linux-gnu/libcrypto.a")

set(COMPILER_FLAGS "-fuse-ld=lld -Wno-unused-command-line-argument -Wno-error=unused-command-line-argument --target=x86_64-unknown-linux-gnu --sysroot='${UE_CLANG_TOOLCHAIN}' -fno-math-errno -fno-rtti -fno-exceptions -funwind-tables -fPIC -nostdlib++")
#set(LINK_FLAGS "-fuse-ld=lld")
#set(LINK_OPTIONS "-fuse-ld=lld")
#set(CMAKE_C_FLAGS "-fuse-ld=lld")
#set(CMAKE_CXX_FLAGS "-stdlib=libc++ -nostdlib -nostdlib++ -std=c++20 -fuse-ld=lld")

#-L${UE_THIRDPARTY}/Unix/LibCxx/lib/Unix/x86_64-unknown-linux-gnu -lc++
set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -L${UE_THIRDPARTY}/Unix/LibCxx/lib/Unix/x86_64-unknown-linux-gnu -lc++ -lc++abi" CACHE STRING "" FORCE)

string(CONCAT CMAKE_CXX_FLAGS			"${CMAKE_CXX_FLAGS_INIT}		-std=c++20 ${COMPILER_FLAGS}")
string(CONCAT CMAKE_CXX_FLAGS_DEBUG		"${CMAKE_CXX_FLAGS_DEBUG_INIT}		${FLAGS_DEBUG}")
string(CONCAT CMAKE_CXX_FLAGS_MINSIZEREL	"${CMAKE_CXX_FLAGS_MINSIZEREL_INIT} 	${FLAGS_MINSIZEREL}")
string(CONCAT CMAKE_CXX_FLAGS_RELEASE		"${CMAKE_CXX_FLAGS_RELEASE_INIT}	${FLAGS_RELEASE}")
string(CONCAT CMAKE_CXX_FLAGS_RELWITHDEBINFO	"${CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT}	${FLAGS_RELWITHDEBINFO}")

string(CONCAT CMAKE_C_FLAGS			"${CMAKE_C_FLAGS_INIT}			${COMPILER_FLAGS}")
string(CONCAT CMAKE_C_FLAGS_DEBUG		"${CMAKE_C_FLAGS_DEBUG_INIT}		${FLAGS_DEBUG}")
string(CONCAT CMAKE_C_FLAGS_MINSIZEREL		"${CMAKE_C_FLAGS_MINSIZEREL_INIT}	${FLAGS_MINSIZEREL}")
string(CONCAT CMAKE_C_FLAGS_RELEASE		"${CMAKE_C_FLAGS_RELEASE_INIT}		${FLAGS_RELEASE}")
string(CONCAT CMAKE_C_FLAGS_RELWITHDEBINFO	"${CMAKE_C_FLAGS_RELWITHDEBINFO_INIT}	${FLAGS_RELWITHDEBINFO}")

#message( LINK_DIRECTORIES="${LINK_DIRECTORIES}")

#set(CMAKE_CXX_CREATE_STATIC_LIBRARY		"<CMAKE_AR> rcs <TARGET> <LINK_FLAGS> <OBJECTS>" CACHE STRING "" FORCE)

##set(CMAKE_CXX_FLAGS "--sysroot=/work/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu/ -nostdlib -nostdlib++ -std=c++20 -fuse-ld=lld")
#set(CMAKE_CXX_STANDARD "20")
## set(CMAKE_C_STANDARD_LIBRARIES "-lc -lm") 
## set(CMAKE_CXX_STANDARD_LIBRARIES "-lc -lm") 
#set(ENV{CC} /work/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu/bin/clang) 
#set(ENV{CXX} /work/v22_clang-16.0.6-centos7/x86_64-unknown-linux-gnu/bin/clang++) 
##set(CMAKE_EXE_LINKER_FLAGS "-lc++ -lc++abi -lc -lm ") 
##set(CMAKE_SHARED_LINKER_FLAGS "-lc++ -lc++abi -lc -lm ") 
#set(CMAKE_EXE_LINKER_FLAGS "/usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o -lc_nonshared -lc++ -lc++abi -lc -lm ") 
#set(CMAKE_SHARED_LINKER_FLAGS "/usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o -lc_nonshared -lc++ -lc++abi -lc -lm ") 
## set(CMAKE_POSITION_INDEPENDENT_CODE On) 
EOF

# Build abseil-cpp
WORKDIR /work/grpc/third_party/abseil-cpp/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DABSL_PROPAGATE_CXX_STD=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  .. && \
  cmake --build . -j ${CORES} --target install

# Build re2
WORKDIR /work/grpc/third_party/re2/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DRE2_BUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  .. && \
  cmake --build . -j ${CORES} --target install

# Build protobuf
WORKDIR /work/grpc/third_party/protobuf/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -Dprotobuf_ABSL_PROVIDER=package \
  -Dabsl_DIR="/opt/otel-cpp" \
  -Dprotobuf_BUILD_TESTS=false \
  -Dprotobuf_WITH_ZLIB=false \
  -Dprotobuf_BUILD_EXAMPLES=false \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  .. && \
  cmake --build . -j ${CORES} --target install

# Build zlib
WORKDIR /work/grpc/third_party/zlib/cmake/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DCMAKE_CXX_FLAGS="--std=c++20" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  ../.. && \
  cmake --build . -j ${CORES} --target install

# Build cares
WORKDIR /work/grpc/third_party/cares/cares/cmake/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DCMAKE_CXX_FLAGS="--std=c++20" \
  -DCARES_STATIC=ON \
  -DCARES_SHARED=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  ../.. && \
  cmake --build . -j ${CORES} --target install

# Build grpc
WORKDIR /work/grpc/build
RUN cmake \
  -DgRPC_INSTALL=ON \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
  -DCMAKE_POSITION_INDEPENDENT_CODE=TRUE \
  -DgRPC_USE_SYSTEMD=OFF \
  -DgRPC_BUILD_TESTS=OFF \
  -DgRPC_BUILD_CSHARP_EXT=OFF \
  -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
  -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
  -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
  -DgRPC_ABSL_PROVIDER=package \
  -Dabsl_DIR="/opt/otel-cpp" \
  -DgRPC_RE2_PROVIDER=package \
  -Dre2_DIR="/opt/otel-cpp/" \
  -DgRPC_PROTOBUF_PROVIDER=package \
  -Dprotobuf_DIR="/opt/otel-cpp/" \
  -DgRPC_ZLIB_PROVIDER=package \
  -DZLIB_DIR="/opt/otel-cpp/" \
  -DgRPC_CARES_PROVIDER=package \
  -Dc-ares_DIR="/opt/otel-cpp/" \
  -DgRPC_SSL_PROVIDER=package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  -DgRPC_BUILD_CODEGEN=ON \
  .. && \
  cmake --build . -j${CORES} --target install

# Build opentelemetry-cpp
WORKDIR /work/opentelemetry-cpp/build
RUN cmake \
  -DCMAKE_TOOLCHAIN_FILE=/work/toolchain.cmake \
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
  -DCMAKE_BUILD_TYPE=Release \
  -Dopentelemetry_ABSL_PROVIDER=package \
  -Dabsl_DIR="/opt/otel-cpp" \
  -Dopentelemetry_gRPC_PROVIDER=package \
  -DgRPC_DIR="/opt/otel-cpp" \
  -DCMAKE_INSTALL_PREFIX=/opt/otel-cpp \
  -DWITH_OTLP_GRPC=ON \
  -DWITH_OTLP_HTTP=ON \
  -DBUILD_STATIC_LIBS=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PREFIX_PATH="/opt/otel-cpp" \
  -DCMAKE_INSTALL_PREFIX="/opt/otel-cpp" \
  .. && \
  cmake --build . -j ${CORES} --target install

FROM scratch
COPY --from=build /opt/otel-cpp /opt/otel-cpp
CMD [""]
