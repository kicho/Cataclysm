/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Opcodes.h"
#include "Log.h"
#include "Corpse.h"
#include "Player.h"
#include "Vehicle.h"
#include "SpellAuras.h"
#include "MapManager.h"
#include "Transports.h"
#include "BattleGround.h"
#include "WaypointMovementGenerator.h"
#include "InstanceSaveMgr.h"
#include "ObjectMgr.h"

void WorldSession::HandleMoveWorldportAckOpcode( WorldPacket & /*recv_data*/ )
{
    DEBUG_LOG( "WORLD: got MSG_MOVE_WORLDPORT_ACK." );
    HandleMoveWorldportAckOpcode();
}

void WorldSession::HandleMoveWorldportAckOpcode()
{
    // ignore unexpected far teleports
    if(!GetPlayer()->IsBeingTeleportedFar())
        return;

    if (_player->GetVehicleKit())
        _player->GetVehicleKit()->RemoveAllPassengers();

    // get the teleport destination
    WorldLocation &loc = GetPlayer()->GetTeleportDest();

    // possible errors in the coordinate validity check
    if(!MapManager::IsValidMapCoord(loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation))
    {
        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: player %s (%d) was teleported far to a not valid location. (map:%u, x:%f, y:%f, "
            "z:%f) We port him to his homebind instead..", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);
        // stop teleportation else we would try this again and again in LogoutPlayer...
        GetPlayer()->SetSemaphoreTeleportFar(false);
        // and teleport the player to a valid place
        GetPlayer()->TeleportToHomebind();
        return;
    }

    // get the destination map entry, not the current one, this will fix homebind and reset greeting
    MapEntry const* mEntry = sMapStore.LookupEntry(loc.mapid);
    InstanceTemplate const* mInstance = ObjectMgr::GetInstanceTemplate(loc.mapid);

    // reset instance validity, except if going to an instance inside an instance
    if(GetPlayer()->m_InstanceValid == false && !mInstance)
        GetPlayer()->m_InstanceValid = true;

    GetPlayer()->SetSemaphoreTeleportFar(false);

    // relocate the player to the teleport destination
    GetPlayer()->SetMap(sMapMgr.CreateMap(loc.mapid, GetPlayer()));
    GetPlayer()->Relocate(loc.coord_x, loc.coord_y, loc.coord_z, loc.orientation);

    GetPlayer()->SendInitialPacketsBeforeAddToMap();
    // the CanEnter checks are done in TeleporTo but conditions may change
    // while the player is in transit, for example the map may get full
    if(!GetPlayer()->GetMap()->Add(GetPlayer()))
    {
        //if player wasn't added to map, reset his map pointer!
        GetPlayer()->ResetMap();

        sLog.outError("WorldSession::HandleMoveWorldportAckOpcode: player %s (%d) was teleported far but couldn't be added to map. (map:%u, x:%f, y:%f, "
            "z:%f) We port him to his homebind instead..", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow(), loc.mapid, loc.coord_x, loc.coord_y, loc.coord_z);
        // teleport the player home
        GetPlayer()->TeleportToHomebind();
        return;
    }

    // battleground state prepare (in case join to BG), at relogin/tele player not invited
    // only add to bg group and object, if the player was invited (else he entered through command)
    if(_player->InBattleGround())
    {
        // cleanup setting if outdated
        if(!mEntry->IsBattleGroundOrArena())
        {
            // We're not in BG
            _player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            // reset destination bg team
            _player->SetBGTeam(0);
        }
        // join to bg case
        else if(BattleGround *bg = _player->GetBattleGround())
        {
            if(_player->IsInvitedForBattleGroundInstance(_player->GetBattleGroundId()))
                bg->AddPlayer(_player);
        }
    }

    GetPlayer()->SendInitialPacketsAfterAddToMap();

    // flight fast teleport case
    if(GetPlayer()->GetMotionMaster()->GetCurrentMovementGeneratorType() == FLIGHT_MOTION_TYPE)
    {
        if(!_player->InBattleGround())
        {
            // short preparations to continue flight
            FlightPathMovementGenerator* flight = (FlightPathMovementGenerator*)(GetPlayer()->GetMotionMaster()->top());
            flight->Reset(*GetPlayer());
            return;
        }

        // battleground state prepare, stop flight
        GetPlayer()->GetMotionMaster()->MovementExpired();
        GetPlayer()->m_taxi.ClearTaxiDestinations();
    }

    // resurrect character at enter into instance where his corpse exist after add to map
    Corpse *corpse = GetPlayer()->GetCorpse();
    if (corpse && corpse->GetType() != CORPSE_BONES && corpse->GetMapId() == GetPlayer()->GetMapId())
    {
        if( mEntry->IsDungeon() )
        {
            GetPlayer()->ResurrectPlayer(0.5f);
            GetPlayer()->SpawnCorpseBones();
        }
    }

    if (mInstance)
    {
        Difficulty diff = GetPlayer()->GetDifficulty(mEntry->IsRaid());
        if(MapDifficulty const* mapDiff = GetMapDifficultyData(mEntry->MapID,diff))
        {
            if (mapDiff->resetTime)
            {
                if (time_t timeReset = sInstanceSaveMgr.GetScheduler().GetResetTimeFor(mEntry->MapID,diff))
                {
                    uint32 timeleft = uint32(timeReset - time(NULL));
                    GetPlayer()->SendInstanceResetWarning(mEntry->MapID, diff, timeleft);
                }
            }
        }
    }

    // mount allow check
    if(!mEntry->IsMountAllowed())
        _player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    // honorless target
    if(GetPlayer()->pvpInfo.inHostileArea)
        GetPlayer()->CastSpell(GetPlayer(), 2479, true);

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMoveTeleportAckOpcode(WorldPacket& recv_data)
{
    DEBUG_LOG("MSG_MOVE_TELEPORT_ACK");

    ObjectGuid guid;

    recv_data >> guid.ReadAsPacked();

    uint32 flags, time;
    recv_data >> flags >> time;
    DEBUG_LOG("Guid: %s", guid.GetString().c_str());
    DEBUG_LOG("Flags %u, time %u", flags, time/IN_MILLISECONDS);

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    if(!plMover || !plMover->IsBeingTeleportedNear())
        return;

    if(guid != plMover->GetObjectGuid())
        return;

    plMover->SetSemaphoreTeleportNear(false);

    uint32 old_zone = plMover->GetZoneId();

    WorldLocation const& dest = plMover->GetTeleportDest();

    plMover->SetPosition(dest.coord_x, dest.coord_y, dest.coord_z, dest.orientation, true);

    uint32 newzone, newarea;
    plMover->GetZoneAndAreaId(newzone, newarea);
    plMover->UpdateZone(newzone, newarea);

    // new zone
    if(old_zone != newzone)
    {
        // honorless target
        if(plMover->pvpInfo.inHostileArea)
            plMover->CastSpell(plMover, 2479, true);
    }

    // resummon pet
    GetPlayer()->ResummonPetTemporaryUnSummonedIfAny();

    //lets process all delayed operations on successful teleport
    GetPlayer()->ProcessDelayedOperations();
}

void WorldSession::HandleMovementOpcodes( WorldPacket & recv_data )
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);
    recv_data.hexlike();

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if(plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    /* extract packet */
    ObjectGuid guid;
    MovementInfo movementInfo;

    recv_data >> guid.ReadAsPacked();
    recv_data >> movementInfo;
    /*----------------*/

    if (!VerifyMovementInfo(movementInfo,guid,mover))
        return;

    // fall damage generation (ignore in flight case that can be triggered also at lags in moment teleportation to another map).
    if (opcode == MSG_MOVE_FALL_LAND && plMover && !plMover->IsTaxiFlying())
        plMover->HandleFall(movementInfo);

    /* process position-change */
    HandleMoverRelocation(movementInfo,mover,plMover);

    if (plMover)
        plMover->UpdateFallInformationIfNeed(movementInfo, opcode);

    // after move info set
    if (opcode == MSG_MOVE_SET_WALK_MODE || opcode == MSG_MOVE_SET_RUN_MODE)
        mover->UpdateWalkMode(mover, false);

    WorldPacket data(opcode, recv_data.size());
    data << mover->GetPackGUID();             // write guid
    movementInfo.Write(data);                               // write data
    mover->SendMessageToSetExcept(&data, _player);
    mover->m_movementInfo = movementInfo;
    mover->SetPosition(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
}

void WorldSession::HandleForceSpeedChangeAckOpcodes(WorldPacket &recv_data)
{
    uint32 opcode = recv_data.GetOpcode();
    DEBUG_LOG("WORLD: Recvd %s (%u, 0x%X) opcode", LookupOpcodeName(opcode), opcode, opcode);
    /* extract packet */
    ObjectGuid guid;
    MovementInfo movementInfo;
    float  newspeed;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();                          // counter or moveEvent
    recv_data >> movementInfo;
    recv_data >> newspeed;

    // now can skip not our packet
    if(_player->GetObjectGuid() != guid)
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }
    /*----------------*/

    // client ACK send one packet for mounted/run case and need skip all except last from its
    // in other cases anti-cheat check can be fail in false case
    UnitMoveType move_type;
    UnitMoveType force_move_type;

    static char const* move_type_name[MAX_MOVE_TYPE] = {  "Walk", "Run", "RunBack", "Swim", "SwimBack", "TurnRate", "Flight", "FlightBack", "PitchRate" };

    switch(opcode)
    {
        case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:          move_type = MOVE_WALK;          force_move_type = MOVE_WALK;        break;
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:           move_type = MOVE_RUN;           force_move_type = MOVE_RUN;         break;
        case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:      move_type = MOVE_RUN_BACK;      force_move_type = MOVE_RUN_BACK;    break;
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:          move_type = MOVE_SWIM;          force_move_type = MOVE_SWIM;        break;
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:     move_type = MOVE_SWIM_BACK;     force_move_type = MOVE_SWIM_BACK;   break;
        case CMSG_FORCE_TURN_RATE_CHANGE_ACK:           move_type = MOVE_TURN_RATE;     force_move_type = MOVE_TURN_RATE;   break;
        case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:        move_type = MOVE_FLIGHT;        force_move_type = MOVE_FLIGHT;      break;
        case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:   move_type = MOVE_FLIGHT_BACK;   force_move_type = MOVE_FLIGHT_BACK; break;
        case CMSG_FORCE_PITCH_RATE_CHANGE_ACK:          move_type = MOVE_PITCH_RATE;    force_move_type = MOVE_PITCH_RATE;  break;
        default:
            sLog.outError("WorldSession::HandleForceSpeedChangeAck: Unknown move type opcode: %u", opcode);
            return;
    }

    // skip all forced speed changes except last and unexpected
    // in run/mounted case used one ACK and it must be skipped.m_forced_speed_changes[MOVE_RUN} store both.
    if(_player->m_forced_speed_changes[force_move_type] > 0)
    {
        --_player->m_forced_speed_changes[force_move_type];
        if(_player->m_forced_speed_changes[force_move_type] > 0)
            return;
    }

    if (!_player->GetTransport() && fabs(_player->GetSpeed(move_type) - newspeed) > 0.01f)
    {
        if(_player->GetSpeed(move_type) > newspeed)         // must be greater - just correct
        {
            sLog.outError("%sSpeedChange player %s is NOT correct (must be %f instead %f), force set to correct value",
                move_type_name[move_type], _player->GetName(), _player->GetSpeed(move_type), newspeed);
            _player->SetSpeedRate(move_type,_player->GetSpeedRate(move_type),true);
        }
        else                                                // must be lesser - cheating
        {
            BASIC_LOG("Player %s from account id %u kicked for incorrect speed (must be %f instead %f)",
                _player->GetName(),_player->GetSession()->GetAccountId(),_player->GetSpeed(move_type), newspeed);
            _player->GetSession()->KickPlayer();
        }
    }
}

