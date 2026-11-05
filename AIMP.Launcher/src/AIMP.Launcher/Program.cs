using System.Text.Json.Serialization;
using AIMP.Launcher.Services;
using Microsoft.AspNetCore.Diagnostics;

namespace AIMP.Launcher;

public static class Program
{
    public static void Main(string[] args)
    {
        if (WorldManifestCli.TryRun(args, out var toolExitCode))
        {
            Environment.ExitCode = toolExitCode;
            return;
        }

        var hostArguments = LauncherStartupOptions.Parse(args, out var developerModeAvailable);
        var accessPolicy = new LocalApiAccessPolicy(
            Environment.GetEnvironmentVariable("AIMP_FRONTEND_ORIGIN"), Environment.GetEnvironmentVariable("AIMP_API_TOKEN"));
        Environment.SetEnvironmentVariable("AIMP_API_TOKEN", null);
        var builder = WebApplication.CreateBuilder(hostArguments);
        ConfigureServices(builder.Services, developerModeAvailable, accessPolicy);

        var app = builder.Build();
        ConfigurePipeline(app, accessPolicy);
        app.Run();
    }

    internal static void ConfigureApiSecurityServices(IServiceCollection services, LocalApiAccessPolicy accessPolicy)
    {
        services.AddCors(options => options.AddDefaultPolicy(policy => policy
            .SetIsOriginAllowed(accessPolicy.IsAllowedOrigin)
            .AllowAnyHeader()
            .AllowAnyMethod()));
    }

    private static void ConfigureServices(
        IServiceCollection services, bool developerModeAvailable, LocalApiAccessPolicy accessPolicy)
    {
        services.AddProblemDetails();
        ConfigureApiSecurityServices(services, accessPolicy);
        services.AddControllers().AddJsonOptions(options =>
            options.JsonSerializerOptions.Converters.Add(new JsonStringEnumConverter()));

        services.AddSingleton(_ => new LauncherDiagnostics());
        services.AddSingleton(_ => new LauncherSettingsStore(developerModeAvailable: developerModeAvailable,
            steamPersona: SteamPersona.Resolve));
        services.AddSingleton(provider => new GamePreparationService(
            provider.GetRequiredService<LauncherSettingsStore>(),
            provider.GetRequiredService<LauncherDiagnostics>(),
            provider.GetRequiredService<IHostApplicationLifetime>()));
        services.AddSingleton<NativeRuntimeController>(provider => new NativeRuntimeController(
            provider.GetRequiredService<LauncherDiagnostics>(),
            provider.GetRequiredService<LauncherSettingsStore>()));
        services.AddSingleton(provider => new ScaleformBridgeListener(
            provider.GetRequiredService<LauncherDiagnostics>()));
        services.AddSingleton(_ => new PerformanceIsolationState());
        services.AddSingleton(provider => new MultiplayerSessionCoordinator(
            provider.GetRequiredService<NativeRuntimeController>(),
            provider.GetRequiredService<LauncherSettingsStore>(),
            provider.GetRequiredService<ScaleformBridgeListener>(),
            provider.GetRequiredService<PerformanceIsolationState>(),
            provider.GetRequiredService<GamePreparationService>(),
            new SteamRelay(provider.GetRequiredService<LauncherDiagnostics>())));
        services.AddSingleton(provider => new UiPakInstaller(
            provider.GetRequiredService<LauncherDiagnostics>()));
        services.AddSingleton(provider => new GameLaunchCoordinator(
            provider.GetRequiredService<LauncherSettingsStore>(),
            provider.GetRequiredService<UiPakInstaller>(),
            provider.GetRequiredService<ScaleformBridgeListener>(),
            provider.GetRequiredService<LauncherDiagnostics>(),
            provider.GetRequiredService<GamePreparationService>()));
        services.AddSingleton(provider => new LauncherRuntimeService(
            provider.GetRequiredService<NativeRuntimeController>(),
            provider.GetRequiredService<LauncherSettingsStore>(),
            provider.GetRequiredService<ScaleformBridgeListener>(),
            provider.GetRequiredService<GameLaunchCoordinator>(),
            provider.GetRequiredService<MultiplayerSessionCoordinator>(),
            provider.GetRequiredService<PerformanceIsolationState>(),
            provider.GetRequiredService<GamePreparationService>(),
            provider.GetRequiredService<WorldManifestService>()));
        services.AddHostedService(provider => provider.GetRequiredService<LauncherRuntimeService>());
        services.AddSingleton(provider => new WorldManifestService(
            provider.GetRequiredService<LauncherSettingsStore>(),
            provider.GetRequiredService<LauncherDiagnostics>(),
            provider.GetRequiredService<IHostApplicationLifetime>()));
    }

    internal static void ConfigurePipeline(WebApplication app, LocalApiAccessPolicy accessPolicy)
    {
        app.UseExceptionHandler(errorApp => errorApp.Run(async context =>
        {
            var error = context.Features.Get<IExceptionHandlerFeature>()?.Error;
            var status = error switch
            {
                ArgumentException => StatusCodes.Status400BadRequest,
                FileNotFoundException => StatusCodes.Status400BadRequest,
                DirectoryNotFoundException => StatusCodes.Status400BadRequest,
                UnauthorizedAccessException => StatusCodes.Status403Forbidden,
                KeyNotFoundException => StatusCodes.Status404NotFound,
                InvalidOperationException => StatusCodes.Status409Conflict,
                _ => StatusCodes.Status500InternalServerError
            };
            context.Response.StatusCode = status;
            await Results.Problem(
                statusCode: status,
                title: status == StatusCodes.Status500InternalServerError
                    ? "AIMP Launcher API error"
                    : "The requested operation could not be completed.",
                detail: error?.Message ?? "An unexpected error occurred.")
                .ExecuteAsync(context);
        }));

        // CORS only controls whether a browser can read a response. Reject
        // untrusted requests before they can read files or execute actions.
        app.Use(accessPolicy.EnforceAsync);
        app.UseCors();
        app.MapControllers();
        app.MapGet("/", () => Results.Ok(new
        {
            name = "AIMP Launcher API",
            version = ProductInfo.LauncherVersion,
            apiBase = "/api/v1",
            localOnly = true
        }));
    }
}
