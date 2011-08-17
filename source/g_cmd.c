// Emacs style mode select -*- C -*- vi:sw=3 ts=3:
//----------------------------------------------------------------------------
//
// Copyright(C) 2000 James Haley
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//--------------------------------------------------------------------------
//
// Game console commands
//
// console commands controlling the game functions.
//
// By Simon Howard
//
//---------------------------------------------------------------------------

#include "z_zone.h"
#include "d_io.h"
#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "c_io.h"
#include "c_runcmd.h"
#include "c_net.h"
#include "f_wipe.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "mn_engin.h"
#include "mn_misc.h"  // haleyjd
#include "m_shots.h"
#include "m_random.h"
#include "m_syscfg.h"
#include "p_inter.h"
#include "p_setup.h"
#include "w_wad.h"
#include "w_levels.h"

#include "s_sound.h"  // haleyjd: restored exit sounds
#include "sounds.h"   // haleyjd: restored exit sounds
#include "p_partcl.h" // haleyjd: add particle event cmds
#include "d_gi.h"     // haleyjd: gamemode pertinent info
#include "e_player.h" // haleyjd: turbo cmd must alter playerclass info

// [CG] Added.
#include "cs_main.h"
#include "cs_demo.h"
#include "cs_wad.h"
#include "cl_main.h"
#include "sv_main.h"

extern void I_WaitVBL(int); // haleyjd: restored exit sounds

extern int automlook;
extern int invert_mouse;
extern int keylookspeed;

////////////////////////////////////////////////////////////////////////
//
// Game Commands
//

CONSOLE_COMMAND(i_exitwithmessage, 0)
{
   I_ExitWithMessage("%s\n", Console.args);
}

CONSOLE_COMMAND(i_fatalerror, 0)
{
   I_FatalError(I_ERR_KILL, "%s\n", QStrConstPtr(&Console.args));
}

CONSOLE_COMMAND(i_error, 0)
{
   I_Error("%s\n", QStrConstPtr(&Console.args));
}

CONSOLE_COMMAND(z_print, cf_hidden)
{
   Z_PrintZoneHeap();
}

CONSOLE_COMMAND(z_dumpcore, cf_hidden)
{
   Z_DumpCore();
}

CONSOLE_COMMAND(starttitle, cf_notnet)
{
   // haleyjd 04/18/03
   if(demorecording || singledemo)
      G_CheckDemoStatus();

   MN_ClearMenus();         // put menu away
   D_StartTitle();
}

CONSOLE_COMMAND(endgame, cf_notnet)
{
   C_SetConsole();
}

CONSOLE_COMMAND(pause, cf_server)
{
   sendpause = true;
}

//
// haleyjd: Restoration of original exit behavior
//
void G_QuitDoom(void)
{
   // haleyjd: re-added code for playing random sound before exit
   extern int snd_card;
   
   if((!netgame || demoplayback) && !nosfxparm && snd_card &&
      GameModeInfo->flags & GIF_HASEXITSOUNDS)
   {
      S_StartSound(NULL, GameModeInfo->exitSounds[(gametic>>2)&7]);
      I_WaitVBL(105);
   }
   
   exit(0);
}

CONSOLE_COMMAND(quit, 0)
{
   if(CS_CLIENT)
   {
      CL_Disconnect();
   }
   G_QuitDoom();
}

CONSOLE_COMMAND(animshot, 0)
{
   if(!Console.argc)
   {
      C_Printf(
         "animated screenshot.\n"
         "usage: animshot <frames>\n");
      return;
   }
   animscreenshot = QStrAtoi(&Console.argv[0]);
   C_InstaPopup();    // turn off console
}

CONSOLE_COMMAND(screenshot, 0)
{
   if(Console.cmdtype != c_typed)
      C_InstaPopup();
   G_ScreenShot();
}

// particle stuff

// haleyjd 05/18/02: changed particle vars
// to INT variables with choice of sprite, particle, or both

char *particle_choices[] = { "sprites", "particles", "both" };

VARIABLE_BOOLEAN(drawparticles, NULL, onoff);
CONSOLE_VARIABLE(draw_particles, drawparticles, 0) {}

VARIABLE_INT(bloodsplat_particle, NULL, 0, 2, particle_choices);
CONSOLE_VARIABLE(bloodsplattype, bloodsplat_particle, 0) {}

VARIABLE_INT(bulletpuff_particle, NULL, 0, 2, particle_choices);
CONSOLE_VARIABLE(bulletpufftype, bulletpuff_particle, 0) {}

VARIABLE_BOOLEAN(drawrockettrails, NULL, onoff);
CONSOLE_VARIABLE(rocket_trails, drawrockettrails, 0) {}

