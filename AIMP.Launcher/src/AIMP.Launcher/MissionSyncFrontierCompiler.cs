using AIMP.Protocol;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;

namespace AIMP.Launcher;

// PayloadTolerant: the source is a Composite/Proxy interface pin and every
// target is a compiler-proven control input. The native runtime may attach the
// pin's data reference to that emission, but no receiver consumes it.
// Repeatable: a player-chosen nested mission start (a hub terminal) can be
// committed again later in the same round; other frontiers commit once.
internal readonly record struct MissionSyncFrontier(
    WorldEntityDescriptor Source, uint Signal, bool PayloadTolerant = false, bool Repeatable = false,
    bool PublicInput = false, bool OrderedOccurrences = false, bool ScheduledInput = false,
    bool PlayerIngress = false);

internal static class MissionSyncFrontierCompiler
{
    // "AMS2": v1 entries plus a uint flags word. Must match AIMP.Native MissionSync.
    internal const uint CatalogMagic = 0x32534D41U;
    internal const uint FlagPayloadTolerant = 1U;
    internal const uint FlagPublicInput = 2U;
    internal const uint FlagOrderedOccurrences = 4U;
    internal const uint FlagScheduledInput = 8U;
    internal const uint FlagPlayerIngress = 16U;

    private static readonly HashSet<uint> CompletionPorts = new[]
    {
        "used", "key_accepted", "completed", "finished", "animation_finished", "on_entered", "on_exited"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();
    // Survival progression is compiled from the manifest's authored graph.
    // These are reusable CATHODE signal *families*, not level names or a list
    // of hardcoded map objectives. Unknown output names can also qualify when
    // their verified targets include a semantic objective/mission-complete sink.
    private static readonly HashSet<uint> SurvivalCompletionPorts = new[]
    {
        "complete", "completed", "finished", "success",
        "objectiveComplete", "objectiveCompleted", "objective_complete",
        "missionComplete", "missionCompleted", "mission_complete",
        "levelComplete", "levelCompleted", "level_complete"
    }.Concat(Enumerable.Range(1, 64).SelectMany(index => new[]
    {
        $"obj{index}Completed", $"obj{index}Complete",
        $"objective{index}Completed", $"objective{index}Complete",
        $"objective_{index}_complete", $"objective_{index}_completed"
    })).Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // These events can represent the interacting Character or an animation
    // rather than a portable milestone. Never suppress/replay them in Survival.
    private static readonly HashSet<uint> SurvivalLocalInteractionPorts = new[]
    {
        "used", "interact", "interaction", "on_entered", "on_exited",
        "animation_finished", "pressed", "activated", "deactivated",
        "entered", "exited", "start", "started"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // Only stateless control inputs are replayable without the sender-local
    // callback/Character. Indexed objective inputs are a *generic* convention.
    private static readonly HashSet<uint> SurvivalControlInputs = new[]
    {
        "complete", "completed", "finished", "success",
        "objectiveComplete", "objectiveCompleted", "objective_complete",
        "missionComplete", "missionCompleted", "mission_complete",
        "levelComplete", "levelCompleted", "level_complete",
        "gotoEndScreen", "showSplash", "setObjIcons"
    }.Concat(Enumerable.Range(1, 64).SelectMany(index => new[]
    {
        $"Objective {index} Complete", $"obj{index}Completed",
        $"obj{index}Complete", $"objective{index}Completed",
        $"objective{index}Complete"
    })).Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // A mission graph that starts a nested mission composite (e.g. a Salvage
    // hub terminal choosing which challenge to run) commits a shared
    // milestone: every participant must run the same mission. Only the start
    // input family of a mission-graph CompositeInstance qualifies.
    private static readonly HashSet<uint> MissionStartInputs = new[]
    {
        "MissionStart", "missionStart", "mission_start", "StartMission", "start_mission"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // Port names are authored control contracts, not the World-domain causal hint.
    // In particular objective triggers belong to UI and carry PotentialCausalEdge=false.
    private static readonly HashSet<uint> ControlInputs = new[]
    {
        "trigger", "start", "force_open", "force_close", "enable", "disable", "activate", "deactivate"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    // Arming a proximity volume is a shared World consequence of an objective
    // (e.g. the next objective's trigger box). Its on_entered output stays
    // per-player and is never a frontier source.
    private static readonly HashSet<uint> ArmingInputs = new[]
    {
        "enable", "disable", "activate", "deactivate"
    }.Select(name => ShortGuidUtils.Generate(name).AsUInt32).ToHashSet();

    internal static IReadOnlyList<MissionSyncFrontier> Compile(
        WorldManifest manifest, bool survival = false)
    {
        var entities = manifest.Entities.ToDictionary(value => value.EntityId);
        var admitted = survival ? DirectFrontiers(manifest, entities, true)
            : CampaignFrontiers(manifest, entities);
        if (survival) admitted.AddRange(ProxiedFrontiers(manifest, entities, admitted));
        if (survival) admitted.AddRange(NpcSpawnFrontiers(manifest, entities));
        var roots = admitted
            .GroupBy(value => (value.Source.EntityId, value.Signal, value.PublicInput, value.ScheduledInput)).Select(group => group.First())
            .OrderBy(value => value.Source.EntityId.High).ThenBy(value => value.Source.EntityId.Low)
            .ThenBy(value => value.Signal).ThenBy(value => value.PublicInput).ThenBy(value => value.ScheduledInput).ToArray();
        if (roots.Length > MissionSignalValidation.MaximumFrontiersPerGeneration)
            throw new InvalidOperationException("MissionSync has too many authored mission frontiers.");
        return roots;
    }

    // The authored graph defines the campaign boundary. Port spelling is not
    // progression semantics: custom mission pins and nested sequences qualify
    // through their method contracts, including UI/AI/Player consequences.
    private static List<MissionSyncFrontier> CampaignFrontiers(WorldManifest manifest,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities)
    {
        var missionGraphs = manifest.Composites.Where(value => !value.AimpNamespace
            && IsMissionGraph(value.Name)).Select(value => value.CompositeShortGuid).ToHashSet();
        // A frontier is an entity authored *directly* in a mission graph, or a
        // pin of a composite instance placed there. The internals of reusable
        // composites nested in a mission (Use, 1st_person_animated, FX_FP_EyeLid,
        // KeyPadKeyCard...) drive the interacting player's own presentation;
        // replaying them made a Client perform the Host's terminal interaction
        // (BSP_TORRENS, build 494). They stay local; their pins are replicated.
        bool InMission(WorldEntityDescriptor value) => missionGraphs.Contains(value.SourceCompositeShortGuid);
        // Composites that directly own Player-domain entities (first-person
        // animated camera, keypads, save terminal, hiding door) bind the LOCAL
        // player. Calling into them on another machine re-runs the interaction
        // there (Use.used -> KeyPadKeyCard.StartInteraction teleported the
        // Client). Each machine reaches them through its own graph instead.
        var playerBound = manifest.Entities.Where(value => value.Domain == WorldDomain.Player)
            .Select(value => value.SourceCompositeShortGuid).Where(value => !missionGraphs.Contains(value))
            .ToHashSet();
        // Logic whose every control output ends on the local player (LogicDelay
        // -> TriggerBindCharacter(player) -> DespawnPlayer, the suit change)
        // is that player's own sequence. Replaying the suit Proxy's
        // anim_finished despawned and respawned the other player too (build
        // 500). Data links into variables are not control flow.
        var outgoing = manifest.Connections.GroupBy(edge => edge.SourceEntity)
            .ToDictionary(group => group.Key, group => group.ToArray());
        bool DataLink(WorldConnectionDescriptor edge) => entities.TryGetValue(edge.TargetEntity, out var target)
            && (target.PrimitiveName?.StartsWith("Variable", StringComparison.Ordinal) ?? false);
        var playerChain = manifest.Entities
            .Where(value => value.Domain == WorldDomain.Player && value.Kind == WorldEntityKind.Function)
            .Select(value => value.EntityId).ToHashSet();
        for (var changed = true; changed;)
        {
            changed = false;
            foreach (var (id, edges) in outgoing)
            {
                if (playerChain.Contains(id) || !entities.TryGetValue(id, out var value)
                    || value.Kind == WorldEntityKind.CompositeInstance) continue;
                var control = edges.Where(edge => !DataLink(edge)).ToArray();
                if (control.Length != 0 && control.All(edge => playerChain.Contains(edge.TargetEntity)))
                    changed = playerChain.Add(id) | changed;
            }
        }
        bool PlayerLocalTarget(WorldEntityDescriptor target) => target.Domain == WorldDomain.Player
            || playerChain.Contains(target.EntityId)
            || target.Kind == WorldEntityKind.CompositeInstance && playerBound.Contains(target.PrimitiveType);
        bool Addressable(WorldEntityDescriptor value) => value.EntityId.IsValid
            && value.Domain != WorldDomain.AIMP
            && (value.Flags & WorldEntityFlags.RuntimeAddressable) != 0;
        // A player's own interaction: a trigger volume, a Use prompt or a pin of
        // a composite bound to the local player (KeyPadKeyCard, Save_Terminal).
        bool Interaction(WorldEntityDescriptor value) => value.PrimitiveName is
                "PlayerTriggerBox" or "PlayerTriggerSphere" or "Interaction" or "MonitorPadInput" or "ButtonMashPrompt"
            || (value.PrimitiveName?.EndsWith(@"Gameplay\Use", StringComparison.Ordinal) ?? false)
            || value.Kind == WorldEntityKind.CompositeInstance && playerBound.Contains(value.PrimitiveType);
        // Only the Host originates autonomous decisions, so a Client that runs an
        // interaction alone waited forever for the Host to repeat it: the Client's
        // key_accepted never left its machine, and its suit-change Proxy
        // (Use.used -> proxy_enable ... anim_finished) kept the Client in the
        // black locker until the Host changed too (build 498). An output emitted
        // by an interaction, or by an entity that interaction drives directly,
        // is proposed by whichever player performs it first.
        var interactionDriven = manifest.Connections
            .Where(edge => entities.TryGetValue(edge.SourceEntity, out var source) && Interaction(source))
            .Select(edge => edge.TargetEntity).ToHashSet();
        bool PlayerIngress(WorldEntityDescriptor value) => Interaction(value) || interactionDriven.Contains(value.EntityId);

        var methods = manifest.CompositePublicInputs
            .GroupBy(value => (value.OwnerCompositeInstance, value.PublicInputShortGuid))
            .Where(group => group.All(value => value.NoTypedData))
            .Select(group => group.Key).ToHashSet();
        bool ControlTarget(WorldConnectionDescriptor edge) =>
            entities.TryGetValue(edge.TargetEntity, out var target) && Addressable(target)
            && !PlayerLocalTarget(target)
            && (target.Kind == WorldEntityKind.CompositeInstance
                ? methods.Contains((target.EntityId, edge.TargetParameter))
                // A Proxy/Alias forwards the same control input to the entity it
                // stands for: KeyPadKeyCard.key_accepted -> Proxy(door).force_open.
                // Rejecting it left the Client's door shut (build 497).
                : target.Kind is WorldEntityKind.Function or WorldEntityKind.Proxy or WorldEntityKind.Alias
                    && ControlInputs.Contains(edge.TargetParameter));

        var outputs = manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => group.Key.SourceParameter != 0U
                && entities.TryGetValue(group.Key.SourceEntity, out var source)
                && Addressable(source) && InMission(source)
                // A Variable's on_changed announces a per-machine value that the
                // commit cannot carry (VariableFloat <- MonitorActionMap
                // movement_stick_y: the Host's stick enabled nothing on the
                // Client, whose own movement was suppressed, so it could never
                // use the first terminal). Each machine runs its own.
                && !(source.PrimitiveName?.StartsWith("Variable", StringComparison.Ordinal) ?? false)
                && source.Kind is WorldEntityKind.CompositeInstance or WorldEntityKind.Function or WorldEntityKind.Proxy
                && group.All(ControlTarget))
            // All authored destinations have been certified as data-free
            // control inputs above: CompositeInstance methods are explicitly
            // NoTypedData, while Function destinations use the small stateless
            // ControlInputs allowlist. The outgoing native callback can carry
            // a sender-local CATHODE reference even though no receiver reads
            // it. Never serialize that pointer; only replay the control edge.
            // Previously PayloadTolerant=false left every such campaign output
            // local (657/748 rejected in the BSP_TORRENS diagnostics).
            .Select(group => group.Key).ToHashSet();
        // Intercept a whole nested mission/sequence at its public Method, before
        // dispatch. DataType.NONE is an authored contract, not a guessed name.
        var methodFrontiers = methods.Where(method => entities.TryGetValue(method.OwnerCompositeInstance, out var owner)
                && Addressable(owner) && !PlayerLocalTarget(owner)
                && (InMission(owner) || missionGraphs.Contains(owner.PrimitiveType))).ToHashSet();

        // A mission edge into such a player sequence starts a run that only the
        // performing player executes (it is no longer replicated). Follow it to
        // where it rejoins the mission: everything the player itself emits on
        // the way (DespawnPlayer.despawned -> SpawnPositionSelect.SpawnPlayer)
        // stays local, and the first mission frontier after it (FinishedSpawning:
        // fade-in, objectives) is proposed by that player instead of waiting for
        // the Host to change too.
        var localOutputs = new HashSet<(WorldEntityId, uint)>();
        var localMethods = new HashSet<(WorldEntityId, uint)>();
        var chainExits = new HashSet<(WorldEntityId, uint)>();
        var pending = new Queue<WorldEntityId>(manifest.Connections
            .Where(edge => playerChain.Contains(edge.TargetEntity)
                && entities.TryGetValue(edge.SourceEntity, out var source) && InMission(source)
                && entities.TryGetValue(edge.TargetEntity, out var target) && target.Domain != WorldDomain.Player)
            .Select(edge => edge.TargetEntity).Distinct());
        var visited = pending.ToHashSet();
        while (pending.TryDequeue(out var current))
        {
            if (!outgoing.TryGetValue(current, out var edges)) continue;
            var local = playerChain.Contains(current)
                || entities.TryGetValue(current, out var currentValue) && currentValue.Domain == WorldDomain.Player;
            foreach (var group in edges.GroupBy(edge => edge.SourceParameter))
            {
                var key = (current, group.Key);
                if (!local && outputs.Contains(key)) { chainExits.Add(key); continue; }
                localOutputs.Add(key);
                foreach (var edge in group)
                {
                    if (local) localMethods.Add((edge.TargetEntity, edge.TargetParameter));
                    if (visited.Add(edge.TargetEntity)) pending.Enqueue(edge.TargetEntity);
                }
            }
        }

        var result = outputs.Where(key => !localOutputs.Contains(key))
            .Select(key => new MissionSyncFrontier(entities[key.SourceEntity], key.SourceParameter,
                PayloadTolerant: true, Repeatable: true, OrderedOccurrences: true,
                PlayerIngress: PlayerIngress(entities[key.SourceEntity]) || chainExits.Contains(key))).ToList();
        result.AddRange(methodFrontiers.Where(method => !localMethods.Contains(method))
            .Select(method => new MissionSyncFrontier(entities[method.OwnerCompositeInstance],
                method.PublicInputShortGuid, PayloadTolerant: true, Repeatable: true,
                PublicInput: true, OrderedOccurrences: true)));
        // These are engine transitions, independent of mission or map names.
        // Catch their actual scheduled trigger even if an outer mission pin
        // cannot be represented without typed sender-local data.
        result.AddRange(manifest.Entities.Where(value => Addressable(value)
                && value.Kind == WorldEntityKind.Function && value.PrimitiveName is "SwitchLevel" or "GoToFrontend" or "Checkpoint")
            .Select(value => new MissionSyncFrontier(value, ShortGuidUtils.Generate("trigger").AsUInt32,
                Repeatable: true, OrderedOccurrences: true, ScheduledInput: true,
                PlayerIngress: value.PrimitiveName == "Checkpoint")));
        return result;
    }

    private static List<MissionSyncFrontier> DirectFrontiers(WorldManifest manifest,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities, bool survival)
    {
        var composites = manifest.Composites.ToDictionary(value => value.CompositeShortGuid);
        var sources = MissionSources(manifest, survival);
        return manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => sources.TryGetValue(group.Key.SourceEntity, out var source)
                && group.All(edge => SafeTarget(edge, entities, survival))
                && (!survival
                    ? CompletionPorts.Contains(group.Key.SourceParameter)
                    : SurvivalBoundary(group.Key.SourceParameter, source, group, entities, composites)))
            .Select(group => new MissionSyncFrontier(sources[group.Key.SourceEntity], group.Key.SourceParameter,
                survival && sources[group.Key.SourceEntity].Kind is WorldEntityKind.Proxy
                    or WorldEntityKind.CompositeInstance,
                survival && group.All(edge => entities.TryGetValue(edge.TargetEntity, out var target)
                    && IsMissionStartSink(target, edge.TargetParameter))))
            .ToList();
    }

    // A mission graph observes another composite's milestone through a Proxy
    // (e.g. Proxy<Power_Generator_Control>.complete). The engine may emit that
    // output from the proxied instance rather than from the Proxy, so admit the
    // proxied entity's same port as well. Its replay reaches both its own
    // section graph and the mission proxy.
    private static IEnumerable<MissionSyncFrontier> ProxiedFrontiers(WorldManifest manifest,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities, IReadOnlyList<MissionSyncFrontier> admitted)
    {
        var targets = ProxyTargets(manifest);
        var edges = manifest.Connections.ToLookup(edge => (edge.SourceEntity, edge.SourceParameter));
        return admitted.Where(root => root.Source.Kind == WorldEntityKind.Proxy)
            .Select(root => (root.Signal, Found: targets.TryGetValue(root.Source.EntityId, out var target), target))
            .Where(value => value.Found && ReplayableProxyTarget(value.target!)
                && edges[(value.target!.EntityId, value.Signal)].All(edge => SafeTarget(edge, entities, true)))
            .Select(value => new MissionSyncFrontier(value.target!, value.Signal,
                value.target!.Kind == WorldEntityKind.CompositeInstance))
            .ToArray();
    }

    // A mission graph may create its NPC only when the LOCAL player enters a
    // trigger volume (ChallengeMap11: Alarm PlayerTriggerBox -> LogicOnce ->
    // LogicDelay -> Xenomorph_NPC.spawn_npc). Each peer keeps its own volume,
    // so a Client survivor tripping it never created the Host's authoritative
    // Alien, and a Host Alien pilot (hidden player) never trips it at all.
    // Replicate the most upstream safe mission output whose control fan-out
    // reaches an NPC spawn input; the volume itself stays per-player.
    private static readonly uint SpawnNpcInput = ShortGuidUtils.Generate("spawn_npc").AsUInt32;
    private const int SpawnReachDepth = 6;

    private static IEnumerable<MissionSyncFrontier> NpcSpawnFrontiers(WorldManifest manifest,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities)
    {
        var composites = manifest.Composites.ToDictionary(value => value.CompositeShortGuid);
        var sources = MissionSources(manifest, true);
        var outgoing = manifest.Connections.ToLookup(edge => edge.SourceEntity);
        bool SpawnSink(WorldConnectionDescriptor edge) => edge.TargetParameter == SpawnNpcInput
            && entities.TryGetValue(edge.TargetEntity, out var target)
            && target.Kind == WorldEntityKind.CompositeInstance
            && target.Domain is WorldDomain.AI or WorldDomain.World;
        // Entities reached through control flow (logic relays only, never into
        // another composite's internals) and whether an NPC spawn is reached.
        (HashSet<WorldEntityId> Reached, bool Spawns) Reach(IEnumerable<WorldConnectionDescriptor> first)
        {
            var reached = new HashSet<WorldEntityId>();
            var frontier = first.ToList();
            for (var depth = 0; depth < SpawnReachDepth && frontier.Count != 0; ++depth)
            {
                if (frontier.Any(SpawnSink)) return (reached, true);
                frontier = frontier.Where(edge => reached.Add(edge.TargetEntity)
                        && entities.TryGetValue(edge.TargetEntity, out var target)
                        && target.Kind is WorldEntityKind.Function or WorldEntityKind.Proxy)
                    .SelectMany(edge => outgoing[edge.TargetEntity]).ToList();
            }
            return (reached, false);
        }
        var candidates = manifest.Connections.GroupBy(edge => (edge.SourceEntity, edge.SourceParameter))
            .Where(group => group.Key.SourceParameter != 0U
                && sources.TryGetValue(group.Key.SourceEntity, out var source)
                && composites.TryGetValue(source.SourceCompositeShortGuid, out var composite)
                && IsMissionGraph(composite.Name)
                && !SurvivalLocalInteractionPorts.Contains(group.Key.SourceParameter)
                && group.All(edge => SpawnSink(edge) || SafeTarget(edge, entities, true)))
            .Select(group => (group.Key, Reach: Reach(group)))
            .Where(value => value.Reach.Spawns).ToArray();
        // Only the most upstream output commits: a downstream relay of the same
        // chain re-emits during the replay and must stay a local consequence.
        return candidates.Where(value => !candidates.Any(other => other.Key != value.Key
                && other.Reach.Reached.Contains(value.Key.SourceEntity)))
            .Select(value => new MissionSyncFrontier(sources[value.Key.SourceEntity], value.Key.SourceParameter))
            .ToArray();
    }

    private static bool ReplayableProxyTarget(WorldEntityDescriptor target) =>
        target.EntityId.IsValid && target.Domain == WorldDomain.World
        && (target.Flags & WorldEntityFlags.RuntimeAddressable) != 0
        && target.Kind is WorldEntityKind.CompositeInstance or WorldEntityKind.Function
        && target.PrimitiveName is not ("Interaction" or "PlayerTriggerBox" or "PlayerTriggerSphere");

    // CATHODE proxy paths are [root?, instance steps..., entity, 0]. Entity
    // descriptors store the instance steps separately from their own ShortGuid.
    // An ambiguous or unresolved path is never replay authority.
    private static Dictionary<WorldEntityId, WorldEntityDescriptor> ProxyTargets(WorldManifest manifest)
    {
        var paths = manifest.InstancePaths.ToDictionary(value => value.PathId, value => value.Steps);
        var byLeaf = manifest.Entities.Where(value => value.EntityId.IsValid).ToLookup(value => value.SourceShortGuid);
        return manifest.References.Where(value => value.Kind == WorldReferenceKind.Proxy)
            .Select(reference => (reference.OwnerEntity,
                Steps: reference.TargetPathSteps.Reverse().SkipWhile(value => value == 0U).Reverse().ToArray()))
            .Where(value => value.Steps.Length != 0)
            .Select(value => (value.OwnerEntity, Matches: byLeaf[value.Steps[^1]]
                .Where(entity => paths.TryGetValue(entity.InstancePathId, out var path)
                    && PathMatches(path, value.Steps[..^1])).Take(2).ToArray()))
            .Where(value => value.Matches.Length == 1)
            .ToDictionary(value => value.OwnerEntity, value => value.Matches[0]);
    }

    private static bool PathMatches(IReadOnlyList<uint> path, uint[] prefix) =>
        path.SequenceEqual(prefix) || prefix.Length != 0 && path.SequenceEqual(prefix[1..]);

    private static bool SurvivalBoundary(
        uint output, WorldEntityDescriptor source,
        IGrouping<(WorldEntityId SourceEntity, uint SourceParameter), WorldConnectionDescriptor> edges,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities,
        IReadOnlyDictionary<uint, WorldCompositeDescriptor> composites)
    {
        if (SurvivalLocalInteractionPorts.Contains(output)) return false;

        var sourceInMission = composites.TryGetValue(source.SourceCompositeShortGuid, out var composite)
            && IsMissionGraph(composite.Name);
        // Known completion outputs need to originate from an authored mission
        // graph, never a generator, player interaction or arbitrary World script.
        if (sourceInMission && SurvivalCompletionPorts.Contains(output)) return true;

        // An authored mission graph starting a nested mission is a shared
        // milestone even though its trigger is a generic output name.
        if (sourceInMission && edges.All(edge => entities.TryGetValue(edge.TargetEntity, out var target)
                && IsMissionStartSink(target, edge.TargetParameter))) return true;

        // Custom/unnamed output conventions can be admitted when the actual
        // fan-out proves a progression sink. This uses entity semantics, not
        // a DLC path, map ID or hardcoded output name.
        return edges.Any(edge => entities.TryGetValue(edge.TargetEntity, out var target)
            && IsObjectiveSink(target));
    }

    private static bool IsMissionStartSink(WorldEntityDescriptor target, uint input) =>
        MissionStartInputs.Contains(input) && target.Domain == WorldDomain.World
        && target.Kind == WorldEntityKind.CompositeInstance && IsMissionGraph(target.PrimitiveName);

    private static bool IsObjectiveSink(WorldEntityDescriptor target)
    {
        if (target.Domain is not (WorldDomain.World or WorldDomain.UI)) return false;
        // A mission's entire graph is not an objective sink. Require a
        // specific target primitive/entity that describes the outcome.
        return IsObjectiveName(target.PrimitiveName) || IsObjectiveName(target.EntityName);
    }

    private static bool IsObjectiveName(string? name) =>
        !string.IsNullOrWhiteSpace(name) && (
            name.Contains("Objective", StringComparison.OrdinalIgnoreCase)
            || name.Contains("MissionComplete", StringComparison.OrdinalIgnoreCase)
            || name.Contains("LevelComplete", StringComparison.OrdinalIgnoreCase)
            || name.Contains("MissionEnd", StringComparison.OrdinalIgnoreCase));

    private static bool SafeTarget(
        WorldConnectionDescriptor edge,
        IReadOnlyDictionary<WorldEntityId, WorldEntityDescriptor> entities,
        bool survival)
    {
        if (!edge.TargetEntity.IsValid || !entities.TryGetValue(edge.TargetEntity, out var target)
            || target.Domain == WorldDomain.AIMP) return false;
        if (!ControlInputs.Contains(edge.TargetParameter)
            && !(survival && SurvivalControlInputs.Contains(edge.TargetParameter))
            && !(survival && IsMissionStartSink(target, edge.TargetParameter))) return false;
        // Surviving mission fan-out must never replay the Character's local
        // movement, a proximity trigger, an AI action or unknown data binding.
        if (!survival) return true;
        if (target.Domain is not (WorldDomain.World or WorldDomain.UI)) return false;
        if (target.PrimitiveName is "PlayerTriggerBox" or "PlayerTriggerSphere")
            return ArmingInputs.Contains(edge.TargetParameter);
        return target.PrimitiveName is not "Interaction";
    }

    // Campaign keeps its existing SCRIPT_StoryMission frontier contract.
    // Survival identifies the enclosing *mission semantics* across any DLC,
    // custom challenge or future map. It also accepts any graph with a
    // proved objective sink (see SurvivalBoundary), regardless of its name.
    private static Dictionary<WorldEntityId, WorldEntityDescriptor> MissionSources(
        WorldManifest manifest, bool survival)
    {
        var scripts = manifest.Composites.Where(value => !value.AimpNamespace
            && (!survival
                ? value.Name.Replace('/', '\\').StartsWith("SCRIPT_StoryMission\\",
                    StringComparison.OrdinalIgnoreCase)
                : IsMissionGraph(value.Name)))
            .Select(value => value.CompositeShortGuid).ToHashSet();
        return manifest.Entities.Where(value => value.EntityId.IsValid
            && value.Domain != WorldDomain.AIMP
            && (survival || scripts.Contains(value.SourceCompositeShortGuid))
            && (value.Flags & WorldEntityFlags.RuntimeAddressable) != 0
            && (survival
                ? value.Domain == WorldDomain.World
                  && value.Kind is WorldEntityKind.CompositeInstance or WorldEntityKind.Function or WorldEntityKind.Proxy
                  && value.PrimitiveName is not ("Interaction" or "PlayerTriggerBox" or "PlayerTriggerSphere")
                : value.Kind == WorldEntityKind.CompositeInstance
                  || value.PrimitiveName is "PlayerTriggerBox" or "PlayerTriggerSphere" or "CAGEAnimation"))
            .ToDictionary(value => value.EntityId);
    }

    private static bool IsMissionGraph(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return false;
        // Match a reusable semantic token rather than an arbitrary substring:
        // "Permission" is not a mission. Recognizes StoryMission, MISSION_X,
        // MissionControl and lowercase "mission" across arbitrary paths.
        var tokens = name.Replace('/', '\\').Split(['\\', '_', '-', '.'],
            StringSplitOptions.RemoveEmptyEntries);
        return tokens.Any(token =>
            token.Equals("mission", StringComparison.OrdinalIgnoreCase)
            || token.StartsWith("mission", StringComparison.OrdinalIgnoreCase)
            || token.EndsWith("Mission", StringComparison.Ordinal));
    }

    internal static byte[] Encode(WorldManifest manifest, IReadOnlyList<MissionSyncFrontier> roots)
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(CatalogMagic);
        writer.Write(manifest.MapContentIdHigh);
        writer.Write(manifest.MapContentIdLow);
        writer.Write((uint)roots.Count);
        foreach (var root in roots)
        {
            writer.Write(root.Source.EntityId.High);
            writer.Write(root.Source.EntityId.Low);
            writer.Write(root.Source.CathodeEntityA);
            writer.Write(root.Source.CathodeEntityB);
            writer.Write(root.Signal);
            writer.Write((root.PayloadTolerant ? FlagPayloadTolerant : 0U)
                | (root.PublicInput ? FlagPublicInput : 0U)
                | (root.OrderedOccurrences ? FlagOrderedOccurrences : 0U)
                | (root.ScheduledInput ? FlagScheduledInput : 0U)
                | (root.PlayerIngress ? FlagPlayerIngress : 0U));
        }
        return stream.ToArray();
    }
}
