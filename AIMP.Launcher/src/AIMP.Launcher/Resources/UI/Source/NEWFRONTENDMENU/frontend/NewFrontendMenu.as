package frontend
{
   import components.TextFieldWrapper;
   import constants.KeyCodes;
   import dlc.CharacterSelection;
   import dlc.MapSelection;
   import dlc.components.SelectionScreenParent;
   import dlc.utils.CharacterData;
   import dlc.utils.ChecklistItemData;
   import dlc.utils.PlaylistData;
   import dlc.utils.PlaylistDetailsData;
   import events.MovieClipExEvent;
   import flash.display.MovieClip;
   import flash.events.Event;
   import flash.events.KeyboardEvent;
   import flash.external.ExternalInterface;
   import frontend.enum.MenuItemId;
   import frontend.enum.MenuPageId;
   import frontend.enum.NewFrontendMenuState;
   import frontend.events.MenuEvent;
   import frontend.events.MenuItemEvent;
   import frontend.events.MenuPageEvent;
   import frontend.menu.MenuItem;
   import frontend.menu.MenuItemExpandable;
   import frontend.menu.MenuItemOptionCheckBox;
   import frontend.menu.MenuItemOptionOptionStepper;
   import frontend.menu.MenuPage;
   import scaleform.clik.constants.InputValue;
   import scaleform.clik.events.InputEvent;
   import scaleform.clik.ui.InputDetails;
   import scaleform.gfx.Extensions;
   import utils.DisplayUtil;
   
   public class NewFrontendMenu extends GameMenu
   {
       
      
      public var splashText:TextFieldWrapper;
      
      public var logoAlien:MovieClip;
      
      public var logoIsolation:MovieClip;
      
      public var leaderboard_loading_spinner_mc:MovieClip;
      
      public var mapSelection:MapSelection;
      
      public var characterSelection:CharacterSelection;
      
      public var mapSelectionParent:SelectionScreenParent;
      
      public var characterSelectionParent:SelectionScreenParent;
      
      public var network_notification_mc:MovieClip;
      
      private var _state:NewFrontendMenuState;
      
      private var _mpBridge:MultiplayerBridge;

      private var _mpLauncherReady:Boolean = false;
      private var _mpPendingCommand:String = "";
      private var _mpLobbyRole:String = "";
      private var _mpCanStart:Boolean = false;
      private var _mpLocalReady:Boolean = false;
      private var _mpResumeLobbyItem:MenuItem;
      
      private var _playGameItem:MenuItemExpandable;
      
      private var _extrasItem:MenuItemExpandable;
      
      private var _storeItem:MenuItem;
      
      private var _selectStorageDeviceItem:MenuItem;
      
      private var _campaignItem:MenuItemExpandable;
      
      private var _twoTeamsItem:MenuItemExpandable;
      
      private var _ripleyItem:MenuItemExpandable;
      
      private var _survivorItem:MenuItem;
      
      private var _startGameItem:MenuItemExpandable;
      
      private var _startOnlineGameItem:MenuItemExpandable;
      
      private var _newGamePlusItem:MenuItem;
      
      private var _continueGameItem:MenuItem;
      
      private var _continueOnlineGameItem:MenuItem;
      
      private var _selectLevelItem:MenuItemExpandable;
      
      private var _multiplayerItem:MenuItemExpandable;
      
      private var _createServerItem:MenuItemExpandable;
      
      private var _onlineCampaignItem:MenuItemExpandable;
      
      private var _mementoBtnKELCONKE:MenuItemExpandable;
      
      private var _onlineSurvivorItem:MenuItem;
      
      private var _joinServerItem:MenuItemExpandable;
      
      private var _directIpJoinItem:MenuItemExpandable;
      
      private var _serverNameItem:MenuItem;
      
      private var _serverPasswordItem:MenuItem;
      
      private var _serverNameValue:String = "Alien Isolation MP Test";
      
      private var _serverPasswordValue:String = "";
      
      private var _directJoinHostIpValue:String = "127.0.0.1";
      
      private var _directJoinPortValue:String = "28015";
      
      private var _serverModeItem:MenuItemOptionOptionStepper;
      
      private var _serverMapItem:MenuItemOptionOptionStepper;
      private var _serverAlienItem:MenuItemOptionOptionStepper;

      private var _serverPvPItem:MenuItemOptionOptionStepper;

      private var _mpAlienVsSurvivors:Boolean = false;
      
      private var _survivorMapIds:Array;
      
      private var _survivorMapLabels:Array;
      
      private var _survivorMapDescriptions:Array;
      
      private var _serverMaxPlayersItem:MenuItemOptionOptionStepper;
      
      private var _serverVisibilityItem:MenuItemOptionOptionStepper;

      private var _mpCreatedVisibility:String = "";
      
      private var _serverStartItem:MenuItemExpandable;
      
      private var _serverRefreshItem:MenuItem;
      
      private var _serverFilterItem:MenuItemOptionOptionStepper;
      
      private var _serverLanItem:MenuItemExpandable;
      
      private var _serverCampaignItem:MenuItemExpandable;
      
      private var _serverSurvivalItem:MenuItemExpandable;

      // Steam lobbies: server browser rows (the first three are the original rows).
      private static const SERVER_ROW_COUNT:int = 8;
      private static const INVITE_ROW_COUNT:int = 6;
      private static const FRIEND_ROW_COUNT:int = 24;
      private var _serverRows:Array;
      private var _friendsItem:MenuItemExpandable;
      private var _friendsRefreshItem:MenuItem;
      private var _friendsEmptyItem:MenuItem;
      private var _inviteRows:Array;
      private var _friendRows:Array;
      private var _friendActionsItem:MenuItemExpandable;
      private var _hostInviteItem:MenuItemExpandable;
      private var _clientInviteItem:MenuItemExpandable;
      private var _hostInviteRows:Array;
      private var _clientInviteRows:Array;
      private var _friendJoinItem:MenuItemExpandable;
      private var _mpSelectedFriendId:String = "";
      private var _mpSelectedFriendName:String = "";
      private var _mpSelectedFriendCanJoin:Boolean = false;
      
      private var _hostLobbyStatusItem:MenuItem;
      
      private var _hostLobbyPlayer1Item:MenuItem;
      
      private var _hostLobbyPlayer2Item:MenuItem;
      
      private var _hostLobbyPlayer3Item:MenuItem;
      
      private var _hostLobbyPlayer4Item:MenuItem;
      
      private var _hostLobbyKick2Item:MenuItem;
      
      private var _hostLobbyKick3Item:MenuItem;
      
      private var _hostLobbyKick4Item:MenuItem;
      
      private var _hostLobbyStartGameItem:MenuItem;
      
      private var _hostLobbyLeaveItem:MenuItem;
      
      private var _clientLobbyStatusItem:MenuItem;
      
      private var _clientLobbyPlayer1Item:MenuItem;
      
      private var _clientLobbyPlayer2Item:MenuItem;

      private var _clientLobbyPlayer3Item:MenuItem;

      private var _clientLobbyPlayer4Item:MenuItem;
      
      private var _clientLobbyReadyItem:MenuItemOptionCheckBox;
      
      private var _clientLobbyLeaveItem:MenuItem;
      
      private var _twoTeamsNoviceDifficultyItem:MenuItem;
      
      private var _twoTeamsEasyDifficultyItem:MenuItem;
      
      private var _twoTeamsMediumDifficultyItem:MenuItem;
      
      private var _twoTeamsHardDifficultyItem:MenuItem;
      
      private var _twoTeamsIronDifficultyItem:MenuItem;
      
      private var _ripleyNoviceDifficultyItem:MenuItem;
      
      private var _ripleyEasyDifficultyItem:MenuItem;
      
      private var _ripleyMediumDifficultyItem:MenuItem;
      
      private var _ripleyHardDifficultyItem:MenuItem;
      
      private var _ripleyIronDifficultyItem:MenuItem;
      
      private var _noviceDifficultyItem:MenuItem;
      
      private var _easyDifficultyItem:MenuItem;
      
      private var _mediumDifficultyItem:MenuItem;
      
      private var _hardDifficultyItem:MenuItem;
      
      private var _ironDifficultyItem:MenuItem;
      
      private var _headTrackingOnItem:MenuItemExpandable;
      
      private var _headTrackingOffItem:MenuItemExpandable;
      
      private var _movementDetectionOnItem:MenuItemExpandable;
      
      private var _movementDetectionOffItem:MenuItemExpandable;
      
      private var _noiseDetectionOnItem:MenuItemExpandable;
      
      private var _noiseDetectionOffItem:MenuItemExpandable;
      
      private var _mementoMoriOnItem:MenuItemExpandable;
      
      private var _mementoMoriOffItem:MenuItemExpandable;
      
      private var _creditsItem:MenuItem;
      
      private var _imageGalleryItem:MenuItem;
      
      private var _devDiariesItem:MenuItemExpandable;
      
      private var _achievementsItem:MenuItem;
      
      private var _makingOfItem:MenuItem;
      
      private var _deletedScenesItem:MenuItem;
      
      private var _trailerItem:MenuItem;
      
      private var _playlistData:PlaylistData;
      
      private var _playlistDetailsData:PlaylistDetailsData;
      
      private var _checklistItemData:ChecklistItemData;
      
      private var _characterData:CharacterData;
      
      private var _mpPendingStartGame:Boolean = false;
      
      private var _mpPendingStartMode:String = "";
      
      private var _mpPendingStartMapId:String = "";
      
      private var _mpPendingStartMapName:String = "";
      
      private var _mpPendingSessionId:String = "";
      
      private var _mpLobbyReturnRole:String = null;
      
      private var _mpLobbyReturnFrames:int = 0;
      
      private var _mpCharacterOptions:Array = null;
      
      private var _mpLastCapturedCharacterId:String = "";
      
      private var _mpLastCapturedCharacterIndex:int = -999;
      
      private var framecounter2:int = 0;
      
      private var _leaderboard_spinner_visible:Boolean;
      
      private var _campaignLevelIds:Array = [];
      private var _campaignSaveItem:MenuItemOptionOptionStepper;
      private var _campaignChoices:Array = [{action:"new",id:"",label:"New campaign"}];
      private var _developerModeAvailable:Boolean = false;
      private var _developerMode:Boolean = false;
      private var _developerItem:MenuItem;
      private var _voicePage:MenuItemExpandable;
      private var _voiceEnabledItem:MenuItem;
      private var _voiceModeItem:MenuItem;
      private var _voiceMicItem:MenuItem;
      private var _voiceKeyItem:MenuItem;
      private var _voiceThresholdItem:MenuItem;
      private var _voiceGainItem:MenuItem;
      private var _voiceVolumeItem:MenuItem;
      private var _voiceThreshold:Number = 0.02;
      private var _voiceGain:Number = 1;
      private var _voiceVolume:Number = 1;
      private var _voiceReady:Boolean = false;
      private var _voiceEnabled:Boolean = true;
      private var _voiceMode:String = "pushToTalk";
      private var _voiceMic:String = "default";
      private var _voiceKey:int = 86;
      private var _voiceDevices:Array = [];
      private var _voiceBinding:Boolean = false;

      private function voiceInitialize() : void
      {
         this._voicePage = new MenuItemExpandable("Multiplayer",this.voiceOpened);
         this._voiceEnabledItem = new MenuItem("Loading voice settings...",this.voiceToggle);
         this._voiceModeItem = new MenuItem("Transmission mode",this.voiceModeChanged);
         this._voiceMicItem = new MenuItem("Microphone",this.voiceMicChanged);
         this._voiceKeyItem = new MenuItem("Push-to-talk key",this.voiceBind);
         this._voiceThresholdItem = new MenuItem("Voice activation threshold",this.voiceThresholdChanged);
         this._voiceGainItem = new MenuItem("Microphone gain",this.voiceGainChanged);
         this._voiceVolumeItem = new MenuItem("Voice volume",this.voiceVolumeChanged);
         this._developerItem = new MenuItem("Developer mode: Off",this.developerToggle);
         this._developerItem.visible = false;
         this._developerItem.description = "Unlocks the Campaign game mode and level selection. Campaign levels are prepared after restarting the game.";
         this._voicePage.page.addItem(this._developerItem);
         this._voicePage.page.addItem(this._voiceEnabledItem);
         this._voicePage.page.addItem(this._voiceModeItem);
         this._voicePage.page.addItem(this._voiceMicItem);
         this._voicePage.page.addItem(this._voiceKeyItem);
         this._voicePage.page.addItem(this._voiceThresholdItem);
         this._voicePage.page.addItem(this._voiceGainItem);
         this._voicePage.page.addItem(this._voiceVolumeItem);
         this._voicePage.page.addItem(new MenuItem("Refresh microphones",this.voiceRefresh));
         _optionsItem.page.addItem(this._voicePage);
      }

      private function developerToggle(event:MenuItemEvent) : void
      {
         if(!this._voiceReady || !this._developerModeAvailable) return;
         this.voiceSend("DEVELOPER_MODE",{enabled:this._developerMode ? "0" : "1"});
      }

      private function voiceOpened(event:MenuPageEvent) : void
      {
         this.voiceSend("VOICE_GET_SETTINGS",{});
      }

      private function voiceRefresh(event:MenuItemEvent) : void
      {
         this.voiceSend("VOICE_GET_SETTINGS",{});
      }

      private function voiceSettings(values:Object) : void
      {
         this._voiceReady = true;
         this._developerModeAvailable = values["developerModeAvailable"] == "1";
         this._developerItem.visible = this._developerModeAvailable;
         this._developerMode = this._developerModeAvailable && values["developerMode"] == "1";
         this._developerItem.name = "Developer mode: " + (this._developerMode ? "On" : "Off");
         this._voiceEnabled = values["enabled"] == "1";
         this._voiceMode = String(values["mode"]);
         this._voiceMic = String(values["microphone"]);
         this._voiceKey = int(values["key"]);
         this._voiceThreshold = Number(values["threshold"]);
         this._voiceGain = Number(values["gain"]);
         this._voiceVolume = Number(values["volume"]);
         this._voiceDevices = [];
         for(var i:int = 0; i < Math.min(64,int(values["count"])); i++)
            this._voiceDevices.push({id:String(values["device" + i + "Id"]),name:String(values["device" + i + "Name"])});
         this.voiceLabels();
      }

      private function voiceLabels() : void
      {
         if(this._voiceEnabledItem == null) return;
         this._voiceEnabledItem.name = "Voice chat: " + (this._voiceEnabled ? "On" : "Off");
         this._voiceModeItem.name = "Transmission: " + (this._voiceMode == "voiceActivity" ? "Voice activity" : "Push to talk");
         var name:String = "Unavailable (select another)";
         for each(var device:Object in this._voiceDevices)
            if(device.id == this._voiceMic) name = String(device.name);
         this._voiceMicItem.name = "Microphone: " + name;
         this._voiceThresholdItem.name = "Voice activation threshold: " + this.voicePercent(this._voiceThreshold);
         this._voiceGainItem.name = "Microphone gain: " + this.voicePercent(this._voiceGain);
         this._voiceVolumeItem.name = "Voice volume: " + this.voicePercent(this._voiceVolume);
         this._voiceKeyItem.name = this._voiceBinding ? "Press a key (Escape to cancel)..." : "Push-to-talk key: " + this.voiceKeyName(this._voiceKey);
         menu.refresh();
      }

      private function voiceKeyName(key:int) : String
      {
         if(key >= 65 && key <= 90 || key >= 48 && key <= 57) return String.fromCharCode(key);
         if(key >= 112 && key <= 123) return "F" + (key - 111);
         if(key == 32) return "Space";
         if(key == 16) return "Shift";
         if(key == 17) return "Ctrl";
         if(key == 18) return "Alt";
         return "VK " + key;
      }

      private function voiceToggle(event:MenuItemEvent) : void
      {
         if(!this._voiceReady) return;
         this._voiceEnabled = !this._voiceEnabled;
         this.voiceSave();
      }

      private function voicePercent(value:Number) : String
      {
         return Math.round(value * 100) + "%";
      }

      private function voiceNext(steps:Array, current:Number) : Number
      {
         for each(var step:Number in steps)
            if(step > current + 0.0001) return step;
         return Number(steps[0]);
      }

      private function voiceThresholdChanged(event:MenuItemEvent) : void
      {
         if(!this._voiceReady) return;
         this._voiceThreshold = this.voiceNext([0.005,0.01,0.02,0.04,0.08],this._voiceThreshold);
         this.voiceSave();
      }

      private function voiceGainChanged(event:MenuItemEvent) : void
      {
         if(!this._voiceReady) return;
         this._voiceGain = this.voiceNext([0.5,0.75,1,1.5,2,3],this._voiceGain);
         this.voiceSave();
      }

      private function voiceVolumeChanged(event:MenuItemEvent) : void
      {
         if(!this._voiceReady) return;
         this._voiceVolume = this.voiceNext([0,0.25,0.5,0.75,1,1.25,1.5,2,2.5,3],this._voiceVolume);
         this.voiceSave();
      }

      private function voiceModeChanged(event:MenuItemEvent) : void
      {
         if(!this._voiceReady) return;
         this._voiceMode = this._voiceMode == "pushToTalk" ? "voiceActivity" : "pushToTalk";
         this.voiceSave();
      }

      private function voiceMicChanged(event:MenuItemEvent) : void
      {
         if(!this._voiceReady || this._voiceDevices.length == 0) return;
         var index:int = -1;
         for(var i:int = 0; i < this._voiceDevices.length; i++)
            if(this._voiceDevices[i].id == this._voiceMic) index = i;
         this._voiceMic = String(this._voiceDevices[(index + 1) % this._voiceDevices.length].id);
         this.voiceSave();
      }

      private function voiceBind(event:MenuItemEvent) : void
      {
         if(!this._voiceReady || stage == null) return;
         this._voiceBinding = true;
         stage.addEventListener(KeyboardEvent.KEY_DOWN,this.voiceKeyPressed,true,1000,true);
         this.voiceLabels();
      }

      private function voiceKeyPressed(event:KeyboardEvent) : void
      {
         if(!this._voiceBinding) return;
         event.stopImmediatePropagation();
         event.preventDefault();
         this.voiceCancelBinding();
         if(event.keyCode != 27 && event.keyCode > 0 && event.keyCode < 255)
         {
            this._voiceKey = event.keyCode;
            this.voiceSave();
         }
         this.voiceLabels();
      }

      private function voiceCancelBinding() : void
      {
         this._voiceBinding = false;
         if(stage != null) stage.removeEventListener(KeyboardEvent.KEY_DOWN,this.voiceKeyPressed,true);
      }

      private function voiceSave() : void
      {
         this.voiceSend("VOICE_SAVE_SETTINGS",{enabled:this._voiceEnabled ? "1" : "0",mode:this._voiceMode,microphone:this._voiceMic,key:String(this._voiceKey),threshold:String(this._voiceThreshold),gain:String(this._voiceGain),volume:String(this._voiceVolume)});
         this.voiceLabels();
      }

      private function voiceSend(type:String, values:Object) : void
      {
         this._mpBridge.voiceCommand(type,values);
      }

      public function NewFrontendMenu()
      {
         this._state = NewFrontendMenuState.SPLASH;
         this._mpBridge = new MultiplayerBridge();
         this._mpBridge.onVoiceSettings = this.voiceSettings;
         this._mpBridge.onReadiness = this.mpReadinessHandler;
         this._mpBridge.onSessionClosed = this.mpSessionClosedHandler;
         this._mpBridge.onStatus = this.mpBridgeStatusHandler;
         this._mpBridge.onError = this.mpBridgeErrorHandler;
         this._mpBridge.onServerList = this.mpBridgeServerListHandler;
         this._mpBridge.onLobbyState = this.mpBridgeLobbyStateHandler;
         this._mpBridge.onGameStart = this.mpBridgeGameStartHandler;
         this._mpBridge.onConfig = this.mpBridgeConfigHandler;
         this._mpBridge.onLobbyOpened = this.mpBridgeLobbyOpenedHandler;
         this._mpBridge.onLobbyReturn = this.mpBridgeLobbyReturnHandler;
         this._mpBridge.onFriendList = this.mpBridgeFriendListHandler;
         this._mpBridge.onInviteReceived = this.mpBridgeInviteReceivedHandler;
         this._playGameItem = new MenuItemExpandable("AI_UI_FRONTEND_PLAY_GAME");
         this._extrasItem = new MenuItemExpandable("AI_UI_FRONTEND_EXTRAS");
         this._storeItem = new MenuItem("AI_UI_FRONTEND_STORE",this.storeHandler);
         this._selectStorageDeviceItem = new MenuItem("AI_UI_FRONTEND_SELECT_STORAGE_DEVICE",this.selectStorageDeviceHandler);
         this._campaignItem = new MenuItemExpandable("AI_UI_FRONTEND_CAMPAIGN",this.campaignHandler);
         this._twoTeamsItem = new MenuItemExpandable("AI_UI_DLC_NOSTROMO_TWO_TEAMS_TITLE");
         this._ripleyItem = new MenuItemExpandable("AI_UI_DLC_NOSTROMO_RIPLEY_TITLE");
         this._survivorItem = new MenuItem("AI_UI_FRONTEND_SURVIVOR",this.survivorHandler);
         this._createServerItem = new MenuItemExpandable("Create Server");
         this._onlineCampaignItem = new MenuItemExpandable("Campaign");
         this._onlineSurvivorItem = new MenuItem("Survival",this.onlineSurvivor);
         this._joinServerItem = new MenuItemExpandable("Join Server");
         this._joinServerItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinServerBrowserPressedHandler,false,0,true);
         this._directIpJoinItem = new MenuItemExpandable("Join Direct IP: " + this.mpGetDirectJoinAddressLabel());
         this._directIpJoinItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinDirectIpHandler,false,0,true);
         this._serverNameItem = new MenuItem("Server Name: " + this._serverNameValue);
         this._serverPasswordItem = new MenuItem("Password: " + this.mpGetMaskedPasswordLabel());
         this._survivorMapIds = ["survival_auto"];
         this._survivorMapLabels = ["Auto Survivor Map"];
         this._survivorMapDescriptions = ["Uses the first available survivor map until the game playlist data is loaded"];
         this._serverModeItem = new MenuItemOptionOptionStepper("Game Mode",["survival"],["Survival"],this.serverModeChangedHandler);
         this._campaignSaveItem = new MenuItemOptionOptionStepper("Campaign save",[0],["New campaign"],this.campaignSaveChangedHandler);
         this._serverMapItem = new MenuItemOptionOptionStepper("Survivor Map",this._survivorMapIds,this._survivorMapLabels,this.serverMapChangedHandler);
         this._serverPvPItem = new MenuItemOptionOptionStepper("Match Type",["coop","pvp"],["Co-op","Alien vs Survivors"],this.serverPvPChangedHandler);
         this._serverAlienItem = new MenuItemOptionOptionStepper("Alien",["1","0"],["Enabled","Disabled"]);
         this._serverAlienItem.selectedIndex = 0;
         this._serverMaxPlayersItem = new MenuItemOptionOptionStepper("Max Players",[2,3,4],["2 Players","3 Players","4 Players"],this.serverMaxPlayersChangedHandler);
         this._serverVisibilityItem = new MenuItemOptionOptionStepper("Visibility",["private","friends","public"],["Private","Friends Only","Public"],this.serverVisibilityChangedHandler);
         this._serverVisibilityItem.selectedIndex = 0;
         this._serverStartItem = new MenuItemExpandable("Create Lobby");
         this._serverStartItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.startServerHandler,false,0,true);
         this._serverRefreshItem = new MenuItem("Refresh List",this.refreshServersHandler);
         this._serverFilterItem = new MenuItemOptionOptionStepper("Filter",["all","campaign","survival"],["All","Campaign","Survival"],this.serverFilterChangedHandler);
         this._serverLanItem = new MenuItemExpandable("No lobby");
         this._serverLanItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinServerEntryHandler,false,0,true);
         this._serverLanItem.data = "";
         this._serverLanItem.enabled = false;
         this._serverCampaignItem = new MenuItemExpandable("No lobby");
         this._serverCampaignItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinServerEntryHandler,false,0,true);
         this._serverCampaignItem.data = "";
         this._serverCampaignItem.enabled = false;
         this._serverSurvivalItem = new MenuItemExpandable("No lobby");
         this._serverSurvivalItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinServerEntryHandler,false,0,true);
         this._serverSurvivalItem.data = "";
         this._serverSurvivalItem.enabled = false;
         this._serverRows = [this._serverLanItem,this._serverCampaignItem,this._serverSurvivalItem];
         while(this._serverRows.length < SERVER_ROW_COUNT)
         {
            var extraServerRow:MenuItemExpandable = new MenuItemExpandable("No lobby");
            extraServerRow.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinServerEntryHandler,false,0,true);
            extraServerRow.data = "";
            extraServerRow.enabled = false;
            this._serverRows.push(extraServerRow);
         }
         this._friendsItem = new MenuItemExpandable("Friends");
         this._friendsItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.friendsPressedHandler,false,0,true);
         this._friendsRefreshItem = new MenuItem("Refresh Friends",this.refreshFriendsHandler);
         this._friendsEmptyItem = new MenuItem("No friends online");
         this._friendsEmptyItem.enabled = false;
         this._friendActionsItem = new MenuItemExpandable("Friend");
         this._friendActionsItem.visible = false;
         this._hostInviteItem = this.createInviteFriendsItem();
         this._clientInviteItem = this.createInviteFriendsItem();
         this._hostInviteRows = this.createInviteFriendRows();
         this._clientInviteRows = this.createInviteFriendRows();
         this._friendJoinItem = new MenuItemExpandable("Join Their Lobby");
         this._friendJoinItem.addEventListener(MenuItemEvent.ITEM_PRESS,this.joinFriendHandler,false,0,true);
         this._inviteRows = [];
         while(this._inviteRows.length < INVITE_ROW_COUNT)
         {
            var inviteRow:MenuItemExpandable = new MenuItemExpandable("Invite");
            inviteRow.addEventListener(MenuItemEvent.ITEM_PRESS,this.acceptInviteHandler,false,0,true);
            inviteRow.data = "";
            inviteRow.visible = false;
            this._inviteRows.push(inviteRow);
         }
         this._friendRows = [];
         while(this._friendRows.length < FRIEND_ROW_COUNT)
         {
            var friendRow:MenuItemExpandable = new MenuItemExpandable("Friend");
            friendRow.addEventListener(MenuItemEvent.ITEM_PRESS,this.friendRowPressedHandler,false,0,true);
            friendRow.data = null;
            friendRow.visible = false;
            this._friendRows.push(friendRow);
         }
         this._hostLobbyStatusItem = new MenuItem("Status: Waiting for players");
         this._hostLobbyPlayer1Item = new MenuItem("1  Owner    You    Ready",this.mpAssignAlienHandler);
         this._hostLobbyPlayer2Item = new MenuItem("2  Empty",this.mpAssignAlienHandler);
         this._hostLobbyPlayer3Item = new MenuItem("3  Empty",this.mpAssignAlienHandler);
         this._hostLobbyPlayer4Item = new MenuItem("4  Empty",this.mpAssignAlienHandler);
         this._hostLobbyKick2Item = new MenuItem("Kick Player 2",this.kickLobbyPlayerHandler);
         this._hostLobbyKick2Item.data = "slot2";
         this._hostLobbyKick3Item = new MenuItem("Kick Player 3",this.kickLobbyPlayerHandler);
         this._hostLobbyKick3Item.data = "slot3";
         this._hostLobbyKick4Item = new MenuItem("Kick Player 4",this.kickLobbyPlayerHandler);
         this._hostLobbyKick4Item.data = "slot4";
         this._hostLobbyStartGameItem = new MenuItem("Start Game",this.startLobbyGameHandler);
         this._hostLobbyLeaveItem = new MenuItem("Close Lobby",this.leaveLobbyHandler);
         this._clientLobbyStatusItem = new MenuItem("Status: Joined lobby");
         this._clientLobbyPlayer1Item = new MenuItem("1  Owner    Host");
         this._clientLobbyPlayer2Item = new MenuItem("2  You      Waiting");
         this._clientLobbyPlayer3Item = new MenuItem("3  Empty");
         this._clientLobbyPlayer4Item = new MenuItem("4  Empty");
         this._clientLobbyReadyItem = new MenuItemOptionCheckBox("Ready",true,["Ready","Not Ready"],this.readyChangedHandler);
         this._clientLobbyLeaveItem = new MenuItem("Leave Lobby",this.leaveLobbyHandler);
         this._startGameItem = new MenuItemExpandable("AI_UI_FRONTEND_START_GAME");
         this._startOnlineGameItem = new MenuItemExpandable("AI_UI_FRONTEND_START_GAME");
         this._newGamePlusItem = new MenuItem("AI_UI_FRONTEND_NEW_GAME_PLUS");
         this._continueGameItem = new MenuItem("AI_UI_FRONTEND_CONTINUE_GAME",this.continueGameHandler);
         this._continueOnlineGameItem = new MenuItem("AI_UI_FRONTEND_CONTINUE_GAME",this.continueOnlineGameHandler);
         this._selectLevelItem = new MenuItemExpandable("Select Level");
         this._multiplayerItem = new MenuItemExpandable("Multiplayer");
         this._multiplayerItem.enabled = false;
         this._mpResumeLobbyItem = new MenuItem("Return to Lobby",this.mpResumeLobbyHandler);
         this._mpResumeLobbyItem.visible = false;





         this._twoTeamsNoviceDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_NOVICE",this.twoTeamsNoviceDifficultyHandler);
         this._twoTeamsEasyDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EASY",this.twoTeamsEasyDifficultyHandler);
         this._twoTeamsMediumDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_MEDIUM",this.twoTeamsMediumDifficultyHandler);
         this._twoTeamsHardDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_HARD",this.twoTeamsHardDifficultyHandler);
         this._twoTeamsIronDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EXPERT",this.twoTeamsIronDifficultyHandler);
         this._ripleyNoviceDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_NOVICE",this.ripleyNoviceDifficultyHandler);
         this._ripleyEasyDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EASY",this.ripleyEasyDifficultyHandler);
         this._ripleyMediumDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_MEDIUM",this.ripleyMediumDifficultyHandler);
         this._ripleyHardDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_HARD",this.ripleyHardDifficultyHandler);
         this._ripleyIronDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EXPERT",this.ripleyIronDifficultyHandler);
         this._noviceDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_NOVICE",this.noviceDifficultyHandler);
         this._easyDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EASY",this.easyDifficultyHandler);
         this._mediumDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_MEDIUM",this.mediumDifficultyHandler);
         this._hardDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_HARD",this.hardDifficultyHandler);
         this._ironDifficultyItem = new MenuItem("AI_UI_MENU_DIFF_EXPERT",this.ironDifficultyHandler);
         this._headTrackingOnItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_ON",this.headTrackingOnHandler);
         this._headTrackingOffItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_OFF",this.headTrackingOffHandler);
         this._movementDetectionOnItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_ON",this.movementDetectionOnHandler);
         this._movementDetectionOffItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_OFF",this.movementDetectionOffHandler);
         this._noiseDetectionOnItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_ON",this.noiseDetectionOnHandler);
         this._noiseDetectionOffItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_OFF",this.noiseDetectionOffHandler);
         this._mementoMoriOnItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_ON",this.mementoMoriOnHandler);
         this._mementoMoriOffItem = new MenuItemExpandable("AI_UI_MENU_QUALITY_NAME_OFF",this.mementoMoriOffHandler);
         this._creditsItem = new MenuItem("AI_UI_FRONTEND_CREDITS",this.creditsHandler);
         this._imageGalleryItem = new MenuItem("AI_UI_FRONTEND_GALLERY");
         this._devDiariesItem = new MenuItemExpandable("AI_UI_FRONTEND_DEV_DIARIES");
         this._achievementsItem = new MenuItem("AI_UI_FRONTEND_ACHIEVEMENTS");
         this._makingOfItem = new MenuItem("AI_UI_FRONTEND_MAKING_OF");
         this._deletedScenesItem = new MenuItem("AI_UI_FRONTEND_DELETED_SCENES");
         this._trailerItem = new MenuItem("AI_UI_FRONTEND_TRAILER");
         addFrameScript(0,this.frame1,9,this.frame10,19,this.frame20,29,this.frame30,35,this.frame36,45,this.frame46,53,this.frame54);
         super("fe_");
         var _loc1_:MenuPage = new MenuPage();
         _loc1_.id = MenuPageId.ROOT;
         _loc1_.addItem(this._multiplayerItem);
         _loc1_.addItem(_optionsItem);
         this.voiceInitialize();
         _loc1_.addItem(this._extrasItem);
         _loc1_.addItem(this._storeItem);
         _loc1_.addItem(_helpItem);
         _loc1_.addItem(_quitGameItem);
         _optionsItem.page.addItem(this._selectStorageDeviceItem);
         this._playGameItem.page.addItem(this._campaignItem);
         this._playGameItem.page.addItem(this._twoTeamsItem);
         this._playGameItem.page.addItem(this._ripleyItem);
         this._playGameItem.page.addItem(this._survivorItem);
         this._multiplayerItem.page.addItem(this._mpResumeLobbyItem);
         this._multiplayerItem.page.addItem(this._createServerItem);
         this._multiplayerItem.page.addItem(this._joinServerItem);
         this._multiplayerItem.page.addItem(this._friendsItem);
         this._multiplayerItem.page.addItem(this._directIpJoinItem);
         this._createServerItem.page.title = "Create Server";
         this._createServerItem.page.addItem(this._serverNameItem);
         this._createServerItem.page.addItem(this._serverPasswordItem);
         this._createServerItem.page.addItem(this._serverModeItem);
         this._createServerItem.page.addItem(this._campaignSaveItem);
         this._createServerItem.page.addItem(this._serverMapItem);
         this._createServerItem.page.addItem(this._serverPvPItem);
         this._createServerItem.page.addItem(this._serverAlienItem);
         this._createServerItem.page.addItem(this._serverMaxPlayersItem);
         this._createServerItem.page.addItem(this._serverVisibilityItem);
         this._createServerItem.page.addItem(this._serverStartItem);
         this._joinServerItem.page.title = "Server Browser";
         this._joinServerItem.page.addItem(this._serverRefreshItem);
         this._joinServerItem.page.addItem(this._serverFilterItem);
         this._joinServerItem.page.addItem(this._serverLanItem);
         this._joinServerItem.page.addItem(this._serverCampaignItem);
         this._joinServerItem.page.addItem(this._serverSurvivalItem);
         var serverRowIndex:int = 3;
         while(serverRowIndex < this._serverRows.length)
         {
            this._joinServerItem.page.addItem(this._serverRows[serverRowIndex]);
            serverRowIndex++;
         }
         this._serverStartItem.page.title = "Lobby";
         this._serverStartItem.page.addItem(this._hostLobbyStatusItem);
         this._serverStartItem.page.addItem(this._hostLobbyPlayer1Item);
         this._serverStartItem.page.addItem(this._hostLobbyPlayer2Item);
         this._serverStartItem.page.addItem(this._hostLobbyPlayer3Item);
         this._serverStartItem.page.addItem(this._hostLobbyPlayer4Item);
         this._serverStartItem.page.addItem(this._hostLobbyKick2Item);
         this._serverStartItem.page.addItem(this._hostLobbyKick3Item);
         this._serverStartItem.page.addItem(this._hostLobbyKick4Item);
         this._serverStartItem.page.addItem(this._hostLobbyStartGameItem);
         this._serverStartItem.page.addItem(this._hostInviteItem);
         this._serverStartItem.page.addItem(this._hostLobbyLeaveItem);
         this._serverLanItem.page.title = "Lobby";
         this._serverLanItem.page.addItem(this._clientLobbyStatusItem);
         this._serverLanItem.page.addItem(this._clientLobbyPlayer1Item);
         this._serverLanItem.page.addItem(this._clientLobbyPlayer2Item);
         this._serverLanItem.page.addItem(this._clientLobbyPlayer3Item);
         this._serverLanItem.page.addItem(this._clientLobbyPlayer4Item);
         this._serverLanItem.page.addItem(this._clientLobbyReadyItem);
         this._serverLanItem.page.addItem(this._clientInviteItem);
         this._serverLanItem.page.addItem(this._clientLobbyLeaveItem);
         this._serverCampaignItem.subPage = this._serverLanItem.page;
         this._serverSurvivalItem.subPage = this._serverLanItem.page;
         this._directIpJoinItem.subPage = this._serverLanItem.page;
         this.buildSteamPages();
         this.updateServerMapVisibility();
         this._onlineCampaignItem.page.addItem(this._startOnlineGameItem);
         this._onlineCampaignItem.page.addItem(this._continueOnlineGameItem);
         this._onlineCampaignItem.page.addItem(_loadGameItem);
         this._onlineCampaignItem.page.addItem(this._selectLevelItem);
         this._campaignItem.page.addItem(this._startGameItem);
         this._campaignItem.page.addItem(this._continueGameItem);
         this._campaignItem.page.addItem(_loadGameItem);
         this._twoTeamsItem.page.addItem(this._twoTeamsNoviceDifficultyItem);
         this._twoTeamsItem.page.addItem(this._twoTeamsEasyDifficultyItem);
         this._twoTeamsItem.page.addItem(this._twoTeamsMediumDifficultyItem);
         this._twoTeamsItem.page.addItem(this._twoTeamsHardDifficultyItem);
         this._twoTeamsItem.page.addItem(this._twoTeamsIronDifficultyItem);
         this._twoTeamsItem.page.defaultSelectedIndex = 2;
         this._ripleyItem.page.addItem(this._ripleyNoviceDifficultyItem);
         this._ripleyItem.page.addItem(this._ripleyEasyDifficultyItem);
         this._ripleyItem.page.addItem(this._ripleyMediumDifficultyItem);
         this._ripleyItem.page.addItem(this._ripleyHardDifficultyItem);
         this._ripleyItem.page.addItem(this._ripleyIronDifficultyItem);
         this._ripleyItem.page.defaultSelectedIndex = 2;
         this._startGameItem.page.addItem(this._noviceDifficultyItem);
         this._startGameItem.page.addItem(this._easyDifficultyItem);
         this._startGameItem.page.addItem(this._mediumDifficultyItem);
         this._startGameItem.page.addItem(this._hardDifficultyItem);
         this._startGameItem.page.addItem(this._ironDifficultyItem);
         this._startOnlineGameItem.page.addItem(this._noviceDifficultyItem);
         this._startOnlineGameItem.page.addItem(this._easyDifficultyItem);
         this._startOnlineGameItem.page.addItem(this._mediumDifficultyItem);
         this._startOnlineGameItem.page.addItem(this._hardDifficultyItem);
         this._startOnlineGameItem.page.addItem(this._ironDifficultyItem);
         this._extrasItem.page.addItem(this._creditsItem);
         this._devDiariesItem.page.addItem(this._makingOfItem);
         this._devDiariesItem.page.addItem(this._deletedScenesItem);
         this._devDiariesItem.page.addItem(this._trailerItem);
         extras.backButtonCallback = this.backButtonCallback;
         menu.extrasTopLevel = true;
         menu.additionalDescription = description;
         menu.additionalTransitions = transitions;
         menu.extras = extras;
         menu.moviePlayer = moviePlayer;
         menu.rootPage = _loc1_;
         this.mapSelection = this.mapSelectionParent.selectionScreen as MapSelection;
         this.characterSelection = this.characterSelectionParent.selectionScreen as CharacterSelection;
         this.mapSelection.menu = menu;
         this.mapSelection.leaderboardLoadedCallback = this.leaderboardLoadedCallback;
         this.mapSelection.leaderboard_loading_spinner_mc = this.leaderboard_loading_spinner_mc;
         menu.addEventListener(MenuEvent.INACTIVITY,this.menuInactivityHandler);
         addEventListener(MovieClipExEvent.STOP,this.movieStopHandler);
         addEventListener(Event.ENTER_FRAME,this.mpCharacterSelectionFrameHandler,false,0,true);
         addEventListener(Event.ENTER_FRAME,this.mpNetworkFrameHandler,false,0,true);
         addEventListener(Event.ADDED_TO_STAGE,this.mpNetworkAddedHandler,false,0,true);
         addEventListener(Event.REMOVED_FROM_STAGE,this.mpNetworkRemovedHandler,false,0,true);
         if(Extensions.isGFxPlayer)
         {
            stage.addEventListener(InputEvent.INPUT,this.startInputHandler,false,0,true);
         }
         this.__setProp_description_Scene1_description_0();
         this.__setProp_userProfilePanel_Scene1_profile_0();
      }
      
      public static function requestState(param1:NewFrontendMenuState) : void
      {
         if(param1 != null)
         {
            ExternalInterface.call("fe_set_state",param1.id);
         }
      }
      
      public function pressed_leaderboards() : *
      {
      }
      
      public function pressed_view_player_info() : *
      {
      }
      
      public function pressed_changed_filter() : *
      {
      }
      
      public function transition_in_spinner() : *
      {
         trace("transition_in_spinner called");
         if(this.leaderboard_loading_spinner_mc)
         {
            this._leaderboard_spinner_visible = true;
            this.leaderboard_loading_spinner_mc.gotoAndPlay("trans_in");
         }
      }
      
      public function transition_out_spinner() : *
      {
         trace("transition_out_spinner called");
         if(this.leaderboard_loading_spinner_mc && this._leaderboard_spinner_visible)
         {
            this._leaderboard_spinner_visible = false;
            this.leaderboard_loading_spinner_mc.gotoAndPlay("trans_out");
         }
      }
      
      override public function init_image_substitution(param1:String, param2:Boolean, param3:Boolean) : void
      {
         trace("New image substitution for NewFrontendMenu");
         if(param2)
         {
            this.mapSelection.leaderboardTutorialLabel.text = "Select challenge             ";
            this.mapSelection.set_is_using_keyboard(true);
            this.characterSelection.set_is_using_keyboard(true);
            this.mapSelection.mapSelectTutorialLabel.text = "Challenge selection           ";
            this.mapSelection.mapSelectTutorialNoLeaderboardLabel.text = "No leaderboard data                         ";
            this.characterSelection.characterSelectionTutorialLabel.text = "AI_UI_TIP_CHARACTER_SELECTION_KB";
            this.splashText.text = "~AI_UI_FRONTEND_PRESS_TO_START_KB";
         }
         else
         {
            this.mapSelection.leaderboardTutorialLabel.text = "Select challenge        ";
            this.mapSelection.mapSelectTutorialLabel.text = "Challenge selection      ";
            this.mapSelection.mapSelectTutorialNoLeaderboardLabel.text = "No leaderboard data                   ";
            this.characterSelection.characterSelectionTutorialLabel.text = "AI_UI_TIP_CHARACTER_SELECTION";
            this.splashText.text = "~AI_UI_FRONTEND_PRESS_TO_START";
            this.mapSelection.set_is_using_keyboard(false);
            this.characterSelection.set_is_using_keyboard(false);
         }
         super.init_image_substitution(param1,param2,param3);
      }
      
      override protected function imageSubstitutionLoadedCallback() : void
      {
         super.imageSubstitutionLoadedCallback();
         _imageSubstitution.Add_TextField(this.splashText.textField);
         _imageSubstitution.Add_TextField(this.characterSelection.characterSelectionTutorialLabel);
         _imageSubstitution.Add_TextField(this.network_notification_mc.network_notification_sub_mc.txt);
         _imageSubstitution.Add_TextField(this.mapSelection.leaderboardTutorialLabel);
         _imageSubstitution.Add_TextField(this.mapSelection.mapSelectTutorialLabel);
         _imageSubstitution.Add_TextField(this.mapSelection.mapSelectTutorialNoLeaderboardLabel);
         this.mapSelection.applyImageSubstitution(_imageSubstitution);
      }
      
      private function leaderboardLoadedCallback() : void
      {
         this.mapSelection.applyImageSubstitution(_imageSubstitution);
      }
      
      private function backButtonCallback() : Boolean
      {
         return undefined;
      }
      
      public function populate_leaderboard(param1:Array, param2:Number, param3:Number) : void
      {
         this.mapSelection.populateLeaderboard(param1,param2,param3);
      }
      
      public function update_leaderboard_enabled(param1:Boolean, param2:String) : void
      {
         this.mapSelection.update_leaderboard_enabled(param1,param2);
      }
      
      public function set_best_score_and_time(param1:Number, param2:Number) : void
      {
         this.mapSelection.set_best_score_and_time(param1,param2);
      }
      
      public function set_leaderboard_focused(param1:Boolean) : void
      {
         this.mapSelection.set_leaderboard_focused(param1);
      }
      
      public function setLanguage(param1:int) : void
      {
      }
      
      public function addStartGameSettings(param1:int) : void
      {
      }
      
      public function addLevel(param1:String) : void
      {
         var _loc2_:MenuItem = new MenuItem(param1,this.levelSelectionHandler);
         _loc2_.data = param1;
         this._selectLevelItem.page.addItem(_loc2_);
      }
      
      public function addDLC(param1:String, param2:String, param3:String, param4:int, param5:Boolean) : void
      {
      }
      
      public function removeAllPlaylists() : void
      {
         this.mapSelection.removeAllPlaylists();
         this.resetCachedSurvivorMaps();
      }
      
      public function setPackagePlaylists(param1:int, param2:String, param3:Array) : void
      {
         this.mapSelection.setPackagePlaylists(param1,param2,param3);
         this.cacheSurvivorMapArray(param3,"package_" + String(param1));
      }
      
      public function setCustomPlaylists(param1:Array) : void
      {
         this.mapSelection.setCustomPlaylists(param1);
         this.cacheSurvivorMapArray(param1,"custom");
      }
      
      public function setMarathonPlaylist(param1:PlaylistData) : void
      {
         this.mapSelection.setMarathonPlaylist(param1);
         this.cacheSurvivorMapCandidate(param1,"marathon",0);
      }
      
      public function setPlaylistDetails(param1:PlaylistDetailsData) : void
      {
         this.mapSelection.setPlaylistDetails(param1);
      }
      
      public function setCharacterSelection(characters:Array) : void
      {
         this._mpCharacterOptions = characters;
         this._mpLastCapturedCharacterId = "";
         this._mpLastCapturedCharacterIndex = -999;
         this.characterSelection.setCharacters(characters);
         requestState(NewFrontendMenuState.DLC_CHAR);
         this.mpCaptureCurrentCharacterSelection("gfx-character-options",false);
         if(this._mpPendingStartGame && this._mpPendingStartMode == "survival")
         {
            this._mpPendingStartGame = false;
            this.show_network_notification("Select a character");
            this._mpBridge.ackStartGame(this._mpPendingSessionId,"character_selection_opened");
         }
      }
      
      public function setSurvivorModeInfo(param1:String, param2:String) : void
      {
         this._survivorItem.name = param1;
         this._survivorItem.description = param2;
      }
      
      public function restartInactivityTimer() : void
      {
         menu.restartInactivityTimer();
      }
      
      public function setState(param1:int, param2:Boolean) : void
      {
         var _loc4_:NewFrontendMenuState = null;
         var _loc5_:String = null;
         var _loc3_:NewFrontendMenuState = NewFrontendMenuState.getState(param1);
         if(_loc3_ != null)
         {
            _loc4_ = this._state;
            _loc5_ = this._state.getTransition(_loc3_.id);
            this._state = _loc3_;
            if(menu != null)
            {
               if(NewFrontendMenuState.MENU.equals(this._state))
               {
                  menuParent.menuOnStage = true;
               }
               menu.inputEnabled = Boolean(Boolean(Boolean(Boolean(Boolean(Boolean(NewFrontendMenuState.MENU.equals(this._state)))))));
            }
            if(this.mapSelection != null)
            {
               if(NewFrontendMenuState.DLC_MAP.equals(this._state))
               {
                  this.mapSelectionParent.selectionScreenOnStage = true;
               }
               this.mapSelection.initSelectionEnabled = !NewFrontendMenuState.DLC_CHAR.equals(_loc4_);
               this.mapSelection.inputEnabled = Boolean(Boolean(Boolean(Boolean(Boolean(Boolean(NewFrontendMenuState.DLC_MAP.equals(this._state)))))));
            }
            if(this.characterSelection != null)
            {
               if(NewFrontendMenuState.DLC_CHAR.equals(this._state))
               {
                  this.characterSelectionParent.selectionScreenOnStage = true;
               }
               this.characterSelection.initSelectionEnabled = true;
               this.characterSelection.inputEnabled = Boolean(Boolean(Boolean(Boolean(Boolean(Boolean(NewFrontendMenuState.DLC_CHAR.equals(this._state)))))));
            }
            if(param2 && DisplayUtil.frameLabelExists(_loc5_,this))
            {
               playTransition(_loc5_);
            }
            else
            {
               if(!DisplayUtil.frameLabelExists(_loc3_.name,this))
               {
                  throw new Error("No label for NewFrontendMenuState: " + _loc3_.name);
               }
               DisplayUtil.stopOnLabel(_loc3_.name,this);
            }
         }
      }
      
      private function campaignHandler(param1:MenuPageEvent) : void
      {
      }
      
      private function survivorHandler(param1:MenuItemEvent) : void
      {
         ExternalInterface.call("request_map_selection");
      }
      
      private function createServer(param1:MenuItemEvent) : void
      {
      }
      
      private function onlineSurvivor(param1:MenuItemEvent) : void
      {
         ExternalInterface.call("request_map_selection");
      }
      
      private function joinServer(param1:MenuItemEvent) : void
      {
      }
      
      private function serverPvPChangedHandler(param1:MenuItemEvent) : void
      {
         this.updateServerMapVisibility();
         this.refreshNetworkMenuRenderers();
      }

      private function serverModeChangedHandler(param1:MenuItemEvent) : void
      {
         this.refreshSurvivorMapOptions();
      }

      // Campaign is a Developer mode game mode. Without it, Survival (Co-op or
      // Alien vs Survivors through Match Type) is the only choice.
      private function refreshServerModeOptions() : void
      {
         var campaign:Boolean = this._developerMode && this._serverModeItem.selectedItem != null && String(this._serverModeItem.selectedItem) == "campaign";
         if(this._developerMode)
         {
            this._serverModeItem.setItems(["survival","campaign"],["Survival","Campaign"]);
            this._serverModeItem.descriptions = ["Create a survival server","Start a new campaign or continue a host save"];
         }
         else
         {
            this._serverModeItem.setItems(["survival"],["Survival"]);
            this._serverModeItem.descriptions = ["Create a survival server"];
         }
         this._serverModeItem.selectedIndex = campaign ? 1 : 0;
      }
      
      private function campaignSaveChangedHandler(event:MenuItemEvent) : void
      {
         this.updateServerMapVisibility();
         this.refreshNetworkMenuRenderers();
      }

      private function getCampaignChoice() : Object
      {
         return this._campaignChoices[this._campaignSaveItem.selectedIndex];
      }

      private function refreshCampaignSaveOptions(values:Object) : void
      {
         var previous:Object = this.getCampaignChoice();
         var choices:Array = [{action:"new",id:"",label:"New campaign"}];
         var ids:Array = [0];
         var labels:Array = ["New campaign"];
         var selected:int = 0;
         for(var i:int = 0; i < int(values["campaignSaveCount"]); i++)
         {
            var id:String = getObjectString(values,"campaignSave" + i + "Id","");
            if(id.length == 0) continue;
            var label:String = getObjectString(values,"campaignSave" + i + "Label","Saved campaign");
            var level:String = getObjectString(values,"campaignSave" + i + "Level","");
            choices.push({action:"continue",id:id,label:label});
            ids.push(choices.length - 1);
            labels.push("Continue: " + label + (level.length > 0 ? " (" + level + ")" : ""));
            if(previous != null && previous.action == "continue" && previous.id == id) selected = choices.length - 1;
         }
         if(this._developerMode && this._campaignLevelIds.length > 0)
         {
            choices.push({action:"level",id:"",label:"Choose level (experimental)"});
            ids.push(choices.length - 1);
            labels.push("Choose level (experimental)");
            if(previous != null && previous.action == "level") selected = choices.length - 1;
         }
         this._campaignChoices = choices;
         this._campaignSaveItem.setItems(ids,labels);
         this._campaignSaveItem.selectedIndex = selected;
      }

      private function serverMapChangedHandler(param1:MenuItemEvent) : void
      {
      }
      
      private function serverMaxPlayersChangedHandler(param1:MenuItemEvent) : void
      {
      }
      
      private function serverVisibilityChangedHandler(param1:MenuItemEvent) : void
      {
         this.updateServerMapVisibility();
         this.refreshNetworkMenuRenderers();
      }
      
      private function serverFilterChangedHandler(param1:MenuItemEvent) : void
      {
         this.applyServerFilter(String(this._serverFilterItem.selectedItem));
         if(this._mpLauncherReady && this._mpLobbyRole == "")
         {
            this._mpBridge.refreshServers(String(this._serverFilterItem.selectedItem));
         }
      }

      // ---- Steam friends page ----

      private function buildSteamPages() : void
      {
         var index:int = 3;
         while(index < this._serverRows.length)
         {
            var serverRow:MenuItemExpandable = this._serverRows[index];
            serverRow.subPage = this._serverLanItem.page;
            serverRow.name = "No lobby";
            index++;
         }
         this._friendsItem.page.title = "Friends";
         this._friendsItem.page.addItem(this._friendsRefreshItem);
         index = 0;
         while(index < this._inviteRows.length)
         {
            var inviteRow:MenuItemExpandable = this._inviteRows[index];
            inviteRow.subPage = this._serverLanItem.page;
            inviteRow.name = "Invite";
            this._friendsItem.page.addItem(inviteRow);
            index++;
         }
         this._friendsItem.page.addItem(this._friendsEmptyItem);
         index = 0;
         while(index < this._friendRows.length)
         {
            var friendRow:MenuItemExpandable = this._friendRows[index];
            friendRow.subPage = this._friendActionsItem.page;
            friendRow.name = "Friend";
            this._friendsItem.page.addItem(friendRow);
            index++;
         }
         // Hidden owner of the shared per-friend page, so Back returns to Friends.
         this._friendsItem.page.addItem(this._friendActionsItem);
         this._friendActionsItem.page.title = "Friend";
         this._friendActionsItem.page.addItem(this._friendJoinItem);
         this.buildInviteFriendsPage(this._hostInviteItem,this._hostInviteRows);
         this.buildInviteFriendsPage(this._clientInviteItem,this._clientInviteRows);
         this._friendJoinItem.subPage = this._serverLanItem.page;
         this._friendJoinItem.name = "Join Their Lobby";
      }

      private function friendsPressedHandler(param1:MenuItemEvent) : void
      {
         this.requestFriendList();
      }

      private function refreshFriendsHandler(param1:MenuItemEvent) : void
      {
         this.requestFriendList();
      }

      private function requestFriendList() : void
      {
         if(!this._mpLauncherReady) return;
         this.show_network_notification("Refreshing friends");
         this._mpBridge.refreshFriends();
      }

      private function mpBridgeFriendListHandler(friends:Array, invites:Array) : void
      {
         var index:int = 0;
         while(index < this._inviteRows.length)
         {
            var inviteRow:MenuItem = this._inviteRows[index];
            if(index < invites.length)
            {
               inviteRow.name = "Accept Invite: " + String(invites[index].name);
               inviteRow.description = "Join the lobby " + String(invites[index].name) + " invited you to";
               inviteRow.data = String(invites[index].from);
               inviteRow.visible = true;
            }
            else
            {
               inviteRow.data = "";
               inviteRow.visible = false;
            }
            index++;
         }
         index = 0;
         while(index < this._friendRows.length)
         {
            var friendRow:MenuItem = this._friendRows[index];
            if(index < friends.length)
            {
               var friend:Object = friends[index];
               friendRow.name = String(friend.name) + " | " + String(friend.status);
               friendRow.description = friend.join == "1" ? "Hosting a lobby - press to join" : "Not hosting a lobby you can join. Invite friends from inside your lobby";
               friendRow.data = friend;
               friendRow.visible = true;
            }
            else
            {
               friendRow.data = null;
               friendRow.visible = false;
            }
            index++;
         }
         this._friendsEmptyItem.visible = friends.length == 0;
         this.fillInviteFriendRows(this._hostInviteRows,friends);
         this.fillInviteFriendRows(this._clientInviteRows,friends);
         this.mpUpdateControls();
      }

      private function mpBridgeInviteReceivedHandler(from:String, fromName:String) : void
      {
         // The launcher also sends a STATUS notification; just refresh the invite rows.
         if(this._mpLauncherReady) this._mpBridge.refreshFriends();
      }

      private function friendRowPressedHandler(param1:MenuItemEvent) : void
      {
         var friend:Object = param1 != null && param1.item != null ? param1.item.data : null;
         if(friend == null) return;
         this._mpSelectedFriendId = String(friend.id);
         this._mpSelectedFriendName = String(friend.name);
         this._mpSelectedFriendCanJoin = friend.join == "1";
         this._friendActionsItem.page.title = this._mpSelectedFriendName;
         this.mpUpdateControls();
      }

      // ---- Invite Friends (inside the host and client lobby pages) ----

      private function createInviteFriendsItem() : MenuItemExpandable
      {
         var item:MenuItemExpandable = new MenuItemExpandable("Invite Friends");
         item.addEventListener(MenuItemEvent.ITEM_PRESS,this.friendsPressedHandler,false,0,true);
         return item;
      }

      private function createInviteFriendRows() : Array
      {
         var rows:Array = [];
         while(rows.length < FRIEND_ROW_COUNT)
         {
            var row:MenuItem = new MenuItem("Friend",this.inviteFriendRowHandler);
            row.data = null;
            row.visible = false;
            rows.push(row);
         }
         return rows;
      }

      private function buildInviteFriendsPage(item:MenuItemExpandable, rows:Array) : void
      {
         item.page.title = "Invite Friends";
         var refresh:MenuItem = new MenuItem("Refresh Friends",this.refreshFriendsHandler);
         refresh.description = "Reload your online Steam friends";
         item.page.addItem(refresh);
         var index:int = 0;
         while(index < rows.length)
         {
            item.page.addItem(rows[index]);
            index++;
         }
      }

      private function fillInviteFriendRows(rows:Array, friends:Array) : void
      {
         var index:int = 0;
         while(index < rows.length)
         {
            var row:MenuItem = rows[index];
            if(index < friends.length)
            {
               row.name = "Invite " + String(friends[index].name) + " | " + String(friends[index].status);
               row.description = "Send " + String(friends[index].name) + " a Steam invite to this lobby";
               row.data = String(friends[index].id);
               row.enabled = true;
               row.visible = true;
            }
            else if(index == 0)
            {
               row.name = "No friends online";
               row.description = "Open Steam and sign in to see your friends";
               row.data = null;
               row.enabled = false;
               row.visible = true;
            }
            else
            {
               row.data = null;
               row.visible = false;
            }
            index++;
         }
      }

      private function inviteFriendRowHandler(param1:MenuItemEvent) : void
      {
         var friendId:String = param1 != null && param1.item != null && param1.item.data != null ? String(param1.item.data) : "";
         if(!this._mpLauncherReady || friendId.length == 0 || this._mpLobbyRole == "") return;
         this.show_network_notification("Sending invite");
         this._mpBridge.inviteFriend(friendId);
      }

      private function joinFriendHandler(param1:MenuItemEvent) : void
      {
         if(this._mpSelectedFriendId == "" || !this.mpBeginCommand("join")) return;
         this.show_network_notification("Joining " + this._mpSelectedFriendName);
         this._mpBridge.joinFriend(this._mpSelectedFriendId);
      }

      private function acceptInviteHandler(param1:MenuItemEvent) : void
      {
         var from:String = param1 != null && param1.item != null && param1.item.data != null ? String(param1.item.data) : "";
         if(from.length == 0 || !this.mpBeginCommand("join")) return;
         this.show_network_notification("Accepting invite");
         this._mpBridge.acceptInvite(from);
      }
      
      private function refreshServersHandler(param1:MenuItemEvent) : void
      {
         this.show_network_notification("Refreshing server list");
         this._mpBridge.refreshServers(String(this._serverFilterItem.selectedItem));
      }
      
      private function joinServerBrowserPressedHandler(param1:MenuItemEvent) : void
      {
         this.show_network_notification("Refreshing server list");
         this._mpBridge.refreshServers(String(this._serverFilterItem.selectedItem));
      }
      
      private function joinServerEntryHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("join")) return;
         var serverId:String = "";
         if(param1 != null && param1.item != null && param1.item.data != null)
         {
            serverId = String(param1.item.data);
         }
         if(serverId == null || serverId.length == 0)
         {
            this._mpPendingCommand = "";
            this.mpUpdateControls();
            this.show_network_notification("No lobby selected");
            return;
         }
         this.show_network_notification("Joining lobby");
         this._mpBridge.joinServer(serverId);
      }
      
      private function joinDirectIpHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("join")) return;
         this.show_network_notification("Joining " + this.mpGetDirectJoinAddressLabel());
         this._mpBridge.joinDirectIp();
      }
      
      private function startServerHandler(param1:MenuItemEvent) : void
      {
         if(this._mpLobbyRole != "" || !this.mpBeginCommand("create")) return;
         this._mpCreatedVisibility = String(this._serverVisibilityItem.selectedItem);
         this.show_network_notification("Creating lobby");
         var pvp:Boolean = this.serverPvPSelected();
         this._mpBridge.createLobby(String(this._serverModeItem.selectedItem),int(this._serverMaxPlayersItem.selectedItem),String(this._serverVisibilityItem.selectedItem),this.getSelectedSurvivorMapId(),this.getSelectedSurvivorMapName(),String(this._serverAlienItem.selectedItem) != "0" || pvp,pvp,String(this.getCampaignChoice().action),String(this.getCampaignChoice().id),"medium");
      }

      private function serverPvPSelected() : Boolean
      {
         return String(this._serverPvPItem.selectedItem) == "pvp" && String(this._serverModeItem.selectedItem) != "campaign";
      }
      
      private function readyChangedHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("ready")) return;
         this._mpBridge.setReady(Boolean(this._clientLobbyReadyItem.selected));
      }
      
      private function mpAssignAlienHandler(param1:MenuItemEvent) : void
      {
         if(!this._mpAlienVsSurvivors || this._mpLobbyRole != "host" || param1 == null || param1.item == null || !this.mpBeginCommand("role")) return;
         var peerId:String = String(param1.item.data);
         if(peerId == "" || peerId == "undefined")
         {
            this._mpPendingCommand = "";
            this.mpUpdateControls();
            return;
         }
         this._mpBridge.setAlienRole(peerId);
      }

      private function kickLobbyPlayerHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("kick")) return;
         this.show_network_notification("Kick requested");
         this._mpBridge.kickPlayer(String(param1.item.data));
      }
      
      private function startLobbyGameHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("start")) return;
         this.show_network_notification("Starting game");
         this._mpBridge.startGame(String(this._serverModeItem.selectedItem),this.getSelectedSurvivorMapId(),this.getSelectedSurvivorMapName());
      }
      
      private function leaveLobbyHandler(param1:MenuItemEvent) : void
      {
         if(!this.mpBeginCommand("leave")) return;
         this.show_network_notification("Leaving lobby");
         this._mpBridge.leaveLobby();
      }
      
      public function mp_update_lobby(param1:String) : void
      {
         this._hostLobbyStatusItem.name = "Status: " + param1;
         this._clientLobbyStatusItem.name = "Status: " + param1;
         this.refreshNetworkMenuRenderers();
      }
      
      public function mp_set_lobby_players(param1:String, param2:String, param3:String, param4:String) : void
      {
         this._hostLobbyPlayer1Item.name = param1;
         this._hostLobbyPlayer2Item.name = param2;
         this._hostLobbyPlayer3Item.name = param3;
         this._hostLobbyPlayer4Item.name = param4;
         this._clientLobbyPlayer1Item.name = param1;
         this._clientLobbyPlayer2Item.name = param2;
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpBridgeConfigHandler(param1:Object) : void
      {
         var _loc2_:String = null;
         var _loc3_:String = null;
         var _loc4_:String = null;
         var _loc5_:String = null;
         if(param1 == null)
         {
            return;
         }
         _loc2_ = String(param1["serverName"]);
         if(_loc2_ != null && _loc2_ != "undefined")
         {
            this._serverNameValue = _loc2_;
         }
         _loc3_ = String(param1["serverPassword"]);
         if(_loc3_ != null && _loc3_ != "undefined")
         {
            this._serverPasswordValue = _loc3_;
         }
         _loc4_ = String(param1["hostIp"]);
         if(_loc4_ != null && _loc4_ != "undefined" && _loc4_.length > 0)
         {
            this._directJoinHostIpValue = _loc4_;
         }
         _loc5_ = String(param1["port"]);
         if(_loc5_ != null && _loc5_ != "undefined" && _loc5_.length > 0)
         {
            this._directJoinPortValue = _loc5_;
         }
         this._developerModeAvailable = param1["developerModeAvailable"] == "1";
         this._developerItem.visible = this._developerModeAvailable;
         this._developerMode = this._developerModeAvailable && param1["developerMode"] == "1";
         this._campaignLevelIds = [];
         for(var i:int = 0; i < int(param1["developerLevelCount"]); i++)
         {
            var level:String = String(param1["developerLevel" + i]);
            if(level.toUpperCase().indexOf("CHALLENGE") != 0 && level.toUpperCase().indexOf("BSPNOSTROMO_") != 0 && level.toUpperCase().indexOf("SALVAGE") != 0)
               this._campaignLevelIds.push(level);
         }
         this.refreshServerModeOptions();
         this.refreshCampaignSaveOptions(param1);
         this.refreshSurvivorMapOptions();
         this.mpRefreshServerConfigItems();
         this.mpUpdateControls();
      }
      
      private function mpRefreshServerConfigItems() : void
      {
         var _loc1_:String = this._serverNameValue;
         if(_loc1_ == null || _loc1_.length == 0)
         {
            _loc1_ = "Alien Isolation MP Test";
         }
         if(this._serverNameItem != null)
         {
            this._serverNameItem.name = "Server Name: " + _loc1_;
         }
         if(this._serverPasswordItem != null)
         {
            this._serverPasswordItem.name = "Password: " + this.mpGetMaskedPasswordLabel();
         }
         if(this._directIpJoinItem != null)
         {
            this._directIpJoinItem.name = "Join Direct IP: " + this.mpGetDirectJoinAddressLabel();
         }
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpGetDirectJoinAddressLabel() : String
      {
         var _loc1_:String = this._directJoinHostIpValue;
         var _loc2_:String = this._directJoinPortValue;
         if(_loc1_ == null || _loc1_.length == 0)
         {
            _loc1_ = "127.0.0.1";
         }
         if(_loc2_ == null || _loc2_.length == 0)
         {
            _loc2_ = "28015";
         }
         return _loc1_ + ":" + _loc2_;
      }
      
      private function mpGetMaskedPasswordLabel() : String
      {
         var _loc1_:int = 0;
         var _loc2_:String = "";
         if(this._serverPasswordValue == null || this._serverPasswordValue.length == 0)
         {
            return "<empty>";
         }
         while(_loc1_ < this._serverPasswordValue.length && _loc1_ < 12)
         {
            _loc2_ += "*";
            _loc1_++;
         }
         if(this._serverPasswordValue.length > 12)
         {
            _loc2_ += "+";
         }
         return _loc2_;
      }
      
      private function mpNetworkFrameHandler(event:Event) : void
      {
         if(stage != null) this._mpBridge.update();
      }

      private function mpNetworkAddedHandler(event:Event) : void
      {
         if(event.target == this) this._mpBridge.resume();
      }

      private function mpNetworkRemovedHandler(event:Event) : void
      {
         if(event.target == this) { this.voiceCancelBinding(); this._voiceReady = false; this._mpBridge.suspend(); }
      }

      private function mpReadinessHandler(ready:Boolean) : void
      {
         this._mpLauncherReady = ready;
         if(!ready) this._mpPendingCommand = "";
         this.mpUpdateControls();
      }

      private function mpUpdateControls() : void
      {
         var idle:Boolean = this._mpLauncherReady && this._mpPendingCommand == "";
         var outsideLobby:Boolean = this._mpLobbyRole == "";
         this._multiplayerItem.enabled = this._mpLauncherReady;
         this._multiplayerItem.description = this._mpLauncherReady ? "Play Alien: Isolation with friends" : "Waiting for the launcher to be ready";
         this._createServerItem.enabled = idle && outsideLobby;
         this._directIpJoinItem.enabled = idle && outsideLobby;
         this._serverStartItem.enabled = idle && outsideLobby;
         this._serverModeItem.enabled = idle && outsideLobby;
         this._campaignSaveItem.enabled = idle && outsideLobby;
         this._serverMapItem.enabled = idle && outsideLobby;
         this._serverAlienItem.enabled = idle && outsideLobby;
         this._serverPvPItem.enabled = idle && outsideLobby;
         this._hostLobbyPlayer1Item.enabled = idle && this._mpLobbyRole == "host" && this._mpAlienVsSurvivors && String(this._hostLobbyPlayer1Item.data).length > 0;
         this._hostLobbyPlayer2Item.enabled = idle && this._mpLobbyRole == "host" && this._mpAlienVsSurvivors && String(this._hostLobbyPlayer2Item.data).length > 0;
         this._hostLobbyPlayer3Item.enabled = idle && this._mpLobbyRole == "host" && this._mpAlienVsSurvivors && String(this._hostLobbyPlayer3Item.data).length > 0;
         this._hostLobbyPlayer4Item.enabled = idle && this._mpLobbyRole == "host" && this._mpAlienVsSurvivors && String(this._hostLobbyPlayer4Item.data).length > 0;
         this._serverRefreshItem.enabled = idle && outsideLobby;
         this._friendsItem.enabled = this._mpLauncherReady;
         this._friendsRefreshItem.enabled = this._mpLauncherReady;
         this._hostInviteItem.enabled = this._mpLauncherReady && this._mpLobbyRole == "host";
         this._clientInviteItem.enabled = this._mpLauncherReady && this._mpLobbyRole == "client";
         this._friendJoinItem.enabled = idle && outsideLobby && this._mpSelectedFriendCanJoin;
         var inviteIndex:int = 0;
         while(inviteIndex < this._inviteRows.length)
         {
            MenuItem(this._inviteRows[inviteIndex]).enabled = idle && outsideLobby;
            inviteIndex++;
         }
         this._mpResumeLobbyItem.visible = !outsideLobby;
         this._mpResumeLobbyItem.enabled = idle;
         this._hostLobbyStartGameItem.enabled = idle && this._mpLobbyRole == "host" && this._mpCanStart;
         this._hostLobbyKick2Item.visible = false;
         this._clientLobbyReadyItem.enabled = idle && this._mpLobbyRole == "client";
         this._hostLobbyLeaveItem.enabled = idle && this._mpLobbyRole == "host";
         this._clientLobbyLeaveItem.enabled = idle && this._mpLobbyRole == "client";
         this.refreshNetworkMenuRenderers();
      }

      private function mpBeginCommand(command:String) : Boolean
      {
         if(!this._mpLauncherReady || this._mpPendingCommand != "") return false;
         this._mpPendingCommand = command;
         this.mpUpdateControls();
         return true;
      }

      private function mpResumeLobbyHandler(event:MenuItemEvent) : void
      {
         if(this._mpLauncherReady && this._mpLobbyRole != "") this.mpOpenLobbyPage(this._mpLobbyRole);
      }

      private function mpSessionClosedHandler(reason:String) : void
      {
         var hadLobby:Boolean = this._mpLobbyRole != "" && this._mpPendingCommand != "create" && this._mpPendingCommand != "join";
         this._mpLobbyRole = "";
         this._mpCanStart = false;
         this._mpPendingCommand = "";
         this._mpLocalReady = false;
         this._clientLobbyReadyItem.selected = false;
         this.mpStopLobbyReturn();
         this.mpUpdateControls();
         if(hadLobby && NewFrontendMenuState.MENU.equals(this._state)) this.menu.setCurrentPage(MenuPageId.ROOT.value,0);
         this.show_network_notification(reason);
      }

      private function mpBridgeStatusHandler(param1:String) : void
      {
         this.show_network_notification(param1);
         this._hostLobbyStatusItem.name = "Status: " + param1;
         this._clientLobbyStatusItem.name = "Status: " + param1;
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpBridgeErrorHandler(param1:String) : void
      {
         this._clientLobbyReadyItem.selected = this._mpLocalReady;
         this._mpPendingCommand = "";
         this.mpUpdateControls();
         this.show_network_notification(param1);
         this._hostLobbyStatusItem.name = "Error: " + param1;
         this._clientLobbyStatusItem.name = "Error: " + param1;
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpBridgeServerListHandler(param1:Array) : void
      {
         var index:int = 0;
         while(index < this._serverRows.length)
         {
            this.updateServerRow(this._serverRows[index],index < param1.length ? param1[index] : null,index);
            index++;
         }
         this.show_network_notification(param1.length > 0 ? "Server list updated" : "No lobbies found");
         this.refreshNetworkMenuRenderers();
      }

      private function updateServerRow(row:Object, server:Object, rowIndex:int) : void
      {
         if(row == null)
         {
            return;
         }
         if(server == null)
         {
            row.name = rowIndex == 0 ? "No lobbies found" : "No lobby";
            row.enabled = false;
            row.data = "";
            row.visible = rowIndex == 0;
            return;
         }
         row.visible = true;
         var serverName:String = String(getObjectString(server,"name","Server"));
         var serverMode:String = String(getObjectString(server,"mode","Survival"));
         var serverPlayers:String = String(getObjectString(server,"players","1"));
         var serverMaxPlayers:String = String(getObjectString(server,"maxPlayers",""));
         if(serverMaxPlayers != null && serverMaxPlayers.length > 0)
         {
            serverPlayers += "/" + serverMaxPlayers;
         }
         var serverPing:String = String(getObjectString(server,"ping","LAN"));
         var serverMap:String = String(getObjectString(server,"mapName","Auto Map"));
         row.name = serverName + " | " + serverMode + " | " + serverMap + " | " + serverPlayers + " | " + serverPing;
         row.enabled = getObjectString(server,"locked","0") != "1";
         row.data = getObjectString(server,"id","");
      }
      
      private function getObjectString(source:Object, key:String, fallback:String) : String
      {
         var value:* = undefined;
         try
         {
            if(source != null)
            {
               value = source[key];
               if(value != undefined && value != null && String(value) != "undefined")
               {
                  return String(value);
               }
            }
         }
         catch(error:Error)
         {
         }
         return fallback == null ? "" : fallback;
      }
      
      private function mpBridgeLobbyStateHandler(values:Object) : void
      {
         var param1:String = String(values["role"]);
         var param2:String = String(values["status"]);
         var param3:String = String(values["p1"]);
         var param4:String = String(values["p2"]);
         var param5:String = String(values["p3"]);
         var param6:String = String(values["p4"]);
         this._mpLobbyRole = param1;
         this._mpAlienVsSurvivors = values["alienVsSurvivors"] == "1";
         this._mpCanStart = values["canStart"] == "1";
         this._mpLocalReady = values["localReady"] == "1";
         this._clientLobbyReadyItem.selected = this._mpLocalReady;
         if(this._mpPendingCommand == "ready" || this._mpPendingCommand == "kick" || this._mpPendingCommand == "role")
         {
            this._mpPendingCommand = "";
         }
         this.mpUpdateControls();
         this._hostLobbyStatusItem.name = "Status: " + param2;
         this._clientLobbyStatusItem.name = "Status: " + param2;
         this._hostLobbyPlayer1Item.name = param3;
         this._hostLobbyPlayer2Item.name = param4;
         this._hostLobbyPlayer3Item.name = param5;
         this._hostLobbyPlayer4Item.name = param6;
         this._hostLobbyPlayer1Item.data = values["peer1"] == undefined ? "" : String(values["peer1"]);
         this._hostLobbyPlayer2Item.data = values["peer2"] == undefined ? "" : String(values["peer2"]);
         this._hostLobbyPlayer3Item.data = values["peer3"] == undefined ? "" : String(values["peer3"]);
         this._hostLobbyPlayer4Item.data = values["peer4"] == undefined ? "" : String(values["peer4"]);
         this._clientLobbyPlayer1Item.name = param3;
         this._clientLobbyPlayer2Item.name = param4;
         this._clientLobbyPlayer3Item.name = param5;
         this._clientLobbyPlayer4Item.name = param6;
         this.mpUpdateControls();
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpBridgeLobbyOpenedHandler(role:String) : void
      {
         this._mpPendingCommand = "";
         this._mpLobbyRole = role;
         this.mpUpdateControls();
         this.mpBridgeLobbyReturnHandler(role);
      }

      private function mpBridgeLobbyReturnHandler(param1:String) : void
      {
         this._mpLobbyReturnRole = param1 == "host" ? "host" : "client";
         this._mpLobbyReturnFrames = 0;
         removeEventListener(Event.ENTER_FRAME,this.mpLobbyReturnFrameHandler);
         addEventListener(Event.ENTER_FRAME,this.mpLobbyReturnFrameHandler,false,0,true);
      }
      
      private function mpLobbyReturnFrameHandler(param1:Event) : void
      {
         this._mpLobbyReturnFrames++;
         if(this._mpLobbyReturnRole == null || this._mpLobbyReturnFrames > 3600)
         {
            this.mpStopLobbyReturn();
            return;
         }
         if(menu == null || this._state == null)
         {
            return;
         }
         if(NewFrontendMenuState.SPLASH.equals(this._state))
         {
            if(this._mpLobbyReturnFrames == 30)
            {
               this.setState(NewFrontendMenuState.MENU.id,true);
            }
            return;
         }
         if(!NewFrontendMenuState.MENU.equals(this._state))
         {
            return;
         }
         this.mpOpenLobbyPage(this._mpLobbyReturnRole);
         this.mpStopLobbyReturn();
      }
      
      private function mpOpenLobbyPage(param1:String) : void
      {
         var _loc2_:MenuPage = param1 == "host" ? this._serverStartItem.page : this._serverLanItem.page;
         try
         {
            menu.rebuildPageMap();
            menu.setCurrentPage(_loc2_.id.value,0);
            this.show_network_notification("Lobby ready");
         }
         catch(error:Error)
         {
         }
         this.refreshNetworkMenuRenderers();
      }
      
      private function mpStopLobbyReturn() : void
      {
         removeEventListener(Event.ENTER_FRAME,this.mpLobbyReturnFrameHandler);
         this._mpLobbyReturnRole = null;
      }
      
      private function refreshNetworkMenuRenderers() : void
      {
         try
         {
            if(this.menu != null)
            {
               this.menu.invalidateData();
               this.menu.invalidateRenderers();
            }
         }
         catch(error:Error)
         {
         }
      }
      
      private function resetCachedSurvivorMaps() : void
      {
         this._survivorMapIds = ["survival_auto"];
         this._survivorMapLabels = ["Auto Survivor Map"];
         this._survivorMapDescriptions = ["Uses the first available survivor map until the game playlist data is loaded"];
         this.refreshSurvivorMapOptions();
      }
      
      private function cacheSurvivorMapArray(param1:Array, param2:String) : void
      {
         var _loc3_:int = 0;
         if(param1 == null)
         {
            return;
         }
         while(_loc3_ < param1.length)
         {
            this.cacheSurvivorMapCandidate(param1[_loc3_],param2,_loc3_);
            _loc3_++;
         }
      }
      
      private function cacheSurvivorMapCandidate(param1:Object, param2:String, param3:int) : void
      {
         var _loc4_:String = null;
         var _loc5_:String = null;
         if(param1 == null)
         {
            return;
         }
         _loc4_ = this.extractFirstString(param1,["id","Id","ID","identifier","Identifier","playlistId","PlaylistId","playlist_id","challengeId","ChallengeId","mapId","MapId","name","Name","title","Title"]);
         _loc5_ = this.extractFirstString(param1,["title","Title","displayName","DisplayName","name","Name","label","Label","playlistName","PlaylistName","challengeName","ChallengeName","mapName","MapName"]);
         if(_loc4_ == null || _loc4_.length == 0)
         {
            _loc4_ = param2 + "_" + String(param3);
         }
         if(_loc5_ == null || _loc5_.length == 0)
         {
            _loc5_ = "Survivor Map " + String(this._survivorMapIds.length);
         }
         this.addSurvivorMapOption(_loc4_,_loc5_);
      }
      
      private function addSurvivorMapOption(param1:String, param2:String) : void
      {
         var _loc3_:int = 0;
         if(param1 == null || param1.length == 0)
         {
            return;
         }
         while(_loc3_ < this._survivorMapIds.length)
         {
            if(String(this._survivorMapIds[_loc3_]) == param1)
            {
               return;
            }
            _loc3_++;
         }
         if(this._survivorMapIds.length == 1 && String(this._survivorMapIds[0]) == "survival_auto")
         {
            this._survivorMapIds = [];
            this._survivorMapLabels = [];
            this._survivorMapDescriptions = [];
         }
         this._survivorMapIds.push(param1);
         this._survivorMapLabels.push(param2);
         this._survivorMapDescriptions.push("Host this survivor map: " + param2);
         this.refreshSurvivorMapOptions();
      }
      
      private function refreshSurvivorMapOptions() : void
      {
         if(this._serverMapItem != null)
         {
            var selectedMap:String = this._serverMapItem.selectedItem == null ? "" : String(this._serverMapItem.selectedItem);
            if(this._developerMode && String(this._serverModeItem.selectedItem) == "campaign")
               this._serverMapItem.setItems(this._campaignLevelIds,this._campaignLevelIds);
            else
               this._serverMapItem.setItems(this._survivorMapIds,this._survivorMapLabels,this._survivorMapDescriptions);
            this._serverMapItem.selectedIndex = 0;
         }
         if(this._serverMapItem != null)
         {
            var maps:Array = String(this._serverModeItem.selectedItem) == "campaign" ? this._campaignLevelIds : this._survivorMapIds;
            var index:int = maps.indexOf(selectedMap);
            if(index >= 0) this._serverMapItem.selectedIndex = index;
         }
         this.updateServerMapVisibility();
         this.refreshNetworkMenuRenderers();
      }
      
      private function updateServerMapVisibility() : void
      {
         if(this._serverMapItem != null && this._serverModeItem != null)
         {
            this._serverModeItem.visible = true;
            this._serverPvPItem.visible = String(this._serverModeItem.selectedItem) != "campaign";
            this._serverAlienItem.visible = String(this._serverPvPItem.selectedItem) != "pvp" || !this._serverPvPItem.visible;
            this._campaignSaveItem.visible = String(this._serverModeItem.selectedItem) == "campaign";
            this._serverMapItem.visible = this._developerMode && (!this._campaignSaveItem.visible || this.getCampaignChoice().action == "level");
            this._serverMapItem.name = String(this._serverModeItem.selectedItem) == "campaign" ? "Campaign Level" : "Survivor Map";
         }
      }
      
      private function getSelectedSurvivorMapId() : String
      {
         if(String(this._serverModeItem.selectedItem) == "campaign")
         {
            return this.getCampaignChoice().action == "level" && this._developerMode && this._serverMapItem.selectedItem != null ? String(this._serverMapItem.selectedItem) : "";
         }
         if(this._serverMapItem == null || this._serverMapItem.selectedItem == null)
         {
            return "survival_auto";
         }
         return String(this._serverMapItem.selectedItem);
      }
      
      private function getSelectedSurvivorMapName() : String
      {
         if(String(this._serverModeItem.selectedItem) == "campaign")
         {
            return this.getCampaignChoice().action == "level" && this._developerMode && this._serverMapItem.selectedItem != null ? String(this._serverMapItem.selectedItem) : "";
         }
         if(this._serverMapItem == null)
         {
            return "Auto Survivor Map";
         }
         return this.lookupSurvivorMapLabel(this.getSelectedSurvivorMapId());
      }
      
      private function lookupSurvivorMapLabel(param1:String) : String
      {
         var _loc2_:int = 0;
         while(_loc2_ < this._survivorMapIds.length)
         {
            if(String(this._survivorMapIds[_loc2_]) == param1)
            {
               return String(this._survivorMapLabels[_loc2_]);
            }
            _loc2_++;
         }
         return "Auto Survivor Map";
      }
      
      private function extractFirstString(param1:Object, param2:Array) : String
      {
         var _loc3_:int = 0;
         var _loc4_:String = null;
         var _loc5_:* = undefined;
         while(_loc3_ < param2.length)
         {
            _loc4_ = String(param2[_loc3_]);
            try
            {
               _loc5_ = param1[_loc4_];
               if(_loc5_ != undefined && _loc5_ != null && String(_loc5_).length > 0)
               {
                  return String(_loc5_);
               }
            }
            catch(error:Error)
            {
            }
            _loc3_++;
         }
         return "";
      }
      
      private function mpBridgeGameStartHandler(mode:String, mapId:String = null, mapName:String = null, sessionId:String = null, engineLevelPath:String = null, campaignAction:String = null, campaignDifficulty:String = null) : void
      {
         this._mpPendingCommand = "start";
         this.mpUpdateControls();
         this._mpPendingStartGame = true;
         this._mpPendingStartMode = mode == null ? "" : mode.toLowerCase();
         this._mpPendingStartMapId = mapId == null ? "" : mapId;
         this._mpPendingStartMapName = mapName == null ? "" : mapName;
         this._mpPendingSessionId = sessionId == null ? "" : sessionId;
         this._hostLobbyStatusItem.name = "Status: Starting game";
         this._clientLobbyStatusItem.name = "Status: Starting game";
         this.refreshNetworkMenuRenderers();
         this._mpBridge.ackStartGame(this._mpPendingSessionId,"gfx_received_start_game");
         if(this._mpPendingStartMode == "survival")
         {
            this.startPendingSurvivorMap();
         }
         else if(this._mpPendingStartMode == "campaign")
         {
            this.startPendingCampaign(engineLevelPath,campaignAction,campaignDifficulty);
         }
      }
      
      private function mpCharacterSelectionFrameHandler(event:Event) : void
      {
         if(this._state == null || this.characterSelection == null)
         {
            return;
         }
         if(NewFrontendMenuState.DLC_CHAR.equals(this._state))
         {
            this.mpCaptureCurrentCharacterSelection("gfx-character-frame",true);
         }
      }
      
      private function mpCaptureCurrentCharacterSelection(source:String, confirmed:Boolean) : void
      {
         var selectedIndex:int = this.mpGetCharacterSelectedIndex();
         var data:Object = this.mpGetCharacterDataForIndex(selectedIndex);
         var characterId:String = this.mpExtractCharacterId(data);
         var characterName:String = this.mpExtractCharacterName(data);
         var modelHint:String = this.mpExtractModelHint(data);
         if(characterId == null || characterId.length == 0)
         {
            return;
         }
         if(characterName == null || characterName.length == 0)
         {
            characterName = characterId;
         }
         if(characterId == this._mpLastCapturedCharacterId && selectedIndex == this._mpLastCapturedCharacterIndex)
         {
            return;
         }
         this._mpLastCapturedCharacterId = characterId;
         this._mpLastCapturedCharacterIndex = selectedIndex;
         if(this._mpBridge != null)
         {
            this._mpBridge.selectCharacter(this._mpPendingSessionId,characterId,characterName,modelHint,source,confirmed);
         }
         this.show_network_notification("Character: " + characterName);
      }
      
      private function mpGetCharacterSelectedIndex() : int
      {
         var selectedIndex:int = -1;
         var fn:Function = null;
         try
         {
            fn = this.characterSelection["get_selected_item_position"] as Function;
            if(fn != null)
            {
               selectedIndex = int(fn.apply(this.characterSelection));
            }
         }
         catch(error1:Error)
         {
         }
         if(selectedIndex >= 0)
         {
            return selectedIndex;
         }
         try
         {
            selectedIndex = int(this.characterSelection["local_menu_selection"]);
         }
         catch(error2:Error)
         {
         }
         if(selectedIndex >= 0)
         {
            return selectedIndex;
         }
         try
         {
            if(this.characterSelection["characterGroup"] != null)
            {
               selectedIndex = int(this.characterSelection["characterGroup"]["selectedButtonIndex"]);
            }
         }
         catch(error3:Error)
         {
         }
         if(selectedIndex >= 0)
         {
            return selectedIndex;
         }
         try
         {
            if(this.characterSelection["selectedRenderer"] != null)
            {
               selectedIndex = int(this.characterSelection["selectedRenderer"]["index"]);
            }
         }
         catch(error4:Error)
         {
         }
         return selectedIndex;
      }
      
      private function mpGetCharacterDataForIndex(selectedIndex:int) : Object
      {
         var data:Object = null;
         try
         {
            if(this.characterSelection != null && this.characterSelection["selectedRenderer"] != null)
            {
               data = this.characterSelection["selectedRenderer"]["characterData"];
               if(data != null)
               {
                  return data;
               }
               data = this.characterSelection["selectedRenderer"]["data"];
               if(data != null)
               {
                  return data;
               }
            }
         }
         catch(error1:Error)
         {
         }
         try
         {
            if(this.characterSelection != null && this.characterSelection["characterInfo"] != null)
            {
               data = this.characterSelection["characterInfo"]["characterData"];
               if(data != null)
               {
                  return data;
               }
            }
         }
         catch(error2:Error)
         {
         }
         if(this._mpCharacterOptions != null && selectedIndex >= 0 && selectedIndex < this._mpCharacterOptions.length)
         {
            return this._mpCharacterOptions[selectedIndex];
         }
         if(this._mpCharacterOptions != null && this._mpCharacterOptions.length > 0)
         {
            return this._mpCharacterOptions[0];
         }
         return null;
      }
      
      private function mpExtractCharacterId(data:Object) : String
      {
         var value:String = this.extractFirstString(data,["characterId","CharacterId","id","Id","ID","identifier","Identifier","name","Name","entry_character","entryCharacter"]);
         if(value != null && value.length > 0)
         {
            return value;
         }
         try
         {
            if(data != null && data["getCharacter"] != null)
            {
               return String(data["getCharacter"]());
            }
         }
         catch(error:Error)
         {
         }
         return "";
      }
      
      private function mpExtractCharacterName(data:Object) : String
      {
         return this.extractFirstString(data,["characterName","CharacterName","displayName","DisplayName","title","Title","name","Name","label","Label"]);
      }
      
      private function mpExtractModelHint(data:Object) : String
      {
         return this.extractFirstString(data,["modelHint","ModelHint","model","Model","modelName","ModelName","asset","Asset","assetName","AssetName","texture","Texture","icon","Icon"]);
      }
      
      private function startPendingCampaign(engineLevelPath:String, action:String, difficulty:String) : void
      {
         // START_GAME is sent only after the launcher has staged the authorized save.
         if(action == "level")
         {
            this.startPendingCampaignLevel(engineLevelPath);
            return;
         }
         if(action != "new" && action != "continue")
         {
            this._mpPendingStartGame = false;
            this._mpPendingCommand = "";
            this.mpUpdateControls();
            this._mpBridge.ackStartGame(this._mpPendingSessionId,"invalid_campaign_action");
            this.show_network_notification("Campaign start was not authorized by the launcher");
            return;
         }
         this._mpPendingStartGame = false;
         if(action == "continue")
         {
            this._mpBridge.ackStartGame(this._mpPendingSessionId,"loading_campaign_checkpoint");
            call("load_current_checkpoint");
            return;
         }
         var gameDifficulty:int = GameMenu.DIFFICULTY_MEDIUM;
         if(difficulty == "novice") gameDifficulty = GameMenu.DIFFICULTY_NOVICE;
         else if(difficulty == "easy") gameDifficulty = GameMenu.DIFFICULTY_EASY;
         else if(difficulty == "hard") gameDifficulty = GameMenu.DIFFICULTY_HARD;
         else if(difficulty == "nightmare") gameDifficulty = GameMenu.DIFFICULTY_IRON;
         this._mpBridge.ackStartGame(this._mpPendingSessionId,"starting_new_campaign");
         call("start_game",gameDifficulty);
      }

      private function startPendingCampaignLevel(engineLevelPath:String) : void
      {
         if(this._mpPendingStartMapId == null || this._mpPendingStartMapId.length == 0)
         {
            this.show_network_notification("No campaign level selected");
            this._mpBridge.ackStartGame(this._mpPendingSessionId,"missing_campaign_level");
            return;
         }
         if(engineLevelPath == null || engineLevelPath == "undefined" || engineLevelPath.length == 0)
            engineLevelPath = "Production\\" + this._mpPendingStartMapId;
         this._mpPendingStartGame = false;
         this._mpBridge.ackStartGame(this._mpPendingSessionId,"loading_campaign_level:" + engineLevelPath);
         call("load_level",engineLevelPath);
      }

      private function startPendingSurvivorMap() : void
      {
         if(this._mpPendingStartMapId == null || this._mpPendingStartMapId.length == 0 || this._mpPendingStartMapId == "survival_auto")
         {
            this.show_network_notification("No survivor map selected");
            this._mpBridge.ackStartGame(this._mpPendingSessionId,"missing_survivor_map");
            return;
         }
         if(this._mpPendingStartMapName != null && this._mpPendingStartMapName.length > 0)
         {
            this.show_network_notification("Loading " + this._mpPendingStartMapName);
         }
         else
         {
            this.show_network_notification("Loading survivor map");
         }
         ExternalInterface.call("request_character_selection",this._mpPendingStartMapId);
      }
      
      private function tryAutoStartSurvivorCharacter(characters:Array) : void
      {
         this.show_network_notification("Select a character");
         this._mpBridge.ackStartGame(this._mpPendingSessionId,"character_selection_opened");
         this._mpPendingStartGame = false;
      }
      
      // The launcher filters lobbies by mode; rows without a lobby stay hidden.
      private function applyServerFilter(param1:String) : void
      {
         var index:int = 0;
         while(index < this._serverRows.length)
         {
            var row:MenuItem = this._serverRows[index];
            row.visible = index == 0 || (row.data != null && String(row.data).length > 0);
            index++;
         }
      }
      
      private function twoTeamsNoviceDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_two_teams",GameMenu.DIFFICULTY_NOVICE);
      }
      
      private function twoTeamsEasyDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_two_teams",GameMenu.DIFFICULTY_EASY);
      }
      
      private function twoTeamsMediumDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_two_teams",GameMenu.DIFFICULTY_MEDIUM);
      }
      
      private function twoTeamsHardDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_two_teams",GameMenu.DIFFICULTY_HARD);
      }
      
      private function twoTeamsIronDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_two_teams",GameMenu.DIFFICULTY_IRON);
      }
      
      private function ripleyNoviceDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_ripley",GameMenu.DIFFICULTY_NOVICE);
      }
      
      private function ripleyEasyDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_ripley",GameMenu.DIFFICULTY_EASY);
      }
      
      private function ripleyMediumDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_ripley",GameMenu.DIFFICULTY_MEDIUM);
      }
      
      private function ripleyHardDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_ripley",GameMenu.DIFFICULTY_HARD);
      }
      
      private function ripleyIronDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("play_dlc_ripley",GameMenu.DIFFICULTY_IRON);
      }
      
      private function storeHandler(param1:MenuItemEvent) : void
      {
         call("open_store");
      }
      
      private function selectStorageDeviceHandler(param1:MenuItemEvent) : void
      {
         call("select_storage_device");
      }
      
      private function continueGameHandler(param1:MenuItemEvent) : void
      {
         call("load_current_checkpoint");
      }
      
      private function continueOnlineGameHandler(param1:MenuItemEvent) : void
      {
         call("load_current_checkpoint");
      }
      
      private function noviceDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("start_game",GameMenu.DIFFICULTY_NOVICE);
      }
      
      private function easyDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("start_game",GameMenu.DIFFICULTY_EASY);
      }
      
      private function mediumDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("start_game",GameMenu.DIFFICULTY_MEDIUM);
      }
      
      private function hardDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("start_game",GameMenu.DIFFICULTY_HARD);
      }
      
      private function ironDifficultyHandler(param1:MenuItemEvent) : void
      {
         call("start_game",GameMenu.DIFFICULTY_IRON);
      }
      
      private function creditsHandler(param1:MenuItemEvent) : void
      {
         call("show_credits_screen");
      }
      
      private function levelSelectionHandler(param1:MenuItemEvent) : void
      {
         call("load_level",param1.item.data as String);
      }
      
      private function startMultiplayerServer(levelName:String) : void
      {
         if(ExternalInterface.available)
         {
            try
            {
               ExternalInterface.call("startMultiplayerServer",levelName);
            }
            catch(error:Error)
            {
               trace("Une erreur s\'est produite lors de l\'appel de startMultiplayerServer: " + error.message);
            }
         }
         else
         {
            trace("ExternalInterface n\'est pas disponible.");
         }
      }
      
      private function menuInactivityHandler(param1:MenuEvent) : void
      {
         if(this._mpLobbyRole != "" || this._mpPendingCommand != "") return;
         requestState(NewFrontendMenuState.ATTRACT);
      }
      
      private function movieStopHandler(param1:MovieClipExEvent) : void
      {
         if(NewFrontendMenuState.ATTRACT.equals(this._state))
         {
            call("show_attract_screen");
         }
         if(!NewFrontendMenuState.MENU.equals(this._state))
         {
            menuParent.menuOnStage = false;
         }
         if(!NewFrontendMenuState.DLC_MAP.equals(this._state))
         {
            this.mapSelectionParent.selectionScreenOnStage = false;
         }
         if(!NewFrontendMenuState.DLC_CHAR.equals(this._state))
         {
            this.characterSelectionParent.selectionScreenOnStage = false;
         }
      }
      
      private function startInputHandler(param1:InputEvent) : void
      {
         if(param1.handled || this._state.id != NewFrontendMenuState.SPLASH.id)
         {
            return;
         }
         var _loc2_:InputDetails = param1.details;
         var _loc3_:Boolean = _loc2_.value == InputValue.KEY_DOWN || _loc2_.value == InputValue.KEY_HOLD;
         if(_loc3_ && _loc2_.code == KeyCodes.GAMEPAD_START)
         {
            this.setState(NewFrontendMenuState.MENU.id,true);
         }
      }
      
      private function headTrackingOnHandler(param1:MenuPageEvent) : void
      {
         call("set_head_tracking",true);
      }
      
      private function headTrackingOffHandler(param1:MenuPageEvent) : void
      {
         call("set_head_tracking",false);
      }
      
      private function movementDetectionOnHandler(param1:MenuPageEvent) : void
      {
         call("set_movement_detection",true);
      }
      
      private function movementDetectionOffHandler(param1:MenuPageEvent) : void
      {
         call("set_movement_detection",false);
      }
      
      private function noiseDetectionOnHandler(param1:MenuPageEvent) : void
      {
         call("set_noise_detection",true);
      }
      
      private function noiseDetectionOffHandler(param1:MenuPageEvent) : void
      {
         call("set_noise_detection",false);
      }
      
      private function mementoMoriOnHandler(param1:MenuPageEvent) : void
      {
         call("set_memento_mori",true);
      }
      
      private function mementoMoriOffHandler(param1:MenuPageEvent) : void
      {
         call("set_memento_mori",false);
      }
      
      override protected function setIds() : void
      {
         super.setIds();
         this._startGameItem.id = MenuItemId.START_GAME;
         this._continueGameItem.id = MenuItemId.CONTINUE_GAME;
         this._storeItem.id = MenuItemId.STORE;
         this._selectStorageDeviceItem.id = MenuItemId.SELECT_STORAGE_DEVICE;
         this._ripleyItem.id = MenuItemId.RIPLEY;
         this._survivorItem.id = MenuItemId.SURVIVOR;
         this._selectLevelItem.id = MenuItemId.DEBUG_LOAD_LEVEL;
         this._playGameItem.page.id = MenuPageId.PLAY_GAME;
         this._extrasItem.page.id = MenuPageId.EXTRAS;
         this._startGameItem.page.id = MenuPageId.START_GAME;
         this._selectLevelItem.page.id = MenuPageId.SELECT_LEVEL;
      }
      
      override protected function setDescriptions() : void
      {
         super.setDescriptions();
         this._playGameItem.description = "AI_UI_FRONTEND_PLAY_GAME_DESC";
         this._extrasItem.description = "AI_UI_FRONTEND_EXTRAS_DESC";
         this._storeItem.description = "AI_UI_FRONTEND_STORE_DESC";
         this._selectStorageDeviceItem.description = "AI_UI_FRONTEND_SELECT_STORAGE_DEVICE_DESC";
         this._campaignItem.description = "AI_UI_FRONTEND_CAMPAIGN_DESC";
         this._twoTeamsItem.description = "AI_UI_DLC_NOSTROMO_TWO_TEAMS_DESC";
         this._ripleyItem.description = "AI_UI_DLC_NOSTROMO_RIPLEY_DESC";
         this._survivorItem.description = "AI_UI_FRONTEND_SURVIVOR_DESC";
         this._startGameItem.description = "AI_UI_FRONTEND_START_GAME";
         this._continueGameItem.description = "AI_UI_FRONTEND_CONTINUE_GAME";
         _loadGameItem.description = "AI_UI_MENU_LOAD_GAME";
         this._twoTeamsNoviceDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_NOVICE";
         this._twoTeamsEasyDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EASY";
         this._twoTeamsMediumDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_MEDIUM";
         this._twoTeamsHardDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_HARD";
         this._twoTeamsIronDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EXPERT";
         this._ripleyNoviceDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_NOVICE";
         this._ripleyEasyDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EASY";
         this._ripleyMediumDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_MEDIUM";
         this._ripleyHardDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_HARD";
         this._ripleyIronDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EXPERT";
         this._noviceDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_NOVICE";
         this._easyDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EASY";
         this._mediumDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_MEDIUM";
         this._hardDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_HARD";
         this._ironDifficultyItem.description = "AI_UI_MENU_DIFF_DESC_EXPERT";
         this._creditsItem.description = "AI_UI_FRONTEND_CREDITS_DESC";
         this._headTrackingOnItem.description = "AI_UI_MENU_HEAD_TRACKING_DESC";
         this._movementDetectionOnItem.description = "AI_UI_MENU_MOVEMENT_DETECTION_DESC";
         this._noiseDetectionOnItem.description = "AI_UI_MENU_NOISE_DETECTION_DESC";
         this._mementoMoriOnItem.description = "AI_UI_MENU_MEMENTO_MORI_DESC";
         this._headTrackingOnItem.visualsName = "HeadTrackingDiagram";
         this._movementDetectionOnItem.visualsName = "MovementDetectionDiagram";
         this._noiseDetectionOnItem.visualsName = "NoiseDetectionDiagram";
         this._mementoMoriOnItem.visualsName = "MementoMoriDiagram";
         this._multiplayerItem.description = this._mpLauncherReady ? "Play Alien: Isolation with friends" : "Waiting for the launcher to be ready";
         this._createServerItem.description = "Configure a new server";
         this._joinServerItem.description = "Browse public and friends' lobbies on Steam";
         this._friendsItem.description = "Accept invites and join friends' lobbies (invite friends from inside a lobby)";
         this._friendsRefreshItem.description = "Reload your online Steam friends and invites";
         this._friendsEmptyItem.description = "Open Steam and sign in to see your friends";
         this._hostInviteItem.description = "Send your online Steam friends an invite to this lobby";
         this._clientInviteItem.description = "Send your online Steam friends an invite to this lobby";
         this._friendJoinItem.description = "Join the lobby this friend is hosting";
         this._directIpJoinItem.description = "Connect directly to the Host IP and Port configured in the launcher";
         this._serverNameItem.description = "Change your player name in launcher settings";
         this._serverNameItem.enabled = false;
         this._serverPasswordItem.visible = false;
         this._serverPasswordItem.description = "Configured in the launcher settings";
         this._serverModeItem.descriptions = ["Create a survival server"];
         this._campaignSaveItem.description = "Start a new campaign on Medium or continue a save provided by the launcher";
         this._serverMapItem.descriptions = this._survivorMapDescriptions;
         this._serverAlienItem.descriptions = ["Normal Alien behavior","Disable the Alien to explore and test this level"];
         this._serverMaxPlayersItem.descriptions = ["Two player MVP","Three players experimental","Four players experimental"];
         this._serverVisibilityItem.descriptions = ["Not listed - friends join by invite or direct IP; you can start alone","Shown in your Steam friends' server browser","Shown in everyone's server browser"];
         this._serverStartItem.description = "Create lobby and wait for players";
         this._serverRefreshItem.description = "Refresh the server list";
         this._serverFilterItem.descriptions = ["Show every server","Show campaign servers","Show survival servers"];
         var rowIndex:int = 0;
         while(rowIndex < this._serverRows.length)
         {
            MenuItem(this._serverRows[rowIndex]).description = "Steam lobby - press to join";
            rowIndex++;
         }
         this._hostLobbyStatusItem.description = "Waiting for players and level verification";
         this._hostLobbyStatusItem.enabled = false;
         this._hostLobbyPlayer1Item.enabled = false;
         this._hostLobbyPlayer2Item.enabled = false;
         this._clientLobbyStatusItem.enabled = false;
         this._clientLobbyPlayer1Item.enabled = false;
         this._clientLobbyPlayer2Item.enabled = false;
         this._hostLobbyPlayer1Item.description = "Local owner slot";
         this._hostLobbyPlayer2Item.description = "Remote player slot";
         this._hostLobbyPlayer3Item.description = "Remote player slot";
         this._hostLobbyPlayer4Item.description = "Remote player slot";
         this._hostLobbyPlayer3Item.visible = true;
         this._hostLobbyPlayer4Item.visible = true;
         this._hostLobbyKick3Item.visible = false;
         this._hostLobbyKick4Item.visible = false;
         this._hostLobbyKick2Item.description = "Remove the other player from this lobby";
         this._hostLobbyKick3Item.description = "Owner only kick action";
         this._hostLobbyKick4Item.description = "Owner only kick action";
         this._hostLobbyStartGameItem.description = "Owner only start action";
         this._hostLobbyLeaveItem.description = "Close hosted lobby";
         this._clientLobbyStatusItem.description = "Connected lobby state";
         this._clientLobbyPlayer1Item.description = "Host player";
         this._clientLobbyPlayer2Item.description = "Your client slot";
         this._clientLobbyReadyItem.descriptions = ["Ready for game start","Not ready yet"];
         this._clientLobbyLeaveItem.description = "Disconnect from lobby";
      }
      
      override protected function setTransitions() : void
      {
         super.setTransitions();
      }
      
      override protected function setVisibility() : void
      {
         super.setVisibility();
         _difficultyItem.visible = false;
         this._selectStorageDeviceItem.visible = false;
         this._twoTeamsItem.visible = true;
         this._ripleyItem.visible = true;
         this._selectLevelItem.visible = true;
         this.updateServerMapVisibility();
         this._playGameItem.visible = false;
         this._joinServerItem.visible = true;
         this._serverModeItem.visible = true;
         this._serverMaxPlayersItem.visible = false;
         this._serverVisibilityItem.visible = true;
         this.mpUpdateControls();
         this._extrasItem.visible = true;
         this._storeItem.visible = true;
         this.applyServerFilter("all");
      }
      
      public function show_network_notification(param1:String) : void
      {
         this.network_notification_mc.network_notification_sub_mc.txt.text = param1;
         this.network_notification_mc.gotoAndPlay("fade_in");
      }
      
      public function hide_network_notification() : void
      {
         this.network_notification_mc.gotoAndPlay("fade_out");
      }
      
      internal function __setProp_description_Scene1_description_0() : *
      {
         try
         {
            description["componentInspectorSetting"] = true;
         }
         catch(e:Error)
         {
         }
         description.transitionInEnabled = true;
         description.transitionInLabel = "transition_in";
         description.transitionOutEnabled = true;
         description.transitionOutLabel = "transition_out";
         description.transitionOwner = "self";
         description.visible = true;
         try
         {
            description["componentInspectorSetting"] = false;
            return;
         }
         catch(e:Error)
         {
            return;
         }
      }
      
      internal function __setProp_userProfilePanel_Scene1_profile_0() : *
      {
         try
         {
            userProfilePanel["componentInspectorSetting"] = true;
         }
         catch(e:Error)
         {
         }
         userProfilePanel.transitionInEnabled = false;
         userProfilePanel.transitionInLabel = "transition_in";
         userProfilePanel.transitionOutEnabled = false;
         userProfilePanel.transitionOutLabel = "transition_out";
         userProfilePanel.transitionOwner = "self";
         userProfilePanel.visible = false;
         try
         {
            userProfilePanel["componentInspectorSetting"] = false;
            return;
         }
         catch(e:Error)
         {
            return;
         }
      }
      
      internal function frame1() : *
      {
         stop();
      }
      
      internal function frame10() : *
      {
         stop();
      }
      
      internal function frame20() : *
      {
         stop();
      }
      
      internal function frame30() : *
      {
         gotoAndStop("menu");
      }
      
      internal function frame36() : *
      {
         stop();
      }
      
      internal function frame46() : *
      {
         stop();
      }
      
      internal function frame54() : *
      {
         stop();
      }
   }
}