VARIABLE_BOOLEAN(drawgrenadetrails, NULL, onoff);
CONSOLE_VARIABLE(grenade_trails, drawgrenadetrails, 0) {}

VARIABLE_BOOLEAN(drawbfgcloud, NULL, onoff);
CONSOLE_VARIABLE(bfg_cloud, drawbfgcloud, 0) {}

// always mlook

VARIABLE_BOOLEAN(automlook, NULL,           onoff);
CONSOLE_VARIABLE(alwaysmlook, automlook, 0) {}

// invert mouse

VARIABLE_BOOLEAN(invert_mouse, NULL,        onoff);
CONSOLE_VARIABLE(invertmouse, invert_mouse, 0) {}

// horizontal mouse sensitivity

VARIABLE_INT(mouseSensitivity_horiz, NULL, 0, 1024, NULL);
CONSOLE_VARIABLE(sens_horiz, mouseSensitivity_horiz, 0) {}

// vertical mouse sensitivity

VARIABLE_INT(mouseSensitivity_vert, NULL, 0, 1024, NULL);
CONSOLE_VARIABLE(sens_vert, mouseSensitivity_vert, 0) {}

int mouseSensitivity_c;

// combined sensitivity (for old-style menu only)
VARIABLE_INT(mouseSensitivity_c, NULL, 0, 16, NULL);
CONSOLE_VARIABLE(sens_combined, mouseSensitivity_c, 0)
{
   mouseSensitivity_horiz = mouseSensitivity_vert = mouseSensitivity_c * 4;
}

// player bobbing -- haleyjd: altered to read default, use netcmd

VARIABLE_BOOLEAN(player_bobbing, &default_player_bobbing, onoff);
CONSOLE_NETVAR(bobbing, player_bobbing, cf_server, netcmd_bobbing)
{
   if(CS_CLIENT)
   {
      CL_SendPlayerScalarInfo(ci_bobbing);
   }
}

VARIABLE_BOOLEAN(doom_weapon_toggles, NULL, onoff);
CONSOLE_VARIABLE(doom_weapon_toggles, doom_weapon_toggles, 0)
{
   if(CS_CLIENT)
   {
      CL_SendPlayerScalarInfo(ci_weapon_toggle);
   }
}

boolean display_target_names = false;
boolean default_display_target_names = false;

// [CG] Display target names (if target is a player) beneath crosshair.
VARIABLE_BOOLEAN(display_target_names, &default_display_target_names, onoff);
CONSOLE_VARIABLE(display_target_names, display_target_names, 0) {}

// [CG] Weapon switching.

char *weapon_switch_choices[] = { "vanilla", "pwo", "never" };
char *ammo_switch_choices[] = { "vanilla", "pwo", "never" };

// [CG] Weapon switch on weapon pickup.

unsigned int weapon_switch_on_pickup = WEAPON_SWITCH_ALWAYS;
unsigned int default_weapon_switch_on_pickup = WEAPON_SWITCH_ALWAYS;

VARIABLE_INT(
   weapon_switch_on_pickup,
   &default_weapon_switch_on_pickup,
   WEAPON_SWITCH_ALWAYS,
   WEAPON_SWITCH_NEVER,
   weapon_switch_choices
);

CONSOLE_VARIABLE(weapon_switch_on_pickup, weapon_switch_on_pickup, 0)
{
   if(CS_CLIENT && net_peer != NULL)
   {
      CL_SendPlayerScalarInfo(ci_wsop);
   }
}

// [CG] Weapon switch on ammo pickup.

unsigned int ammo_switch_on_pickup = AMMO_SWITCH_VANILLA;
unsigned int default_ammo_switch_on_pickup = AMMO_SWITCH_VANILLA;

VARIABLE_INT(
   ammo_switch_on_pickup,
   &default_ammo_switch_on_pickup,
   AMMO_SWITCH_VANILLA,
   AMMO_SWITCH_DISABLED,
   ammo_switch_choices
);

CONSOLE_VARIABLE(ammo_switch_on_pickup, ammo_switch_on_pickup, 0)
{
   if(CS_CLIENT && net_peer != NULL)
   {
      CL_SendPlayerScalarInfo(ci_asop);
   }
}

// turbo scale

int turbo_scale = 100;
VARIABLE_INT(turbo_scale, NULL,         10, 400, NULL);
CONSOLE_VARIABLE(turbo, turbo_scale, 0)
{
   if(clientserver)
   {
      C_Printf("Cannot enable turbo in c/s mode.\n");
      return;
   }
   C_Printf("turbo scale: %i%%\n",turbo_scale);
   E_ApplyTurbo(turbo_scale);
}

