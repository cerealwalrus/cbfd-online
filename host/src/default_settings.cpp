// The settings a new player starts with: the launcher's settings tabs as the game's author has them. They are only defaults: a
// setting the player has changed (saved in the tab's json in the config folder) is never touched. conker_config.cpp's seed_defaults
// puts these into the tabs' files, before the tabs load them: each flat tab file gets the keys it doesn't have yet, and the controls
// file is written when there is none (0.10.19: the author's files of 25 October 2026, the Online tab too). To change what new players get, replace the text of a tab here (a copy of its json).

namespace conker {
    namespace default_settings {
        extern const char* const general_json;
        const char* const general_json = R"CONKERJSON(
{
 "aim_stick_sensitivity": 100,
 "background_input_mode": "On",
 "debug_mode": false,
 "joystick_deadzone": 5,
 "launcher_window_mode": "Windowed",
 "launcher_windowed_size": "FillScreen",
 "online_chat_key": "Y"
}
)CONKERJSON";

        extern const char* const graphics_json;
        const char* const graphics_json = R"CONKERJSON(
{
 "api_option": "Auto",
 "ar_option": "Expand",
 "cutscene_aspect": "Expand",
 "developer_mode": false,
 "ds_option": 0,
 "hpfb_option": "Off",
 "hr_option": "Expand",
 "msaa_option": "None",
 "res_option": "Auto",
 "rr_manual_value": 60,
 "rr_option": "Original",
 "show_fps": "Off",
 "texture_pack": "ModsMenu",
 "vsync": "Off",
 "wm_option": "Windowed"
}
)CONKERJSON";

        extern const char* const camera_json;
        const char* const camera_json = R"CONKERJSON(
{
 "aim_swap_sticks": "On",
 "camera_invert_turning": "None",
 "camera_turn_speed": 100,
 "look_stick_invert": "None",
 "look_stick_response": "Direct",
 "stick_free_camera": "On"
}
)CONKERJSON";

        extern const char* const controls_json;
        const char* const controls_json = R"CONKERJSON(
{
 "controllers": [],
 "profiles": [
  {
   "custom": false,
   "device": "Keyboard",
   "key": "keyboard_sp",
   "mappings": {
    "A": [
     {
      "input_id": 44,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "ACCEPT_MENU": [
     {
      "input_id": 40,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "APPLY_MENU": [
     {
      "input_id": 9,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "B": [
     {
      "input_id": 225,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "BACK_MENU": [
     {
      "input_id": 106,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "C_DOWN": [
     {
      "input_id": 22,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "C_LEFT": [
     {
      "input_id": 4,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "C_RIGHT": [
     {
      "input_id": 7,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "C_UP": [
     {
      "input_id": 26,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_DOWN": [
     {
      "input_id": 14,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_LEFT": [
     {
      "input_id": 13,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_RIGHT": [
     {
      "input_id": 15,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_UP": [
     {
      "input_id": 12,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "L": [
     {
      "input_id": 8,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "R": [
     {
      "input_id": 21,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "START": [
     {
      "input_id": 40,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TAB_LEFT_MENU": [
     {
      "input_id": 107,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TAB_RIGHT_MENU": [
     {
      "input_id": 108,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TOGGLE_MENU": [
     {
      "input_id": 41,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "X_AXIS_NEG": [
     {
      "input_id": 80,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "X_AXIS_POS": [
     {
      "input_id": 79,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Y_AXIS_NEG": [
     {
      "input_id": 81,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Y_AXIS_POS": [
     {
      "input_id": 82,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Z": [
     {
      "input_id": 20,
      "input_type": 1
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ]
   },
   "name": "Keyboard (SP)"
  },
  {
   "custom": false,
   "device": "Controller",
   "key": "controller_sp",
   "mappings": {
    "A": [
     {
      "input_id": 0,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "ACCEPT_MENU": [
     {
      "input_id": 0,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "APPLY_MENU": [
     {
      "input_id": 3,
      "input_type": 3
     },
     {
      "input_id": 6,
      "input_type": 3
     }
    ],
    "B": [
     {
      "input_id": 2,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "BACK_MENU": [
     {
      "input_id": 2,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "C_DOWN": [
     {
      "input_id": 4,
      "input_type": 4
     },
     {
      "input_id": 10,
      "input_type": 3
     }
    ],
    "C_LEFT": [
     {
      "input_id": -3,
      "input_type": 4
     },
     {
      "input_id": 3,
      "input_type": 3
     }
    ],
    "C_RIGHT": [
     {
      "input_id": 3,
      "input_type": 4
     },
     {
      "input_id": 1,
      "input_type": 3
     }
    ],
    "C_UP": [
     {
      "input_id": -4,
      "input_type": 4
     },
     {
      "input_id": 8,
      "input_type": 3
     }
    ],
    "DPAD_DOWN": [
     {
      "input_id": 12,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_LEFT": [
     {
      "input_id": 13,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_RIGHT": [
     {
      "input_id": 14,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "DPAD_UP": [
     {
      "input_id": 11,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "L": [
     {
      "input_id": 9,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "R": [
     {
      "input_id": 5,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "START": [
     {
      "input_id": 6,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TAB_LEFT_MENU": [
     {
      "input_id": 9,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TAB_RIGHT_MENU": [
     {
      "input_id": 10,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "TOGGLE_MENU": [
     {
      "input_id": 4,
      "input_type": 3
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "X_AXIS_NEG": [
     {
      "input_id": -1,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "X_AXIS_POS": [
     {
      "input_id": 1,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Y_AXIS_NEG": [
     {
      "input_id": 2,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Y_AXIS_POS": [
     {
      "input_id": -2,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ],
    "Z": [
     {
      "input_id": 6,
      "input_type": 4
     },
     {
      "input_id": 0,
      "input_type": 0
     }
    ]
   },
   "name": "Controller (SP)"
  }
 ],
 "version": 3
}
)CONKERJSON";


        extern const char* const online_json;
        const char* const online_json = R"CONKERJSON(
{
 "online_chat_key": "Y"
}
)CONKERJSON";
    }
}
