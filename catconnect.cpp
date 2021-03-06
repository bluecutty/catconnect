#include "catconnect.h"
#include "tier1/KeyValues.h"
#include "catfiles.h"
#include "printers.h"
#include "ctfpartyclient.h"
#include "ctfplayerresource.h"
#include "cgcclientsystem.h"
#include "ctfgcclientsystem.h"
#include "cbaseentity.h"
#include "globals.h"
#include "isteamfriends.h"
#include "isteamuser.h"
#include "menu/menu.h"
#include "visual/drawer.h"
#include "visual/glow.h"
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm.hpp>
#include "cmdwrapper.h"
#include "vgui_controls/MessageMap.h"
#include "vgui_controls/Panel.h"
#include "vgui/ipanel.h"
#include "vgui/ISurface.h"
#include "shareddefs.h"
#include "hudelement.h"
#include "hud_macros.h"
#pragma push_macro ("DECLARE_CLASS_SIMPLE")
#include "public/declarefix.h"
#include "hud_basedeathnotice.h"
#pragma pop_macro ("DECLARE_CLASS_SIMPLE")
#include "inetchannel.h"
#include "inetchannelinfo.h"
#include "filesystem.h"

#include <optional>
#include <vector>

#define CAT_IDENTIFY 0xCA7
#define CAT_REPLY 0xCA8
#define CAT_FAKE 0xCA9

#define CHAT_FILTER_NONE 0
#define CHAT_FILTER_JOINLEAVE 0x1
#define CHAT_FILTER_NAMECHANGE 0x2
#define CHAT_FILTER_PUBLICCHAT 0x4
#define CHAT_FILTER_SERVERMSG 0x8
#define CHAT_FILTER_TEAMCHANGE 0x10
#define CHAT_FILTER_ACHIEVEMENT 0x20

NSCore::CSetting notify_partychat(xorstr_("catconnect.partychat.notify.bots"), xorstr_("1"));
NSCore::CSetting notify_gamechat(xorstr_("catconnect.gamechat.notify.bots"), xorstr_("1"));
NSCore::CSetting notify_saychat(xorstr_("catconnect.chat.notify.bots"), xorstr_("0"));
NSCore::CSetting votekicks_manage(xorstr_("catconnect.votekicks.manage"), xorstr_("1"));
NSCore::CSetting votekicks_showvoters(xorstr_("catconnect.votekicks.partychat.notifyvoters"), xorstr_("2"));
NSCore::CSetting votekicks_autoleave(xorstr_("catconnect.votekicks.autoleave"), xorstr_("3"));
NSCore::CSetting votekicks_autoleave_minvotes(xorstr_("catconnect.votekicks.autoleave.min"), xorstr_("4"));
NSCore::CSetting scoreboard_showcats(xorstr_("catconnect.scoreboard.showcats"), xorstr_("1"));
NSCore::CSetting scoreboard_showfriends(xorstr_("catconnect.scoreboard.showfriends"), xorstr_("1"));
NSCore::CSetting deathnotice_changecolors(xorstr_("catconnect.deathnotice.changecolors"), xorstr_("1"));
NSCore::CSetting glow_suppresstfglow(xorstr_("catconnect.glow.suppress.stocks"), xorstr_("1"));
NSCore::CSetting partyclient_autostanby(xorstr_("catconnect.partyclient.autostandby"), xorstr_("1"));
NSCore::CSetting debug_show(xorstr_("catconnect.showdebug"), xorstr_("0"));
extern NSCore::CSetting remove_newlines;

struct SSavedCat
{
	uint32_t m_iFriendID;
	uint8_t m_iIsCat; //1 = cat, 2 = fakecat
};

void CCatConnect::Init()
{
	//also install hook to listen for catbots

	NSInterfaces::g_pGameEventsMan2->AddListener(&ms_GameEventAchievementListener, xorstr_("achievement_earned"), false);
	NSInterfaces::g_pGameEventsMan2->AddListener(&ms_GameEventPlayerListener, xorstr_("player_connect_client"), false);
	NSInterfaces::g_pGameEventsMan2->AddListener(&ms_GameEventVoteCastListener, xorstr_("vote_cast"), false);
	NSInterfaces::g_pGameEventsMan2->AddListener(&ms_GameEventVoteCastListener, xorstr_("vote_changed"), false);

	//load saved data
	LoadSavedCats();

	//init gui
	NSCore::CDrawer::Init();
	NSMenu::g_CMenu.CreateGUI();

	//and glow
	NSCore::GlowInit();

	//and interface
	static CCatConnectExpose CCatConnectExtern;
	g_pCatConnectIface = &CCatConnectExtern;
	NSGlobals::g_mMyInterfaces[xorstr_(CATCONNECT_IFACE_VERSION)] = g_pCatConnectIface;

	//print message about successful injection
	NSUtils::PrintToClientConsole(Color{ 0, 255, 0, 255 }, xorstr_(MSG_PREFIX "Successfuly injected! Press HOME to open menu.\n"));

	//and don't forget about timer!
	ms_pCheckTimer = g_CTimerMan.CreateTimer(nullptr, 120.0);
	ms_pCheckTimer->SetCallback(NSUtils::TimerCallbackFn(&CCatConnect::OnAuthTimer));
	ms_pCheckTimer->SetFlags(TIMER_REPEAT);
	ms_pCheckTimer->Trigger(); //trigger timer first time

	ms_pVotingBackTimer = g_CTimerMan.CreateTimer(nullptr, 2.0);
	ms_pVotingBackTimer->SetCallback(NSUtils::TimerCallbackFn(&CCatConnect::OnBackVoteTimer));
	ms_pVotingBackTimer->SetFlags(TIMER_REPEAT);

	NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(nullptr, 1.0);
	pTimer->SetCallback(NSUtils::TimerCallbackFn(&CCatConnect::OnQueueCheck));
	pTimer->SetFlags(TIMER_REPEAT);

	ConVar * pVar = NSInterfaces::g_pCVar->FindVar(xorstr_("cl_vote_ui_active_after_voting"));
	pVar->SetValue(1); //keep it active after voting

	NSGlobals::g_bCatConnectInited = true;

	//Handle late injection, this must be last call in this function!
	if (NSInterfaces::g_pEngineClient->IsInGame())
		OnMapStart();
}

void CCatConnect::Destroy()
{
	NSInterfaces::g_pGameEventsMan2->RemoveListener(&ms_GameEventAchievementListener);
	NSInterfaces::g_pGameEventsMan2->RemoveListener(&ms_GameEventPlayerListener);
	NSInterfaces::g_pGameEventsMan2->RemoveListener(&ms_GameEventVoteCastListener);
	//save cats
	SaveCats();
	CCatFiles::SaveData();
	//we destroyed
	NSGlobals::g_bCatConnectInited = false;
}

