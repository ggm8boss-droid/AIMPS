using System.Threading.Channels;
using AIMP.Protocol;

namespace AIMP.Launcher;

internal sealed record AlienPvPNetworkBinding(LobbyHost? Host,LobbyClient? Client,
    Guid Session,int RoundGeneration,int WorldGeneration,int NativeGeneration,ulong OwnerPeer,
    ulong LocalPeer,Guid Connection,int ProcessId,bool Active);

// One bounded worker connects authenticated lobby data to the engine mailbox.
// No async-void event handlers, no native pointer on the network, no task per
// sample. Every delayed operation rechecks its original session + IPC lifetime.
internal sealed class AlienPvPNetworkBridge : IDisposable
{
    private sealed record Work(object? Source,object Value,long At);
    private readonly Func<AlienPvPNetworkBinding> _capture;
    private readonly NativeRuntimeController _controller;
    private readonly Action<string> _log;
    private readonly Channel<Work> _queue=Channel.CreateBounded<Work>(new BoundedChannelOptions(128)
    {SingleReader=true,SingleWriter=false,FullMode=BoundedChannelFullMode.Wait,AllowSynchronousContinuations=false});
    private readonly Channel<Work> _critical=Channel.CreateBounded<Work>(new BoundedChannelOptions(128)
    {SingleReader=true,SingleWriter=false,FullMode=BoundedChannelFullMode.Wait,AllowSynchronousContinuations=false});
    private readonly AlienPvPDeliveryOutbox _deliveries=new();
    private readonly AlienPvPDeliveryInbox _receipts=new();
    private Work? _criticalHead;
    private AlienPvPNativeFrame? _deferredCommit,_deferredPose;
    private long _deferredCommitAt,_deferredPoseAt;
    private int _overflow;
    private bool _transportFault;
    private readonly CancellationTokenSource _shutdown=new();
    private readonly Task _worker;
    private NativeAlienPvP? _native;
    private Guid _receipt;
    private AlienControlLease? _lease;
    private ulong _readySequence;
    private (ulong Epoch,bool Backstage,bool Busy)? _poseStage;
    private long _dropped,_stale,_inputs,_commits,_poses,_lastSummary,_lastError;
    private int _disposed;
    private readonly HashSet<AlienAction> _unsupported=new();

