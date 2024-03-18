/**
 * =============================================================================
 * CleanerCS2
 * Copyright (C) 2024 Poggu
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include "extension.h"
#include <iserver.h>
#include <funchook.h>
#include "utils/module.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include "imgui/main.h"
#include <thread>
#include <schemasystem/schemasystem.h>
#include <entity2/entitysystem.h>
#include "interfaces.h"
#include "networkstringtabledefs.h"
#include "igameeventsystem.h"
#include "imgui/panels/eventlogger/eventlogger.h"
#include "../protobufs/generated/netmessages.pb.h"
#include "../protobufs/generated/usercmd.pb.h"
#include "../protobufs/generated/cs_usercmd.pb.h"
#include "networksystem/inetworkmessages.h"

#ifdef _WIN32
#define ROOTBIN "/bin/win64/"
#define GAMEBIN "/csgo/bin/win64/"
#else
#define ROOTBIN "/bin/linuxsteamrt64/"
#define GAMEBIN "/csgo/bin/linuxsteamrt64/"
#endif

CS2ServerGUI g_CS2ServerGUI;
std::thread g_thread;

typedef bool (*FilterMessage_t)(void* player, INetworkSerializable* pEvent, void* pData, void* pNetChan);
FilterMessage_t g_pFilterMessage = nullptr;
funchook_t* g_pHook = nullptr;

SH_DECL_HOOK8_void(IGameEventSystem, PostEventAbstract, SH_NOATTRIB, 0, CSplitScreenSlot, bool, int, const uint64*,
	INetworkSerializable*, const void*, unsigned long, NetChannelBufType_t);

CGameEntitySystem* GameEntitySystem()
{
#ifdef WIN32
	static int offset = 88;
#else
	static int offset = 80;
#endif
	return *reinterpret_cast<CGameEntitySystem**>((uintptr_t)(g_pGameResourceServiceServer)+offset);
}

// Should only be called within the active game loop (i e map should be loaded and active)
// otherwise that'll be nullptr!
CGlobalVars* GetGameGlobals()
{
	INetworkGameServer* server = g_pNetworkServerService->GetIGameServer();

	if (!server)
		return nullptr;

	return g_pNetworkServerService->GetIGameServer()->GetGlobals();
}

CON_COMMAND_F(gui, "Opens server GUI", FCVAR_LINKED_CONCOMMAND | FCVAR_SPONLY)
{
	if (!GUI::g_GUICtx.m_bIsGUIOpen)
	{
		g_thread = std::thread(GUI::InitializeGUI);
		g_thread.detach();
	}
}

int aa = 0;

template <typename T>
bool ReadPBFromBuffer(bf_read& buffer, T& pb)
{
	auto size = buffer.ReadVarInt32();

	if (size < 0 || size > 262140)
	{
		return false;
	}

	if (size > buffer.GetNumBytesLeft())
	{
		return false;
	}

	if ((buffer.GetNumBitsRead() % 8) == 0)
	{
		bool parseResult = pb.ParseFromArray(buffer.GetBasePointer() + buffer.GetNumBytesRead(), size);
		buffer.SeekRelative(size * 8);

		return true;
	}

	void* parseBuffer = stackalloc(size);
	if (!buffer.ReadBytes(parseBuffer, size))
	{
		return false;
	}

	if (!pb.ParseFromArray(parseBuffer, size))
	{
		return false;
	}

	return true;
}

bool Detour_FilterMessage(void* player, INetworkSerializable* pEvent, void* pData, void* pNetChan)
{
	NetMessageInfo_t* info = pEvent->GetNetMessageInfo();
	if (info)
	{
		if (info->m_MessageId == CLC_Messages::clc_Move)
		{
			CCLCMsg_Move* msg = (CCLCMsg_Move*)pData;

			if (msg->has_data() && msg->has_num_commands() && msg->num_commands() >= 1)
			{
				bf_read buffer(msg->data().data(), msg->data().size());

				for (int i = 0; i < msg->num_commands(); i++)
				{
					CSGOUserCmdPB userCmd;
					if (ReadPBFromBuffer(buffer, userCmd))
						GUI::EventLogger::AddEventLog(std::string(info->m_pBinding->GetName()), std::string(userCmd.DebugString().c_str()));
				}
			}
		}
		else
		{
			CUtlString str;
			info->m_pBinding->ToString(pData, str);

			GUI::EventLogger::AddEventLog(std::string(info->m_pBinding->GetName()), std::string(str.String()));
		}
	}

	return g_pFilterMessage(player, pEvent, pData, pNetChan);
}

void SetupHook()
{
	auto engineModule = new CModule(ROOTBIN, "engine2");

	int err;
	const byte sig[] = "\x40\x53\x48\x83\xEC\x30\x48\x3B\x15\x1B\xAB\x51\x00";

	g_pFilterMessage = (FilterMessage_t)engineModule->FindSignature((byte*)sig, sizeof(sig) - 1, err);

	if (err)
	{
		META_CONPRINTF("[CS2ServerGUI] Failed to find signature: %i\n", err);
		return;
	}

	auto g_pHook = funchook_create();
	funchook_prepare(g_pHook, (void**)&g_pFilterMessage, (void*)Detour_FilterMessage);
	funchook_install(g_pHook, 0);

	return;
}

PLUGIN_EXPOSE(CS2ServerGUI, g_CS2ServerGUI);
bool CS2ServerGUI::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, Interfaces::engine, IVEngineServer, INTERFACEVERSION_VENGINESERVER);
	GET_V_IFACE_CURRENT(GetEngineFactory, Interfaces::icvar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, Interfaces::server, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, Interfaces::gameclients, IServerGameClients, INTERFACEVERSION_SERVERGAMECLIENTS);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, Interfaces::g_pSchemaSystem2, CSchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceServiceServer, IGameResourceServiceServer, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, Interfaces::networkStringTableContainerServer, INetworkStringTableContainer, SOURCE2ENGINETOSERVERSTRINGTABLE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, Interfaces::gameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, Interfaces::networkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	g_SMAPI->AddListener( this, this );

	SH_ADD_HOOK_MEMFUNC(IGameEventSystem, PostEventAbstract, Interfaces::gameEventSystem, this, &CS2ServerGUI::Hook_PostEvent, false);

	SetupHook();

	g_pCVar = Interfaces::icvar;
	ConVar_Register( FCVAR_RELEASE | FCVAR_CLIENT_CAN_EXECUTE | FCVAR_GAMEDLL );

	// InitializeGUI on another thread
	g_thread = std::thread(GUI::InitializeGUI);
	g_thread.detach();
	return true;
}

void CS2ServerGUI::Hook_PostEvent(CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64* clients,
	INetworkSerializable* pEvent, const void* pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	if (!GUI::g_GUICtx.m_WindowStates.m_bEventLogger)
		return;

	NetMessageInfo_t* info = pEvent->GetNetMessageInfo();
	if (info)
	{
		CUtlString str;
		info->m_pBinding->ToString(pData, str);

		GUI::EventLogger::AddEventLog(std::string(info->m_pBinding->GetName()), std::string(str.String()));
	}

}

bool CS2ServerGUI::Unload(char *error, size_t maxlen)
{
	SH_REMOVE_HOOK_MEMFUNC(IGameEventSystem, PostEventAbstract, Interfaces::gameEventSystem, this, &CS2ServerGUI::Hook_PostEvent, false);
	return true;
}

void CS2ServerGUI::AllPluginsLoaded()
{
}

void CS2ServerGUI::OnLevelInit( char const *pMapName,
									 char const *pMapEntities,
									 char const *pOldLevel,
									 char const *pLandmarkName,
									 bool loadGame,
									 bool background )
{
	META_CONPRINTF("OnLevelInit(%s)\n", pMapName);
}

void CS2ServerGUI::OnLevelShutdown()
{
	META_CONPRINTF("OnLevelShutdown()\n");
}

bool CS2ServerGUI::Pause(char *error, size_t maxlen)
{
	return true;
}

bool CS2ServerGUI::Unpause(char *error, size_t maxlen)
{
	return true;
}

const char *CS2ServerGUI::GetLicense()
{
	return "GPLv3";
}

const char *CS2ServerGUI::GetVersion()
{
	return "1.0.0";
}

const char *CS2ServerGUI::GetDate()
{
	return __DATE__;
}

const char *CS2ServerGUI::GetLogTag()
{
	return "SERVERGUI";
}

const char *CS2ServerGUI::GetAuthor()
{
	return "Poggu";
}

const char *CS2ServerGUI::GetDescription()
{
	return "Server GUI using Dear ImGui";
}

const char *CS2ServerGUI::GetName()
{
	return "CS2ServerGUI";
}

const char *CS2ServerGUI::GetURL()
{
	return "https://poggu.me";
}