void CCatConnect::Trigger()
{
	if (!NSInterfaces::g_pEngineClient->IsInGame() || NSInterfaces::g_pEngineClient->IsPlayingDemo())
	{
		if (debug_show.GetBool())
			NSUtils::PrintToClientConsole(Color{ 255, 0, 0, 255 }, xorstr_(MSG_PREFIX "Cannot force auth while not in game!\n"));
		return;
	}
	if (ms_pCheckTimer)
		ms_pCheckTimer->Trigger();
}

void CCatConnect::OnScoreBoardUpdate(void * pThis)
{
	void * pRealThis = (void *)NSUtils::CVirtualMemberTableMan::FindCompleteThisPointer((void ***)pThis);
	void * pPlayerListBlue = *(void **)((char *)pRealThis + 0x234);
	void * pPlayerListRed = *(void **)((char *)pRealThis + 0x238); //0x234 && 0x238 are offsets relative to this pointer

	//223 - vtable offset of AddItem
	//224 - vtable offset of ModifyItem
	//225 - vtable offset of RemoveItem
	//228 - vtable offset of SetItemFgColor
	//229 - vtable offset of SetItemBgColor
	//240 - vtable offset of GetSelectedItem
	//243 - vtable offset of GetItemData
	//246 - vtable offset of IsItemIDValid
	//248 - vtable offset of GetItemCount
	//249 - vtable offset of GetItemIDFromRow
	//250 - vtable offset of GetRowFromItemID

	for (uint8_t iList = 0; iList < 2; iList++)
	{
		void * pCurrentList = (!iList ? pPlayerListBlue : pPlayerListRed);
		for (int iNumItems = CALL_VFUNC_OFFS(int (__thiscall *)(void * pThis), pCurrentList, 248)(pCurrentList) - 1; iNumItems >= 0; iNumItems--) //GetItemCount
		{
			int iItemID = CALL_VFUNC_OFFS(int (__thiscall *)(void * pThis, int), pCurrentList, 249)(pCurrentList, iNumItems); //GetItemIDFromRow
			if (!CALL_VFUNC_OFFS(bool (__thiscall *)(void * pThis, int), pCurrentList, 246)(pCurrentList, iItemID)) //IsItemIDValid
				continue;
			KeyValues * pKV = CALL_VFUNC_OFFS(KeyValues * (__thiscall *)(void * pThis, int), pCurrentList, 243)(pCurrentList, iItemID); //GetItemData
			if (!pKV)
				continue;
			int iClient = pKV->GetInt(xorstr_("playerIndex"));
			int iConnected = pKV->GetInt(xorstr_("connected"));
			//check if client actually connected. 0 - not connected, 1 - lost connection / connecting, 2 - connected
			if (iConnected != 2)
				continue; //we don't care
			ECatState eClientState = GetClientState(iClient);
			if (eClientState != CatState_Default)
			{
				if (!(scoreboard_showcats.GetBool() && (eClientState == CatState_Cat || eClientState == CatState_FakeCat || eClientState == CatState_VoteBack) ||
					scoreboard_showfriends.GetBool() && (eClientState == CatState_Friend || eClientState == CatState_Party)))
					continue;

				//if he is a CAT, then set his color to green on scoreboard and show his class

				int iClientTeam = NSReclass::g_pCTFPlayerResource->GetPlayerTeam(iClient);
				Color cScoreBoardColor = GetStateColor(eClientState, iClientTeam);

				IClientNetworkable * pNetworkable = NSInterfaces::g_pClientEntityList->GetClientNetworkable(iClient);
				IClientUnknown * pUnknown = nullptr;
				if (pNetworkable && (pUnknown = pNetworkable->GetIClientUnknown()))
				{
					NSReclass::CBaseEntity * pClientEnt = (NSReclass::CBaseEntity *)pUnknown->GetBaseEntity();
					if (pClientEnt) //valid client
					{
						bool bModifyItem = false;
						bool bAlive = NSReclass::g_pCTFPlayerResource->IsPlayerAlive(iClient);
						if (!bAlive)
							cScoreBoardColor[3] /= 3;
						NSReclass::CBaseEntity * pMySelf = (NSReclass::CBaseEntity *)NSGlobals::g_pLocalPlayer;
						if (eClientState == CatState_Cat && pMySelf && pMySelf->GetTeam() != iClientTeam && iClientTeam > 1) //2 == red, 3 == blue
						{
							int iClass = NSReclass::g_pCTFPlayerResource->GetPlayerClass(iClient);
							int iClassEmblem = 0;
							if (bAlive)
								iClassEmblem = *(int *)((char *)pRealThis + 0x360 + iClass * 4);
							else
								iClassEmblem = *(int *)((char *)pRealThis + 0x384 + iClass * 4);
							//It's not a cheat! Show class of CATs only, not fake cats!
							pKV->SetInt(xorstr_("class"), iClassEmblem);
							bModifyItem = true;
						}

						if (remove_newlines.GetBool())
						{
							const char * pName = pKV->GetString(xorstr_("name"), "");
							if (*pName)
							{
								std::string sName = pName;
								RemoveUnprintable(sName);
								pKV->SetString(xorstr_("name"), sName.c_str());
								bModifyItem = true;
							}
						}

						if (bModifyItem)
						{
							//we need to create new instance of KeyValues cuz old will be deleted
							KeyValues * pNewKV = pKV->MakeCopy();
							CALL_VFUNC_OFFS(bool (__thiscall *)(void * pThis, int iItem, int iSection, const KeyValues * pKV), pCurrentList, 224)(pCurrentList, iItemID, 0, pNewKV); //ModifyItem
							pNewKV->deleteThis(); //it was copied by called function. Now delete this to prevent memory leak
						}
					}
				}

				CALL_VFUNC_OFFS(void (__thiscall *)(void * pThis, int, Color), pCurrentList, 228)(pCurrentList, iItemID, cScoreBoardColor); //SetItemFgColor
			}
		}
	}
}

