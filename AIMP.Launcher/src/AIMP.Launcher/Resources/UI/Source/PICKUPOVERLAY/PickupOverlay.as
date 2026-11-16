package
{
   import flash.display.*;
   import flash.events.*;
   import flash.external.*;
   import flash.net.Socket;
   import flash.net.URLRequest;
   import flash.geom.Point;
   import flash.geom.Rectangle;
   import flash.text.*;
   import flash.utils.*;
   
   public class PickupOverlay extends MovieClip
   {
      private var aimp_vent_layer:Sprite;
      private var aimp_vent_at:int = 0;

      private function aimp_hide_vents() : void
      {
         if(this.aimp_vent_layer != null && this.aimp_vent_layer.parent != null)
            this.aimp_vent_layer.parent.removeChild(this.aimp_vent_layer);
         this.aimp_vent_layer = null;
      }

      private function aimp_update_vents(line:String) : void
      {
         this.aimp_hide_vents();this.aimp_vent_at = getTimer();
         if(this.aimp_get_bridge_value(line,"active") != "1" || this.aimp_loading_layer != null || this.aimp_round_end_layer != null) return;
         var ready:Boolean = this.aimp_get_bridge_value(line,"controlReady") == "1";
         var free:Boolean = this.aimp_get_bridge_value(line,"freeCamera") == "1";
         var pending:Boolean = this.aimp_get_bridge_value(line,"pending") == "1";
         var nearby:int = int(this.aimp_get_bridge_value(line,"nearby"));
         var phase:int = int(this.aimp_get_bridge_value(line,"phase"));
         var action:int = int(this.aimp_get_bridge_value(line,"action"));
         if(ready && !free && !pending && nearby < 0) return;
         var width:Number = stage != null ? stage.stageWidth : 1280;
         var height:Number = stage != null ? stage.stageHeight : 720;
         var text:String = "Waiting for Alien control authorization from Host...";
         var help:String = "";
         if(ready)
         {
            if(pending) text = action == 3 ? "Exiting ventilation... E / Q: cancel" : "Entering ventilation... E / Q: cancel";
            else if(free) text = nearby >= 0 ? 'Press "E" to exit ventilation here' : "Move close to a ventilation opening to exit";
            else text = 'Press "E" to enter ventilation';
            if(free) help = "Mouse: look | WASD: fly | Space / Ctrl: up / down | Shift: faster";
            else if(phase == 3 || phase == 4) help = "Ventilation unavailable. Move closer and try again.";
         }
         this.aimp_vent_layer = new Sprite();this.aimp_vent_layer.mouseEnabled = false;this.aimp_vent_layer.mouseChildren = false;
         this.aimp_vent_layer.graphics.beginFill(0,0.82);
         this.aimp_vent_layer.graphics.drawRect(width * 0.12,height * 0.80,width * 0.76,help == "" ? 52 : 84);
         this.aimp_vent_layer.graphics.endFill();
         var prompt:TextField = this.aimp_create_round_end_text(text,22,width * 0.76,height * 0.80 + 10);prompt.x = width * 0.12;
         this.aimp_vent_layer.addChild(prompt);
         if(help != "")
         {
            var hint:TextField = this.aimp_create_round_end_text(help,16,width * 0.76,height * 0.80 + 45);hint.x = width * 0.12;
            this.aimp_vent_layer.addChild(hint);
         }
         addChild(this.aimp_vent_layer);
      }

      // Spectate banner (adapted from alien-isolation-coop's "SPECTATING <name>").
      // Refreshed by the Launcher while the local player is dead; a stale
      // banner hides itself like the vent bar.
      private var aimp_spectate_layer:Sprite;
      private var aimp_spectate_at:int = 0;

      private function aimp_hide_spectate() : void
      {
         if(this.aimp_spectate_layer != null && this.aimp_spectate_layer.parent != null)
            this.aimp_spectate_layer.parent.removeChild(this.aimp_spectate_layer);
         this.aimp_spectate_layer = null;
      }

      private function aimp_update_spectate(line:String) : void
      {
         this.aimp_hide_spectate();this.aimp_spectate_at = getTimer();
         if(this.aimp_get_bridge_value(line,"active") != "1" || this.aimp_loading_layer != null || this.aimp_round_end_layer != null || this.aimp_mission_wait_layer != null) return;
         var name:String = this.aimp_get_bridge_value(line,"name");
         var others:int = int(this.aimp_get_bridge_value(line,"others"));
         var width:Number = stage != null ? stage.stageWidth : 1280;
         var height:Number = stage != null ? stage.stageHeight : 720;
         var top:Number = height * 0.05;
         var panel:Number = others > 1 ? 112 : 84;
         this.aimp_spectate_layer = new Sprite();this.aimp_spectate_layer.mouseEnabled = false;this.aimp_spectate_layer.mouseChildren = false;
         this.aimp_spectate_layer.graphics.beginFill(0,0.6);
         this.aimp_spectate_layer.graphics.drawRect(width * 0.3,top,width * 0.4,panel);
         this.aimp_spectate_layer.graphics.endFill();
         var title:TextField = this.aimp_create_round_end_text("SPECTATING",16,width * 0.4,top + 8);title.x = width * 0.3;
         this.aimp_spectate_layer.addChild(title);
         var label:TextField = this.aimp_create_round_end_text(others > 1 ? "<   " + name + "   >" : name,28,width * 0.4,top + 32);label.x = width * 0.3;
         this.aimp_spectate_layer.addChild(label);
         if(others > 1)
         {
            var hint:TextField = this.aimp_create_round_end_text("Left / Right arrow: switch player",14,width * 0.4,top + 78);hint.x = width * 0.3;
            this.aimp_spectate_layer.addChild(hint);
         }
         addChild(this.aimp_spectate_layer);
      }

      public var pickup_cursor_mc:MovieClip;

      public var scriptedMessage_mc:MovieClip;
      
      public var mc_tutorial_message_root:MovieClip;
      
      public var audiolog_popup_mc:MovieClip;
      
      public var glyphtest_mc:MovieClip;
      
      public var mc_tutorial_overlay:MovieClip;
      
      public var mc_tutorial_overlay_kb:MovieClip;
      
      public var preNames_mc:MovieClip;
      
      public var upgradeNotifier_mc:MovieClip;
      
      public var staticPrompt_mc:MovieClip;
      
      public var breathingtutorial_mc:MovieClip;
      
      public var container_mc:MovieClip;
      
      public var rewirePopUp_mc:MovieClip;
      
      public var holdThrowPrompt_mc:MovieClip;
      
      internal const STATE_OFF:* = 0;
      
      internal const STATE_CONTAINER:* = 1;
      
      internal const STATE_HACKING:* = 2;
      
      internal const STATE_KEYPAD:* = 3;
      
      internal const DELAY_BEFORE_EXIT_ON_EMPTY_CONTAINER:* = 1000;
      
      private var m_state:int = 0;
      
      private var container_hide_timer:Timer;
      
      internal var audio_pickup_notification_timer:Timer = null;
      
      internal const AUDIO_NOTICIFACTION_TIME:* = 2500;
      
      internal var frame_counter_count:Number = 0;
      
      internal var total_duration:Number = 0;
      
      internal var m_current_message_mc:MovieClip;
      
      internal const NUM_CONTAINER_SLOTS:* = 3;
      
      private var m_container_selection:int = 0;
      
      private var m_container_contents:Vector.<String>;
      
      private var roomForContainerItem:Array;
      
      private var m_size_icon:Vector.<String>;
      
      private var m_keypad_selection:int = -1;
      
      private var m_keypad_code:String = "0000";
      
      private var m_current_code_entered:String = "";
      
      internal const HACKING_CURSOR_TWEEN_LENGTH:* = 25;
      
      internal const HACKING_WIN_AREA_MAX_EXTENSION:* = 3;
      
      internal const HACKING_BAR_WIDTH:* = 286;
      
      internal const HACKING_STAGES:* = 3;
      
      private var m_hacking_stage:int = 0;
      
      private var m_hacking_result:int = 0;
      
      private var m_exit_on_fail:Boolean;
      
      private var m_hacking_win_area_start:Vector.<int>;
      
      private var m_hacking_win_area_end:Vector.<int>;
      
      internal const MAX_PICKUP_CURSOR_ALPHA:* = 0.5;
      
      internal const CURSOR_FADE_PER_FRAME:* = 0.02;
      
      private var m_cursor_fade_active:Boolean = false;
      
      private var m_cursor_target_alpha:Number = 0;
      
      private var m_inventory_currently_full:Boolean = false;
      
      public var m_hud_forced_hidden:Boolean = false;
      
      public var substitution:ImageSubstitution;
      
      private var m_layer_name:String;
      
      private var aimp_bridge_socket:Socket;
      
      private var aimp_bridge_buffer:String = "";
      
      private var aimp_bridge_connected:Boolean = false;
      
      private var aimp_remote_state_text:TextField;
      
      private var aimp_remote_state_ack_sent:Boolean = false;
      
      private var aimp_cathode_command_count:int = 0;
      
      private var aimp_loading_layer:Sprite;

      private var aimp_loading_text:TextField;

      private var aimp_loading_session:String = "";

      private var aimp_loading_generation:String = "";

      // Preserve release tombstones when the visible layer is removed. A delayed
      // status or HELLO replay must never cover an already released world again.
      private var aimp_loading_released:Object = {};

      private var aimp_round_end_layer:Sprite;

      // MISSION_WAIT: this player finished; other participants are still playing.
      private var aimp_mission_wait_layer:Sprite;

      private var aimp_mission_wait_title:TextField;

      private var aimp_mission_wait_text:TextField;
      
      private var aimp_round_end_count_text:TextField;
      
      private var aimp_round_end_deadline:int = 0;

      private var aimp_round_end_active:Boolean = false;

      private var aimp_round_return_fields:String = "";

      private var aimp_returning:Boolean = false;

      private var aimp_round_return_pending:Boolean = false;

      private var aimp_round_return_ack_deadline:int = 0;
      private var aimp_round_return_retry:Boolean = false;

      private var aimp_bridge_close_pending:Boolean = false;

      private var aimp_bridge_init_pending:Boolean = false;

      private var aimp_bridge_connect_pending:Boolean = false;

      private var aimp_bridge_error_pending:Boolean = false;
      
      private var aimp_round_end_remaining:int = 0;
      
      internal var size_keyframes:Array;
      
      public function PickupOverlay()
      {
         this.container_hide_timer = new Timer(this.DELAY_BEFORE_EXIT_ON_EMPTY_CONTAINER,0);
         this.m_container_contents = new Vector.<String>(this.NUM_CONTAINER_SLOTS);
         this.roomForContainerItem = new Array(this.NUM_CONTAINER_SLOTS);
         this.m_size_icon = new Vector.<String>(this.NUM_CONTAINER_SLOTS);
         this.m_hacking_win_area_start = new Vector.<int>(this.HACKING_STAGES);
         this.m_hacking_win_area_end = new Vector.<int>(this.HACKING_STAGES);
         this.substitution = new ImageSubstitution();
         this.size_keyframes = ["small","medium","large"];
         super();
         addFrameScript(0,this.frame1);
         addEventListener(Event.ADDED_TO_STAGE,this.initclass);
         addEventListener(Event.ADDED_TO_STAGE,this.aimp_resume_bridge);
         addEventListener(Event.REMOVED_FROM_STAGE,this.aimp_release_bridge);
         this.container_hide_timer.addEventListener(TimerEvent.TIMER,this.end_exit_timer);
      }
      
      public function init_image_substitution(param1:String, param2:Boolean, param3:Boolean) : void
      {
         if(param2)
         {
            if(this.mc_tutorial_overlay.visible)
            {
               this.mc_tutorial_overlay_kb.visible = true;
               this.mc_tutorial_overlay.visible = false;
            }
            this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg_kb.visible = true;
            this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg.visible = false;
            this.staticPrompt_mc.mc.glyph_txt.y = -9.5;
         }
         else
         {
            if(this.mc_tutorial_overlay_kb.visible)
            {
               this.mc_tutorial_overlay_kb.visible = false;
               this.mc_tutorial_overlay.visible = true;
            }
            this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg_kb.visible = false;
            this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg.visible = true;
            this.staticPrompt_mc.mc.glyph_txt.y = -11.5;
         }
         this.substitution.register_loaded_callback(this.substitution_loaded_callback);
         this.substitution.initialise(param1,param2,param3,ImageSubstitution.SUBSTITUTION_ACTIONS | ImageSubstitution.SUBSTITUTION_BUTTONS | ImageSubstitution.SUBSTITUTION_UI);
      }
      
      private function substitution_loaded_callback() : void
      {
         this.substitution.Add_TextField(this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg);
         this.substitution.Add_TextField(this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg_kb);
         this.substitution.Add_TextField(this.staticPrompt_mc.mc.txt);
         this.substitution.Add_TextField(this.staticPrompt_mc.mc.glyph_txt);
         this.substitution.Add_TextField(this.holdThrowPrompt_mc.mc.nested_mc.txt);
         this.substitution.Add_TextField(this.mc_tutorial_overlay.mc_tutorial_overlay_sub.txt);
         this.substitution.Add_TextField(this.mc_tutorial_overlay_kb.mc_tutorial_overlay_sub.txt);
         this.substitution.Add_TextField(this.container_mc.legend_mc.controls_1_mc);
         this.substitution.Add_TextField(this.container_mc.legend_mc.controls_2_mc);
         this.substitution.Add_TextField(this.container_mc.legend_mc.controls_3_mc);
         this.substitution.Add_TextField(this.upgradeNotifier_mc.text_mc.sub);
         this.substitution.Add_TextField(this.rewirePopUp_mc.content.rewireInstruction3);
         this.substitution.Add_TextField(this.rewirePopUp_mc.content.continueGlyph);
         this.substitution.Add_TextField(this.preNames_mc.prompt);
         this.substitution.Add_TextField(this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg);
      }
      
      public function force_hud_visible() : void
      {
         this.staticPrompt_mc.gotoAndStop("off");
         this.container_mc.visible = true;
         this.staticPrompt_mc.visible = true;
         this.holdThrowPrompt_mc.visible = true;
         this.mc_tutorial_message_root.visible = true;
         this.m_hud_forced_hidden = false;
      }
      
      public function showRewirePopUp() : *
      {
         this.rewirePopUp_mc.visible = true;
         this.rewirePopUp_mc.gotoAndPlay("trans_in");
      }
      
      public function hideRewirePopUp() : *
      {
         this.rewirePopUp_mc.gotoAndPlay("trans_out");
      }
      
      public function showChar() : *
      {
         this.preNames_mc.visible = true;
      }
      
      public function switchChar(param1:int) : *
      {
         if(param1 == 0)
         {
            this.preNames_mc.charName.text = "AI_UI_CHAR_NAME_3";
         }
         else if(param1 == 1)
         {
            this.preNames_mc.charName.text = "AI_UI_CHAR_NAME_4";
         }
         else if(param1 == 2)
         {
            this.preNames_mc.charName.text = "AI_UI_CHAR_NAME_2";
         }
         else
         {
            this.preNames_mc.charName.text = "AI_UI_CHAR_NAME_3";
         }
      }
      
      public function hideChar() : *
      {
         this.preNames_mc.visible = false;
      }
      
      public function display_glyphtest() : void
      {
         this.glyphtest_mc.visible = true;
      }
      
      public function hide_glyphtest() : void
      {
         this.glyphtest_mc.visible = false;
      }
      
      public function force_hud_hidden() : void
      {
         this.container_mc.visible = false;
         this.staticPrompt_mc.visible = false;
         this.holdThrowPrompt_mc.visible = false;
         this.mc_tutorial_message_root.visible = false;
         this.staticPrompt_mc.gotoAndStop("off");
         this.m_hud_forced_hidden = true;
      }
      
      public function resizeListener() : *
      {
         var _loc1_:Number = 1280;
         var _loc2_:Number = 720;
         var _loc3_:Number = stage.stageWidth;
         var _loc4_:Number = stage.stageHeight;
         var _loc5_:Number = _loc3_ / _loc1_;
         var _loc6_:Number = _loc4_ / _loc2_;
         var _loc7_:Number = _loc5_;
         if(_loc6_ < _loc7_)
         {
            _loc7_ = _loc6_;
         }
         stage.scaleMode = StageScaleMode.NO_SCALE;
         stage.align = StageAlign.TOP_LEFT;
         var _loc8_:Number = 853;
         var _loc9_:Number = 51;
         this.scriptedMessage_mc.x = stage.stageWidth / 2;
         this.scriptedMessage_mc.y = 160 * _loc7_;
         this.scriptedMessage_mc.width = _loc8_ * _loc7_;
         this.scriptedMessage_mc.height = _loc9_ * _loc7_;
         var _loc10_:Number = 750;
         var _loc11_:Number = 80;
         this.staticPrompt_mc.width = _loc10_ * _loc7_;
         this.staticPrompt_mc.height = _loc11_ * _loc7_;
         this.staticPrompt_mc.x = stage.stageWidth / 2;
         this.staticPrompt_mc.y = stage.stageHeight / 2 + 85 * _loc7_;
         var _loc12_:Number = 479;
         var _loc13_:Number = 75;
         this.mc_tutorial_message_root.width = _loc12_ * _loc7_;
         this.mc_tutorial_message_root.height = _loc13_ * _loc7_;
         this.mc_tutorial_message_root.x = stage.stageWidth / 2;
         this.mc_tutorial_message_root.y = stage.stageHeight / 2 + 160 * _loc7_;
         var _loc14_:Number = 690;
         var _loc15_:Number = 100;
         this.container_mc.width = _loc14_ * _loc7_;
         this.container_mc.height = _loc15_ * _loc7_;
         this.container_mc.x = stage.stageWidth / 2;
         this.container_mc.y = stage.stageHeight / 2;
         this.pickup_cursor_mc.x = stage.stageWidth / 2;
         this.pickup_cursor_mc.y = stage.stageHeight / 2;
         var _loc16_:Number = 385;
         var _loc17_:Number = 64;
         this.holdThrowPrompt_mc.width = _loc16_ * _loc7_;
         this.holdThrowPrompt_mc.height = _loc17_ * _loc7_;
         this.holdThrowPrompt_mc.x = 96 * _loc7_;
         this.holdThrowPrompt_mc.y = stage.stageHeight - 132 * _loc7_;
         this.breathingtutorial_mc.x = stage.stageWidth - 40;
         this.breathingtutorial_mc.y = stage.stageHeight - 40;
         var _loc18_:Number = 356;
         var _loc19_:Number = 90;
         this.audiolog_popup_mc.width = _loc18_ * _loc7_;
         this.audiolog_popup_mc.height = _loc19_ * _loc7_;
         this.audiolog_popup_mc.x = stage.stageWidth - 60 * _loc7_;
         this.audiolog_popup_mc.y = 250 * _loc7_;
         var _loc20_:Number = 1175;
         var _loc21_:Number = 30;
         this.mc_tutorial_overlay.width = _loc20_ * _loc7_;
         this.mc_tutorial_overlay.height = _loc21_ * _loc7_;
         this.mc_tutorial_overlay.x = stage.stageWidth / 2;
         this.mc_tutorial_overlay.y = stage.stageHeight - 65 * _loc7_;
         this.mc_tutorial_overlay_kb.width = _loc20_ * _loc7_;
         this.mc_tutorial_overlay_kb.height = _loc21_ * _loc7_;
         this.mc_tutorial_overlay_kb.x = stage.stageWidth / 2;
         this.mc_tutorial_overlay_kb.y = stage.stageHeight - 65 * _loc7_;
         var _loc22_:Number = 411;
         var _loc23_:Number = 93;
         this.upgradeNotifier_mc.width = _loc22_ * _loc7_;
         this.upgradeNotifier_mc.height = _loc23_ * _loc7_;
         this.upgradeNotifier_mc.x = stage.stageWidth / 2;
         this.upgradeNotifier_mc.y = 110 * _loc7_;
         var _loc24_:Number = 540;
         var _loc25_:Number = 80;
         this.preNames_mc.width = _loc24_ * _loc7_;
         this.preNames_mc.height = _loc25_ * _loc7_;
         this.preNames_mc.x = stage.stageWidth / 2;
         this.preNames_mc.y = 590 * _loc7_;
         var _loc26_:Number = 725;
         var _loc27_:Number = 370;
         this.rewirePopUp_mc.width = _loc26_ * _loc7_;
         this.rewirePopUp_mc.height = _loc27_ * _loc7_;
         this.rewirePopUp_mc.x = stage.stageWidth / 2;
         this.rewirePopUp_mc.y = stage.stageHeight / 2;
      }
      
      public function Init(param1:String) : *
      {
         this.m_layer_name = param1;
         this.mc_tutorial_overlay.mc_tutorial_overlay_sub.txt.multiline = true;
         this.container_mc.legend_mc.controls_1_mc.htmlText = "AI_UI_PICKUPOVERLAY_TAKE";
         this.container_mc.legend_mc.controls_2_mc.htmlText = "AI_UI_PICKUPOVERLAY_TAKE_ALL";
         this.container_mc.legend_mc.controls_3_mc.htmlText = "AI_UI_PICKUPOVERLAY_EXIT";
      }
      
      public function display_alt_tutorial_overlay(param1:String, param2:String) : *
      {
         this.mc_tutorial_overlay.mc_tutorial_overlay_sub.txt.text = param1;
         this.mc_tutorial_overlay.bg.width = this.mc_tutorial_overlay.mc_tutorial_overlay_sub.txt.textWidth + this.staticPrompt_mc.mc.txt.textWidth / 8;
         this.mc_tutorial_overlay_kb.mc_tutorial_overlay_sub.txt.text = param2;
         this.mc_tutorial_overlay_kb.bg.width = this.mc_tutorial_overlay_kb.mc_tutorial_overlay_sub.txt.textWidth + this.staticPrompt_mc.mc.txt.textWidth / 8;
         if(this.substitution.using_keyboard())
         {
            if(this.mc_tutorial_overlay_kb.currentFrameLabel == "hidden")
            {
               this.mc_tutorial_overlay_kb.visible = true;
            }
         }
         else if(this.mc_tutorial_overlay.currentFrameLabel == "hidden")
         {
            this.mc_tutorial_overlay.visible = true;
         }
         this.mc_tutorial_overlay_kb.gotoAndPlay("trans-in");
         this.mc_tutorial_overlay.gotoAndPlay("trans-in");
      }
      
      public function hide_alt_tutorial_overlay() : *
      {
         if(this.substitution.using_keyboard())
         {
            if(this.mc_tutorial_overlay_kb.visible)
            {
               this.mc_tutorial_overlay_kb.gotoAndPlay("trans-out");
            }
            this.mc_tutorial_overlay.gotoAndPlay("trans-out");
         }
         else
         {
            if(this.mc_tutorial_overlay.visible)
            {
               this.mc_tutorial_overlay.gotoAndPlay("trans-out");
            }
            this.mc_tutorial_overlay_kb.gotoAndPlay("trans-out");
         }
      }
      
      public function set_tutorial_overlay_message(param1:String, param2:String) : *
      {
         this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg.text = param1;
         this.mc_tutorial_message_root.mc_tutorialmessage.txt_mc.txt_tutorial_msg_kb.text = param2;
      }
      
      public function display_tutorial_overlay(param1:Boolean) : *
      {
         this.mc_tutorial_message_root.anim.visible = param1;
         this.mc_tutorial_message_root.visible = true;
         this.mc_tutorial_message_root.gotoAndPlay("trans-in");
      }
      
      public function display_audio_pickup_message(param1:String, param2:String, param3:Number) : *
      {
         this.audiolog_popup_mc.log_author_mc.txt_loglauthor.text = param1;
         this.audiolog_popup_mc.logtitle_mc.txt_logtitle.text = param2;
         this.audiolog_popup_mc.timecode_mc.txt_timecode.text = param3;
         this.audiolog_popup_mc.visible = true;
         this.audiolog_popup_mc.gotoAndPlay("trans-in");
         if(this.audio_pickup_notification_timer == null)
         {
            this.audio_pickup_notification_timer = new Timer(this.AUDIO_NOTICIFACTION_TIME,0);
            this.audio_pickup_notification_timer.addEventListener(TimerEvent.TIMER,this.OnAudioNotificationCallback);
            this.audio_pickup_notification_timer.start();
         }
         this.total_duration = param3;
         this.frame_counter_count = param3;
      }
      
      public function cancel_audio_pickup_message() : *
      {
         this.total_duration = 0;
         this.frame_counter_count = 0;
         if(this.audio_pickup_notification_timer != null)
         {
            this.audio_pickup_notification_timer.removeEventListener(TimerEvent.TIMER,this.OnAudioNotificationCallback);
            this.audio_pickup_notification_timer.stop();
            this.audio_pickup_notification_timer = null;
         }
         this.audiolog_popup_mc.gotoAndPlay("trans-out");
      }
      
      public function OnUpdateFrameCounter(param1:Number) : void
      {
         var _loc2_:Number = NaN;
         if(this.frame_counter_count > 0)
         {
            this.frame_counter_count -= param1;
            if(this.frame_counter_count < 0)
            {
               this.frame_counter_count = 0;
            }
            _loc2_ = this.frame_counter_count / this.total_duration;
            this.audiolog_popup_mc.timecode_mc.txt_timecode.text = this.frame_counter_count.toFixed(2);
            if(this.frame_counter_count <= 0)
            {
               this.audio_pickup_complete();
            }
         }
      }
      
      internal function go_to_second_audio_loop() : void
      {
         this.audiolog_popup_mc.gotoAndPlay("transition_to_b");
      }
      
      internal function audio_pickup_complete() : void
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("audio_pickup_complete",0);
         }
         this.audiolog_popup_mc.gotoAndPlay("trans-out");
      }
      
      internal function OnAudioNotificationCallback(param1:TimerEvent) : void
      {
         trace("Audio notification callback");
         this.go_to_second_audio_loop();
         this.audio_pickup_notification_timer.removeEventListener(TimerEvent.TIMER,this.OnAudioNotificationCallback);
         this.audio_pickup_notification_timer = null;
      }
      
      public function display_breathing_game_tutorial_1() : *
      {
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.text = "AI_UI_TUTORIAL_BREATHING_TUTORIAL_2";
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.textColor = 16777215;
      }
      
      public function display_breathing_game_tutorial_2() : *
      {
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.text = "AI_UI_TUTORIAL_BREATHING_TUTORIAL_3";
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.textColor = 16777215;
      }
      
      public function display_breathing_game_tutorial_fail() : *
      {
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.textColor = 16711680;
      }
      
      public function hide_tutorial_overlay(param1:Boolean) : *
      {
         this.mc_tutorial_message_root.anim.visible = param1;
         this.mc_tutorial_message_root.gotoAndPlay("trans-out");
      }
      
      public function initclass(param1:Event) : *
      {
         removeEventListener(Event.ADDED_TO_STAGE,this.initclass);
         stage.addEventListener(KeyboardEvent.KEY_DOWN,this.KeyDownList);
         trace("pickup overlay added to stage");
         this.container_mc.item0_mc.gotoAndStop("empty");
         this.container_mc.item1_mc.gotoAndStop("empty");
         this.container_mc.item2_mc.gotoAndStop("empty");
         this.container_mc.item0_mc.inventoryfull_mc.txt.text = "AI_UI_INVENTORY_FULL";
         this.container_mc.item1_mc.inventoryfull_mc.txt.text = "AI_UI_INVENTORY_FULL";
         this.container_mc.item2_mc.inventoryfull_mc.txt.text = "AI_UI_INVENTORY_FULL";
         this.pickup_cursor_mc.alpha = 0;
      }
      
      private function aimp_resume_bridge(param1:Event) : void
      {
         if(!this.aimp_returning)
         {
            this.aimp_bridge_init_pending = this.aimp_bridge_socket == null;
            addEventListener(Event.ENTER_FRAME,this.aimp_on_bridge_frame,false,0,true);
         }
      }

      private function aimp_release_bridge(param1:Event) : void
      {
         removeEventListener(Event.ENTER_FRAME,this.aimp_on_bridge_frame);
         this.aimp_bridge_init_pending = false;
         this.aimp_hide_round_end();
         this.aimp_hide_mission_wait();
         this.aimp_hide_loading();
         this.aimp_close_bridge();
         this.aimp_returning = false;
         this.aimp_round_return_pending = false;
         this.aimp_bridge_close_pending = false;
      }

      private function aimp_close_bridge() : void
      {
         this.aimp_bridge_connected = false;
         this.aimp_bridge_connect_pending = false;
         this.aimp_bridge_error_pending = false;
         this.aimp_bridge_buffer = "";
         if(this.aimp_bridge_socket == null)
         {
            return;
         }
         this.aimp_bridge_socket.removeEventListener(Event.CONNECT,this.aimp_on_bridge_connect);
         this.aimp_bridge_socket.removeEventListener(Event.CLOSE,this.aimp_on_bridge_close);
         this.aimp_bridge_socket.removeEventListener(ProgressEvent.SOCKET_DATA,this.aimp_on_bridge_data);
         this.aimp_bridge_socket.removeEventListener(IOErrorEvent.IO_ERROR,this.aimp_on_bridge_io_error);
         this.aimp_bridge_socket.removeEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimp_on_bridge_security_error);
         try
         {
            this.aimp_bridge_socket.close();
         }
         catch(error:Error)
         {
         }
         this.aimp_bridge_socket = null;
      }

      private function aimp_init_runtime_bridge() : void
      {
         try
         {
            this.aimp_close_bridge();
            this.aimp_remote_state_ack_sent = false;
            this.aimp_bridge_socket = new Socket();
            this.aimp_bridge_socket.addEventListener(Event.CONNECT,this.aimp_on_bridge_connect);
            this.aimp_bridge_socket.addEventListener(Event.CLOSE,this.aimp_on_bridge_close);
            this.aimp_bridge_socket.addEventListener(ProgressEvent.SOCKET_DATA,this.aimp_on_bridge_data);
            this.aimp_bridge_socket.addEventListener(IOErrorEvent.IO_ERROR,this.aimp_on_bridge_io_error);
            this.aimp_bridge_socket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimp_on_bridge_security_error);
            this.aimp_bridge_socket.connect("127.0.0.1",12345);
         }
         catch(error:Error)
         {
            trace("AIMP gameplay bridge init failed: " + error.message);
            this.aimp_set_remote_state_status("Bridge init failed");
         }
      }
      
      private function aimp_on_bridge_connect(param1:Event) : void
      {
         if(param1.currentTarget == this.aimp_bridge_socket)
         {
            this.aimp_bridge_connect_pending = true;
         }
      }
      
      private function aimp_on_bridge_data(param1:ProgressEvent) : void
      {
         if(param1.currentTarget != this.aimp_bridge_socket || this.aimp_bridge_socket == null || this.aimp_returning)
         {
            return;
         }
         try
         {
            if(this.aimp_bridge_socket.bytesAvailable > 65536 || this.aimp_bridge_buffer.length + this.aimp_bridge_socket.bytesAvailable > 65536)
            {
               trace("AIMP gameplay bridge exceeded its receive limit; closing connection.");
               this.aimp_bridge_error_pending = true;
               return;
            }
            this.aimp_bridge_buffer += this.aimp_bridge_socket.readUTFBytes(this.aimp_bridge_socket.bytesAvailable);
         }
         catch(error:Error)
         {
            trace("AIMP gameplay bridge read failed: " + error.message);
            this.aimp_bridge_error_pending = true;
         }
      }

      // Socket callbacks only collect data. Display changes, native callbacks
      // and the round deadline are consumed in order on the movie's frame.
      private function aimp_on_bridge_frame(param1:Event) : void
      {
         if(this.aimp_vent_layer != null && getTimer() - this.aimp_vent_at > 2000) this.aimp_hide_vents();
         if(this.aimp_spectate_layer != null && (getTimer() - this.aimp_spectate_at > 2000 || this.aimp_round_end_layer != null || this.aimp_mission_wait_layer != null)) this.aimp_hide_spectate();
         var lineEnd:int = 0;
         var line:String = null;
         var count:int = 0;
         var remaining:int = 0;
         if(stage == null || this.aimp_returning)
         {
            return;
         }
         // Close only after the Launcher's matching response was consumed on
         // the previous frame. flush()+close() can discard the last write in GFx.
         if(this.aimp_bridge_close_pending)
         {
            this.aimp_returning = true;
            removeEventListener(Event.ENTER_FRAME,this.aimp_on_bridge_frame);
            this.aimp_close_bridge();
            return;
         }
         if(this.aimp_bridge_init_pending)
         {
            this.aimp_bridge_init_pending = false;
            this.aimp_init_runtime_bridge();
         }
         if(this.aimp_bridge_connect_pending)
         {
            this.aimp_bridge_connect_pending = false;
            this.aimp_bridge_connected = true;
            this.aimp_send_bridge_message("HELLO|source=pickup_overlay|phase=gameplay");
            this.aimp_send_bridge_message("PICKUP_OVERLAY_LOADED|source=pickup_overlay|phase=gameplay");
            this.aimp_log_bridge("info","lifecycle","PickupOverlay bridge connected and ready");
         }
         while(count < 64 && stage != null && !this.aimp_returning)
         {
            lineEnd = this.aimp_bridge_buffer.indexOf("\n");
            if(lineEnd < 0)
            {
               break;
            }
            line = this.aimp_bridge_buffer.substr(0,lineEnd);
            this.aimp_bridge_buffer = this.aimp_bridge_buffer.substr(lineEnd + 1);
            this.aimp_handle_bridge_line(line);
            count++;
         }
         // Preserve the FIFO across frames, including commands received before
         // a transport close. Never coalesce gameplay commands or round controls.
         if(this.aimp_bridge_buffer.indexOf("\n") >= 0 || stage == null)
         {
            return;
         }
         if(this.aimp_bridge_error_pending)
         {
            this.aimp_close_bridge();
            this.aimp_set_remote_state_status("Launcher bridge offline");
         }
         if(this.aimp_round_end_active)
         {
            remaining = Math.max(0,Math.ceil((this.aimp_round_end_deadline - getTimer()) / 1000));
            if(remaining != this.aimp_round_end_remaining)
            {
               this.aimp_round_end_remaining = remaining;
               this.aimp_update_round_end_count();
            }
            if(remaining == 0)
            {
               this.aimp_round_end_active = false;
               this.aimp_round_return_pending = true;
               this.aimp_round_return_ack_deadline = getTimer() + 10000;
               if(!this.aimp_send_bridge_message("ROUND_RETURN_READY|source=pickup_overlay|phase=gameplay" + this.aimp_round_return_fields))
               {
                  this.aimp_round_end_count_text.text = "Unable to return to lobby. Use Disconnect in Pause.";
               }
            }
         }
         if(this.aimp_round_return_pending && !this.aimp_bridge_close_pending && getTimer() >= this.aimp_round_return_ack_deadline)
         {
            if(this.aimp_round_return_retry)
            {
               // A timeout does not end the readiness handshake. Keep this
               // movie alive until its matching ACK is consumed on a frame.
               this.aimp_round_end_count_text.text = "Returning to lobby...";
               this.aimp_round_return_ack_deadline = getTimer() + 2000;
               if(this.aimp_bridge_socket == null)
               {
                  this.aimp_bridge_init_pending = true;
               }
               else
               {
                  this.aimp_send_bridge_message("ROUND_RETURN_READY|source=pickup_overlay|phase=gameplay" + this.aimp_round_return_fields);
               }
               return;
            }
            this.aimp_round_end_count_text.text = "Unable to return to lobby. Use Disconnect in Pause.";
            this.aimp_returning = true;
            removeEventListener(Event.ENTER_FRAME,this.aimp_on_bridge_frame);
            this.aimp_close_bridge();
         }
      }
      
      private function aimp_handle_bridge_line(param1:String) : void
      {
         var _loc2_:String = null;
         if(param1 == null)
         {
            return;
         }
         param1 = param1.split("\r").join("");
         if(param1.length == 0)
         {
            return;
         }
         if(param1.indexOf("ALIEN_VENTS|") == 0)
         {
            this.aimp_update_vents(param1);
            return;
         }
         if(param1.indexOf("SPECTATE|") == 0)
         {
            this.aimp_update_spectate(param1);
            return;
         }
         if(param1.indexOf("SESSION_LOADING|") == 0)
         {
            this.aimp_update_loading(param1);
            return;
         }
         if(this.aimp_round_return_pending)
         {
            if(this.aimp_round_return_retry && param1.indexOf("ROUND_END|") == 0 &&
               this.aimp_get_bridge_value(param1,"retryReturn") == "1" &&
               this.aimp_get_bridge_value(param1,"sessionId") == this.aimp_get_bridge_value(this.aimp_round_return_fields,"sessionId") &&
               this.aimp_get_bridge_value(param1,"generation") == this.aimp_get_bridge_value(this.aimp_round_return_fields,"generation"))
            {
               // Only a renewed token for the same ended round is admissible.
               this.aimp_round_return_fields = "|sessionId=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"sessionId"));
               this.aimp_round_return_fields += "|generation=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"generation"));
               this.aimp_round_return_fields += "|returnToken=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"returnToken"));
               this.aimp_round_return_ack_deadline = getTimer() + 10000;
               this.aimp_send_bridge_message("ROUND_RETURN_READY|source=pickup_overlay|phase=gameplay" + this.aimp_round_return_fields);
               return;
            }
            if(param1.indexOf("ROUND_RETURN_CLOSE|") == 0 &&
               this.aimp_get_bridge_value(param1,"returnToken") == this.aimp_get_bridge_value(this.aimp_round_return_fields,"returnToken") &&
               this.aimp_get_bridge_value(param1,"sessionId") == this.aimp_get_bridge_value(this.aimp_round_return_fields,"sessionId") &&
               this.aimp_get_bridge_value(param1,"generation") == this.aimp_get_bridge_value(this.aimp_round_return_fields,"generation"))
            {
               this.aimp_bridge_close_pending = true;
            }
            else if(param1.indexOf("CATHODE_COMMAND|") == 0)
            {
               this.aimp_send_bridge_message("CATHODE_COMMAND_ACK|source=pickup_overlay|phase=gameplay|result=round_ended|requestId=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"requestId")));
            }
            return;
         }
         if(param1.indexOf("CATHODE_COMMAND") == 0)
         {
            this.aimp_execute_cathode_command(param1);
         }
         else if(param1.indexOf("REMOTE_PLAYER_STATE|") == 0)
         {
            this.aimp_update_remote_state(param1);
         }
         else if(param1.indexOf("MISSION_WAIT|") == 0)
         {
            this.aimp_show_mission_wait(param1);
         }
         else if(param1.indexOf("ROUND_END|") == 0 || param1 == "ROUND_END")
         {
            this.aimp_show_round_end(param1);
         }
         else if(param1.indexOf("AIMP_RUNTIME_TEST") == 0)
         {
            _loc2_ = this.aimp_get_bridge_value(param1,"message");
            if(_loc2_ == "")
            {
               _loc2_ = "Runtime test from launcher";
            }
            this.aimp_set_remote_state_status(_loc2_);
            this.aimp_send_bridge_message("AIMP_RUNTIME_TEST_ACK|source=pickup_overlay|phase=gameplay|result=displayed");
         }
         else if(param1.indexOf("STATUS") == 0)
         {
            _loc2_ = this.aimp_get_bridge_value(param1,"message");
            if(_loc2_ != "")
            {
               this.aimp_set_remote_state_status(_loc2_);
            }
         }
         else if(param1.indexOf("ERROR") == 0)
         {
            _loc2_ = this.aimp_get_bridge_value(param1,"message");
            if(_loc2_ == "")
            {
               _loc2_ = "Launcher bridge error";
            }
            this.aimp_set_remote_state_status(_loc2_);
         }
         else
         {
            this.aimp_log_bridge("warn","rx_unknown","Ignored unknown bridge message: " + param1);
         }
      }
      
      // Native actor simulation is held independently of this display. The movie
      // keeps advancing and servicing its socket while native manifest work runs.
      private function aimp_update_loading(param1:String) : void
      {
         var sessionId:String = this.aimp_get_bridge_value(param1,"sessionId");
         var generation:String = this.aimp_get_bridge_value(param1,"generation");
         var active:Boolean = this.aimp_get_bridge_value(param1,"active") == "1";
         var message:String = this.aimp_get_bridge_value(param1,"message");
         var width:Number = stage != null ? stage.stageWidth : 1280;
         var height:Number = stage != null ? stage.stageHeight : 720;
         var generationNumber:Number = Number(generation);
         if(sessionId == "" || generation == "" || isNaN(generationNumber)
            || generationNumber < 1 || generationNumber > 2147483647
            || generationNumber != Math.floor(generationNumber)) return;
         if(active && this.aimp_loading_released[sessionId] != undefined
            && generationNumber <= Number(this.aimp_loading_released[sessionId])) return;
         if(!active && (sessionId != this.aimp_loading_session || generation != this.aimp_loading_generation)
            && (this.aimp_loading_released[sessionId] == undefined
                || generationNumber != Number(this.aimp_loading_released[sessionId]))) return;
         if(active && this.aimp_loading_session != "" &&
            (sessionId != this.aimp_loading_session || Number(generation) < Number(this.aimp_loading_generation))) return;
         if(active)
         {
            if(this.aimp_loading_layer == null)
            {
               this.aimp_loading_layer = new Sprite();
               this.aimp_loading_layer.mouseEnabled = true;
               this.aimp_loading_layer.mouseChildren = false;
               this.aimp_loading_layer.graphics.beginFill(0,1);
               this.aimp_loading_layer.graphics.drawRect(0,0,width,height);
               this.aimp_loading_layer.graphics.endFill();
               this.aimp_loading_layer.addChild(this.aimp_create_round_end_text("PREPARING MULTIPLAYER",42,width,height * 0.40));
               this.aimp_loading_text = this.aimp_create_round_end_text("",26,width,height * 0.55);
               this.aimp_loading_layer.addChild(this.aimp_loading_text);
               addChild(this.aimp_loading_layer);
            }
            this.aimp_loading_session = sessionId;
            this.aimp_loading_generation = generation;
            this.aimp_loading_text.text = message == "" ? "Preparing world and waiting for players..." : message;
            setChildIndex(this.aimp_loading_layer,numChildren - 1);
         }
         else
         {
            this.aimp_loading_released[sessionId] = generationNumber;
            // An idempotent release for an older token must not remove a newer
            // active loading layer, even though its own acknowledgement is valid.
            if(sessionId == this.aimp_loading_session && generation == this.aimp_loading_generation)
               this.aimp_hide_loading();
         }
         this.aimp_send_bridge_message("SESSION_LOADING_ACK|source=pickup_overlay|phase=gameplay|active=" + (active ? "1" : "0") + "|sessionId=" + this.aimp_encode_bridge_value(sessionId) + "|generation=" + this.aimp_encode_bridge_value(generation));
      }

      private function aimp_hide_loading() : void
      {
         if(this.aimp_loading_layer != null && this.aimp_loading_layer.parent != null)
            this.aimp_loading_layer.parent.removeChild(this.aimp_loading_layer);
         this.aimp_loading_layer = null;
         this.aimp_loading_text = null;
         this.aimp_loading_session = "";
         this.aimp_loading_generation = "";
      }

      private function aimp_show_round_end(param1:String) : void
      {
         var _loc2_:String = this.aimp_get_bridge_value(param1,"title");
         var _loc3_:String = this.aimp_get_bridge_value(param1,"message");
         var _loc4_:int = int(this.aimp_get_bridge_value(param1,"seconds"));
         var _loc5_:Number = 1280;
         var _loc6_:Number = 720;
         var _loc7_:TextField = null;
         if(_loc4_ <= 0 || _loc4_ > 60)
         {
            _loc4_ = 5;
         }
         this.aimp_hide_mission_wait();
         this.aimp_hide_round_end();
         try
         {
            if(stage != null)
            {
               _loc5_ = stage.stageWidth;
               _loc6_ = stage.stageHeight;
            }
            this.aimp_round_end_layer = new Sprite();
            this.aimp_round_end_layer.mouseEnabled = false;
            this.aimp_round_end_layer.mouseChildren = false;
            this.aimp_round_end_layer.graphics.beginFill(0,0.75);
            this.aimp_round_end_layer.graphics.drawRect(0,0,_loc5_,_loc6_);
            this.aimp_round_end_layer.graphics.endFill();
            _loc7_ = this.aimp_create_round_end_text(_loc2_ == "" ? "GAME OVER" : _loc2_,52,_loc5_,_loc6_ * 0.36);
            this.aimp_round_end_layer.addChild(_loc7_);
            _loc7_ = this.aimp_create_round_end_text(_loc3_,28,_loc5_,_loc6_ * 0.36 + 80);
            this.aimp_round_end_layer.addChild(_loc7_);
            this.aimp_round_end_count_text = this.aimp_create_round_end_text("",28,_loc5_,_loc6_ * 0.36 + 130);
            this.aimp_round_end_layer.addChild(this.aimp_round_end_count_text);
            addChild(this.aimp_round_end_layer);
            this.aimp_round_end_remaining = _loc4_;
            this.aimp_update_round_end_count();
            this.aimp_round_end_deadline = getTimer() + _loc4_ * 1000;
            this.aimp_round_return_retry = this.aimp_get_bridge_value(param1,"retryReturn") == "1";
            this.aimp_round_end_active = true;
            this.aimp_round_return_fields = "|sessionId=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"sessionId"));
            this.aimp_round_return_fields += "|generation=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"generation"));
            this.aimp_round_return_fields += "|returnToken=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"returnToken"));
            this.aimp_send_bridge_message("ROUND_END_ACK|source=pickup_overlay|phase=gameplay|result=displayed" + this.aimp_round_return_fields);
         }
         catch(error:Error)
         {
            this.aimp_log_bridge("error","round_end","Round end overlay failed: " + error.message);
         }
      }
      
      // Kept until the Host ends the round; ROUND_END replaces it with the shared
      // lobby-return countdown. Never shown over an active round-end layer.
      private function aimp_show_mission_wait(param1:String) : void
      {
         var title:String = this.aimp_get_bridge_value(param1,"title");
         var message:String = this.aimp_get_bridge_value(param1,"message");
         var width:Number = stage != null ? stage.stageWidth : 1280;
         var height:Number = stage != null ? stage.stageHeight : 720;
         if(this.aimp_round_end_layer != null)
         {
            return;
         }
         try
         {
            if(this.aimp_mission_wait_layer == null)
            {
               this.aimp_mission_wait_layer = new Sprite();
               this.aimp_mission_wait_layer.mouseEnabled = false;
               this.aimp_mission_wait_layer.mouseChildren = false;
               this.aimp_mission_wait_layer.graphics.beginFill(0,0.75);
               this.aimp_mission_wait_layer.graphics.drawRect(0,0,width,height);
               this.aimp_mission_wait_layer.graphics.endFill();
               this.aimp_mission_wait_title = this.aimp_create_round_end_text("",52,width,height * 0.36);
               this.aimp_mission_wait_layer.addChild(this.aimp_mission_wait_title);
               this.aimp_mission_wait_text = this.aimp_create_round_end_text("",28,width,height * 0.36 + 80);
               this.aimp_mission_wait_layer.addChild(this.aimp_mission_wait_text);
               addChild(this.aimp_mission_wait_layer);
            }
            this.aimp_mission_wait_title.text = title == "" ? "MISSION COMPLETE" : title;
            this.aimp_mission_wait_text.text = message == "" ? "Waiting for the other players..." : message;
            setChildIndex(this.aimp_mission_wait_layer,numChildren - 1);
            this.aimp_send_bridge_message("MISSION_WAIT_ACK|source=pickup_overlay|phase=gameplay|result=displayed|sessionId=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"sessionId")) + "|generation=" + this.aimp_encode_bridge_value(this.aimp_get_bridge_value(param1,"generation")));
         }
         catch(error:Error)
         {
            this.aimp_log_bridge("error","mission_wait","Mission wait overlay failed: " + error.message);
         }
      }

      private function aimp_hide_mission_wait() : void
      {
         if(this.aimp_mission_wait_layer != null && this.aimp_mission_wait_layer.parent != null)
         {
            this.aimp_mission_wait_layer.parent.removeChild(this.aimp_mission_wait_layer);
         }
         this.aimp_mission_wait_layer = null;
         this.aimp_mission_wait_title = null;
         this.aimp_mission_wait_text = null;
      }

      private function aimp_create_round_end_text(param1:String, param2:int, param3:Number, param4:Number) : TextField
      {
         var _loc5_:TextField = new TextField();
         var _loc6_:TextFormat = new TextFormat();
         var _loc7_:Object = null;
         try
         {
            _loc7_ = Object(this.scriptedMessage_mc).message_mc.messageContent_txt;
            if(_loc7_ != null)
            {
               _loc6_.font = TextField(_loc7_).getTextFormat().font;
               _loc5_.embedFonts = TextField(_loc7_).embedFonts;
            }
         }
         catch(fontError:Error)
         {
         }
         _loc6_.size = param2;
         _loc6_.color = 16777215;
         _loc6_.align = TextFormatAlign.CENTER;
         _loc5_.defaultTextFormat = _loc6_;
         _loc5_.selectable = false;
         _loc5_.mouseEnabled = false;
         _loc5_.multiline = true;
         _loc5_.wordWrap = true;
         _loc5_.width = param3;
         _loc5_.height = param2 * 2;
         _loc5_.x = 0;
         _loc5_.y = param4;
         _loc5_.text = param1;
         return _loc5_;
      }
      
      private function aimp_update_round_end_count() : void
      {
         if(this.aimp_round_end_count_text == null)
         {
            return;
         }
         if(this.aimp_round_end_remaining > 0)
         {
            this.aimp_round_end_count_text.text = "Returning to lobby in " + this.aimp_round_end_remaining + " s";
         }
         else
         {
            this.aimp_round_end_count_text.text = "Returning to lobby...";
         }
      }
      
      private function aimp_hide_round_end() : void
      {
         this.aimp_round_end_active = false;
         this.aimp_round_return_fields = "";
         this.aimp_round_return_retry = false;
         if(this.aimp_round_end_layer != null && this.aimp_round_end_layer.parent != null)
         {
            this.aimp_round_end_layer.parent.removeChild(this.aimp_round_end_layer);
         }
         this.aimp_round_end_layer = null;
         this.aimp_round_end_count_text = null;
      }
      
      private function aimp_send_bridge_message(param1:String) : Boolean
      {
         if(this.aimp_bridge_socket != null && this.aimp_bridge_socket.connected)
         {
            try
            {
               this.aimp_bridge_socket.writeUTFBytes(param1 + "\n");
               this.aimp_bridge_socket.flush();
               return true;
            }
            catch(error:Error)
            {
               this.aimp_bridge_error_pending = true;
               trace("AIMP gameplay bridge send failed: " + error.message);
            }
         }
         return false;
      }
      
      private function aimp_log_bridge(param1:String, param2:String, param3:String, param4:String = "", param5:String = "", param6:String = "", param7:String = "", param8:String = "", param9:String = "") : void
      {
         var _loc10_:String = "BRIDGE_LOG|source=pickup_overlay|phase=gameplay";
         _loc10_ += "|level=" + this.aimp_encode_bridge_value(param1 == null ? "info" : param1);
         _loc10_ += "|category=" + this.aimp_encode_bridge_value(param2 == null ? "bridge" : param2);
         _loc10_ += "|message=" + this.aimp_encode_bridge_value(param3 == null ? "" : param3);
         if(param4 != null && param4 != "")
         {
            _loc10_ += "|requestId=" + this.aimp_encode_bridge_value(param4);
         }
         if(param5 != null && param5 != "")
         {
            _loc10_ += "|callback=" + this.aimp_encode_bridge_value(param5);
         }
         if(param6 != null && param6 != "")
         {
            _loc10_ += "|kind=" + this.aimp_encode_bridge_value(param6);
         }
         if(param7 != null && param7 != "")
         {
            _loc10_ += "|value=" + this.aimp_encode_bridge_value(param7);
         }
         if(param8 != null && param8 != "")
         {
            _loc10_ += "|sentValue=" + this.aimp_encode_bridge_value(param8);
         }
         if(param9 != null && param9 != "")
         {
            _loc10_ += "|wrapper=" + this.aimp_encode_bridge_value(param9);
         }
         this.aimp_send_bridge_message(_loc10_);
      }
      
      private function aimp_execute_cathode_command(param1:String) : void
      {
         var _loc2_:String = this.aimp_get_bridge_value(param1,"kind");
         var _loc3_:String = this.aimp_get_bridge_value(param1,"callback");
         var _loc4_:String = this.aimp_get_bridge_value(param1,"value");
         var _loc5_:String = "";
         var _loc6_:Number = NaN;
         var _loc7_:int = 0;
         var _loc8_:String = this.aimp_get_bridge_value(param1,"requestId");
         var _loc9_:String = "";
         if(_loc3_ == "")
         {
            _loc3_ = this.aimp_get_bridge_value(param1,"command");
         }
         _loc2_ = _loc2_.toLowerCase();
         this.aimp_log_bridge("info","cathode_command_rx","Received Cathode command from launcher",_loc8_,_loc3_,_loc2_,_loc4_);
         if(_loc3_ == "")
         {
            this.aimp_set_remote_state_status("Cathode command ignored\nMissing callback name");
            this.aimp_log_bridge("warn","cathode_command","Cathode command ignored: missing callback name",_loc8_,_loc3_,_loc2_,_loc4_);
            this.aimp_send_bridge_message("CATHODE_COMMAND_ACK|source=pickup_overlay|phase=gameplay|result=missing_callback|requestId=" + this.aimp_encode_bridge_value(_loc8_));
            return;
         }
         if(!ExternalInterface.available)
         {
            this.aimp_set_remote_state_status("Cathode command failed\nExternalInterface unavailable\nCallback: " + _loc3_);
            this.aimp_log_bridge("error","cathode_command","ExternalInterface unavailable",_loc8_,_loc3_,_loc2_,_loc4_);
            this.aimp_send_bridge_message("CATHODE_COMMAND_ACK|source=pickup_overlay|phase=gameplay|result=external_interface_unavailable|requestId=" + this.aimp_encode_bridge_value(_loc8_) + "|callback=" + this.aimp_encode_bridge_value(_loc3_) + "|kind=" + this.aimp_encode_bridge_value(_loc2_));
            return;
         }
         try
         {
            if(_loc2_ == "float")
            {
               _loc6_ = Number(_loc4_);
               if(isNaN(_loc6_))
               {
                  _loc6_ = 0;
               }
               _loc9_ = String(_loc6_);
               _loc5_ = "GenericCallbackFloat";
               this.aimp_log_bridge("info","cathode_command_call","Calling ExternalInterface",_loc8_,_loc3_,_loc2_,_loc4_,_loc9_,_loc5_);
               ExternalInterface.call("GenericCallbackFloat",_loc3_,_loc6_);
            }
            else if(_loc2_ == "int" || _loc2_ == "integer")
            {
               _loc7_ = int(parseInt(_loc4_));
               if(isNaN(_loc7_))
               {
                  _loc7_ = 0;
               }
               _loc9_ = String(_loc7_);
               _loc5_ = "GenericCallbackInt";
               this.aimp_log_bridge("info","cathode_command_call","Calling ExternalInterface",_loc8_,_loc3_,_loc2_,_loc4_,_loc9_,_loc5_);
               ExternalInterface.call("GenericCallbackInt",_loc3_,_loc7_);
            }
            else
            {
               _loc7_ = int(parseInt(_loc4_));
               if(isNaN(_loc7_))
               {
                  _loc7_ = 1;
               }
               _loc9_ = String(_loc7_);
               _loc5_ = "GenericCallbackInt callback";
               _loc2_ = "callback";
               this.aimp_log_bridge("info","cathode_command_call","Calling ExternalInterface",_loc8_,_loc3_,_loc2_,_loc4_,_loc9_,_loc5_);
               ExternalInterface.call("GenericCallbackInt",_loc3_,_loc7_);
            }
            this.aimp_cathode_command_count++;
            this.aimp_set_remote_state_status("Cathode command sent #" + this.aimp_cathode_command_count + "\nWrapper: " + _loc5_ + "\nCallback: " + _loc3_ + "\nValue: " + _loc4_ + "\nSentValue: " + _loc9_);
            this.aimp_log_bridge("info","cathode_command_result","ExternalInterface call completed",_loc8_,_loc3_,_loc2_,_loc4_,_loc9_,_loc5_);
            this.aimp_send_bridge_message("CATHODE_COMMAND_ACK|source=pickup_overlay|phase=gameplay|result=sent|requestId=" + this.aimp_encode_bridge_value(_loc8_) + "|callback=" + this.aimp_encode_bridge_value(_loc3_) + "|kind=" + this.aimp_encode_bridge_value(_loc2_) + "|value=" + this.aimp_encode_bridge_value(_loc4_) + "|sentValue=" + this.aimp_encode_bridge_value(_loc9_) + "|wrapper=" + this.aimp_encode_bridge_value(_loc5_));
         }
         catch(error:Error)
         {
            this.aimp_set_remote_state_status("Cathode command failed\nCallback: " + _loc3_ + "\nError: " + error.message);
            this.aimp_log_bridge("error","cathode_command_error","ExternalInterface call failed: " + error.message,_loc8_,_loc3_,_loc2_,_loc4_,_loc9_,_loc5_);
            this.aimp_send_bridge_message("CATHODE_COMMAND_ACK|source=pickup_overlay|phase=gameplay|result=error|requestId=" + this.aimp_encode_bridge_value(_loc8_) + "|callback=" + this.aimp_encode_bridge_value(_loc3_) + "|kind=" + this.aimp_encode_bridge_value(_loc2_) + "|value=" + this.aimp_encode_bridge_value(_loc4_) + "|sentValue=" + this.aimp_encode_bridge_value(_loc9_) + "|wrapper=" + this.aimp_encode_bridge_value(_loc5_) + "|message=" + this.aimp_encode_bridge_value(error.message));
         }
      }
      
      private function aimp_create_remote_state_display() : void
      {
         if(this.aimp_remote_state_text != null)
         {
            try
            {
               if(this.aimp_remote_state_text.parent != null)
               {
                  this.aimp_remote_state_text.parent.removeChild(this.aimp_remote_state_text);
               }
            }
            catch(error:Error)
            {
            }
            this.aimp_remote_state_text = null;
         }
      }
      
      private function aimp_set_remote_state_status(param1:String) : void
      {
         trace("AIMP remote state: " + param1);
      }
      
      private function aimp_update_remote_state(param1:String) : void
      {
         var _loc2_:String = this.aimp_get_bridge_value(param1,"playerId");
         var _loc4_:String = this.aimp_get_bridge_value(param1,"tick");
         trace("AIMP remote state received: playerId=" + _loc2_ + " tick=" + _loc4_);
         if(!this.aimp_remote_state_ack_sent)
         {
            this.aimp_remote_state_ack_sent = true;
            this.aimp_send_bridge_message("REMOTE_PLAYER_STATE_ACK|source=pickup_overlay|phase=gameplay|playerId=" + _loc2_ + "|tick=" + _loc4_);
         }
      }
      
      private function aimp_get_bridge_value(param1:String, param2:String) : String
      {
         var _loc3_:Array = param1.split("|");
         var _loc4_:int = 1;
         var _loc5_:String = null;
         var _loc6_:int = 0;
         var _loc7_:String = null;
         while(_loc4_ < _loc3_.length)
         {
            _loc5_ = String(_loc3_[_loc4_]);
            _loc6_ = _loc5_.indexOf("=");
            if(_loc6_ > 0 && _loc5_.substr(0,_loc6_) == param2)
            {
               _loc7_ = _loc5_.substr(_loc6_ + 1);
               return this.aimp_decode_bridge_value(_loc7_);
            }
            _loc4_++;
         }
         return "";
      }
      
      private function aimp_decode_bridge_value(param1:String) : String
      {
         if(param1 == null)
         {
            return "";
         }
         return param1.split("%0D").join("\r").split("%0A").join("\n").split("%3D").join("=").split("%7C").join("|").split("%25").join("%");
      }
      
      private function aimp_encode_bridge_value(param1:String) : String
      {
         if(param1 == null)
         {
            return "";
         }
         return param1.split("%").join("%25").split("|").join("%7C").split("=").join("%3D").split("\n").join("%0A").split("\r").join("%0D");
      }
      
      public function AIMP_CathodeEvent(... param1) : void
      {
         this.aimp_send_cathode_event_to_launcher("AIMP_CathodeEvent",param1);
      }
      
      public function AIMP_CathodeInt(... param1) : void
      {
         this.aimp_send_cathode_event_to_launcher("AIMP_CathodeInt",param1);
      }
      
      public function AIMP_CathodeFloat(... param1) : void
      {
         this.aimp_send_cathode_event_to_launcher("AIMP_CathodeFloat",param1);
      }
      
      public function AIMP_CathodeVector3(... param1) : void
      {
         this.aimp_send_cathode_event_to_launcher("AIMP_CathodeVector3",param1);
      }
      
      public function AIMP_CathodePing(... param1) : void
      {
         this.aimp_send_cathode_event_to_launcher("AIMP_CathodePing",param1);
      }
      
      private function aimp_send_cathode_event_to_launcher(param1:String, param2:Array) : void
      {
         var _loc5_:String = null;
         var _loc6_:String = null;
         var _loc7_:int = 0;
         if(param2 == null)
         {
            param2 = [];
         }
         var _loc3_:String = "AIMP_CATHODE_EVENT|source=pickup_overlay|phase=gameplay";
         _loc3_ += "|method=" + this.aimp_encode_bridge_value(param1);
         _loc3_ += "|argc=" + String(param2.length);
         _loc7_ = 0;
         while(_loc7_ < param2.length && _loc7_ < 8)
         {
            _loc5_ = "arg" + String(_loc7_);
            _loc6_ = String(param2[_loc7_]);
            _loc3_ += "|" + _loc5_ + "=" + this.aimp_encode_bridge_value(_loc6_);
            _loc7_++;
         }
         var _loc4_:String = param2.length > 0 ? String(param2[0]) : "";
         _loc3_ += "|eventId=" + this.aimp_encode_bridge_value(_loc4_);
         if(param2.length > 1)
         {
            _loc3_ += "|value=" + this.aimp_encode_bridge_value(String(param2[1]));
         }
         this.aimp_send_bridge_message(_loc3_);
         this.aimp_set_remote_state_status("Cathode event sent to launcher\nMethod: " + param1 + "\nEventId: " + _loc4_);
      }
      
      private function aimp_on_bridge_io_error(param1:IOErrorEvent) : void
      {
         if(param1.currentTarget == this.aimp_bridge_socket)
         {
            this.aimp_bridge_error_pending = true;
         }
      }
      
      private function aimp_on_bridge_security_error(param1:SecurityErrorEvent) : void
      {
         if(param1.currentTarget == this.aimp_bridge_socket)
         {
            this.aimp_bridge_error_pending = true;
         }
      }

      private function aimp_on_bridge_close(param1:Event) : void
      {
         if(param1.currentTarget == this.aimp_bridge_socket)
         {
            this.aimp_bridge_error_pending = true;
         }
      }
      
      public function SetExpandedUIButtons(param1:String, param2:String) : *
      {
      }
      
      public function CollectAnimFinished() : *
      {
         var _loc2_:MovieClip = null;
         var _loc1_:int = 0;
         while(_loc1_ < this.NUM_CONTAINER_SLOTS)
         {
            if(this.m_container_contents[_loc1_] == "")
            {
               _loc2_ = this.container_mc.getChildByName("item" + _loc1_ + "_mc") as MovieClip;
               _loc2_.ContainerItemText_mc.txt.text = "AI_UI_EMPTY";
               _loc2_.gotoAndStop("empty");
            }
            _loc1_++;
         }
         this.HighlightCurrentContainerSlot();
      }
      
      private function UnhighlightCurrentContainerSlot() : *
      {
         var _loc1_:MovieClip = null;
         if(this.m_container_contents[this.m_container_selection] != "")
         {
            _loc1_ = this.container_mc.getChildByName("item" + this.m_container_selection + "_mc") as MovieClip;
            _loc1_.gotoAndStop("Deselected");
         }
      }
      
      private function HighlightCurrentContainerSlot() : *
      {
         var _loc1_:MovieClip = null;
         if(this.m_container_contents[this.m_container_selection] != "")
         {
            _loc1_ = this.container_mc.getChildByName("item" + this.m_container_selection + "_mc") as MovieClip;
            _loc1_.gotoAndPlay("Selected");
         }
      }
      
      public function PlayInventoryFullAnimFromIndex(param1:Number) : *
      {
         var _loc2_:MovieClip = null;
         _loc2_ = this.container_mc.getChildByName("item" + param1 + "_mc") as MovieClip;
         trace("m_container_selection is: " + param1);
         _loc2_.gotoAndPlay("InventoryFull");
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Inventory_Full");
         }
      }
      
      public function PlayTakeItemAnimFromIndex(param1:Number) : *
      {
         var _loc2_:MovieClip = null;
         this.m_container_contents[param1] = "";
         _loc2_ = this.container_mc.getChildByName("item" + param1 + "_mc") as MovieClip;
         _loc2_.gotoAndPlay("Collected");
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Collect");
         }
         this.MoveCursorDown();
      }
      
      public function PlayInventoryFullAnim() : *
      {
         var _loc1_:MovieClip = null;
         _loc1_ = this.container_mc.getChildByName("item" + this.m_container_selection + "_mc") as MovieClip;
         trace("m_container_selection is: " + this.m_container_selection);
         _loc1_.gotoAndPlay("InventoryFull");
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Inventory_Full");
         }
      }
      
      public function PlayTakeItemAnim() : *
      {
         var _loc1_:MovieClip = null;
         this.m_container_contents[this.m_container_selection] = "";
         _loc1_ = this.container_mc.getChildByName("item" + this.m_container_selection + "_mc") as MovieClip;
         _loc1_.gotoAndPlay("Collected");
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Collect");
         }
         this.MoveCursorDown();
      }
      
      private function HandleContainerInput(param1:*) : *
      {
         switch(param1)
         {
            case ButtonCode.A:
               if(this.m_container_contents[this.m_container_selection] == "")
               {
                  break;
               }
               if(ExternalInterface.available)
               {
                  ExternalInterface.call("UseContainer",this.m_container_selection);
               }
               break;
            case ButtonCode.B:
               if(ExternalInterface.available)
               {
                  ExternalInterface.call("CancelContainer",0);
               }
               break;
            case ButtonCode.PLAYER_RELOAD:
               if(ExternalInterface.available)
               {
                  if(this.m_container_contents[0] != "")
                  {
                     ExternalInterface.call("UseContainer",0);
                  }
                  if(this.m_container_contents[1] != "")
                  {
                     ExternalInterface.call("UseContainer",1);
                  }
                  if(this.m_container_contents[2] != "")
                  {
                     ExternalInterface.call("UseContainer",2);
                  }
               }
               break;
            case ButtonCode.LSTICK_UP:
            case ButtonCode.UP:
               this.MoveCursorUp();
               break;
            case ButtonCode.LSTICK_DOWN:
            case ButtonCode.DOWN:
               this.MoveCursorDown();
         }
      }
      
      private function KeyDownList(param1:KeyboardEvent) : *
      {
         this.HandleInput(param1.keyCode);
      }
      
      public function HandleInput(param1:*) : *
      {
         if(this.aimp_loading_layer != null) return;
         switch(this.m_state)
         {
            case this.STATE_CONTAINER:
               this.HandleContainerInput(param1);
         }
      }
      
      public function display_breathing_game_tutorial() : *
      {
         this.breathingtutorial_mc.gotoAndStop("visible");
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.text = "AI_UI_TUTORIAL_BREATHING_TUTORIAL_1";
         this.breathingtutorial_mc.breathing_tutorial_mc.txt_msg.textColor = 16777215;
      }
      
      public function hide_breathing_game_tutorial() : *
      {
         this.breathingtutorial_mc.gotoAndPlay("hidden");
      }
      
      public function MoveCursorUp() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Move_Cursor");
         }
         this.UnhighlightCurrentContainerSlot();
         var _loc1_:int = 0;
         do
         {
            --this.m_container_selection;
            if(this.m_container_selection < 0)
            {
               this.m_container_selection = this.NUM_CONTAINER_SLOTS - 1;
            }
            _loc1_++;
         }
         while(this.m_container_contents[this.m_container_selection] == "" && _loc1_ < this.NUM_CONTAINER_SLOTS);
         
         this.HighlightCurrentContainerSlot();
      }
      
      public function MoveCursorDown() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Move_Cursor");
         }
         this.UnhighlightCurrentContainerSlot();
         var _loc1_:int = 0;
         do
         {
            ++this.m_container_selection;
            if(this.m_container_selection == this.NUM_CONTAINER_SLOTS)
            {
               this.m_container_selection = 0;
            }
            _loc1_++;
         }
         while(this.m_container_contents[this.m_container_selection] == "" && _loc1_ < this.NUM_CONTAINER_SLOTS);
         
         this.HighlightCurrentContainerSlot();
      }
      
      public function SetIsActive(param1:int, param2:Boolean) : *
      {
         if(param2)
         {
            this.staticPrompt_mc.mc.staticPromptGlyph_mc.visible = false;
            this.staticPrompt_mc.mc.glyph_txt.visible = true;
            this.staticPrompt_mc.mc.txt.textColor = 16777215;
         }
         else
         {
            this.staticPrompt_mc.mc.staticPromptGlyph_mc.visible = true;
            this.staticPrompt_mc.mc.staticPromptGlyph_mc.gotoAndStop("Locked");
            this.staticPrompt_mc.mc.glyph_txt.visible = false;
            this.staticPrompt_mc.mc.txt.textColor = 8947848;
         }
      }
      
      public function Set_Interaction_Prompt_Visible() : *
      {
         this.staticPrompt_mc.visible = true;
      }
      
      public function Set_Interaction_Prompt_Invisible() : *
      {
         this.staticPrompt_mc.visible = false;
         this.staticPrompt_mc.gotoAndPlay("off");
      }
      
      public function Set_Interaction_Prompt_Invisible_no_anim() : *
      {
         this.staticPrompt_mc.visible = false;
      }
      
      public function Hide_Audio_Pickup_for_Pause() : *
      {
         this.audiolog_popup_mc.visible = false;
      }
      
      public function Show_Audio_Pickup_for_Pause() : *
      {
         this.audiolog_popup_mc.visible = true;
      }
      
      public function Hide_Container_for_Pause() : *
      {
         this.container_mc.visible = false;
      }
      
      public function Show_Container_for_Pause() : *
      {
         this.container_mc.visible = true;
      }
      
      public function Hide_Grenade_Prompt_for_Pause() : *
      {
         this.holdThrowPrompt_mc.visible = false;
      }
      
      public function Show_Grenade_Prompt_for_Pause() : *
      {
         this.holdThrowPrompt_mc.visible = true;
      }
      
      public function Configure_quantity(param1:int, param2:String) : *
      {
         var _loc3_:String = param2;
         this.staticPrompt_mc.mc.txt.text = _loc3_;
         this.staticPrompt_mc.mc.bg_mc.width = 30 + this.staticPrompt_mc.mc.txt.textWidth + 35;
      }
      
      public function Configure(param1:int, param2:String, param3:String, param4:Number = 0, param5:Boolean = false) : *
      {
         var _loc6_:String = param2;
         this.m_inventory_currently_full = false;
         this.staticPrompt_mc.gotoAndPlay("trans_in");
         this.staticPrompt_mc.mc.txt.text = _loc6_;
         this.staticPrompt_mc.mc.bg_mc.width = 30 + this.staticPrompt_mc.mc.txt.textWidth + 35;
      }
      
      public function Show(param1:int) : *
      {
      }
      
      public function FlashInventoryFull() : *
      {
         if(this.staticPrompt_mc.mc_inventoryfull)
         {
            this.staticPrompt_mc.mc_inventoryfull.gotoAndPlay("flash");
         }
      }
      
      public function Set_Inventory_Full(param1:Boolean) : *
      {
         this.m_inventory_currently_full = param1;
         var _loc2_:String = "";
         if(ExternalInterface.available)
         {
            _loc2_ = "AI_UI_INVENTORY_FULL";
         }
         this.staticPrompt_mc.mc_prompt_inventoryfull.mc_inventoryfull.mc_inventory_full_tween.txt_inventory_full_msg.text = _loc2_;
         this.staticPrompt_mc.mc_prompt_inventoryfull.mc_inventoryfull.visible = param1;
      }
      
      public function Expand(param1:int) : *
      {
         this.Set_Inventory_Full(this.m_inventory_currently_full);
         this.staticPrompt_mc.gotoAndPlay("trans_in");
      }
      
      public function Collect(param1:int) : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Pick_Up_Item");
         }
      }
      
      public function Shrink(param1:int) : *
      {
         this.staticPrompt_mc.gotoAndPlay("trans_out");
      }
      
      public function Hide(param1:int) : *
      {
      }
      
      public function SetIconPosition(param1:int, param2:Number, param3:Number) : *
      {
      }
      
      private function HideGrenadePromptFullInventoryAndSpinner() : void
      {
         this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.gotoAndStop("off");
         this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.visible = false;
      }
      
      public function ExpandGrenadePrompt(param1:String) : void
      {
         this.HideGrenadePromptFullInventoryAndSpinner();
         this.holdThrowPrompt_mc.mc.nested_mc.txt.multiline = false;
         this.holdThrowPrompt_mc.mc.nested_mc.txt.text = param1;
         this.holdThrowPrompt_mc.gotoAndPlay("trans_in");
         this.holdThrowPrompt_mc.mc.bg_mc.width = this.holdThrowPrompt_mc.mc.nested_mc.txt.textWidth + this.holdThrowPrompt_mc.mc.nested_mc.txt.textWidth / 8;
      }
      
      public function ShowAndSetGrenadePromptSpinner(param1:int) : void
      {
         var _loc2_:Boolean = this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.visible == false && param1 > 0;
         if(_loc2_)
         {
            this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.visible = true;
            this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.gotoAndPlay("trans_in");
         }
         var _loc3_:MovieClip = this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.spinner_mc;
         var _loc4_:Number = _loc3_.totalFrames;
         var _loc5_:Number = param1 / 100;
         var _loc6_:* = Math.round(_loc4_ * _loc5_);
         _loc3_.gotoAndStop(_loc6_);
      }
      
      public function HideGrenadePromptSpinnerForSuccess() : void
      {
         this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.gotoAndPlay("trans_out");
      }
      
      public function HideGrenadePromptSpinnerForCancel() : void
      {
         this.holdThrowPrompt_mc.mc_prompt_spinner.mc_spinner.gotoAndPlay("trans_out_cancel");
      }
      
      public function ShrinkGrenadePrompt() : void
      {
         this.holdThrowPrompt_mc.gotoAndPlay("trans_out");
      }
      
      public function ClearContainerItems() : *
      {
         var _loc2_:MovieClip = null;
         var _loc1_:int = 0;
         while(_loc1_ < this.NUM_CONTAINER_SLOTS)
         {
            _loc2_ = this.container_mc.getChildByName("item" + _loc1_ + "_mc") as MovieClip;
            _loc2_.gotoAndStop("empty");
            _loc2_.ContainerItemText_mc.txt.text = "AI_UI_EMPTY";
            _loc1_++;
         }
      }
      
      public function SetContainerItem(param1:int, param2:String, param3:String, param4:Boolean, param5:int, param6:int) : *
      {
         var _loc8_:String = null;
         var _loc9_:String = null;
         this.m_container_contents[param1] = param2;
         this.roomForContainerItem[param1] = param4;
         trace("SetContainerItem:" + param2);
         trace("index:" + param1);
         trace("item_name:" + param2);
         var _loc7_:MovieClip = this.container_mc.getChildByName("item" + param1 + "_mc") as MovieClip;
         if(param2 == "")
         {
            _loc7_.gotoAndStop("empty");
            _loc7_.ContainerItemText_mc.txt.text = "AI_UI_EMPTY";
         }
         else
         {
            _loc7_.gotoAndStop("deselected");
            if(param6)
            {
               if(ExternalInterface.available)
               {
                  _loc9_ = param3;
                  _loc8_ = String(ExternalInterface.call("TranslateString",_loc9_));
                  _loc7_.ContainerItemText_mc.txt.text = _loc8_ + " (" + param6 + ")";
               }
            }
            else
            {
               _loc7_.ContainerItemText_mc.txt.text = param3;
            }
         }
         this.m_size_icon[param1] = this.size_keyframes[param5];
      }
      
      public function ShowContainer() : *
      {
         var _loc1_:MovieClip = null;
         this.m_state = this.STATE_CONTAINER;
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Open");
         }
         this.container_mc.gotoAndPlay("trans_in");
         var _loc2_:Boolean = false;
         var _loc3_:int = 0;
         while(_loc3_ < this.NUM_CONTAINER_SLOTS)
         {
            if(this.m_container_contents[_loc3_] != "")
            {
               trace("container contents " + _loc3_);
               trace("name " + this.m_container_contents[_loc3_]);
               this.m_container_selection = _loc3_;
               break;
            }
            _loc2_ = true;
            _loc3_++;
         }
         if(!_loc2_)
         {
            _loc1_ = this.container_mc.getChildByName("item" + this.m_container_selection + "_mc") as MovieClip;
            _loc1_.gotoAndStop("Deselected");
            this.HighlightCurrentContainerSlot();
         }
         else
         {
            this.start_exit_timer();
         }
      }
      
      public function start_exit_timer() : *
      {
         this.container_hide_timer.start();
      }
      
      public function end_exit_timer(param1:TimerEvent) : *
      {
         this.container_hide_timer.stop();
         if(ExternalInterface.available)
         {
            ExternalInterface.call("CancelContainer",0);
         }
      }
      
      public function HideContainer() : *
      {
         this.m_state = this.STATE_OFF;
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Close");
         }
         this.container_mc.gotoAndPlay("trans_out");
      }
      
      public function ShowKeypad(param1:int, param2:int, param3:int, param4:int, param5:Boolean) : *
      {
         this.m_exit_on_fail = param5;
         this.m_state = this.STATE_KEYPAD;
         ExternalInterface.call("RegisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_LSTICK_RADIAL);
         this.m_keypad_code = "" + param1;
         this.m_keypad_code += param2;
         this.m_keypad_code += param3;
         this.m_keypad_code += param4;
         this.m_current_code_entered = "";
         UnhighlightCurrentKeypadButton();
         this.m_keypad_selection = -1;
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","UI_Keypad_Start");
         }
      }
      
      public function SetControllerPrompt(param1:int, param2:String, param3:String) : *
      {
      }
      
      public function hide_objective_message() : *
      {
         this.scriptedMessage_mc.visible = false;
      }
      
      public function unhide_objective_message() : *
      {
         this.scriptedMessage_mc.visible = true;
      }
      
      public function ShowMessage(param1:String, param2:String, param3:String, param4:String) : *
      {
         trace("");
         trace("Show message triggered:");
         trace("header " + param1);
         trace("title " + param2);
         trace("description " + param3);
         trace("icon " + param4);
         trace("");
         if(param3 == "")
         {
            this.m_current_message_mc = this.scriptedMessage_mc;
            this.m_current_message_mc.message_mc.messageContent_txt.text = param2;
         }
         else
         {
            this.m_current_message_mc = pickupMessage_mc;
            this.m_current_message_mc.message_mc.messageContent_txt.text = "TEXT_" + param2;
            pickupMessage_mc.CeremonyMessageDescrip_mc.ceremonyMessageDescrip_txt.text = param3;
         }
         if(param2 == "")
         {
            this.m_current_message_mc.messageBG_mc.visible = false;
         }
         else
         {
            this.m_current_message_mc.messageBG_mc.visible = true;
         }
         this.m_current_message_mc.messageHeading_mc.SetText(param1);
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Objective_Update");
         }
         this.m_current_message_mc.gotoAndPlay("Transition_In");
      }
      
      public function HideMessage() : *
      {
         if(this.m_current_message_mc.messageBG_mc.visible)
         {
            this.m_current_message_mc.gotoAndPlay("Transition_Out");
         }
      }
      
      public function ShowPickupCursor() : *
      {
         if(!this.m_cursor_fade_active)
         {
            addEventListener(Event.ENTER_FRAME,this.CursorFadePerFrame);
            this.m_cursor_fade_active = true;
         }
         this.m_cursor_target_alpha = this.MAX_PICKUP_CURSOR_ALPHA;
      }
      
      public function HidePickupCursor() : *
      {
         if(!this.m_cursor_fade_active)
         {
            addEventListener(Event.ENTER_FRAME,this.CursorFadePerFrame);
            this.m_cursor_fade_active = true;
         }
         this.m_cursor_target_alpha = 0;
      }
      
      private function CursorFadePerFrame(param1:Event) : *
      {
         if(this.pickup_cursor_mc.alpha < this.m_cursor_target_alpha)
         {
            this.pickup_cursor_mc.alpha += this.CURSOR_FADE_PER_FRAME;
            if(this.pickup_cursor_mc.alpha > this.m_cursor_target_alpha)
            {
               this.pickup_cursor_mc.alpha = this.m_cursor_target_alpha;
            }
         }
         else if(this.pickup_cursor_mc.alpha > this.m_cursor_target_alpha)
         {
            this.pickup_cursor_mc.alpha -= this.CURSOR_FADE_PER_FRAME;
            if(this.pickup_cursor_mc.alpha < this.m_cursor_target_alpha)
            {
               this.pickup_cursor_mc.alpha = this.m_cursor_target_alpha;
            }
         }
         if(this.pickup_cursor_mc.alpha == this.m_cursor_target_alpha)
         {
            removeEventListener(Event.ENTER_FRAME,this.CursorFadePerFrame);
            this.m_cursor_fade_active = false;
         }
      }
      
      public function ShowRespawnDialog() : *
      {
         respawn_mc.gotoAndPlay("trans_in");
      }
      
      public function HideRespawnDialog() : *
      {
         respawn_mc.gotoAndPlay("trans_out");
      }
      
      public function SetRespawnTime(param1:Number) : *
      {
         if(param1 > 0)
         {
            respawn_mc.mc.respawnCounter_txt.text = "RESPAWN IN " + Math.ceil(param1) + " SECONDS";
            respawn_mc.mc.a_prompt_mc.alpha = 0.2;
         }
         else
         {
            respawn_mc.mc.respawnCounter_txt.text = "READY TO RESPAWN";
            respawn_mc.mc.a_prompt_mc.alpha = 1;
         }
      }
      
      public function showUpgradeNotification(param1:int) : *
      {
         this.upgradeNotifier_mc.visible = true;
         this.upgradeNotifier_mc.gotoAndPlay("trans_in");
         if(param1 == 0)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_0_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_0_DESC";
         }
         else if(param1 == 1)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_1_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_1_DESC";
         }
         else if(param1 == 2)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_2_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_2_DESC";
         }
         else if(param1 == 3)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_3_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_3_DESC";
         }
         else if(param1 == 4)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_4_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_4_DESC";
         }
         else if(param1 == 5)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_5_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_5_DESC";
         }
         else if(param1 == 6)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_6_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_6_DESC";
         }
         else if(param1 == 7)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_7_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_7_DESC";
         }
         else if(param1 == 8)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_8_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_8_DESC";
         }
         else if(param1 == 9)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_9_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_9_DESC";
         }
         else if(param1 == 10)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_10_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_10_DESC";
         }
         else if(param1 == 11)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_11_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_11_DESC";
         }
         else if(param1 == 12)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_12_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_12_DESC";
         }
         else if(param1 == 13)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_13_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "";
         }
         else if(param1 == 14)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_14_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "AI_UI_COLLECT_POPUP_14_DESC";
         }
         else if(param1 == 15)
         {
            this.upgradeNotifier_mc.text_mc.headertext.text = "AI_UI_COLLECT_POPUP_15_TITLE";
            this.upgradeNotifier_mc.text_mc.sub.text = "";
         }
      }
      
      internal function frame1() : *
      {
         stop();
      }
   }
}