CONSOLE_NETCMD(exitlevel, cf_server|cf_level, netcmd_exitlevel)
{
   // haleyjd 09/04/02: prevent exit if dead, unless comp flag on
   player_t *player = &players[Console.cmdsrc];

   if((player->health > 0) || comp[comp_zombie])
      G_ExitLevel();
}

//////////////////////////////////////
//
// C/S Demo Stuff
//

CONSOLE_COMMAND(playcsdemo, 0)
{
   if(!clientserver)
   {
      C_Printf("C/S mode only.\n");
      return;
   }

   if(Console.argc < 1)
   {
      C_Printf("Usage: playcsdemo demoname\n");
   }

   if(!CS_CLIENT)
   {
      C_Printf("Client-only for now.\n");
      return;
   }

   if(net_peer)
   {
      CL_Disconnect();
   }
   else if(cs_demo_playback || cs_demo_recording)
   {
      if(!CS_StopDemo())
      {
         C_Printf("Error stopping demo: %s.\n", CS_GetDemoErrorMessage());
      }
   }

   if(!CS_PlayDemo((char *)QStrConstPtr(&Console.argv[0])))
   {
      C_Printf("Error playing demo: %s.\n", CS_GetDemoErrorMessage());
   }
   else
   {
      C_Printf("Playing demo %s.\n", QStrConstPtr(&Console.argv[0]));
   }
}

CONSOLE_COMMAND(recordcsdemo, 0)
{
   nm_sync_t sync_packet;
   byte *buffer = NULL;
   size_t buffer_size;

   if(!clientserver)
   {
      C_Printf("C/S mode only.\n");
      return;
   }

   if(!CS_CLIENT)
   {
      C_Printf("Client-only for now.\n");
      return;
   }

   if(!net_peer)
   {
      C_Printf("Must be connected.\n");
      return;
   }

   if(cs_demo_recording)
   {
      C_Printf("Already recording.\n");
      return;
   }

   if(!CS_RecordDemo())
   {
      C_Printf("Error: %s.\n", CS_GetDemoErrorMessage());
      CS_StopDemo();
      return;
   }

   if(!CS_AddNewMapToDemo())
   {
      C_Printf("Error: %s.\n", CS_GetDemoErrorMessage());
      CS_StopDemo();
      return;
   }

   buffer_size = CS_BuildGameState(consoleplayer, &buffer);

   if(!CS_WriteNetworkMessageToDemo(buffer, buffer_size, 0))
   {
      C_Printf("Error: %s.\n", CS_GetDemoErrorMessage());
      free(buffer);
      CS_StopDemo();
      return;
   }

   free(buffer);

   sync_packet.message_type = nm_sync;
   if(CS_CLIENT)
   {
      sync_packet.world_index = cl_current_world_index;
   }
   else if(CS_SERVER)
   {
      sync_packet.world_index = sv_world_index;
   }
   sync_packet.gametic = gametic;
   sync_packet.levelstarttic = levelstarttic;
   sync_packet.basetic = basetic;
   sync_packet.leveltime = leveltime;

   if(!CS_WriteNetworkMessageToDemo(&sync_packet, sizeof(nm_sync_t), 0))
   {
      C_Printf("Error: %s.\n", CS_GetDemoErrorMessage());
      CS_StopDemo();
      return;
   }

   C_Printf("Recording new demo %s.ecd.\n", cs_demo_path);
}

CONSOLE_COMMAND(stopcsdemo, 0)
{
   if(!clientserver)
   {
      C_Printf("C/S mode only.\n");
      return;
   }

   if(!CS_CLIENT)
   {
      C_Printf("Client-only for now.\n");
      return;
   }

   if(cs_demo_playback || cs_demo_recording)
   {
      if(!CS_StopDemo())
      {
         C_Printf("Error stopping demo: %s.\n", CS_GetDemoErrorMessage());
      }
      else if(cs_demo_playback)
      {
         C_Printf("Demo playback stopped.\n");
      }
      else if(cs_demo_recording)
      {
         C_Printf("Demo recording stopped.\n");
      }
      else
      {
         C_Printf("Demo stopped.\n");
      }
   }
   else
   {
      C_Printf("No current demo.\n");
   }
}

//////////////////////////////////////
//
// Demo Stuff
//

CONSOLE_COMMAND(playdemo, cf_notnet)
{
   if(Console.argc < 1)
   {
      C_Printf("usage: playdemo demoname\n");
      return;
   }

   // haleyjd 02/15/10: check in both ns_demos and ns_global
   if(W_CheckNumForNameNSG(QStrConstPtr(&Console.argv[0]), ns_demos) < 0)
   {
      C_Printf(FC_ERROR "%s not found\n", Console.argv[0]);
      return;
   }
   
   G_DeferedPlayDemo(QStrConstPtr(&Console.argv[0]));
   singledemo = true;            // quit after one demo
}