void WorldSession::HandleSetActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_SET_ACTIVE_MOVER");
    recv_data.hexlike();

    ObjectGuid guid;
    recv_data >> guid;

    if (Unit *pMover = ObjectAccessor::GetUnit(*GetPlayer(), guid))
        GetPlayer()->SetMover(pMover);
    else
        GetPlayer()->SetMover(NULL);
}

void WorldSession::HandleMoveNotActiveMoverOpcode(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_MOVE_NOT_ACTIVE_MOVER");
    recv_data.hexlike();

    ObjectGuid guid;
    MovementInfo mi;

    recv_data >> guid.ReadAsPacked();
    recv_data >> mi;

    GetPlayer()->m_movementInfo = mi;
}

void WorldSession::HandleDismissControlledVehicle(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_DISMISS_CONTROLLED_VEHICLE");
    recv_data.hexlike();

    ObjectGuid guid;
    MovementInfo mi;

    recv_data >> guid.ReadAsPacked();
    recv_data >> mi;

    ObjectGuid vehicleGUID = _player->GetVehicleGUID();

    if (vehicleGUID.IsEmpty())                              // something wrong here...
        return;

    _player->m_movementInfo = mi;

    if(Vehicle *vehicle = ObjectAccessor::GetVehicle(vehicleGUID))
    {
        if(vehicle->GetVehicleFlags() & VF_DESPAWN_AT_LEAVE)
            vehicle->Dismiss();
        else
            _player->ExitVehicle();
    }
}

