/*
 * Copyright (c) 2012 Toni Spets <toni.spets@iki.fi>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* Code revised by DD (2020) (v.0.2.0.2) */

#include <bass/bass.h>
#include <windows.h>
#include <winreg.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include "player.h"

#define MAGIC_DEVICEID 0xBEEF
#define MAX_TRACKS 99

/* BASS PLAYER DEFINES START */
HWND win;

HSTREAM *strs;
int strc;
HMUSIC *mods;
int modc;
HSAMPLE *sams;
int samc;

/* BASS PLAYER DEFINES END */

struct track_info
{
    char path[MAX_PATH];    /* full path to ogg */
    unsigned int length;    /* seconds */
    unsigned int position;  /* seconds */
};

static struct track_info tracks[MAX_TRACKS];

struct play_info
{
    int first;
    int last;
};

#ifdef _DEBUG
    #define dprintf(...) if (fh) { fprintf(fh, __VA_ARGS__); fflush(NULL); }
    FILE *fh = NULL;
#else
    #define dprintf(...)
#endif

int playloop = 0;
int current  = 1;
int opened = 0;
int paused = 0;
int stopped = 0;
int closed = 0;
int notify = 0;
int playing = 0;
int notready = 1;
HANDLE player = NULL;
int firstTrack = -1;
int lastTrack = 0;
int numTracks = 1; /* +1 for data track on mixed mode cd's */
DWORD dwCurTimeFormat = -1;
char music_path[2048];
int time_format = MCI_FORMAT_TMSF;
CRITICAL_SECTION cs;
char alias_s[100] = "cdaudio";
static struct play_info info = { -1, -1 };

int AudioLibrary;
int FileFormat;
int PlaybackMode;
char MusicFolder;
char musfold[255];


/* NOTE: The player is currently incapable of playing tracks from a specified
 * position. Instead it plays whole tracks only. Previous pause logic using
 * Sleep caused crackling sound and was not acceptable. Future plan is to add
 * proper seek commands to support playback from arbitrary positions on the track.
 */
 
int player_config(int AudioLibrary, int FileFormat, int PlaybackMode, char MusicFolder)
{
	TCHAR ConfigFileNameFullPath3[MAX_PATH];
	LPCSTR ConfigFileName3 = "wgmus.ini";

	*(strrchr(ConfigFileNameFullPath3, '\\')+1)=0;
	strcat(ConfigFileNameFullPath3,ConfigFileName3);
	
	dprintf("where is our config file?:%s\r\n", ConfigFileNameFullPath3);

	AudioLibrary = GetPrivateProfileInt("Settings", "AudioLibrary", 0, ConfigFileNameFullPath3);
	FileFormat = GetPrivateProfileInt("Settings", "FileFormat", 0, ConfigFileNameFullPath3);
	PlaybackMode = GetPrivateProfileInt("Settings", "PlaybackMode", 0, ConfigFileNameFullPath3);
	MusicFolder = GetPrivateProfileString("Settings", "MusicFolder", "tamus", musfold, MAX_PATH, ConfigFileNameFullPath3);
	
	return AudioLibrary, FileFormat, PlaybackMode, MusicFolder;
}
 