CONSOLE_COMMAND(stopdemo, cf_notnet)
{
   G_StopDemo();
}

CONSOLE_COMMAND(timedemo, cf_notnet)
{
   if(Console.argc != 2)
   {
      C_Printf("usage: timedemo demoname showmenu\n");
      return;
   }
   G_TimeDemo(QStrConstPtr(&Console.argv[0]), !!QStrAtoi(&Console.argv[1]));
}

// 'cool' demo

VARIABLE_BOOLEAN(cooldemo, NULL,            onoff);
CONSOLE_VARIABLE(cooldemo, cooldemo, 0) {}

///////////////////////////////////////////////////
//
// Wads
//

// load new wad
// buffered command: r_init during load

CONSOLE_COMMAND(addfile, cf_notnet|cf_buffered)
{
   if(GameModeInfo->flags & GIF_SHAREWARE)
   {
      C_Printf(FC_ERROR"command not available in shareware games\n");
      return;
   }
   D_AddNewFile(QStrConstPtr(&Console.argv[0]));
}

// list loaded wads

CONSOLE_COMMAND(listwads, 0)
{
   D_ListWads();
}
                                     
// random seed

CONST_INT(rngseed);
CONSOLE_CONST(rngseed, rngseed);

// suicide

CONSOLE_NETCMD(kill, cf_level, netcmd_kill)
{
   mobj_t *mobj;
   int playernum;
   
   playernum = Console.cmdsrc;
   
   mobj = players[playernum].mo;
   P_DamageMobj(mobj, NULL, NULL,
                2*(players[playernum].health+players[playernum].armorpoints),
                MOD_SUICIDE);
   mobj->momx = mobj->momy = mobj->momz = 0;
   players[playernum].momx = players[playernum].momy = 0;
}

// [CG] Display map list
CONSOLE_COMMAND(maplist, 0)
{
   cs_map_t *map;
   unsigned int i, j;
   size_t line_length;
   char *buf = NULL;
   char *buf_index = NULL;

   if(!clientserver)
   {
      C_Printf("Maplists are not supported outside of c/s mode.\n");
      return;
   }

   for(i = 0; i < cs_map_count; i++)
   {
      map = &cs_maps[i];
      if(map->resource_count == 0)
      {
         C_Printf("%d. %s\n", i + 1, map->name);
         continue;
      }

      // [CG] Calculate the length of the line and allocate a buffer.  Be
      //      cautious and over-allocate (probably, unless there's 100,000 maps
      //      or something...).
      line_length = 10 + strlen(map->name);
      for(j = 0; j < map->resource_count; j++)
      {
         line_length += strlen(cs_resources[map->resource_indices[j]].name);
         if(j != (map->resource_count - 1))
            line_length += 2;
      }

      buf = buf_index = realloc(buf, line_length * sizeof(char));

      buf_index += sprintf(buf, "%d. %s: ", i + 1, map->name);
      for(j = 0; j < map->resource_count; j++)
      {
         buf_index += sprintf(buf_index, "%s",
            cs_resources[map->resource_indices[j]].name
         );
         if(j < (map->resource_count - 1))
            buf_index += sprintf(buf_index, ", ");
      }
      if(CS_HEADLESS)
         C_Printf("%s\n", buf);
      else
         C_Printf("%s", buf);
   }

   if(buf)
      free(buf);
}