void WorldSession::HandleRequestVehicleExit(WorldPacket &recv_data)
{
    sLog.outDebug("WORLD: Recvd CMSG_REQUEST_VEHICLE_EXIT");
    recv_data.hexlike();

    GetPlayer()->ExitVehicle();
}

void WorldSession::HandleRequestVehiclePrevSeat(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_REQUEST_VEHICLE_PREV_SEAT");
    recv_data.hexlike();

    GetPlayer()->ChangeSeat(-1, false);
}

void WorldSession::HandleRequestVehicleNextSeat(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_REQUEST_VEHICLE_NEXT_SEAT");
    recv_data.hexlike();

    GetPlayer()->ChangeSeat(-1, true);
}

void WorldSession::HandleRequestVehicleSwitchSeat(WorldPacket &recv_data)
{
    sLog.outDebug("WORLD: Recvd CMSG_REQUEST_VEHICLE_SWITCH_SEAT");
    recv_data.hexlike();

    uint64 vehicleGUID = _player->GetVehicleGUID();

    if(!vehicleGUID)                                        // something wrong here...
        return;

    if(Vehicle *vehicle = ObjectAccessor::GetVehicle(vehicleGUID))
    {
        ObjectGuid guid;
        recv_data >> guid.ReadAsPacked();

        int8 seatId = 0;
        recv_data >> seatId;

        if(!guid.IsEmpty())
        {
            if(vehicleGUID != guid.GetRawValue())
            {
                if(Vehicle *veh = ObjectAccessor::GetVehicle(guid.GetRawValue()))
                {
                    if(!_player->IsWithinDistInMap(veh, 10))
                        return;

                    if(Vehicle *v = veh->FindFreeSeat(&seatId, false))
                    {
                        vehicle->RemovePassenger(_player);
                        _player->EnterVehicle(v, seatId, false);
                    }
                }
                return;
            }
        }
        if(Vehicle *v = vehicle->FindFreeSeat(&seatId, false))
        {
            vehicle->RemovePassenger(_player);
            _player->EnterVehicle(v, seatId, false);
        }
    }
}

