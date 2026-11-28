namespace AIMP.Protocol;

// Receipt is a fresh opaque GUID allocated by the launcher for an exact local
// (IPC connection, process, world, native actor ticket). It is not a pointer or
// an assertion that two native runtime IDs match on different computers.
public sealed record AlienControlReady(Guid SessionId,int LevelGeneration,ulong PeerId,
    ulong AlienPeerId,ulong Sequence,Guid Receipt,uint EntityA,uint EntityB,bool Ready);
public sealed record AlienControlBinding(ulong PeerId,Guid Receipt);
public sealed record AlienControlLease(Guid SessionId,int LevelGeneration,ulong AlienPeerId,
    ulong Epoch,uint EntityA,uint EntityB,bool Active,IReadOnlyList<AlienControlBinding> Bindings);
public sealed record AlienControlPose(Guid SessionId,int LevelGeneration,ulong AlienPeerId,
    ulong Epoch,ulong Sequence,ulong HostSequence,ulong AcknowledgedInput,uint EntityA,uint EntityB,
    float X,float Y,float Z,float Yaw,float VelocityX,float VelocityZ,uint Gait,bool NativeBusy,bool Backstage=false,bool OwnerMeasured=false,
    bool VentAnimating=false,bool Crawling=false);

public static class AlienControlAuthorityValidation
{
    public static void Validate(AlienControlReady? r)
    {
        if(r is null || r.SessionId==Guid.Empty || r.LevelGeneration<1 || r.PeerId==0 ||
            r.AlienPeerId==0 || r.Sequence==0 || r.Receipt==Guid.Empty || (r.EntityA==0 && r.EntityB==0))
            throw new ProtocolException("Invalid Alien readiness receipt.");
    }
    public static void Validate(AlienControlLease? l)
    {
        if(l is null || l.SessionId==Guid.Empty || l.LevelGeneration<1 || l.AlienPeerId==0 ||
            l.Epoch==0 || (l.EntityA==0 && l.EntityB==0) || l.Bindings is null ||
            l.Bindings.Count is <1 or >32 || l.Bindings.Any(b=>b is null || b.PeerId==0 || b.Receipt==Guid.Empty) ||
            l.Bindings.Select(b=>b.PeerId).Distinct().Count()!=l.Bindings.Count ||
            !l.Bindings.Any(b=>b.PeerId==l.AlienPeerId))
            throw new ProtocolException("Invalid Alien possession epoch.");
    }
    public static void Validate(AlienControlPose? p)
    {
        if(p is null || p.SessionId==Guid.Empty || p.LevelGeneration<1 || p.AlienPeerId==0 ||
            p.Epoch==0 || p.Sequence==0 || (p.EntityA==0 && p.EntityB==0) || p.Gait>3)
            throw new ProtocolException("Invalid Alien pose identity.");
        foreach(var v in new[]{p.X,p.Y,p.Z,p.Yaw,p.VelocityX,p.VelocityZ})
            if(!float.IsFinite(v))throw new ProtocolException("Non-finite Alien pose.");
        if(p.VentAnimating && (!p.NativeBusy || !p.Backstage))throw new ProtocolException("Invalid Alien vent animation stage.");
        if(Math.Abs(p.X)>100000 || Math.Abs(p.Y)>100000 || Math.Abs(p.Z)>100000 ||
            Math.Abs(p.Yaw)>MathF.PI+0.00001F ||
            Math.Sqrt((double)p.VelocityX*p.VelocityX+(double)p.VelocityZ*p.VelocityZ)>30.01)
            throw new ProtocolException("Alien pose outside supported bounds.");
    }
}