// change level
CONSOLE_NETCMD(map, cf_server, netcmd_map)
{
   int lumpnum;

   if(clientserver)
   {
      int map_number;

      if(CS_DEMO)
      {
         // [CG] Clients may issue the map command while recording a demo, but
         //      we don't want to honor that command while playing the demo
         //      back.  To avoid an error message, silently quit without doing
         //      anything here.
         return;
      }

      if(!CS_SERVER)
      {
         C_Printf("C/S server mode only.\n");
         return;
      }

      if(!Console.argc || !QStrIsNum(&Console.argv[0]))
      {
         C_Printf("usage: map <map number>\n");
         return;
      }
      map_number = QStrAtoi(&Console.argv[0]) - 1;
      if(map_number < 0 || map_number >= cs_map_count)
      {
         C_Printf("Invalid map number '%d'.\n", map_number + 1);
         return;
      }

      if(CS_DEMO)
      {
         if(!CS_LoadDemoMap(map_number + 1))
         {
            C_Printf(
               "Error during demo playback: %s.\n", CS_GetDemoErrorMessage()
            );
            CS_StopDemo();
         }
      }
      else if(CS_SERVER)
      {
         cs_current_map_number = map_number;
         SV_BroadcastMapCompleted(false);
         G_DoCompleted(false);
         CS_DoWorldDone();
      }
      return;
   }

   if(!Console.argc)
   {
      C_Printf("usage: map <mapname>\n");
      C_Printf("    or map <wadfile.wad>\n");
      return;
   }

   // check for .wad files
   // i'm not particularly a fan of this myself, but..

   // haleyjd 03/12/06: no .wad loading in netgames
   
   if(!netgame && QStrLen(&Console.argv[0]) > 4)
   {
      const char *extension = QStrBufferAt(
         &Console.argv[0], QStrLen(&Console.argv[0]) - 4
      );
      if(!strcmp(extension, ".wad"))
      {
         if(D_AddNewFile(QStrConstPtr(&Console.argv[0])))
         {
            G_DeferedInitNew(gameskill, firstlevel);
         }
         return;
      }
   }

   // haleyjd 02/23/04: strict error checking
   lumpnum = W_CheckNumForName(QStrConstPtr(&Console.argv[0]));
   if(lumpnum != -1 &&
      P_CheckLevel(&w_GlobalDir, lumpnum) != LEVEL_FORMAT_INVALID)
   {   
      G_DeferedInitNew(gameskill, QStrConstPtr(&Console.argv[0]));
   }
   else
   {
      C_Printf(
          FC_ERROR "%s not found or is not a valid map\n",
          QStrConstPtr(&Console.argv[0])
      );
   }
}

        // player name
VARIABLE_STRING(default_name, NULL,             20);
CONSOLE_NETVAR(name, default_name, cf_handlerset, netcmd_name)
{
   int playernum;

   if(Console.argc < 1)
      return;

   if(CS_SERVER)
   {
       // [CG] Servers can't change their names.
       return;
   }

   if(Console.argv[0].size <= 0)
   {
      C_Printf("Cannot blank your name.\n");
      return;
   }
   
   playernum = Console.cmdsrc;
   
   QStrCNCopy(players[playernum].name, &Console.argv[0], 20);

   if(playernum == consoleplayer)
   {
      free(default_name);
      default_name = QStrCDup(&Console.argv[0], PU_STATIC);
   }

   if(CS_CLIENT)
   {
      CL_SendPlayerStringInfo(ci_name);
   }
}

// screenshot type

char *str_pcx[] = { "bmp", "pcx", "tga" };
VARIABLE_INT(screenshot_pcx, NULL, 0, 2, str_pcx);
CONSOLE_VARIABLE(shot_type, screenshot_pcx, 0) {}

VARIABLE_BOOLEAN(screenshot_gamma, NULL, yesno);
CONSOLE_VARIABLE(shot_gamma, screenshot_gamma, 0) {}

// textmode startup

extern int textmode_startup;            // d_main.c
VARIABLE_BOOLEAN(textmode_startup, NULL,        onoff);
CONSOLE_VARIABLE(textmode_startup, textmode_startup, 0) {}

// demo insurance

extern int demo_insurance;
char *insure_str[]={"off", "on", "when recording"};
VARIABLE_INT(demo_insurance, &default_demo_insurance, 0, 2, insure_str);
CONSOLE_VARIABLE(demo_insurance, demo_insurance, cf_notnet) {}

extern int smooth_turning;
VARIABLE_BOOLEAN(smooth_turning, NULL,          onoff);
CONSOLE_VARIABLE(smooth_turning, smooth_turning, 0) {}


// SoM: mouse accel
char *accel_options[]={ "off", "linear", "choco" };
VARIABLE_INT(mouseAccel_type, NULL, 0, 2, accel_options);
CONSOLE_VARIABLE(mouse_accel, mouseAccel_type, 0) {}

VARIABLE_BOOLEAN(novert, NULL, onoff);
CONSOLE_VARIABLE(mouse_novert, novert, 0) {}

// haleyjd: new stuff

extern int map_point_coordinates;
VARIABLE_BOOLEAN(map_point_coordinates, NULL, onoff);
CONSOLE_VARIABLE(map_coords, map_point_coordinates, 0) {}

VARIABLE_INT(dogs, &default_dogs, 0, 3, NULL);
CONSOLE_VARIABLE(numhelpers, dogs, cf_notnet) {}

VARIABLE_BOOLEAN(dog_jumping, &default_dog_jumping, onoff);
CONSOLE_NETVAR(dogjumping, dog_jumping, cf_server, netcmd_dogjumping) {}