void CCatConnect::OnDeathNoticePaintPre(void * pThis)
{
	if (!deathnotice_changecolors.GetBool())
		return;

	ms_vColors.clear();

	static CHudBaseDeathNotice * pRealThis = (CHudBaseDeathNotice *)NSUtils::CVirtualMemberTableMan::DoDynamicCast((void ***)pThis, xorstr_("CHudBaseDeathNotice")); //this never changed

	typedef void(__thiscall * RetireExpiredDeathNoticesFn)(CHudBaseDeathNotice *);
	static RetireExpiredDeathNoticesFn pRetireExpiredDeathNotices = nullptr;
	if (!pRetireExpiredDeathNotices)
	{
		pRetireExpiredDeathNotices = (RetireExpiredDeathNoticesFn)NSUtils::Sigscan(xorstr_("client.dll"), xorstr_("\x55\x8B\xEC\x83\xEC\x08\x53\x56\x8B\xF1\x57\x8B\xBE\xCC\x01\x00\x00"), 17);
		if (!pRetireExpiredDeathNotices)
			return; //wtf?
	}

	pRetireExpiredDeathNotices(pRealThis);

	struct DeathNoticeItemFixed : public DeathNoticeItem { void * pDummy; };
	CUtlVector<DeathNoticeItemFixed> * m_vDeathNotices = (CUtlVector<DeathNoticeItemFixed> *)((char *)pRealThis + 448);

	//In fact there must be no clients with totally same name on the server! This is actually illegal and if they have same name then it's one of 2 cases:
	//1. There an invisible character in name
	//2. There (n) prefix before name
	auto FindClientUserIDByName = [](const char * pName) -> int
	{
		for (int i = 1; i <= NSInterfaces::g_pEngineClient->GetMaxClients(); i++)
		{
			player_info_t sInfo;
			if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(i, &sInfo))
				continue;

			if (!strcmp(sInfo.name, pName))
				return sInfo.userID;
		}

		return -1;
	};

	auto ShouldAddDummyColor = [](DeathNoticeItemFixed & sMsg, bool bKiller) -> bool
	{
		if (bKiller)
		{
			if (sMsg.bSelfInflicted && (sMsg.iKillerID == sMsg.iVictimID || //suicide
				sMsg.iKillerID == 0)) //killer world
				return false;
			return true;
		}

		return true;
	};

	for (int i = 0, iCount = m_vDeathNotices->Count(); i < iCount; i++)
	{
		DeathNoticeItemFixed & sMsg = m_vDeathNotices->Element(i);

		//when client is dominated / etc. his id will be -1: https://i.imgur.com/s7fTuCR.png so we need restore it from prev message then
		//the same shit for pick up / capture intels, points, etc. No info actually provided so we need to listen for events (or hook CTFHudDeathNotice::OnGameEvent) or find client by name (why not?)
		//in the second case here is a problem, what if there were many ppl? Then sMsg.Killer.szName will look like: name1 + name2 etc.
		//in this case we don't care

		if (sMsg.iKillerID == -1 || sMsg.iVictimID == -1)
		{
			if (sMsg.Killer.szName[0] && sMsg.Killer.iTeam > 0)
			{
				int iTriedKiller = FindClientUserIDByName(sMsg.Killer.szName);
				if (iTriedKiller != -1)
					sMsg.iKillerID = iTriedKiller;
			}

			if (sMsg.Victim.szName[0] && sMsg.Victim.iTeam > 0)
			{
				int iTriedVictim = FindClientUserIDByName(sMsg.Victim.szName);
				if (iTriedVictim != -1)
					sMsg.iVictimID = iTriedVictim;
			}
		}

		if (sMsg.Killer.szName[0])
		{
			if (remove_newlines.GetBool()) //doing it here should not cause any problems
			{
				std::string sName = sMsg.Killer.szName;
				RemoveUnprintable(sName);
				strcpy(sMsg.Killer.szName, sName.c_str());
			}
			int iKillerClient = NSInterfaces::g_pEngineClient->GetPlayerForUserID(sMsg.iKillerID);
			ms_vColors.push_back(ShouldChangeColor(iKillerClient) ? GetClientColor(iKillerClient, sMsg.Killer.iTeam) : Color(0, 0, 0, 0));
		}
		else if (ShouldAddDummyColor(sMsg, true))
			ms_vColors.push_back(Color(0, 0, 0, 0));

		if (sMsg.Victim.szName[0])
		{
			if (remove_newlines.GetBool()) //doing it here should not cause any problems
			{
				std::string sName = sMsg.Victim.szName;
				RemoveUnprintable(sName);
				strcpy(sMsg.Victim.szName, sName.c_str());
			}
			int iVictimClient = NSInterfaces::g_pEngineClient->GetPlayerForUserID(sMsg.iVictimID);
			ms_vColors.push_back(ShouldChangeColor(iVictimClient) ? GetClientColor(iVictimClient, sMsg.Victim.iTeam) : Color(0, 0, 0, 0));
		}
		else if (ShouldAddDummyColor(sMsg, false))
			ms_vColors.push_back(Color(0, 0, 0, 0));
	}
}

std::string CCatConnect::OnChatMessage(int iClient, int iFilter, const char * pMessage)
{
	std::string sOriginalMsg = pMessage;
	if (iClient > 0)
	{
		//player_info_t sInfo;
		//if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo) || GetSavedState(sInfo.friendsID) != 1)
		//	return sOriginalMsg;
		if (iFilter == CHAT_FILTER_NONE || (iFilter & CHAT_FILTER_PUBLICCHAT))
		{
			//remove unprintable spammed by cats from chat
			RemoveUnprintable(sOriginalMsg);
		}
	}
	return sOriginalMsg;
}

void CCatConnect::OnMapStart()
{
	if (!NSGlobals::g_bCatConnectInited) return;
	//reinit it here to fix gay black glow
	NSCore::GlowCreate();
	//we just joined
	ms_bJustJoined = true;
	ms_bIsVotingBack = false;
	ms_pVotingBackTimer->ResetTime(2.0);
}

void CCatConnect::OnMapEnd()
{
	if (!NSGlobals::g_bCatConnectInited) return;
	NSCore::GlowDestroy();
	SetVoteState();
	ms_bIsVotingBack = false;
	ms_pVotingBackTimer->ResetTime(2.0);
	ms_iVoteStartedAgainstMe = -1;
}

bool CCatConnect::OnClientCommand(const char * pCmdLine) { return NSCore::CCmdWrapper::OnStringCommand(pCmdLine); }

void CCatConnect::CreateMove(float flInputSample, CUserCmd * pCmd)
{
	if (ms_bJustJoined && NSInterfaces::g_pEngineClient->IsInGame() && !NSInterfaces::g_pEngineClient->IsPlayingDemo())
	{
		//create another auth timer here
		auto * pTimer = g_CTimerMan.CreateTimer(nullptr, 5.0);
		pTimer->SetCallback(NSUtils::TimerCallbackFn(&CCatConnect::OnAuthTimer));
		pTimer = g_CTimerMan.CreateTimer(nullptr, 0.5);
		pTimer->SetCallback(NSUtils::TimerCallbackFn([](NSUtils::ITimer * pTimer, void * pData)
		{
			for (int i = 1; i <= NSInterfaces::g_pEngineClient->GetMaxClients(); i++)
			{
				ECatState eState = CCatConnect::GetClientState(i);
				if (eState == CatState_Cat || eState == CatState_FakeCat)
					CCatConnect::NotifyCat(i);
			}
			return true;
		}));
		SetVoteState();
		ms_bJustJoined = false;
	}
}

