using AIMP.Launcher;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using System.Buffers.Binary;

namespace AIMP.Protocol.Tests;

internal static class MissionFrontierTests
{
    private static uint Port(string name) => ShortGuidUtils.Generate(name).AsUInt32;
    private static readonly WorldEntityId Source = new(1, 2), Objective = new(3, 4), Door = new(5, 6);

    private static WorldEntityDescriptor Entity(WorldEntityId id, WorldEntityKind kind, WorldDomain domain) =>
        new(id, domain, kind, WorldEntityFlags.RuntimeAddressable, 100, 200, 1, 0, 200, 300, 0, 0, "", "");

    private static WorldManifest Manifest() => new(WorldManifestConstants.FormatVersion,
        WorldManifestConstants.CompilerVersion, 10, 11, 100, 0, 0,
        new WorldManifestSourceInfo("", "", true, false, true, "", ""),
        [new WorldCompositeDescriptor(100, "SCRIPT_StoryMission/M01_Pt01", WorldDomain.World, false, true)], [],
        [Entity(Source, WorldEntityKind.CompositeInstance, WorldDomain.World),
            Entity(Objective, WorldEntityKind.Function, WorldDomain.UI),
            Entity(Door, WorldEntityKind.Proxy, WorldDomain.World)], [], [], [],
        [new WorldConnectionDescriptor(Source, Port("key_accepted"), Objective, Port("trigger"), false),
            new WorldConnectionDescriptor(Source, Port("key_accepted"), Door, Port("force_open"), true)], [], [], [], 0, 0, []);

