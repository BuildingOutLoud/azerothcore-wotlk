/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>, released under GNU GPL v2 license, you may redistribute it and/or modify it under version 2 of the License, or (at your option), any later version.
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */

#include "CellImpl.h"
#include "GossipDef.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "MapManager.h"
#include "MapRefManager.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "ScriptedCreature.h"
#include "ScriptMgr.h"
#include "Transport.h"
#include "WaypointManager.h"
#include "World.h"

/// Put scripts in the execution queue
void Map::ScriptsStart(ScriptMapMap const& scripts, uint32 id, Object* source, Object* target)
{
    ///- Find the script map
    ScriptMapMap::const_iterator s = scripts.find(id);
    if (s == scripts.end())
        return;

    // prepare static data
    uint64 sourceGUID = source ? source->GetGUID() : uint64(0); //some script commands doesn't have source
    uint64 targetGUID = target ? target->GetGUID() : uint64(0);
    uint64 ownerGUID  = (source && source->GetTypeId() == TYPEID_ITEM) ? ((Item*)source)->GetOwnerGUID() : uint64(0);

    ///- Schedule script execution for all scripts in the script map
    ScriptMap const* s2 = &(s->second);
    bool immedScript = false;
    for (ScriptMap::const_iterator iter = s2->begin(); iter != s2->end(); ++iter)
    {
        ScriptAction sa;
        sa.sourceGUID = sourceGUID;
        sa.targetGUID = targetGUID;
        sa.ownerGUID  = ownerGUID;

        sa.script = &iter->second;
        m_scriptSchedule.insert(ScriptScheduleMap::value_type(time_t(sWorld->GetGameTime() + iter->first), sa));
        if (iter->first == 0)
            immedScript = true;

        sScriptMgr->IncreaseScheduledScriptsCount();
    }
    ///- If one of the effects should be immediate, launch the script execution
    if (/*start &&*/ immedScript && !i_scriptLock)
    {
        i_scriptLock = true;
        ScriptsProcess();
        i_scriptLock = false;
    }
}

void Map::ScriptCommandStart(ScriptInfo const& script, uint32 delay, Object* source, Object* target)
{
    // NOTE: script record _must_ exist until command executed

    // prepare static data
    uint64 sourceGUID = source ? source->GetGUID() : uint64(0);
    uint64 targetGUID = target ? target->GetGUID() : uint64(0);
    uint64 ownerGUID  = (source && source->GetTypeId() == TYPEID_ITEM) ? ((Item*)source)->GetOwnerGUID() : uint64(0);

    ScriptAction sa;
    sa.sourceGUID = sourceGUID;
    sa.targetGUID = targetGUID;
    sa.ownerGUID  = ownerGUID;

    sa.script = &script;
    m_scriptSchedule.insert(ScriptScheduleMap::value_type(time_t(sWorld->GetGameTime() + delay), sa));

    sScriptMgr->IncreaseScheduledScriptsCount();

    ///- If effects should be immediate, launch the script execution
    if (delay == 0 && !i_scriptLock)
    {
        i_scriptLock = true;
        ScriptsProcess();
        i_scriptLock = false;
    }
}

// Helpers for ScriptProcess method.
inline Player* Map::_GetScriptPlayerSourceOrTarget(Object* source, Object* target, const ScriptInfo* scriptInfo) const
{
    Player* player = nullptr;
    if (!source && !target)
        LOG_ERROR("server", "%s source and target objects are nullptr.", scriptInfo->GetDebugInfo().c_str());
    else
    {
        // Check target first, then source.
        if (target)
            player = target->ToPlayer();
        if (!player && source)
            player = source->ToPlayer();

        if (!player)
            LOG_ERROR("server", "%s neither source nor target object is player (source: TypeId: %u, Entry: %u, GUID: %u; target: TypeId: %u, Entry: %u, GUID: %u), skipping.",
                           scriptInfo->GetDebugInfo().c_str(),
                           source ? source->GetTypeId() : 0, source ? source->GetEntry() : 0, source ? source->GetGUIDLow() : 0,
                           target ? target->GetTypeId() : 0, target ? target->GetEntry() : 0, target ? target->GetGUIDLow() : 0);
    }
    return player;
}

inline Creature* Map::_GetScriptCreatureSourceOrTarget(Object* source, Object* target, const ScriptInfo* scriptInfo, bool bReverse) const
{
    Creature* creature = nullptr;
    if (!source && !target)
        LOG_ERROR("server", "%s source and target objects are nullptr.", scriptInfo->GetDebugInfo().c_str());
    else
    {
        if (bReverse)
        {
            // Check target first, then source.
            if (target)
                creature = target->ToCreature();
            if (!creature && source)
                creature = source->ToCreature();
        }
        else
        {
            // Check source first, then target.
            if (source)
                creature = source->ToCreature();
            if (!creature && target)
                creature = target->ToCreature();
        }

        if (!creature)
            LOG_ERROR("server", "%s neither source nor target are creatures (source: TypeId: %u, Entry: %u, GUID: %u; target: TypeId: %u, Entry: %u, GUID: %u), skipping.",
                           scriptInfo->GetDebugInfo().c_str(),
                           source ? source->GetTypeId() : 0, source ? source->GetEntry() : 0, source ? source->GetGUIDLow() : 0,
                           target ? target->GetTypeId() : 0, target ? target->GetEntry() : 0, target ? target->GetGUIDLow() : 0);
    }
    return creature;
}