void CCatConnect::OnPotentialVoteKickStarted(int iTeam, int iCaller, int iTarget, const char * pReason)
{
	ms_iVoteStartedAgainstMe = 0;

	if (!votekicks_manage.GetBool())
		return;

	auto ShouldMarkAsVoteBack = [](ECatState eState) -> bool
	{
		switch (eState)
		{
		case CatState_Friend:
		case CatState_Party:
		case CatState_Cat:
			return false;
		default:
			return true;
		}
	};

	auto GetPlayerPrefixPrint = [](ECatState eState, bool bFirstUpper) -> std::string
	{
		std::string sToRet = xorstr_("player");
		switch (eState)
		{
		case CatState_Friend:
			sToRet = xorstr_("friend");
			break;
		case CatState_Party:
			sToRet = xorstr_("party member");
			break;
		case CatState_Cat:
			sToRet = xorstr_("CAT");
			break;
		case CatState_FakeCat:
			sToRet = xorstr_("fake CAT");
			break;
		}

		if (bFirstUpper)
			sToRet[0] = (char)std::toupper((unsigned char)sToRet[0]);
		return sToRet;
	};

	player_info_t sInfoTarget, sInfoCaller;
	auto pMySelf = ((NSReclass::CBaseEntity *)NSGlobals::g_pLocalPlayer);

	if (iCaller <= 0 || iTarget <= 0 || !pMySelf || !NSInterfaces::g_pEngineClient->GetPlayerInfo(iTarget, &sInfoTarget) || !NSInterfaces::g_pEngineClient->GetPlayerInfo(iCaller, &sInfoCaller))
		return;

	ECatState eTargetState = GetClientState(iTarget);
	ECatState eCallerState = GetClientState(iCaller);

	std::string sTargetPrefix = GetPlayerPrefixPrint(eTargetState, false);
	std::string sCallerPrefix = GetPlayerPrefixPrint(eCallerState, true);

	std::string sTargetName = sInfoTarget.name;
	std::string sCallerName = sInfoCaller.name;

	if (remove_newlines.GetBool())
	{
		RemoveUnprintable(sTargetName);
		RemoveUnprintable(sCallerName);
	}

	if (iTeam != pMySelf->GetTeam())
	{
		if (iTeam > 1)
		{
			if (ShouldMarkAsVoteBack(eCallerState) && !ShouldMarkAsVoteBack(eTargetState))
			{
				if (votekicks_manage.GetInt() == 2)
				{
					if (eCallerState != CatState_VoteBack)
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) from enemy team called a vote against %s %s ([U:1:%u]), and has been marked as to \"voteback\", and will be kicked when possible."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
					else
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) from enemy team called a vote against %s %s ([U:1:%u])."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
				}
				else
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) from enemy team called a vote against %s %s ([U:1:%u])."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
				MarkAsToVoteBack(sInfoCaller.friendsID);
			}
		}
		return;
	}

	if (iCaller == NSInterfaces::g_pEngineClient->GetLocalPlayer())
	{
		SetVoteState(1);
		if (ms_bIsVotingBack)
		{
			NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Calling a vote on %s %s ([U:1:%u])."), sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
			ms_bIsVotingBack = false;
		}
		return;
	}

	char * pVoteOption = new char[32];
	*pVoteOption = 0;

	if (iTarget != NSInterfaces::g_pEngineClient->GetLocalPlayer())
	{
		switch (eTargetState)
		{
			case CatState_Cat:
			{
				NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick CAT %s ([U:1:%u])! Voting no."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
				strcpy(pVoteOption, xorstr_("vote option2"));
				NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
				pTimer->SetCallback(&CCatConnect::OnVoteTimer);
				SetVoteState(2);
				if (ShouldMarkAsVoteBack(eCallerState)) MarkAsToVoteBack(sInfoCaller.friendsID);
				break;
			}
			case CatState_Friend:
			{
				NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick friend %s ([U:1:%u])! Voting no."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
				strcpy(pVoteOption, xorstr_("vote option2"));
				NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
				pTimer->SetCallback(&CCatConnect::OnVoteTimer);
				SetVoteState(2);
				if (ShouldMarkAsVoteBack(eCallerState)) MarkAsToVoteBack(sInfoCaller.friendsID);
				break;
			}
			case CatState_VoteBack:
			{
				NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick player %s ([U:1:%u])! Voting yes."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
				strcpy(pVoteOption, xorstr_("vote option1"));
				NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
				pTimer->SetCallback(&CCatConnect::OnVoteTimer);
				SetVoteState(1);
				break;
			}
			case CatState_Party:
			{
				switch (eCallerState)
				{
					case CatState_Friend:
					{
						if (InSameParty(sInfoCaller.friendsID))
						{
							NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Friend %s ([U:1:%u]) trying to kick party member %s ([U:1:%u])! Voting yes."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
							strcpy(pVoteOption, xorstr_("vote option1"));
							SetVoteState(1);
						}
						else
						{
							NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Friend %s ([U:1:%u]) trying to kick party member %s ([U:1:%u])! But friend not in party, voting no."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
							strcpy(pVoteOption, xorstr_("vote option2"));
							SetVoteState(2);
						}
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						break;
					}
					case CatState_Party:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Party member %s ([U:1:%u]) trying to kick party member %s ([U:1:%u])! Voting no."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option2"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(2);
						break;
					}
					case CatState_FakeCat:
					case CatState_Cat:
					default:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick party member %s ([U:1:%u])! Voting no."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option2"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(2);
						if (ShouldMarkAsVoteBack(eCallerState)) MarkAsToVoteBack(sInfoCaller.friendsID);
						break;
					}
				}
				break;
			}
			case CatState_FakeCat:
			default:
			{
				switch (eCallerState)
				{
					case CatState_Friend:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Friend %s ([U:1:%u]) trying to kick %s %s ([U:1:%u])! Voting yes."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option1"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(1);
						break;
					}
					case CatState_Party:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Party member %s ([U:1:%u]) trying to kick %s %s ([U:1:%u])! Voting yes."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option1"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(1);
						break;
					}
					case CatState_Cat:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "CAT %s ([U:1:%u]) trying to kick %s %s ([U:1:%u])! Voting yes."), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option1"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(1);
						break;
					}
					case CatState_FakeCat:
					default:
					{
						NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick %s %s ([U:1:%u])! Voting no."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID, sTargetPrefix.c_str(), sTargetName.c_str(), sInfoTarget.friendsID);
						strcpy(pVoteOption, xorstr_("vote option2"));
						NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
						pTimer->SetCallback(&CCatConnect::OnVoteTimer);
						SetVoteState(2);
						break;
					}
				}
				break;
			}
		}
	}
	else
	{
		ECatState eCallerState = GetClientState(iCaller);
		NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s %s ([U:1:%u]) trying to kick me! Voting no."), sCallerPrefix.c_str(), sCallerName.c_str(), sInfoCaller.friendsID);
		strcpy(pVoteOption, xorstr_("vote option2")); //vote no if we didn't vote yet
		NSUtils::ITimer * pTimer = g_CTimerMan.CreateTimer(pVoteOption, 0.5);
		pTimer->SetCallback(&CCatConnect::OnVoteTimer);
		SetVoteState(2);
		if (ShouldMarkAsVoteBack(eCallerState)) MarkAsToVoteBack(sInfoCaller.friendsID);
		ms_iVoteStartedAgainstMe = 1;
	}

	if (!*pVoteOption)
		delete[] pVoteOption; //it was unused
}

