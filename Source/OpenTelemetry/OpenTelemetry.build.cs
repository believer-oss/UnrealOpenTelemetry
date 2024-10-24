using UnrealBuildTool;

public class OpenTelemetry : ModuleRules
{
	public OpenTelemetry(ReadOnlyTargetRules Target) : base(Target)
	{
		DefaultBuildSettings = BuildSettingsVersion.V2;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"GameplayTags"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Analytics",
			"Engine",
			"CoreUObject",
			"RenderCore",
			"RHI",
			"libotel",
		});

		// Make sure headers are using the same definitions as the built otel libs
		PrivateDefinitions.AddRange(new string[] {
			"OPENTELEMETRY_NO_DEPRECATED_CODE",
			"OPENTELEMETRY_STL_VERSION=2017",
			"HAVE_MSGPACK",
			"OPENTELEMETRY_ABI_VERSION_NO=1",
		});
	}
}
