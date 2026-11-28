namespace AIMP.Protocol;

public sealed partial class LobbyHost
{
    private readonly SemaphoreSlim _alienControlDispatchGate = new(1, 1);
    private readonly AlienControlAuthority _alienAuthority = new();
    public event EventHandler<AlienControlCommit>? AlienControlCommitted;
    public event EventHandler<AlienControlLease>? AlienControlLeaseChanged;
    public event EventHandler<AlienControlPose>? AlienControlPoseReceived;
    public event EventHandler<AlienActionResolution>? AlienActionResolved;

    public Task SubmitHostAlienControlAsync(AlienControlIntent intent,
        CancellationToken cancellationToken = default) =>
        AcceptAlienControlAsync(intent, HostPeerId, cancellationToken);
    public Task ReportHostAlienReadyAsync(AlienControlReady ready,
        CancellationToken cancellationToken = default) =>
        AcceptAlienReadyAsync(ready,HostPeerId,cancellationToken);

    // All three admission paths share ordering, including revoke -> new epoch.
    // _stateLock protects lobby data; native work is never done under that lock.
    private AlienControlLease? EvaluateAlienReadinessLocked(long now)
    {
        if(!_options.AlienVsSurvivors || _state!="Starting" || _alienPeerId==0)
        {
            var old=_alienAuthority.Current;
            _alienAuthority.Clear();
            return old is {Active:true}?old with {Active=false}:null;
        }
        _alienAuthority.Begin(SessionId,_levelGeneration,_alienPeerId);
        // The Host is always the physical simulation authority, even after its
        // survivor dies. Dead remote survivors no longer block live players.
        var peers=_clients.Keys.Where(p=>p==_alienPeerId || !_deadPeers.Contains(p))
            .Append(HostPeerId).ToArray();
        return _alienAuthority.Evaluate(peers,now);
    }
    private async Task PublishAlienLeaseAsync(AlienControlLease? lease,CancellationToken token)
    {
        if(lease is null)return;
        AlienControlAuthorityValidation.Validate(lease);
        AlienControlLeaseChanged?.Invoke(this,lease);
        await BroadcastAsync(MessageType.AlienControlLease,JsonPayload.Serialize(lease),token).ConfigureAwait(false);
        Log?.Invoke(this,$"Alien possession epoch {lease.Epoch}: {(lease.Active?"active":"revoked")}, replicas={lease.Bindings.Count}.");
    }
    private async Task HandleAlienReadyAsync(ClientState client,ReadOnlyMemory<byte> bytes,CancellationToken token)
    {
        var ready=JsonPayload.Deserialize<AlienControlReady>(bytes.Span);
        await AcceptAlienReadyAsync(ready,client.PeerId,token).ConfigureAwait(false);
    }
    private async Task AcceptAlienReadyAsync(AlienControlReady ready,ulong sender,CancellationToken token)
    {
        AlienControlAuthorityValidation.Validate(ready);
        if(ready.PeerId!=sender)throw new ProtocolException("Alien readiness cannot impersonate another participant.");
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlLease? changed;
            lock(_stateLock)
            {
                if(!_options.AlienVsSurvivors || ready.SessionId!=SessionId ||
                    (sender!=HostPeerId && !_clients.ContainsKey(sender)))
                    throw new ProtocolException("Alien readiness belongs to a different session.");
                // A queued receipt from a finished round is harmless, not a new
                // grant and not grounds for disconnecting the next round.
                if(_state!="Starting" || ready.LevelGeneration!=_levelGeneration || ready.AlienPeerId!=_alienPeerId)return;
                _alienAuthority.Begin(SessionId,_levelGeneration,_alienPeerId);
                _alienAuthority.Observe(ready,Environment.TickCount64);
                changed=EvaluateAlienReadinessLocked(Environment.TickCount64);
            }
            await PublishAlienLeaseAsync(changed,token).ConfigureAwait(false);
        }
        finally{_alienControlDispatchGate.Release();}
    }
    private async Task HandleAlienControlProposalAsync(ClientState client,
        ReadOnlyMemory<byte> bytes,CancellationToken token)
    {
        var intent=JsonPayload.Deserialize<AlienControlIntent>(bytes.Span);
        if(intent.AlienPeerId!=client.PeerId)throw new ProtocolException("Alien control cannot spoof another participant.");
        await AcceptAlienControlAsync(intent,client.PeerId,token).ConfigureAwait(false);
    }
    private async Task AcceptAlienControlAsync(AlienControlIntent intent,ulong sender,CancellationToken token)
    {
        AlienControlProtocol.Validate(intent);
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlCommit? committed;AlienControlLease? changed;
            lock(_stateLock)
            {
                if(!_options.AlienVsSurvivors || intent.SessionId!=SessionId || intent.AlienPeerId!=sender)
                    throw new ProtocolException("Alien input belongs to another participant/session.");
                if(_state!="Starting" || intent.LevelGeneration!=_levelGeneration)return;
                if(_alienPeerId!=sender)throw new ProtocolException("Only the selected Alien can decide its actions.");
                changed=EvaluateAlienReadinessLocked(Environment.TickCount64);
                committed=_alienAuthority.Admit(intent,Environment.TickCount64) ?? _alienAuthority.Replay(intent);
            }
            await PublishAlienLeaseAsync(changed,token).ConfigureAwait(false);
            if(committed is null)return;
            // Do not delay local admission until every remote socket has drained.
            AlienControlCommitted?.Invoke(this,committed);
            await BroadcastAsync(MessageType.AlienControlCommitted,JsonPayload.Serialize(committed),token).ConfigureAwait(false);
        }
        finally{_alienControlDispatchGate.Release();}
    }
    public async Task PublishHostAlienPoseAsync(AlienControlPose pose,CancellationToken token=default)
    {
        AlienControlAuthorityValidation.Validate(pose);
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlLease? changed;bool admitted;
            lock(_stateLock)
            {
                changed=EvaluateAlienReadinessLocked(Environment.TickCount64);
                admitted=_alienAuthority.AdmitPose(pose,Environment.TickCount64);
            }
            await PublishAlienLeaseAsync(changed,token).ConfigureAwait(false);
            if(!admitted)return;
            AlienControlPoseReceived?.Invoke(this,pose);
            // AvS pass 19: when the Host itself pilots the Alien, its physical pose
            // IS the owner's measurement. Mark it so survivor PCs run the owner
            // presentation (committed input + per-frame pose blend, vent replay)
            // instead of generic AI routing. The local event keeps the raw pose.
            bool hostPilot;
            lock(_stateLock) hostPilot=_alienPeerId==HostPeerId;
            var outgoing=hostPilot?pose with {OwnerMeasured=true}:pose;
            await BroadcastAsync(MessageType.AlienControlPose,JsonPayload.Serialize(outgoing),token).ConfigureAwait(false);
        }
        finally{_alienControlDispatchGate.Release();}
    }
    private async Task HandleAlienPoseObservationAsync(ClientState client,ReadOnlyMemory<byte> bytes,CancellationToken token)
    {
        var pose=JsonPayload.Deserialize<AlienControlPose>(bytes.Span);
        AlienControlAuthorityValidation.Validate(pose);
        if(pose.AlienPeerId!=client.PeerId || client.PeerId==HostPeerId)
            throw new ProtocolException("Only the selected Alien owner may report its physical observation.");
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlPose? accepted;
            AlienControlLease? changed;
            lock(_stateLock)
            {
                changed=EvaluateAlienReadinessLocked(Environment.TickCount64);
                accepted=_alienAuthority.AdmitOwnerPose(pose,Environment.TickCount64);
            }
            await PublishAlienLeaseAsync(changed,token).ConfigureAwait(false);
            if(accepted is null)return;
            AlienControlPoseReceived?.Invoke(this,accepted);
            await BroadcastAsync(MessageType.AlienControlPose,JsonPayload.Serialize(accepted),token).ConfigureAwait(false);
        }
        finally {_alienControlDispatchGate.Release();}
    }
    // This entry is local-only, fed by the authenticated native IPC of this
    // Host. Client sockets have no handler that can invoke it.
    public async Task PublishHostAlienActionAsync(AlienActionResolution resolution,CancellationToken token=default)
    {
        AlienActionValidation.Validate(resolution);
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlLease? changed;bool admitted;
            lock(_stateLock)
            {
                changed=EvaluateAlienReadinessLocked(Environment.TickCount64);
                // Native validation happens before entry. A death notification
                // may race the Started receipt: do not reject a real attack just
                // because its already-authoritative consequence arrived first.
                admitted=_alienAuthority.AdmitResolution(resolution,Environment.TickCount64) || _alienAuthority.IsResolutionReplay(resolution);
            }
            await PublishAlienLeaseAsync(changed,token).ConfigureAwait(false);
            if(!admitted)return;
            AlienActionResolved?.Invoke(this,resolution);
            await BroadcastAsync(MessageType.AlienActionResolved,JsonPayload.Serialize(resolution),token).ConfigureAwait(false);
        }
        finally{_alienControlDispatchGate.Release();}
    }
    private async Task RevokeAlienControlAsync(CancellationToken token)
    {
        await _alienControlDispatchGate.WaitAsync(token).ConfigureAwait(false);
        try
        {
            AlienControlLease? lease;
            lock(_stateLock)
            {
                lease=_alienAuthority.Current is {Active:true} active?active with {Active=false}:null;
                _alienAuthority.Clear();
            }
            await PublishAlienLeaseAsync(lease,token).ConfigureAwait(false);
        }
        finally{_alienControlDispatchGate.Release();}
    }
}