bool CCatConnect::OnPlayingDemoCheck(bool &bResult)
{
	if (!glow_suppresstfglow.GetBool())
		return false;
	bResult = true;
	return ms_bIsDrawingPostScreenSpaceEffects;
}

void CCatConnect::LoadSavedCats()
{
	auto oCats = CCatFiles::GetSectionByID(Section_Cats);
	if (!oCats.has_value())
		return; //should never happen
	auto &vCats = oCats.value();
	if (!(*vCats)->size())
		return; //there are no cats. Is this first run?
	const char * pActualData = (*vCats)->data();
	uint32_t iCountOfCats = uint32_t((*vCats)->size() / sizeof(SSavedCat));
	for (uint32_t iCat = 0; iCat < iCountOfCats; iCat++)
	{
		SSavedCat * pCat = (SSavedCat *)((char *)pActualData + sizeof(SSavedCat) * iCat);
		ms_mSavedCatState[pCat->m_iFriendID] = pCat->m_iIsCat;
	}
}

void CCatConnect::SaveCats()
{
	auto oCats = CCatFiles::GetSectionByID(Section_Cats);
	if (!oCats.has_value())
	{
		//should we log this?
		return; //should never happen
	}
	auto &vCats = oCats.value();
	(*vCats)->clear();
	SSavedCat sCat;
	for (auto iCat : ms_mSavedCatState)
	{
		sCat.m_iFriendID = iCat.first;
		sCat.m_iIsCat = iCat.second;
		(*vCats)->insert((*vCats)->end(), (char *)&sCat, (char *)&sCat + sizeof(SSavedCat));
	}
}

bool CCatConnect::OnFakeAuthTimer(NSUtils::ITimer * pTimer, void * pData)
{
	if (!NSInterfaces::g_pEngineClient->IsInGame() || NSInterfaces::g_pEngineClient->IsPlayingDemo())
		return false;

	SendCatMessage(CAT_FAKE);
	return false;
}

bool CCatConnect::OnAuthTimer(NSUtils::ITimer * pTimer, void * pData)
{
	//return true if success, false if timer should be stopped
	//we are not in-game, so we can't send messages to cats :c
	if (!NSInterfaces::g_pEngineClient->IsInGame() || NSInterfaces::g_pEngineClient->IsPlayingDemo()) return true;

	//oh yeah! We are in game. Now send message to all cats!

	if (debug_show.GetBool())
		NSUtils::PrintToClientConsole(Color{ 0, 0, 255, 255 }, xorstr_(MSG_PREFIX "Sending CAT auth message to the server...\n"));

	SendCatMessage(CAT_IDENTIFY);

	auto * pTimer2 = g_CTimerMan.CreateTimer(nullptr, 2.0);
	pTimer2->SetCallback(NSUtils::TimerCallbackFn(&CCatConnect::OnFakeAuthTimer));

	return true;
}

bool CCatConnect::OnVoteTimer(NSUtils::ITimer * pTimer, void * pData)
{
	const char * pVoteOption = (const char *)pData;
	if (pVoteOption)
	{
		if (*pVoteOption)
		{
			NSInterfaces::g_pEngineClient->ClientCmd_Unrestricted(pVoteOption);
			memset(pData, 0, strlen(pVoteOption));
		}
		delete[] pVoteOption;
	}
	return false;
}

bool CCatConnect::OnBackVoteTimer(NSUtils::ITimer * pTimer, void * pData)
{
	//pTimer->ResetTime(2.0); //always reset timer here

	if (votekicks_manage.GetInt() != 2 || ms_bJustJoined || ms_bIsVotingBack || !NSInterfaces::g_pEngineClient->IsInGame() || NSInterfaces::g_pEngineClient->IsPlayingDemo())
	{
		ms_bIsVotingBack = false;
		return true;
	}

	//first lookup for player to kick

	uint32_t iToKick = 0;

	for (int i = 1; i <= NSInterfaces::g_pEngineClient->GetMaxClients(); i++)
	{
		if (i == NSInterfaces::g_pEngineClient->GetLocalPlayer())
			continue;
		ECatState eState = GetClientState(i);
		if (eState == CatState_VoteBack)
		{
			if (NSReclass::g_pCTFPlayerResource->GetPlayerTeam(i) == ((NSReclass::CBaseEntity *)NSGlobals::g_pLocalPlayer)->GetTeam())
			{
				player_info_t sInfo;
				if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(i, &sInfo) || !sInfo.friendsID)
					continue;
				iToKick = sInfo.userID;
			}
		}
	}

	if (!iToKick)
		return true;

	ms_bIsVotingBack = true;

	char cKickCmd[64];
	sprintf(cKickCmd, xorstr_("callvote Kick \"%i cheating\""), iToKick);
	NSInterfaces::g_pEngineClient->ClientCmd_Unrestricted(cKickCmd);
	memset(cKickCmd, 0, strlen(cKickCmd));

	return true;
}

bool CCatConnect::OnQueueCheck(NSUtils::ITimer * pTimer, void * pData)
{
	if (!partyclient_autostanby.GetBool())
		return true;

	auto * pClientSystem = NSReclass::CTFGCClientSystem::GTFGCClientSystem();
	auto * pPartyClient = NSReclass::CTFPartyClient::GTFPartyClient();

	if (!pClientSystem->BHaveLiveMatch())
	{
		if (pPartyClient->BInStandbyQueue())
			return true;
		if (!pPartyClient->BCanQueueForStandby())
			return true; //we can't queue for standby for some reason
		CSteamID sPartyLeaderID;
		if (!pPartyClient->GetCurrentPartyLeader(sPartyLeaderID))
			return true;
		CSteamID sMyID = NSInterfaces::g_pSteamUser->GetSteamID();
		if (sMyID.GetAccountID() == sPartyLeaderID.GetAccountID())
			return true; //we can't queue for standby
		pPartyClient->RequestQueueForStandby();
		NSUtils::PrintToClientConsole(Color{ 0, 255, 0, 255 }, xorstr_(MSG_PREFIX "Starting standby queue to join match."));
	}

	return true;
}

void CCatConnect::SendCatMessage(int iMessage)
{
	//static IAchievementMgr * pAchivMan = NSInterfaces::g_pEngineClient->GetAchievementMgr();
	//pAchivMan->AwardAchievement(iMessage);

	KeyValues * pKV = new KeyValues(xorstr_("AchievementEarned"));
	pKV->SetInt(xorstr_("achievementID"), iMessage);
	NSInterfaces::g_pEngineClient->ServerCmdKeyValues(pKV);
}