void WorldSession::HandleChangeSeatsOnControlledVehicle(WorldPacket &recv_data)
{
    sLog.outDebug("WORLD: Recvd CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE");
    recv_data.hexlike();

    uint64 vehicleGUID = _player->GetVehicleGUID();

    if(!vehicleGUID)                                        // something wrong here...
        return;

    if(recv_data.GetOpcode() == CMSG_REQUEST_VEHICLE_PREV_SEAT)
    {
        _player->ChangeSeat(-1, false);
        return;
    }
    else if(recv_data.GetOpcode() == CMSG_REQUEST_VEHICLE_NEXT_SEAT)
    {
        _player->ChangeSeat(-1, true);
        return;
    }

    ObjectGuid guid, guid2;
    recv_data >> guid.ReadAsPacked();

    MovementInfo mi;
    recv_data >> mi;
    _player->m_movementInfo = mi;

    recv_data >> guid2.ReadAsPacked(); //guid of vehicle or of vehicle in target seat

    int8 seatId;
    recv_data >> seatId;

    if(guid.GetRawValue() == guid2.GetRawValue())
        _player->ChangeSeat(seatId, false);
    else if(Vehicle *vehicle = ObjectAccessor::GetVehicle(guid2.GetRawValue()))
    {
        if(vehicle->HasEmptySeat(seatId))
        {
            _player->ExitVehicle();
            _player->EnterVehicle(vehicle, seatId);
        }
    }
}