    internal static Task SelectionAsync()
    {
        var manifest = Manifest();
        var roots = MissionSyncFrontierCompiler.Compile(manifest);
        AssertEx.Equal(1, roots.Count); // UI objective is part of the same authored fan-out.
        AssertEx.Equal(Source, roots[0].Source.EntityId);
        // A campaign mission output feeding exclusively a stateless control
        // input is safe to replay without its sender-local callback reference.
        var controlOnly = manifest with { Connections = manifest.Connections
            .Where(value => value.TargetEntity == Objective).ToArray() };
        var campaign = MissionSyncFrontierCompiler.Compile(controlOnly);
        AssertEx.Equal(1, campaign.Count);
        AssertEx.True(campaign[0].PayloadTolerant);
        AssertEx.True(campaign[0].OrderedOccurrences);
        var typedBranch = manifest with { Connections = manifest.Connections.Append(
            new WorldConnectionDescriptor(Source, Port("key_accepted"), Door, Port("position"), true)).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(typedBranch).Count);
        var nonMission = manifest with { Composites = [manifest.Composites[0] with { Name = "UTILITY/Keypad" }] };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(nonMission).Count);
        var localOnly = manifest with { Entities = manifest.Entities.Select(value =>
            value.EntityId == Source ? value with { Flags = WorldEntityFlags.None } : value).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(localOnly).Count);
        var aimp = manifest with { Entities = manifest.Entities.Select(value =>
            value.EntityId == Door ? value with { Domain = WorldDomain.AIMP } : value).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(aimp).Count);
        return Task.CompletedTask;
    }

    internal static Task SurvivalAsync()
    {
        // ChallengeMap16 ships DLC\\ChallengeMap16\\MISSION_ChallengeMap16,
        // not SCRIPT_StoryMission. Progression is an objective output, not a
        // local generator "used" interaction or a player-trigger edge.
        var story = Manifest();
        var objective = Port("obj1Completed");
        var script = story.Composites[0] with
        {
            Name = @"DLC\ChallengeMap16\MISSION_ChallengeMap16"
        };
        var survival = story with
        {
            Composites = [script],
            Connections =
            [
                new WorldConnectionDescriptor(Source, objective, Objective, Port("trigger"), false),
                new WorldConnectionDescriptor(Source, objective, Door, Port("force_open"), true)
            ]
        };
        var roots = MissionSyncFrontierCompiler.Compile(survival, survival: true);
        AssertEx.Equal(1, roots.Count);
        AssertEx.Equal(Source, roots[0].Source.EntityId);
        AssertEx.Equal(objective, roots[0].Signal);
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(survival).Count);
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(story, survival: true).Count);

        // A different Survival map and objective number must use exactly
        // the same compiler path: no per-level ID, no fixed objective slots.
        var differentLevel = survival with
        {
            Composites = [script with { Name = @"Mods\SurvivalScenario\MainMissionLogic" }],
            Connections = [
                new WorldConnectionDescriptor(Source, Port("obj17Completed"), Objective, Port("trigger"), false)
            ]
        };
        var otherRoots = MissionSyncFrontierCompiler.Compile(differentLevel, survival: true);
        AssertEx.Equal(1, otherRoots.Count);
        AssertEx.Equal(Port("obj17Completed"), otherRoots[0].Signal);

        // Arbitrary authored output vocabulary is supported if its complete
        // fan-out includes a proven objective sink and no player-local input.
        var semanticObjective = survival with
        {
            Composites = [script with { Name = @"Mods\SurvivalScenario\GenericLogic" }],
            Entities = survival.Entities.Select(value => value.EntityId == Objective
                ? value with { PrimitiveName = "ObjectiveState" } : value).ToArray(),
            Connections = [
                new WorldConnectionDescriptor(Source, Port("myCustomObjectiveEvent"), Objective, Port("trigger"), false)
            ]
        };
        var semanticRoots = MissionSyncFrontierCompiler.Compile(semanticObjective, survival: true);
        AssertEx.Equal(1, semanticRoots.Count);
        AssertEx.Equal(Port("myCustomObjectiveEvent"), semanticRoots[0].Signal);
        // An arbitrary output routed to a generic World entity has no proof
        // of progression and is intentionally kept local.
        var noObjective = semanticObjective with
        {
            Connections = [
                new WorldConnectionDescriptor(Source, Port("myCustomObjectiveEvent"), Door, Port("trigger"), true)
            ]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(noObjective, survival: true).Count);

        // Raw interaction "used" must never replay its sender Character.
        var interaction = survival with
        {
            Connections =
            [
                new WorldConnectionDescriptor(Source, Port("used"), Door, Port("trigger"), true)
            ]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(interaction, survival: true).Count);
        var objectiveInteraction = semanticObjective with
        {
            Connections = [
                new WorldConnectionDescriptor(Source, Port("used"), Objective, Port("trigger"), false)
            ]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(objectiveInteraction, survival: true).Count);
        var misleadingName = survival with
        {
            Composites = [script with { Name = @"Mods\MyPermissionController" }]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(misleadingName, survival: true).Count);

        // Reject the ENTIRE output fan-out if any branch can affect a local
        // player, an AI actor or an unproved/unknown semantic target.
        foreach (var domain in new[] { WorldDomain.Player, WorldDomain.AI, WorldDomain.Unknown })
        {
            var unsafeManifest = survival with
            {
                Entities = survival.Entities.Select(value => value.EntityId == Door
                    ? value with { Domain = domain } : value).ToArray()
            };
            AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(unsafeManifest, survival: true).Count);
        }
        var dataDependent = survival with
        {
            Connections = survival.Connections.Append(new WorldConnectionDescriptor(
                Source, objective, Door, Port("position"), true)).ToArray()
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(dataDependent, survival: true).Count);
        var noRuntime = survival with
        {
            Entities = survival.Entities.Select(value => value.EntityId == Source
                ? value with { Flags = WorldEntityFlags.None } : value).ToArray()
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(noRuntime, survival: true).Count);
        return Task.CompletedTask;
    }

    // SalvageMode1 shape: the hub terminal's chosen TriggerSimple in
    // MISSION_SalvageMode1 starts a nested MISSION_ChallengeMap10 instance.
    // Starting a mission is a shared, repeatable milestone; a non-mission
    // target or a Campaign catalogue keeps it local.
    // ChallengeMap11 shape: Alarm PlayerTriggerBox.on_entered -> LogicOnce ->
    // LogicDelay.triggered -> Xenomorph_NPC.spawn_npc (AI domain). Only the
    // most upstream mission output commits; the volume and relay stay local.
    internal static Task NpcSpawnAsync()
    {
        WorldEntityId box = new(9, 1), once = new(9, 2), delay = new(9, 3), alarm = new(9, 4), alien = new(9, 5);
        WorldEntityDescriptor At(WorldEntityId id, WorldEntityKind kind, WorldDomain domain, string primitive) =>
            new(id, domain, kind, WorldEntityFlags.RuntimeAddressable, 100, (uint)id.Low, 1, 0,
                (uint)id.Low, 300, 0, 0, primitive, "");
        var trigger = Port("trigger");
        var manifest = Manifest() with
        {
            Composites = [new WorldCompositeDescriptor(100, @"DLC\ChallengeMap11\MISSION_ChallengeMap11", WorldDomain.World, false, true)],
            Entities =
            [
                At(box, WorldEntityKind.Function, WorldDomain.World, "PlayerTriggerBox"),
                At(once, WorldEntityKind.Function, WorldDomain.World, "LogicOnce"),
                At(delay, WorldEntityKind.Function, WorldDomain.World, "LogicDelay"),
                At(alarm, WorldEntityKind.Function, WorldDomain.World, "LogicDelay"),
                At(alien, WorldEntityKind.CompositeInstance, WorldDomain.AI, @"Archetypes\NPCs\Alien\Xenomorph_NPC")
            ],
            Connections =
            [
                new WorldConnectionDescriptor(box, Port("on_entered"), once, trigger, false),
                new WorldConnectionDescriptor(once, Port("on_success"), delay, trigger, false),
                new WorldConnectionDescriptor(delay, Port("triggered"), alarm, trigger, false),
                new WorldConnectionDescriptor(delay, Port("triggered"), alien, Port("spawn_npc"), false)
            ]
        };
        var roots = MissionSyncFrontierCompiler.Compile(manifest, survival: true);
        AssertEx.Equal(1, roots.Count);
        AssertEx.Equal(once, roots[0].Source.EntityId);
        AssertEx.Equal(Port("on_success"), roots[0].Signal);
        // A chain that never spawns an NPC is not a milestone.
        var noSpawn = manifest with { Connections = manifest.Connections.Take(3).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(noSpawn, survival: true).Count);
        // An unsafe fan-out on the upstream output leaves the relay as the root.
        var unsafeOnce = manifest with { Connections = manifest.Connections.Append(
            new WorldConnectionDescriptor(once, Port("on_success"), alarm, Port("position"), true)).ToArray() };
        var relay = MissionSyncFrontierCompiler.Compile(unsafeOnce, survival: true);
        AssertEx.Equal(1, relay.Count);
        AssertEx.Equal(delay, relay[0].Source.EntityId);
        // Outside a mission graph nothing is replicated.
        var nonMission = manifest with { Composites = [manifest.Composites[0] with { Name = @"AYZ\Props\Alarm" }] };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(nonMission, survival: true).Count);
        return Task.CompletedTask;
    }

    internal static Task MissionStartAsync()
    {
        WorldEntityId trigger = new(9, 1), mission = new(9, 2);
        WorldEntityDescriptor At(WorldEntityId id, WorldEntityKind kind, string primitive) =>
            new(id, WorldDomain.World, kind, WorldEntityFlags.RuntimeAddressable, 100, (uint)id.Low, 1, 0,
                (uint)id.Low, 300, 0, 0, primitive, "");
        var start = Manifest() with
        {
            Composites = [new WorldCompositeDescriptor(100, @"DLC\SalvageMode1\MISSION_SalvageMode1", WorldDomain.World, false, true)],
            Entities =
            [
                At(trigger, WorldEntityKind.Function, "TriggerSimple"),
                At(mission, WorldEntityKind.CompositeInstance, @"DLC\ChallengeMap10\MISSION_ChallengeMap10")
            ],
            Connections = [new WorldConnectionDescriptor(trigger, Port("triggered"), mission, Port("MissionStart"), false)]
        };
        var roots = MissionSyncFrontierCompiler.Compile(start, survival: true);
        AssertEx.Equal(1, roots.Count);
        AssertEx.Equal(trigger, roots[0].Source.EntityId);
        AssertEx.Equal(Port("triggered"), roots[0].Signal);
        AssertEx.True(roots[0].Repeatable);
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(start).Count);
        var notMission = start with
        {
            Entities = start.Entities.Select(value => value.EntityId == mission
                ? value with { PrimitiveName = @"AYZ\_Props_\Interactive\Door_Controller" } : value).ToArray()
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(notMission, survival: true).Count);
        var notFromMission = start with
        {
            Composites = [start.Composites[0] with { Name = @"DLC\SalvageMode1\HUB_SalvageMode1" }]
        };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(notFromMission, survival: true).Count);
        return Task.CompletedTask;
    }

    // ChallengeMap16 shape: MISSION_ChallengeMap16 observes the generator through
    // Proxy<Power_Generator_Control>.complete, which arms the next PlayerTriggerBox
    // and sets the next SetSubObjective. Both the Proxy and the proxied instance
    // are frontiers so either native emission identity is captured once.
    internal static Task ProxyAsync()
    {
        WorldEntityId proxy = new(7, 1), objective = new(7, 2), box = new(7, 3),
            generator = new(7, 4), delay = new(7, 5);
        WorldEntityDescriptor At(WorldEntityId id, WorldEntityKind kind, WorldDomain domain,
            uint composite, uint guid, uint path, string primitive) =>
            new(id, domain, kind, WorldEntityFlags.RuntimeAddressable, composite, guid, path, 0,
                guid, 300, 0, 0, primitive, "");
        var complete = Port("complete");
        var baseline = Manifest() with
        {
            Composites =
            [
                new WorldCompositeDescriptor(100, @"DLC\ChallengeMap16\MISSION_ChallengeMap16", WorldDomain.World, false, true),
                new WorldCompositeDescriptor(101, @"AYZ\Levels\DLC_ChallengeMaps\ChallengMap_16_Tech_Section", WorldDomain.World, false, true)
            ],
            InstancePaths = [new WorldInstancePathDescriptor(7, [0x8823D1E4U, 0x100EFE12U], 0, 0)],
            Entities =
            [
                At(proxy, WorldEntityKind.Proxy, WorldDomain.World, 100, 0xFF104B68U, 1, "Proxy_0x1A410713"),
                At(objective, WorldEntityKind.Function, WorldDomain.UI, 100, 0xC91C5E00U, 1, "SetSubObjective"),
                At(box, WorldEntityKind.Function, WorldDomain.World, 100, 0x985002F8U, 1, "PlayerTriggerBox"),
                At(generator, WorldEntityKind.CompositeInstance, WorldDomain.World, 101, 0x338558FCU, 7,
                    @"AYZ\_Props_\Interactive\Generic\Power_Generator_Control"),
                At(delay, WorldEntityKind.Function, WorldDomain.World, 101, 0x40232208U, 7, "LogicDelay")
            ],
            References = [new WorldReferenceDescriptor(proxy, WorldReferenceKind.Proxy, 0x1A410713U,
                [0xB5D0AE3EU, 0x8823D1E4U, 0x100EFE12U, 0x338558FCU, 0U])],
            Connections =
            [
                new WorldConnectionDescriptor(proxy, complete, objective, Port("trigger"), false),
                new WorldConnectionDescriptor(proxy, complete, box, Port("enable"), true),
                new WorldConnectionDescriptor(generator, complete, delay, Port("trigger"), true)
            ]
        };
        var roots = MissionSyncFrontierCompiler.Compile(baseline, survival: true);
        AssertEx.Equal(2, roots.Count);
        AssertEx.True(roots.All(value => value.Signal == complete && value.PayloadTolerant));
        AssertEx.True(roots.Select(value => value.Source.EntityId).OrderBy(value => value.Low)
            .SequenceEqual([proxy, generator]));
        // Campaign contract is unchanged.
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(baseline).Count);

        // A proximity volume may be armed, never pulsed/replayed.
        var pulsed = baseline with { Connections = baseline.Connections.Append(
            new WorldConnectionDescriptor(proxy, complete, box, Port("trigger"), true)).ToArray() };
        AssertEx.Equal(0, MissionSyncFrontierCompiler.Compile(pulsed, survival: true).Count);

        // An unsafe fan-out on the proxied instance keeps only the Proxy frontier.
        var unsafeTarget = baseline with { Connections = baseline.Connections.Append(
            new WorldConnectionDescriptor(generator, complete, delay, Port("position"), true)).ToArray() };
        var proxyOnly = MissionSyncFrontierCompiler.Compile(unsafeTarget, survival: true);
        AssertEx.Equal(1, proxyOnly.Count);
        AssertEx.Equal(proxy, proxyOnly[0].Source.EntityId);

        // Ambiguous or unresolved proxy paths are never replay authority.
        var unresolved = baseline with { References = [baseline.References[0] with
            { TargetPathSteps = [0xB5D0AE3EU, 0x12345678U, 0U] }] };
        AssertEx.Equal(1, MissionSyncFrontierCompiler.Compile(unresolved, survival: true).Count);

        // A data-carrying Function source keeps the strict payloadless contract.
        var function = baseline with { Entities = baseline.Entities.Select(value => value.EntityId == proxy
            ? value with { Kind = WorldEntityKind.Function, PrimitiveName = "VariableBool" } : value).ToArray() };
        var functionRoots = MissionSyncFrontierCompiler.Compile(function, survival: true);
        AssertEx.Equal(1, functionRoots.Count);
        AssertEx.True(!functionRoots[0].PayloadTolerant);
        return Task.CompletedTask;
    }

    // Opt-in validation with any extracted level's COMMANDS source. Synthetic
    // manifests test the generic boundary contract; retail coverage is checked
    // separately before claiming that a particular objective is replicable.
    internal static Task RetailMissionGraphAsync()
    {
        var cached = Environment.GetEnvironmentVariable("AIMP_TEST_MISSION_MANIFEST");
        if (!string.IsNullOrWhiteSpace(cached))
        {
            // An existing WorldManifestCache .aimpwm avoids recompiling COMMANDS.
            var roots = MissionSyncFrontierCompiler.Compile(WorldManifestBinaryCodec.Read(cached), survival: true);
            foreach (var root in roots)
                Console.WriteLine($"  frontier {root.Source.EntityId} {root.Source.Kind} " +
                    $"{root.Source.PrimitiveName} port=0x{root.Signal:X8} payloadTolerant={root.PayloadTolerant}");
            AssertEx.True(roots.Count != 0, "The cached manifest has zero proven objective frontiers.");
            return Task.CompletedTask;
        }
        var level = Environment.GetEnvironmentVariable("AIMP_TEST_MISSION_LEVEL");
        if (string.IsNullOrWhiteSpace(level))
        {
            Console.WriteLine("SKIP MissionSync retail graph: AIMP_TEST_MISSION_LEVEL is not set.");
            return Task.CompletedTask;
        }
        var temporary = Path.Combine(Path.GetTempPath(), $"aimp-mission-test-{Guid.NewGuid():N}.aimpwm");
        try
        {
            var manifest = new WorldManifestCompiler().Compile(level, outputPath: temporary, useCache: false).Manifest;
            var roots = MissionSyncFrontierCompiler.Compile(manifest, survival: true);
            AssertEx.True(roots.Count != 0,
                "The selected level has zero proven objective frontiers. " +
                "Do not claim objective coverage before inspecting its COMMANDS graph.");
            Console.WriteLine($"Survival MissionSync: {roots.Count} graph-proven objective frontiers.");
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        return Task.CompletedTask;
    }

    internal static Task BinaryAsync()
    {
        var manifest = Manifest();
        var roots = MissionSyncFrontierCompiler.Compile(manifest);
        var bytes = MissionSyncFrontierCompiler.Encode(manifest, roots);
        AssertEx.Equal(56, bytes.Length);
        AssertEx.Equal(0x32534D41U, BinaryPrimitives.ReadUInt32LittleEndian(bytes));
        AssertEx.Equal(1U, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(20)));
        AssertEx.Equal(Source.High, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24)));
        AssertEx.Equal(Port("key_accepted"), BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(48)));
        AssertEx.Equal(0U, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(52)));
        var tolerant = MissionSyncFrontierCompiler.Encode(manifest, [roots[0] with { PayloadTolerant = true }]);
        AssertEx.Equal(MissionSyncFrontierCompiler.FlagPayloadTolerant,
            BinaryPrimitives.ReadUInt32LittleEndian(tolerant.AsSpan(52)));
        var reordered = manifest with { Connections = manifest.Connections.Reverse().ToArray(),
            Entities = manifest.Entities.Reverse().ToArray() };
        AssertEx.True(bytes.SequenceEqual(MissionSyncFrontierCompiler.Encode(reordered,
            MissionSyncFrontierCompiler.Compile(reordered))));
        ModelValidation.Validate(new SessionPreparedPayload(Guid.NewGuid(), 1, 1, new string('A', 64)));
        AssertEx.Throws<ProtocolException>(() => ModelValidation.Validate(new SessionPreparedPayload(Guid.NewGuid(), 1, 1, "bad")));
        return Task.CompletedTask;
    }
}