VARIABLE_BOOLEAN(startOnNewMap, NULL, yesno);
CONSOLE_VARIABLE(startonnewmap, startOnNewMap, 0) {}

VARIABLE_BOOLEAN(autorun, NULL, onoff);
CONSOLE_VARIABLE(autorun, autorun, 0) {}

VARIABLE_BOOLEAN(runiswalk, NULL, onoff);
CONSOLE_VARIABLE(runiswalk, runiswalk, 0) {}

// haleyjd 03/22/09: iwad cvars

VARIABLE_STRING(gi_path_doomsw,  NULL, UL);
VARIABLE_STRING(gi_path_doomreg, NULL, UL);
VARIABLE_STRING(gi_path_doomu,   NULL, UL);
VARIABLE_STRING(gi_path_doom2,   NULL, UL);
VARIABLE_STRING(gi_path_tnt,     NULL, UL);
VARIABLE_STRING(gi_path_plut,    NULL, UL);
VARIABLE_STRING(gi_path_hacx,    NULL, UL);
VARIABLE_STRING(gi_path_hticsw,  NULL, UL);
VARIABLE_STRING(gi_path_hticreg, NULL, UL);
VARIABLE_STRING(gi_path_sosr,    NULL, UL);

VARIABLE_STRING(w_masterlevelsdirname, NULL, UL);

static boolean G_TestIWADPath(char *path)
{
   M_NormalizeSlashes(path);

   // test for read access, and warn if non-existent
   if(access(path, R_OK))
   {
      if(menuactive)
         MN_ErrorMsg("Warning: cannot access filepath");
      else
         C_Printf(FC_ERROR "Warning: cannot access filepath\n");

      return false;
   }

   return true;
}

CONSOLE_VARIABLE(iwad_doom_shareware,    gi_path_doomsw,  0) 
{
   G_TestIWADPath(gi_path_doomsw);
}

CONSOLE_VARIABLE(iwad_doom,              gi_path_doomreg, 0) 
{
   G_TestIWADPath(gi_path_doomreg);
}

CONSOLE_VARIABLE(iwad_ultimate_doom,     gi_path_doomu,   0) 
{
   G_TestIWADPath(gi_path_doomu);
}

CONSOLE_VARIABLE(iwad_doom2,             gi_path_doom2,   0) 
{
   G_TestIWADPath(gi_path_doom2);
}

CONSOLE_VARIABLE(iwad_tnt,               gi_path_tnt,     0) 
{
   G_TestIWADPath(gi_path_tnt);
}

CONSOLE_VARIABLE(iwad_plutonia,          gi_path_plut,    0) 
{
   G_TestIWADPath(gi_path_plut);
}

CONSOLE_VARIABLE(iwad_hacx,              gi_path_hacx,    0)
{
   G_TestIWADPath(gi_path_hacx);
}

CONSOLE_VARIABLE(iwad_heretic_shareware, gi_path_hticsw,  0) 
{
   G_TestIWADPath(gi_path_hticsw);
}

CONSOLE_VARIABLE(iwad_heretic,           gi_path_hticreg, 0) 
{
   G_TestIWADPath(gi_path_hticreg);
}

CONSOLE_VARIABLE(iwad_heretic_sosr,      gi_path_sosr,    0) 
{
   G_TestIWADPath(gi_path_sosr);
}

CONSOLE_VARIABLE(master_levels_dir, w_masterlevelsdirname, 0)
{
   if(G_TestIWADPath(w_masterlevelsdirname))
      W_EnumerateMasterLevels(true);
}

VARIABLE_BOOLEAN(use_doom_config, NULL, yesno);
CONSOLE_VARIABLE(use_doom_config, use_doom_config, 0) {}

CONSOLE_COMMAND(m_resetcomments, 0)
{
   M_ResetDefaultComments();
   M_ResetSysComments();
}

////////////////////////////////////////////////////////////////
//
// Chat Macros
//

void G_AddChatMacros(void)
{
   int i;
   
   for(i=0; i<10; i++)
   {
      variable_t *variable;
      command_t *command;
      char tempstr[32];

      memset(tempstr, 0, 32);
      
      // create the variable first
      variable = malloc(sizeof(*variable));
      variable->variable = &chat_macros[i];
      variable->v_default = NULL;
      variable->type = vt_string;      // string value
      variable->min = 0;
      variable->max = 128;
      variable->defines = NULL;
      
      // now the command
      command = malloc(sizeof(*command));
      
      sprintf(tempstr, "chatmacro%i", i);
      command->name = strdup(tempstr);
      command->type = ct_variable;
      command->flags = 0;
      command->variable = variable;
      command->handler = NULL;
      command->netcmd = 0;
      
      (C_AddCommand)(command); // hook into cmdlist
   }
}

