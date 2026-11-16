using AIMP.Protocol;
using System.Security.Cryptography;
using System.Text.Json;
using CATHODE;

namespace AIMP.Launcher;

/// <summary>
/// Gives every remote-player representation (crew NPC: E_RIPLEY, LAMBERT,
/// DALLAS, PARKER, ASH; see PlayerCharacterCatalog) real crouched
/// animations, from clips the game already ships. The CROUCHED animation tag,
/// which the native stance helpers set, selects the clip database's CROUCHED
/// context: retail E_RIPLEY only fills it with crouched starts/stops, impacts
/// and deaths, so idles and walk cycles stayed standing. This patch:
///  1. maps the idles to the third-person female crouch idle E_RIPLEY loads;
///  2. maps the walk/strafe/aimed-walk cycles to the player's own crouched
///     walks (FEMALEFP), which E_RIPLEY may retarget through the existing
///     FEMALEFP-&gt;FEMALENPC mapping once FEMALEFP is one of its low-res sources
///     and the player's clip group is one of its dependencies.
/// Hiding entry/idle/exit aliases also use the player's FEMALEFP clips in the
/// base table for isolated remote animation commands. Upper-body weapon layers
/// still come from the weapon contexts.
/// AnimationSync: every other first-person alias the representation lacks
/// (interactions: buttons, generators, terminals, vents, levers, hacking...) is
/// added to its base table too, so any scripted clip the owner plays can be
/// replayed by alias on the remote body. Retail aliases are never replaced:
/// every shareable player clip is also added under ReplayAlias(alias), which
/// AnimationSync plays so the body runs the owner's exact clip even where the
/// representation has its own different clip under that alias.
/// ANIMATION.PAK is global: the retail file is backed up once and every
/// installation rebuilds the patched file from that backup.
/// </summary>
internal static class RemoteCrouchAnimationInstaller
{
    private const int PatchVersion = 9;
    // Mirrors AIMP.Native AnimationSync.h ReplayAlias: the representation's
    // alias for the player's exact clip of a player alias.
    internal static uint ReplayAlias(uint alias) => alias ^ 0xA1B5C0DEu;
    private const string PlayerClipDbEntry = @"DATA\ANIM_SYS\2554777613_ANIM_CLIP_DB.BIN"; // 1ST_PERSON_RIPLEY
    private const string GlobalClipDbEntry = @"DATA\ANIM_SYS\ANIM_CLIP_DB.BIN";
    private const string CrouchedContext = "CROUCHED";
    // Every playable crew representation (PlayerCharacterCatalog). The male crew
    // sample on the MALE low-res skeleton: they also need FEMALE (the crouch
    // idle is a FEMALE clip), retargeted through retail FEMALE<->MALE maps.
    internal sealed record Profile(string Character, string LowRes, string[] ExtraSources)
    {
        internal string ClipDbEntry => $@"DATA\ANIM_SYS\{AnimClipDatabase.HashName(Character)}_ANIM_CLIP_DB.BIN";
        internal string SkeletonDefEntry => $@"DATA\ANIM_SYS\SKELE\DEFS\{AnimClipDatabase.HashName(Character)}.BIN";
        internal uint Hash => AnimClipDatabase.HashName(Character);
        internal string BlendPrefix => @"AIMP_" + Character + @"_CROUCHED\";
    }

    internal static readonly IReadOnlyList<Profile> Profiles = PlayerCharacterCatalog.All
        .Select(c => c.Female ? new Profile(c.Name, "FEMALENPC", []) : new Profile(c.Name, "MALE", ["FEMALE"]))
        .ToArray();
    private const string PlayerSkeleton = "FEMALEFP";

    private const string CrouchIdle = "idle_crouch";        // FEMALE\LOCOMOTION\CROUCH\UNARMED\IDLE_01 (E_RIPLEY)
    private const string CrouchForward = "fp_walk_f_crouch"; // FEMALEFP\...\WALK_F_CROUCH (1ST_PERSON_RIPLEY)
    private const string CrouchBackward = "fp_walk_b_crouch";
    private const string CrouchLeft = "fp_walk_l_crouch";

    private static readonly string[] HidingClips =
    [
        "player_hiding_high_enter", "player_hiding_high_idle", "player_hiding_high_exit",
        "player_hiding_low_enter", "player_hiding_low_idle", "player_hiding_low_exit",
    ];

    private static IEnumerable<string> PlayerSources => Replacements.Where(r => r.PlayerClip)
        .Select(r => r.Source).Concat(HidingClips).Distinct();