inline Unit* Map::_GetScriptUnit(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const
{
    Unit* unit = nullptr;
    if (!obj)
        LOG_ERROR("server", "%s %s object is nullptr.", scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target");
    else if (!obj->isType(TYPEMASK_UNIT))
        LOG_ERROR("server", "%s %s object is not unit (TypeId: %u, Entry: %u, GUID: %u), skipping.",
                       scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target", obj->GetTypeId(), obj->GetEntry(), obj->GetGUIDLow());
    else
    {
        unit = obj->ToUnit();
        if (!unit)
            LOG_ERROR("server", "%s %s object could not be casted to unit.",
                           scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target");
    }
    return unit;
}

inline Player* Map::_GetScriptPlayer(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const
{
    Player* player = nullptr;
    if (!obj)
        LOG_ERROR("server", "%s %s object is nullptr.", scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target");
    else
    {
        player = obj->ToPlayer();
        if (!player)
            LOG_ERROR("server", "%s %s object is not a player (TypeId: %u, Entry: %u, GUID: %u).",
                           scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target", obj->GetTypeId(), obj->GetEntry(), obj->GetGUIDLow());
    }
    return player;
}

inline Creature* Map::_GetScriptCreature(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const
{
    Creature* creature = nullptr;
    if (!obj)
        LOG_ERROR("server", "%s %s object is nullptr.", scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target");
    else
    {
        creature = obj->ToCreature();
        if (!creature)
            LOG_ERROR("server", "%s %s object is not a creature (TypeId: %u, Entry: %u, GUID: %u).", scriptInfo->GetDebugInfo().c_str(),
                           isSource ? "source" : "target", obj->GetTypeId(), obj->GetEntry(), obj->GetGUIDLow());
    }
    return creature;
}

inline WorldObject* Map::_GetScriptWorldObject(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const
{
    WorldObject* pWorldObject = nullptr;
    if (!obj)
        LOG_ERROR("server", "%s %s object is nullptr.",
                       scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target");
    else
    {
        pWorldObject = dynamic_cast<WorldObject*>(obj);
        if (!pWorldObject)
            LOG_ERROR("server", "%s %s object is not a world object (TypeId: %u, Entry: %u, GUID: %u).",
                           scriptInfo->GetDebugInfo().c_str(), isSource ? "source" : "target", obj->GetTypeId(), obj->GetEntry(), obj->GetGUIDLow());
    }
    return pWorldObject;
}

inline void Map::_ScriptProcessDoor(Object* source, Object* target, const ScriptInfo* scriptInfo) const
{
    bool bOpen = false;
    uint32 guid = scriptInfo->ToggleDoor.GOGuid;
    int32 nTimeToToggle = std::max(15, int32(scriptInfo->ToggleDoor.ResetDelay));
    switch (scriptInfo->command)
    {
        case SCRIPT_COMMAND_OPEN_DOOR:
            bOpen = true;
            break;
        case SCRIPT_COMMAND_CLOSE_DOOR:
            break;
        default:
            LOG_ERROR("server", "%s unknown command for _ScriptProcessDoor.", scriptInfo->GetDebugInfo().c_str());
            return;
    }
    if (!guid)
        LOG_ERROR("server", "%s door guid is not specified.", scriptInfo->GetDebugInfo().c_str());
    else if (!source)
        LOG_ERROR("server", "%s source object is nullptr.", scriptInfo->GetDebugInfo().c_str());
    else if (!source->isType(TYPEMASK_UNIT))
        LOG_ERROR("server", "%s source object is not unit (TypeId: %u, Entry: %u, GUID: %u), skipping.", scriptInfo->GetDebugInfo().c_str(),
                       source->GetTypeId(), source->GetEntry(), source->GetGUIDLow());
    else
    {
        WorldObject* wSource = dynamic_cast <WorldObject*> (source);
        if (!wSource)
            LOG_ERROR("server", "%s source object could not be casted to world object (TypeId: %u, Entry: %u, GUID: %u), skipping.",
                           scriptInfo->GetDebugInfo().c_str(), source->GetTypeId(), source->GetEntry(), source->GetGUIDLow());
        else
        {
            GameObject* pDoor = _FindGameObject(wSource, guid);
            if (!pDoor)
                LOG_ERROR("server", "%s gameobject was not found (guid: %u).", scriptInfo->GetDebugInfo().c_str(), guid);
            else if (pDoor->GetGoType() != GAMEOBJECT_TYPE_DOOR)
                LOG_ERROR("server", "%s gameobject is not a door (GoType: %u, Entry: %u, GUID: %u).",
                               scriptInfo->GetDebugInfo().c_str(), pDoor->GetGoType(), pDoor->GetEntry(), pDoor->GetGUIDLow());
            else if (bOpen == (pDoor->GetGoState() == GO_STATE_READY))
            {
                pDoor->UseDoorOrButton(nTimeToToggle);

                if (target && target->isType(TYPEMASK_GAMEOBJECT))
                {
                    GameObject* goTarget = target->ToGameObject();
                    if (goTarget && goTarget->GetGoType() == GAMEOBJECT_TYPE_BUTTON)
                        goTarget->UseDoorOrButton(nTimeToToggle);
                }
            }
        }
    }
}

inline GameObject* Map::_FindGameObject(WorldObject* searchObject, uint32 guid) const
{
    GameObject* gameobject = nullptr;

    CellCoord p(acore::ComputeCellCoord(searchObject->GetPositionX(), searchObject->GetPositionY()));
    Cell cell(p);

    acore::GameObjectWithDbGUIDCheck goCheck(guid);
    acore::GameObjectSearcher<acore::GameObjectWithDbGUIDCheck> checker(searchObject, gameobject, goCheck);

    TypeContainerVisitor<acore::GameObjectSearcher<acore::GameObjectWithDbGUIDCheck>, GridTypeMapContainer > objectChecker(checker);
    cell.Visit(p, objectChecker, *searchObject->GetMap(), *searchObject, searchObject->GetGridActivationRange());

    return gameobject;
}

/// Process queued scripts
void Map::ScriptsProcess()
{
    if (m_scriptSchedule.empty())
        return;

    ///- Process overdue queued scripts
    ScriptScheduleMap::iterator iter = m_scriptSchedule.begin();
    // ok as multimap is a *sorted* associative container
    while (!m_scriptSchedule.empty() && (iter->first <= sWorld->GetGameTime()))
    {
        ScriptAction const& step = iter->second;

        Object* source = nullptr;
        if (step.sourceGUID)
        {
            switch (GUID_HIPART(step.sourceGUID))
            {
                case HIGHGUID_ITEM: // as well as HIGHGUID_CONTAINER
                    if (Player* player = GetPlayer(step.ownerGUID))
                        source = player->GetItemByGuid(step.sourceGUID);
                    break;
                case HIGHGUID_UNIT:
                case HIGHGUID_VEHICLE:
                    source = GetCreature(step.sourceGUID);
                    break;
                case HIGHGUID_PET:
                    source = GetPet(step.sourceGUID);
                    break;
                case HIGHGUID_PLAYER:
                    source = GetPlayer(step.sourceGUID);
                    break;
                case HIGHGUID_TRANSPORT:
                case HIGHGUID_GAMEOBJECT:
                    source = GetGameObject(step.sourceGUID);
                    break;
                case HIGHGUID_CORPSE:
                    source = GetCorpse(step.sourceGUID);
                    break;
                case HIGHGUID_MO_TRANSPORT:
                    {
                        GameObject* go = GetGameObject(step.sourceGUID);
                        source = go ? go->ToTransport() : nullptr;
                        break;
                    }
                default:
                    LOG_ERROR("server", "%s source with unsupported high guid (GUID: " UI64FMTD ", high guid: %u).",
                                   step.script->GetDebugInfo().c_str(), step.sourceGUID, GUID_HIPART(step.sourceGUID));
                    break;
            }
        }

        WorldObject* target = nullptr;
        if (step.targetGUID)
        {
            switch (GUID_HIPART(step.targetGUID))
            {
                case HIGHGUID_UNIT:
                case HIGHGUID_VEHICLE:
                    target = GetCreature(step.targetGUID);
                    break;
                case HIGHGUID_PET:
                    target = GetPet(step.targetGUID);
                    break;
                case HIGHGUID_PLAYER:                       // empty GUID case also
                    target = GetPlayer(step.targetGUID);
                    break;
                case HIGHGUID_TRANSPORT:
                case HIGHGUID_GAMEOBJECT:
                    target = GetGameObject(step.targetGUID);
                    break;
                case HIGHGUID_CORPSE:
                    target = GetCorpse(step.targetGUID);
                    break;
                case HIGHGUID_MO_TRANSPORT:
                    {
                        GameObject* go = GetGameObject(step.targetGUID);
                        target = go ? go->ToTransport() : nullptr;
                        break;
                    }
                default:
                    LOG_ERROR("server", "%s target with unsupported high guid (GUID: " UI64FMTD ", high guid: %u).",
                                   step.script->GetDebugInfo().c_str(), step.targetGUID, GUID_HIPART(step.targetGUID));
                    break;
            }
        }

        switch (step.script->command)
        {
            case SCRIPT_COMMAND_TALK:
                if (step.script->Talk.ChatType > CHAT_TYPE_WHISPER && step.script->Talk.ChatType != CHAT_MSG_RAID_BOSS_WHISPER)
                {
                    LOG_ERROR("server", "%s invalid chat type (%u) specified, skipping.", step.script->GetDebugInfo().c_str(), step.script->Talk.ChatType);
                    break;
                }
                if (step.script->Talk.Flags & SF_TALK_USE_PLAYER)
                {
                    if (Player* player = _GetScriptPlayerSourceOrTarget(source, target, step.script))
                    {
                        LocaleConstant loc_idx = player->GetSession()->GetSessionDbLocaleIndex();
                        BroadcastText const* broadcastText = sObjectMgr->GetBroadcastText(step.script->Talk.TextID);
                        std::string text = broadcastText->GetText(loc_idx, player->getGender());

                        switch (step.script->Talk.ChatType)
                        {
                            case CHAT_TYPE_SAY:
                                player->Say(text, LANG_UNIVERSAL);
                                break;
                            case CHAT_TYPE_YELL:
                                player->Yell(text, LANG_UNIVERSAL);
                                break;
                            case CHAT_TYPE_TEXT_EMOTE:
                            case CHAT_TYPE_BOSS_EMOTE:
                                player->TextEmote(text);
                                break;
                            case CHAT_TYPE_WHISPER:
                            case CHAT_MSG_RAID_BOSS_WHISPER:
                                {
                                    uint64 targetGUID = target ? target->GetGUID() : 0;
                                    if (!targetGUID || !IS_PLAYER_GUID(targetGUID))
                                        LOG_ERROR("server", "%s attempt to whisper to non-player unit, skipping.", step.script->GetDebugInfo().c_str());
                                    else
                                        player->Whisper(text, LANG_UNIVERSAL, targetGUID);
                                    break;
                                }
                            default:
                                break;                              // must be already checked at load
                        }
                    }
                }
                else
                {
                    // Source or target must be Creature.
                    if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                    {
                        uint64 targetGUID = target ? target->GetGUID() : 0;
                        switch (step.script->Talk.ChatType)
                        {
                            case CHAT_TYPE_SAY:
                                cSource->MonsterSay(step.script->Talk.TextID, LANG_UNIVERSAL, target);
                                break;
                            case CHAT_TYPE_YELL:
                                cSource->MonsterYell(step.script->Talk.TextID, LANG_UNIVERSAL, target);
                                break;
                            case CHAT_TYPE_TEXT_EMOTE:
                                cSource->MonsterTextEmote(step.script->Talk.TextID, target);
                                break;
                            case CHAT_TYPE_BOSS_EMOTE:
                                cSource->MonsterTextEmote(step.script->Talk.TextID, target, true);
                                break;
                            case CHAT_TYPE_WHISPER:
                                if (!targetGUID || !IS_PLAYER_GUID(targetGUID))
                                    LOG_ERROR("server", "%s attempt to whisper to non-player unit, skipping.", step.script->GetDebugInfo().c_str());
                                else
                                    cSource->MonsterWhisper(step.script->Talk.TextID, target->ToPlayer());
                                break;
                            case CHAT_MSG_RAID_BOSS_WHISPER:
                                if (!targetGUID || !IS_PLAYER_GUID(targetGUID))
                                    LOG_ERROR("server", "%s attempt to raidbosswhisper to non-player unit, skipping.", step.script->GetDebugInfo().c_str());
                                else
                                    cSource->MonsterWhisper(step.script->Talk.TextID, target->ToPlayer(), true);
                                break;
                            default:
                                break;                              // must be already checked at load
                        }
                    }
                }
                break;

            case SCRIPT_COMMAND_EMOTE:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    if (step.script->Emote.Flags & SF_EMOTE_USE_STATE)
                        cSource->SetUInt32Value(UNIT_NPC_EMOTESTATE, step.script->Emote.EmoteID);
                    else
                        cSource->HandleEmoteCommand(step.script->Emote.EmoteID);
                }
                break;

            case SCRIPT_COMMAND_FIELD_SET:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    // Validate field number.
                    if (step.script->FieldSet.FieldID <= OBJECT_FIELD_ENTRY || step.script->FieldSet.FieldID >= cSource->GetValuesCount())
                        LOG_ERROR("server", "%s wrong field %u (max count: %u) in object (TypeId: %u, Entry: %u, GUID: %u) specified, skipping.",
                                       step.script->GetDebugInfo().c_str(), step.script->FieldSet.FieldID,
                                       cSource->GetValuesCount(), cSource->GetTypeId(), cSource->GetEntry(), cSource->GetGUIDLow());
                    else
                        cSource->SetUInt32Value(step.script->FieldSet.FieldID, step.script->FieldSet.FieldValue);
                }
                break;

            case SCRIPT_COMMAND_MOVE_TO:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    Unit* unit = (Unit*)cSource;
                    if (step.script->MoveTo.TravelTime != 0)
                    {
                        float speed = unit->GetDistance(step.script->MoveTo.DestX, step.script->MoveTo.DestY, step.script->MoveTo.DestZ) / ((float)step.script->MoveTo.TravelTime * 0.001f);
                        unit->MonsterMoveWithSpeed(step.script->MoveTo.DestX, step.script->MoveTo.DestY, step.script->MoveTo.DestZ, speed);
                    }
                    else
                        unit->NearTeleportTo(step.script->MoveTo.DestX, step.script->MoveTo.DestY, step.script->MoveTo.DestZ, unit->GetOrientation());
                }
                break;

            case SCRIPT_COMMAND_FLAG_SET:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    // Validate field number.
                    if (step.script->FlagToggle.FieldID <= OBJECT_FIELD_ENTRY || step.script->FlagToggle.FieldID >= cSource->GetValuesCount())
                        LOG_ERROR("server", "%s wrong field %u (max count: %u) in object (TypeId: %u, Entry: %u, GUID: %u) specified, skipping.",
                                       step.script->GetDebugInfo().c_str(), step.script->FlagToggle.FieldID,
                                       cSource->GetValuesCount(), cSource->GetTypeId(), cSource->GetEntry(), cSource->GetGUIDLow());
                    else
                        cSource->SetFlag(step.script->FlagToggle.FieldID, step.script->FlagToggle.FieldValue);
                }
                break;

            case SCRIPT_COMMAND_FLAG_REMOVE:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    // Validate field number.
                    if (step.script->FlagToggle.FieldID <= OBJECT_FIELD_ENTRY || step.script->FlagToggle.FieldID >= cSource->GetValuesCount())
                        LOG_ERROR("server", "%s wrong field %u (max count: %u) in object (TypeId: %u, Entry: %u, GUID: %u) specified, skipping.",
                                       step.script->GetDebugInfo().c_str(), step.script->FlagToggle.FieldID,
                                       cSource->GetValuesCount(), cSource->GetTypeId(), cSource->GetEntry(), cSource->GetGUIDLow());
                    else
                        cSource->RemoveFlag(step.script->FlagToggle.FieldID, step.script->FlagToggle.FieldValue);
                }
                break;

            case SCRIPT_COMMAND_TELEPORT_TO:
                if (step.script->TeleportTo.Flags & SF_TELEPORT_USE_CREATURE)
                {
                    // Source or target must be Creature.
                    if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script, true))
                        cSource->NearTeleportTo(step.script->TeleportTo.DestX, step.script->TeleportTo.DestY, step.script->TeleportTo.DestZ, step.script->TeleportTo.Orientation);
                }
                else
                {
                    // Source or target must be Player.
                    if (Player* player = _GetScriptPlayerSourceOrTarget(source, target, step.script))
                        player->TeleportTo(step.script->TeleportTo.MapID, step.script->TeleportTo.DestX, step.script->TeleportTo.DestY, step.script->TeleportTo.DestZ, step.script->TeleportTo.Orientation);
                }
                break;

            case SCRIPT_COMMAND_QUEST_EXPLORED:
                {
                    if (!source)
                    {
                        LOG_ERROR("server", "%s source object is nullptr.", step.script->GetDebugInfo().c_str());
                        break;
                    }
                    if (!target)
                    {
                        LOG_ERROR("server", "%s target object is nullptr.", step.script->GetDebugInfo().c_str());
                        break;
                    }

                    // when script called for item spell casting then target == (unit or GO) and source is player
                    WorldObject* worldObject;
                    Player* player = target->ToPlayer();
                    if (player)
                    {
                        if (source->GetTypeId() != TYPEID_UNIT && source->GetTypeId() != TYPEID_GAMEOBJECT && source->GetTypeId() != TYPEID_PLAYER)
                        {
                            LOG_ERROR("server", "%s source is not unit, gameobject or player (TypeId: %u, Entry: %u, GUID: %u), skipping.",
                                           step.script->GetDebugInfo().c_str(), source->GetTypeId(), source->GetEntry(), source->GetGUIDLow());
                            break;
                        }
                        worldObject = dynamic_cast<WorldObject*>(source);
                    }
                    else
                    {
                        player = source->ToPlayer();
                        if (player)
                        {
                            if (target->GetTypeId() != TYPEID_UNIT && target->GetTypeId() != TYPEID_GAMEOBJECT && target->GetTypeId() != TYPEID_PLAYER)
                            {
                                LOG_ERROR("server", "%s target is not unit, gameobject or player (TypeId: %u, Entry: %u, GUID: %u), skipping.",
                                               step.script->GetDebugInfo().c_str(), target->GetTypeId(), target->GetEntry(), target->GetGUIDLow());
                                break;
                            }
                            worldObject = dynamic_cast<WorldObject*>(target);
                        }
                        else
                        {
                            LOG_ERROR("server", "%s neither source nor target is player (source: TypeId: %u, Entry: %u, GUID: %u; target: TypeId: %u, Entry: %u, GUID: %u), skipping.",
                                           step.script->GetDebugInfo().c_str(), source->GetTypeId(), source->GetEntry(), source->GetGUIDLow(),
                                           target->GetTypeId(), target->GetEntry(), target->GetGUIDLow());
                            break;
                        }
                    }

                    // quest id and flags checked at script loading
                    if ((worldObject->GetTypeId() != TYPEID_UNIT || ((Unit*)worldObject)->IsAlive()) &&
                            (step.script->QuestExplored.Distance == 0 || worldObject->IsWithinDistInMap(player, float(step.script->QuestExplored.Distance))))
                        player->AreaExploredOrEventHappens(step.script->QuestExplored.QuestID);
                    else
                        player->FailQuest(step.script->QuestExplored.QuestID);

                    break;
                }

            case SCRIPT_COMMAND_KILL_CREDIT:
                // Source or target must be Player.
                if (Player* player = _GetScriptPlayerSourceOrTarget(source, target, step.script))
                {
                    if (step.script->KillCredit.Flags & SF_KILLCREDIT_REWARD_GROUP)
                        player->RewardPlayerAndGroupAtEvent(step.script->KillCredit.CreatureEntry, player);
                    else
                        player->KilledMonsterCredit(step.script->KillCredit.CreatureEntry, 0);
                }
                break;

            case SCRIPT_COMMAND_RESPAWN_GAMEOBJECT:
                if (!step.script->RespawnGameobject.GOGuid)
                {
                    LOG_ERROR("server", "%s gameobject guid (datalong) is not specified.", step.script->GetDebugInfo().c_str());
                    break;
                }

                // Source or target must be WorldObject.
                if (WorldObject* pSummoner = _GetScriptWorldObject(source, true, step.script))
                {
                    GameObject* pGO = _FindGameObject(pSummoner, step.script->RespawnGameobject.GOGuid);
                    if (!pGO)
                    {
                        LOG_ERROR("server", "%s gameobject was not found (guid: %u).", step.script->GetDebugInfo().c_str(), step.script->RespawnGameobject.GOGuid);
                        break;
                    }

                    if (pGO->GetGoType() == GAMEOBJECT_TYPE_FISHINGNODE ||
                            pGO->GetGoType() == GAMEOBJECT_TYPE_DOOR        ||
                            pGO->GetGoType() == GAMEOBJECT_TYPE_BUTTON      ||
                            pGO->GetGoType() == GAMEOBJECT_TYPE_TRAP)
                    {
                        LOG_ERROR("server", "%s can not be used with gameobject of type %u (guid: %u).",
                                       step.script->GetDebugInfo().c_str(), uint32(pGO->GetGoType()), step.script->RespawnGameobject.GOGuid);
                        break;
                    }

                    // Check that GO is not spawned
                    if (!pGO->isSpawned())
                    {
                        int32 nTimeToDespawn = std::max(5, int32(step.script->RespawnGameobject.DespawnDelay));
                        pGO->SetLootState(GO_READY);
                        pGO->SetRespawnTime(nTimeToDespawn);

                        pGO->GetMap()->AddToMap(pGO);
                    }
                }
                break;

            case SCRIPT_COMMAND_TEMP_SUMMON_CREATURE:
                {
                    // Source must be WorldObject.
                    if (WorldObject* pSummoner = _GetScriptWorldObject(source, true, step.script))
                    {
                        if (!step.script->TempSummonCreature.CreatureEntry)
                            LOG_ERROR("server", "%s creature entry (datalong) is not specified.", step.script->GetDebugInfo().c_str());
                        else
                        {
                            uint32 entry = step.script->TempSummonCreature.CreatureEntry;

                            float x = step.script->TempSummonCreature.PosX;
                            float y = step.script->TempSummonCreature.PosY;
                            float z = step.script->TempSummonCreature.PosZ;
                            float o = step.script->TempSummonCreature.Orientation;

                            if (step.script->TempSummonCreature.CheckIfExists)
                                if (Unit* trigger = pSummoner->SummonTrigger(x, y, z, o, 1))
                                    if (trigger->FindNearestCreature(entry, 60.0f))
                                        break;

                            if (!pSummoner->SummonCreature(entry, x, y, z, o, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, step.script->TempSummonCreature.DespawnDelay))
                                LOG_ERROR("server", "%s creature was not spawned (entry: %u).", step.script->GetDebugInfo().c_str(), step.script->TempSummonCreature.CreatureEntry);
                        }
                    }
                    break;
                }

            case SCRIPT_COMMAND_OPEN_DOOR:
            case SCRIPT_COMMAND_CLOSE_DOOR:
                _ScriptProcessDoor(source, target, step.script);
                break;

            case SCRIPT_COMMAND_ACTIVATE_OBJECT:
                // Source must be Unit.
                if (Unit* unit = _GetScriptUnit(source, true, step.script))
                {
                    // Target must be GameObject.
                    if (!target)
                    {
                        LOG_ERROR("server", "%s target object is nullptr.", step.script->GetDebugInfo().c_str());
                        break;
                    }

                    if (target->GetTypeId() != TYPEID_GAMEOBJECT)
                    {
                        LOG_ERROR("server", "%s target object is not gameobject (TypeId: %u, Entry: %u, GUID: %u), skipping.",
                                       step.script->GetDebugInfo().c_str(), target->GetTypeId(), target->GetEntry(), target->GetGUIDLow());
                        break;
                    }

                    if (GameObject* pGO = target->ToGameObject())
                        pGO->Use(unit);
                }
                break;

            case SCRIPT_COMMAND_REMOVE_AURA:
                {
                    // Source (datalong2 != 0) or target (datalong2 == 0) must be Unit.
                    bool bReverse = step.script->RemoveAura.Flags & SF_REMOVEAURA_REVERSE;
                    if (Unit* unit = _GetScriptUnit(bReverse ? source : target, bReverse, step.script))
                        unit->RemoveAurasDueToSpell(step.script->RemoveAura.SpellID);
                    break;
                }

            case SCRIPT_COMMAND_CAST_SPELL:
                {
                    // TODO: Allow gameobjects to be targets and casters
                    if (!source && !target)
                    {
                        LOG_ERROR("server", "%s source and target objects are nullptr.", step.script->GetDebugInfo().c_str());
                        break;
                    }

                    Unit* uSource = nullptr;
                    Unit* uTarget = nullptr;
                    // source/target cast spell at target/source (script->datalong2: 0: s->t 1: s->s 2: t->t 3: t->s
                    switch (step.script->CastSpell.Flags)
                    {
                        case SF_CASTSPELL_SOURCE_TO_TARGET: // source -> target
                            uSource = source ? source->ToUnit() : nullptr;
                            uTarget = target ? target->ToUnit() : nullptr;
                            break;
                        case SF_CASTSPELL_SOURCE_TO_SOURCE: // source -> source
                            uSource = source ? source->ToUnit() : nullptr;
                            uTarget = uSource;
                            break;
                        case SF_CASTSPELL_TARGET_TO_TARGET: // target -> target
                            uSource = target ? target->ToUnit() : nullptr;
                            uTarget = uSource;
                            break;
                        case SF_CASTSPELL_TARGET_TO_SOURCE: // target -> source
                            uSource = target ? target->ToUnit() : nullptr;
                            uTarget = source ? source->ToUnit() : nullptr;
                            break;
                        case SF_CASTSPELL_SEARCH_CREATURE: // source -> creature with entry
                            uSource = source ? source->ToUnit() : nullptr;
                            uTarget = uSource ? GetClosestCreatureWithEntry(uSource, abs(step.script->CastSpell.CreatureEntry), step.script->CastSpell.SearchRadius) : nullptr;
                            break;
                    }

                    if (!uSource || !uSource->isType(TYPEMASK_UNIT))
                    {
                        LOG_ERROR("server", "%s no source unit found for spell %u", step.script->GetDebugInfo().c_str(), step.script->CastSpell.SpellID);
                        break;
                    }

                    if (!uTarget || !uTarget->isType(TYPEMASK_UNIT))
                    {
                        LOG_ERROR("server", "%s no target unit found for spell %u", step.script->GetDebugInfo().c_str(), step.script->CastSpell.SpellID);
                        break;
                    }

                    bool triggered = (step.script->CastSpell.Flags != 4) ?
                                     step.script->CastSpell.CreatureEntry & SF_CASTSPELL_TRIGGERED :
                                     step.script->CastSpell.CreatureEntry < 0;
                    uSource->CastSpell(uTarget, step.script->CastSpell.SpellID, triggered);
                    break;
                }

            case SCRIPT_COMMAND_PLAY_SOUND:
                // Source must be WorldObject.
                if (WorldObject* object = _GetScriptWorldObject(source, true, step.script))
                {
                    // PlaySound.Flags bitmask: 0/1=anyone/target
                    Player* player = nullptr;
                    if (step.script->PlaySound.Flags & SF_PLAYSOUND_TARGET_PLAYER)
                    {
                        // Target must be Player.
                        player = _GetScriptPlayer(target, false, step.script);
                        if (!target)
                            break;
                    }

                    // PlaySound.Flags bitmask: 0/2=without/with distance dependent
                    if (step.script->PlaySound.Flags & SF_PLAYSOUND_DISTANCE_SOUND)
                        object->PlayDistanceSound(step.script->PlaySound.SoundID, player);
                    else
                        object->PlayDirectSound(step.script->PlaySound.SoundID, player);
                }
                break;

            case SCRIPT_COMMAND_CREATE_ITEM:
                // Target or source must be Player.
                if (Player* pReceiver = _GetScriptPlayerSourceOrTarget(source, target, step.script))
                {
                    ItemPosCountVec dest;
                    InventoryResult msg = pReceiver->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, step.script->CreateItem.ItemEntry, step.script->CreateItem.Amount);
                    if (msg == EQUIP_ERR_OK)
                    {
                        if (Item* item = pReceiver->StoreNewItem(dest, step.script->CreateItem.ItemEntry, true))
                            pReceiver->SendNewItem(item, step.script->CreateItem.Amount, false, true);
                    }
                    else
                        pReceiver->SendEquipError(msg, nullptr, nullptr, step.script->CreateItem.ItemEntry);
                }
                break;

            case SCRIPT_COMMAND_DESPAWN_SELF:
                // Target or source must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script, true))
                    cSource->DespawnOrUnsummon(step.script->DespawnSelf.DespawnDelay);
                break;

            case SCRIPT_COMMAND_LOAD_PATH:
                // Source must be Unit.
                if (Unit* unit = _GetScriptUnit(source, true, step.script))
                {
                    if (!sWaypointMgr->GetPath(step.script->LoadPath.PathID))
                        LOG_ERROR("server", "%s source object has an invalid path (%u), skipping.", step.script->GetDebugInfo().c_str(), step.script->LoadPath.PathID);
                    else
                        unit->GetMotionMaster()->MovePath(step.script->LoadPath.PathID, step.script->LoadPath.IsRepeatable);
                }
                break;

            case SCRIPT_COMMAND_CALLSCRIPT_TO_UNIT:
                {
                    if (!step.script->CallScript.CreatureEntry)
                    {
                        LOG_ERROR("server", "%s creature entry is not specified, skipping.", step.script->GetDebugInfo().c_str());
                        break;
                    }
                    if (!step.script->CallScript.ScriptID)
                    {
                        LOG_ERROR("server", "%s script id is not specified, skipping.", step.script->GetDebugInfo().c_str());
                        break;
                    }

                    Creature* cTarget = nullptr;
                    WorldObject* wSource = dynamic_cast <WorldObject*> (source);
                    if (wSource) //using grid searcher
                    {
                        CellCoord p(acore::ComputeCellCoord(wSource->GetPositionX(), wSource->GetPositionY()));
                        Cell cell(p);

                        acore::CreatureWithDbGUIDCheck target_check(step.script->CallScript.CreatureEntry);
                        acore::CreatureSearcher<acore::CreatureWithDbGUIDCheck> checker(wSource, cTarget, target_check);

                        TypeContainerVisitor<acore::CreatureSearcher <acore::CreatureWithDbGUIDCheck>, GridTypeMapContainer > unit_checker(checker);
                        cell.Visit(p, unit_checker, *wSource->GetMap(), *wSource, wSource->GetGridActivationRange());
                    }
                    else //check hashmap holders
                    {
                        if (CreatureData const* data = sObjectMgr->GetCreatureData(step.script->CallScript.CreatureEntry))
                            cTarget = ObjectAccessor::GetObjectInWorld<Creature>(data->mapid, data->posX, data->posY, MAKE_NEW_GUID(step.script->CallScript.CreatureEntry, data->id, HIGHGUID_UNIT), cTarget);
                    }

                    if (!cTarget)
                    {
                        LOG_ERROR("server", "%s target was not found (entry: %u)", step.script->GetDebugInfo().c_str(), step.script->CallScript.CreatureEntry);
                        break;
                    }

                    //Lets choose our ScriptMap map
                    ScriptMapMap* datamap = GetScriptsMapByType(ScriptsType(step.script->CallScript.ScriptType));
                    //if no scriptmap present...
                    if (!datamap)
                    {
                        LOG_ERROR("server", "%s unknown scriptmap (%u) specified, skipping.", step.script->GetDebugInfo().c_str(), step.script->CallScript.ScriptType);
                        break;
                    }

                    // Insert script into schedule but do not start it
                    ScriptsStart(*datamap, step.script->CallScript.ScriptID, cTarget, nullptr);
                    break;
                }

            case SCRIPT_COMMAND_KILL:
                // Source or target must be Creature.
                if (Creature* cSource = _GetScriptCreatureSourceOrTarget(source, target, step.script))
                {
                    if (cSource->isDead())
                        LOG_ERROR("server", "%s creature is already dead (Entry: %u, GUID: %u)",
                                       step.script->GetDebugInfo().c_str(), cSource->GetEntry(), cSource->GetGUIDLow());
                    else
                    {
                        cSource->setDeathState(JUST_DIED);
                        if (step.script->Kill.RemoveCorpse == 1)
                            cSource->RemoveCorpse();
                    }
                }
                break;

            case SCRIPT_COMMAND_ORIENTATION:
                // Source must be Unit.
                if (Unit* sourceUnit = _GetScriptUnit(source, true, step.script))
                {
                    if (step.script->Orientation.Flags & SF_ORIENTATION_FACE_TARGET)
                    {
                        // Target must be Unit.
                        Unit* targetUnit = _GetScriptUnit(target, false, step.script);
                        if (!targetUnit)
                            break;

                        sourceUnit->SetFacingToObject(targetUnit);
                    }
                    else
                        sourceUnit->SetFacingTo(step.script->Orientation.Orientation);
                }
                break;

            case SCRIPT_COMMAND_EQUIP:
                // Source must be Creature.
                if (Creature* cSource = _GetScriptCreature(source, true, step.script))
                    cSource->LoadEquipment(step.script->Equip.EquipmentID);
                break;

            case SCRIPT_COMMAND_MODEL:
                // Source must be Creature.
                if (Creature* cSource = _GetScriptCreature(source, true, step.script))
                    cSource->SetDisplayId(step.script->Model.ModelID);
                break;

            case SCRIPT_COMMAND_CLOSE_GOSSIP:
                // Source must be Player.
                if (Player* player = _GetScriptPlayer(source, true, step.script))
                    player->PlayerTalkClass->SendCloseGossip();
                break;

            case SCRIPT_COMMAND_PLAYMOVIE:
                // Source must be Player.
                if (Player* player = _GetScriptPlayer(source, true, step.script))
                    player->SendMovieStart(step.script->PlayMovie.MovieID);
                break;

            case SCRIPT_COMMAND_MOVEMENT:
                // Source must be Creature.
                if (Creature* cSource = _GetScriptCreature(source, true, step.script))
                {
                    if (!cSource->IsAlive())
                        return;

                    cSource->GetMotionMaster()->MovementExpired();
                    cSource->GetMotionMaster()->MoveIdle();

                    switch (step.script->Movement.MovementType)
                    {
                        case RANDOM_MOTION_TYPE:
                            cSource->GetMotionMaster()->MoveRandom((float)step.script->Movement.MovementDistance);
                            break;
                        case WAYPOINT_MOTION_TYPE:
                            cSource->GetMotionMaster()->MovePath(step.script->Movement.Path, false);
                            break;
                    }
                }
                break;

            default:
                LOG_ERROR("server", "Unknown script command %s.", step.script->GetDebugInfo().c_str());
                break;
        }

        m_scriptSchedule.erase(iter);
        iter = m_scriptSchedule.begin();
        sScriptMgr->DecreaseScheduledScriptCount();
    }
}