// Pure state machine, no IO/clock/native reads. Caller serializes access and
// supplies its OWN monotonic clock. Never compare wall clocks between PCs.
public sealed class AlienControlAuthority
{
    public const long ReadinessTimeoutMs=1500;
    private readonly Dictionary<ulong,(AlienControlReady Proof,long At)> _ready=new();
    private readonly AlienActionJournal _actions=new();
    private readonly SortedDictionary<ulong,AlienControlCommit> _recentCommits=new();
    private Guid _session;
    private int _generation;
    private ulong _owner,_epochCounter,_lastInput,_hostSequence,_lastPose,_lastPoseHost,_lastPoseInput;
    private long _rateAt;
    private int _rateCount;
    private AlienControlPose? _lastPhysicalPose;
    private AlienActionResolution? _lastVentResolution;
    private bool _ventTransitionConsumed=true;
    private long _physicalAt,_ventAt,_ventDeadline;
    private ulong _observationEpoch,_observationSequence;
    public AlienControlLease? Current {get;private set;}
    public void Begin(Guid session,int generation,ulong owner)
    {
        if(_session==session && _generation==generation && _owner==owner)return;
        _session=session;_generation=generation;_owner=owner;
        _ready.Clear();_actions.Clear();_recentCommits.Clear();Current=null;_lastInput=_hostSequence=_lastPose=_lastPoseHost=_lastPoseInput=0;_rateAt=0;_rateCount=0;
        // Epochs never repeat during this lobby's lifetime, even after a round.
    }
    public void Clear()
    { _ready.Clear();_actions.Clear();_recentCommits.Clear();Current=null;_session=Guid.Empty;_generation=0;_owner=0; }
    public bool Observe(AlienControlReady proof,long now)
    {
        AlienControlAuthorityValidation.Validate(proof);
        if(proof.SessionId!=_session || proof.LevelGeneration!=_generation || proof.AlienPeerId!=_owner)return false;
        if(_ready.TryGetValue(proof.PeerId,out var old) && proof.Sequence<=old.Proof.Sequence)return false;
        _ready[proof.PeerId]=(proof,now);return true;
    }
    public AlienControlLease? Evaluate(IReadOnlyCollection<ulong> peers,long now)
    {
        var expected=peers.Distinct().OrderBy(p=>p).ToArray();
        var complete=expected.Length>=2 && expected.Contains(_owner) && !_actions.Stalled(now);
        var proofs=new List<AlienControlReady>();
        foreach(var peer in expected)
        {
            if(!_ready.TryGetValue(peer,out var p) || !p.Proof.Ready || now<p.At || now-p.At>ReadinessTimeoutMs)
            {complete=false;continue;}
            proofs.Add(p.Proof);
        }
        if(proofs.Count>0 && proofs.Any(p=>p.EntityA!=proofs[0].EntityA || p.EntityB!=proofs[0].EntityB))complete=false;
        var bindings=proofs.Select(p=>new AlienControlBinding(p.PeerId,p.Receipt)).ToArray();
        var same=complete && Current is {Active:true} c && c.EntityA==proofs[0].EntityA &&
            c.EntityB==proofs[0].EntityB && c.Bindings.SequenceEqual(bindings);
        if(same)return null;
        if(Current is {Active:true} active)
        {
            // Revoke first; do not transform an old readiness grant in place.
            Current=active with {Active=false};_actions.Clear();_recentCommits.Clear();return Current;
        }
        if(!complete)return null;
        if(_epochCounter==ulong.MaxValue)throw new ProtocolException("Alien epoch exhausted.");
        Current=new(_session,_generation,_owner,++_epochCounter,proofs[0].EntityA,proofs[0].EntityB,true,bindings);
        _actions.Clear();_recentCommits.Clear();_lastInput=_hostSequence=_lastPose=_lastPoseHost=_lastPoseInput=0;_rateAt=now;_rateCount=0;return Current;
    }
    public AlienControlCommit? Admit(AlienControlIntent intent,long now)
    {
        AlienControlProtocol.Validate(intent);
        if(Current is not {Active:true} c || intent.SessionId!=_session || intent.LevelGeneration!=_generation ||
            intent.AlienPeerId!=_owner || intent.Epoch!=c.Epoch || intent.Sequence<=_lastInput)return null;
        if(now<_rateAt || now-_rateAt>=1000){_rateAt=now;_rateCount=0;}
        if(++_rateCount>120)return null; // burst budget includes discrete action edges.
        if(_hostSequence==ulong.MaxValue)throw new ProtocolException("Alien command order exhausted.");
        if((intent.Action is AlienAction.Attack or AlienAction.EnterVent or AlienAction.ExitVent) && !_actions.CanIssue)return null;
        var commit=new AlienControlCommit(_hostSequence+1,intent);
        if(!_actions.Issue(commit,now))return null;
        _lastInput=intent.Sequence;++_hostSequence;
        _recentCommits[intent.Sequence]=commit;
        while(_recentCommits.Count>128)_recentCommits.Remove(_recentCommits.First().Key);
        return commit;
    }
    public AlienControlCommit? Replay(AlienControlIntent i) =>
        Current is {Active:true} c && c.Epoch==i.Epoch && _recentCommits.TryGetValue(i.Sequence,out var old) && old.Intent==i ? old : null;
    public bool IsResolutionReplay(AlienActionResolution r) =>
        Current is {Active:true} c && c.Epoch==r.Epoch && _actions.Contains(r);
    public bool AdmitResolution(AlienActionResolution resolution,long now=0)
    {
        AlienActionValidation.Validate(resolution);
        if(Current is not {Active:true} c || resolution.SessionId!=_session ||
            resolution.LevelGeneration!=_generation || resolution.AlienPeerId!=_owner ||
            resolution.Epoch!=c.Epoch || resolution.EntityA!=c.EntityA || resolution.EntityB!=c.EntityB)return false;
        if(!_actions.Accept(resolution,now))return false;
        if(resolution.Action is AlienAction.EnterVent or AlienAction.ExitVent)
        {
            // A replica can time out after the owner has already traversed.
            // Authorize the measured transition from our approved Started
            // selection, rather than requiring both native runs to complete.
            // Failed/Cancelled keep the unused stage proof; Rejected creates
            // none. Only animation playback retains a wall-clock deadline.
            if(resolution.Phase==AlienActionPhase.Started)
            {
                _lastVentResolution=resolution;_ventAt=now;
                _ventTransitionConsumed=false;
                _ventDeadline=now+(resolution.Action==AlienAction.EnterVent?12000:27000);
            }
            else if(resolution.Phase==AlienActionPhase.Completed)
            {_lastVentResolution=resolution;_ventAt=now;_ventDeadline=now+2000;}
        }
        return true;
    }
    public bool AdmitPose(AlienControlPose pose,long now=0)
    {
        AlienControlAuthorityValidation.Validate(pose);
        if(Current is not {Active:true} c || pose.SessionId!=_session || pose.LevelGeneration!=_generation ||
            pose.AlienPeerId!=_owner || pose.Epoch!=c.Epoch || pose.EntityA!=c.EntityA || pose.EntityB!=c.EntityB ||
            pose.Sequence<=_lastPose || pose.AcknowledgedInput>_lastInput || pose.HostSequence>_hostSequence ||
            pose.AcknowledgedInput<_lastPoseInput || pose.HostSequence<_lastPoseHost)return false;
        if(_observationEpoch==c.Epoch && _observationSequence>0)return false;
        _lastPhysicalPose=pose;_physicalAt=now;
        _lastPose=pose.Sequence;_lastPoseInput=pose.AcknowledgedInput;_lastPoseHost=pose.HostSequence;return true;
    }
    public AlienControlPose? AdmitOwnerPose(AlienControlPose pose,long now)
    {
        AlienControlAuthorityValidation.Validate(pose);
        if(Current is not {Active:true} c || pose.SessionId!=_session || pose.LevelGeneration!=_generation ||
            pose.AlienPeerId!=_owner || pose.Epoch!=c.Epoch || pose.EntityA!=c.EntityA || pose.EntityB!=c.EntityB ||
            (pose.NativeBusy && !pose.VentAnimating) || pose.HostSequence>_hostSequence || pose.AcknowledgedInput>_lastInput ||
            pose.HostSequence<_lastPoseHost || pose.AcknowledgedInput<_lastPoseInput ||
            _lastPhysicalPose is not { } old || old.Epoch!=c.Epoch || now<_physicalAt)return null;
        if(_observationEpoch!=c.Epoch){_observationEpoch=c.Epoch;_observationSequence=0;}
        if(pose.Sequence<=_observationSequence)return null;
        var dt=Math.Min(1.0,(now-_physicalAt)/1000.0);
        var dx=pose.X-old.X;var dz=pose.Z-old.Z;
        var ordinary=pose.Backstage==old.Backstage && Math.Sqrt(dx*dx+dz*dz)<=1.0+30.0*dt &&
            Math.Abs(pose.Y-old.Y)<=2.0+12.0*dt;
        var vent=_lastVentResolution;
        // A native passage may finish after cancellation was acknowledged.
        // Retain one unused stage reconciliation at the approved portal until
        // consumed or superseded. Journal timeout is not physical rollback.
        var transition=vent is not null && !_ventTransitionConsumed &&
            pose.Backstage!=old.Backstage && vent.Epoch==c.Epoch && vent.HostSequence<=pose.HostSequence &&
            now>=_ventAt && pose.Backstage==(vent.Action==AlienAction.EnterVent) &&
            Math.Sqrt(Math.Pow(pose.X-vent.ResolvedX,2)+Math.Pow(pose.Z-vent.ResolvedZ,2))<=6.0 &&
            Math.Abs(pose.Y-vent.ResolvedY)<=5.0;
        if(pose.VentAnimating) {
            // Only an observed passage at the Host-selected portal can
            // bypass the busy-pose fence. No arbitrary animation/placement.
            if(vent is not {Phase:AlienActionPhase.Started} ||
                vent.Epoch!=c.Epoch || vent.HostSequence>pose.HostSequence || now<_ventAt || now>_ventDeadline ||
                Math.Sqrt(Math.Pow(pose.X-vent.ResolvedX,2)+Math.Pow(pose.Z-vent.ResolvedZ,2))>6.0 ||
                Math.Abs(pose.Y-vent.ResolvedY)>5.0)return null;
        } else if(!ordinary && !transition)return null;
        if(!pose.VentAnimating && pose.Backstage!=old.Backstage && transition)_ventTransitionConsumed=true;
        if(_lastPose==ulong.MaxValue)throw new ProtocolException("Alien pose sequence exhausted.");
        _observationSequence=pose.Sequence;
        var accepted=pose with {Sequence=_lastPose+1,OwnerMeasured=true};
        _lastPose=accepted.Sequence;_lastPoseInput=accepted.AcknowledgedInput;_lastPoseHost=accepted.HostSequence;
        // Passage samples drive presentation, not the stable stage baseline.
        // A failed entry may still return to its original stage; conversely,
        // its first stable backstage sample must consume the pending proof.
        if(!pose.VentAnimating){_lastPhysicalPose=accepted;_physicalAt=now;}
        return accepted;
    }
}