    private static readonly string[] Idles =
    [
        "idle", "idle_standard_01", "idle_nervous_01", "idle_001_fidget", "idle_002_fidget", "idle_003_fidget",
        "idle_004_fidget", "idle_005_fidget", "idle_search",
        // The NPC idle tree requests these aliases as well as idle/idle_search.
        // v397 resolver captures returned standing gesture bases while CROUCHED
        // was active: replacing idle alone leaves these branches unchanged.
        "npc_neutral_idle_base", "npc_neutral_idle_wshift_01", "npc_neutral_idle_wshift_02",
        "npc_scared_base_idle", "npc_scared_idle_wshift01", "npc_scared_idle_wshift02",
        "npc_angry_idle_base", "npc_angry_idle_wshift_01",
        "npc_suspicious_idle_base", "npc_suspicious_idle_wshift_01", "npc_suspicious_idle_wshift_02",
        "aim_to_idle", "walk_slow_on_spot_0",
        "turn_90_l", "turn_90_r", "turn_180_l", "turn_180_r",
    ];
    // Aimed trees layer weapon/look-at poses over this base. The unarmed
    // crouch idle is not an aiming reference pose and pulls their arms back.
    // Use the existing full-body two-handed crouch aim, not an FP arm pose.
    private static readonly string[] AimedIdles =
    [
        "idle_aimed", "idle_aimed_v2", "search_aimed_idle_01", "search_aimed_idle_02",
        "walk_aim_on_spot_0", "idle_to_aim",
        "turn_45_l_aimed", "turn_45_r_aimed", "turn_90_l_aimed", "turn_90_r_aimed", "turn_180_l_aimed", "turn_180_r_aimed",
    ];
    private static readonly string[] Forward =
    [
        "walk_forward", "walk_forward_slow", "walk_forward_fast", "jog_forward", "sprint", "sprint_turn_l",
        "walk_gait01_l_100", "walk_slow_gait01_l_100", "walk_fast_gait01_l_150", "jog_gait01_l_100",
        "walk_incline_down_20", "walk_incline_down_20_turn_l", "walk_incline_up_20", "walk_incline_up_20_turn_l",
        "walk_fast_incline_down_20", "walk_fast_incline_down_20_turn_l", "walk_fast_incline_up_20", "walk_fast_incline_up_20_turn_l",
        "jog_incline_down_20", "jog_incline_down_20_turn_l", "jog_incline_up_20", "jog_incline_up_turn_l_20",
        "walk_aim_fwd", "walk_aim_fwd_turn_left", "walk_aim_fwd_turn_right",
        "walk_aim_fwd_left_45", "walk_aim_fwd_left_45_turn_back", "walk_aim_fwd_left_45_turn_fwd",
        "walk_aim_fwd_right_45", "walk_aim_fwd_right_45_turn_back", "walk_aim_fwd_right_45_turn_fwd",
        "walk_aim_incline_down_20", "walk_aim_incline_down_20_turn_l", "walk_aim_incline_down_20_turn_r",
        "walk_aim_incline_up_20", "walk_aim_incline_up_20_turn_l", "walk_aim_incline_up_20_turn_r",
        "strafe_fast_45_fwd_right", "strafe_fast_45_fwd_right_turn_back", "strafe_fast_45_fwd_right_turn_fwd",
        "strafe_jog_45_fwd_right", "strafe_jog_45_fwd_right_turn_back", "strafe_jog_45_fwd_right_turn_fwd",
    ];
    private static readonly string[] Backward =
    [
        "jog_backwards", "jog_backwards_turn_l", "strafe_walk_back", "strafe_walk_back_turn_left",
        "walk_aim_back", "walk_aim_back_turn_left", "walk_aim_back_turn_right",
        "walk_aim_back_left_45", "walk_aim_back_left_45_turn_back", "walk_aim_back_left_45_turn_fwd",
        "walk_aim_back_right_45", "walk_aim_back_right_45_turn_back", "walk_aim_back_right_45_turn_fwd",
        "strafe_fast_45_back_left", "strafe_fast_45_back_left_turn_back", "strafe_fast_45_back_left_turn_fwd",
        "strafe_jog_45_back_left", "strafe_jog_45_back_left_turn_back", "strafe_jog_45_back_left_turn_fwd",
    ];
    private static readonly string[] Left = ["walk_aim_left", "walk_aim_left_turn_back", "walk_aim_left_turn_fwd"];
    // Our source is leftward; invert each retail rightward sample's mirror
    // flag, including the samples retail already mirrored for leftward travel.
    private static readonly string[] Right =
    [
        "walk_aim_right", "walk_aim_right_turn_back", "walk_aim_right_turn_fwd",
        "strafe_walk_right", "strafe_walk_right_turn_back", "strafe_walk_right_turn_fwd",
        "strafe_slow_right", "strafe_slow_right_turn_back", "strafe_slow_right_turn_fwd",
        "strafe_fast_right", "strafe_fast_right_turn_back", "strafe_fast_right_turn_fwd",
        "strafe_jog_right", "strafe_jog_right_turn_back", "strafe_jog_right_turn_fwd",
    ];

    private static IEnumerable<(string Name, string Source, bool PlayerClip)> Replacements =>
        Idles.Select(n => (n, CrouchIdle, false))
            .Concat(Forward.Select(n => (n, CrouchForward, true)))
            .Concat(Backward.Select(n => (n, CrouchBackward, true)))
            .Concat(Left.Concat(Right).Select(n => (n, CrouchLeft, true)));