void WorldSession::HandleEnterPlayerVehicle(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_PLAYER_VEHICLE_ENTER");
    recv_data.hexlike();

    ObjectGuid guid;
    recv_data >> guid;

    if (Player* pl = ObjectAccessor::FindPlayer(guid))
    {
        if (!pl->GetVehicleKit())
            return;
        if (!pl->IsInSameRaidWith(GetPlayer()))
            return;
        if (!pl->IsWithinDistInMap(GetPlayer(), INTERACTION_DISTANCE))
            return;
        if (pl->GetTransport())
            return;
        GetPlayer()->EnterVehicle(pl->GetVehicleKit());
    }
}

void WorldSession::HandleEjectPasenger(WorldPacket &recv_data)
{
    DEBUG_LOG("WORLD: Recvd CMSG_EJECT_PASSENGER");
    recv_data.hexlike();

    if(recv_data.GetOpcode()==CMSG_EJECT_PASSENGER)
    {
        if (GetPlayer()->GetVehicleKit())
        {
            ObjectGuid guid;
            recv_data >> guid;
            if(Player* Pl = ObjectAccessor::FindPlayer(guid))
                Pl->ExitVehicle();
        }
    }
}

void WorldSession::HandleMountSpecialAnimOpcode(WorldPacket& /*recvdata*/)
{
    //DEBUG_LOG("WORLD: Recvd CMSG_MOUNTSPECIAL_ANIM");

    WorldPacket data(SMSG_MOUNTSPECIAL_ANIM, 8);
    data << GetPlayer()->GetObjectGuid();

    GetPlayer()->SendMessageToSet(&data, false);
}

void WorldSession::HandleMoveKnockBackAck( WorldPacket & recv_data )
{
    DEBUG_LOG("CMSG_MOVE_KNOCK_BACK_ACK");

    Unit *mover = _player->GetMover();
    Player *plMover = mover->GetTypeId() == TYPEID_PLAYER ? (Player*)mover : NULL;

    // ignore, waiting processing in WorldSession::HandleMoveWorldportAckOpcode and WorldSession::HandleMoveTeleportAck
    if(plMover && plMover->IsBeingTeleported())
    {
        recv_data.rpos(recv_data.wpos());                   // prevent warnings spam
        return;
    }

    ObjectGuid guid;
    MovementInfo movementInfo;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();                          // knockback packets counter
    recv_data >> movementInfo;

    if (!VerifyMovementInfo(movementInfo,guid,mover))
        return;

    HandleMoverRelocation(movementInfo, mover, plMover);

    WorldPacket data(MSG_MOVE_KNOCK_BACK, recv_data.size() + 15);
    data << mover->GetPackGUID();
    data << movementInfo;
    data << movementInfo.GetJumpInfo().sinAngle;
    data << movementInfo.GetJumpInfo().cosAngle;
    data << movementInfo.GetJumpInfo().xyspeed;
    data << movementInfo.GetJumpInfo().velocity;
    mover->SendMessageToSetExcept(&data, _player);
}

void WorldSession::HandleMoveHoverAck( WorldPacket& recv_data )
{
    DEBUG_LOG("CMSG_MOVE_HOVER_ACK");

    ObjectGuid guid;                                        // guid - unused
    MovementInfo movementInfo;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();                          // unk1
    recv_data >> movementInfo;
    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleMoveWaterWalkAck(WorldPacket& recv_data)
{
    DEBUG_LOG("CMSG_MOVE_WATER_WALK_ACK");

    ObjectGuid guid;                                        // guid - unused
    MovementInfo movementInfo;

    recv_data >> guid.ReadAsPacked();
    recv_data >> Unused<uint32>();                          // unk1
    recv_data >> movementInfo;
    recv_data >> Unused<uint32>();                          // unk2
}

void WorldSession::HandleSummonResponseOpcode(WorldPacket& recv_data)
{
    if(!_player->isAlive() || _player->isInCombat() )
        return;

    uint64 summoner_guid;
    bool agree;
    recv_data >> summoner_guid;
    recv_data >> agree;

    _player->SummonIfPossible(agree);
}

bool WorldSession::VerifyMovementInfo(MovementInfo& movementInfo, ObjectGuid& guid, Unit* mover) const
{
    // ignore wrong guid (player attempt cheating own session for not own guid possible...)
    if (guid != mover->GetObjectGuid())
        return false;

    if (!MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o))
        return false;

    if((movementInfo.HasMovementFlag (MOVEFLAG_ONTRANSPORT)) && (movementInfo.HasMovementFlag (MOVEFLAG_ROOT)))
    {
        if(mover->GetVehicle() && mover->GetVehicleGUID() && mover->GetTypeId()==TYPEID_PLAYER)
        {
            _player->ExitVehicle();
        }
    }
    else if (movementInfo.HasMovementFlag (MOVEFLAG_ONTRANSPORT) && !mover->GetVehicle() && !mover->GetVehicleGUID())
    {
        // transports size limited
        // (also received at zeppelin/lift leave by some reason with t_* as absolute in continent coordinates, can be safely skipped)
        if( movementInfo.GetTransportPos()->x > 50 || movementInfo.GetTransportPos()->y > 50 || movementInfo.GetTransportPos()->z > 100 )
            return false;

        if( !MaNGOS::IsValidMapCoord(movementInfo.GetPos()->x + movementInfo.GetTransportPos()->x, movementInfo.GetPos()->y + movementInfo.GetTransportPos()->y,
            movementInfo.GetPos()->z + movementInfo.GetTransportPos()->z, movementInfo.GetPos()->o + movementInfo.GetTransportPos()->o) )
        {
            return false;
        }
    }

    return true;
}

