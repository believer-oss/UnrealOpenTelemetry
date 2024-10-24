if ((Test-Path ThirdParty) -eq $False) {
    Write-Output "Copy the OpenSSL and zlib libraries from the Unreal Engine ThirdParty directory into a ThirdParty directory located here."
    return
}
if (Test-Path otel-cpp) {
    Remove-Item -Recurse otel-cpp\
}
docker build -t otel-cpp-build -f Dockerfile .
docker create --name=otel-cpp otel-cpp-build
docker cp otel-cpp:C:\work\otel-cpp\ otel-cpp\
docker rm otel-cpp
Copy-Item otel-cpp\lib\*.lib ..\..\1.16.1\lib\Win64\Release
if (Test-Path otel-cpp) {
    Remove-Item -Recurse otel-cpp\
}