    private sealed record State(int PatchVersion, string OriginalSha256, string PatchedSha256);

    public static void Install(string game, CancellationToken token)
    {
        var target = Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK");
        if (!File.Exists(target)) throw new FileNotFoundException("ANIMATION.PAK is required to prepare remote crouch animations.", target);
        var root = Path.Combine(game, "AIMP", "PreparationBackups", "Animation");
        Directory.CreateDirectory(root);
        var backup = Path.Combine(root, "ANIMATION.PAK.original");
        var statePath = Path.Combine(root, "state.json");
        var state = File.Exists(statePath) ? JsonSerializer.Deserialize<State>(File.ReadAllText(statePath)) : null;

        var current = Hash(target);
        if (state != null && state.PatchVersion == PatchVersion && current == state.PatchedSha256 &&
            File.Exists(backup) && Hash(backup) == state.OriginalSha256)
        {
            AnimationSyncBoneMasks.Ensure(game, backup, state.OriginalSha256, token);
            return;
        }

        // The current file is retail unless it is our own earlier output. A
        // repaired or updated game replaces the retained original.
        var isOurOutput = state != null && current == state.PatchedSha256;
        if (!File.Exists(backup) || (!isOurOutput && Hash(backup) != current))
        {
            if (isOurOutput) throw new InvalidDataException("The retail ANIMATION.PAK backup is missing; verify the game files and prepare again.");
            CopyVerified(target, backup + ".tmp", current);
            File.Move(backup + ".tmp", backup, true);
        }
        var original = Hash(backup);
        token.ThrowIfCancellationRequested();

        var pak = new PAK2(backup);
        Patch(pak);
        token.ThrowIfCancellationRequested();

        var staged = target + ".aimp-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            if (!pak.Save(staged, false)) throw new IOException("Could not write the patched ANIMATION.PAK.");
            Validate(new PAK2(staged));
            var patched = Hash(staged);
            using (var stream = new FileStream(staged, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) stream.Flush(true);
            if (Hash(target) != current) throw new IOException("ANIMATION.PAK changed during preparation.");
            File.Replace(staged, target, null);
            if (Hash(target) != patched) throw new IOException("The published ANIMATION.PAK differs from its staged copy.");
            File.WriteAllText(statePath + ".tmp", JsonSerializer.Serialize(new State(PatchVersion, original, patched)));
            File.Move(statePath + ".tmp", statePath, true);
        }
        finally { if (File.Exists(staged)) File.Delete(staged); }
        AnimationSyncBoneMasks.Ensure(game, backup, original, token);
    }