int player_main(struct play_info *info)
{
    int first = info->first;
    int last = info->last -1; /* -1 for plr logic */
    if(last<first)last = first; /* manage plr logic */
    current = first;
    if(current<firstTrack)current = firstTrack;
    dprintf("OGG Player logic: %d to %d\r\n", first, last);

    while (current <= last && playing)
    {
        dprintf("Next track: %s\r\n", tracks[current].path);
        plr_play(tracks[current].path);

        while (1)
        {
            if (plr_pump() == 0)
                break;

            if (!playing)
            {
                return 0;
            }
        }
        current++;
    }

    playloop = 0; /* IMPORTANT: Can not update the 'playing' variable from inside the 
                     thread since it's tied to the threads while loop condition and
                     can cause thread sync issues and a crash/deadlock. 
                     (For example: 'WinQuake' startup) */

    /* Sending notify successful message:*/
    if(notify && !paused)
    {
        dprintf("  Sending MCI_NOTIFY_SUCCESSFUL message...\r\n");
        SendMessageA((HWND)0xffff, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, 0xBEEF);
        notify = 0;
        /* NOTE: Notify message after successful playback is not working in Vista+.
        MCI_STATUS_MODE does not update to show that the track is no longer playing.
        Bug or broken design in mcicda.dll (also noted by the Wine team) */
    }
	player_config;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
#ifdef _DEBUG
        fh = fopen("winmm.log", "w"); /* Renamed to .log*/
#endif
        GetModuleFileName(hinstDLL, music_path, sizeof music_path);

        memset(tracks, 0, sizeof tracks);

        char *last = strrchr(music_path, '\\');
        if (last)
        {
            *last = '\0';
        }
		const char *mF = &MusicFolder;
        strncat(music_path, mF, sizeof music_path - 1);

        dprintf("TA-winmm music directory is %s\r\n", music_path);
        dprintf("TA-winmm searching tracks...\r\n");

        unsigned int position = 0;

        for (int i = 1; i < MAX_TRACKS; i++) /* "Changed: int i = 0" to "1" we can skip track00.ogg" */
        {
			snprintf(tracks[i].path, sizeof tracks[i].path, "%s\\%02d.ogg", music_path, i);
            tracks[i].length = plr_length(tracks[i].path);
            tracks[i].position = position;

            if (tracks[i].length < 4)
            {
                tracks[i].path[0] = '\0';
                position += 4; /* missing tracks are 4 second data tracks for us */
            }
            else
            {
                if (firstTrack == -1)
                {
                    firstTrack = i;
                }
                if(i == numTracks) numTracks -= 1; /* Take into account pure music cd's starting with track01.ogg */

                dprintf("Track %02d: %02d:%02d @ %d seconds\r\n", i, tracks[i].length / 60, tracks[i].length % 60, tracks[i].position);
                numTracks++;
                lastTrack = i;
                position += tracks[i].length;
            }
        }

        dprintf("Emulating total of %d CD tracks.\r\n\r\n", numTracks);
    }

#ifdef _DEBUG
    if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (fh)
        {
            fclose(fh);
            fh = NULL;
        }
    }
#endif

    return TRUE;
}

