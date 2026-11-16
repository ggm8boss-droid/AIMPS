package frontend
{
   import flash.events.Event;
   import flash.events.IOErrorEvent;
   import flash.events.ProgressEvent;
   import flash.events.SecurityErrorEvent;
   import flash.net.Socket;
   import flash.utils.getTimer;
   
   public class MultiplayerBridge
   {
      
      private static const HOST:String = "127.0.0.1";
      
      private static const PORT:int = 12345;
      
      private static const PROTOCOL_VERSION:String = "1";
       
      
      private var _socket:Socket;
      
      private var _buffer:String;
      
      private var _connecting:Boolean = false;
      private var _active:Boolean = true;
      private var _lastConnectionError:String = "";
      private var _launcherReady:Boolean = false;
      private var _nextPoll:int = 0;
      private var _lastReady:int = 0;

      public var onVoiceSettings:Function;
      public function voiceCommand(type:String, values:Object) : void
      {
         if(type == "VOICE_GET_SETTINGS" || type == "VOICE_SAVE_SETTINGS" || type == "DEVELOPER_MODE") this.send(type,values);
      }
      public var onReadiness:Function;
      public var onSessionClosed:Function;
      
      private var _connected:Boolean;
      
      public var onStatus:Function;
      
      public var onServerList:Function;
      
      public var onLobbyState:Function;
      
      public var onGameStart:Function;
      
      public var onConfig:Function;
      
      public var onError:Function;
      
      public var onLobbyReturn:Function;
      public var onLobbyOpened:Function;
      public var onFriendList:Function;
      public var onInviteReceived:Function;

      public function MultiplayerBridge()
      {
         super();
         this._buffer = "";
         this._connected = false;
         this.connect();
      }
      
      private static function encodeValue(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         value = value.split("%").join("%25");
         value = value.split("|").join("%7C");
         value = value.split("=").join("%3D");
         value = value.split("\n").join("%0A");
         return value.split("\r").join("%0D");
      }
      
      private static function decodeValue(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         value = value.split("%0D").join("\r");
         value = value.split("%0A").join("\n");
         value = value.split("%3D").join("=");
         value = value.split("%7C").join("|");
         return value.split("%25").join("%");
      }
      
      private static function trim(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         while(value.length > 0 && (value.charAt(0) == " " || value.charAt(0) == "\r" || value.charAt(0) == "\n" || value.charAt(0) == "\t"))
         {
            value = value.substr(1);
         }
         while(value.length > 0 && (value.charAt(value.length - 1) == " " || value.charAt(value.length - 1) == "\r" || value.charAt(value.length - 1) == "\n" || value.charAt(value.length - 1) == "\t"))
         {
            value = value.substr(0,value.length - 1);
         }
         return value;
      }
      
      public function update() : void
      {
         if(!this._active) return;
         var now:int = getTimer();
         if(this._launcherReady && now - this._lastReady > 10000)
         {
            this.setReadyState(false);
            this.invokeError("Launcher is not responding. Waiting for it to be ready...");
         }
         if(now < this._nextPoll) return;
         this._nextPoll = now + 2000;
         if(!this._connected) this.connect();
         else this.send(this._launcherReady ? "MENU_STATE" : "HELLO",{"client":"gfx","version":PROTOCOL_VERSION});
      }

      public function suspend() : void
      {
         this._active = false;
         this._connecting = false;
         this._connected = false;
         this.setReadyState(false);
         this.releaseSocket();
      }

      public function resume() : void
      {
         this._active = true;
         this._nextPoll = 0;
      }

      public function connect() : void
      {
         if(this._connecting || this._connected) return;
         this.releaseSocket();
         this._connecting = true;
         this._socket = new Socket();
         // Do not assign Socket.timeout: the game runtime may expose a sealed Socket
         // without this property, causing ReferenceError #1056 during menu startup.
         this._socket.addEventListener(Event.CONNECT,this.connectedHandler,false,0,true);
         this._socket.addEventListener(Event.CLOSE,this.closeHandler,false,0,true);
         this._socket.addEventListener(ProgressEvent.SOCKET_DATA,this.socketDataHandler,false,0,true);
         this._socket.addEventListener(IOErrorEvent.IO_ERROR,this.ioErrorHandler,false,0,true);
         this._socket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.securityErrorHandler,false,0,true);
         try { this._socket.connect(HOST,PORT); }
         catch(error:Error) { this.failConnection("Unable to connect to the launcher"); }
      }

      private function setReadyState(ready:Boolean) : void
      {
         if(ready) this._lastReady = getTimer();
         if(this._launcherReady == ready) return;
         this._launcherReady = ready;
         if(this.onReadiness != null) this.onReadiness(ready);
      }

      private function releaseSocket() : void
      {
         if(this._socket == null) return;
         this._socket.removeEventListener(Event.CONNECT,this.connectedHandler);
         this._socket.removeEventListener(Event.CLOSE,this.closeHandler);
         this._socket.removeEventListener(ProgressEvent.SOCKET_DATA,this.socketDataHandler);
         this._socket.removeEventListener(IOErrorEvent.IO_ERROR,this.ioErrorHandler);
         this._socket.removeEventListener(SecurityErrorEvent.SECURITY_ERROR,this.securityErrorHandler);
         try { this._socket.close(); } catch(error:Error) { }
         this._socket = null;
         this._buffer = "";
      }

      private function failConnection(message:String) : void
      {
         this._connected = false;
         this._connecting = false;
         this.setReadyState(false);
         this.releaseSocket();
         this._nextPoll = getTimer() + 2000;
         if(this._lastConnectionError != message) this.invokeError(message);
         this._lastConnectionError = message;
      }

      public function createLobby(mode:String, maxPlayers:int, visibility:String, mapId:String, mapName:String, alienEnabled:Boolean = true, alienVsSurvivors:Boolean = false, campaignAction:String = "", campaignSaveId:String = "", campaignDifficulty:String = "medium") : void
      {
         var values:Object = {};
         values["mode"] = mode;
         values["maxPlayers"] = String(maxPlayers);
         values["visibility"] = visibility;
         values["mapId"] = mapId;
         values["mapName"] = mapName;
         values["alienEnabled"] = alienEnabled ? "1" : "0";
         values["alienVsSurvivors"] = alienVsSurvivors ? "1" : "0";
         if(mode == "campaign")
         {
            values["campaignAction"] = campaignAction;
            values["campaignSaveId"] = campaignSaveId;
            values["campaignDifficulty"] = campaignDifficulty;
         }
         this.send("CREATE_LOBBY",values);
      }
      
      public function refreshServers(filter:String) : void
      {
         var values:Object = {};
         values["filter"] = filter;
         this.send("REFRESH_SERVERS",values);
      }
      
      public function joinServer(serverId:String) : void
      {
         var values:Object = {};
         values["serverId"] = serverId;
         this.send("JOIN_SERVER",values);
      }
      
      public function joinDirectIp() : void
      {
         this.send("JOIN_DIRECT_IP",{});
      }

      public function refreshFriends() : void
      {
         this.send("REFRESH_FRIENDS",{});
      }

      public function inviteFriend(friendId:String) : void
      {
         this.send("INVITE_FRIEND",{"friendId":friendId});
      }

      public function joinFriend(friendId:String) : void
      {
         this.send("JOIN_FRIEND",{"friendId":friendId});
      }

      public function acceptInvite(from:String) : void
      {
         this.send("ACCEPT_INVITE",{"from":from});
      }
      
      public function setAlienRole(peerId:String) : void
      {
         var values:Object = {};
         values["peerId"] = peerId;
         this.send("SET_ALIEN_ROLE",values);
      }

      public function setReady(ready:Boolean) : void
      {
         var values:Object = {};
         values["ready"] = ready ? "1" : "0";
         this.send("READY",values);
      }
      
      public function kickPlayer(slotOrClientId:String) : void
      {
         var values:Object = {};
         values["target"] = slotOrClientId;
         this.send("KICK",values);
      }
      
      public function startGame(mode:String, mapId:String, mapName:String) : void
      {
         var values:Object = {};
         values["mode"] = mode;
         values["mapId"] = mapId;
         values["mapName"] = mapName;
         this.send("START_GAME",values);
      }
      
      public function ackStartGame(sessionId:String, status:String) : void
      {
         var values:Object = {};
         values["sessionId"] = sessionId;
         values["status"] = status;
         this.send("START_GAME_ACK",values);
      }
      
      public function selectCharacter(sessionId:String, characterId:String, characterName:String, modelHint:String, source:String, confirmed:Boolean) : void
      {
         var values:Object = {};
         values["sessionId"] = sessionId;
         values["characterId"] = characterId;
         values["characterName"] = characterName;
         values["modelHint"] = modelHint;
         values["source"] = source;
         values["confirmed"] = confirmed ? "1" : "0";
         this.send("CHARACTER_SELECTED",values);
      }
      
      public function leaveLobby() : void
      {
         this.send("LEAVE",{});
      }
      
      private function connectedHandler(event:Event) : void
      {
         this._lastConnectionError = "";
         this._connecting = false;
         this._connected = true;
         this.invokeStatus("Waiting for the launcher, please wait...");
         this.send("HELLO",{
            "client":"gfx",
            "version":PROTOCOL_VERSION
         });
         this._nextPoll = getTimer() + 2000;
      }
      
      private function closeHandler(event:Event) : void
      {
         this.failConnection("Launcher disconnected");
      }
      
      private function ioErrorHandler(event:IOErrorEvent) : void
      {
         this.failConnection("Unable to reach the launcher");
      }
      
      private function securityErrorHandler(event:SecurityErrorEvent) : void
      {
         this.failConnection("Launcher connection was blocked");
      }
      
      private function socketDataHandler(event:ProgressEvent) : void
      {
         var text:String = this._socket.readUTFBytes(this._socket.bytesAvailable);
         this._buffer += text;
         var index:int = this._buffer.indexOf("\n");
         while(index >= 0)
         {
            var line:String = this._buffer.substr(0,index);
            this._buffer = this._buffer.substr(index + 1);
            this.handleLine(line);
            index = this._buffer.indexOf("\n");
         }
      }
      
      private function handleLine(line:String) : void
      {
         line = trim(line);
         if(line.length == 0)
         {
            return;
         }
         var parts:Array = line.split("|");
         var type:String = String(parts[0]);
         var values:Object = {};
         var i:int = 1;
         while(i < parts.length)
         {
            var part:String = String(parts[i]);
            var eq:int = part.indexOf("=");
            if(eq > 0)
            {
               values[part.substr(0,eq)] = decodeValue(part.substr(eq + 1));
            }
            i++;
         }
         if(type == "VOICE_SETTINGS")
         {
            if(this.onVoiceSettings != null) this.onVoiceSettings(values);
         }
         else if(type == "FRONTEND_STATE")
         {
            this.setReadyState(values["ready"] == "1");
         }
         else if(type == "SESSION_CLOSED")
         {
            if(this.onSessionClosed != null) this.onSessionClosed(String(values["reason"]));
         }
         else if(type == "STATUS")
         {
            this.invokeStatus(String(values["message"]));
         }
         else if(type == "ERROR")
         {
            this.invokeError(String(values["message"]));
         }
         else if(type == "SERVER_LIST")
         {
            this.handleServerList(values);
         }
         else if(type == "FRIEND_LIST")
         {
            this.handleFriendList(values);
         }
         else if(type == "INVITE_RECEIVED")
         {
            if(this.onInviteReceived != null) this.onInviteReceived(String(values["from"]),String(values["name"]));
         }
         else if(type == "LOBBY_STATE")
         {
            if(this.onLobbyState != null)
            {
               this.onLobbyState(values);
            }
         }
         else if(type == "START_GAME")
         {
            if(this.onGameStart != null)
            {
               this.onGameStart(String(values["mode"]),String(values["mapId"]),String(values["mapName"]),String(values["sessionId"]),String(values["engineLevelPath"]),String(values["campaignAction"]),String(values["campaignDifficulty"]));
            }
         }
         else if(type == "CONFIG")
         {
            if(this.onConfig != null)
            {
               this.onConfig(values);
            }
         }
         else if(type == "LOBBY_OPENED")
         {
            if(this.onLobbyOpened != null) this.onLobbyOpened(String(values["role"]));
         }
         else if(type == "LOBBY_RETURN")
         {
            if(this.onLobbyReturn != null)
            {
               this.onLobbyReturn(String(values["role"]));
            }
         }
      }
      
      private function handleServerList(values:Object) : void
      {
         var entries:Array = [];
         var count:int = int(values["count"]);
         var i:int = 0;
         while(i < count)
         {
            var entry:Object = {};
            entry.id = String(values["s" + i + "_id"]);
            entry.name = String(values["s" + i + "_name"]);
            entry.mode = String(values["s" + i + "_mode"]);
            entry.players = String(values["s" + i + "_players"]);
            entry.maxPlayers = String(values["s" + i + "_maxPlayers"]);
            entry.ping = String(values["s" + i + "_ping"]);
            entry.locked = String(values["s" + i + "_locked"]);
            entry.mapId = String(values["s" + i + "_mapId"]);
            entry.mapName = String(values["s" + i + "_mapName"]);
            entries.push(entry);
            i++;
         }
         if(this.onServerList != null)
         {
            this.onServerList(entries);
         }
      }
      
      private function handleFriendList(values:Object) : void
      {
         var friends:Array = [];
         var invites:Array = [];
         var count:int = int(values["count"]);
         var i:int = 0;
         while(i < count)
         {
            var friend:Object = {};
            friend.id = String(values["f" + i + "_id"]);
            friend.name = String(values["f" + i + "_name"]);
            friend.status = String(values["f" + i + "_status"]);
            friend.aimp = String(values["f" + i + "_aimp"]);
            friend.join = String(values["f" + i + "_join"]);
            friends.push(friend);
            i++;
         }
         var inviteCount:int = int(values["icount"]);
         i = 0;
         while(i < inviteCount)
         {
            var invite:Object = {};
            invite.from = String(values["i" + i + "_from"]);
            invite.name = String(values["i" + i + "_name"]);
            invites.push(invite);
            i++;
         }
         if(this.onFriendList != null)
         {
            this.onFriendList(friends,invites);
         }
      }

      private function send(type:String, values:Object) : void
      {
         var line:String = type;
         for(var key in values)
         {
            line += "|" + key + "=" + encodeValue(String(values[key]));
         }
         line += "\n";
         if(!this._connected || this._socket == null || !this._socket.connected)
         {
            this.invokeError("Wait for the launcher to be ready before trying again");
            return;
         }
         try
         {
            this._socket.writeUTFBytes(line);
            this._socket.flush();
         }
         catch(error:Error) { this.failConnection("Launcher connection was lost"); }
      }

      private function invokeStatus(message:String) : void
      {
         if(this.onStatus != null)
         {
            this.onStatus(message);
         }
      }
      
      private function invokeError(message:String) : void
      {
         if(this.onError != null)
         {
            this.onError(message);
         }
         else
         {
            this.invokeStatus(message);
         }
      }
   }
}