///////////////////////////////////////////////////////////////
//
// Weapon Prefs
//

extern int weapon_preferences[2][NUMWEAPONS+1];                   

void G_SetWeapPref(int prefnum, int newvalue)
{
   int i;
   
   // find the pref which has the new value
   
   for(i=0; i<NUMWEAPONS; i++)
      if(weapon_preferences[0][i] == newvalue) break;
      
   weapon_preferences[0][i] = weapon_preferences[0][prefnum];
   weapon_preferences[0][prefnum] = newvalue;
}

char *weapon_str[NUMWEAPONS] =
{"fist", "pistol", "shotgun", "chaingun", "rocket launcher", "plasma gun",
 "bfg", "chainsaw", "double shotgun"};

void G_WeapPrefHandler(void)
{
   int newvalue;

   if(Console.argc)
   {
      int prefnum = 
         (int *)(Console.command->variable->variable) - weapon_preferences[0];
      G_SetWeapPref(prefnum, QStrAtoi(&Console.argv[0]));

      if(CS_CLIENT && net_peer != NULL)
      {
         // [CG] C/S clients have to inform the server of their new weapon
         //      preferences so the server can assign the proper weapon when
         //      they pick up weapons and ammo, and when they exhaust
         //      ammunition.
         printf(
            "G_WeapPrefHandler: prefnum/weapon: %d/%d.\n",
            prefnum,
            QStrAtoi(&Console.argv[0])
         );
         CL_SendPlayerArrayInfo(ci_pwo, prefnum);
      }
   }
}

void G_AddWeapPrefs(void)
{
   int i;
   
   for(i=0; i<NUMWEAPONS; i++)   // haleyjd
   {
      variable_t *variable;
      command_t *command;
      char tempstr[16]; // haleyjd: increased size -- bug fix!

      memset(tempstr, 0, 16);
      
      // create the variable first
      variable = malloc(sizeof(*variable));
      variable->variable = &weapon_preferences[0][i];
      variable->v_default = NULL;
      variable->type = vt_int;
      variable->min = 1;
      variable->max = NUMWEAPONS; // haleyjd
      variable->defines = weapon_str;  // use weapon string defines

      // now the command
      command = malloc(sizeof(*command));

      sprintf(tempstr, "weappref_%i", i+1);
      command->name = strdup(tempstr);
      command->type = ct_variable;
      command->flags = cf_handlerset;
      command->variable = variable;
      command->handler = G_WeapPrefHandler;
      command->netcmd = 0;

      (C_AddCommand)(command); // hook into cmdlist
   }
}

///////////////////////////////////////////////////////////////
//
// Autoloaded Files
//
// haleyjd 03/13/06
//

static char *autoload_names[] =
{
   "auto_wad_1",
   "auto_wad_2",
   "auto_deh_1",
   "auto_deh_2",
   "auto_csc_1",
   "auto_csc_2",
};

extern char *wad_files[];
extern char *deh_files[];
extern char *csc_files[];

static char **autoload_ptrs[] =
{
   &wad_files[0],
   &wad_files[1],
   &deh_files[0],
   &deh_files[1],
   &csc_files[0],
   &csc_files[1],
};

void G_AddAutoloadFiles(void)
{
   variable_t *variable;
   command_t *command;
   int i;
   
   for(i = 0; i < 6; ++i)
   {
      // create the variable first
      variable = malloc(sizeof(*variable));
      variable->variable = autoload_ptrs[i];
      variable->v_default = NULL;
      variable->type = vt_string;
      variable->min = 0;
      variable->max = 1024;
      variable->defines = NULL;
      
      // now the command
      command = malloc(sizeof(*command));
      command->name = autoload_names[i];
      command->type = ct_variable;
      command->flags = 0;
      command->variable = variable;
      command->handler = NULL;
      command->netcmd = 0;

      (C_AddCommand)(command); // hook into cmdlist
   }
}

///////////////////////////////////////////////////////////////
//
// Compatibility vectors
//

// names given to cmds
const char *comp_strings[] =
{
  "telefrag",
  "dropoff",
  "vile",
  "pain",
  "skull",
  "blazing",
  "doorlight",
  "model",
  "god",
  "falloff",
  "floors",
  "skymap",
  "pursuit",
  "doorstuck",
  "staylift",
  "zombie",
  "stairs",
  "infcheat",
  "zerotags",
  "terrain",    // haleyjd: TerrainTypes
  "respawnfix", //          Nightmare respawn location fix
  "fallingdmg", //          Players take falling damage
  "soul",       //          Lost soul bouncing
  "theights",   //          Thing heights fix
  "overunder",  //          10/19/02: z checking
  "planeshoot", //          09/22/07: plane shooting
  "special",    //          08/29/09: special failure behavior
  "ninja",      //          04/18/10: ninja spawn
};