    private static PAK2.File Entry(PAK2 pak, string name) =>
        pak.Entries.FirstOrDefault(e => e.Filename.Equals(name, StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidDataException($"ANIMATION.PAK does not contain {name}.");

    internal static void Patch(PAK2 pak)
    {
        foreach (var profile in Profiles) Patch(pak, profile);
    }

    private static void Patch(PAK2 pak, Profile profile)
    {
        var character = profile.Character;
        var clipEntry = Entry(pak, profile.ClipDbEntry);
        var db = ParseExact(clipEntry.Content, character);
        var player = ParseExact(Entry(pak, PlayerClipDbEntry).Content, "1ST_PERSON_RIPLEY");
        var context = db.Context(CrouchedContext) ?? throw new InvalidDataException($"{character} has no CROUCHED animation context.");
        foreach (var (name, source, playerClip) in Replacements)
        {
            var key = AnimClipDatabase.HashName(name);
            if (!db.Animations.Contains(key)) throw new InvalidDataException($"{character} has no '{name}' clip.");
            var clip = (playerClip ? player : db).Animations.Get(AnimClipDatabase.HashName(source))
                ?? throw new InvalidDataException($"The crouch source clip '{source}' is missing.");
            context.Animations.Set(key, clip);
        }
        var crouchAim = CrouchAim(db);
        foreach (var name in AimedIdles)
        {
            var key = AnimClipDatabase.HashName(name);
            if (!db.Animations.Contains(key)) throw new InvalidDataException($"{character} has no '{name}' clip.");
            context.Animations.Set(key, crouchAim);
        }
        foreach (var name in HidingClips)
            db.Animations.Set(AnimClipDatabase.HashName(name), PlayerClip(player, name));

        // Resident sources need character dependencies. Streamed hiding clips
        // keep their native on-demand path; adding those hashes as resident
        // dependencies would make the engine request nonexistent files.
        var globalEntry = Entry(pak, GlobalClipDbEntry);
        var global = GlobalClipDatabase.Parse(globalEntry.Content);
        if (!global.Serialize().AsSpan().SequenceEqual(globalEntry.Content))
            throw new InvalidDataException("The global animation database is not in the supported format.");
        AddSharedPlayerClips(pak, global, player, db, profile);
        foreach (var source in PlayerSources)
        {
            var clip = PlayerClip(player, source);
            if (PlayerResidentSection(pak, global, clip, source) is uint group)
                global.AddDependency(profile.Hash, group);
        }
        global.AddDependency(profile.Hash, global.SectionGroup(crouchAim.Path)
            ?? throw new InvalidDataException("The crouched two-handed aim has no clip section."));
        var timing = new Dictionary<uint, RetailAnimationClipTiming>();
        foreach (var (_, source, _) in Replacements.Where(r => r.PlayerClip).DistinctBy(r => r.Source))
        {
            var clip = player.Animations.Get(AnimClipDatabase.HashName(source))!.Value;
            if (clip.MetadataInstance != 0) throw new InvalidDataException("Unsupported crouch walk metadata instance.");
            var section = global.SectionGroup(clip.Path)!.Value;
            var data = Entry(pak, $@"DATA\ANIM_SYS\RESIDENT\ANIM_CLIP_DB_SEC_{section}.BIN");
            timing.Add(clip.Path, RetailAnimationClipTiming.Read(data.Content, global.SectionClipIndex(clip.Path)));
        }
        var walkTiming = Replacements.Where(r => r.PlayerClip).ToDictionary(r => AnimClipDatabase.HashName(r.Name),
            r => timing[player.Animations.Get(AnimClipDatabase.HashName(r.Source))!.Value.Path]);
        // Scale only the crouched clone. Full strides must start below the
        // observed 0.5 m/s; source duration and extraction speed determine
        // cadence. Affine scaling preserves the cached grid weights.
        foreach (var (alias, source) in LocomotionBlends)
        {
            var target = AnimClipDatabase.HashName(profile.BlendPrefix + alias);
            global.CloneBlend(AnimClipDatabase.HashName(source), target,
                profile.Hash, AnimClipDatabase.HashName(CrouchedContext));
            CrouchLocomotionBlend.Adapt(global.Blend(target), walkTiming,
                Right.Select(AnimClipDatabase.HashName).ToHashSet());
            context.BlendSets.Set(AnimClipDatabase.HashName(alias), target);
        }
        // Each character samples on its low-res skeleton (FEMALENPC or MALE),
        // both with retail FEMALEFP maps: allow FEMALEFP clips (and FEMALE for
        // the male crew) as low-res sources.
        var defEntry = Entry(pak, profile.SkeletonDefEntry);
        var skeleton = AddLowResSource(defEntry.Content, profile, AnimClipDatabase.HashName(PlayerSkeleton));
        foreach (var extra in profile.ExtraSources)
            skeleton = AddLowResSource(skeleton, profile, AnimClipDatabase.HashName(extra));
        var clips = db.Serialize();
        var dependencies = global.Serialize();
        clipEntry.Content = clips;
        globalEntry.Content = dependencies;
        defEntry.Content = skeleton;
    }

    private static readonly (string Alias, string Source)[] LocomotionBlends =
    [
        ("locomotion_cycle_blend", @"HUMAN\LOCOMOTIONCYCLE"),
        ("aimed_locomotion_cycle_blend", @"HUMAN\AIMED_LOCOMOTION_CYCLE_BLEND"),
    ];

    // AnimationSync: player base-table aliases this representation lacks. Only
    // clips without a per-database metadata instance, in a section layout the
    // engine can load for another character. A resident clip group that cannot
    // be added as a dependency (shared range) leaves that group's aliases out
    // rather than failing the whole preparation.
    private static void AddSharedPlayerClips(PAK2 pak, GlobalClipDatabase global, AnimClipDatabase player,
        AnimClipDatabase db, Profile profile)
    {
        var entries = EntryNames(pak);
        var candidates = new List<(uint Hash, AnimClipDatabase.Clip Clip, uint? Group)>();
        foreach (var (hash, slot) in player.Animations.Keys)
        {
            var clip = player.Animations.Values[slot];
            if (!SharedSection(entries, global, clip, out var group)) continue;
            // The player's exact clip, also where the representation has its
            // own different clip under the same alias.
            var replay = ReplayAlias(hash);
            if (!db.Animations.Contains(replay) && !player.Animations.Contains(replay)) candidates.Add((replay, clip, group));
            if (!db.Animations.Contains(hash)) candidates.Add((hash, clip, group));
        }
        var refused = new HashSet<uint>();
        foreach (var group in candidates.Select(c => c.Group).OfType<uint>().Distinct())
        {
            try { global.AddDependency(profile.Hash, group); }
            catch (InvalidDataException) { refused.Add(group); }
        }
        foreach (var (hash, clip, group) in candidates)
            if (group is not uint resident || !refused.Contains(resident)) db.Animations.Set(hash, clip);
    }

    // true = shareable; group = the resident clip group to depend on (null when streamed).
    private static HashSet<string> EntryNames(PAK2 pak) =>
        pak.Entries.Select(e => e.Filename).ToHashSet(StringComparer.OrdinalIgnoreCase);

    private static bool SharedSection(HashSet<string> entries, GlobalClipDatabase global, AnimClipDatabase.Clip clip, out uint? group)
    {
        group = null;
        if (clip.MetadataInstance != 0 || global.SectionGroup(clip.Path) is not uint section) return false;
        var streamed = global.SectionClipIndex(clip.Path) == uint.MaxValue;
        if (streamed && section != clip.Path) return false;
        var name = $@"DATA\ANIM_SYS\{(streamed ? "STREAMED" : "RESIDENT")}\ANIM_CLIP_DB_SEC_{section}.BIN";
        if (!entries.Contains(name)) return false;
        group = streamed ? null : section;
        return true;
    }

    private static AnimClipDatabase.Clip CrouchAim(AnimClipDatabase db) =>
        db.Context("WEAPON_SHOTGUN")?.Animations.Get(AnimClipDatabase.HashName("cover_low_aim"))
        ?? throw new InvalidDataException("The character has no full-body crouched two-handed aim pose.");

    private static AnimClipDatabase.Clip PlayerClip(AnimClipDatabase player, string name)
    {
        var clip = player.Animations.Get(AnimClipDatabase.HashName(name))
            ?? throw new InvalidDataException($"The player source clip '{name}' is missing.");
        if (clip.MetadataInstance != 0)
            throw new InvalidDataException($"Unsupported player clip metadata instance for '{name}'.");
        return clip;
    }

    private static uint? PlayerResidentSection(PAK2 pak, GlobalClipDatabase global, AnimClipDatabase.Clip clip, string name)
    {
        var group = global.SectionGroup(clip.Path)
            ?? throw new InvalidDataException($"The player clip '{name}' has no clip section.");
        // Retail uses index -1 for streamed clips. Character dependency
        // loading (00a389b0) always selects the RESIDENT directory.
        var streamed = global.SectionClipIndex(clip.Path) == uint.MaxValue;
        if (streamed && group != clip.Path)
            throw new InvalidDataException($"Unsupported streamed section for player clip '{name}'.");
        var directory = streamed ? "STREAMED" : "RESIDENT";
        _ = Entry(pak, $@"DATA\ANIM_SYS\{directory}\ANIM_CLIP_DB_SEC_{group}.BIN");
        return streamed ? null : group;
    }

    private static AnimClipDatabase ParseExact(byte[] raw, string what)
    {
        var db = AnimClipDatabase.Parse(raw);
        if (!db.Serialize().AsSpan().SequenceEqual(raw)) throw new InvalidDataException($"The {what} clip database is not in the supported format.");
        return db;
    }

    // DATA\ANIM_SYS\SKELE\DEFS\<id>.BIN: [self][lo-res][lo path][count][10 x source]...
    private const int LowResCountOffset = 12, LowResSourcesOffset = 16, LowResCapacity = 10;
    internal static byte[] AddLowResSource(byte[] def, Profile profile, uint skeleton)
    {
        var character = profile.Character;
        if (def.Length != 176 || BitConverter.ToUInt32(def, 0) != profile.Hash ||
            BitConverter.ToUInt32(def, 4) != AnimClipDatabase.HashName(profile.LowRes))
            throw new InvalidDataException($"The {character} skeleton definition is not in the supported format.");
        var count = BitConverter.ToInt32(def, LowResCountOffset);
        for (var i = 0; i < Math.Clamp(count, 0, LowResCapacity); i++)
            if (BitConverter.ToUInt32(def, LowResSourcesOffset + i * 4) == skeleton) return def;
        if (count < 0 || count >= LowResCapacity) throw new InvalidDataException($"The {character} skeleton definition has no free source slot.");
        for (var i = count; i < LowResCapacity; i++)
            if (BitConverter.ToUInt32(def, LowResSourcesOffset + i * 4) != 0) throw new InvalidDataException($"Unexpected {character} skeleton definition layout.");
        var result = (byte[])def.Clone();
        BitConverter.GetBytes(skeleton).CopyTo(result, LowResSourcesOffset + count * 4);
        BitConverter.GetBytes(count + 1).CopyTo(result, LowResCountOffset);
        return result;
    }

    internal static void Validate(PAK2 pak)
    {
        foreach (var profile in Profiles) Validate(pak, profile);
    }

    private static void Validate(PAK2 pak, Profile profile)
    {
        var character = profile.Character;
        var db = AnimClipDatabase.Parse(Entry(pak, profile.ClipDbEntry).Content);
        var player = AnimClipDatabase.Parse(Entry(pak, PlayerClipDbEntry).Content);
        var context = db.Context(CrouchedContext) ?? throw new InvalidDataException($"Patched {character} lost its CROUCHED context.");
        foreach (var (name, source, playerClip) in Replacements)
            if (!Equals(context.Animations.Get(AnimClipDatabase.HashName(name)),
                    (playerClip ? player : db).Animations.Get(AnimClipDatabase.HashName(source))))
                throw new InvalidDataException($"Patched {character} CROUCHED context lacks '{name}'.");
        foreach (var name in AimedIdles)
            if (context.Animations.Get(AnimClipDatabase.HashName(name)) != CrouchAim(db))
                throw new InvalidDataException($"Patched {character} crouched aim lacks '{name}'.");
        foreach (var name in HidingClips)
            if (db.Animations.Get(AnimClipDatabase.HashName(name)) != PlayerClip(player, name))
                throw new InvalidDataException($"Patched {character} lacks hiding clip '{name}'.");
        var global = GlobalClipDatabase.Parse(Entry(pak, GlobalClipDbEntry).Content);
        foreach (var (alias, _) in LocomotionBlends)
        {
            var target = AnimClipDatabase.HashName(profile.BlendPrefix + alias);
            var blend = global.Blend(target);
            _ = CrouchLocomotionBlend.Describe(blend);
            if (context.BlendSets.Get(AnimClipDatabase.HashName(alias)) != target ||
                BitConverter.ToUInt32(blend, 12) != profile.Hash ||
                BitConverter.ToUInt32(blend, 16) != AnimClipDatabase.HashName(CrouchedContext))
                throw new InvalidDataException($"{character} crouched locomotion blend uses the wrong clip database or context.");
        }
        foreach (var source in PlayerSources)
        {
            var clip = PlayerClip(player, source);
            if (PlayerResidentSection(pak, global, clip, source) is uint group &&
                !global.Dependencies(profile.Hash).Contains(group))
                throw new InvalidDataException($"Patched {character} does not load player clip '{source}'.");
        }
        // AnimationSync: a shared player alias must be loadable where present.
        var dependencies = global.Dependencies(profile.Hash);
        var entries = EntryNames(pak);
        var shared = 0;
        foreach (var (hash, slot) in player.Animations.Keys)
        {
            var clip = player.Animations.Values[slot];
            if (db.Animations.Get(hash) != clip || !SharedSection(entries, global, clip, out var group)) continue;
            if (group is uint resident && !dependencies.Contains(resident))
                throw new InvalidDataException($"Patched {character} does not load shared player clip {hash}.");
            shared++;
        }
        if (shared == 0) throw new InvalidDataException($"Patched {character} lacks the shared player clips.");
        var def = Entry(pak, profile.SkeletonDefEntry).Content;
        var sources = Enumerable.Range(0, BitConverter.ToInt32(def, LowResCountOffset))
            .Select(i => BitConverter.ToUInt32(def, LowResSourcesOffset + i * 4)).ToHashSet();
        foreach (var required in profile.ExtraSources.Append(PlayerSkeleton))
            if (!sources.Contains(AnimClipDatabase.HashName(required)))
                throw new InvalidDataException($"Patched {character} skeleton cannot retarget {required} clips.");
    }

    private static void CopyVerified(string source, string destination, string expected)
    {
        File.Copy(source, destination, true);
        if (Hash(destination) != expected) throw new IOException("Could not back up ANIMATION.PAK.");
    }

    private static string Hash(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
}

/// <summary>
/// Byte-exact model of the head of DATA\ANIM_SYS\ANIM_CLIP_DB.BIN: clip
/// sections keyed by clip path, per-character dependency ranges, then
/// [count][count x clip group]. Retail reads the file sequentially (no
/// absolute offsets). Blend definitions follow the resident-section count;
/// the final table remains verbatim. CrouchLocomotionBlend adapts only the
/// cloned locomotion geometry, source timing and lateral mirror flags.
/// </summary>
internal sealed class GlobalClipDatabase
{
    private readonly record struct Pair(uint A, uint B);
    private AnimClipDatabase.Table<Pair> _sections = new(), _dependencies = new();
    private List<uint> _groups = [];
    private uint _residentSectionCount;
    private AnimClipDatabase.Table<uint> _blendNames = new();
    private readonly List<byte[]> _blends = [];
    private byte[] _tail = [];

    public static GlobalClipDatabase Parse(byte[] data)
    {
        using var reader = new BinaryReader(new MemoryStream(data));
        var db = new GlobalClipDatabase
        {
            _sections = AnimClipDatabase.ReadTable(reader, r => new Pair(r.ReadUInt32(), r.ReadUInt32())),
            _dependencies = AnimClipDatabase.ReadTable(reader, r => new Pair(r.ReadUInt32(), r.ReadUInt32())),
        };
        var count = reader.ReadUInt32();
        if (count != db._dependencies.Values.Sum(v => (long)v.B) ||
            db._dependencies.Values.Any(v => (long)v.A + v.B > count))
            throw new InvalidDataException("Unexpected global animation dependency list.");
        for (var i = 0u; i < count; i++) db._groups.Add(reader.ReadUInt32());
        db._residentSectionCount = reader.ReadUInt32();
        db._blendNames = AnimClipDatabase.ReadTable(reader, r => r.ReadUInt32());
        if (db._blendNames.Values.Order().Where((v, i) => v != i).Any())
            throw new InvalidDataException("Unexpected global blend indices.");
        for (var i = 0; i < db._blendNames.Values.Count; i++) db._blends.Add(ReadBlend(reader));
        db._tail = reader.ReadBytes((int)(data.Length - reader.BaseStream.Position));
        return db;
    }

    public byte[] Serialize()
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        Action<BinaryWriter, Pair> pair = (w, v) => { w.Write(v.A); w.Write(v.B); };
        AnimClipDatabase.WriteTable(writer, _sections, pair);
        AnimClipDatabase.WriteTable(writer, _dependencies, pair);
        writer.Write((uint)_groups.Count);
        foreach (var group in _groups) writer.Write(group);
        writer.Write(_residentSectionCount);
        AnimClipDatabase.WriteTable(writer, _blendNames, (w, v) => w.Write(v));
        foreach (var blend in _blends) writer.Write(blend);
        writer.Write(_tail);
        writer.Flush();
        return stream.ToArray();
    }

    public uint? SectionGroup(uint clipPath) => _sections.Get(clipPath)?.A;
    public uint SectionClipIndex(uint clipPath) => (_sections.Get(clipPath)
        ?? throw new InvalidDataException("The animation has no clip section.")).B;

    public byte[] Blend(uint name) => _blends[checked((int)(_blendNames.Get(name)
        ?? throw new InvalidDataException("Required animation blend is missing.")))];

    public void CloneBlend(uint source, uint target, uint character, uint context)
    {
        var copy = (byte[])Blend(source).Clone();
        BitConverter.GetBytes(character).CopyTo(copy, 12);
        BitConverter.GetBytes(context).CopyTo(copy, 16);
        if (_blendNames.Get(target) is uint index) _blends[checked((int)index)] = copy;
        else
        {
            _blendNames.Set(target, (uint)_blends.Count);
            _blends.Add(copy);
        }
    }

    // Retail 00A492F0 reads version 4 in this order. Keep each serialized
    // blend opaque except for its database/context hashes at offsets 12/16.
    private static byte[] ReadBlend(BinaryReader reader)
    {
        var start = reader.BaseStream.Position;
        if (reader.ReadUInt64() != 4UL) throw new InvalidDataException("Unsupported animation blend version.");
        reader.ReadBytes(12); // name, database, context
        var dimensions = reader.ReadByte();
        var samples = reader.ReadByte();
        var components = reader.ReadByte();
        var cells = reader.ReadUInt16();
        if (dimensions is < 1 or > 3 || samples == 0 || components == 0)
            throw new InvalidDataException("Invalid animation blend dimensions.");
        reader.ReadBytes(24); // six blend-space bounds
        var x = reader.ReadUInt16();
        var y = reader.ReadUInt16();
        reader.ReadBytes(12); // three parameter hashes
        if (dimensions > 1 && x == 0) throw new InvalidDataException("Invalid animation blend grid.");
        var z = dimensions < 2 ? 0 : cells / (x * (y == 0 ? 1 : y));
        var grid = checked((x + 1) * (y + 1) * (z + 1));
        if (grid > ushort.MaxValue) throw new InvalidDataException("Animation blend grid is too large.");
        var remaining = checked(samples * 16 + components * dimensions * 4 +
            grid * (dimensions == 1 ? 4 : dimensions == 2 ? 8 : 16) +
            components * 4 + components + samples * 4 + samples);
        if (reader.ReadBytes(remaining).Length != remaining)
            throw new InvalidDataException("Truncated animation blend.");
        var end = reader.BaseStream.Position;
        reader.BaseStream.Position = start;
        return reader.ReadBytes(checked((int)(end - start)));
    }

    public IReadOnlyList<uint> Dependencies(uint character)
    {
        var range = _dependencies.Get(character) ?? throw new InvalidDataException("The character has no clip dependencies.");
        return _groups.GetRange((int)range.A, (int)range.B);
    }

    public void AddDependency(uint character, uint group)
    {
        var range = _dependencies.Get(character) ?? throw new InvalidDataException("The character has no clip dependencies.");
        if (Dependencies(character).Contains(group)) return;
        var at = (int)(range.A + range.B);
        // Another character's range spanning the insertion point would gain
        // this group too; refuse rather than change a retail character.
        if (_dependencies.Keys.Any(k => k.Hash != character &&
                _dependencies.Values[k.Slot].A < at && _dependencies.Values[k.Slot].A + _dependencies.Values[k.Slot].B > at))
            throw new InvalidDataException("Another character shares this character's clip dependencies.");
        _groups.Insert(at, group);
        foreach (var (hash, slot) in _dependencies.Keys)
        {
            var value = _dependencies.Values[slot];
            if (hash == character) _dependencies.Values[slot] = value with { B = value.B + 1 };
            else if (value.A >= at) _dependencies.Values[slot] = value with { A = value.A + 1 };
        }
    }
}

/// <summary>
/// Byte-exact model of a per-character DATA\ANIM_SYS\&lt;id&gt;_ANIM_CLIP_DB.BIN.
/// Every table is [count][count][(nameHash, slot) x count][value x count] with
/// keys sorted by descending FNV-1a name hash (retail binary-searches them).
/// </summary>
internal sealed class AnimClipDatabase
{
    internal readonly record struct Clip(uint Path, int MetadataInstance);

    internal sealed class Table<T> where T : struct
    {
        public readonly List<(uint Hash, int Slot)> Keys = [];
        public readonly List<T> Values = [];
        public bool Contains(uint hash) => Keys.Any(k => k.Hash == hash);
        public T? Get(uint hash)
        {
            foreach (var key in Keys) if (key.Hash == hash) return Values[key.Slot];
            return null;
        }
        public void Set(uint hash, T value)
        {
            var index = Keys.FindIndex(k => k.Hash == hash);
            if (index >= 0) { Values[Keys[index].Slot] = value; return; }
            var position = Keys.FindIndex(k => k.Hash < hash);
            Keys.Insert(position < 0 ? Keys.Count : position, (hash, Values.Count));
            Values.Add(value);
        }
    }

    internal sealed class ContextTables
    {
        public uint NameHash;
        public Table<Clip> Animations = new();
        public Table<uint> BlendSets = new();
    }

    private byte[] _header = [];
    private uint _character;
    private byte[] _padding = [];
    private byte[] _trailer = [];
    public Table<Clip> Animations = new();
    public Table<uint> BlendSets = new();
    private Table<uint> _contextNames = new();
    public readonly List<ContextTables> Contexts = [];

    public static uint HashName(string text)
    {
        var hash = 2166136261u;
        foreach (var c in text) { hash ^= c; hash *= 16777619u; }
        return hash;
    }

    public ContextTables? Context(string name)
    {
        var hash = HashName(name);
        return Contexts.FirstOrDefault(c => c.NameHash == hash);
    }

    public static AnimClipDatabase Parse(byte[] data)
    {
        using var reader = new BinaryReader(new MemoryStream(data));
        var db = new AnimClipDatabase
        {
            _header = reader.ReadBytes(8), _character = reader.ReadUInt32(), _padding = reader.ReadBytes(4),
        };
        db.Animations = Read(reader, ReadClip);
        db.BlendSets = Read(reader, r => r.ReadUInt32());
        db._contextNames = Read(reader, r => r.ReadUInt32());
        for (var i = 0; i < db._contextNames.Keys.Count; i++)
        {
            var name = db._contextNames.Keys.FirstOrDefault(k => db._contextNames.Values[k.Slot] == (uint)i);
            db.Contexts.Add(new ContextTables
            {
                NameHash = name.Hash, Animations = Read(reader, ReadClip), BlendSets = Read(reader, r => r.ReadUInt32()),
            });
        }
        db._trailer = reader.ReadBytes((int)(data.Length - reader.BaseStream.Position));
        return db;
    }

    public byte[] Serialize()
    {
        using var stream = new MemoryStream();
        using var writer = new BinaryWriter(stream);
        writer.Write(_header); writer.Write(_character); writer.Write(_padding);
        Write(writer, Animations, WriteClip);
        Write(writer, BlendSets, (w, v) => w.Write(v));
        Write(writer, _contextNames, (w, v) => w.Write(v));
        foreach (var context in Contexts)
        {
            Write(writer, context.Animations, WriteClip);
            Write(writer, context.BlendSets, (w, v) => w.Write(v));
        }
        writer.Write(_trailer);
        writer.Flush();
        return stream.ToArray();
    }

    private static Clip ReadClip(BinaryReader r) => new(r.ReadUInt32(), r.ReadInt32());
    private static void WriteClip(BinaryWriter w, Clip c) { w.Write(c.Path); w.Write(c.MetadataInstance); }

    internal static Table<T> ReadTable<T>(BinaryReader reader, Func<BinaryReader, T> item) where T : struct => Read(reader, item);
    internal static void WriteTable<T>(BinaryWriter writer, Table<T> table, Action<BinaryWriter, T> item) where T : struct => Write(writer, table, item);

    private static Table<T> Read<T>(BinaryReader reader, Func<BinaryReader, T> item) where T : struct
    {
        var table = new Table<T>();
        var count = reader.ReadInt32();
        if (count < 0 || reader.ReadInt32() != count) throw new InvalidDataException("Invalid animation hash table.");
        for (var i = 0; i < count; i++) table.Keys.Add((reader.ReadUInt32(), reader.ReadInt32()));
        for (var i = 0; i < count; i++) table.Values.Add(item(reader));
        for (var i = 1; i < count; i++)
            if (table.Keys[i - 1].Hash <= table.Keys[i].Hash) throw new InvalidDataException("Animation hash table is not sorted.");
        return table;
    }

    private static void Write<T>(BinaryWriter writer, Table<T> table, Action<BinaryWriter, T> item) where T : struct
    {
        writer.Write(table.Keys.Count); writer.Write(table.Keys.Count);
        foreach (var (hash, slot) in table.Keys) { writer.Write(hash); writer.Write(slot); }
        foreach (var value in table.Values) item(writer, value);
    }
}
