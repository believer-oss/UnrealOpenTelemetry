using UnrealBuildTool;

public class OpenTelemetryEditor : ModuleRules
{
	public OpenTelemetryEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		DefaultBuildSettings = BuildSettingsVersion.V2;

		PublicDependencyModuleNames.AddRange(new string[] {
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Analytics",
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",

			"OpenTelemetry",
		});
	}
}