uint8_t CCatConnect::GetSavedState(int iIndex)
{
	if (ms_mSavedCatState.find(iIndex) == ms_mSavedCatState.end()) return 0;
	return ms_mSavedCatState[iIndex];
}

void CCatConnect::CAchievementListener::FireGameEvent(IGameEvent * pEvent)
{
	if (!pEvent) return; //wtf?

	//now process event
	int iClient = pEvent->GetInt(xorstr_("player"), 0xDEAD);
	int iAchv = pEvent->GetInt(xorstr_("achievement"), 0xDEAD);

	if (iClient != 0xDEAD && (iAchv == CAT_IDENTIFY || iAchv == CAT_REPLY || iAchv == CAT_FAKE))
	{
		//if so, then it's cathook user or catbot. Let'say to him we are catbot too :P
		bool bReply = (iAchv == CAT_IDENTIFY);

		player_info_t sInfo;
		if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo))
			return;

		std::string sPlayerName = sInfo.name;
		if (remove_newlines.GetBool()) RemoveUnprintable(sPlayerName);

		if (iClient != NSInterfaces::g_pEngineClient->GetLocalPlayer())
		{
			uint8_t iIsCat = GetSavedState(sInfo.friendsID);
			if (bReply)
			{
				//send reply
				if (!iIsCat || debug_show.GetBool()) //print message only if it's unknown cat
					NSUtils::PrintToClientConsole(Color{ 0, 255, 0, 255 }, xorstr_(MSG_PREFIX "Received CAT auth message from player %s ([U:1:%u])! Sending response and marking him as CAT.\n"), sPlayerName.c_str(), sInfo.friendsID);
				SendCatMessage(CAT_REPLY);
			}
			else if (!iIsCat || debug_show.GetBool())
				NSUtils::PrintToClientConsole(Color{ 0, 255, 0, 255 }, xorstr_(MSG_PREFIX "Received CAT %s message from player %s ([U:1:%u])! Marking him as CAT.\n"), (iAchv == CAT_FAKE ? xorstr_("fake") : xorstr_("reply")), sPlayerName.c_str(), sInfo.friendsID);

			if ((!iIsCat || iIsCat == 3) && iAchv != CAT_FAKE)
			{
				ms_mSavedCatState[sInfo.friendsID] = 1;
				if (notify_partychat.GetBool())
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) marked as CAT!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_saychat.GetBool())
					NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) marked as CAT!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_gamechat.GetBool())
					NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XPlayer %s ([U:1:%u]) marked as CAT (catbot or cathook user). Most likely it's a catbot. Don't kill him or you will pay!"), 0xE05938, 0xCD71E1, sPlayerName.c_str(), sInfo.friendsID);
				SaveCats();
				CCatFiles::SaveData();
			}
			else if (iIsCat == 1 && iAchv == CAT_FAKE)
			{
				ms_mSavedCatState[sInfo.friendsID] = 2;
				if (notify_partychat.GetBool())
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) remarked as fake CAT (CatConnect user)!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_saychat.GetBool())
					NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) remarked fake CAT (CatConnect user)!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_gamechat.GetBool())
					NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XPlayer %s ([U:1:%u]) remarked as fake CAT (CatConnect user)!"), 0xE05938, 0xCD71E1, sPlayerName.c_str(), sInfo.friendsID);
				SaveCats();
				CCatFiles::SaveData();
			}
			else if ((!iIsCat || iIsCat == 3) && iAchv == CAT_FAKE)
			{
				ms_mSavedCatState[sInfo.friendsID] = 2;
				if (notify_partychat.GetBool())
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) marked as fake CAT (CatConnect user)!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_saychat.GetBool())
					NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "Player %s ([U:1:%u]) marked fake CAT (CatConnect user)!"), sPlayerName.c_str(), sInfo.friendsID);
				if (notify_gamechat.GetBool())
					NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XPlayer %s ([U:1:%u]) marked as fake CAT (CatConnect user)!"), 0xE05938, 0xCD71E1, sPlayerName.c_str(), sInfo.friendsID);
				SaveCats();
				CCatFiles::SaveData();
			}
		}
	}
}

void CCatConnect::CPlayerListener::FireGameEvent(IGameEvent * pEvent)
{
	if (!pEvent) return;

	const char * pEventName = pEvent->GetName();

	if (!pEventName || !*pEventName)
		return;

	if (!strcmp(pEventName, xorstr_("player_connect_client")))
	{
		const char * pNetID = pEvent->GetString(xorstr_("networkid"));
		if (strlen(pNetID) < 6)
			return;
		int iFriendID = atoi(pNetID + 5);
		uint8_t iCat = GetSavedState(iFriendID);
		std::string sPlayerName = pEvent->GetString(xorstr_("name"));
		if (remove_newlines.GetBool()) RemoveUnprintable(sPlayerName);
		if (iCat == 1)
		{
			if (notify_partychat.GetBool())
				NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "CAT %s (%s) joined the game."), sPlayerName.c_str(), pNetID);
			if (notify_saychat.GetBool())
				NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "CAT %s (%s) joined the game."), sPlayerName.c_str(), pNetID);
			if (notify_gamechat.GetBool())
				NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XCAT %s (%s) joined the game. Most likely it's a catbot. Don't kill him or you will pay!"), 0xE05938, 0xCD71E1, sPlayerName.c_str(), pNetID);
		}
		else if (iCat == 2)
		{
			if (notify_partychat.GetBool())
				NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Fake CAT %s (%s) joined the game."), sPlayerName.c_str(), pNetID);
			if (notify_saychat.GetBool())
				NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "Fake CAT %s (%s) joined the game."), sPlayerName.c_str(), pNetID);
			if (notify_gamechat.GetBool())
				NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XFake CAT %s (%s) joined the game."), 0xE05938, 0xCD71E1, sPlayerName.c_str(), pNetID);
		}
	}
}