    internal AlienPvPNetworkBridge(NativeRuntimeController controller,
        Func<AlienPvPNetworkBinding> capture,Action<string> log)
    {
        _controller=controller;_capture=capture;_log=log;
        _controller.AlienPvPNativeProduced+=OnNative;
        _worker=Task.Run(RunAsync);
    }
    internal void Bind(LobbyHost host)
    {
        host.AlienControlLeaseChanged+=OnLease;
        host.AlienControlCommitted+=OnCommit;
        host.AlienControlPoseReceived+=OnPose;
        host.AlienActionResolved+=OnResolution;
    }
    internal void Unbind(LobbyHost host)
    {
        host.AlienControlLeaseChanged-=OnLease;
        host.AlienControlCommitted-=OnCommit;
        host.AlienControlPoseReceived-=OnPose;
        host.AlienActionResolved-=OnResolution;
        Post(host,"reset");
    }
    internal void Bind(LobbyClient client)
    {
        client.AlienControlLeaseChanged+=OnLease;
        client.AlienControlCommitted+=OnCommit;
        client.AlienControlPoseReceived+=OnPose;
        client.AlienActionResolved+=OnResolution;
    }
    internal void Unbind(LobbyClient client)
    {
        client.AlienControlLeaseChanged-=OnLease;
        client.AlienControlCommitted-=OnCommit;
        client.AlienControlPoseReceived-=OnPose;
        client.AlienActionResolved-=OnResolution;
        Post(client,"reset");
    }
    private void OnNative(object? sender,NativeAlienPvP value)=>Post(sender,value);
    private void OnLease(object? sender,AlienControlLease value)=>Post(sender,value);
    private void OnCommit(object? sender,AlienControlCommit value)=>Post(sender,value);
    private void OnResolution(object? sender,AlienActionResolution value)=>Post(sender,value);
    private void OnPose(object? sender,AlienControlPose value)=>Post(sender,value);
    private static bool Critical(object value) => value switch
    {
        AlienControlLease or AlienActionResolution => true,
        AlienControlCommit c => c.Intent.Action!=AlienAction.None,
        NativeAlienPvP n => AlienPvPDelivery.Reliable(n.Frame) || n.Frame.Kind==AlienPvPNativeKind.Acknowledge,
        _ => false
    };
    private void Post(object? source,object value)
    {
        if(Volatile.Read(ref _disposed)!=0)return;
        var writer=Critical(value)?_critical.Writer:_queue.Writer;
        if(!writer.TryWrite(new(source,value,Environment.TickCount64)))
        {
            Interlocked.Increment(ref _dropped);
            if(Critical(value))Interlocked.Exchange(ref _overflow,1);
        }
    }
    private static bool SameNative(NativeAlienPvP n,AlienPvPNetworkBinding b) =>
        b.Active && n.ConnectionIdentity==b.Connection && n.ProcessId==b.ProcessId &&
        n.Frame.Scope.Cookie==AISyncCodec.SessionCookie(b.Session) &&
        n.Frame.Scope.NativeGeneration==(uint)b.NativeGeneration &&
        n.Frame.Scope.WorldGeneration==(uint)b.WorldGeneration && n.Frame.Scope.OwnerPeer==b.OwnerPeer;
    private bool SameLease(AlienControlLease lease,AlienPvPNetworkBinding b) =>
        _native is not null && SameNative(_native,b) && lease.SessionId==b.Session &&
        lease.LevelGeneration==b.RoundGeneration && lease.AlienPeerId==b.OwnerPeer &&
        lease.EntityA==_native.Frame.Scope.EntityA && lease.EntityB==_native.Frame.Scope.EntityB &&
        lease.Bindings.Any(p=>p.PeerId==b.LocalPeer && p.Receipt==_receipt);
    private bool SourceCurrent(object? source,AlienPvPNetworkBinding b) =>
        (b.Host is not null && ReferenceEquals(source,b.Host)) ||
        (b.Client is not null && ReferenceEquals(source,b.Client));
    private static bool Recent(long now,long at,long limit=250) => now>=at && now-at<=limit;
    private async Task RunAsync()
    {
        try
        {
            while(!_shutdown.IsCancellationRequested)
            {
                // A timed pump also resends a lost ACK without waiting for new input.
                await Task.Delay(5,_shutdown.Token).ConfigureAwait(false);
                if(Interlocked.Exchange(ref _overflow,0)!=0)_transportFault=true;
                for(var budget=0;budget<32;++budget)
                {
                    if(_criticalHead is null && !_critical.Reader.TryRead(out _criticalHead))break;
                    var work=_criticalHead!;
                    if(await TryProcessAsync(work).ConfigureAwait(false))_criticalHead=null;
                    else
                    {
                        if(!Recent(Environment.TickCount64,work.At,AlienPvPDelivery.DeadlineMs))
                        { _transportFault=true;_criticalHead=null; }
                        break; // Retry this discrete event before later ones.
                    }
                }
                for(var budget=0;budget<32 && _queue.Reader.TryRead(out var work);++budget)
                    await TryProcessAsync(work).ConfigureAwait(false);
                using var deadline=CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
                deadline.CancelAfter(TimeSpan.FromMilliseconds(400));
                try {await FlushDeliveriesAsync(deadline.Token).ConfigureAwait(false);}
                catch(OperationCanceledException) when(_shutdown.IsCancellationRequested){break;}
                catch(Exception error){ReportError(error);}
            }
        }
        catch(OperationCanceledException) when(_shutdown.IsCancellationRequested) { }
    }
    private void ReportError(Exception error)
    {
        var now=Environment.TickCount64;
        if(now-_lastError>=5000){_lastError=now;_log("Stream operation deferred: "+error.Message);}
    }
    private async Task<bool> TryProcessAsync(Work work)
    {
        using var deadline=CancellationTokenSource.CreateLinkedTokenSource(_shutdown.Token);
        deadline.CancelAfter(TimeSpan.FromMilliseconds(400));
        try
        {
            var b=_capture();var now=Environment.TickCount64;
            if(!b.Active){await StopAsync(deadline.Token).ConfigureAwait(false);return true;}
            if(work.Value is string)
            {
                if(_native is not null && !SameNative(_native,b))await StopAsync(deadline.Token).ConfigureAwait(false);
                return true;
            }
            if(work.Value is NativeAlienPvP native)
            {
                if(!SameNative(native,b)){Interlocked.Increment(ref _stale);return true;}
                if(native.Frame.Kind==AlienPvPNativeKind.Acknowledge)
                { _deliveries.Acknowledge(native.Frame);return true; }
                var f=native.Frame;
                if(f.Kind==AlienPvPNativeKind.Status && (f.Sequence==0 || f.Sequence>uint.MaxValue))return true;
                if(f.Kind!=AlienPvPNativeKind.Status &&
                    (_native is null || _native.Frame.Scope!=f.Scope))return true; // wait for actor Status, no ACK
                if(f.Kind==AlienPvPNativeKind.Status && _native is not null &&
                    _native.ConnectionIdentity==native.ConnectionIdentity && _native.ProcessId==native.ProcessId &&
                    _native.Frame.Scope.NativeGeneration==f.Scope.NativeGeneration &&
                    (f.Scope.Incarnation<_native.Frame.Scope.Incarnation ||
                     (f.Scope==_native.Frame.Scope && f.Sequence<_native.Frame.Sequence)))return true;
                if(f.DeliveryId!=0)
                {
                    var verdict=_receipts.Inspect(f.DeliveryId);
                    if(verdict==AlienPvPDeliveryInbox.Verdict.Gap)return true; // sender retries its head
                    if(verdict==AlienPvPDeliveryInbox.Verdict.Duplicate)
                    { await SendNativeAsync(AlienPvPDelivery.Acknowledgement(f),deadline.Token).ConfigureAwait(false);return true; }
                }
                var fresh=Recent(now,work.At,1000) && f.SampledAtMs<=long.MaxValue &&
                    Recent(now,(long)f.SampledAtMs,AlienPvPDelivery.Reliable(f)?2000:500);
                // A stale click is not executed later as a new click. Retire its
                // delivery, invalidate readiness, and request a fresh epoch.
                if(f.Kind==AlienPvPNativeKind.Input && !Recent(now,(long)f.SampledAtMs,500))fresh=false;
                if(fresh)await HandleNativeAsync(native,b,deadline.Token).ConfigureAwait(false);
                else {Interlocked.Increment(ref _stale);if(f.DeliveryId!=0)_transportFault=true;}
                if(f.DeliveryId!=0)
                {
                    _receipts.Commit(f.DeliveryId);
                    await SendNativeAsync(AlienPvPDelivery.Acknowledgement(f),deadline.Token).ConfigureAwait(false);
                }
            }
            else if(SourceCurrent(work.Source,b))
            {
                switch(work.Value)
                {
                    case AlienControlLease lease:
                        await HandleLeaseAsync(lease,b,deadline.Token).ConfigureAwait(false);break;
                    case AlienControlCommit commit when Recent(now,work.At,commit.Intent.Action==AlienAction.None?250:500):
                        await HandleCommitAsync(commit,b,deadline.Token).ConfigureAwait(false);break;
                    case AlienActionResolution resolution when Recent(now,work.At,2000):
                        await HandleResolutionAsync(resolution,b,deadline.Token).ConfigureAwait(false);break;
                    case AlienControlPose pose when Recent(now,work.At):
                        await HandlePoseAsync(pose,b,deadline.Token).ConfigureAwait(false);break;
                    default:Interlocked.Increment(ref _stale);if(Critical(work.Value))_transportFault=true;break;
                }
            }
            else Interlocked.Increment(ref _stale);
            if(now-_lastSummary>=5000)
            {
                _lastSummary=now;
                _log($"stream epoch={_lease?.Epoch??0} ready={_lease?.Active==true} input={_inputs} commit={_commits} pose={_poses} stale={_stale} overflow={Interlocked.Read(ref _dropped)} pendingDelivery={_deliveries.Count} fault={_transportFault}");
            }
            return true;
        }
        catch(OperationCanceledException) when(_shutdown.IsCancellationRequested){return false;}
        catch(Exception error){ReportError(error);return false;}
    }
    private async Task FlushDeliveriesAsync(CancellationToken token)
    {
        var n=_native;var b=_capture();var now=Environment.TickCount64;
        if(n is null || !SameNative(n,b))return;
        if(_deliveries.Expired(now))_transportFault=true;
        if(_deliveries.Due(now) is { } frame)
            await _controller.SendAlienPvPAsync(n.ConnectionIdentity,n.ProcessId,frame,token).ConfigureAwait(false);
        if(_deliveries.Count!=0)return;
        // Motion may coalesce, actions never do. No newer steering can pass an
        // unacknowledged discrete Commit in the native gameplay sequence.
        var move=_deferredCommit;var pose=_deferredPose;
        _deferredCommit=_deferredPose=null;
        if(move is not null && Recent(now,_deferredCommitAt))await SendNativeAsync(move,token).ConfigureAwait(false);
        if(pose is not null && Recent(now,_deferredPoseAt))await SendNativeAsync(pose,token).ConfigureAwait(false);
    }
    private async Task SendNativeAsync(AlienPvPNativeFrame frame,CancellationToken token)
    {
        var n=_native;var current=_capture();
        if(n is null || !SameNative(n,current) || frame.Scope!=n.Frame.Scope)return;
        var now=Environment.TickCount64;
        if(AlienPvPDelivery.Reliable(frame))
        {
            if(!_deliveries.Enqueue(frame,now))throw new ProtocolException("Native reliable delivery journal is full.");
            return;
        }
        if(_deliveries.Count!=0 && frame.Kind==AlienPvPNativeKind.Commit)
        {_deferredCommit=frame;_deferredCommitAt=now;return;}
        if(_deliveries.Count!=0 && frame.Kind==AlienPvPNativeKind.Correction)
        {_deferredPose=frame;_deferredPoseAt=now;return;}
        await _controller.SendAlienPvPAsync(n.ConnectionIdentity,n.ProcessId,frame,token).ConfigureAwait(false);
    }
    private async Task StopAsync(CancellationToken token)
    {
        var native=_native;var lease=_lease;
        _native=null;_lease=null;_receipt=Guid.Empty;_unsupported.Clear();
        _deliveries.Clear();_receipts.Clear();_deferredCommit=_deferredPose=null;_transportFault=false;
        if(native is null || lease is not {Active:true})return;
        // The controller's exact connection check rejects a replacement DLL.
        await _controller.SendAlienPvPAsync(native.ConnectionIdentity,native.ProcessId,
            new(){Kind=AlienPvPNativeKind.Stop,Scope=native.Frame.Scope,Epoch=lease.Epoch},token).ConfigureAwait(false);
    }
    private async Task HandleNativeAsync(NativeAlienPvP native,AlienPvPNetworkBinding b,CancellationToken token)
    {
        var f=native.Frame;
        if(f.Kind==AlienPvPNativeKind.Status)
        {
            if(_native is null || _native.ConnectionIdentity!=native.ConnectionIdentity ||
                _native.ProcessId!=native.ProcessId || _native.Frame.Scope!=f.Scope || _native.Frame.Sequence!=f.Sequence)
            {
                _native=native;_receipt=Guid.NewGuid();_lease=null;_unsupported.Clear();
                _deliveries.Clear(f.Sequence<<32);_receipts.Clear(f.Sequence<<32);_deferredCommit=_deferredPose=null;_transportFault=false;
                _log($"Native actor receipt changed: entity={f.Scope.EntityA:X8}:{f.Scope.EntityB:X8}, world={f.Scope.WorldGeneration}, native={f.Scope.NativeGeneration}; awaiting all replicas.");
            }
            else _native=native;
            var localOwner=b.LocalPeer==b.OwnerPeer;
            var ready=!_transportFault && f.Flags.HasFlag(AlienPvPNativeFlags.Bound) &&
                f.Flags.HasFlag(AlienPvPNativeFlags.PoseValid) &&
                f.Flags.HasFlag(AlienPvPNativeFlags.PilotReady) &&
                f.Flags.HasFlag(AlienPvPNativeFlags.LocalOwner)==localOwner &&
                f.Flags.HasFlag(AlienPvPNativeFlags.Host)==(b.Host is not null);
            if(_readySequence==ulong.MaxValue)throw new ProtocolException("Alien readiness sequence exhausted.");
            var proof=new AlienControlReady(b.Session,b.RoundGeneration,b.LocalPeer,b.OwnerPeer,
                ++_readySequence,_receipt,f.Scope.EntityA,f.Scope.EntityB,ready);
            if(b.Host is not null)await b.Host.ReportHostAlienReadyAsync(proof,token).ConfigureAwait(false);
            else if(b.Client is not null)await b.Client.ReportAlienReadyAsync(proof,token).ConfigureAwait(false);
            if(_lease is {Active:true} lease && SameLease(lease,_capture()) && f.Epoch!=lease.Epoch)
                await SendNativeAsync(new(){Kind=AlienPvPNativeKind.Activate,Scope=f.Scope,Epoch=lease.Epoch},token).ConfigureAwait(false);
            return;
        }
        if(_native is null || _native.Frame.Scope!=f.Scope || _lease is not {Active:true} active ||
            active.Epoch!=f.Epoch || !SameLease(active,b))return;
        if(f.Kind==AlienPvPNativeKind.Input && b.OwnerPeer==b.LocalPeer &&
            f.Flags.HasFlag(AlienPvPNativeFlags.LocalOwner))
        {
            var intent=new AlienControlIntent(b.Session,b.OwnerPeer,b.RoundGeneration,f.Sequence,
                f.Forward,f.Strafe,Math.Clamp(f.Yaw*180F/MathF.PI,-180F,180F),
                Math.Clamp(f.Pitch*180F/MathF.PI,-90F,90F),f.Gait>=2,f.Gait==0,f.Action,f.Epoch,f.Gait,
                f.Flags.HasFlag(AlienPvPNativeFlags.Focused),
                f.Flags.HasFlag(AlienPvPNativeFlags.VentSelected)?new(f.X,f.Y,f.Z):null);
            if(b.Host is not null)await b.Host.SubmitHostAlienControlAsync(intent,token).ConfigureAwait(false);
            else if(b.Client is not null)await b.Client.SendAlienControlAsync(intent,token).ConfigureAwait(false);
            ++_inputs;
        }
        else if(f.Kind==AlienPvPNativeKind.Pose && f.Flags.HasFlag(AlienPvPNativeFlags.PoseValid))
        {
            var stage=(f.Epoch,f.Flags.HasFlag(AlienPvPNativeFlags.Backstage),f.Flags.HasFlag(AlienPvPNativeFlags.NativeBusy));
            if(_poseStage!=stage)
            {
                _poseStage=stage;
                _log($"Native stage epoch={f.Epoch} backstage={stage.Item2} busy={stage.Item3} appliedHost={f.HostSequence} input={f.AcknowledgedInput} pos=({f.X:F2},{f.Y:F2},{f.Z:F2}).");
            }
            var pose=new AlienControlPose(b.Session,b.RoundGeneration,b.OwnerPeer,f.Epoch,
                f.Sequence,f.HostSequence,f.AcknowledgedInput,f.Scope.EntityA,f.Scope.EntityB,
                f.X,f.Y,f.Z,f.Yaw,f.VelocityX,f.VelocityZ,f.Gait,
                f.Flags.HasFlag(AlienPvPNativeFlags.NativeBusy),f.Flags.HasFlag(AlienPvPNativeFlags.Backstage),
                VentAnimating:f.Flags.HasFlag(AlienPvPNativeFlags.VentAnimating),
                Crawling:f.Flags.HasFlag(AlienPvPNativeFlags.Crawling));
            if(b.Host is not null && f.Flags.HasFlag(AlienPvPNativeFlags.Host))
                await b.Host.PublishHostAlienPoseAsync(pose,token).ConfigureAwait(false);
            else if(b.Client is not null && b.OwnerPeer==b.LocalPeer && f.Flags.HasFlag(AlienPvPNativeFlags.LocalOwner))
                await b.Client.SendAlienPoseObservationAsync(pose,token).ConfigureAwait(false);
            ++_poses;
        }
        else if(f.Kind==AlienPvPNativeKind.Resolution && b.Host is not null && f.Flags.HasFlag(AlienPvPNativeFlags.Host))
        {
            await b.Host.PublishHostAlienActionAsync(new(b.Session,b.RoundGeneration,b.OwnerPeer,f.Epoch,
                f.Sequence,f.HostSequence,f.Action,f.ResolutionRevision,f.Phase,f.Reason,f.Scope.EntityA,f.Scope.EntityB,
                f.TargetPeer,f.TargetA,f.TargetB,f.Selector,f.X,f.Y,f.Z,f.ResolvedX,f.ResolvedY,f.ResolvedZ),token).ConfigureAwait(false);
            _log($"Host action {f.HostSequence} {f.Action}: {f.Phase} ({f.Reason}), target={f.TargetPeer}.");
        }
        else if(f.Kind==AlienPvPNativeKind.ActionResult)
        {
            // Host approval and replica execution are different facts. Report
            // local NoVent/WrongStage/Fault rather than silently logging only
            // the successful Host resolution while this instance did nothing.
            _log($"Local native action {f.HostSequence} {f.Action}: {f.Phase} ({f.Reason}), target={f.TargetPeer}.");
            if(f.Flags.HasFlag(AlienPvPNativeFlags.UnsupportedAction) && _unsupported.Add(f.Action))
                _log($"Native action {f.Action} has no certified adapter for this runtime; no fabricated animation/damage/teleport was applied.");
        }
    }
    private async Task HandleLeaseAsync(AlienControlLease lease,AlienPvPNetworkBinding b,CancellationToken token)
    {
        if(!SameLease(lease,b) || (_lease is not null && (lease.Epoch<_lease.Epoch ||
            (lease.Epoch==_lease.Epoch && !_lease.Active && lease.Active))))return;
        _lease=lease;
        if(!lease.Active)_transportFault=false;
        await SendNativeAsync(new(){Kind=lease.Active?AlienPvPNativeKind.Activate:AlienPvPNativeKind.Stop,
            Scope=_native!.Frame.Scope,Epoch=lease.Epoch},token).ConfigureAwait(false);
        _log($"Host {(lease.Active?"granted":"revoked")} epoch {lease.Epoch} for local receipt {_receipt:N}.");
    }
    private async Task HandleCommitAsync(AlienControlCommit commit,AlienPvPNetworkBinding b,CancellationToken token)
    {
        var i=commit.Intent;
        if(_lease is not {Active:true} lease || !SameLease(lease,b) || i.Epoch!=lease.Epoch ||
            i.SessionId!=b.Session || i.LevelGeneration!=b.RoundGeneration || i.AlienPeerId!=b.OwnerPeer)return;
        await SendNativeAsync(new(){Kind=AlienPvPNativeKind.Commit,Scope=_native!.Frame.Scope,
            Epoch=i.Epoch,Sequence=i.Sequence,HostSequence=commit.HostSequence,
            Flags=(i.Focused?AlienPvPNativeFlags.Focused:AlienPvPNativeFlags.None)|
                (i.Vent is not null?AlienPvPNativeFlags.VentSelected:AlienPvPNativeFlags.None),
            X=i.Vent?.X??0,Y=i.Vent?.Y??0,Z=i.Vent?.Z??0,
            Forward=i.Forward,Strafe=i.Strafe,Yaw=i.Yaw*MathF.PI/180F,Pitch=i.Pitch*MathF.PI/180F,
            Gait=i.Gait,Action=i.Action},token).ConfigureAwait(false);
        ++_commits;
    }
    private async Task HandleResolutionAsync(AlienActionResolution r,AlienPvPNetworkBinding b,CancellationToken token)
    {
        if(b.Host is not null || _lease is not {Active:true} l || !SameLease(l,b) || r.SessionId!=b.Session ||
            r.LevelGeneration!=b.RoundGeneration || r.AlienPeerId!=b.OwnerPeer || r.Epoch!=l.Epoch ||
            r.EntityA!=l.EntityA || r.EntityB!=l.EntityB)return;
        await SendNativeAsync(new(){Kind=AlienPvPNativeKind.Resolution,Scope=_native!.Frame.Scope,
            Epoch=r.Epoch,Sequence=r.InputSequence,HostSequence=r.HostSequence,Action=r.Action,
            ResolutionRevision=r.Revision,Phase=r.Phase,Reason=r.Reason,TargetPeer=r.TargetPeer,
            TargetA=r.TargetA,TargetB=r.TargetB,Selector=r.Selector,X=r.X,Y=r.Y,Z=r.Z,
            ResolvedX=r.ResolvedX,ResolvedY=r.ResolvedY,ResolvedZ=r.ResolvedZ},token).ConfigureAwait(false);
        _log($"Action {r.HostSequence} {r.Action}: Host {r.Phase} ({r.Reason}).");
    }
    private async Task HandlePoseAsync(AlienControlPose pose,AlienPvPNetworkBinding b,CancellationToken token)
    {
        if((b.Host is not null && !pose.OwnerMeasured) || _lease is not {Active:true} lease || !SameLease(lease,b) ||
            pose.SessionId!=b.Session || pose.LevelGeneration!=b.RoundGeneration || pose.AlienPeerId!=b.OwnerPeer ||
            pose.Epoch!=lease.Epoch || pose.EntityA!=lease.EntityA || pose.EntityB!=lease.EntityB)return;
        await SendNativeAsync(new(){Kind=AlienPvPNativeKind.Correction,Scope=_native!.Frame.Scope,
            Epoch=pose.Epoch,Sequence=pose.Sequence,HostSequence=pose.HostSequence,AcknowledgedInput=pose.AcknowledgedInput,
            Flags=AlienPvPNativeFlags.PoseValid|(pose.NativeBusy?AlienPvPNativeFlags.NativeBusy:AlienPvPNativeFlags.None)|
                (pose.Backstage?AlienPvPNativeFlags.Backstage:AlienPvPNativeFlags.None)|
                (pose.VentAnimating?AlienPvPNativeFlags.VentAnimating:AlienPvPNativeFlags.None)|
                (pose.OwnerMeasured?AlienPvPNativeFlags.OwnerMeasured:AlienPvPNativeFlags.None)|
                (pose.Crawling?AlienPvPNativeFlags.Crawling:AlienPvPNativeFlags.None),
            X=pose.X,Y=pose.Y,Z=pose.Z,Yaw=pose.Yaw,VelocityX=pose.VelocityX,VelocityZ=pose.VelocityZ,Gait=pose.Gait},token).ConfigureAwait(false);
        ++_poses;
    }
    public void Dispose()
    {
        if(Interlocked.Exchange(ref _disposed,1)!=0)return;
        _controller.AlienPvPNativeProduced-=OnNative;
        _queue.Writer.TryComplete();_critical.Writer.TryComplete();_shutdown.Cancel();
        _worker.GetAwaiter().GetResult();_shutdown.Dispose();
        // The coordinator separately performs the certified native lease release.
    }
}