/* MCI commands */
/* https://docs.microsoft.com/windows/win32/multimedia/multimedia-commands */
MCIERROR WINAPI fake_mciSendCommandA(MCIDEVICEID IDDevice, UINT uMsg, DWORD_PTR fdwCommand, DWORD_PTR dwParam)
{
    dprintf("mciSendCommandA(IDDevice=%p, uMsg=%p, fdwCommand=%p, dwParam=%p)\r\n", IDDevice, uMsg, &fdwCommand, &dwParam);

    if (fdwCommand & MCI_NOTIFY)
    {
		if(AudioLibrary == 5)
		{
			SendMessageA((HWND)0xffff, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, 0xBEEF);
			return 0;
		}
    }
	else
    if (fdwCommand & MCI_WAIT)
    {
		if(AudioLibrary == 5)
		{
			return 0;
		}
    }
	else
    if (uMsg == MCI_OPEN)
    {
		if(AudioLibrary == 5)
		{
			if((closed = 1) && (opened = 0))
			{
				BASS_Init(1, 44100, 0, win, NULL);
				BASS_SetDevice(MAGIC_DEVICEID);
				opened = 1;
				closed = 0;
			}
			return 0;
		}
    }
	else
    if (uMsg == MCI_SET)
    {
		if(AudioLibrary == 5)
		{
			if (IDDevice == MAGIC_DEVICEID || IDDevice == 0 || IDDevice == 0xFFFFFFFF)
			{
				if (uMsg == MCI_SET)
				{
					LPMCI_SET_PARMS parms = (LPVOID)dwParam;

					dprintf("  MCI_SET\r\n");

					if (fdwCommand & MCI_SET_TIME_FORMAT)
					{
						dprintf("    MCI_SET_TIME_FORMAT\r\n");

						time_format = parms->dwTimeFormat;

						if (parms->dwTimeFormat == MCI_FORMAT_BYTES)
						{
							dprintf("      MCI_FORMAT_BYTES\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_FRAMES)
						{
							dprintf("      MCI_FORMAT_FRAMES\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_HMS)
						{
							dprintf("      MCI_FORMAT_HMS\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_MILLISECONDS)
						{
							dprintf("      MCI_FORMAT_MILLISECONDS\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_MSF)
						{
							dprintf("      MCI_FORMAT_MSF\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_SAMPLES)
						{
							dprintf("      MCI_FORMAT_SAMPLES\r\n");
						}
						else
						if (parms->dwTimeFormat == MCI_FORMAT_TMSF)
						{
							dprintf("      MCI_FORMAT_TMSF\r\n");
						}
					}
				}
			}
			return 0;
		}		
	}
	else
    if (uMsg == MCI_CLOSE)
    {
		if(AudioLibrary == 5)
		{
			BASS_Free();
			opened = 0;
			closed = 1;
			return 0;
		}
    }
	else
    if (uMsg == MCI_PLAY)
    {
		LPMCI_PLAY_PARMS parms = (LPVOID)dwParam;
		if(paused == 1)
		{
			BASS_Start();
			paused = 0;
		}
		else
		if (fdwCommand & MCI_FROM)
		{
			dprintf("    dwFrom: %d\r\n", parms->dwFrom);
			if(AudioLibrary == 5)
			{
				if (time_format == MCI_FORMAT_TMSF)
				{
                    info.first = MCI_TMSF_TRACK(parms->dwFrom);

                    dprintf("      TRACK  %d\n", MCI_TMSF_TRACK(parms->dwFrom));
                    dprintf("      MINUTE %d\n", MCI_TMSF_MINUTE(parms->dwFrom));
                    dprintf("      SECOND %d\n", MCI_TMSF_SECOND(parms->dwFrom));
                    dprintf("      FRAME  %d\n", MCI_TMSF_FRAME(parms->dwFrom));
				}
                else if (time_format == MCI_FORMAT_MILLISECONDS)
                {
                    info.first = 0;

                    for (int i = 0; i < MAX_TRACKS; i++)
                    {
                        // FIXME: take closest instead of absolute
                        if (tracks[i].position == parms->dwFrom / 1000)
                        {
                            info.first = i;
                        }
                    }

                    dprintf("      mapped milliseconds to %d\n", info.first);
                }
                else
                {
                    // FIXME: not really
                    info.first = parms->dwFrom;
                }
                if (info.first < firstTrack)
				{
                    info.first = firstTrack;
				}
                if (info.first > lastTrack)
				{
                    info.first = lastTrack;
				}
                info.last = info.first;
				HSTREAM str = BASS_StreamCreateFile(FALSE, tracks[current].path, 0, 0, 0);
				playing = 1;
			}
			return 0;
		}
    }
	else
    if (uMsg == MCI_STOP)
    {
		if(AudioLibrary == 5)
		{
			if(stopped == 0)
			{
				BASS_Pause();
				BASS_Stop();
				stopped = 1;
				playing = 0;
			}
			return 0;
		}
    }
	else
    if (uMsg == MCI_PAUSE)
    {
		if(AudioLibrary == 5)
		{
			if(paused == 0)
			{
				BASS_Pause();
				paused = 1;
				playing = 0;
			}	
			return 0;
		}
    }
	else
    if (uMsg == MCI_SYSINFO)
    {
		if(AudioLibrary == 5)
		{
			return 0;
		}
    }
	else
	if (uMsg == MCI_INFO)
	{
		if(AudioLibrary == 5)
		{
			return 0;
		}		
	}
	else
    if (uMsg == MCI_STATUS)
    {
		if(AudioLibrary == 5)
		{
			return 0;
		}
    }

    /* fallback */
    return MCIERR_UNRECOGNIZED_COMMAND;
}

/* MCI command strings */
/* https://docs.microsoft.com/windows/win32/multimedia/multimedia-command-strings */
MCIERROR WINAPI fake_mciSendStringA(LPCTSTR cmd, LPTSTR ret, UINT cchReturn, HANDLE hwndCallback)
{
	MCIERROR err;
	if(TRUE) {
		static char sPlayerNickName[80+1] = "";
		char sNickName[80+1];
		char sCommand[80+1];
		char sDevice[80+1];
		char *sCmdTarget;
		DWORD dwCommand;
		
		DWORD dwNewTimeFormat = -1;
		
		char cmdbuf[1024];
		char cmp_str[1024];

		if(!strcmp(sCommand, "open")) dwCommand = MCI_OPEN; else
		if(!strcmp(sCommand, "close")) dwCommand = MCI_CLOSE; else
		if(!strcmp(sCommand, "stop")) dwCommand = MCI_STOP; else
		if(!strcmp(sCommand, "pause")) dwCommand = MCI_PAUSE; else
		if(!strcmp(sCommand, "resume")) dwCommand = MCI_RESUME; else
		if(!strcmp(sCommand, "set")) dwCommand = MCI_SET; else
		if(!strcmp(sCommand, "status")) dwCommand = MCI_STATUS; else
		if(!strcmp(sCommand, "play")) dwCommand = MCI_PLAY; else
		if(!strcmp(sCommand, "seek")) dwCommand = MCI_SEEK; else
		if(!strcmp(sCommand, "capability")) dwCommand = MCI_GETDEVCAPS; else
		dwCommand = 0; 
		
		if(dwCommand && (dwCommand != MCI_OPEN)){
			// don't try to parse unknown commands, nor open command that
			// doesn't necessarily have extra arguments
			sCmdTarget = (char *)cmd;
			while (*sCmdTarget && *sCmdTarget != ' ') sCmdTarget++; // skip command
			while (*sCmdTarget && *sCmdTarget == ' ') sCmdTarget++; // skip first separator
			while (*sCmdTarget && *sCmdTarget != ' ') sCmdTarget++; // skip deviceid
			while (*sCmdTarget && *sCmdTarget == ' ') sCmdTarget++; // skip second separator
		}

		dprintf("[MCI String = %s]\n", cmd);

		/* copy cmd into cmdbuf */
		strcpy (cmdbuf,cmd);
		/* change cmdbuf into lower case */
		for (int i = 0; cmdbuf[i]; i++)
		{
			cmdbuf[i] = tolower(cmdbuf[i]);
		}

		if (strstr(cmd, "sysinfo cdaudio quantity"))
		{
			dprintf("  Returning quantity: 1\r\n");
			strcpy(ret, "1");
			return 0;
		}

		/* Example: "sysinfo cdaudio name 1 open" returns "cdaudio" or the alias.*/
		if (strstr(cmd, "sysinfo cdaudio name"))
		{
			dprintf("  Returning name: cdaudio\r\n");
			sprintf(ret, "%s", alias_s);
			return 0;
		}	

		sprintf(cmp_str, "info %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			fake_mciSendCommandA(MAGIC_DEVICEID, MCI_INFO, 0, (DWORD_PTR)NULL);
			return 0;
		}
		
		sprintf(cmp_str, "stop %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STOP, 0, (DWORD_PTR)NULL);
			return 0;
		}

		sprintf(cmp_str, "pause %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			fake_mciSendCommandA(MAGIC_DEVICEID, MCI_PAUSE, 0, (DWORD_PTR)NULL);
			return 0;
		}

		sprintf(cmp_str, "open %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			fake_mciSendCommandA(MAGIC_DEVICEID, MCI_OPEN, 0, (DWORD_PTR)NULL);
			return 0;
		}
		
		sprintf(cmp_str, "close %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			fake_mciSendCommandA(MAGIC_DEVICEID, MCI_CLOSE, 0, (DWORD_PTR)NULL);
			return 0;
		}

		/* Handle "set cdaudio/alias time format" */
		sprintf(cmp_str, "set %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			dwNewTimeFormat = -1;
			if (strstr(cmd, "time format milliseconds"))
			{
				static MCI_SET_PARMS parms;
				parms.dwTimeFormat = MCI_FORMAT_MILLISECONDS;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&parms);
				dwNewTimeFormat = 1;
				return 0;
			}
			else
			if (strstr(cmd, "time format tmsf"))
			{
				static MCI_SET_PARMS parms;
				parms.dwTimeFormat = MCI_FORMAT_TMSF;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&parms);
				dwNewTimeFormat = 2;
				return 0;
			}
			else
			if (strstr(cmd, "time format msf"))
			{
				static MCI_SET_PARMS parms;
				parms.dwTimeFormat = MCI_FORMAT_MSF;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_SET, MCI_SET_TIME_FORMAT, (DWORD_PTR)&parms);
				dwNewTimeFormat = 3;
				return 0;
			}
			else
			if (dwNewTimeFormat == -1)
			{
				dprintf("set time format failed\r\n");
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_CLOSE, 0, (DWORD_PTR)NULL);
			}
		}

		/* Handle "status cdaudio/alias" */
		sprintf(cmp_str, "status %s", alias_s);
		if (strstr(cmd, cmp_str)){
			if (strstr(cmd, "number of tracks"))
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_NUMBER_OF_TRACKS;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_WAIT, (DWORD_PTR)&parms);
				dprintf("  Returning number of tracks (%d)\r\n", numTracks);
				sprintf(ret, "%d", numTracks);
				return 0;
			}
			int track = 0;
			if (sscanf(cmd, "status %*s length track %d", &track) == 1)
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_LENGTH;
				parms.dwTrack = track;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_TRACK|MCI_WAIT, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (strstr(cmd, "length"))
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_LENGTH;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (sscanf(cmd, "status %*s type track %d", &track) == 1)
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_CDA_STATUS_TYPE_TRACK;
				parms.dwTrack = track;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_TRACK, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (sscanf(cmd, "status %*s position track %d", &track) == 1)
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_POSITION;
				parms.dwTrack = track;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_TRACK, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (strstr(cmd, "position"))
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_POSITION;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (strstr(cmd, "mode"))
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_MODE;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_STATUS_MODE, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (strstr(cmd, "current"))
			{
				static MCI_STATUS_PARMS parms;
				parms.dwItem = MCI_STATUS_CURRENT_TRACK;
				parms.dwTrack = track;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_STATUS, MCI_STATUS_ITEM|MCI_TRACK, (DWORD_PTR)&parms);
				sprintf(ret, "%d", parms.dwReturn);
				return 0;
			}
			if (strstr(cmd, "media present"))
			{
				strcpy(ret, "TRUE");
				return 0;
			}
		}

		/* Handle "play cdaudio/alias" */
		int from = -1, to = -1;
		sprintf(cmp_str, "play %s", alias_s);
		if (strstr(cmd, cmp_str))
		{
			if (strstr(cmd, "notify"))
			{
			notify = 1; /* storing the notify request */
			}
			if (sscanf(cmd, "play %*s from %d to %d", &from, &to) == 2)
			{
				static MCI_PLAY_PARMS parms;
				parms.dwFrom = from;
				parms.dwTo = to;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_PLAY, MCI_FROM|MCI_TO|MCI_NOTIFY, (DWORD_PTR)&parms);
				return 0;
			}
			if (sscanf(cmd, "play %*s from %d", &from) == 1)
			{
				static MCI_PLAY_PARMS parms;
				parms.dwFrom = from;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_PLAY, MCI_FROM, (DWORD_PTR)&parms);
				return 0;
			}
			if (sscanf(cmd, "play %*s to %d", &to) == 1)
			{
				static MCI_PLAY_PARMS parms;
				parms.dwTo = to;
				fake_mciSendCommandA(MAGIC_DEVICEID, MCI_PLAY, MCI_TO, (DWORD_PTR)&parms);
				return 0;
			}
		}
	}
    return err;
}

UINT WINAPI fake_auxGetNumDevs()
{
    dprintf("fake_auxGetNumDevs()\r\n");
    return 1;
}

MMRESULT WINAPI fake_auxGetDevCapsA(UINT_PTR uDeviceID, LPAUXCAPS lpCaps, UINT cbCaps)
{
    dprintf("fake_auxGetDevCapsA(uDeviceID=%08X, lpCaps=%p, cbCaps=%08X\n", uDeviceID, lpCaps, cbCaps);

    lpCaps->wMid = 2 /*MM_CREATIVE*/;
    lpCaps->wPid = 401 /*MM_CREATIVE_AUX_CD*/;
    lpCaps->vDriverVersion = 1;
    strcpy(lpCaps->szPname, "ogg-winmm virtual CD");
    lpCaps->wTechnology = AUXCAPS_CDAUDIO;
    lpCaps->dwSupport = AUXCAPS_VOLUME;

    return MMSYSERR_NOERROR;
}


MMRESULT WINAPI fake_auxGetVolume(UINT uDeviceID, LPDWORD lpdwVolume)
{
    dprintf("fake_auxGetVolume(uDeviceId=%08X, lpdwVolume=%p)\r\n", uDeviceID, lpdwVolume);
    *lpdwVolume = 0x00000000;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fake_auxSetVolume(UINT uDeviceID, DWORD dwVolume)
{

	static DWORD oldVolume = -1;
	char cmdbuf[256];

	
    DWORD dataBuffer;
    DWORD bufferSize = sizeof(dataBuffer);

    HKEY hkey;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, TEXT("SOFTWARE\\Cavedog Entertainment\\Total Annihilation"), 0, KEY_READ, &hkey) != ERROR_SUCCESS) {
        printf("failed to open key");
        return 1;
    }

    LRESULT status = RegQueryValueEx(
            hkey,
            TEXT("musicvol"),
            NULL,
            NULL,
            (LPBYTE)&dataBuffer,
            &bufferSize);

    if (RegCloseKey(hkey) != ERROR_SUCCESS) {
        printf("failed to close key");
        return 1;
    }

    dprintf("musicvol regkey status: %d\n", status);
    dprintf("musicvol regkey value: %d\n", dataBuffer);
    dprintf("musicvol regkey size: %d\n", bufferSize);
	oldVolume = dataBuffer;


    dprintf("fake_auxSetVolume(uDeviceId=%08X, dwVolume=%08X)\r\n", uDeviceID, dwVolume);

    if (dwVolume == oldVolume)
    {
        return MMSYSERR_NOERROR;
    }

    /*oldVolume = dwVolume;*/

    unsigned short left = LOWORD(dwVolume);
    unsigned short right = HIWORD(dwVolume);

    dprintf("    left : %ud (%04X)\n", left, left);
    dprintf("    right: %ud (%04X)\n", right, right);

    plr_volume((left / 65535.0f) * 100);

    return MMSYSERR_NOERROR;
}