void CCatConnect::CVoteListener::FireGameEvent(IGameEvent * pEvent)
{
	if (!votekicks_showvoters.GetBool())
		return;

	if (!strcmp(pEvent->GetName(), xorstr_("vote_cast")))
	{
		int iClient = pEvent->GetInt(xorstr_("entityid"));
		int iTeam = pEvent->GetInt(xorstr_("team"));
		player_info_t sInfo;
		if (iClient == NSInterfaces::g_pEngineClient->GetLocalPlayer() || iTeam != ((NSReclass::CBaseEntity *)NSGlobals::g_pLocalPlayer)->GetTeam() || !NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo))
			return;

		int iVoteOption = pEvent->GetInt(xorstr_("vote_option")) ? 1 : 0; //vote_option => 0 = yes, 1 = no

		if (votekicks_showvoters.GetInt() == 2 && iVoteOption + 1 == ms_iCurrentVoteChoice)
			return;

		std::string sPlayerName = sInfo.name;
		if (remove_newlines.GetBool()) RemoveUnprintable(sPlayerName);

		NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s ([U:1:%u]) voted %s."), sPlayerName.c_str(), sInfo.friendsID, !iVoteOption ? xorstr_("yes") : xorstr_("no"));
	}
	else if (!strcmp(pEvent->GetName(), xorstr_("vote_changed")))
	{
		if (!votekicks_autoleave.GetBool() || ms_iVoteStartedAgainstMe != 1)
			return;

		if (!NSInterfaces::g_pEngineClient->IsInGame() || NSInterfaces::g_pEngineClient->IsPlayingDemo())
			return;

		int iYesCount = pEvent->GetInt(xorstr_("vote_option1"));
		int iNoCount = pEvent->GetInt(xorstr_("vote_option2"));

		if (votekicks_autoleave_minvotes.GetInt() > iYesCount + iNoCount)
			return;

		if (iYesCount <= iNoCount)
			return;

		auto * pClientSystem = NSReclass::CTFGCClientSystem::GTFGCClientSystem();
		auto * pPartyClient = NSReclass::CTFPartyClient::GTFPartyClient();

		switch (votekicks_autoleave.GetInt())
		{
			case 1: //leave always
				if (pClientSystem->BConnectedToMatchServer())
				{
					pClientSystem->AbandonCurrentMatch();
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Leaving current match due to current votekick votes: %i/%i"), iYesCount, iNoCount);
				}
				break;
			case 2: //leave only if in party
				if (pClientSystem->BConnectedToMatchServer() && pPartyClient->GetNumPartyMembers() > 1)
				{
					pClientSystem->AbandonCurrentMatch();
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Leaving current match due to current votekick votes: %i/%i"), iYesCount, iNoCount);
				}
				break;
			case 3: //leave only if in party and not a party leader
			{
				if (pClientSystem->BConnectedToMatchServer() && pPartyClient->GetNumPartyMembers() > 1)
				{
					CSteamID sPartyLeaderID;
					if (!pPartyClient->GetCurrentPartyLeader(sPartyLeaderID))
						break;
					uint32_t iPartyLeaderID = sPartyLeaderID.GetAccountID();
					player_info_t sInfo;
					if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(NSInterfaces::g_pEngineClient->GetLocalPlayer(), &sInfo))
						break;
					if (sInfo.friendsID == iPartyLeaderID)
						break; //localplayer is party leader
					pClientSystem->AbandonCurrentMatch();
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Leaving current match due to current votekick votes: %i/%i"), iYesCount, iNoCount);
				}
				break;
			}
			case 4: //leave only if in party and not a party leader and party leader still in-game
			{
				if (pClientSystem->BConnectedToMatchServer() && pPartyClient->GetNumPartyMembers() > 1)
				{
					CSteamID sPartyLeaderID;
					if (!pPartyClient->GetCurrentPartyLeader(sPartyLeaderID))
						break;

					uint32_t iPartyLeaderID = sPartyLeaderID.GetAccountID();

					player_info_t sInfo;
					if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(NSInterfaces::g_pEngineClient->GetLocalPlayer(), &sInfo))
						break;

					if (sInfo.friendsID == iPartyLeaderID)
						break; //localplayer is party leader

					//look up for party leader now
					bool bFound = false;
					for (int iClient = 1; iClient <= NSInterfaces::g_pEngineClient->GetMaxClients(); iClient++)
					{
						if (iClient == NSInterfaces::g_pEngineClient->GetLocalPlayer())
							continue;

						if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo))
							continue;

						if (sInfo.friendsID == iPartyLeaderID)
						{
							bFound = true;
							break;
						}
					}

					if (!bFound)
						break; //party leader not in-game

					pClientSystem->AbandonCurrentMatch();
					NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "Leaving current match due to current votekick votes: %i/%i"), iYesCount, iNoCount);
				}
				break;
			}
		}
	}
}

ECatState CCatConnect::GetClientState(int iClient)
{
	if (iClient <= 0 || iClient > NSInterfaces::g_pEngineClient->GetMaxClients())
		return CatState_Default;

	if (iClient == NSInterfaces::g_pEngineClient->GetLocalPlayer())
		return CatState_Friend;

	player_info_t sInfo;
	if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo))
		return CatState_Default;

	if (sInfo.fakeplayer || sInfo.ishltv || !sInfo.friendsID)
		return CatState_Default;

	uint8_t iCat = GetSavedState(sInfo.friendsID);

	if (iCat == 1)
		return CatState_Cat;

	if (NSInterfaces::g_pSteamFriends)
	{
		CSteamID cSteamID = CSteamID(sInfo.friendsID, k_EUniversePublic, k_EAccountTypeIndividual);
		if (NSInterfaces::g_pSteamFriends->HasFriend(cSteamID, k_EFriendFlagImmediate))
			return CatState_Friend;
	}
	else
		NSUtils::PrintToClientConsole(Color{ 255, 0, 0, 255 }, xorstr_(MSG_PREFIX "Error: ISteamFriends interface is null!\n"));

	std::vector<unsigned int> vPartyMembers = NSReclass::CTFPartyClient::GTFPartyClient()->GetPartySteamIDs();

	auto iFound = boost::range::find(vPartyMembers, sInfo.friendsID);
	if (iFound != vPartyMembers.end())
		return CatState_Party;

	if (iCat == 3)
		return CatState_VoteBack;
	else if (iCat == 2)
		return CatState_FakeCat;

	return CatState_Default;
}

void CCatConnect::NotifyCat(int iClient)
{
	player_info_t sInfo;
	if (!NSInterfaces::g_pEngineClient->GetPlayerInfo(iClient, &sInfo))
		return;

	uint8_t iCat = GetSavedState(sInfo.friendsID);
	std::string sPlayerName = sInfo.name;
	if (remove_newlines.GetBool()) RemoveUnprintable(sPlayerName);
	if (iCat == 1)
	{
		if (notify_partychat.GetBool())
			NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s ([U:1:%u]) is a CAT."), sPlayerName.c_str(), sInfo.friendsID);
		if (notify_saychat.GetBool())
			NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "%s ([U:1:%u]) is a CAT."), sPlayerName.c_str(), sInfo.friendsID);
		if (notify_gamechat.GetBool())
			NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06X%s ([U:1:%u]) is a CAT. Most likely it's a catbot. Don't kill him or you will pay!"), 0xE05938, 0xCD71E1, sPlayerName.c_str(), sInfo.friendsID);
	}
	else if (iCat == 2)
	{
		if (notify_partychat.GetBool())
			NSUtils::PrintToPartyChat(xorstr_(MSG_PREFIX "%s ([U:1:%u]) is a fake CAT."), sInfo.name, sInfo.friendsID);
		if (notify_saychat.GetBool())
			NSUtils::PrintToChatAll(!!(notify_saychat.GetInt() - 1), xorstr_(MSG_PREFIX "%s ([U:1:%u]) is a fake CAT."), sInfo.name, sInfo.friendsID);
		if (notify_gamechat.GetBool())
			NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06X%s ([U:1:%u]) is a fake CAT."), 0xE05938, 0xCD71E1, sInfo.name, sInfo.friendsID);
	}
}