static void Handler_CompTHeights(void)
{
   P_ChangeThingHeights();
}

void G_AddCompat(void)
{
   int i;
   
   for(i = 0; i <= comp_ninja; i++)   // haleyjd: update this regularly
   {
      variable_t *variable;
      command_t *command;
      char tempstr[32];

      // create the variable first
      variable = malloc(sizeof(*variable));
      variable->variable = &comp[i];
      variable->v_default = &default_comp[i];
      variable->type = vt_int;      // string value
      variable->min = 0;
      variable->max = 1;
      variable->defines = yesno;
      
      // now the command
      command = malloc(sizeof(*command));
      
      psnprintf(tempstr, sizeof(tempstr), "comp_%s", comp_strings[i]);
      command->name = strdup(tempstr);
      command->type = ct_variable;

      switch(i)
      {
      case comp_theights:
         command->flags = cf_server | cf_netvar;
         command->handler = Handler_CompTHeights;
         break;
      default:
         command->flags = cf_server | cf_netvar;
         command->handler = NULL;
         break;
      }

      command->variable = variable;
      command->netcmd = netcmd_comp_0 + i;
      
      (C_AddCommand)(command); // hook into cmdlist
   }
}

void G_AddCommands(void)
{
   C_AddCommand(i_exitwithmessage);
   C_AddCommand(i_fatalerror);
   C_AddCommand(i_error);
   C_AddCommand(z_print);
   C_AddCommand(z_dumpcore);
   C_AddCommand(starttitle);
   C_AddCommand(endgame);
   C_AddCommand(pause);
   C_AddCommand(quit);
   C_AddCommand(animshot);
   C_AddCommand(screenshot);
   C_AddCommand(shot_type);
   C_AddCommand(shot_gamma);
   C_AddCommand(alwaysmlook);
   C_AddCommand(bobbing);
   C_AddCommand(doom_weapon_toggles);
   C_AddCommand(sens_vert);
   C_AddCommand(sens_horiz);
   C_AddCommand(sens_combined);
   C_AddCommand(mouse_accel);
   C_AddCommand(mouse_novert);
   C_AddCommand(invertmouse);
   C_AddCommand(turbo);
   C_AddCommand(playdemo);
   C_AddCommand(timedemo);
   C_AddCommand(cooldemo);
   C_AddCommand(stopdemo);
   C_AddCommand(exitlevel);
   C_AddCommand(addfile);
   C_AddCommand(listwads);
   C_AddCommand(rngseed);
   C_AddCommand(kill);
   C_AddCommand(map);
   C_AddCommand(name);
   C_AddCommand(textmode_startup);
   C_AddCommand(demo_insurance);
   C_AddCommand(smooth_turning);
   
   // haleyjd: new stuff
   C_AddCommand(map_coords);
   C_AddCommand(numhelpers);   
   C_AddCommand(dogjumping);
   C_AddCommand(draw_particles);
   C_AddCommand(bloodsplattype);
   C_AddCommand(bulletpufftype);
   C_AddCommand(rocket_trails);
   C_AddCommand(grenade_trails);
   C_AddCommand(bfg_cloud);
   C_AddCommand(startonnewmap);
   C_AddCommand(autorun);
   C_AddCommand(runiswalk);
   C_AddCommand(m_resetcomments);

   // haleyjd 03/22/09: iwad paths
   C_AddCommand(iwad_doom_shareware);
   C_AddCommand(iwad_doom);
   C_AddCommand(iwad_ultimate_doom);
   C_AddCommand(iwad_doom2);
   C_AddCommand(iwad_tnt);
   C_AddCommand(iwad_plutonia);
   C_AddCommand(iwad_hacx);
   C_AddCommand(iwad_heretic_shareware);
   C_AddCommand(iwad_heretic);
   C_AddCommand(iwad_heretic_sosr);
   C_AddCommand(master_levels_dir);
   C_AddCommand(use_doom_config);

   // [CG] More new stuff.
   C_AddCommand(maplist);
   C_AddCommand(weapon_switch_on_pickup);
   C_AddCommand(ammo_switch_on_pickup);
   C_AddCommand(display_target_names);
   C_AddCommand(playcsdemo);
   C_AddCommand(recordcsdemo);
   C_AddCommand(stopcsdemo);

   G_AddChatMacros();
   G_AddWeapPrefs();
   G_AddCompat();
   G_AddAutoloadFiles(); // haleyjd
   P_AddEventVars(); // haleyjd

}

// EOF