void WorldSession::HandleMoverRelocation(MovementInfo& movementInfo, Unit* mover, Player* plMover)
{
    movementInfo.UpdateTime(getMSTime());

    if (plMover)
    {
        if (movementInfo.HasMovementFlag(MOVEFLAG_ONTRANSPORT))
        {
            if (!plMover->m_transport)
            {
                // elevators also cause the client to send MOVEFLAG_ONTRANSPORT - just unmount if the guid can be found in the transport list
                for (MapManager::TransportSet::const_iterator iter = sMapMgr.m_Transports.begin(); iter != sMapMgr.m_Transports.end(); ++iter)
                {
                    if ((*iter)->GetObjectGuid() == movementInfo.GetTransportGuid())
                    {
                        plMover->m_transport = (*iter);
                        (*iter)->AddPassenger(plMover);

                        if (plMover->GetVehicleKit())
                            plMover->GetVehicleKit()->RemoveAllPassengers();
                        break;
                    }
                }
            }
        }
        else if (plMover->m_transport)               // if we were on a transport, leave
        {
            plMover->m_transport->RemovePassenger(plMover);
            plMover->m_transport = NULL;
            movementInfo.ClearTransportData();
        }

        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING) != plMover->IsInWater())
        {
            // now client not include swimming flag in case jumping under water
            plMover->SetInWater( !plMover->IsInWater() || plMover->GetBaseMap()->IsUnderWater(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z) );
        }
        if (movementInfo.HasMovementFlag(MOVEFLAG_SWIMMING))
        {
            if(mover->GetTypeId() == TYPEID_UNIT)
            {
                if(((Creature*)mover)->IsVehicle() && !((Creature*)mover)->CanSwim())
                {
                    // NOTE : we should enter evade mode here, but...
                    ((Vehicle*)mover)->SetSpawnDuration(1);
                }
            }
        }

        plMover->SetPosition(movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
        plMover->m_movementInfo = movementInfo;

        if(movementInfo.GetPos()->z < -500.0f)
        {
            if(plMover->InBattleGround()
                && plMover->GetBattleGround()
                && plMover->GetBattleGround()->HandlePlayerUnderMap(_player))
            {
                // do nothing, the handle already did if returned true
            }
            else
            {
                // NOTE: this is actually called many times while falling
                // even after the player has been teleported away
                // TODO: discard movement packets after the player is rooted
                if(plMover->isAlive())
                {
                    plMover->EnvironmentalDamage(DAMAGE_FALL_TO_VOID, plMover->GetMaxHealth());
                    // pl can be alive if GM/etc
                    if(!plMover->isAlive())
                    {
                        // change the death state to CORPSE to prevent the death timer from
                        // starting in the next player update
                        plMover->KillPlayer();
                        plMover->BuildPlayerRepop();
                    }
                }

                // cancel the death timer here if started
                plMover->RepopAtGraveyard();
            }
        }
    }
    else                                                    // creature charmed
    {
        if (mover->IsInWorld())
        {
            mover->GetMap()->CreatureRelocation((Creature*)mover, movementInfo.GetPos()->x, movementInfo.GetPos()->y, movementInfo.GetPos()->z, movementInfo.GetPos()->o);
            if(((Creature*)mover)->IsVehicle())
                ((Vehicle*)mover)->RellocatePassengers(mover->GetMap());
        }
    }
}