bool CCatConnect::InSameParty(int iIndex)
{
	std::vector<unsigned int> vPartyMembers = NSReclass::CTFPartyClient::GTFPartyClient()->GetPartySteamIDs();
	return boost::range::find(vPartyMembers, iIndex) != vPartyMembers.end();
}

Color CCatConnect::GetStateColor(ECatState eState, int iTeam)
{
	switch (eState)
	{
	case CatState_Cat:
		switch (iTeam)
		{
		case 2: //red
			return Color(255, 120, 190, 255);
		case 3: //blue
			return Color(178, 140, 255, 255);
		}
		return Color(0, 255, 0, 255);
	case CatState_Friend:
	case CatState_Party:
		return Color(0, 213, 153, 255);
	case CatState_FakeCat:
		switch (iTeam)
		{
		case 2: //red
			return Color(255, 0, 0, 255);
		case 3: //blue
			return Color(0, 0, 255, 255);
		}
		return Color(255, 255, 255, 255);
	case CatState_VoteBack:
		switch (iTeam)
		{
		case 2: //red
			return Color(255, 106, 0, 255);
		case 3: //blue
			return Color(66, 39, 101, 255);
		}
		return Color(122, 88, 88, 255);
	}

	return Color(0, 0, 0, 0);
}

bool CCatConnect::ShouldChangeColor(int iClient)
{
	ECatState eState = GetClientState(iClient);

	switch (eState)
	{
	case ECatState::CatState_Cat:
	case ECatState::CatState_FakeCat:
	case ECatState::CatState_VoteBack:
		return scoreboard_showcats.GetBool();
	case ECatState::CatState_Friend:
	case ECatState::CatState_Party:
		return scoreboard_showfriends.GetBool();
	default:
		return false;
	}
}

void CCatConnect::MarkAsToVoteBack(uint32_t iSteamID3) { ms_mSavedCatState[iSteamID3] = 3; SaveCats(); CCatFiles::SaveData(); }

bool CCatConnect::IsVotingBack(int iReason, int iSeconds)
{
	//FIXME: Something is broken
	/*if (ms_pVotingBackTimer)
	{
		if (iReason == 2 || iReason == 8 || iReason == 17)
			ms_pVotingBackTimer->ResetTime(float(iSeconds + 1));
	}*/

	bool bRet = ms_bIsVotingBack;
	ms_bIsVotingBack = false;
	return bRet;
}

void CCatConnect::RemoveUnprintable(std::string & sStr)
{
	boost::erase_all(sStr, xorstr_("\r"));
	boost::erase_all(sStr, xorstr_("\n"));
	boost::erase_all(sStr, xorstr_("\t"));
	boost::erase_all(sStr, xorstr_("\v"));
	boost::erase_all(sStr, xorstr_("\x1B")); //ESC character
}

NSCore::CCatCommandSafe getcommandslist(xorstr_("ccat_cmdlist"), CCMD_FLAG_NONE, xorstr_("Shows all catconnect commands"), [](const CCommand& rCmd)
{
	uint32_t iCount = NSCore::CCmdWrapper::GetCommandsCount();
	if (iCount <= 1)
	{
		NSUtils::PrintToClientConsole(Color(255, 0, 0, 255), xorstr_(MSG_PREFIX "Commands not found."));
		return true;
	}

	NSUtils::PrintToClientConsole(Color(0, 255, 0, 255), xorstr_(MSG_PREFIX "List of commands:"));

	for (uint32_t iCmd = 0; iCmd < iCount; iCmd++)
	{
		auto * pCmd = NSCore::CCmdWrapper::GetCommandByNumber(iCmd);
		if (pCmd->GetMyFlags() & (CCMD_FLAG_HOOK | CCMD_FLAG_HIDDEN))
			continue;
		NSUtils::PrintToClientConsole(Color(0, 255, 0, 255), xorstr_("Command \"%s\" - \"%s\"."), pCmd->GetMyName(), pCmd->GetMyDescription());
	}

	return true;
});

NSCore::CCatCommandSafe votehook(xorstr_("callvote"), CCMD_FLAG_HOOK, [](const CCommand & rCmd)
{
	if (!votekicks_manage.GetBool())
		return false;

	if (rCmd.ArgC() < 3)
		return false;

	if (stricmp(rCmd.Arg(1), xorstr_("kick")))
		return false;

	int iUserID = atoi(rCmd.Arg(2));

	if (iUserID <= 0)
		return false;

	int iClient = NSInterfaces::g_pEngineClient->GetPlayerForUserID(iUserID);
	if (iClient <= 0)
		return false;

	if (CCatConnect::GetClientState(iClient) == ECatState::CatState_Cat)
	{
		NSUtils::PrintToClientChat(xorstr_("\x07%06X" MSG_PREFIX "\x07%06XYour kick attempt was blocked. It's very bad idea to kick CATs except you wanna be marked as rage permanently by all CATs on the server and in IPC. Disable \"Manage votekicks\" option in menu if you really want to do this."), 0xE05938, 0xFF0000);
		return true;
	}

	return false;
});

NSCore::CCatCommandSafe voteopthook(xorstr_("vote"), CCMD_FLAG_HOOK, [](const CCommand & rCmd)
{
	if (rCmd.ArgC() < 2)
		return false;

	//set vote state here for the case if player vote manually

	if (!stricmp(rCmd.Arg(1), xorstr_("option1")))
		CCatConnect::SetVoteState(1);
	else if (!stricmp(rCmd.Arg(1), xorstr_("option2")))
		CCatConnect::SetVoteState(2);
	else
		CCatConnect::SetVoteState();

	return false;
});

ICatConnect * g_pCatConnectIface = nullptr;

NSUtils::ITimer * CCatConnect::ms_pCheckTimer;
CCatConnect::CAchievementListener CCatConnect::ms_GameEventAchievementListener;
CCatConnect::CPlayerListener CCatConnect::ms_GameEventPlayerListener;
CCatConnect::CVoteListener CCatConnect::ms_GameEventVoteCastListener;
bool CCatConnect::ms_bJustJoined = false;
std::map<uint32_t, uint8_t> CCatConnect::ms_mSavedCatState;
std::vector<Color> CCatConnect::ms_vColors;
unsigned int CCatConnect::ms_iCurrentVoteChoice = 0;
NSUtils::ITimer * CCatConnect::ms_pVotingBackTimer = nullptr;
bool CCatConnect::ms_bIsVotingBack = false;
bool CCatConnect::ms_bIsDrawingPostScreenSpaceEffects = false;
int CCatConnect::ms_iVoteStartedAgainstMe = -1;