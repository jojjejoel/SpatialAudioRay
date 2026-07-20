using UnrealBuildTool;

public class SpatialAudioRay : ModuleRules
{
	public SpatialAudioRay(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange([
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore"
		]);

		PrivateDependencyModuleNames.AddRange([
			"Slate",
			"SlateCore"
		]);

		// Flat include layout carried over from the original game module: consumers
		// include both "SpatialAudioTypes.h" and "Audio/SpatialAudioSourceActor.h".
		PublicIncludePaths.AddRange([
			ModuleDirectory,
			ModuleDirectory + "/Audio",
			ModuleDirectory + "/Voice"
		]);
	}
}
