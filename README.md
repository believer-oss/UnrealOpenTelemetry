# OpenTelemetry Plugin

This plugin integrates the [OpenTelemetry C++](https://github.com/open-telemetry/opentelemetry-cpp) library and provides Unreal-native wrappers for the API.

## Installation

Copy the entire repo (without the .git directory) into your project's `Plugins/` folder, and enable the plugin in your `.uproject`
file like so:

```
{
	"Plugins":
	[
		{
			"Name": "OpenTelemetry",
			"Enabled": true
		}
	]
}
```

## Configuration

To specify the destination endpoint, auth headers, service name, and other attributes, you will need to add a `DefaultOtel.ini` into
your project's `Config/`. There is an example config provided at `Config/DefaultOtel.ini`.

## Building

This project intentionally does not provide prebuilt OpenTelemetry libraries for security reasons. You will need to build it as well as all dependencies
yourself and put them into the appropriate platform folders:
- Windows: `Source/ThirdParty/libotel/1.16.1/lib/Win64/Release`
- Linux: `Source/ThirdParty/libotel/1.16.1/lib/Linux/Release`
See `libotel.Build.cs` for a list of all required libraries.

For convenience, we provide [Docker](https://www.docker.com/) files that are based on the [ue4-docker](https://github.com/adamrehn/ue4-docker) project. The Otel docker files expect there to be a `ThirdParty/` directory with the `OpenSSL/` and `zlib/` folders copied from the Unreal Engine your project is built with.

Once you have the `ThirdParty/` directory in place, run the build script, which will build the docker image and copy the built libraries into the appropriate location.
- Windows: `Source/ThirdParty/libotel/1.16.1/Build/Windows/build.ps1`
- Linux: `Source/ThirdParty/libotel/1.16.1/Build/Linux/build.sh`

## Testing

Also provided is a script and config file to run a local otel collector so you can iterate locally. Simply run:
```powershell
cd Test
.\run-container.ps1
```
