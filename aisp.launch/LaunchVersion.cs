using System.Reflection;

namespace aisp.launch;

internal static class LaunchVersion
{
    public static string Display { get; } =
        Assembly
            .GetExecutingAssembly()
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion
        ?? "dev";

    public static string WindowTitle => $"aisp.launch - {Display}";
}
