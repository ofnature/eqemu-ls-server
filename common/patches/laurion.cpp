/*	EQEMu: Everquest Server Emulator

	Copyright (C) 2001-2016 EQEMu Development Team (http://eqemulator.net)

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; version 2 of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY except by those people which sell it, which
	are required to give you total support for your newly bought product;
	without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "../global_define.h"
#include "../eqemu_config.h"
#include "../eqemu_logsys.h"
#include "laurion.h"
#include "../opcodemgr.h"

#include "../eq_stream_ident.h"
#include "../crc32.h"

#include "../eq_packet_structs.h"
#include "../misc_functions.h"
#include "../strings.h"
#include "../inventory_profile.h"
#include "laurion_structs.h"
#include "../rulesys.h"
#include "../path_manager.h"
#include "../classes.h"
#include "../races.h"
#include "../raid.h"

#include <iostream>
#include <sstream>
#include <numeric>
#include <cassert>
#include <cinttypes>
#include <algorithm>
#include <cstring>

namespace Laurion
{
	static const char* name = "Laurion";
	static OpcodeManager* opcodes = nullptr;
	static Strategy struct_strategy;

	// Global logger for encoding - helps identify unmapped opcodes
	void LogEncode(const EQApplicationPacket* app, const char* funcname) {
		if (!app) return;
		// [DH_LOG_CLEANUP_2] Per-packet [LAURION_ENCODE] for size>500 disabled.
		// Log all large packets and any packet that might be suspicious
		// if (app->size > 500) {
		// 	Log(Logs::General, Logs::Error,
		// 		"[LAURION_ENCODE] %s | EmuOpcode: %d (%s) | Size: %d bytes",
		// 		funcname,
		// 		app->GetOpcode(),
		// 		opcodes ? opcodes->EmuToName(app->GetOpcode()) : "Unknown",
		// 		app->size);
		// }
		(void)funcname;
	}

	void SerializeItem(SerializeBuffer &buffer, const EQ::ItemInstance* inst, int16 slot_id, uint8 depth, ItemPacketType packet_type, bool prepend_dh_count = true);

	// message link converters
	static inline void ServerToLaurionConvertLinks(std::string& message_out, const std::string& message_in);
	static inline void LaurionToServerConvertLinks(std::string& message_out, const std::string& message_in);

	// SpawnAppearance
	static inline uint32 ServerToLaurionSpawnAppearanceType(uint32 server_type);
	static inline uint32 LaurionToServerSpawnAppearanceType(uint32 laurion_type);

	// server to client inventory location converters
	static inline structs::InventorySlot_Struct ServerToLaurionSlot(uint32 server_slot);
	static inline structs::InventorySlot_Struct ServerToLaurionCorpseSlot(uint32 server_corpse_slot);
	static inline uint32 ServerToLaurionCorpseMainSlot(uint32 server_corpse_slot);
	static inline structs::TypelessInventorySlot_Struct ServerToLaurionTypelessSlot(uint32 server_slot, int16 server_type);

	// client to server inventory location converters
	static inline uint32 LaurionToServerSlot(structs::InventorySlot_Struct laurion_slot);
	static inline uint32 LaurionToServerCorpseSlot(structs::InventorySlot_Struct laurion_corpse_slot);
	static inline uint32 LaurionToServerCorpseMainSlot(uint32 laurion_corpse_slot);
	static inline uint32 LaurionToServerTypelessSlot(structs::TypelessInventorySlot_Struct laurion_slot, int16 laurion_type);
	static inline structs::InventorySlot_Struct LaurionCastingInventorySlotToInventorySlot(structs::CastSpellInventorySlot_Struct laurion_slot);
	static inline structs::CastSpellInventorySlot_Struct LaurionInventorySlotToCastingInventorySlot(structs::InventorySlot_Struct laurion_slot);

	// Item packet types
	static item::ItemPacketType ServerToLaurionItemPacketType(ItemPacketType laurion_type);

	// casting slots
	static inline spells::CastingSlot ServerToLaurionCastingSlot(EQ::spells::CastingSlot slot);
	static inline EQ::spells::CastingSlot LaurionToServerCastingSlot(spells::CastingSlot slot);

	// buff slots
	static inline int ServerToLaurionBuffSlot(int index);
	static inline int LaurionToServerBuffSlot(int index);

	void Register(EQStreamIdentifier& into)
	{
		//create our opcode manager if we havent already
		if (opcodes == nullptr) {

			std::string opfile = fmt::format("{}/patch_{}.conf", path.GetPatchPath(), name);

			//load up the opcode manager.
			//TODO: figure out how to support shared memory with multiple patches...
			opcodes = new RegularOpcodeManager();
			if (!opcodes->LoadOpcodes(opfile.c_str())) {
				LogNetcode("[OPCODES] Error loading opcodes file [{}]. Not registering patch [{}]", opfile.c_str(), name);
				return;
			}
		}

		//ok, now we have what we need to register.

		EQStreamInterface::Signature signature;
		std::string pname;

		//register our world signature.
		pname = std::string(name) + "_world";
		signature.ignore_eq_opcode = 0;
		signature.first_length = sizeof(structs::LoginInfo_Struct);
		signature.first_eq_opcode = opcodes->EmuToEQ(OP_SendLoginInfo);
		into.RegisterPatch(signature, pname.c_str(), &opcodes, &struct_strategy);

		//register our zone signature.
		pname = std::string(name) + "_zone";
		signature.ignore_eq_opcode = opcodes->EmuToEQ(OP_AckPacket);
		signature.first_length = sizeof(structs::ClientZoneEntry_Struct);
		signature.first_eq_opcode = opcodes->EmuToEQ(OP_ZoneEntry);
		into.RegisterPatch(signature, pname.c_str(), &opcodes, &struct_strategy);
		LogNetcode("[StreamIdentify] Registered patch [{}]", name);
	}

	void Reload()
	{
		//we have a big problem to solve here when we switch back to shared memory
		//opcode managers because we need to change the manager pointer, which means
		//we need to go to every stream and replace it's manager.

		if (opcodes != nullptr) {
			std::string opfile = fmt::format("{}/patch_{}.conf", path.GetPatchPath(), name);
			if (!opcodes->ReloadOpcodes(opfile.c_str())) {
				LogNetcode("[OPCODES] Error reloading opcodes file [{}] for patch [{}]", opfile.c_str(), name);
				return;
			}
			LogNetcode("[OPCODES] Reloaded opcodes for patch [{}]", name);
		}
	}

	void SerializeOneDragonHoardItem(SerializeBuffer& buffer, const EQ::ItemInstance* inst, int16 slot_id)
	{
		SerializeItem(buffer, inst, slot_id, 0, ItemPacketType::ItemPacketDragonHoard, false);
	}

	Strategy::Strategy() : StructStrategy()
	{
		//all opcodes default to passthrough.
#include "ss_register.h"
#include "laurion_ops.h"
	}

	std::string Strategy::Describe() const
	{
		std::string r;
		r += "Patch ";
		r += name;
		return(r);
	}

	const EQ::versions::ClientVersion Strategy::ClientVersion() const
	{
		return EQ::versions::ClientVersion::Laurion;
	}

#include "ss_define.h"

	// ENCODE methods
	ENCODE(OP_Action) {
		ENCODE_LENGTH_EXACT(Action_Struct);
		SETUP_DIRECT_ENCODE(Action_Struct, structs::MissileHitInfo);

		//This is mostly figured out; there's two unknowns, only unknown1 is read by the client
		OUT(target);
		OUT(source);
		eq->spell_id = emu->spell;
		eq->effect_type = emu->effect_flag;
		eq->effective_casting_level = 0; //if you set this to != 0 it will use this level instead of calculating it
		eq->unknown1 = 0;
		eq->unknown2 = 0;
		eq->damage = 0; //client doesn't read this but live sends it here, can just set 0
		eq->modifier = 1.0f + (emu->instrument_mod - 10) / 10.0f;
		OUT(force);
		OUT(hit_heading);
		OUT(hit_pitch);
		eq->skill = emu->type;
		OUT(level);

		FINISH_ENCODE();
	}

	ENCODE(OP_Animation)
	{
		ENCODE_LENGTH_EXACT(Animation_Struct);
		SETUP_DIRECT_ENCODE(Animation_Struct, structs::Animation_Struct);

		OUT(spawnid);
		OUT(action);
		OUT(speed);

		FINISH_ENCODE();
	}

	ENCODE(OP_ApplyPoison)
	{
		ENCODE_LENGTH_EXACT(ApplyPoison_Struct);
		SETUP_DIRECT_ENCODE(ApplyPoison_Struct, structs::ApplyPoison_Struct);

		eq->inventorySlot = ServerToLaurionTypelessSlot(emu->inventorySlot, EQ::invtype::typePossessions);
		OUT(success);

		FINISH_ENCODE();
	}

	ENCODE(OP_AugmentInfo)
	{
		ENCODE_LENGTH_EXACT(AugmentInfo_Struct);
		SETUP_DIRECT_ENCODE(AugmentInfo_Struct, structs::AugmentInfo_Struct);

		OUT(itemid);
		OUT(window);
		strn0cpy(eq->augment_info, emu->augment_info, 64);

		FINISH_ENCODE();
	}

	ENCODE(OP_BeginCast)
	{
		ENCODE_LENGTH_EXACT(BeginCast_Struct);
		SETUP_DIRECT_ENCODE(BeginCast_Struct, structs::BeginCast_Struct);

		OUT(spell_id);
		OUT(caster_id);
		OUT(cast_time);
		eq->unknown0e = 1; //not sure what this is; but its usually 1 on live

		FINISH_ENCODE();
	}

	ENCODE(OP_BlockedBuffs)
	{
		ENCODE_LENGTH_EXACT(BlockedBuffs_Struct);
		SETUP_DIRECT_ENCODE(BlockedBuffs_Struct, structs::BlockedBuffs_Struct);

		for (uint32 i = 0; i < BLOCKED_BUFF_COUNT; ++i)
			eq->SpellID[i] = emu->SpellID[i];

		for (uint32 i = BLOCKED_BUFF_COUNT; i < structs::BLOCKED_BUFF_COUNT; ++i)
			eq->SpellID[i] = -1;

		OUT(Count);
		OUT(Pet);
		OUT(Initialise);
		OUT(Flags);

		FINISH_ENCODE();
	}

	ENCODE(OP_Buff)
	{
		ENCODE_LENGTH_EXACT(SpellBuffPacket_Struct);
		SETUP_DIRECT_ENCODE(SpellBuffPacket_Struct, structs::EQAffectPacket_Struct);

		eq->entity_id = emu->entityid;
		eq->unknown004 = 0;

		//fill in affect info
		eq->affect.caster_id.Id = emu->buff.player_id;
		eq->affect.flags = 0;
		eq->affect.spell_id = emu->buff.spellid;
		eq->affect.duration = emu->buff.duration;
		eq->affect.initial_duration = emu->buff.duration;
		eq->affect.hit_count = emu->buff.num_hits;
		eq->affect.viral_timer = 0;
		eq->affect.modifier = emu->buff.bard_modifier == 10 ? 1.0f : emu->buff.bard_modifier / 10.0f;
		eq->affect.y = emu->buff.y;
		eq->affect.x = emu->buff.x;
		eq->affect.z = emu->buff.z;
		eq->affect.level = emu->buff.level;

		eq->slot_id = ServerToLaurionBuffSlot(emu->slotid);
		if (emu->bufffade == 1)
		{
			eq->buff_fade = 1;
		}
		else
		{
			eq->buff_fade = 2;
		}

		EQApplicationPacket* outapp = nullptr;
		if (emu->bufffade == 1)
		{
			// Bit of a hack. OP_Buff appears to add/remove the buff while OP_BuffCreate adds/removes the actual buff icon
			outapp = new EQApplicationPacket(OP_BuffCreate, 30);
			outapp->WriteUInt32(emu->entityid);
			outapp->WriteUInt32(0);	// tic timer
			outapp->WriteUInt8(0);		// Type of OP_BuffCreate packet ?
			outapp->WriteUInt16(1);		// 1 buff in this packet
			outapp->WriteUInt32(ServerToLaurionBuffSlot(emu->slotid));
			outapp->WriteUInt32(0xffffffff);		// SpellID (0xffff to remove)
			outapp->WriteUInt32(0);			// Duration
			outapp->WriteUInt32(0);			// numhits
			outapp->WriteUInt8(0);		// Caster name
			outapp->WriteUInt8(0);		// Type
			outapp->WriteUInt8(0);		// Type
		}

		FINISH_ENCODE();

		if (outapp) {
			dest->FastQueuePacket(&outapp);
		}
	}

	ENCODE(OP_BuffCreate)
	{
		SETUP_VAR_ENCODE(BuffIcon_Struct);

		//Laurion has one extra 0x00 byte before the end byte
		uint32 sz = 13 + (17 * emu->count) + emu->name_lengths; // 17 includes nullterm
		__packet->size = sz;
		__packet->pBuffer = new unsigned char[sz];
		memset(__packet->pBuffer, 0, sz);

		__packet->WriteUInt32(emu->entity_id);
		__packet->WriteUInt32(emu->tic_timer);
		__packet->WriteUInt8(emu->all_buffs);			// 1 indicates all buffs on the player (0 to add or remove a single buff)
		__packet->WriteUInt16(emu->count);

		for (int i = 0; i < emu->count; ++i)
		{
			__packet->WriteUInt32(emu->type == 0 ? ServerToLaurionBuffSlot(emu->entries[i].buff_slot) : emu->entries[i].buff_slot);
			__packet->WriteUInt32(emu->entries[i].spell_id);
			__packet->WriteUInt32(emu->entries[i].tics_remaining);
			__packet->WriteUInt32(emu->entries[i].num_hits); // Unknown
			__packet->WriteString(emu->entries[i].caster);
		}
		__packet->WriteUInt8(0); // Unknown1
		__packet->WriteUInt8(emu->type); // Unknown2

		FINISH_ENCODE();
	}

	ENCODE(OP_CancelTrade)
	{
		ENCODE_LENGTH_EXACT(CancelTrade_Struct);
		SETUP_DIRECT_ENCODE(CancelTrade_Struct, structs::CancelTrade_Struct);

		OUT(fromid);
		OUT(action);

		FINISH_ENCODE();
	}

	ENCODE(OP_CastSpell)
	{
		ENCODE_LENGTH_EXACT(CastSpell_Struct);
		SETUP_DIRECT_ENCODE(CastSpell_Struct, structs::CastSpell_Struct);

		eq->slot = static_cast<uint32>(ServerToLaurionCastingSlot(static_cast<EQ::spells::CastingSlot>(emu->slot)));

		OUT(spell_id);
		OUT(inventoryslot);
		OUT(target_id);
		// cs_unknown array is left as zeros

		FINISH_ENCODE();
	}

	ENCODE(OP_ChannelMessage)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;

		ChannelMessage_Struct* emu = (ChannelMessage_Struct*)in->pBuffer;

		unsigned char* __emu_buffer = in->pBuffer;

		std::string old_message = emu->message;
		std::string new_message;
		ServerToLaurionConvertLinks(new_message, old_message);

		in->size = strlen(emu->sender) + strlen(emu->targetname) + new_message.length() + 43;

		in->pBuffer = new unsigned char[in->size];

		char* OutBuffer = (char*)in->pBuffer;

		VARSTRUCT_ENCODE_STRING(OutBuffer, emu->sender);
		VARSTRUCT_ENCODE_STRING(OutBuffer, emu->targetname);
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, emu->language);
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, emu->chan_num);
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint8, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, emu->skill_in_language);
		VARSTRUCT_ENCODE_STRING(OutBuffer, new_message.c_str());

		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint32, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint16, OutBuffer, 0);	// Unknown
		VARSTRUCT_ENCODE_TYPE(uint8, OutBuffer, 0);	// Unknown

		delete[] __emu_buffer;
		dest->FastQueuePacket(&in, ack_req);
	}

	ENCODE(OP_CharInventory) {
		// Consume the packet
		EQApplicationPacket* in = *p;
		*p = nullptr;

		LogEncode(in, "OP_CharInventory");

		// Handle empty inventory case
		if (!in->size) {
			in->size = 4;
			in->pBuffer = new uchar[in->size];
			memset(in->pBuffer, 0, in->size);
			dest->FastQueuePacket(&in, ack_req);
			return;
		}

		// Store away the emu struct
		uchar* __emu_buffer = in->pBuffer;

		// Calculate item count - 64-bit client uses InternalSerializedItem_Struct
		// which contains: int16 slot_id (2 bytes) + const void* inst (8 bytes on 64-bit) = 16 bytes per item
		const size_t struct_size = sizeof(EQ::InternalSerializedItem_Struct);
		int item_count = in->size / struct_size;

		// Validate packet size alignment
		if (!item_count || (in->size % struct_size) != 0) {
			Log(Logs::General, Logs::Netcode,
				"[STRUCTS] Wrong size on outbound %s: Got %d, expected multiple of %d (struct_size)",
				opcodes->EmuToName(in->GetOpcode()), in->size, struct_size);
			delete in;
			return;
		}

		// Cast to internal struct array
		EQ::InternalSerializedItem_Struct* eq = (EQ::InternalSerializedItem_Struct*)in->pBuffer;

		// Create serialize buffer for 64-bit client
		SerializeBuffer buffer;
		buffer.WriteUInt32(item_count);

		// Serialize each item for the 64-bit client format
		for (int index = 0; index < item_count; ++index, ++eq) {
			// Validate pointer before dereferencing
			if (!eq->inst) {
				Log(Logs::General, Logs::Netcode,
					"[STRUCTS] NULL item instance at index %d in OP_CharInventory", index);
				continue;
			}

			SerializeItem(buffer, (const EQ::ItemInstance*)eq->inst, eq->slot_id, 0, ItemPacketCharInventory);
		}

		// Replace packet buffer with serialized data
		in->size = buffer.size();
		in->pBuffer = new unsigned char[in->size];
		memcpy(in->pBuffer, buffer.buffer(), in->size);

		// Clean up old buffer
		delete[] __emu_buffer;

		// Send to client
		dest->FastQueuePacket(&in, ack_req);
	}

	ENCODE(OP_ClickObjectAction)
	{
		ENCODE_LENGTH_EXACT(ClickObjectAction_Struct);
		SETUP_DIRECT_ENCODE(ClickObjectAction_Struct, structs::ClickObjectAction_Struct);

		OUT(drop_id);
		eq->unknown04 = -1;
		eq->unknown08 = -1;
		OUT(type);
		OUT(icon);
		eq->unknown16 = 0;
		OUT_str(object_name);

		FINISH_ENCODE();
	}

	ENCODE(OP_ClientUpdate)
	{
		ENCODE_LENGTH_EXACT(PlayerPositionUpdateServer_Struct);
		SETUP_DIRECT_ENCODE(PlayerPositionUpdateServer_Struct, structs::PlayerPositionUpdateServer_Struct);

		OUT(spawn_id);
		OUT(vehicle_id);
		eq->position.x = emu->x_pos;
		eq->position.y = emu->y_pos;
		eq->position.z = emu->z_pos;
		eq->position.heading = emu->heading;
		eq->position.deltaX = emu->delta_x;
		eq->position.deltaY = emu->delta_y;
		eq->position.deltaZ = emu->delta_z;
		eq->position.deltaHeading = emu->delta_heading;
		eq->position.animation = emu->animation;

		FINISH_ENCODE();
	}

	ENCODE(OP_Consider)
	{
		ENCODE_LENGTH_EXACT(Consider_Struct);
		SETUP_DIRECT_ENCODE(Consider_Struct, structs::Consider_Struct);

		OUT(playerid);
		OUT(targetid);
		OUT(faction);
		OUT(level);

		FINISH_ENCODE();
	}

	ENCODE(OP_Damage) {
		SETUP_DIRECT_ENCODE(CombatDamage_Struct, structs::CombatDamage_Struct);

		OUT(target);
		OUT(source);
		OUT(type);
		OUT(spellid);
		OUT(damage);
		OUT(force);
		OUT(hit_heading);
		OUT(hit_pitch);
		OUT(special);

		FINISH_ENCODE();
	}

	ENCODE(OP_Death)
	{
		ENCODE_LENGTH_EXACT(Death_Struct);
		SETUP_DIRECT_ENCODE(Death_Struct, structs::Death_Struct);

		OUT(spawn_id);
		OUT(killer_id);
		OUT(spell_id);
		OUT(attack_skill);
		OUT(damage);

		//This is a hack, we need to actually fix the ordering in source as this wont respect filters etc
		if (emu->attack_skill != 231) {
			auto combat_packet = new EQApplicationPacket(OP_Damage, sizeof(structs::CombatDamage_Struct));
			structs::CombatDamage_Struct* cds = (structs::CombatDamage_Struct*)combat_packet->pBuffer;

			cds->target = emu->spawn_id;
			cds->source = emu->killer_id;
			cds->type = emu->attack_skill;
			cds->damage = emu->damage;
			cds->spellid = -1;

			dest->FastQueuePacket(&combat_packet, ack_req);
		}

		FINISH_ENCODE();
	}

	ENCODE(OP_DeleteCharge)
	{
		Log(Logs::Detail, Logs::Netcode, "Laurion::ENCODE(OP_DeleteCharge)");

		ENCODE_FORWARD(OP_MoveItem);
	}

	ENCODE(OP_DeleteItem)
	{
		ENCODE_LENGTH_EXACT(DeleteItem_Struct);
		SETUP_DIRECT_ENCODE(DeleteItem_Struct, structs::DeleteItem_Struct);

		eq->from_slot = ServerToLaurionSlot(emu->from_slot);
		eq->to_slot = ServerToLaurionSlot(emu->to_slot);
		OUT(number_in_stack);

		FINISH_ENCODE();
	}

	ENCODE(OP_DeleteSpawn)
	{
		ENCODE_LENGTH_EXACT(DeleteSpawn_Struct);
		SETUP_DIRECT_ENCODE(DeleteSpawn_Struct, structs::DeleteSpawn_Struct);

		OUT(spawn_id);
		eq->unknown04 = 1;	// Observed

		FINISH_ENCODE();
	}

	ENCODE(OP_DisciplineUpdate)
	{
		ENCODE_LENGTH_EXACT(Disciplines_Struct);
		SETUP_DIRECT_ENCODE(Disciplines_Struct, structs::Disciplines_Struct);

		memcpy(&eq->values, &emu->values, sizeof(Disciplines_Struct));

		FINISH_ENCODE();
	}

	ENCODE(OP_ExpansionInfo)
	{
		ENCODE_LENGTH_EXACT(ExpansionInfo_Struct);
		SETUP_DIRECT_ENCODE(ExpansionInfo_Struct, structs::ExpansionInfo_Struct);

		// Zero out the unknown padding
		memset(eq->Unknown000, 0, sizeof(eq->Unknown000));
		
		OUT(Expansions);

		Log(Logs::General, Logs::Error, "[EXPANSION_ENCODE] Sending expansion packet:");
		Log(Logs::General, Logs::Error, "[EXPANSION_ENCODE]   Expansions value: %u (0x%X)", eq->Expansions, eq->Expansions);
		Log(Logs::General, Logs::Error, "[EXPANSION_ENCODE]   Packet size: %d bytes", __packet->size);
		Log(Logs::General, Logs::Error, "[EXPANSION_ENCODE]   Expected size: %d bytes", sizeof(structs::ExpansionInfo_Struct));

		FINISH_ENCODE();
	}

	ENCODE(OP_ExpUpdate)
	{
		SETUP_DIRECT_ENCODE(ExpUpdate_Struct, structs::ExpUpdate_Struct);

		//later we should change the underlying server to use this more accurate value
		//and encode the 330 in the other patches
		eq->exp = emu->exp * 100000 / 330;

		FINISH_ENCODE();
	}

	ENCODE(OP_FeatureUnlock)
	{
		SETUP_VAR_ENCODE(FeatureUnlock_Struct);
		
		// OP_FeatureUnlock structure:
		// 1 byte header + 4 bytes count + (count * 8 bytes per entry)
		// Each entry: feature_id(4) + feature_count(4)
		
		// Calculate output size: 1 (header) + 4 (count) + (count * 8)
		uint32_t out_size = 1 + 4 + (emu->count * 8);
		__packet->size = out_size;
		__packet->pBuffer = new unsigned char[out_size];
		memset(__packet->pBuffer, 0, out_size);
		
		// Write header byte
		__packet->WriteUInt8(emu->header);
		
		// Write count
		__packet->WriteUInt32(emu->count);
		
		// Write each feature entry
		FeatureUnlock_Entry* entries = (FeatureUnlock_Entry*)((unsigned char*)emu + sizeof(FeatureUnlock_Struct));
		for (uint32_t i = 0; i < emu->count; i++) {
			__packet->WriteUInt32(entries[i].feature_id);
			__packet->WriteUInt32(entries[i].feature_count);
		}

		// [DH_LOG_CLEANUP_2] [FEATURE_UNLOCK_ENCODE] count + output HEX disabled.
		// Log(Logs::General, Logs::Error, "[FEATURE_UNLOCK_ENCODE] Encoded %u feature(s), packet size: %u bytes",
		// 	emu->count, __packet->size);
		// std::ostringstream hex;
		// hex << std::hex << std::setfill('0');
		// for (uint32_t i = 0; i < __packet->size; i++) {
		// 	if (i > 0 && i % 4 == 0) hex << " ";
		// 	hex << std::setw(2) << (int)__packet->pBuffer[i];
		// }
		// Log(Logs::General, Logs::Error, "[FEATURE_UNLOCK_ENCODE] Output packet HEX: %s", hex.str().c_str());

		FINISH_ENCODE();
	}

	ENCODE(OP_FormattedMessage)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;

		FormattedMessage_Struct* emu = (FormattedMessage_Struct*)in->pBuffer;

		char* old_message_ptr = (char*)in->pBuffer;
		old_message_ptr += sizeof(FormattedMessage_Struct);

		std::string old_message_array[9];

		for (int i = 0; i < 9; ++i) {
			if (*old_message_ptr == 0) { break; }
			old_message_array[i] = old_message_ptr;
			old_message_ptr += old_message_array[i].length() + 1;
		}

		SerializeBuffer buffer;
		buffer.WriteUInt32(emu->unknown0);
		buffer.WriteUInt8(0); // Observed
		buffer.WriteUInt32(emu->string_id);
		buffer.WriteUInt32(emu->type);

		for (int i = 0; i < 9; ++i) {
			std::string new_message;
			ServerToLaurionConvertLinks(new_message, old_message_array[i]);
			buffer.WriteLengthString(new_message);
		}

		auto outapp = new EQApplicationPacket(OP_FormattedMessage, buffer.size());
		outapp->WriteData(buffer.buffer(), buffer.size());
		dest->FastQueuePacket(&outapp, ack_req);

		delete in;
	}

	ENCODE(OP_GMTraining) {
		ENCODE_LENGTH_EXACT(GMTrainee_Struct);
		SETUP_DIRECT_ENCODE(GMTrainee_Struct, structs::GMTrainee_Struct);

		OUT(npcid);
		OUT(playerid);

		for (int i = 0; i < 100; ++i) {
			OUT(skills[i]);
		}

		eq->unknown408[0] = 1;
		eq->unknown408[1] = 0xC9;
		eq->unknown408[2] = 0xC9;
		eq->unknown408[3] = 0xC9;
		eq->unknown408[4] = 0xC9;
		eq->unknown408[5] = 0xC9;
		eq->unknown408[6] = 0xC9;
		eq->unknown408[7] = 0xC9;
		eq->unknown408[8] = 0xC9;
		eq->unknown408[9] = 0xC9;
		eq->unknown408[10] = 0xC9;
		eq->unknown408[11] = 0xC9;
		eq->unknown408[12] = 0xC9;
		eq->unknown408[13] = 0xC9;
		eq->unknown408[14] = 0xC9;
		eq->unknown408[15] = 0xC9;
		eq->unknown408[16] = 0xC9;
		eq->unknown408[17] = 0xC9;
		eq->unknown408[18] = 0xC9;
		eq->unknown408[19] = 0xC9;
		eq->unknown408[20] = 0xC9;
		eq->unknown408[21] = 0xC9;
		eq->unknown408[22] = 0xC9;
		eq->unknown408[23] = 0xC9;
		eq->unknown408[24] = 0xC9;
		eq->unknown408[25] = 0xC9;
		eq->unknown408[26] = 0xC9;
		eq->unknown408[27] = 0xC9;
		eq->unknown408[28] = 0xC9;
		eq->unknown408[29] = 0xC9;
		eq->unknown408[30] = 0xC9;
		eq->unknown408[31] = 0xC9;
		eq->unknown408[32] = 0xC9;
		eq->unknown408[33] = 0xCA; //the client far as I can tell doesn't read past the 1 byte + 32 0xc9s, but still setting it to what we saw
		eq->unknown408[34] = 0x8C;
		eq->unknown408[35] = 0xEC;

		FINISH_ENCODE();
	}

	ENCODE(OP_GMTrainSkillConfirm)
	{
		ENCODE_LENGTH_EXACT(GMTrainSkillConfirm_Struct);
		SETUP_DIRECT_ENCODE(GMTrainSkillConfirm_Struct, structs::GMTrainSkillConfirm_Struct);

		OUT(SkillID);
		OUT(Cost);
		OUT(NewSkill);
		OUT_str(TrainerName);

		FINISH_ENCODE();
	}

	ENCODE(OP_GroundSpawn)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;
		Object_Struct* emu = (Object_Struct*)in->pBuffer;

		SerializeBuffer buffer;
		buffer.WriteUInt32(emu->drop_id);
		buffer.WriteString(emu->object_name);
		buffer.WriteUInt16(emu->zone_id);
		buffer.WriteUInt16(emu->zone_instance);
		buffer.WriteUInt32(emu->drop_id); //this is some other sub but it's okay to duplicate
		buffer.WriteUInt32(0); //expires
		buffer.WriteFloat(emu->heading);
		buffer.WriteFloat(emu->tilt_x);
		buffer.WriteFloat(emu->tilt_y);
		buffer.WriteFloat(emu->size != 0 && (float)emu->size < 5000.f ? (float)((float)emu->size / 100.0f) : 1.f); //size, with weird peq hack
		buffer.WriteFloat(emu->y);
		buffer.WriteFloat(emu->x);
		buffer.WriteFloat(emu->z);
		buffer.WriteFloat(emu->object_type); //weight

		auto outapp = new EQApplicationPacket(OP_GroundSpawn, buffer.size());
		outapp->WriteData(buffer.buffer(), buffer.size());
		dest->FastQueuePacket(&outapp, ack_req);

		delete in;
	}

	ENCODE(OP_HPUpdate)
	{
		SETUP_DIRECT_ENCODE(SpawnHPUpdate_Struct, structs::SpawnHPUpdate_Struct);

		OUT(spawn_id);
		OUT(cur_hp);
		OUT(max_hp);

		FINISH_ENCODE();
	}

	ENCODE(OP_Illusion)
	{
		ENCODE_LENGTH_EXACT(Illusion_Struct);
		SETUP_DIRECT_ENCODE(Illusion_Struct, structs::Illusion_Struct);

		OUT(spawnid);
		OUT_str(charname);
		OUT(race);
		eq->unknown006[0] = 0;
		eq->unknown006[1] = 0;
		OUT(gender);
		OUT(texture);
		OUT(helmtexture);
		OUT(face);
		OUT(hairstyle);
		OUT(haircolor);
		OUT(beard);
		OUT(beardcolor);
		OUT(size);
		OUT(drakkin_heritage);
		OUT(drakkin_tattoo);
		OUT(drakkin_details);

		FINISH_ENCODE();
	}

	ENCODE(OP_ItemPacket)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;
		uchar* __emu_buffer = in->pBuffer;

		LogEncode(in, "OP_ItemPacket");

		ItemPacket_Struct* old_item_pkt = (ItemPacket_Struct*)__emu_buffer;

		auto type = ServerToLaurionItemPacketType(old_item_pkt->PacketType);
		if (type == item::ItemPacketType::ItemPacketInvalid) {
			delete in;
			return;
		}

		// [DH_ENCODE_CHAIN] OP_ItemPacket Laurion encode path:
		//   - Maps emu ItemPacket_Struct (after 4-byte PacketType) via InternalSerializedItem_Struct → inst + slot_id.
		//   - Writes client packet type prefix: buffer.WriteInt32((int32_t)type) then SerializeItem(..., old_item_pkt->PacketType).
		//   - ItemPacketDragonHoard (single): case falls through to default — same SerializeItem call; DH wire body is built inside
		//     SerializeItem when slot_id is 5000..5199 (is_dragon_hoard): 16-byte GUID + sub_14029dc10-aligned header + SerializeItemDefinitionLaurionBlob + post-blob tail.
		//   - ItemPacketDragonHoardBatch: memcpy passthrough of payload after first 4 bytes — no SerializeItem / no blob builder here.
		//   - SerializeItemDefinitionLaurionBlob is only invoked from SerializeItem's is_dragon_hoard branch (not directly from this ENCODE).
		//   - SerializeOneDragonHoardItem() also calls SerializeItem(..., ItemPacketDragonHoard) for batch assembly elsewhere.
		switch (type)
		{
		case item::ItemPacketType::ItemPacketParcel: {
			ParcelMessaging_Struct       pms{};
			EQ::Util::MemoryStreamReader ss(reinterpret_cast<char*>(in->pBuffer), in->size);
			cereal::BinaryInputArchive   ar(ss);
			ar(pms);

			uint32 player_name_length = pms.player_name.length();
			uint32 note_length = pms.note.length();

			auto* int_struct = (EQ::InternalSerializedItem_Struct*)pms.serialized_item.data();

			SerializeBuffer buffer;
			buffer.WriteInt32((int32_t)type);
			SerializeItem(buffer, (const EQ::ItemInstance*)int_struct->inst, int_struct->slot_id, 0, old_item_pkt->PacketType);

			buffer.WriteUInt32(pms.sent_time);
			buffer.WriteLengthString(pms.player_name);
			buffer.WriteLengthString(pms.note);

			auto outapp = new EQApplicationPacket(OP_ItemPacket, buffer.size());
			outapp->WriteData(buffer.buffer(), buffer.size());
			dest->FastQueuePacket(&outapp, ack_req);
			break;
		}
		case item::ItemPacketType::ItemPacketDragonHoard: {
			// Batch packet: buffer is [PacketType 4][count 4][item1 258][item2 258]...; output payload only (count + N×258), no type prefix.
			if (old_item_pkt->PacketType == ItemPacketType::ItemPacketDragonHoardBatch && in->size > 4) {
				size_t payload_len = in->size - 4;
				auto outapp = new EQApplicationPacket(OP_ItemPacket, payload_len);
				memcpy(outapp->pBuffer, __emu_buffer + 4, payload_len);
				dest->FastQueuePacket(&outapp, ack_req);
				break;
			}
			// fallthrough: single-item DH
		}
		default: {
			EQ::InternalSerializedItem_Struct* int_struct = (EQ::InternalSerializedItem_Struct*)(&__emu_buffer[4]);
			SerializeBuffer buffer;
			buffer.WriteInt32((int32_t)type);
			SerializeItem(buffer, (const EQ::ItemInstance*)int_struct->inst, int_struct->slot_id, 0, old_item_pkt->PacketType);

			// [DH_LOG_CLEANUP] [WIRE_ITEM_RAW] per-packet DH wire hex disabled.
			// [WIRE_ITEM_RAW] exact wire bytes (type prefix + payload) that the client receives — all DH packets logged, first 320 bytes
			// if (type == item::ItemPacketType::ItemPacketDragonHoard) {
			// 	size_t wire_len = buffer.size();
			// 	size_t dump_len = std::min(wire_len, size_t(320));
			// 	const unsigned char* wire_buf = reinterpret_cast<const unsigned char*>(buffer.buffer());
			// 	std::ostringstream wire_hex;
			// 	wire_hex << std::hex << std::setfill('0');
			// 	for (size_t i = 0; i < dump_len; i++) {
			// 		if (i > 0 && i % 16 == 0) wire_hex << "\n";
			// 		if (i % 16 == 0) wire_hex << std::setw(4) << i << ": ";
			// 		wire_hex << std::setw(2) << (int)wire_buf[i] << " ";
			// 	}
			// 	uint32_t type_prefix = 0;
			// 	if (wire_len >= 4)
			// 		memcpy(&type_prefix, wire_buf, 4);
			// 	LogInfo("[WIRE_ITEM_RAW] type_prefix=0x{:08X} packet_size={} first {} bytes (hex):\n{}", type_prefix, wire_len, dump_len, wire_hex.str());
			// }

			auto outapp = new EQApplicationPacket(OP_ItemPacket, buffer.size());
			outapp->WriteData(buffer.buffer(), buffer.size());
			dest->FastQueuePacket(&outapp, ack_req);
		}
		}

		delete in;
	}

	ENCODE(OP_LogServer) {
		SETUP_VAR_ENCODE(LogServer_Struct);
		ALLOC_LEN_ENCODE(1840);

		//pvp
		if (emu->enable_pvp) {
			*(char*)&__packet->pBuffer[0x04] = 1;
		}

		if (emu->enable_FV) {
			//FV sets these both to 1
			//one appears to enable the no drop flag the other just marks the server as special?
			*(char*)&__packet->pBuffer[0x08] = 1;
			*(char*)&__packet->pBuffer[0x0a] = 1;
		}

		//This has something to do with heirloom and prestige items but im not sure what it does
		//Seems to sit at 0
		*(char*)&__packet->pBuffer[0x71d] = 0;

		//not sure what this does, something to do with server select
		*(char*)&__packet->pBuffer[0x09] = 0;

		//this appears to have some effect on the tradeskill system; disabling made by tags perhaps?
		*(char*)&__packet->pBuffer[0x0b] = 0;

		//not sure, setting it to the value ive seen
		*(char*)&__packet->pBuffer[0x0c] = 1;

		//Something to do with languages
		*(char*)&__packet->pBuffer[0x0d] = 1;

		//These seem to affect if server has betabuff enabled
		*(char*)&__packet->pBuffer[0x5c0] = 0;
		*(char*)&__packet->pBuffer[0x5c1] = 0;
		//This is set on test so it's probably indicating this is a test server
		*(char*)&__packet->pBuffer[0x5c2] = 0;

		//not sure, but it's grouped with the beta and test stuff
		*(char*)&__packet->pBuffer[0x5c3] = 0;

		//world short name
		strncpy((char*)&__packet->pBuffer[0x15], emu->worldshortname, 32);

		//not sure, affects some player calculation but didn't care to look more
		*(char*)&__packet->pBuffer[0x5c2] = 0;

		//Looks right
		if (emu->enablemail) {
			*(char*)&__packet->pBuffer[0x5b5] = 1;
		}

		//Looks right
		if (emu->enablevoicemacros) {
			*(char*)&__packet->pBuffer[0x5b4] = 1;
		}

		//Not sure, sending what we've seen
		*(char*)&__packet->pBuffer[0x5b6] = 0;

		//Not sure sending what we've seen
		*(char*)&__packet->pBuffer[0x5b8] = 1;

		//Not sure sending what we've seen
		*(int32_t*)&__packet->pBuffer[0x5fc] = -1;

		//Test sets this to 1, everyone else seems to set it to 0
		*(int32_t*)&__packet->pBuffer[0x600] = 0;

		//Disassembly puts it next to code dealing with commands, ive not seen anyone send anything but 0
		*(char*)&__packet->pBuffer[0x705] = 0;

		//Something about item restrictions, seems to always be set to 1
		*(char*)&__packet->pBuffer[0x710] = 1;

		//This and 0x724 are often multiplied together in guild favor calcs, live and test send 1.0f
		*(float*)&__packet->pBuffer[0x720] = 1.0f;
		*(float*)&__packet->pBuffer[0x724] = 1.0f;

		//This and 0x72c are often multiplied together in non-guild favor calcs, live and test send 1.0f
		*(float*)&__packet->pBuffer[0x728] = 1.0f;
		*(float*)&__packet->pBuffer[0x72c] = 1.0f;

		FINISH_ENCODE();
	}

	ENCODE(OP_ManaChange) {
		ENCODE_LENGTH_EXACT(ManaChange_Struct);
		SETUP_DIRECT_ENCODE(ManaChange_Struct, structs::ManaChange_Struct);

		OUT(new_mana);
		OUT(stamina);
		OUT(spell_id);
		OUT(keepcasting);
		OUT(slot);

		FINISH_ENCODE();
	}

	ENCODE(OP_MobHealth) {
		ENCODE_LENGTH_EXACT(SpawnHPUpdate_Struct2);
		SETUP_DIRECT_ENCODE(SpawnHPUpdate_Struct2, structs::MobHealth_Struct);

		OUT(spawn_id);
		OUT(hp);

		FINISH_ENCODE();
	}

	ENCODE(OP_MoneyOnCorpse)
	{
		ENCODE_LENGTH_EXACT(moneyOnCorpseStruct);
		SETUP_DIRECT_ENCODE(moneyOnCorpseStruct, structs::moneyOnCorpseStruct);

		eq->type = emu->response;
		OUT(platinum);
		OUT(gold);
		OUT(silver);
		OUT(copper);
		eq->flags = 0;

		FINISH_ENCODE();
	}

	ENCODE(OP_MoveItem)
	{
		ENCODE_LENGTH_EXACT(MoveItem_Struct);
		SETUP_DIRECT_ENCODE(MoveItem_Struct, structs::MoveItem_Struct);

		Log(Logs::Detail, Logs::Netcode, "Laurion::ENCODE(OP_MoveItem)");

		eq->from_slot = ServerToLaurionSlot(emu->from_slot);
		eq->to_slot = ServerToLaurionSlot(emu->to_slot);
		OUT(number_in_stack);

		FINISH_ENCODE();
	}

	ENCODE(OP_NewSpawn) { ENCODE_FORWARD(OP_ZoneSpawns); }

	ENCODE(OP_NewZone) {
		EQApplicationPacket* in = *p;
		*p = nullptr;

		LogEncode(in, "OP_NewZone");

		unsigned char* __emu_buffer = in->pBuffer;
		NewZone_Struct* emu = (NewZone_Struct*)__emu_buffer;

		SerializeBuffer buffer;
		/*
		char Shortname[];
		char Longname[];
		char WeatherType[];
		char WeatherTypeOverride[];
		char SkyType[];
		char SkyTypeOverride[];
		*/
		buffer.WriteString(emu->zone_short_name);
		buffer.WriteString(emu->zone_long_name);
		buffer.WriteString("");
		buffer.WriteString("");
		buffer.WriteString("");
		buffer.WriteString("");

		/*
		s32 ZoneType;
		s16 ZoneId;
		s16 ZoneInstance;
		float ZoneExpModifier;
		s32 GroupLvlExpRelated;
		s32 FilterID;
		s32 Unknown1;
		*/
		buffer.WriteInt32(emu->ztype);
		buffer.WriteInt16(emu->zone_id);
		buffer.WriteInt16(emu->zone_instance);
		buffer.WriteFloat(emu->zone_exp_multiplier);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);

		//float FogDensity;
		buffer.WriteFloat(emu->fog_density);

		//WeatherState state[4];
		for (int i = 0; i < 4; ++i) {
			/*
			float FogStart;
			float FogEnd;
			u8 FogRed;
			u8 FogGreen;
			u8 FogBlue;
			u8 RainChance;
			u8 RainDuration;
			u8 SnowChance;
			u8 SnowDuration;
			*/

			buffer.WriteFloat(emu->fog_minclip[i]);
			buffer.WriteFloat(emu->fog_maxclip[i]);
			buffer.WriteUInt8(emu->fog_red[i]);
			buffer.WriteUInt8(emu->fog_green[i]);
			buffer.WriteUInt8(emu->fog_blue[i]);
			buffer.WriteUInt8(emu->rain_chance[i]);
			buffer.WriteUInt8(emu->rain_duration[i]);
			buffer.WriteUInt8(emu->snow_chance[i]);
			buffer.WriteUInt8(emu->snow_duration[i]);

		}

		/*
		u8 PrecipitationType;
		float BloomIntensity;
		float ZoneGravity;
		s32 LavaDamage;
		s32 MinLavaDamage;
		*/
		buffer.WriteUInt8(emu->sky);
		buffer.WriteFloat(1.0f);
		buffer.WriteFloat(emu->gravity);
		buffer.WriteInt32(emu->lava_damage);
		buffer.WriteInt32(emu->min_lava_damage);

		/*
		s32 TimeStringID;
		s32 Unknown3;
		s32 SkyLock;
		s32 SkyLockOverride;
		*/
		buffer.WriteInt32(0);
		buffer.WriteInt32(1);
		buffer.WriteInt32(0);
		buffer.WriteInt32(-1);

		/*
		float SafeY;
		float SafeX;
		float SafeZ;
		float SafeHeading;
		float Ceiling;
		float Floor;
		*/

		buffer.WriteFloat(emu->safe_y);
		buffer.WriteFloat(emu->safe_x);
		buffer.WriteFloat(emu->safe_z);
		buffer.WriteFloat(emu->safe_heading);
		buffer.WriteFloat(emu->max_z);
		buffer.WriteFloat(emu->underworld);

		/*
		float MinClip;
		float MaxClip;
		s32 FallThroughWorldTeleportID;
		*/
		buffer.WriteFloat(emu->minclip);
		buffer.WriteFloat(emu->maxclip);
		buffer.WriteInt32(emu->underworld_teleport_index);

		/*
		s32 Unknown4;
		s32 ScriptIDHour;
		s32 ScriptIDMinute;
		s32 ScriptIDTick;
		s32 ScriptIDOnPlayerDeath;
		s32 ScriptIDOnNPCDeath;
		s32 ScriptIDPlayerEnteringZone;
		s32 ScriptIDOnZonePop;
		s32 ScriptIDNPCLoot;
		s32 Unknown4b;
		s32 ScriptIDOnFishing;
		s32 ScriptIDOnForage;
		s32 Unknown4c;
		*/
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);

		/*
		s32 NPCAgroMaxDist;
		*/
		buffer.WriteInt32(600);

		/*
		s32 ForageLow;
		s32 ForageMedium;
		s32 ForageHigh;
		s32 ForageSpecial;
		s32 FishingLow;
		s32 FishingMedium;
		s32 FishingHigh;
		s32 FishingRelated;
		*/
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);
		buffer.WriteInt32(-1);

		/*
		s32 CanPlaceCampsite;
		s32 CanPlaceGuildBanner;
		s32 Unknown4d;
		*/

		buffer.WriteInt32(2);
		buffer.WriteInt32(2);
		buffer.WriteInt32(0);

		/*
		s32 FastRegenHP;
		s32 FastRegenMana;
		s32 FastRegenEndurance;
		*/
		buffer.WriteInt32(emu->fast_regen_hp);
		buffer.WriteInt32(emu->fast_regen_mana);
		buffer.WriteInt32(emu->fast_regen_endurance);

		/*
		u8 NewEngineZone;
		u8 SkyEnabled;
		u8 FogOnOff;
		u8 ClimateType;
		u8 bNoPlayerLight;
		*/
		buffer.WriteUInt8(0); //not sure what happens if we set this incorrectly but we probably need to add this to the zone database
		buffer.WriteUInt8(1);
		buffer.WriteUInt8(1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);

		/*
		u8 bUnknown5;
		u8 bNoAttack;
		u8 bAllowPVP;
		u8 bNoEncumber;
		u8 Unknown6;
		u8 Unknown7;
		*/
		buffer.WriteUInt8(1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);

		/*
		u8 bNoLevitate;
		u8 bNoBuffExpiration;
		u8 bDisallowManaStone;
		u8 bNoBind;
		u8 bNoCallOfTheHero;
		u8 bUnknown8;
		u8 bNoFear;
		u8 bUnknown9;
		*/

		buffer.WriteUInt8(0);
		buffer.WriteUInt8(emu->suspend_buffs);
		buffer.WriteUInt8(1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);

		auto outapp = new EQApplicationPacket(OP_NewZone, buffer.size());
		outapp->WriteData(buffer.buffer(), buffer.size());
		dest->FastQueuePacket(&outapp, ack_req);

		delete in;
	}

	ENCODE(OP_OnLevelMessage)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;
		OnLevelMessage_Struct* emu = (OnLevelMessage_Struct*)in->pBuffer;
		SerializeBuffer buffer;

		buffer.WriteLengthString(emu->Title);
		buffer.WriteLengthString(emu->Text);
		buffer.WriteLengthString(emu->ButtonName0);
		buffer.WriteLengthString(emu->ButtonName1);
		buffer.WriteUInt8(emu->Buttons);
		buffer.WriteUInt8(emu->SoundControls);
		buffer.WriteUInt32(emu->Duration);
		buffer.WriteUInt32(emu->PopupID);
		buffer.WriteUInt32(emu->NegativeID);
		buffer.WriteUInt32(0); //seen -1 & 0
		buffer.WriteUInt32(0); //seen 0

		auto outapp = new EQApplicationPacket(OP_OnLevelMessage, buffer.size());
		outapp->WriteData(buffer.buffer(), buffer.size());
		dest->FastQueuePacket(&outapp, ack_req);

		delete in;
	}

	ENCODE(OP_PlayerProfile) {
		EQApplicationPacket* in = *p;
		*p = nullptr;

		LogEncode(in, "OP_PlayerProfile");

		unsigned char* __emu_buffer = in->pBuffer;
		PlayerProfile_Struct* emu = (PlayerProfile_Struct*)__emu_buffer;

		SerializeBuffer out;

		/*
		u32 crc;
		u32 length;
		*/
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		//PcProfile begin
		/*
		u32 profile_type;
		u32 profile_id;
		u32 shroud_template_id;
		*/
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		/*
		u8 gender;
		u32 race;
		u32 class;
		u8 level;
		u8 level1;
		*/
		out.WriteUInt8(emu->gender);
		out.WriteUInt32(emu->race);
		out.WriteUInt32(emu->class_);
		out.WriteUInt8(emu->level);
		out.WriteUInt8(emu->level);

		//u32 bind_count;
		out.WriteUInt32(5);

		for (int r = 0; r < 5; r++)
		{
			/*
			u32 zoneid;
			float x;
			float y;
			float z;
			float heading;
			*/
			out.WriteUInt32(emu->binds[r].zone_id);
			out.WriteFloat(emu->binds[r].x);
			out.WriteFloat(emu->binds[r].y);
			out.WriteFloat(emu->binds[r].z);
			out.WriteFloat(emu->binds[r].heading);
		}

		/*
		u32 deity;
		u32 intoxication;
		*/
		out.WriteUInt32(emu->deity);
		out.WriteUInt32(emu->intoxication);

		//u32 property_count;
		out.WriteUInt32(10); // properties count

		//u32 properties[property_count];
		for (int i = 0; i < 10; i++) {
			out.WriteUInt32(0);
		}

		//u32 armor_prop_count;
		out.WriteUInt32(22); //armor count
		for (int i = 0; i < 22; ++i) {
			/*
			s32 type;
			s32 variation;
			s32 material;
			s32 newArmorId;
			s32 newArmorType;
			*/
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
		}

		//u32 base_armor_prop_count;
		out.WriteUInt32(9); //base armor count
		for (int i = 0; i < 9; ++i) {
			/*
			s32 type;
			s32 variation;
			s32 material;
			s32 newArmorId;
			s32 newArmorType;
			*/
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
		}

		//u32 body_tint_count;
		out.WriteUInt32(9); //body_tint_count
		//u32 body_tints[body_tint_count];
		for (int i = 0; i < 9; ++i) {
			out.WriteUInt32(0);
		}

		//u32 equip_tint_count;
		out.WriteUInt32(9); //equip_tint_count
		//u32 equip_tints[equip_tint_count];
		for (int i = 0; i < 9; ++i) {
			out.WriteUInt32(0);
		}

		/*
		u8 hair_color;
		u8 facial_hair_color;
		u32 npc_tint_index;
		u8 eye_color1;
		u8 eye_color2;
		u8 hair_style;
		u8 facial_hair;
		u8 face;
		u8 old_face;
		u32 heritage;
		u32 tattoo;
		u32 details;
		*/
		out.WriteUInt8(emu->haircolor);
		out.WriteUInt8(emu->beardcolor);
		out.WriteUInt32(0); //npc tint index
		out.WriteUInt8(emu->eyecolor1);
		out.WriteUInt8(emu->eyecolor2);
		out.WriteUInt8(emu->hairstyle);
		out.WriteUInt8(emu->beard);
		out.WriteUInt8(emu->face);
		out.WriteUInt8(0); //old face
		out.WriteUInt32(emu->drakkin_heritage);
		out.WriteUInt32(emu->drakkin_tattoo);
		out.WriteUInt32(emu->drakkin_details);

		/*
		u8 texture_type;
		u8 material;
		u8 variation;
		*/
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);

		/*
		float height;
		float width;
		float length;
		float view_height;
		*/
		out.WriteFloat(5.0f);
		out.WriteFloat(3.0f);
		out.WriteFloat(2.5f);
		out.WriteFloat(5.5f);

		/*
		u32 primary;
		u32 secondary;
		*/
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		/*
		u32 practices;
		u32 base_mana;
		u32 base_hp;
		u32 base_str;
		u32 base_sta;
		u32 base_cha;
		u32 base_dex;
		u32 base_int;
		u32 base_agi;
		u32 base_wis;
		u32 base_heroic_str;
		u32 base_heroic_sta;
		u32 base_heroic_cha;
		u32 base_heroic_dex;
		u32 base_heroic_int;
		u32 base_heroic_agi;
		u32 base_heroic_wis;
		*/
		out.WriteUInt32(emu->points);
		out.WriteUInt32(emu->mana);
		out.WriteUInt32(emu->cur_hp);
		out.WriteUInt32(emu->STR);
		out.WriteUInt32(emu->STA);
		out.WriteUInt32(emu->CHA);
		out.WriteUInt32(emu->DEX);
		out.WriteUInt32(emu->INT);
		out.WriteUInt32(emu->AGI);
		out.WriteUInt32(emu->WIS);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		//u32 aa_count;
		out.WriteUInt32(300);
		for (int i = 0; i < 240; ++i) {
			/*
			s32 index;
			s32 points_spent;
			s32 charges_spent;
			u8 unknown1;
			*/
			out.WriteUInt32(emu->aa_array[i].AA);
			out.WriteUInt32(emu->aa_array[i].value);
			out.WriteUInt32(emu->aa_array[i].charges);
			out.WriteUInt8(0);
		}

		for (int i = 0; i < 60; ++i) {
			/*
			s32 index;
			s32 points_spent;
			s32 charges_spent;
			u8 unknown1; //not sure about this one
			*/
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt8(0);
		}

		/*u32 skill_count;*/
		out.WriteUInt32(100);
		//s32 skills[skill_count];
		for (int i = 0; i < 100; ++i) {
			out.WriteUInt32(emu->skills[i]);
		}

		//u32 innate_skill_count;
		out.WriteUInt32(25);
		//s32 innate_skills[innate_skill_count];
		for (int i = 0; i < 25; ++i) {
			out.WriteUInt32(emu->InnateSkills[i]);
		}

		/*
		u32 combat_ability_count;
		*/
		out.WriteUInt32(300);
		//s32 combat_abilities[combat_ability_count];
		for (int i = 0; i < 100; ++i) {
			out.WriteUInt32(emu->disciplines.values[i]);
		}

		for (int i = 0; i < 200; ++i) {
			out.WriteUInt32(0);
		}

		//u32 combat_ability_timer_count;
		out.WriteUInt32(25);
		//s32 combat_ability_timers[combat_ability_timer_count];
		for (int i = 0; i < 20; ++i) {
			out.WriteUInt32(emu->disciplines.values[i]);
		}

		for (int i = 0; i < 5; ++i) {
			out.WriteUInt32(0);
		}

		//u32 unk_ability_count;
		out.WriteUInt32(0);

		//u32 linked_spell_timer_count;
		out.WriteUInt32(25);
		//s32 linked_spell_timers[linked_spell_timer_count];
		for (int i = 0; i < 25; ++i) {
			out.WriteUInt32(0);
		}

		//u32 item_recast_timer_count;
		out.WriteUInt32(100);
		//s32 item_recast_timers[item_recast_timer_count];
		for (int i = 0; i < 100; ++i) {
			out.WriteUInt32(0);
		}

		//u32 spell_book_slot_count;
		out.WriteUInt32(1120);

		//s32 spell_book_slots[spell_book_slot_count];
		for (int i = 0; i < 720; ++i) {
			out.WriteUInt32(emu->spell_book[i]);
		}

		for (int i = 0; i < 400; ++i) {
			out.WriteUInt32(0xFFFFFFFF);
		}

		//u32 spell_gem_count;
		out.WriteUInt32(18);
		//s32 spell_gems[spell_gem_count];
		for (int i = 0; i < 12; ++i) {
			out.WriteUInt32(emu->mem_spells[i]);
		}

		for (int i = 0; i < 6; ++i) {
			out.WriteUInt32(0xFFFFFFFF);
		}

		/*
		u32 spell_recast_timer_count;
		*/
		out.WriteUInt32(15);

		//s32 spell_recast_timers[spell_recast_timer_count];
		for (int i = 0; i < 12; ++i) {
			out.WriteUInt32(emu->spellSlotRefresh[i]);
		}

		for (int i = 0; i < 3; ++i) {
			out.WriteUInt32(0);
		}

		//u8 max_allowed_spell_slots;
		out.WriteUInt8(0);

		//u32 buff_count;
		out.WriteUInt32(42);

		//PackedEQAffect buffs[buff_count];
		//todo: fix
		for (int i = 0; i < 42; ++i) {
			/*
			struct EQAffect
			{
				float modifier;
				EqGuid caster;
				u32 duration;
				u32 max_duration;
				u8 level;
				s32 spell_id;
				s32 hitcount;
				u32 flags;
				u32 viral_timer;
				u8 type;
				SlotData slots[6];
			};

			*/
			out.WriteFloat(1.0f);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt8(0);
			out.WriteUInt32(0xFFFFFFFF);
			out.WriteUInt32(0);
			out.WriteUInt8(0);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			//SlotData slots[6];
			for (int j = 0; j < 6; ++j) {
				/*
				s32 slot_id;
				s64 value;
				*/
				out.WriteInt32(-1);
				out.WriteUInt64(0);
			}
		}

		//Coin coin;
		/*
		u32 platinum;
		u32 gold;
		u32 silver;
		u32 copper;
		*/
		out.WriteUInt32(emu->platinum);
		out.WriteUInt32(emu->gold);
		out.WriteUInt32(emu->silver);
		out.WriteUInt32(emu->copper);

		//Coin cursor_coin;
		/*
		u32 platinum;
		u32 gold;
		u32 silver;
		u32 copper;
		*/
		out.WriteUInt32(emu->platinum_cursor);
		out.WriteUInt32(emu->gold_cursor);
		out.WriteUInt32(emu->silver_cursor);
		out.WriteUInt32(emu->copper_cursor);

		/*
		u32 disc_timer;
		u32 mend_timer;
		u32 forage_timer;
		u32 thirst;
		u32 hunger;
		*/

		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(emu->thirst_level);
		out.WriteUInt32(emu->hunger_level);

		//u32 aa_spent;
		out.WriteUInt32(emu->aapoints_spent);

		//u32 aa_window_count;
		out.WriteUInt32(6);
		//u32 aa_window_stats[aa_window_count];
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		//u32 aa_points_unspent;
		out.WriteUInt32(emu->aapoints);

		/*
		u8 sneak;
		u8 hide;
		*/
		out.WriteUInt8(0);
		out.WriteUInt8(0);

		//u32 bandolier_count;
		out.WriteUInt32(20);

		//BandolierSet bandolier_sets[bandolier_count];
		for (int i = 0; i < 20; ++i) {
			//char name[];
			out.WriteString(emu->bandoliers[i].Name);

			//BandolierItemInfo items[4];
			for (int j = 0; j < 4; ++j) {
				//char name[];
				out.WriteString(emu->bandoliers[i].Items[j].Name);
				//s32 item_id;
				out.WriteUInt32(emu->bandoliers[i].Items[j].ID);
				//s32 icon;
				out.WriteUInt32(emu->bandoliers[i].Items[j].Icon);
			}
		}

		//u32 invslot_bitmask;
		out.WriteUInt32(0xFFFFFFFF);

		/*
		u32 basedata_hp;
		u32 basedata_mana;
		u32 basedata_endur;
		u32 basedata_mr;
		u32 basedata_fr;
		u32 basedata_cr;
		u32 basedata_pr;
		u32 basedata_dr;
		u32 basedata_corrupt;
		u32 basedata_phr;
		*/

		out.WriteUInt32(5);
		out.WriteUInt32(5);
		out.WriteUInt32(5);
		out.WriteUInt32(25);
		out.WriteUInt32(25);
		out.WriteUInt32(25);
		out.WriteUInt32(15);
		out.WriteUInt32(15);
		out.WriteUInt32(15);
		out.WriteUInt32(15);

		/*
		float basedata_walkspeed;
		float basedata_runspeed;
		*/

		out.WriteFloat(0.46f);
		out.WriteFloat(0.7f);

		/*
		u32 basedata_hpregen;
		u32 basedata_manaregen;
		u32 basedata_mountmanaregen;
		u32 basedata_endurregen;
		u32 basedata_ac;
		u32 basedata_atk;
		u32 basedata_dmg;
		u32 basedata_delay;
		u32 endurance;
		u32 heroic_type;
		*/

		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(emu->endurance);
		out.WriteUInt32(0);

		//ItemIndex keyring_item_index[5];
		for (int i = 0; i < 5; ++i) {
			/*
			s16 slot1;
			s16 slot2;
			s16 slot3;
			*/

			out.WriteInt16(-1);
			out.WriteInt16(-1);
			out.WriteInt16(-1);
		}

		/*
		u64 exp;
		u32 aa_exp;
		*/
		out.WriteUInt64(emu->exp);
		out.WriteUInt32(emu->expAA);

		//PcClient begin
		/*
		u32 character_id;
		u16 world_id;
		u16 reserved;
		*/
		out.WriteUInt32(emu->char_id);
		out.WriteUInt16(RuleI(World, Id));
		out.WriteUInt16(0);

		/*u32 name_length;*/
		/*char name[name_length];*/
		out.WriteLengthString(64, emu->name);

		//u32 last_name_length;
		//char last_name[last_name_length];
		out.WriteLengthString(32, emu->last_name);

		/*
		u32 creation_time;
		u32 account_creation_time;
		u32 last_played_time;
		u32 played_minutes;
		u32 entitled_days;
		u32 expansion_flags;
		*/

		out.WriteUInt32(emu->birthday);
		out.WriteUInt32(emu->birthday);
		out.WriteUInt32(emu->lastlogin);
		out.WriteUInt32(5000);
		out.WriteUInt32(6000);
		out.WriteUInt32(0x3FFFFFFF);

		//u32 language_count;
		out.WriteUInt32(32);
		for (int i = 0; i < 28; i++)
		{
			//u8 languages[language_count];
			out.WriteUInt8(emu->languages[i]);
		}

		for (int i = 0; i < 4; i++)
		{
			out.WriteUInt8(0);
		}

		/*
		u32 current_zone;
		float current_x;
		float current_y;
		float current_z;
		float current_heading;
		*/
		out.WriteUInt16(emu->zone_id);
		out.WriteUInt16(emu->zoneInstance);
		out.WriteFloat(emu->x);
		out.WriteFloat(emu->y);
		out.WriteFloat(emu->z);
		out.WriteFloat(emu->heading);

		/*
		u8 animation;
		u8 pvp;
		u8 anon;
		u8 gm;
		*/
		out.WriteUInt8(100);
		out.WriteUInt8(emu->pvp);
		out.WriteUInt8(emu->anon);
		out.WriteUInt8(emu->gm);

		/*
		u64 guild_id;
		u8 guild_show_sprite;
		u8 status;
		*/

		out.WriteInt32(emu->guild_id);
		out.WriteUInt32(0);
		out.WriteUInt8(0);
		out.WriteUInt8(5);

		//Coin coin;
		out.WriteUInt32(emu->platinum);
		out.WriteUInt32(emu->gold);
		out.WriteUInt32(emu->silver);
		out.WriteUInt32(emu->copper);

		//Coin bank;
		out.WriteUInt32(emu->platinum_bank);
		out.WriteUInt32(emu->gold_bank);
		out.WriteUInt32(emu->silver_bank);
		out.WriteUInt32(emu->copper_bank);

		//u32 bank_shared_plat;
		out.WriteUInt32(emu->platinum_shared);

		//u32 claim_count;
		out.WriteUInt32(0);
		//Claim claims[claim_count];

		//Tribute tribute;
		/*
		u32 BenefitTimer;
		s32 unknown1;
		s32 current_favor;
		s32 unknown2;
		s32 all_time_favor;
		s32 unknown3; //some of these are probably the bools on the pcclient;
		u16 unknown4;
		*/
		out.WriteUInt32(600000);
		out.WriteInt32(-1);
		out.WriteUInt32(emu->tribute_points);
		out.WriteUInt32(0);
		out.WriteUInt32(emu->career_tribute_points);
		out.WriteUInt32(0);
		out.WriteUInt16(0);

		//u32 tribute_benefit_count
		out.WriteUInt32(5);

		//TributeBenefit tribute_benefits[tribute_benefit_count];
		for (int i = 0; i < 5; ++i) {
			/*
			s32 benefit_id;
			s32 benefit_tier;
			*/

			out.WriteUInt32(emu->tributes[i].tribute);
			out.WriteUInt32(emu->tributes[i].tier);
		}

		//u32 trophy_tribute_benefit_count;
		out.WriteUInt32(10);

		//TributeBenefit trophy_tribute_benefit[trophy_tribute_benefit_count];
		for (int i = 0; i < 10; ++i) {
			/*
			s32 benefit_id;
			s32 benefit_tier;
			*/

			out.WriteUInt32(0xFFFFFFFF);
			out.WriteUInt32(0);
		}
		const uint8_t task_data[137] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00,
		};

		//u8 tasks[137]; //on live and on xac's capture from 12/28/23 these both are the same size;
		for (int i = 0; i < 137; i++)
		{
			out.WriteUInt8(task_data[i]);
		}

		/*
		u32 good_points_available;
		u32 good_points_earned;
		u32 bad_points_available;
		u32 bad_points_earned;
		*/

		out.WriteUInt32(emu->currentRadCrystals);
		out.WriteUInt32(emu->careerRadCrystals);
		out.WriteUInt32(emu->currentEbonCrystals);
		out.WriteUInt32(emu->careerEbonCrystals);

		/*
		u32 momentum_balance;
		u32 loyalty_reward_balance;
		u32 parcel_status;
		*/

		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		/*
		u32 vehicle_name_length;
		char vehicle_name[vehicle_name_length];
		*/

		out.WriteUInt32(64);
		for (int i = 0; i < 64; ++i) {
			out.WriteUInt8(0);
		}

		/*
		u8 super_pkill;
		u8 unclone;
		u8 dead;
		*/

		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);

		/*
		u32 ld_timer;
		u32 spell_interrupt_count;
		u8 autosplit;
		u8 tells_off;
		*/

		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt8(emu->autosplit);
		out.WriteUInt8(0);

		/*
		u8 gm_invis;
		u32 kill_me;
		u8 cheater_ld_flag;
		u8 norent;
		u8 corpse;
		u8 client_gm_flag_set;
		u32 mentor_pct;
		*/

		out.WriteUInt8(0);
		out.WriteUInt32(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt32(0);

		//RaidData raid;
		/*
		u32 main_assist1;
		u32 main_assist2;
		u32 main_assist3;
		char main_assist_name1[];
		char main_assist_name2[];
		char main_assist_name3[];
		u32 main_marker1;
		u32 main_marker2;
		u32 main_marker3;
		u32 master_looter;
		*/

		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt8(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		//u32 unique_player_id;
		out.WriteUInt32(emu->char_id);

		//LdonData ldon_data;
		/*
		u32 count;
		u32 ldon_categories[count];
		u32 ldon_points_available;
		*/

		out.WriteUInt32(6);
		out.WriteUInt32(0);
		out.WriteUInt32(emu->ldon_points_guk);
		out.WriteUInt32(emu->ldon_points_mir);
		out.WriteUInt32(emu->ldon_points_mmc);
		out.WriteUInt32(emu->ldon_points_ruj);
		out.WriteUInt32(emu->ldon_points_tak);
		out.WriteUInt32(emu->ldon_points_available);

		//u32 air_supply;
		out.WriteUInt32(emu->air_remaining);

		//PvPData pvp_data;
		/*
		u32 kills;
		u32 deaths;
		u32 current_points;
		u32 career_points;
		u32 best_kill_streak;
		u32 worst_death_streak;
		u32 current_kill_streak;
		*/

		out.WriteUInt32(emu->PVPKills);
		out.WriteUInt32(emu->PVPDeaths);
		out.WriteUInt32(emu->PVPCurrentPoints);
		out.WriteUInt32(emu->PVPCareerPoints);
		out.WriteUInt32(emu->PVPBestKillStreak);
		out.WriteUInt32(emu->PVPWorstDeathStreak);
		out.WriteUInt32(emu->PVPCurrentKillStreak);

		//PvPKill last_kill;
		/*
		char name[];
		u32 level;
		u32 unknown1; //not sure
		u32 unknown2; //not sure
		u32 race;
		u32 class;
		u32 zone;
		u32 time;
		u32 points;
		*/
		out.WriteString(emu->PVPLastKill.Name);
		out.WriteUInt32(emu->PVPLastKill.Level);
		out.WriteUInt32(0);
		out.WriteUInt32(0);
		out.WriteUInt32(emu->PVPLastKill.Race);
		out.WriteUInt32(emu->PVPLastKill.Class);
		out.WriteUInt32(emu->PVPLastKill.Zone);
		out.WriteUInt32(emu->PVPLastKill.Time);
		out.WriteUInt32(emu->PVPLastKill.Points);

		//PvPDeath last_death;
		/*
		char name[];
		u32 level;
		u32 race;
		u32 class;
		u32 zone;
		u32 time;
		u32 points;
		*/

		out.WriteString(emu->PVPLastDeath.Name);
		out.WriteUInt32(emu->PVPLastDeath.Level);
		out.WriteUInt32(emu->PVPLastDeath.Race);
		out.WriteUInt32(emu->PVPLastDeath.Class);
		out.WriteUInt32(emu->PVPLastDeath.Zone);
		out.WriteUInt32(emu->PVPLastDeath.Time);
		out.WriteUInt32(emu->PVPLastDeath.Points);

		/*
		u32 kills_in_past_24_hours;
		*/

		out.WriteUInt32(emu->PVPNumberOfKillsInLast24Hours);

		//u32 kill_list_count;
		out.WriteUInt32(50);

		//PvPKill kill_list[kill_list_count];
		for (int i = 0; i < 50; ++i) {
			/*
			char name[];
			u32 level;
			u32 unknown1; //not sure
			u32 unknown2; //not sure
			u32 race;
			u32 class;
			u32 zone;
			u32 time;
			u32 points;
			*/
			out.WriteString(emu->PVPRecentKills[i].Name);
			out.WriteUInt32(emu->PVPRecentKills[i].Level);
			out.WriteUInt32(0);
			out.WriteUInt32(0);
			out.WriteUInt32(emu->PVPRecentKills[i].Race);
			out.WriteUInt32(emu->PVPRecentKills[i].Class);
			out.WriteUInt32(emu->PVPRecentKills[i].Zone);
			out.WriteUInt32(emu->PVPRecentKills[i].Time);
			out.WriteUInt32(emu->PVPRecentKills[i].Points);
		}

		/*
		u32 pvp_infamy_level;
		u32 pvp_vitality;
		*/
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		/*
		u32 cursor_krono;
		u32 krono;
		*/
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		/*
		u8 autoconsent_group;
		u8 autoconsent_raid;
		u8 autoconsent_guild;
		u8 autoconsent_fellowship;
		*/

		out.WriteUInt8(emu->groupAutoconsent);
		out.WriteUInt8(emu->raidAutoconsent);
		out.WriteUInt8(emu->guildAutoconsent);
		out.WriteUInt8(1);

		/*
		u8 private_for_eq_players;
		u32 main_level;
		u8 show_helm;
		u32 downtime;
		*/
		out.WriteUInt8(1);
		out.WriteUInt32(emu->level);
		out.WriteUInt8(emu->showhelm);
		out.WriteUInt32(emu->RestTimer);

		//AltCurrency alt_currency;
		/*
		u32 alt_currency_str_length;
		u32 unknown1;
		char alt_currency_string[alt_currency_str_length];
		*/
		out.WriteUInt32(1);
		out.WriteUInt32(0);
		out.WriteUInt8(0x31);

		//u32 completed_event_subcomponent_count;
		out.WriteUInt32(0);
		//AchivementSubComponentData completed_event_subcomponents[completed_event_subcomponent_count];

		//u32 inprogress_event_subcomponent_count;
		out.WriteUInt32(0);
		//AchivementSubComponentData inprogress_event_subcomponents[inprogress_event_subcomponent_count];

		/*
		u64 merc_aa_exp;
		u32 merc_aa_points;
		u32 merc_aa_spent;
		*/
		out.WriteUInt64(0);
		out.WriteUInt32(0);
		out.WriteUInt32(0);

		//u32 starting_city_zone_id;
		//we don't actually support this yet
		out.WriteUInt32(394);

		/*
		u8 use_advanced_looting;
		u8 is_master_loot_candidate;
		*/

		out.WriteUInt8(1);
		out.WriteUInt8(1);

		//alchemy_bonus_list_count
		out.WriteUInt32(0);
		//AlchemyBonusSkillData alchemy_bonus_list[alchemy_bonus_list_count];

		//u32 persona_count;
		out.WriteUInt32(0);
		//PersonaEquipmentSet persona_equipment_set[persona_count];

		//u8 term;
		out.WriteUInt8(0);

		auto outapp = new EQApplicationPacket(OP_PlayerProfile, out.length());
		outapp->WriteData(out.buffer(), out.length());
		outapp->SetWritePosition(4);
		outapp->WriteUInt32(outapp->size - 9);
		CRC32::SetEQChecksum(outapp->pBuffer, outapp->size - 1, 8);
		dest->FastQueuePacket(&outapp, ack_req);
		delete in;
	}

	ENCODE(OP_RecipeAutoCombine)
	{
		ENCODE_LENGTH_EXACT(RecipeAutoCombine_Struct);
		SETUP_DIRECT_ENCODE(RecipeAutoCombine_Struct, structs::RecipeAutoCombine_Struct);

		OUT(object_type);
		OUT(some_id);
		eq->container_slot = ServerToLaurionSlot(emu->unknown1);
		structs::InventorySlot_Struct LaurionSlot;
		LaurionSlot.Type = 8;	// Observed
		LaurionSlot.Padding1 = 0;
		LaurionSlot.Slot = 0xffff;
		LaurionSlot.SubIndex = 0xffff;
		LaurionSlot.AugIndex = 0xffff;
		LaurionSlot.Padding2 = 0;
		eq->unknown_slot = LaurionSlot;
		OUT(recipe_id);
		OUT(reply_code);

		FINISH_ENCODE();
	}

	ENCODE(OP_RemoveBlockedBuffs) { ENCODE_FORWARD(OP_BlockedBuffs); }

	ENCODE(OP_RespondAA)
	{
		SETUP_DIRECT_ENCODE(AATable_Struct, structs::AATable_Struct);

		eq->aa_spent = emu->aa_spent;
		// These fields may need to be correctly populated at some point
		eq->aapoints_assigned[0] = emu->aa_spent;
		eq->aapoints_assigned[1] = 0;
		eq->aapoints_assigned[2] = 0;
		eq->aapoints_assigned[3] = 0;
		eq->aapoints_assigned[4] = 0;
		eq->aapoints_assigned[5] = 0;

		for (uint32 i = 0; i < MAX_PP_AA_ARRAY; ++i)
		{
			eq->aa_list[i].AA = emu->aa_list[i].AA;
			eq->aa_list[i].value = emu->aa_list[i].value;
			eq->aa_list[i].charges = emu->aa_list[i].charges;
			eq->aa_list[i].bUnknown0x0c = false;
		}

		FINISH_ENCODE();
	}

	ENCODE(OP_RequestClientZoneChange)
	{
		ENCODE_LENGTH_EXACT(RequestClientZoneChange_Struct);
		SETUP_DIRECT_ENCODE(RequestClientZoneChange_Struct, structs::RequestClientZoneChange_Struct);

		OUT(zone_id);
		OUT(instance_id);
		OUT(y);
		OUT(x);
		OUT(z);
		OUT(heading);
		eq->type = 0x0b;
		eq->unknown004 = 0xffffffff;
		eq->unknown172 = 0x0168b500;

		FINISH_ENCODE();
	}

	ENCODE(OP_SendAATable)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;
		LogEncode(in, "OP_SendAATable");

		AARankInfo_Struct* emu = (AARankInfo_Struct*)in->pBuffer;

		std::vector<int32> skill;
		std::vector<int32> points;
		in->SetReadPosition(sizeof(AARankInfo_Struct) + emu->total_effects * sizeof(AARankEffect_Struct));
		for (auto i = 0; i < emu->total_prereqs; ++i) {
			skill.push_back(in->ReadUInt32());
			points.push_back(in->ReadUInt32());
		}

		SerializeBuffer buffer;

		/*
		s32 AbilityId;
		u8 ShowInAbilityWindow;
		s32 ShortName;
		s32 ShortName2;
		s32 Name;
		s32 Desc;
		*/

		buffer.WriteUInt32(emu->id);
		buffer.WriteUInt8(1);
		buffer.WriteInt32(emu->upper_hotkey_sid);
		buffer.WriteInt32(emu->lower_hotkey_sid);
		buffer.WriteInt32(emu->title_sid);
		buffer.WriteInt32(emu->desc_sid);

		/*
		s32 MinLevel;
		s32 Cost;
		s32 GroupID;
		s32 CurrentRank;
		*/
		buffer.WriteInt32(emu->level_req);
		buffer.WriteInt32(emu->cost);
		buffer.WriteUInt32(emu->seq);
		buffer.WriteUInt32(emu->current_level);

		/*
		u32 PrereqSkillCount;
		s32 PrereqSkills[PrereqSkillCount];
		u32 PrereqLevelCount;
		s32 PrereqLevels[PrereqLevelCount];
		*/

		if (emu->total_prereqs) {
			buffer.WriteUInt32(emu->total_prereqs);
			for (auto& e : skill)
				buffer.WriteInt32(e);
			buffer.WriteUInt32(emu->total_prereqs);
			for (auto& e : points)
				buffer.WriteInt32(e);
		}
		else {
			buffer.WriteUInt32(1);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(1);
			buffer.WriteUInt32(0);
		}

		/*
		u32 Type;
		s32 SpellId;
		*/
		buffer.WriteInt32(emu->type);
		buffer.WriteInt32(emu->spell);

		/*
		u32 TimerIdCount;
		s32 TimerIds[TimerIdCount];
		s32 ReuseTimer;
		u32 Classes;
		*/
		buffer.WriteInt32(1);
		buffer.WriteInt32(emu->spell_type);
		buffer.WriteInt32(emu->spell_refresh);
		buffer.WriteInt32(emu->classes);

		/*
		s32 MaxRank;
		s32 PrevAbilityId;
		s32 NextAbilityId;
		s32 TotalPoints;
		*/

		buffer.WriteInt32(emu->max_level);
		buffer.WriteInt32(emu->prev_id);
		buffer.WriteInt32(emu->next_id);
		buffer.WriteInt32(emu->total_cost);

		/*
		u8 bRefund;
		s32 QuestOnly;
		u8 bIgnoreDeLevel;
		*/
		buffer.WriteUInt8(0);
		buffer.WriteUInt32(emu->grant_only);
		buffer.WriteUInt8(0);

		/*
		s32 Charges;
		s32 Expansion;
		s32 Category;
		*/
		buffer.WriteUInt32(emu->charges);
		buffer.WriteInt32(emu->expansion);
		buffer.WriteInt32(emu->category);

		/*
		u8 bShroud;
		u8 bBetaOnly;
		u8 bResetOnDeath;
		u8 AutoGrant;
		*/
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);

		/*
		s32 AutoGrantExpansion;
		s32 Unknown098;
		u8 Unknown09C;
		*/

		buffer.WriteInt32(emu->expansion);
		buffer.WriteInt32(0);
		buffer.WriteUInt8(0);

		//u32 TotalEffects;
		buffer.WriteUInt32(emu->total_effects);
		in->SetReadPosition(sizeof(AARankInfo_Struct));
		for (auto i = 0; i < emu->total_effects; ++i) {
			auto skill_id = in->ReadUInt32();
			auto base1 = in->ReadUInt32();
			auto base2 = in->ReadUInt32();
			auto slot = in->ReadUInt32();

			/*
			u32 effect_id;
			s64 base1;
			s64 base2;
			u32 slot;
			*/

			buffer.WriteUInt32(skill_id);
			buffer.WriteInt64(base1);
			buffer.WriteInt64(base2);
			buffer.WriteUInt32(slot);
		}

		auto outapp = new EQApplicationPacket(OP_SendAATable, buffer.size());
		outapp->WriteData(buffer.buffer(), buffer.size());
		dest->FastQueuePacket(&outapp, ack_req);

		delete in;
	}

	ENCODE(OP_SendCharInfo) {
		ENCODE_LENGTH_ATLEAST(CharacterSelect_Struct);
		SETUP_VAR_ENCODE(CharacterSelect_Struct);

		// Zero-character count shunt
		if (emu->CharCount == 0) {
			ALLOC_VAR_ENCODE(structs::CharacterSelect_Struct, sizeof(structs::CharacterSelect_Struct));
			eq->CharCount = emu->CharCount;

			FINISH_ENCODE();
			return;
		}

		unsigned char* emu_ptr = __emu_buffer;
		emu_ptr += sizeof(CharacterSelect_Struct);
		CharacterSelectEntry_Struct* emu_cse = (CharacterSelectEntry_Struct*)nullptr;

		size_t names_length = 0;
		size_t character_count = 0;
		for (; character_count < emu->CharCount; ++character_count) {
			emu_cse = (CharacterSelectEntry_Struct*)emu_ptr;
			names_length += strlen(emu_cse->Name);
			emu_ptr += sizeof(CharacterSelectEntry_Struct);
		}

		size_t total_length = sizeof(structs::CharacterSelect_Struct)
			+ character_count * sizeof(structs::CharacterSelectEntry_Struct)
			+ names_length;

		ALLOC_VAR_ENCODE(structs::CharacterSelect_Struct, total_length);
		structs::CharacterSelectEntry_Struct* eq_cse = (structs::CharacterSelectEntry_Struct*)nullptr;

		eq->CharCount = character_count;

		emu_ptr = __emu_buffer;
		emu_ptr += sizeof(CharacterSelect_Struct);

		unsigned char* eq_ptr = __packet->pBuffer;
		eq_ptr += sizeof(structs::CharacterSelect_Struct);

		for (int counter = 0; counter < character_count; ++counter) {
			emu_cse = (CharacterSelectEntry_Struct*)emu_ptr;
			eq_cse = (structs::CharacterSelectEntry_Struct*)eq_ptr; // base address

			strcpy(eq_cse->Name, emu_cse->Name);
			eq_ptr += strlen(emu_cse->Name);

			eq_cse = (structs::CharacterSelectEntry_Struct*)eq_ptr;
			eq_cse->Name[0] = '\0';

			eq_cse->Class = emu_cse->Class;
			eq_cse->Race = emu_cse->Race;
			eq_cse->Level = emu_cse->Level;
			eq_cse->ShroudClass = emu_cse->ShroudClass;
			eq_cse->ShroudRace = emu_cse->ShroudRace;
			eq_cse->Zone = emu_cse->Zone;
			eq_cse->Instance = emu_cse->Instance;
			eq_cse->Gender = emu_cse->Gender;
			eq_cse->Face = emu_cse->Face;

			for (int equip_index = 0; equip_index < EQ::textures::materialCount; equip_index++) {
				eq_cse->Equip[equip_index].Material = emu_cse->Equip[equip_index].Material;
				eq_cse->Equip[equip_index].Unknown1 = emu_cse->Equip[equip_index].Unknown1;
				eq_cse->Equip[equip_index].EliteMaterial = emu_cse->Equip[equip_index].EliteModel;
				eq_cse->Equip[equip_index].HeroForgeModel = emu_cse->Equip[equip_index].HerosForgeModel;
				eq_cse->Equip[equip_index].Material2 = emu_cse->Equip[equip_index].Unknown2;
				eq_cse->Equip[equip_index].Color = emu_cse->Equip[equip_index].Color;
			}

			eq_cse->Unknown1 = 255;
			eq_cse->Unknown2 = 0;
			eq_cse->DrakkinTattoo = emu_cse->DrakkinTattoo;
			eq_cse->DrakkinDetails = emu_cse->DrakkinDetails;
			eq_cse->Deity = emu_cse->Deity;
			eq_cse->PrimaryIDFile = emu_cse->PrimaryIDFile;
			eq_cse->SecondaryIDFile = emu_cse->SecondaryIDFile;
			eq_cse->HairColor = emu_cse->HairColor;
			eq_cse->BeardColor = emu_cse->BeardColor;
			eq_cse->EyeColor1 = emu_cse->EyeColor1;
			eq_cse->EyeColor2 = emu_cse->EyeColor2;
			eq_cse->HairStyle = emu_cse->HairStyle;
			eq_cse->Beard = emu_cse->Beard;
			eq_cse->GoHome = emu_cse->GoHome;
			eq_cse->Tutorial = emu_cse->Tutorial;
			eq_cse->DrakkinHeritage = emu_cse->DrakkinHeritage;
			eq_cse->Enabled = emu_cse->Enabled;
			eq_cse->LastLogin = emu_cse->LastLogin;
			eq_cse->Unknown3 = 0;
			eq_cse->Unknown4 = 0;
			eq_cse->Unknown5 = 0;
			eq_cse->Unknown6 = 0;
			eq_cse->Unknown7 = 0;
			eq_cse->CharacterId = 0;
			eq_cse->Unknown8 = 1;

			emu_ptr += sizeof(CharacterSelectEntry_Struct);
			eq_ptr += sizeof(structs::CharacterSelectEntry_Struct);
		}

		DumpPacket(__packet);

		FINISH_ENCODE();
	}

	ENCODE(OP_SendMaxCharacters) {
		ENCODE_LENGTH_EXACT(MaxCharacters_Struct);
		SETUP_DIRECT_ENCODE(MaxCharacters_Struct, structs::MaxCharacters_Struct);

		//OUT(max_chars);
		eq->max_chars = 8; //needs to be fixed
		eq->marketplace_chars = 0;
		eq->unknown008 = -1;
		eq->unknown00c = 196608;
		eq->unknown010 = 0;
		eq->unknown014 = 0;
		eq->unknown018 = 0;
		eq->unknown01c = 0;
		eq->unknown020 = -1;
		eq->unknown024 = 0;
		eq->unknown028 = 0;
		eq->unknown02c = 0;
		eq->unknown030 = 0;
		eq->unknown034 = 0;

		FINISH_ENCODE();
	}

	ENCODE(OP_SendMembership) {
		// Use ATLEAST instead of EXACT because server struct (152 bytes) != client struct (145 bytes packed)
		// Server: uint32 membership + exit_url_length = 152 bytes
		// Client: uint8 membership, no exit_url_length = 145 bytes
		ENCODE_LENGTH_ATLEAST(Membership_Struct);
		SETUP_DIRECT_ENCODE(Membership_Struct, structs::Membership_Struct);

		eq->membership = emu->membership;
		eq->races = emu->races;
		eq->classes = emu->classes;
		eq->entrysize = 33;

		// [DH_LOG_CLEANUP_2] [MEMBERSHIP_ENCODE_VALUES] disabled.
		// Debug: Log the values being encoded
		// LogInfo("[MEMBERSHIP_ENCODE_VALUES] membership={}, races={:#x}, classes={:#x}, entrysize={}",
		// 	eq->membership, eq->races, eq->classes, eq->entrysize);

		eq->entries[0] = -1;  // Max AA Restriction
		eq->entries[1] = -1;  // Max Level Restriction
		eq->entries[2] = -1;  // Max Char Slots per Account (not used by client?)
		eq->entries[3] = -1;  // SpellTier
		eq->entries[4] = -1;  // Main Inventory Size
		eq->entries[5] = -1;  // Max Platinum per level
		eq->entries[6] = 1;   // Send Mail
		eq->entries[7] = 1;   // Use Parcels
		eq->entries[8] = 1;   // Loyalty
		eq->entries[9] = -1;  // Merc Tiers
		eq->entries[10] = 1;  // Housing
		eq->entries[11] = -1; // Shared Bank Slots
		eq->entries[12] = -1; // Max Journal Quests
		eq->entries[13] = 1;  // CreateGuild
		eq->entries[14] = 1;  // Bazaar
		eq->entries[15] = 1;  // Barter
		eq->entries[16] = 1;  // Chat
		eq->entries[17] = 1;  // Petition
		eq->entries[18] = 1;  // Advertising
		eq->entries[19] = -1; // UseItem
		eq->entries[20] = -1; // StartingCity
		eq->entries[21] = 1;  // Ornament
		eq->entries[22] = 0;  // HeroicCharacter
		eq->entries[23] = 0;  // AutoGrantAA
		eq->entries[24] = 0;  // MountKeyRingSlots
		eq->entries[25] = 0;  // IllusionKeyRingSlots
		eq->entries[26] = 0;  // FamiliarKeyRingSlots
		eq->entries[27] = 0;  // FamiliarAutoLeave
		eq->entries[28] = 500;  // HeroForgeKeyRingSlots - ENABLED
		eq->entries[29] = 200;  // DragonHoardSlots - ENABLED!
		eq->entries[30] = 0;  // TeleportKeyRingSlots
		eq->entries[31] = 500;  // PersonalDepotSlots (TradeskillDepot) - ENABLED!
		eq->entries[32] = 0;

		// [DH_LOG_CLEANUP_3] Debug logging disabled (do not delete).
		// LogInfo("[MEMBERSHIP_ENCODE] entrysize=[{}], entries[29]=[{}] (DragonHoard), entries[31]=[{}] (Depot), packet_size=[{}] bytes, expected_size=[{}] bytes",
		//     eq->entrysize, eq->entries[29], eq->entries[31], __packet->size, sizeof(structs::Membership_Struct));

		FINISH_ENCODE();
	}

	ENCODE(OP_SendMembershipDetails) {
		ENCODE_LENGTH_EXACT(Membership_Details_Struct);
		SETUP_DIRECT_ENCODE(Membership_Details_Struct, structs::Membership_Details_Struct);

		int32 settings[96][3] = {
			{ 0, 0, 500 }, { 1, 0, 500 }, { 0, 1, -1 }, { 1, 1, -1 },
			{ 0, 2, 2 }, { 2, 0, -1 }, { 3, 0, -1 }, { 1, 2, 4 },
			{ 0, 3, 1 }, { 2, 1, -1 }, { 3, 1, -1 }, { 1, 3, 1 },
			{ 0, 4, -1 }, { 2, 2, -1 }, { 3, 2, -1 }, { 1, 4, -1 },
			{ 0, 5, -1 }, { 2, 3, -1 }, { 3, 3, -1 }, { 1, 5, -1 },
			{ 0, 6, 0 }, { 2, 4, -1 }, { 3, 4, -1 }, { 1, 6, 0 },
			{ 0, 7, 1 }, { 2, 5, -1 }, { 3, 5, -1 }, { 1, 7, 1 },
			{ 0, 8, 1 }, { 2, 6, 1 }, { 3, 6, 1 }, { 1, 8, 1 },
			{ 0, 9, 5 }, { 2, 7, 1 }, { 3, 7, 1 }, { 1, 9, 5 },
			{ 0, 10, 0 }, { 2, 8, 1 }, { 3, 8, 1 }, { 0, 11, -1 },
			{ 1, 10, 1 }, { 2, 9, -1 }, { 3, 9, -1 }, { 0, 12, -1 },
			{ 1, 11, -1 }, { 2, 10, 1 }, { 3, 10, 1 }, { 0, 13, 0 },
			{ 1, 12, -1 }, { 2, 11, -1 }, { 3, 11, -1 }, { 0, 14, 0 },
			{ 1, 13, 1 }, { 2, 12, -1 }, { 3, 12, -1 }, { 0, 15, 0 },
			{ 1, 14, 0 }, { 2, 13, 1 }, { 3, 13, 1 }, { 0, 16, 0 },
			{ 1, 15, 0 }, { 2, 14, 1 }, { 3, 14, 1 }, { 0, 17, 0 },
			{ 1, 16, 1 }, { 2, 15, 1 }, { 3, 15, 1 }, { 0, 18, 0 },
			{ 1, 17, 0 }, { 2, 16, 1 }, { 3, 16, 1 }, { 0, 19, 0 },
			{ 1, 18, 0 }, { 2, 17, 1 }, { 3, 17, 1 }, { 0, 20, 0 },
			{ 1, 19, 0 }, { 2, 18, 1 }, { 3, 18, 1 }, { 0, 21, 0 },
			{ 1, 20, 0 }, { 2, 19, -1 }, { 3, 19, -1 }, { 0, 22, 0 },
			{ 1, 21, 0 }, { 2, 20, -1 }, { 3, 20, -1 }, { 2, 21, 0 },
			{ 0, 23, 0 }, { 1, 22, 0 }, { 3, 21, 0 }, { 2, 22, 0 },
			{ 1, 23, 0 }, { 3, 22, 0 }, { 2, 23, 0 }, { 3, 23, 0 }
		};

		uint32 races[17][2] = {
			{ 1, 131071 },
			{ 333, 131071 },
			{ 90287, 131071 },
			{ 90289, 16 },
			{ 90290, 32 },
			{ 90291, 64 },
			{ 90292, 128 },
			{ 90293, 256 },
			{ 90294, 512 },
			{ 90295, 1024 },
			{ 90296, 2048 },
			{ 90297, 8192 },
			{ 90298, 16384 },
			{ 90299, 32768 },
			{ 90300, 65536 },
			{ 2012271, 131071 },
			{ 2012277, 131071 }
		};

		uint32 classes[17][2] = {
			{ 1, 131071 },
			{ 333, 131071 },
			{ 90287, 131071 },
			{ 90301, 8 },
			{ 90302, 16 },
			{ 90303, 32 },
			{ 90304, 64 },
			{ 90305, 128 },
			{ 90306, 256 },
			{ 90307, 1024 },
			{ 90308, 2048 },
			{ 90309, 8192 },
			{ 90310, 16384 },
			{ 90311, 32768 },
			{ 90312, 65536 },
			{ 2012271, 131071 },
			{ 2012277, 131071 }
		};

		eq->membership_setting_count = 96;

		for (int i = 0; i < 96; ++i) {
			eq->settings[i].setting_index = (int8)settings[i][0];
			eq->settings[i].setting_id = settings[i][1];
			eq->settings[i].setting_value = settings[i][2];
		}

		eq->class_entry_count = 17;
		for (int i = 0; i < 17; ++i) {
			eq->membership_classes[i].purchase_id = classes[i][0];
			eq->membership_classes[i].bitwise_entry = classes[i][1];
		}

		eq->race_entry_count = 17;
		for (int i = 0; i < 17; ++i) {
			eq->membership_races[i].purchase_id = races[i][0];
			eq->membership_races[i].bitwise_entry = races[i][1];
		}

		eq->exit_url_length = 0;

		// [DH_LOG_CLEANUP_2] [MEMBERSHIP_DETAILS_FULL] verbose dump disabled.
		// COMPREHENSIVE LOGGING: Dump all settings that might contain slot counts
		// LogInfo("[MEMBERSHIP_DETAILS_FULL] Packet size=[{}] bytes", __packet->size);
		// LogInfo("[MEMBERSHIP_DETAILS_FULL] membership_setting_count=[{}]", eq->membership_setting_count);
		// LogInfo("[MEMBERSHIP_DETAILS_FULL] Settings array (looking for slot counts):");
		// for (int i = 0; i < 96; ++i) {
		// 	int32 val = eq->settings[i].setting_value;
		// 	if (val >= 100 && val <= 10000) {
		// 		LogInfo("[MEMBERSHIP_DETAILS_FULL]   settings[{}]: index={}, id={}, VALUE={} ← POTENTIAL SLOT COUNT!",
		// 		    i, (int)eq->settings[i].setting_index, eq->settings[i].setting_id, val);
		// 	} else if (val != -1 && val != 0 && val != 1) {
		// 		LogInfo("[MEMBERSHIP_DETAILS_FULL]   settings[{}]: index={}, id={}, value={}",
		// 		    i, (int)eq->settings[i].setting_index, eq->settings[i].setting_id, val);
		// 	}
		// }
		// LogInfo("[MEMBERSHIP_DETAILS_FULL] Byte layout:");
		// LogInfo("[MEMBERSHIP_DETAILS_FULL]   offset 0x00: membership_setting_count = {}", eq->membership_setting_count);
		// LogInfo("[MEMBERSHIP_DETAILS_FULL]   offset 0x04: settings array start");
		// LogInfo("[MEMBERSHIP_DETAILS_FULL]   offset calculated: class_entry_count at ~0x{:03X} = {}",
		//     4 + (96 * 12), eq->class_entry_count);
		// LogInfo("[MEMBERSHIP_DETAILS_FULL]   offset calculated: race_entry_count at ~0x{:03X} = {}",
		//     4 + (96 * 12) + 4 + (17 * 8), eq->race_entry_count);

		FINISH_ENCODE();
	}

	ENCODE(OP_SendZonepoints)
	{
		SETUP_VAR_ENCODE(ZonePoints);
		ALLOC_VAR_ENCODE(structs::ZonePoints, sizeof(structs::ZonePoints) + sizeof(structs::ZonePoint_Entry) * (emu->count + 1));

		eq->count = emu->count;
		for (uint32 i = 0; i < emu->count; ++i)
		{
			eq->zpe[i].iterator = emu->zpe[i].iterator;
			eq->zpe[i].x = emu->zpe[i].x;
			eq->zpe[i].y = emu->zpe[i].y;
			eq->zpe[i].z = emu->zpe[i].z;
			eq->zpe[i].heading = emu->zpe[i].heading;
			eq->zpe[i].zoneid = emu->zpe[i].zoneid;
			eq->zpe[i].zoneinstance = emu->zpe[i].zoneinstance;
		}

		FINISH_ENCODE();
	}

	ENCODE(OP_ShopPlayerBuy)
	{
		ENCODE_LENGTH_EXACT(Merchant_Sell_Struct);
		SETUP_DIRECT_ENCODE(Merchant_Sell_Struct, structs::Merchant_Sell_Response_Struct);

		OUT(npcid);
		OUT(playerid);
		OUT(itemslot);
		OUT(quantity);
		OUT(price);

		FINISH_ENCODE();
	}

	ENCODE(OP_ShopPlayerSell)
	{
		ENCODE_LENGTH_EXACT(Merchant_Purchase_Struct);
		SETUP_DIRECT_ENCODE(Merchant_Purchase_Struct, structs::Merchant_Purchase_Response_Struct);

		OUT(npcid);
		eq->inventory_slot = ServerToLaurionTypelessSlot(emu->itemslot, EQ::invtype::typePossessions);
		OUT(quantity);
		OUT(price);

		FINISH_ENCODE();
	}

	ENCODE(OP_ShopRequest)
	{
		ENCODE_LENGTH_EXACT(MerchantClick_Struct);
		SETUP_DIRECT_ENCODE(MerchantClick_Struct, structs::MerchantClickResponse_Struct);

		if (emu->command == 0) {
			OUT(player_id);
			eq->npc_id = 0;
		}
		else {
			OUT(npc_id);
			OUT(player_id);
			OUT(rate);
			OUT(tab_display);
			eq->unknown028 = 256;
		}

		FINISH_ENCODE();
	}

	ENCODE(OP_SkillUpdate)
	{
		ENCODE_LENGTH_EXACT(SkillUpdate_Struct);
		SETUP_DIRECT_ENCODE(SkillUpdate_Struct, structs::SkillUpdate_Struct);

		OUT(skillId);
		OUT(value);
		eq->active = 1;

		FINISH_ENCODE();
	}

	ENCODE(OP_SpecialMesg)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;

		SerializeBuffer buf(in->size);
		buf.WriteInt8(in->ReadUInt8()); // speak mode
		buf.WriteInt8(in->ReadUInt8()); // journal mode
		buf.WriteInt8(in->ReadUInt8()); // language
		buf.WriteInt32(in->ReadUInt32()); // message type
		buf.WriteInt32(in->ReadUInt32()); // target spawn id

		std::string name;
		in->ReadString(name); // NPC names max out at 63 chars

		buf.WriteString(name);

		buf.WriteInt32(in->ReadUInt32()); // loc
		buf.WriteInt32(in->ReadUInt32());
		buf.WriteInt32(in->ReadUInt32());

		std::string old_message;
		std::string new_message;

		in->ReadString(old_message);

		ServerToLaurionConvertLinks(new_message, old_message);

		buf.WriteString(new_message);

		auto outapp = new EQApplicationPacket(OP_SpecialMesg, buf);

		dest->FastQueuePacket(&outapp, ack_req);
		delete in;
	}

	ENCODE(OP_SpawnAppearance)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;

		unsigned char* emu_buffer = in->pBuffer;

		SpawnAppearance_Struct* sas = (SpawnAppearance_Struct*)emu_buffer;

		if (sas->type != AppearanceType::Size)
		{
			//laurion struct is different than rof2's but the idea is the same
			//we will probably want to better implement Laurion's structure later
			auto outapp = new EQApplicationPacket(OP_SpawnAppearance, sizeof(structs::SpawnAppearance_Struct));
			structs::SpawnAppearance_Struct *eq = (structs::SpawnAppearance_Struct*)outapp->pBuffer;

			eq->spawn_id = sas->spawn_id;
			eq->type = ServerToLaurionSpawnAppearanceType(sas->type);
			eq->parameter = sas->parameter;

			dest->FastQueuePacket(&outapp, ack_req);
			delete in;
			return;
		}

		auto outapp = new EQApplicationPacket(OP_ChangeSize, sizeof(ChangeSize_Struct));

		ChangeSize_Struct* css = (ChangeSize_Struct*)outapp->pBuffer;

		css->EntityID = sas->spawn_id;
		css->Size = (float)sas->parameter;
		css->Unknown08 = 0;
		css->Unknown12 = 1.0f;

		dest->FastQueuePacket(&outapp, ack_req);
		delete in;
	}

	ENCODE(OP_SpawnDoor)
	{
		SETUP_VAR_ENCODE(Door_Struct);
		int door_count = __packet->size / sizeof(Door_Struct);
		int total_length = door_count * sizeof(structs::Door_Struct);
		ALLOC_VAR_ENCODE(structs::Door_Struct, total_length);

		int r;
		for (r = 0; r < door_count; r++) {
			strncpy(eq[r].name, emu[r].name, 32);
			eq[r].DefaultY = emu[r].yPos;
			eq[r].DefaultX = emu[r].xPos;
			eq[r].DefaultZ = emu[r].zPos;
			eq[r].DefaultHeading = emu[r].heading;
			eq[r].DefaultDoorAngle = emu[r].incline;
			eq[r].Y = emu[r].yPos;
			eq[r].X = emu[r].xPos;
			eq[r].Z = emu[r].zPos;
			eq[r].Heading = emu[r].heading;
			//there's a door angle here but im not sure if / what we set it to since ive literally never seen it as anything but 0 on live
			//based on pattern it probably is supposed to match the default angle?
			//I'm not 100% sure this is a float it might be a uint32
			eq[r].DoorAngle = emu[r].incline;
			eq[r].ScaleFactor = emu[r].size;
			eq[r].Id = emu[r].doorId;
			eq[r].Type = emu[r].opentype;
			eq[r].State = emu[r].state_at_spawn;
			eq[r].DefaultState = emu[r].invert_state;
			eq[r].Param = emu[r].door_param;
			eq[r].bVisible = 1;
			eq[r].bUsable = 1;
		}

		FINISH_ENCODE();
	}

	ENCODE(OP_Stun)
	{
		ENCODE_LENGTH_EXACT(Stun_Struct);
		SETUP_DIRECT_ENCODE(Stun_Struct, structs::Stun_Struct);

		OUT(duration);
		eq->unknown005 = 163;
		eq->unknown006 = 67;

		FINISH_ENCODE();
	}

	ENCODE(OP_WearChange)
	{
		ENCODE_LENGTH_EXACT(WearChange_Struct);
		SETUP_DIRECT_ENCODE(WearChange_Struct, structs::WearChange_Struct);

		OUT(spawn_id);
		eq->wear_slot_id = emu->wear_slot_id;
		eq->armor_id = emu->material;
		eq->variation = emu->unknown06;
		eq->material = emu->elite_material;
		eq->new_armor_id = emu->hero_forge_model;
		eq->new_armor_type = emu->unknown18;
		eq->color = emu->color.Color;

		FINISH_ENCODE();
	}

	ENCODE(OP_ZoneChange)
	{
		ENCODE_LENGTH_EXACT(ZoneChange_Struct);
		SETUP_DIRECT_ENCODE(ZoneChange_Struct, structs::ZoneChange_Struct);

		memcpy(eq->char_name, emu->char_name, sizeof(emu->char_name));
		OUT(zoneID);
		OUT(instanceID);
		OUT(y);
		OUT(x);
		OUT(z)
			OUT(zone_reason);
		OUT(success);

		if (eq->success < 0)
			eq->success -= 1;

		FINISH_ENCODE();
	}

	ENCODE(OP_ZoneEntry) { ENCODE_FORWARD(OP_ZoneSpawns); }

	ENCODE(OP_ZonePlayerToBind)
	{
		SETUP_VAR_ENCODE(ZonePlayerToBind_Struct);
		ALLOC_LEN_ENCODE(sizeof(structs::ZonePlayerToBind_Struct) + strlen(emu->zone_name));

		__packet->SetWritePosition(0);
		__packet->WriteUInt16(emu->bind_zone_id);
		__packet->WriteUInt16(emu->bind_instance_id);
		__packet->WriteFloat(emu->x);
		__packet->WriteFloat(emu->y);
		__packet->WriteFloat(emu->z);
		__packet->WriteFloat(emu->heading);
		__packet->WriteString(emu->zone_name);
		__packet->WriteUInt32(60);
		__packet->WriteUInt32(0);
		__packet->WriteUInt32(51);
		__packet->WriteUInt32(41);

		FINISH_ENCODE();
	}

	ENCODE(OP_ZoneSpawns)
	{
		EQApplicationPacket* in = *p;
		*p = nullptr;

		LogEncode(in, "OP_ZoneSpawns");

		//store away the emu struct
		unsigned char* __emu_buffer = in->pBuffer;
		Spawn_Struct* emu = (Spawn_Struct*)__emu_buffer;

		int entrycount = in->size / sizeof(Spawn_Struct);
		if (entrycount == 0 || (in->size % sizeof(Spawn_Struct)) != 0) {
			LogNetcode("[STRUCTS] Wrong size on outbound [{}]: Got [{}], expected multiple of [{}]", opcodes->EmuToName(in->GetOpcode()), in->size, sizeof(Spawn_Struct));
			delete in;
			return;
		}

		for (int i = 0; i < entrycount; i++, emu++) {
			SerializeBuffer buffer;

			auto SpawnSize = emu->size;
			if (!((emu->NPC == 0) || (emu->race <= Race::Gnome) || (emu->race == Race::Iksar) ||
				(emu->race == Race::VahShir) || (emu->race == Race::Froglok2) || (emu->race == Race::Drakkin))
				)
			{
				if (emu->size == 0)
				{
					emu->size = 6;
					SpawnSize = 6;
				}
			}

			if (SpawnSize == 0)
			{
				SpawnSize = 3;
			}

			/*
			char Name[];
			u32 SpawnId;
			u8 Level;
			float MeleeRadius;
			*/
			buffer.WriteString(emu->name);
			buffer.WriteUInt32(emu->spawnId);
			buffer.WriteUInt8(emu->level);
			if (emu->DestructibleObject) //bounding radius: we should consider supporting this officially in the future
			{
				buffer.WriteFloat(10.0f);
			}
			else
			{

				buffer.WriteFloat(SpawnSize - 0.7f);
			}

			/*
			EqGuid HashKey;
			*/
			buffer.WriteUInt32(emu->CharacterGuid.Id);
			buffer.WriteUInt16(emu->CharacterGuid.WorldId);
			buffer.WriteUInt16(0);

			/*
			u8 Type;
			ActorFlags Flags;
			*/
			buffer.WriteUInt8(emu->NPC);

			structs::Spawn_Struct_Bitfields flags;
			memset(&flags, 0, sizeof(structs::Spawn_Struct_Bitfields));

			flags.gender = emu->gender;
			flags.ispet = emu->is_pet;
			flags.afk = emu->afk;
			flags.anon = emu->anon;
			flags.gm = emu->gm;
			flags.sneak = 0;
			flags.lfg = emu->lfg;
			flags.invis = emu->invis; //we need to implement this
			flags.linkdead = 0; //on live I often see this as 1 for npcs, maybe consider adding this in the future
			flags.showhelm = emu->showhelm;
			flags.trader = emu->trader ? 1 : 0;
			flags.targetable = 1;
			flags.targetable_with_hotkey = emu->targetable_with_hotkey ? 1 : 0;
			flags.showname = emu->show_name;
			flags.buyer = emu->buyer ? 1 : 0;
			flags.title = strlen(emu->title) > 0 ? 1 : 0;
			flags.suffix = strlen(emu->suffix) > 0 ? 1 : 0;

			if (emu->DestructibleObject || emu->class_ == Class::LDoNTreasure) {
				flags.interactiveobject = 1;
			}

			//write flags
			//buffer.WriteStructure(flags);
			for (int j = 0; j < 5; ++j) {
				buffer.WriteUInt8(flags.raw[j]);
			}

			/*
			float EmitterScalingRadius;
			s32 DefaultEmitterID;
			*/

			//we don't support this atm; wouldn't be hard to add I don't think
			//RoF also supports this so might be worth implementing.
			buffer.WriteFloat(1.0f);
			buffer.WriteInt32(-1);

			if (emu->DestructibleObject || emu->class_ == Class::LDoNTreasure)
			{
				/*
				char InteractiveObjectModelName[];
				char InteractiveObjectName[];
				char InteractiveObjectOtherName[];
				*/

				buffer.WriteString(emu->DestructibleModel);
				buffer.WriteString(emu->DestructibleName2);
				buffer.WriteString(emu->DestructibleString);

				/*
				s32 CurrIOState;
				s32 ObjectAnimationID;
				*/
				buffer.WriteUInt32(emu->DestructibleAppearance);
				buffer.WriteUInt32(emu->DestructibleUnk1);

				/*
				s32 SoundId[10];
				*/
				buffer.WriteUInt32(emu->DestructibleID1);
				buffer.WriteUInt32(emu->DestructibleID2);
				buffer.WriteUInt32(emu->DestructibleID3);
				buffer.WriteUInt32(emu->DestructibleID4);
				buffer.WriteUInt32(emu->DestructibleUnk2);
				buffer.WriteUInt32(emu->DestructibleUnk3);
				buffer.WriteUInt32(emu->DestructibleUnk4);
				buffer.WriteUInt32(emu->DestructibleUnk5);
				buffer.WriteUInt32(emu->DestructibleUnk6);
				buffer.WriteUInt32(emu->DestructibleUnk7);
				/*
				s8 Collidable;
				s8 ObjectType;
				*/
				buffer.WriteUInt8(emu->DestructibleUnk8);
				buffer.WriteUInt8(emu->DestructibleUnk9);
			}

			/*
			u8 PropertyCount;
			u32 Properties[PropertyCount];
			*/
			//We don't actually support multiple body types yet, but we should consider it in the future

			if (!emu->DestructibleObject)
			{
				buffer.WriteUInt8(1);
				buffer.WriteUInt32(emu->bodytype);
			}
			else
			{
				buffer.WriteUInt8(0);
			}

			/*
			u8 HPCurrentPct;
			*/
			buffer.WriteUInt8(emu->curHp);

			/*
			s8 HairColor;
			s8 FacialHairColor;
			s8 EyeColor1;
			s8 EyeColor2;
			s8 HairStyle;
			s8 FacialHair;
			s32 Heritage;
			s32 Tattoo;
			s32 Details;
			*/

			buffer.WriteUInt8(emu->haircolor);
			buffer.WriteUInt8(emu->beardcolor);
			buffer.WriteUInt8(emu->eyecolor1);
			buffer.WriteUInt8(emu->eyecolor2);
			buffer.WriteUInt8(emu->hairstyle);
			buffer.WriteUInt8(emu->beard);
			buffer.WriteUInt32(emu->drakkin_heritage);
			buffer.WriteUInt32(emu->drakkin_tattoo);
			buffer.WriteUInt32(emu->drakkin_details);

			/*
			s8 TextureType;
			s8 Material;
			s8 Variation;
			s8 HeadType;
			*/
			buffer.WriteUInt8(emu->equip_chest2);
			buffer.WriteUInt8(0);
			buffer.WriteUInt8(0);
			buffer.WriteUInt8(emu->helm);

			/*
			float Height;
			s8 FaceStyle;
			float MyWalkSpeed;
			float RunSpeed;
			s32 Race;
			*/

			buffer.WriteFloat(emu->size);
			buffer.WriteInt8(emu->face);
			buffer.WriteFloat(emu->walkspeed);
			buffer.WriteFloat(emu->runspeed);
			buffer.WriteInt32(emu->race);

			/*
			u8 HoldingAnimation;
			u32 Deity;
			EqGuid GuildID;
			u32 Class;
			*/

			buffer.WriteUInt8(0);
			buffer.WriteUInt32(emu->deity);
			if (emu->NPC) {
				buffer.WriteInt32(-1);
				buffer.WriteUInt32(0);
			}
			else { //guilds will probably need a ton of work
				buffer.WriteUInt32(emu->guildID);
				buffer.WriteUInt32(0);
			}
			buffer.WriteUInt32(emu->class_);

			/*
			u8 PvP;
			u8 StandState;
			u8 Light;
			u8 GravityBehavior;
			*/

			buffer.WriteUInt8(0);
			buffer.WriteUInt8(emu->StandState);
			buffer.WriteUInt8(emu->light);
			buffer.WriteUInt8(emu->flymode);

			/*
			char LastName[];
			*/
			buffer.WriteString(emu->lastName);

			/*
			u8 bGuildShowAnim;
			u8 bTempPet;
			u32 MasterID;
			u8 FindBits;
			*/

			buffer.WriteUInt8(emu->guild_show);
			buffer.WriteUInt8(0);
			buffer.WriteUInt32(emu->petOwnerId);
			buffer.WriteUInt8(0);

			/*
			u32 PlayerState;
			u32 NpcTintIndex;
			u32 PrimaryTintIndex;
			u32 SecondaryTintIndex;
			*/

			buffer.WriteUInt32(emu->PlayerState);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);

			/*
			u32 EncounterLockState;
			u64 LockID;
			*/

			buffer.WriteUInt32(0);
			buffer.WriteUInt64(0);

			//u32 SeeInvis[3];
			if (emu->NPC == 1) {
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
			}

			/*
			s32 Primary;
			s32 Secondary;
			*/
			buffer.WriteUInt32(0xffffffff);
			buffer.WriteUInt32(0xffffffff);

			if ((emu->NPC == 0) || (emu->race <= Race::Gnome) || (emu->race == Race::Iksar) ||
				(emu->race == Race::VahShir) || (emu->race == Race::Froglok2) || (emu->race == Race::Drakkin)
				)
			{
				/*
				u32 ArmorColor[9];
				*/
				for (int k = EQ::textures::textureBegin; k < EQ::textures::materialCount; ++k)
				{
					buffer.WriteUInt32(emu->equipment_tint.Slot[k].Color);
				}

				/*
				Armor Armor[9];
				*/
				for (int k = EQ::textures::textureBegin; k < EQ::textures::materialCount; k++) {
					buffer.WriteUInt32(emu->equipment.Slot[k].Material);
					buffer.WriteUInt32(emu->equipment.Slot[k].Unknown1);
					buffer.WriteUInt32(emu->equipment.Slot[k].EliteModel);
					buffer.WriteUInt32(emu->equipment.Slot[k].HerosForgeModel);
					buffer.WriteUInt32(emu->equipment.Slot[k].Unknown2);
				}
			}
			else
			{
				//Armor Armor[3];
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);

				buffer.WriteUInt32(emu->equipment.Primary.Material);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);

				buffer.WriteUInt32(emu->equipment.Secondary.Material);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
			}

			//u8 CPhysicsData[20];
			structs::Spawn_Struct_Position position;
			memset(&position, 0, sizeof(structs::Spawn_Struct_Position));

			position.y = emu->y;
			position.deltaZ = emu->deltaZ;
			position.deltaX = emu->deltaX;
			position.x = emu->x;
			position.heading = emu->heading;
			position.deltaHeading = emu->deltaHeading;
			position.z = emu->z;
			position.animation = emu->animation;
			position.deltaY = emu->deltaY;

			//buffer.WriteStructure(position);
			for (int j = 0; j < 5; ++j) {
				buffer.WriteUInt32(position.raw[j]);
			}

			/*
			if(Flags.title) {
				char Title[];
			}
			*/
			if (flags.title) {
				buffer.WriteString(emu->title);
			}

			/*
			if(Flags.suffix) {
				char Suffix[];
			}
			*/
			if (flags.suffix) {
				buffer.WriteString(emu->suffix);
			}

			/*
			u32 Unknown0x0164;
			s32 SplineID;
			*/

			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);

			/*
			u8 Mercenary;
			*/
			buffer.WriteUInt8(emu->IsMercenary);

			/*
				char realEstateItemGuid[];
				s32 RealEstateID;
				s32 RealEstateItemId;
			*/

			buffer.WriteString("0000000000000000");
			buffer.WriteInt32(-1);
			buffer.WriteInt32(-1);

			/*
			s32 MercId;
			s32 ContractorID;
			u32 Birthdate;
			u8 bAlwaysShowAura;
			*/

			buffer.WriteInt32(0);
			buffer.WriteInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt8(0);

			/*
			u32 physicsEffectCount;
			PhysicsEffect physicsEffects[physicsEffectCount];
			*/
			buffer.WriteUInt32(0);

			//s32 SpawnStatus[6];
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);
			buffer.WriteUInt32(0);

			if (flags.interactiveobject && emu->DestructibleUnk9 == 4) {
				/*
				s32 BannerIndex0;
				s32 BannerIndex1;
				s32 BannerTint0;
				s32 BannerTint1;
				*/
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
				buffer.WriteUInt32(0);
			}

			auto outapp = new EQApplicationPacket(OP_ZoneEntry, buffer.size());
			outapp->WriteData(buffer.buffer(), buffer.size());
			dest->FastQueuePacket(&outapp, ack_req);
		}

		delete in;
	}

	// DECODE methods
	DECODE(OP_Animation)
	{
		DECODE_LENGTH_EXACT(structs::Animation_Struct);
		SETUP_DIRECT_DECODE(Animation_Struct, structs::Animation_Struct);

		IN(spawnid);
		IN(action);
		IN(speed);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ApplyPoison)
	{
		DECODE_LENGTH_EXACT(structs::ApplyPoison_Struct);
		SETUP_DIRECT_DECODE(ApplyPoison_Struct, structs::ApplyPoison_Struct);

		emu->inventorySlot = LaurionToServerTypelessSlot(eq->inventorySlot, invtype::typePossessions);
		IN(success);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_AugmentInfo)
	{
		DECODE_LENGTH_EXACT(structs::AugmentInfo_Struct);
		SETUP_DIRECT_DECODE(AugmentInfo_Struct, structs::AugmentInfo_Struct);

		IN(itemid);
		IN(window);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_AugmentItem)
	{
		DECODE_LENGTH_EXACT(structs::AugmentItem_Struct);
		SETUP_DIRECT_DECODE(AugmentItem_Struct, structs::AugmentItem_Struct);

		emu->container_slot = LaurionToServerSlot(eq->container_slot);
		emu->augment_slot = LaurionToServerSlot(eq->augment_slot);
		emu->container_index = eq->container_index;
		emu->augment_index = eq->augment_index;
		emu->dest_inst_id = eq->dest_inst_id;
		emu->augment_action = eq->augment_action;

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_BlockedBuffs)
	{
		DECODE_LENGTH_EXACT(structs::BlockedBuffs_Struct);
		SETUP_DIRECT_DECODE(BlockedBuffs_Struct, structs::BlockedBuffs_Struct);

		for (uint32 i = 0; i < BLOCKED_BUFF_COUNT; ++i)
			emu->SpellID[i] = eq->SpellID[i];

		IN(Count);
		IN(Pet);
		IN(Initialise);
		IN(Flags);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_CastSpell)
	{
		DECODE_LENGTH_EXACT(structs::CastSpell_Struct);
		SETUP_DIRECT_DECODE(CastSpell_Struct, structs::CastSpell_Struct);

		emu->slot = static_cast<uint32>(LaurionToServerCastingSlot(static_cast<spells::CastingSlot>(eq->slot)));

		IN(spell_id);
		IN(inventoryslot);
		IN(target_id);
		// Position data is in cs_unknown array, not used by server
		emu->y_pos = 0;
		emu->x_pos = 0;
		emu->z_pos = 0;
		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ChannelMessage)
	{
		unsigned char* __eq_buffer = __packet->pBuffer;

		char* InBuffer = (char*)__eq_buffer;

		char Sender[64];
		char Target[64];

		VARSTRUCT_DECODE_STRING(Sender, InBuffer);
		VARSTRUCT_DECODE_STRING(Target, InBuffer);

		//packet seems the same as rof2 with 4 more empty bytes before language
		InBuffer += 8;

		uint32 Language = VARSTRUCT_DECODE_TYPE(uint32, InBuffer);
		uint32 Channel = VARSTRUCT_DECODE_TYPE(uint32, InBuffer);

		InBuffer += 5;

		uint32 Skill = VARSTRUCT_DECODE_TYPE(uint32, InBuffer);

		std::string old_message = InBuffer;
		std::string new_message;
		LaurionToServerConvertLinks(new_message, old_message);

		__packet->size = sizeof(ChannelMessage_Struct) + new_message.length() + 1;
		__packet->pBuffer = new unsigned char[__packet->size];
		ChannelMessage_Struct* emu = (ChannelMessage_Struct*)__packet->pBuffer;

		strn0cpy(emu->targetname, Target, sizeof(emu->targetname));
		strn0cpy(emu->sender, Target, sizeof(emu->sender));
		emu->language = Language;
		emu->chan_num = Channel;
		emu->skill_in_language = Skill;
		strcpy(emu->message, new_message.c_str());

		delete[] __eq_buffer;
	}

	DECODE(OP_ClickDoor)
	{
		DECODE_LENGTH_EXACT(structs::ClickDoor_Struct);
		SETUP_DIRECT_DECODE(ClickDoor_Struct, structs::ClickDoor_Struct);

		IN(doorid);
		IN(player_id);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ClientUpdate)
	{
		// for some odd reason, there is an extra byte on the end of this on occasion..
		DECODE_LENGTH_ATLEAST(structs::PlayerPositionUpdateClient_Struct);
		SETUP_DIRECT_DECODE(PlayerPositionUpdateClient_Struct, structs::PlayerPositionUpdateClient_Struct);

		IN(spawn_id);
		IN(vehicle_id);
		IN(sequence);
		emu->x_pos = eq->position.x;
		emu->y_pos = eq->position.y;
		emu->z_pos = eq->position.z;
		emu->heading = eq->position.heading;
		emu->delta_x = eq->position.delta_x;
		emu->delta_y = eq->position.delta_y;
		emu->delta_z = eq->position.delta_z;
		emu->delta_heading = eq->position.delta_heading;
		emu->animation = eq->position.animation;

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_Consider)
	{
		DECODE_LENGTH_EXACT(structs::Consider_Struct);
		SETUP_DIRECT_DECODE(Consider_Struct, structs::Consider_Struct);

		IN(playerid);
		IN(targetid);
		IN(faction);
		IN(level);
		//emu->cur_hp = 1;
		//emu->max_hp = 2;
		//emu->pvpcon = 0;

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ConsiderCorpse) { DECODE_FORWARD(OP_Consider); }

	DECODE(OP_DeleteItem)
	{
		DECODE_LENGTH_EXACT(structs::DeleteItem_Struct);
		SETUP_DIRECT_DECODE(DeleteItem_Struct, structs::DeleteItem_Struct);

		emu->from_slot = LaurionToServerSlot(eq->from_slot);
		emu->to_slot = LaurionToServerSlot(eq->to_slot);
		IN(number_in_stack);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_EnterWorld)
	{
		DECODE_LENGTH_EXACT(structs::EnterWorld_Struct);
		SETUP_DIRECT_DECODE(EnterWorld_Struct, structs::EnterWorld_Struct);

		memcpy(emu->name, eq->name, sizeof(emu->name));
		emu->return_home = 0;
		emu->tutorial = 0;

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_GMTraining)
	{
		DECODE_LENGTH_EXACT(structs::GMTrainee_Struct);
		SETUP_DIRECT_DECODE(GMTrainee_Struct, structs::GMTrainee_Struct);

		IN(npcid);
		IN(playerid);

		for (int i = 0; i < 100; ++i) {
			IN(skills[i]);
		}

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_GroupDisband)
	{
		DECODE_LENGTH_EXACT(structs::GroupGeneric_Struct);
		SETUP_DIRECT_DECODE(GroupGeneric_Struct, structs::GroupGeneric_Struct);

		memcpy(emu->name1, eq->name1, sizeof(emu->name1));
		memcpy(emu->name2, eq->name2, sizeof(emu->name2));

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_GroupInvite)
	{
		DECODE_LENGTH_EXACT(structs::GroupGeneric_Struct);
		SETUP_DIRECT_DECODE(GroupGeneric_Struct, structs::GroupGeneric_Struct);

		memcpy(emu->name1, eq->name1, sizeof(emu->name1));
		memcpy(emu->name2, eq->name2, sizeof(emu->name2));

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_GroupInvite2)
	{
		DECODE_FORWARD(OP_GroupInvite);
	}

	DECODE(OP_MoveItem)
	{
		// [DH_LOG_CLEANUP] Per-packet MoveItem raw size + INCOMING hex disabled; keep size mismatch (real decode failure).
		// Full hex dump of raw client packet — DH deposit uses Type=38 (to_slot); if client sends different size we drop after this log
		// Log(Logs::General, Logs::Error, "[MOVEITEM] INCOMING raw client packet size=%u (expected Laurion MoveItem_Struct=%u)", __packet->size, (unsigned)sizeof(structs::MoveItem_Struct));
		// if (__packet->pBuffer && __packet->size > 0) {
		// 	for (uint32_t offset = 0; offset < __packet->size; offset += 16) {
		// 		std::ostringstream line;
		// 		line << std::setfill('0') << std::setw(4) << std::dec << offset << ": ";
		// 		for (uint32_t i = offset; i < offset + 16 && i < __packet->size; i++)
		// 			line << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)__packet->pBuffer[i] << " ";
		// 		Log(Logs::General, Logs::Error, "[MOVEITEM] INCOMING raw client hex (Type at ~byte 12; Type=38=DH) %s", line.str().c_str());
		// 	}
		// }
		if (__packet->size != sizeof(structs::MoveItem_Struct)) {
			Log(Logs::General, Logs::Error, "[MOVEITEM] Size mismatch — packet will be dropped, zone handler will not see it.");
		}

		DECODE_LENGTH_EXACT(structs::MoveItem_Struct);
		SETUP_DIRECT_DECODE(MoveItem_Struct, structs::MoveItem_Struct);

		Log(Logs::Detail, Logs::Netcode, "Laurion::DECODE(OP_MoveItem)");

		// [DH_LOG_CLEANUP] Per-packet MoveItem decode slot spam disabled.
		// Type=0 Slot=35 = cursor (decodes to server slot 33). DH deposit uses Type=38 (to_slot); if client sends
		// different packet size for Type=38, DECODE_LENGTH_EXACT fails above and we never reach this log — see raw hex above.
		// Log(Logs::General, Logs::Error, "[MOVEITEM_DECODE] Client from_slot: Type=%d Slot=%d SubIndex=%d AugIndex=%d",
		// 	(int)eq->from_slot.Type, (int)eq->from_slot.Slot, (int)eq->from_slot.SubIndex, (int)eq->from_slot.AugIndex);
		// Log(Logs::General, Logs::Error, "[MOVEITEM_DECODE] Client to_slot:   Type=%d Slot=%d SubIndex=%d AugIndex=%d  (Type=38 = DH window; Type=0 Slot=35 = cursor)",
		// 	(int)eq->to_slot.Type, (int)eq->to_slot.Slot, (int)eq->to_slot.SubIndex, (int)eq->to_slot.AugIndex);
		IN(number_in_stack);
		// Log(Logs::General, Logs::Error, "[MOVEITEM_DECODE] Client number_in_stack=%u", emu->number_in_stack);

		emu->from_slot = LaurionToServerSlot(eq->from_slot);
		emu->to_slot = LaurionToServerSlot(eq->to_slot);

		// Log(Logs::General, Logs::Error, "[MOVEITEM_DECODE] Decoded server from_slot=%u to_slot=%u (0x%X = INVALID if DH/depot not mapped)",
		// 	emu->from_slot, emu->to_slot, (unsigned)emu->to_slot);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ReadBook)
	{
		DECODE_LENGTH_EXACT(structs::BookRequest_Struct);
		SETUP_DIRECT_DECODE(BookRequest_Struct, structs::BookRequest_Struct);

		Log(Logs::Detail, Logs::Netcode, "Laurion::DECODE(OP_ReadBook) - Converting 164 byte packet to 28 byte server format");

		IN(window);
		IN(type);
		IN(invslot);
		IN(target_id);
		memcpy(emu->txtfile, eq->txtfile, sizeof(emu->txtfile));

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_RemoveBlockedBuffs) { DECODE_FORWARD(OP_BlockedBuffs); }

	DECODE(OP_SetServerFilter)
	{
		DECODE_LENGTH_EXACT(structs::SetServerFilter_Struct);
		SETUP_DIRECT_DECODE(SetServerFilter_Struct, structs::SetServerFilter_Struct);

		int r;
		for (r = 0; r < 29; r++) {
			// Size 68 in Laurion
			IN(filters[r]);
		}

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ShopPlayerBuy)
	{
		DECODE_LENGTH_EXACT(structs::Merchant_Sell_Request_Struct);
		SETUP_DIRECT_DECODE(Merchant_Sell_Struct, structs::Merchant_Sell_Request_Struct);

		IN(npcid);
		IN(playerid);
		IN(itemslot);
		IN(quantity);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ShopPlayerSell)
	{
		DECODE_LENGTH_EXACT(structs::Merchant_Purchase_Request_Struct);
		SETUP_DIRECT_DECODE(Merchant_Purchase_Struct, structs::Merchant_Purchase_Request_Struct);

		IN(npcid);
		emu->itemslot = LaurionToServerTypelessSlot(eq->inventory_slot, invtype::typePossessions);
		IN(quantity);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ShopRequest)
	{
		DECODE_LENGTH_EXACT(structs::MerchantClickRequest_Struct);
		SETUP_DIRECT_DECODE(MerchantClick_Struct, structs::MerchantClickRequest_Struct);

		IN(npc_id);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_LootItem)
	{
		DECODE_LENGTH_EXACT(structs::LootingItem_Struct);
		SETUP_DIRECT_DECODE(LootingItem_Struct, structs::LootingItem_Struct);

		Log(Logs::Detail, Logs::Netcode, "Laurion::DECODE(OP_LootItem)");

		IN(lootee);
		IN(looter);
		emu->slot_id = eq->slot_id; // Laurion uses same slot mapping as server
		IN(auto_loot);
		// eq->unknown16 is ignored (Laurion-specific field)

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_SpawnAppearance) {
		DECODE_LENGTH_EXACT(structs::SpawnAppearance_Struct);
		SETUP_DIRECT_DECODE(SpawnAppearance_Struct, structs::SpawnAppearance_Struct);

		IN(spawn_id);
		emu->type = LaurionToServerSpawnAppearanceType(eq->type);
		IN(parameter);

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_TradeSkillCombine)
	{
		DECODE_LENGTH_EXACT(structs::NewCombine_Struct);
		SETUP_DIRECT_DECODE(NewCombine_Struct, structs::NewCombine_Struct);

		emu->container_slot = LaurionToServerSlot(eq->container_slot);
		emu->guildtribute_slot = LaurionToServerSlot(eq->guildtribute_slot); // this should only return INVALID_INDEX until implemented

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_WearChange)
	{
		DECODE_LENGTH_EXACT(structs::WearChange_Struct);
		SETUP_DIRECT_DECODE(WearChange_Struct, structs::WearChange_Struct);

		IN(spawn_id);
		emu->wear_slot_id = eq->wear_slot_id;
		emu->material = eq->armor_id;
		emu->unknown06 = eq->variation;
		emu->elite_material = eq->material;
		emu->hero_forge_model = eq->new_armor_id;
		emu->unknown18 = eq->new_armor_type;
		emu->color.Color = eq->color;

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ZoneEntry)
	{
		DECODE_LENGTH_EXACT(structs::ClientZoneEntry_Struct);
		SETUP_DIRECT_DECODE(ClientZoneEntry_Struct, structs::ClientZoneEntry_Struct);

		memcpy(emu->char_name, eq->char_name, sizeof(emu->char_name));

		FINISH_DIRECT_DECODE();
	}

	DECODE(OP_ZoneChange)
	{
		DECODE_LENGTH_EXACT(structs::ZoneChange_Struct);
		SETUP_DIRECT_DECODE(ZoneChange_Struct, structs::ZoneChange_Struct);

		memcpy(emu->char_name, eq->char_name, sizeof(emu->char_name));
		IN(zoneID);
		IN(instanceID);
		IN(y);
		IN(x);
		IN(z)
			IN(zone_reason);
		IN(success);

		FINISH_DIRECT_DECODE();
	}

	//Naive version but should work well enough for now
	int ExtractIDFile(const std::string& input) {
		std::string number;
		for (char ch : input) {
			if (std::isdigit(static_cast<unsigned char>(ch))) {
				number += ch;
			}
		}

		if (number.empty()) {
			return 0;
		}

		return std::stoi(number);
	}

	static const size_t LAURION_BLOB_TOTAL_SIZE = 4096;

#if 0  // [DH_BLOB_LEGACY] Previous blob serializer (variable prefix, [DH_BLOB_ORDER_AUDIT], audits, synthetic ID). Preserved — do not delete.
	// Blob layout for client parser sub_14065bd70 (item definition stream). Used only for Dragon's Hoard length-prefixed blob.
	// Client parser needs enough blob data to complete without running off the end. We write known fields at fixed offsets, then pad to total size.
	// [DH_CLEANUP_R1] Removed DH_BLOB_ENTRY and other investigation-only logging from blob serializer.
	static const size_t LAURION_BLOB_TOTAL_SIZE = 4096;
	// [DH_BLOB_ORDER_AUDIT] SerializeItemDefinitionLaurionBlob — write order (keep in sync with body below):
	//   1 WriteUInt8        ItemClass (client arg1[0xe9] / stream byte after cursor sync in sub_14065bd70)
	//   2 WriteBytes        Name + NUL (strlen+1)
	//   3 WriteBytes        Lore + NUL (strlen+1)
	//   4 WriteInt32        ExtractIDFile(item->IDFile)
	//   5 WriteInt32        0 (IDFile2)
	//   6 WriteInt32        ItemNumber: (uint32_t)item->ID | 0x00800000u [DH_SYNTHETIC_ID_V2] (plain ID line commented in body)
	//   7 WriteInt32        Weight
	//   8 WriteUInt8        NoRent
	//   9 WriteUInt8        NoDrop
	//  10 WriteUInt8        Attuneable
	//  11 WriteUInt8        Size
	//  12 WriteInt32        Slots
	//  13 WriteUInt32       max(Price,1) Cost
	//  14 WriteInt32        Icon
	//  15 WriteUInt8        0 eGMRequirement
	//  16 WriteUInt8        0 unknown (client +0xea)
	//  17 WriteUInt8        CR
	//  18 WriteUInt8        DR
	//  19 WriteUInt8        PR
	//  20 WriteUInt8        MR
	//  21 WriteUInt8        FR
	//  22 WriteUInt8        SVCorruption
	//  23 WriteUInt8        AStr
	//  24 WriteUInt8        ASta
	//  25 WriteUInt8        AAgi
	//  26 WriteUInt8        ADex
	//  27 WriteUInt8        ACha
	//  28 WriteUInt8        AInt
	//  29 WriteUInt8        AWis
	//  30 WriteInt32        HP
	//  31 WriteInt32        Mana
	//  32 WriteInt32        Endur
	//  33 WriteInt32        AC
	//  34 WriteInt32        Regen
	//  35 WriteInt32        ManaRegen
	//  36 WriteInt32        EnduranceRegen
	//  37 WriteInt32        Classes
	//  38 WriteInt32        Races
	//  39 WriteInt32        Deity
	//  40 WriteBytes        zero pad to LAURION_BLOB_TOTAL_SIZE (4096) if needed
	static void SerializeItemDefinitionLaurionBlob(SerializeBuffer& buffer, const EQ::ItemData* item) {
		// byte 0: Type / ItemClass (arg1[0xe9])
		buffer.WriteUInt8(item->ItemClass);
		// Name: null-terminated string (variable length)
		// (commented out: fixed 64-byte zero-padded)
		// char name64[64];
		// memset(name64, 0, sizeof(name64));
		// size_t nameLen = std::min(strlen(item->Name), (size_t)63);
		// memcpy(name64, item->Name, nameLen);
		// buffer.WriteBytes(name64, 64);
		// [DH_SHORT_NAME_REVERTED]
		size_t name_consumed = strlen(item->Name) + 1;
		buffer.WriteBytes(item->Name, name_consumed);
		// Lore: null-terminated string (variable length)
		// (commented out: fixed 80-byte zero-padded)
		// char lore80[80];
		// memset(lore80, 0, sizeof(lore80));
		// size_t loreLen = std::min(strlen(item->Lore), (size_t)79);
		// memcpy(lore80, item->Lore, loreLen);
		// buffer.WriteBytes(lore80, 80);
		size_t lore_consumed = strlen(item->Lore) + 1;
		buffer.WriteBytes(item->Lore, lore_consumed);
		// [DH_BLOB_C0_AUDIT] Absolute blob offset 0xC0 (192) is NOT a fixed field: bytes [0]=ItemClass, then
		// null-terminated Name, then null-terminated Lore. Fixed binary tail starts at
		// P = 1 + name_consumed + lore_consumed. If 0xC0 < P, the byte is inside ItemClass/Name/Lore.
		// If P <= 0xC0 < P+87, map rel = 0xC0 - P to the tail layout (87 bytes):
		//   0-3 IDFile, 4-7 IDFile2(0), 8-11 ItemNumber, 12-15 Weight, 16 NoRent, 17 NoDrop, 18 Attuneable,
		//   19 Size, 20-23 Slots, 24-27 Cost, 28-31 Icon, 32 eGM(0), 33 unknown(0), 34-39 CR,DR,PR,MR,FR,SVCorruption,
		//   40-46 STR,STA,AGI,DEX,CHA,INT,WIS, 47-50 HP, 51-54 Mana, 55-58 Endur, 59-62 AC, 63-66 Regen,
		//   67-70 ManaRegen, 71-74 EnduranceRegen, 75-78 Classes, 79-82 Races, 83-86 Deity.
		// If 0xC0 >= P+87, the byte is in zero padding to LAURION_BLOB_TOTAL_SIZE.
		// Example: P=105 => 0xC0 is first padding byte after Deity. P=121 => 0xC0 is first byte of EnduranceRegen (LE).
		// [DH_BLOB_F0_AUDIT] Absolute offset 0xF0 (240): same P and 87-byte tail as [DH_BLOB_C0_AUDIT].
		//   If 240 < P: byte is inside ItemClass/Name/Lore (which substring depends on P).
		//   If P <= 240 < P+87: rel = 240 - P maps to the same tail table (rel 0..86); e.g. rel 40 = AStr, rel 83-86 = Deity (LE).
		//   If 240 >= P+87: zero padding to LAURION_BLOB_TOTAL_SIZE.
		//   Tail coverage of 240 requires 154 <= P <= 240 (since rel <= 86 => P >= 240-86 = 154).
		// Typical item (short name + short/empty lore): P is often ~20–120, so P+87 < 240 → 0xF0 is padding (0x00), not a field.
		// Long prefix example: P=200 => rel=40 => uint8 item->AStr (STR stat).
		// LogInfo("[DH_BLOB_FIX] item_id={} name_len={} lore_len={}", item->ID, name_consumed, lore_consumed);
		// IDFile, IDFile2, ItemNumber, Weight, ... (offsets now variable after Name/Lore)
		// bytes 145-148: IDFile (int32)
		buffer.WriteInt32(ExtractIDFile(item->IDFile));
		// bytes 149-152: IDFile2 (int32, 0)
		buffer.WriteInt32(0);
		// [DH_BLOB_ID_AUDIT] ItemNumber / server item ID: single write in this blob.
		//   Code: WriteInt32(static_cast<int32_t>(item->ID)) — 4 bytes, little-endian (SerializeBuffer convention).
		//   Active serializer: [DH_SYNTHETIC_ID] / [DH_SYNTHETIC_ID_V2] OR 0x00800000; plain WriteInt32(item->ID) commented below.
		//   Position in fixed tail (after ItemClass + Name\0 + Lore\0): relative bytes [8..11] of the 87-byte tail
		//   (tail byte 0 = start of IDFile int32). Absolute blob indices: (P+8)..(P+11) with
		//   P = 1 + strlen(Name)+1 + strlen(Lore)+1.
		//   IDFile (tail [0..3]) is separate: ExtractIDFile(item->IDFile) from the item's IDFile string, not item->ID.
		// Synthetic / non-client-db IDs: the client still receives this int32 in the definition stream; if it does
		// not match a row in the client's local item table, UI/tooltips may fall back to blob fields, show wrong
		// art, or reject the item — behavior is client-dependent (verify in client, e.g. sub_14065bd70 callers).
		// bytes 153-156: ID / ItemNumber (int32) — obsolete fixed-offset comment (valid only if Name/Lore were fixed 64+80)
		// [DH_SYNTHETIC_ID] OR in 0x00800000 on ItemNumber so the int32 does not match a real client item DB id; goal is
		// to force the Laurion client to treat this as a stream-defined item and use blob-backed fields (e.g. near
		// item+0x140 in the client struct) instead of the cached/local-DB template path (e.g. item+0xc0 region).
		// [DH_SYNTHETIC_ID_V2] Synthetic ItemNumber active: original item->ID WriteInt32 stays commented; no lines deleted.
		// buffer.WriteInt32(static_cast<int32_t>(item->ID));
		buffer.WriteInt32(static_cast<int32_t>(static_cast<uint32_t>(item->ID) | 0x00800000u));
		// bytes 157-160: Weight (int32)
		buffer.WriteInt32(item->Weight);
		// byte 161: NoRent (uint8)
		buffer.WriteUInt8(item->NoRent);
		// byte 162: NoDrop / IsDroppable (uint8)
		buffer.WriteUInt8(item->NoDrop);
		// byte 163: Attuneable (uint8)
		buffer.WriteUInt8(item->Attuneable);
		// byte 164: Size (uint8)
		buffer.WriteUInt8(item->Size);
		// bytes 165-168: Slots / EquipSlots (int32)
		buffer.WriteInt32(static_cast<int32_t>(item->Slots));
		// bytes 169-172: Cost (uint32), ensure nonzero
		buffer.WriteUInt32(std::max(static_cast<uint32_t>(item->Price), 1u));
		// bytes 173-176: Icon / IconNumber (int32)
		buffer.WriteInt32(item->Icon);
		// byte 177: eGMRequirement (uint8, 0)
		buffer.WriteUInt8(0);
		// [DH_BLOB_STATS] Extended blob fields — resistances, stats, AC, HP, classes/races (sub_14065bd70):
		// +0xea: unknown flag (1 byte)
		buffer.WriteUInt8(0);
		// +0xf5: CR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->CR));
		// +0xf8: DR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->DR));
		// +0xf9: PR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->PR));
		// +0xf7: MR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->MR));
		// +0xf6: FR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->FR));
		// +0xfa: SVCorruption (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->SVCorruption));
		// +0xfb: STR (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->AStr));
		// +0xfc: STA (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->ASta));
		// +0xfd: AGI (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->AAgi));
		// +0xfe: DEX (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->ADex));
		// +0xff: CHA (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->ACha));
		// +0x100: INT (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->AInt));
		// +0x101: WIS (1 byte)
		buffer.WriteUInt8(static_cast<uint8_t>(item->AWis));
		// +0x104: HP (4 bytes)
		buffer.WriteInt32(item->HP);
		// +0x108: Mana (4 bytes)
		buffer.WriteInt32(item->Mana);
		// +0x56c: Endurance (4 bytes) — ItemData::Endur
		buffer.WriteInt32(item->Endur);
		// +0x10c: AC (4 bytes)
		buffer.WriteInt32(item->AC);
		// +0x574: HP regen (4 bytes)
		buffer.WriteInt32(item->Regen);
		// +0x578: Mana regen (4 bytes)
		buffer.WriteInt32(item->ManaRegen);
		// +0x57c: Endurance regen (4 bytes)
		buffer.WriteInt32(item->EnduranceRegen);
		// +0x140: Classes (4 bytes)
		buffer.WriteInt32(static_cast<int32_t>(item->Classes));
		// +0x144: Races (4 bytes)
		buffer.WriteInt32(static_cast<int32_t>(item->Races));
		// +0x148: Deity (4 bytes)
		buffer.WriteInt32(static_cast<int32_t>(item->Deity));
		// pad to LAURION_BLOB_TOTAL_SIZE (4096) [DH_BLOB_ORDER_AUDIT] step 40
		const size_t pos = buffer.size();
		if (pos < LAURION_BLOB_TOTAL_SIZE) {
			const size_t pad_len = LAURION_BLOB_TOTAL_SIZE - pos;
			static uint8_t zeroes[LAURION_BLOB_TOTAL_SIZE] = {0};
			buffer.WriteBytes(reinterpret_cast<const char*>(zeroes), pad_len);
		}
	}
#endif  // [DH_BLOB_LEGACY]

	// [DH_BLOB_REWRITE] Numeric tail from IDFile onward matches mq2-laurion ItemDefinition order (0x0b0–0x14b), then pad to LAURION_BLOB_TOTAL_SIZE.
	// [DH_BLOB_VARLEN] Name/Lore are variable-length NUL-terminated strings (not fixed Name[64]/LoreName[80]/AdvancedLoreName[32]).
	// 0x0ec Lore int32 uses ItemData::LoreGroup (MQ2 ItemDefinition::Lore). [DH_SYNTHETIC_ID_V2] remains only in legacy #if 0 above.
	static void SerializeItemDefinitionLaurionBlob(SerializeBuffer& buffer, const EQ::ItemData* item) {
		// [DH_BLOB_VARLEN] Fixed-size ItemDefinition-style prefix (do not delete):
		// char name64[64]{};
		// char lore80[80]{};
		// {
		// 	const size_t nlen = std::min(strlen(item->Name), (size_t)63);
		// 	memcpy(name64, item->Name, nlen);
		// 	name64[nlen] = '\0';
		// }
		// buffer.WriteBytes(name64, 64);
		// {
		// 	const size_t llen = std::min(strlen(item->Lore), (size_t)79);
		// 	memcpy(lore80, item->Lore, llen);
		// 	lore80[llen] = '\0';
		// }
		// buffer.WriteBytes(lore80, 80);
		// char advanced_lore[32]{};
		// buffer.WriteBytes(advanced_lore, sizeof(advanced_lore));

		buffer.WriteBytes(item->Name, strlen(item->Name) + 1);
		buffer.WriteBytes(item->Lore, strlen(item->Lore) + 1);

		buffer.WriteInt32(ExtractIDFile(item->IDFile));
		buffer.WriteInt32(0);
		buffer.WriteInt32(static_cast<int32_t>(item->ID));
		buffer.WriteInt32(static_cast<int32_t>(item->Slots));
		buffer.WriteInt32(static_cast<int32_t>(item->Price));
		buffer.WriteInt32(item->Icon);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(item->Weight);
		buffer.WriteUInt8(item->NoRent);
		buffer.WriteUInt8(item->NoDrop);
		buffer.WriteUInt8(item->Attuneable);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(0);
		buffer.WriteUInt8(item->Size);
		buffer.WriteUInt8(item->ItemClass);
		buffer.WriteUInt8(item->Tradeskills ? 1u : 0u);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(item->LoreGroup);
		buffer.WriteInt32(0);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(static_cast<uint8_t>(item->CR));
		buffer.WriteUInt8(static_cast<uint8_t>(item->FR));
		buffer.WriteUInt8(static_cast<uint8_t>(item->MR));
		buffer.WriteUInt8(static_cast<uint8_t>(item->DR));
		buffer.WriteUInt8(static_cast<uint8_t>(item->PR));
		buffer.WriteUInt8(static_cast<uint8_t>(item->SVCorruption));
		buffer.WriteUInt8(static_cast<uint8_t>(item->AStr));
		buffer.WriteUInt8(static_cast<uint8_t>(item->ASta));
		buffer.WriteUInt8(static_cast<uint8_t>(item->AAgi));
		buffer.WriteUInt8(static_cast<uint8_t>(item->ADex));
		buffer.WriteUInt8(static_cast<uint8_t>(item->ACha));
		buffer.WriteUInt8(static_cast<uint8_t>(item->AInt));
		buffer.WriteUInt8(static_cast<uint8_t>(item->AWis));
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(item->HP);
		buffer.WriteInt32(item->Mana);
		buffer.WriteInt32(item->AC);
		buffer.WriteInt32(static_cast<int32_t>(item->ReqLevel));
		buffer.WriteInt32(static_cast<int32_t>(item->RecLevel));
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(static_cast<int32_t>(item->Classes));
		buffer.WriteInt32(static_cast<int32_t>(item->Races));
		buffer.WriteInt32(static_cast<int32_t>(item->Deity));

		const size_t pos = buffer.size();
		if (pos < LAURION_BLOB_TOTAL_SIZE) {
			const size_t pad_len = LAURION_BLOB_TOTAL_SIZE - pos;
			static uint8_t zeroes[LAURION_BLOB_TOTAL_SIZE] = {0};
			buffer.WriteBytes(reinterpret_cast<const char*>(zeroes), pad_len);
		}
	}

	// Helper Functions
	void SerializeItemDefinition(SerializeBuffer& buffer, const EQ::ItemData* item) {
		//u8 Type;
		buffer.WriteUInt8(item->ItemClass);
		//char Name[];
		buffer.WriteString(item->Name);

		//char LoreName[];
		buffer.WriteString(item->Lore);

		//we need to parse id file
		//s32 IDFile;
		int32 idfile = ExtractIDFile(item->IDFile);
		buffer.WriteUInt32(idfile);

		//s32 IDFile2;
		buffer.WriteUInt32(0); //unsupported atm

		/*
		ibs.id = item->ID;
		ibs.weight = item->Weight;
		ibs.norent = item->NoRent;
		ibs.nodrop = item->NoDrop;
		ibs.attune = item->Attuneable;
		ibs.size = item->Size;
		ibs.slots = item->Slots;
		ibs.price = item->Price;
		ibs.icon = item->Icon;
		*/

		//s32 ItemNumber;
		buffer.WriteUInt32(item->ID);

		//s32 Weight;
		buffer.WriteInt32(item->Weight);

		//bool NoRent;
		buffer.WriteUInt8(item->NoRent);

		//bool IsDroppable;
		buffer.WriteUInt8(item->NoDrop);

		//bool Attuneable;
		buffer.WriteUInt8(item->Attuneable);

		//u8 Size;
		buffer.WriteUInt8(item->Size);

		//u32 EquipSlots;
		buffer.WriteUInt32(item->Slots);

		//u32 Cost;
		buffer.WriteUInt32(item->Price);

		//s32 IconNumber;
		buffer.WriteInt32(item->Icon);

		//bool eGMRequirement;
		buffer.WriteUInt8(0); //unsupported atm

		//bool Tradeskills;
		buffer.WriteUInt8(0); //unsupported atm

		//s8 SvCold;
		//s8 SvDisease;
		//s8 SvPoison;
		//s8 SvMagic;
		//s8 SvFire;
		//s8 SvCorruption;
		buffer.WriteInt8(item->CR);
		buffer.WriteInt8(item->DR);
		buffer.WriteInt8(item->PR);
		buffer.WriteInt8(item->MR);
		buffer.WriteInt8(item->FR);
		buffer.WriteInt8(item->SVCorruption);

		//s8 STR;
		//s8 STA;
		//s8 AGI;
		//s8 DEX;
		//s8 CHA;
		//s8 INT;
		//s8 WIS;
		buffer.WriteInt8(item->AStr);
		buffer.WriteInt8(item->ASta);
		buffer.WriteInt8(item->AAgi);
		buffer.WriteInt8(item->ADex);
		buffer.WriteInt8(item->ACha);
		buffer.WriteInt8(item->AInt);
		buffer.WriteInt8(item->AWis);

		//s32 HP;
		//s32 Mana;
		//s32 Endurance;
		//s32 AC;
		buffer.WriteInt32(item->HP);
		buffer.WriteInt32(item->Mana);
		buffer.WriteInt32(item->Endur);
		buffer.WriteInt32(item->AC);

		//s32 HPRegen;
		//s32 ManaRegen;
		//s32 EnduranceRegen;
		buffer.WriteInt32(item->Regen);
		buffer.WriteInt32(item->ManaRegen);
		buffer.WriteInt32(item->EnduranceRegen);

		//u32 Classes;
		//u32 Races;
		//u32 Deity;
		buffer.WriteUInt32(item->Classes);
		buffer.WriteUInt32(item->Races);
		buffer.WriteUInt32(item->Deity);

		//u32 SkillModValue;
		//u32 SkillModMax;
		//s32 SkillModType;
		//s32 SkillModBonus;
		buffer.WriteInt32(item->SkillModValue);
		buffer.WriteUInt32(item->SkillModMax);
		buffer.WriteInt32(item->SkillModType);
		buffer.WriteInt32(0); //unsupported atm

		//s32 BaneDMGRace;
		//s32 BaneDMGBodyType;
		//s32 BaneDMGRaceValue;
		//s32 BaneDMGBodyTypeValue;
		buffer.WriteInt32(item->BaneDmgRace);
		buffer.WriteInt32(item->BaneDmgBody);
		buffer.WriteInt32(item->BaneDmgRaceAmt);
		buffer.WriteInt32(item->BaneDmgAmt);

		//bool Magic;
		buffer.WriteUInt8(item->Magic);

		//s32 FoodDuration;
		buffer.WriteInt32(item->CastTime_);

		//s32 RequiredLevel;
		buffer.WriteInt32(item->ReqLevel > 125 ? 125 : item->ReqLevel);

		//s32 RecommendedLevel;
		buffer.WriteInt32(item->RecLevel > 125 ? 125 : item->RecLevel);

		//s32 InstrumentType;
		//s32 InstrumentMod;
		buffer.WriteInt32(item->BardType);
		buffer.WriteInt32(item->BardValue);

		//u8 Light;
		buffer.WriteUInt8(item->Light);

		//u8 Delay;
		buffer.WriteUInt8(item->Delay);

		//u8 ElementalFlag;
		//u8 ElementalDamage;
		buffer.WriteUInt8(item->ElemDmgType);
		buffer.WriteUInt8(item->ElemDmgAmt);

		//u8 Range;
		buffer.WriteUInt8(item->Range);

		//u32 Damage;
		buffer.WriteUInt32(item->Damage);

		//u32 MaterialTintIndex;
		//u32 Prestige;
		buffer.WriteUInt32(item->Color);
		buffer.WriteUInt32(0); //unsupported atm

		//u8 ItemClass;
		buffer.WriteUInt8(item->ItemType);

		//ArmorProperties properties;
		//s32 Type;
		//s32 Material;
		//s32 Variation;
		//s32 NewArmorId;
		//s32 NewArmorType;
		buffer.WriteUInt32(item->Material);
		buffer.WriteUInt32(0); //unsupported atm
		buffer.WriteUInt32(item->EliteMaterial);
		buffer.WriteUInt32(item->HerosForgeModel);
		buffer.WriteUInt32(0); //unsupported atm

		//float MerchantGreedMod;
		buffer.WriteFloat(item->SellRate);

		//s32 DmgBonusSkill;
		//s32 DmgBonusValue;
		buffer.WriteInt32(item->ExtraDmgSkill);
		buffer.WriteInt32(item->ExtraDmgAmt);

		//s32 ScriptID;
		//char CharmFile[];
		buffer.WriteUInt32(item->CharmFileID);
		buffer.WriteString(item->CharmFile);

		//s32 AugType;
		//u32 AugSkinTypeMask;
		//u32 AugRestrictions;
		buffer.WriteUInt32(item->AugType);
		buffer.WriteInt32(-1); //unsupported atm
		buffer.WriteUInt32(item->AugRestrict);

		//ItemAugmentationSocket AugData[6];
		for (int j = 0; j < 6; ++j) {
			/*
			s32 Type;
			bool Visible;
			bool Infusible;
			*/

			buffer.WriteInt32(item->AugSlotType[j]);
			buffer.WriteUInt8(item->AugSlotVisible[j]);
			buffer.WriteUInt8(item->AugSlotUnk2[j]); //not entirely supported atm
		}

		//s32 LDType;
		//s32 LDTheme;
		//s32 LDCost;
		//s32 PointBuyBackPercent;
		//s32 NeedAdventureCompleted;
		buffer.WriteUInt32(item->PointType);
		buffer.WriteUInt32(item->LDoNTheme);
		buffer.WriteUInt32(item->LDoNPrice);
		buffer.WriteUInt32(item->LDoNSellBackRate);
		buffer.WriteUInt32(item->LDoNSold);

		//u8 ContainerType;
		//u8 Slots;
		//u8 SizeCapacity;
		//u8 WeightReduction;
		buffer.WriteUInt8(item->BagType);
		buffer.WriteUInt8(item->BagSlots);
		buffer.WriteUInt8(item->BagSize);
		buffer.WriteUInt8(item->BagWR);

		//u8 BookType;
		//u8 BookLang;
		//char BookFile[];
		buffer.WriteUInt8(item->Book);
		buffer.WriteUInt8(item->BookType); //doesn't match the name for eqlib
		buffer.WriteString(item->Filename);

		//s32 Lore;
		buffer.WriteInt32(item->LoreGroup);

		//bool Artifact;
		buffer.WriteUInt8(item->ArtifactFlag);

		//s32 Favor;
		buffer.WriteUInt32(item->Favor);

		//bool bIsFVNoDrop;
		buffer.WriteUInt8(item->FVNoDrop);

		//s32 Attack;
		//s32 Haste;
		buffer.WriteInt32(item->Attack);
		buffer.WriteInt32(item->Haste);

		//s32 GuildFavor;
		buffer.WriteUInt32(item->GuildFavor);

		//s32 SolventItemID;
		buffer.WriteUInt32(item->AugDistiller);

		//s32 AnimationOverride;
		buffer.WriteInt32(-1); //unsupported atm

		//u32 PaletteTintIndex;
		buffer.WriteInt32(0); //unsupported atm

		//bool bNoPetGive;
		buffer.WriteUInt8(item->NoPet);

		//bool bSomeProfile;
		buffer.WriteUInt8(0); //unsupported atm

		//u32 StackSize;
		buffer.WriteUInt32(item->ID == PARCEL_MONEY_ITEM_ID ? 0x7FFFFFFF : ((item->Stackable ? item->StackSize : 0)));

		//bool bNoStorage;
		buffer.WriteUInt8(item->NoTransfer);

		//bool Expendable;
		uint8 expendable = item->ExpendableArrow;

		if (item->ItemType == EQ::item::ItemTypeFishingPole && item->SubType == 0) {
			expendable = 1;
		}

		buffer.WriteUInt8(expendable);

		//u8 SpellDataSkillMask[78];
		for (int j = 0; j < 78; ++j) {
			buffer.WriteUInt8(0); //unsure what this is exactly
		}

		/* SpellData:
			s32 SpellId;
			u8 RequiredLevel;
			u8 EffectType;
			s32 EffectiveCasterLevel;
			s32 MaxCharges;
			s32 CastTime;
			s32 RecastTime;
			s32 RecastType;
			s32 ProcRate;
			char OverrideName[];
			s32 OverrideDesc;
		*/

		//SpellData SpellDataClicky;
		buffer.WriteInt32(item->Click.Effect);
		buffer.WriteUInt8(item->Click.Level2);
		buffer.WriteUInt8(item->Click.Type);
		buffer.WriteInt32(item->Click.Level);
		buffer.WriteInt32(item->MaxCharges);
		buffer.WriteInt32(item->CastTime);
		buffer.WriteInt32(item->RecastDelay);
		buffer.WriteInt32(item->RecastType);
		buffer.WriteInt32(0); //unsupported atm
		if (strlen(item->ClickName) > 0) {
			buffer.WriteString(item->ClickName);
		}
		else {
			buffer.WriteString("");
		}
		buffer.WriteInt32(0); //unsupported atm

		//SpellData SpellDataProc;
		buffer.WriteInt32(item->Proc.Effect);
		buffer.WriteUInt8(item->Proc.Level2);
		buffer.WriteUInt8(item->Proc.Type);
		buffer.WriteInt32(item->Proc.Level);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0); //unsupported atm; even live sets this to 0 for procs
		if (strlen(item->ProcName) > 0) {
			buffer.WriteString(item->ProcName);
		}
		else {
			buffer.WriteString("");
		}
		buffer.WriteInt32(0); //unsupported atm

		//SpellData SpellDataWorn;
		buffer.WriteInt32(item->Worn.Effect);
		buffer.WriteUInt8(item->Worn.Level2);
		buffer.WriteUInt8(item->Worn.Type);
		buffer.WriteInt32(item->Worn.Level);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		if (strlen(item->WornName) > 0) {
			buffer.WriteString(item->WornName);
		}
		else {
			buffer.WriteString("");
		}
		buffer.WriteInt32(0); //unsupported atm

		//SpellData SpellDataFocus;
		buffer.WriteInt32(item->Focus.Effect);
		buffer.WriteUInt8(item->Focus.Level2);
		buffer.WriteUInt8(item->Focus.Type);
		buffer.WriteInt32(item->Focus.Level);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		if (strlen(item->FocusName) > 0) {
			buffer.WriteString(item->FocusName);
		}
		else {
			buffer.WriteString("");
		}
		buffer.WriteInt32(0); //unsupported atm

		//SpellData SpellDataScroll;
		buffer.WriteInt32(item->Scroll.Effect);
		buffer.WriteUInt8(item->Scroll.Level2);
		buffer.WriteUInt8(item->Scroll.Type);
		buffer.WriteInt32(item->Scroll.Level);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		if (strlen(item->ScrollName) > 0) {
			buffer.WriteString(item->ScrollName);
		}
		else {
			buffer.WriteString("");
		}
		buffer.WriteInt32(0); //unsupported atm

		//SpellData SpellDataFocus2; //unsupported atm
		buffer.WriteInt32(-1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteString("");
		buffer.WriteInt32(0);

		//SpellData SpellDataBlessing; //unsupported atm
		buffer.WriteInt32(-1);
		buffer.WriteUInt8(0);
		buffer.WriteUInt8(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteInt32(0);
		buffer.WriteString("");
		buffer.WriteInt32(0);

		//s32 RightClickScriptID;
		buffer.WriteInt32(0); //unsupported atm

		//bool QuestItem;
		buffer.WriteInt8(item->QuestItemFlag);

		//s32 MaxPower;
		buffer.WriteInt32(0); //unsupported atm

		//s32 Purity;
		buffer.WriteInt32(item->Purity);

		//s32 BackstabDamage;
		buffer.WriteInt32(item->BackstabDmg);

		//s32 HeroicSTR;
		//s32 HeroicINT;
		//s32 HeroicWIS;
		//s32 HeroicAGI;
		//s32 HeroicDEX;
		//s32 HeroicSTA;
		//s32 HeroicCHA;
		buffer.WriteInt32(item->HeroicStr);
		buffer.WriteInt32(item->HeroicInt);
		buffer.WriteInt32(item->HeroicWis);
		buffer.WriteInt32(item->HeroicAgi);
		buffer.WriteInt32(item->HeroicDex);
		buffer.WriteInt32(item->HeroicSta);
		buffer.WriteInt32(item->HeroicCha);

		//s32 HealAmount;
		//s32 SpellDamage;
		//s32 Clairvoyance;
		buffer.WriteInt32(item->HealAmt);
		buffer.WriteInt32(item->SpellDmg);
		buffer.WriteInt32(item->Clairvoyance);

		//s32 SubClass;
		buffer.WriteInt32(item->SubType);

		//bool bLoginRegReqItem;
		buffer.WriteUInt8(0); //unsupported atm

		//s32 ItemLaunchScriptID;
		buffer.WriteInt32(0); //unsupported atm

		//bool Heirloom;
		buffer.WriteUInt8(0); //unsupported atm

		//s32 Placeable;
		buffer.WriteInt32(0); //unsupported atm

		//bool bPlaceableIgnoreCollisions;
		buffer.WriteUInt8(0);

		//s32 PlacementType;
		buffer.WriteInt32(0); //unsupported atm

		//s32 RealEstateDefID;
		buffer.WriteInt32(0); //unsupported atm

		//float PlaceableScaleRangeMin;
		//float PlaceableScaleRangeMax;
		buffer.WriteFloat(0.0f); //unsupported atm
		buffer.WriteFloat(0.0f); //unsupported atm

		//s32 RealEstateUpkeepID;
		buffer.WriteInt32(0); //unsupported atm

		//s32 MaxPerRealEstate;
		buffer.WriteInt32(-1); //unsupported atm

		//char HousepetFileName[];
		buffer.WriteString(""); //unsupported atm

		//bool bInteractiveObject;
		buffer.WriteUInt8(0); //unsupported atm

		//s32 TrophyBenefitID;
		buffer.WriteInt32(-1); //unsupported atm

		//bool bDisablePlacementRotation;
		//bool bDisableFreePlacement;
		buffer.WriteUInt8(0); //unsupported atm
		buffer.WriteUInt8(0); //unsupported atm

		//s32 NpcRespawnInterval;
		buffer.WriteInt32(0); //unsupported atm

		//float PlaceableDefScale;
		//float PlaceableDefHeading;
		//float PlaceableDefPitch;
		//float PlaceableDefRoll;
		buffer.WriteFloat(0.0f);
		buffer.WriteFloat(0.0f);
		buffer.WriteFloat(0.0f);
		buffer.WriteFloat(0.0f);

		//u8 SocketSubClassCount;
		//s32 SocketSubClass[SocketSubClassCount];
		buffer.WriteUInt8(0); //unsupported atm

		//bool Collectible;
		buffer.WriteUInt8(0); //unsupported atm

		//bool NoDestroy;
		buffer.WriteUInt8(0); //unsupported atm

		//bool bNoNPC;
		buffer.WriteUInt8(0); //unsupported atm

		//bool NoZone;
		buffer.WriteUInt8(0); //unsupported atm

		//s32 MakerId;
		buffer.WriteInt32(0); //unsupported atm

		//bool NoGround;
		buffer.WriteUInt8(0); //unsupported atm

		//bool bNoLoot;
		buffer.WriteUInt8(0); //unsupported atm

		//bool MarketPlace;
		buffer.WriteUInt8(0); //unsupported atm

		//bool bFreeSlot;
		buffer.WriteUInt8(0); //unsupported atm

		//bool bAutoUse;
		buffer.WriteUInt8(0); //unsupported atm

		//s32 Unknown0x0e4;
		buffer.WriteInt32(-1); //unsupported atm

		//s32 MinLuck;
		//s32 MaxLuck;
		buffer.WriteUInt32(0); //unsupported atm
		buffer.WriteUInt32(0); //unsupported atm

		//s32 LoreEquipped;
		buffer.WriteUInt32(0); //unsupported atm
	}

	void SerializeItem(SerializeBuffer& buffer, const EQ::ItemInstance* inst, int16 slot_id_in, uint8 depth, ItemPacketType packet_type, bool prepend_dh_count) {
		(void)prepend_dh_count;
		const EQ::ItemData* item = inst->GetUnscaledItem();

		// DEBUG: Hex dump for Dragon's Hoard virtual container (slots 5000-5199)
		bool is_dragon_hoard = (slot_id_in >= 5000 && slot_id_in <= 5199);
		size_t buffer_start_pos = buffer.size();

		// Only Dragon's Hoard (5000-5199) uses client-aligned 59-byte header; all other items use original layout.
		// Non-DH: original 16-char ASCII decimal string (e.g. "0000000000000058"). DH: 16-byte binary GUID (serial in low 8 bytes LE).
		auto item_guid = fmt::format("{:016}", inst->GetSerialNumber());

		structs::InventorySlot_Struct slot_id{};
		switch (packet_type) {
		case ItemPacketLoot:
			slot_id = ServerToLaurionCorpseSlot(slot_id_in);
			break;
		default:
			slot_id = ServerToLaurionSlot(slot_id_in);
			break;
		}

		auto stacksize =
			item->ID == PARCEL_MONEY_ITEM_ID ? inst->GetPrice() : (inst->IsStackable() ? ((inst->GetCharges() > 1000)
				? 0xFFFFFFFF : inst->GetCharges()) : 1);
		uint32_t slot_type_val = inst->GetMerchantSlot() ? invtype::typeMerchant : slot_id.Type;
		uint64_t price = inst->GetPrice();
		uint32_t merchant_qty = inst->GetMerchantSlot() ? inst->GetMerchantCount() : 1;
		uint32_t script_index = inst->IsScaling() ? (inst->GetExp() / 100) : 0;
		uint64_t merchant_slot = inst->GetMerchantSlot() ? inst->GetMerchantSlot() : inst->GetSerialNumber();
		uint32_t recast = inst->GetRecastTimestamp();
		auto charges = (inst->IsStackable() ? (item->MaxCharges ? 1 : 0) : ((inst->GetCharges() > 254) ? -1 : inst->GetCharges()));
		int32_t nodrop = inst->IsAttuned() ? 1 : 0;

		// [DH_CLEANUP_R1] Removed DH_GUID_INSPECT / DH_NO_COUNT / DH_BLOB_* / DH_LAYOUT_V2 / DH_RAW_BYTES investigation logs and obsolete commented blocks; kept DH_GUID_UNIQUE, [DH_POST_BLOB_ORDER], [DH_BLOB_HERE].
		if (is_dragon_hoard) {
			uint8_t guid_binary[16] = {0};
			uint32_t item_id = static_cast<uint32_t>(item->ID);
			uint64_t serial = static_cast<uint64_t>(inst->GetSerialNumber());
			for (int i = 0; i < 4; ++i) {
				guid_binary[i] = static_cast<uint8_t>((item_id >> (i * 8)) & 0xFF);
			}
			for (int i = 0; i < 8; ++i) {
				guid_binary[4 + i] = static_cast<uint8_t>((serial >> (i * 8)) & 0xFF);
			}
			// [DH_GUID_UNIQUE] Overwrite serial high dword (bytes 8..11) with a session-unique counter to avoid GUID registry cache hits.
			static uint32_t dh_session_counter = 0;
			++dh_session_counter;
			for (int i = 0; i < 4; ++i) {
				guid_binary[8 + i] = static_cast<uint8_t>((dh_session_counter >> (i * 8)) & 0xFF);
			}
			for (int i = 0; i < 3; ++i) {
				guid_binary[12 + i] = static_cast<uint8_t>((item_id >> (i * 8)) & 0xFF);
			}
			for (int i = 0; i < 15; ++i) {
				if (guid_binary[i] == 0)
					guid_binary[i] |= 0x01;
			}
			guid_binary[15] = 0x00;
			buffer.WriteBytes(guid_binary, 16);
			// --- ALL post-GUID writes before blob (commented out — replaced by sub_14029dc10 layout below) ---
			// Post-GUID: client sub_14029dc10 reads uint32, uint32, uint16, uint16, uint16, uint64, uint32, uint32, uint64 (then MerchantSlot at item+0xf0).
			// int16_t slot_field = inst->GetMerchantSlot() ? inst->GetMerchantSlot() : (slot_id.Slot + 1);
			// buffer.WriteInt16(slot_field);
			// buffer.WriteInt16(inst->GetMerchantSlot() ? (int16_t)0xffff : slot_id.SubIndex);
			// buffer.WriteInt16(inst->GetMerchantSlot() ? (int16_t)0xffff : slot_id.AugIndex);
			// buffer.WriteUInt32(stacksize);
			// buffer.WriteUInt32(slot_type_val);
			// buffer.WriteUInt64(price);
			// buffer.WriteUInt32(merchant_qty);
			// buffer.WriteUInt32(script_index);
			// buffer.WriteUInt64(merchant_slot);
			// buffer.WriteUInt32(recast);
			// buffer.WriteInt8(0);
			// buffer.WriteUInt32(0);  // rsi+0x74 Charges
			// buffer.WriteUInt32(0);  // rsi[0x10] NoDrop
			// buffer.WriteUInt8(0);   // EvolvingItem
			// buffer.WriteUInt32(6);
			// buffer.WriteUInt16(static_cast<uint16_t>(5000 + slot_id.Slot));
			// buffer.WriteUInt16(0xFFFF);
			// buffer.WriteUInt16(0xFFFF);
			// buffer.WriteUInt32(0); buffer.WriteUInt32(0); buffer.WriteUInt8(0);
			// buffer.WriteUInt32(0); buffer.WriteUInt32(0); buffer.WriteUInt32(static_cast<uint32_t>(item->Icon));
			// buffer.WriteUInt32(0); buffer.WriteUInt32(0); buffer.WriteUInt8(0); buffer.WriteUInt32(0);
			// buffer.WriteUInt32(item->ID);
			// --- NEW LAYOUT matching sub_14029dc10 read sequence ---
			uint32_t slot_1based = static_cast<uint32_t>(slot_id.Slot + 1);
			// [DH_LAYOUT_FIX2] StackCount and ContainerType at top (deserializer expects these first).
			// buffer.WriteUInt32((uint32_t)slot_1based);   // item+0x28 (slot)
			// buffer.WriteUInt32(0);                       // item+0x68 (unknown)
			buffer.WriteUInt32((uint32_t)stacksize);     // item+0x50 = StackCount
			buffer.WriteUInt32(38);                      // item+0xd0 = ContainerType (38 = DH)
			buffer.WriteUInt16((uint16_t)slot_1based);    // item+0xd4 (SubIndex slot?)
			buffer.WriteUInt16(0xFFFF);                  // item+0xd6 (SubIndex = -1)
			buffer.WriteUInt16(0xFFFF);                  // item+0xd8 (AugIndex = -1)
			buffer.WriteUInt64(0);                       // item+0xe0 (unknown)
			buffer.WriteUInt32(0);                       // item+0x08 (unknown)
			buffer.WriteUInt32(1);                       // [DH_REF_MATCH] item+0xc8 = 1 matching reference packet at 0x2b
			buffer.WriteUInt64((uint64_t)merchant_slot); // item+0xf0 = MerchantSlot/serial
			buffer.WriteUInt32(0);                       // item+0x7c (unknown)
			buffer.WriteUInt32(0);                       // item+0x5c (unknown)
			// [DH_LAYOUT_FIX2] StackCount now written at top; comment out duplicate here.
			// buffer.WriteUInt32((uint32_t)stacksize);     // item+0x50 = StackCount
			buffer.WriteUInt32(0);                       // item+0x84 (unknown)
			buffer.WriteUInt32(0);                       // item+0x8c (unknown)
			buffer.WriteUInt8(0);                       // item+0xbc = bConvertable
			// GlobalIndex written here via sub_1405647b0 equivalent (existing — keep as-is)
			buffer.WriteUInt32(6);
			buffer.WriteUInt16(static_cast<uint16_t>(5000 + slot_id.Slot));
			buffer.WriteUInt16(0xFFFF);
			buffer.WriteUInt16(0xFFFF);
			buffer.WriteUInt32((uint32_t)item->Icon);   // item+0x74 = IconNumber
			buffer.WriteUInt32(0);                       // item+0x80 (unknown)
			buffer.WriteUInt8(0);                       // evolving item gate = 0
			// post-evolving-block fields (42-byte header equivalent)
			buffer.WriteUInt32(0);                      // item+0x4c
			buffer.WriteUInt32(0);                      // item+0xcc
			buffer.WriteUInt32((uint32_t)item->Icon);   // item+0x70 = IconNumber (display)
			// buffer.WriteUInt32(0);                      // item+0x3c
			buffer.WriteUInt32(0xFFFFFFFF);              // [DH_REF_MATCH] item+0x3c = -1 matching reference packet
			buffer.WriteUInt32(0);                      // item+0xe8
			buffer.WriteUInt32(0);                      // item+0x130
			buffer.WriteUInt8(0);                       // item+0x89
			buffer.WriteUInt32(0);                      // item+0x38
			buffer.WriteUInt32((uint32_t)item->ID);     // item+0x54 = ItemNumber for registry
			{
				SerializeBuffer blob_buf;
				SerializeItemDefinitionLaurionBlob(blob_buf, item);
				// [DH_BLOB_DUMP] Temporary: first 32 bytes of DH item-definition blob hex (remove after layout verified).
				{
					const size_t k_blob_dump_len = 32;
					const unsigned char *const blob_p = blob_buf.buffer();
					const size_t blob_sz = blob_buf.size();
					const size_t dump_n = blob_sz < k_blob_dump_len ? blob_sz : k_blob_dump_len;
					std::ostringstream blob_dump_hex;
					blob_dump_hex << std::hex << std::setfill('0');
					for (size_t di = 0; di < dump_n; ++di) {
						blob_dump_hex << std::setw(2) << static_cast<unsigned>(blob_p[di]);
						if (di + 1 < dump_n)
							blob_dump_hex << ' ';
					}
					LogError("[DH_BLOB_DUMP] item_id={} blob_size={} first_32_or_less_hex=[{}]",
						item->ID, static_cast<uint32_t>(blob_sz), blob_dump_hex.str());
				}
				// [DH_BLOB_HERE]
				buffer.WriteBytes(blob_buf.buffer(), blob_buf.size());
			}
			// Post-blob: aug count → aug data → EvolvingItem → container count → container data → rsi[9].b → uint64 → uint32 (sub_1401d4ab0 tail).
			buffer.WriteUInt32(0);   // aug count = 0 (deserializer reads this, loops if nonzero) — [DH_POST_BLOB_ORDER]
			// aug data: (none when aug count == 0) [DH_POST_BLOB_ORDER]
			buffer.WriteUInt8(0);    // EvolvingItem byte (rsi[9]) — [DH_POST_BLOB_ORDER]
			buffer.WriteUInt32(0);   // container count = 0 (deserializer reads this, loops if nonzero) — [DH_POST_BLOB_ORDER]
			// container data: (none when container count == 0) [DH_POST_BLOB_ORDER]
			buffer.WriteUInt8(0);    // rsi[9].b second read — [DH_POST_BLOB_ORDER]
			buffer.WriteUInt64(0);   // sub_1401d4ab0 reads uint64 → item+0x98 — [DH_POST_BLOB_ORDER]
			buffer.WriteUInt32(0);   // deserializer reads uint32 → item+0x100 — [DH_POST_BLOB_ORDER]
			// LogInfo("[SERIALIZE_ITEM_5000] Starting serialization for slot {} (Dragon's Hoard)", (int)slot_id_in);
			// LogInfo("[SERIALIZE_ITEM_5000] ItemData: ID={}, Name={}, ItemClass={}, BagSlots={}, StackSize={}, Stackable={}",
			// 	item->ID, item->Name, item->ItemClass, item->BagSlots, item->StackSize, item->Stackable);
			// LogInfo("[SERIALIZE_ITEM_5000] ItemInstance: Charges={}, IsStackable={}, SerialNumber={}",
			// 	inst->GetCharges(), inst->IsStackable(), inst->GetSerialNumber());
		} else {
			// Original layout for all other items (pre-login, char select, bank, equipment, etc.). Match _test: GUID as null-terminated string.
			buffer.WriteString(item_guid);
			buffer.WriteUInt32(stacksize);
			buffer.WriteUInt32(slot_type_val);
			buffer.WriteInt16(inst->GetMerchantSlot() ? inst->GetMerchantSlot() : slot_id.Slot);
			buffer.WriteInt16(inst->GetMerchantSlot() ? (int16_t)0xffff : slot_id.SubIndex);
			buffer.WriteInt16(inst->GetMerchantSlot() ? (int16_t)0xffff : slot_id.AugIndex);
			buffer.WriteUInt64(price);
			buffer.WriteUInt32(merchant_qty);
			buffer.WriteUInt32(script_index);
			buffer.WriteUInt64(merchant_slot);
			buffer.WriteUInt32(recast);
			buffer.WriteInt32(charges);
			buffer.WriteInt32(nodrop);
			buffer.WriteInt32(0);   // Power
			buffer.WriteInt32(0);   // AugFlag
			buffer.WriteInt8(0);    // bConvertable
			buffer.WriteInt32(0);   // ConvertItemNameLength
			buffer.WriteInt32(0);   // ConvertItemID
			buffer.WriteInt32(0);   // Open
		}

		if (!is_dragon_hoard) {
			//bool EvolvingItem
			buffer.WriteInt8(item->EvolvingItem);

			//EvoData evoData;
			if (item->EvolvingItem > 0) {
				buffer.WriteInt32(0);
				buffer.WriteInt32(item->EvolvingLevel);
				buffer.WriteDouble(0.0);
				buffer.WriteInt32(item->EvolvingMax);
				buffer.WriteInt32(0);
			}

			uint32 ornamentation_icon = (inst->GetOrnamentationIcon() ? inst->GetOrnamentationIcon() : 0);
			uint32 hero_model = 0;
			if (inst->GetOrnamentationIDFile()) {
				hero_model = inst->GetOrnamentHeroModel(EQ::InventoryProfile::CalcMaterialFromSlot(slot_id_in));
				buffer.WriteInt32(inst->GetOrnamentationIDFile());
				buffer.WriteInt32(inst->GetOrnamentationIDFile());
			}
			else {
				buffer.WriteInt32(0);
				buffer.WriteInt32(0);
			}
			buffer.WriteInt32(ornamentation_icon);
			buffer.WriteInt32(-1);
			buffer.WriteInt32(hero_model);
			buffer.WriteInt32(0);
			buffer.WriteUInt8(0);
			buffer.WriteInt32(-1);
			buffer.WriteInt32(0);

			//ItemDefinition Item
			SerializeItemDefinition(buffer, item);
		}

		//u32 RealEstateArrayCount;
		buffer.WriteInt32(0);
		//s32 RealEstateArray[RealEstateArrayCount];

		//bool bRealEstateItemPlaceable;
		buffer.WriteInt8(0);

		//u32 SubContentSize;
		uint32 subitem_count = 0;

		int16 SubSlotNumber = EQ::invbag::SLOT_INVALID;

		if (slot_id_in <= EQ::invslot::GENERAL_END && slot_id_in >= EQ::invslot::GENERAL_BEGIN)
			SubSlotNumber = EQ::invbag::GENERAL_BAGS_BEGIN + ((slot_id_in - EQ::invslot::GENERAL_BEGIN) * EQ::invbag::SLOT_COUNT);
		else if (slot_id_in == EQ::invslot::slotCursor)
			SubSlotNumber = EQ::invbag::CURSOR_BAG_BEGIN;
		else if (slot_id_in >= 5000 && slot_id_in <= 5199)
			SubSlotNumber = slot_id_in; // Dragon's Hoard: 5000-5199
		else if (slot_id_in <= EQ::invslot::BANK_END && slot_id_in >= EQ::invslot::BANK_BEGIN)
			SubSlotNumber = EQ::invbag::BANK_BAGS_BEGIN + ((slot_id_in - EQ::invslot::BANK_BEGIN) * EQ::invbag::SLOT_COUNT);
		else if (slot_id_in <= EQ::invslot::SHARED_BANK_END && slot_id_in >= EQ::invslot::SHARED_BANK_BEGIN)
			SubSlotNumber = EQ::invbag::SHARED_BANK_BAGS_BEGIN + ((slot_id_in - EQ::invslot::SHARED_BANK_BEGIN) * EQ::invbag::SLOT_COUNT);
		else
			SubSlotNumber = slot_id_in; // not sure if this is the best way to handle this..leaving for now

		if (SubSlotNumber != EQ::invbag::SLOT_INVALID) {
			for (uint32 index = EQ::invbag::SLOT_BEGIN; index <= EQ::invbag::SLOT_END; ++index) {
				EQ::ItemInstance* sub = inst->GetItem(index);
				if (!sub)
					continue;

				++subitem_count;
			}

			buffer.WriteUInt32(subitem_count);

			for (uint32 index = EQ::invbag::SLOT_BEGIN; index <= EQ::invbag::SLOT_END; ++index) {
				EQ::ItemInstance* sub = inst->GetItem(index);
				if (!sub)
					continue;

				buffer.WriteUInt32(index);

				SerializeItem(buffer, sub, SubSlotNumber, (depth + 1), packet_type);
			}
		}

		//bool bCollected;
		buffer.WriteInt8(0); //unsupported atm
		//u64 DontKnow;
		buffer.WriteUInt64(0); //unsupported atm
		//s32 Luck;
		buffer.WriteInt32(0); //unsupported atm
		
		// DEBUG: Hex dump for Dragon's Hoard — layout matches client sub_14029dc10 (first 59 bytes)
		if (is_dragon_hoard) {
			size_t serialized_size = buffer.size() - buffer_start_pos;
			// LogInfo("[SERIALIZE_ITEM_5000] Serialization complete. Total bytes: {}", serialized_size);

			const unsigned char* buf = buffer.buffer() + buffer_start_pos;

			// LogInfo("[SERIALIZE_ITEM_5000] ItemGUID = 16 bytes binary (serial {} in low 8 bytes LE)", inst->GetSerialNumber());

			// Hex dump first 128 bytes
			// size_t dump_size = std::min(serialized_size, size_t(128));
			// std::ostringstream hex_stream;
			// hex_stream << std::hex << std::setfill('0');
			// for (size_t i = 0; i < dump_size; i++) {
			// 	if (i > 0 && i % 16 == 0) hex_stream << "\n";
			// 	if (i % 16 == 0) hex_stream << std::setw(4) << i << ": ";
			// 	hex_stream << std::setw(2) << (int)buf[i] << " ";
			// }
			// LogInfo("[SERIALIZE_ITEM_5000] First {} bytes (hex):\n{}", dump_size, hex_stream.str());

			// Key fields — 4-byte count then 59-byte header at 0x04: guid(16) | slot(2) sub(2) aug(2) | stack, type, price, ...
			if (serialized_size >= 0x3F) {
				// LogInfo("[SERIALIZE_ITEM_5000] Key fields (count at 0x00, 59-byte header at 0x04):");
				uint32_t count = *(uint32_t*)(buf + 0x00);
				// LogInfo("  0x00-0x03 (4):   count           = {} (0x{:X})", count, count);
				// LogInfo("  0x04-0x13 (16): guid_binary     = {:02x} {:02x} .. {:02x} {:02x} (byte15=null)", buf[0x04], buf[0x05], buf[0x12], buf[0x13]);
				int16_t slot = *(int16_t*)(buf + 0x14);
				int16_t sub = *(int16_t*)(buf + 0x16);
				int16_t aug = *(int16_t*)(buf + 0x18);
				// LogInfo("  0x14-0x19 (6):   Slot,Sub,Aug   = {}, {}, {}", slot, sub, aug);
				uint32_t stack_count = *(uint32_t*)(buf + 0x1A);
				// LogInfo("  0x1A-0x1D (4):   StackCount     = {} ({:#x})", stack_count, stack_count);
				uint32_t slot_type = *(uint32_t*)(buf + 0x1E);
				// LogInfo("  0x1E-0x21 (4):   Type           = {} (0x{:02X} = typeDragonHoard)", slot_type, slot_type);
				uint64_t price = *(uint64_t*)(buf + 0x22);
				// LogInfo("  0x22-0x29 (8):   Price          = {} ({:#018x})", price, price);
				uint32_t mqty = *(uint32_t*)(buf + 0x2A);
				uint32_t script = *(uint32_t*)(buf + 0x2E);
				// LogInfo("  0x2A-0x2D (4):   MerchantQty    = {}", mqty);
				// LogInfo("  0x2E-0x31 (4):   ScriptIndex    = {}", script);
				uint64_t merch_slot = *(uint64_t*)(buf + 0x32);
				// LogInfo("  0x32-0x39 (8):   MerchantSlot   = {} ({:#018x})", merch_slot, merch_slot);
				uint32_t recast = *(uint32_t*)(buf + 0x3A);
				uint8_t convert = buf[0x3E];
				// LogInfo("  0x3A-0x3D (4):   Recast         = {}", recast);
				// LogInfo("  0x3E     (1):   bConvertable   = {}", (int)convert);
				if (serialized_size >= 0x43) {
					int32_t charges = *(int32_t*)(buf + 0x3F);
					int32_t nodrop = *(int32_t*)(buf + 0x43);
					// LogInfo("  0x3F-0x42 (4):   Charges        = {}", charges);
					// LogInfo("  0x43-0x46 (4):   NoDropFlag     = {}", nodrop);
				}
			}
			// Raw bytes 0x3F..0x4B: 9 trailing bytes then 4-byte blob L. L=1469 at 0x48.
			if (serialized_size >= 0x4C) {
				std::ostringstream hex;
				hex << std::hex << std::setfill('0');
				for (int i = 0x3F; i <= 0x4B; i++)
					hex << std::setw(2) << (int)buf[i] << " ";
				// LogInfo("[SERIALIZE_ITEM_5000] 0x3F-0x4B (9 trail + L): {}", hex.str());
				uint32_t blob_len_at_0x48 = *(uint32_t*)(buf + 0x48);
				// LogInfo("[SERIALIZE_ITEM_5000] Blob L at 0x48 = {} (0x{:X})", blob_len_at_0x48, blob_len_at_0x48);
			}
			// Blob starts at packet offset 0x4C. First byte = arg1[0xe9] (ItemClass).
			const size_t blob_start = 0x4C;
			if (serialized_size >= blob_start + 16) {
				uint8_t blob_first = buf[blob_start];
				// LogInfo("[SERIALIZE_ITEM_5000] Blob byte 0 (arg1[0xe9] ItemClass) = {} (0x{:02X})", blob_first, blob_first);
				std::ostringstream blob_hex;
				blob_hex << std::hex << std::setfill('0');
				for (size_t i = 0; i < 16; i++)
					blob_hex << std::setw(2) << (int)buf[blob_start + i] << " ";
				// LogInfo("[SERIALIZE_ITEM_5000] Blob bytes 0x00-0x0F: {}", blob_hex.str());
			}
			if (serialized_size >= blob_start + 0x98) {
				std::ostringstream hex90;
				hex90 << std::hex << std::setfill('0');
				for (int i = 0x90; i <= 0x9B; i++)
					hex90 << std::setw(2) << (int)buf[blob_start + i] << " ";
				uint32_t blob_itemid = *(uint32_t*)(buf + blob_start + 0x91);
				int32_t blob_weight = *(int32_t*)(buf + blob_start + 0x95);
				// LogInfo("[SERIALIZE_ITEM_5000] Blob 0x90-0x9B (arg1+0xb0/b4): {}", hex90.str());
				// LogInfo("[SERIALIZE_ITEM_5000] Blob ItemID at 0x91 = {} (0x{:X})  Weight at 0x95 = {}", blob_itemid, blob_itemid, blob_weight);
			}
		}
		// [DH_LOG_CLEANUP_2] [REFERENCE_ITEM_RAW] one-time non-DH comparison dump disabled.
		// One-time dump of a working non-DH packet for comparison with DH
		// if (!is_dragon_hoard) {
		// 	static bool logged = false;
		// 	if (!logged) {
		// 		logged = true;
		// 		size_t ref_dump = std::min(buffer.size(), size_t(160));
		// 		const unsigned char* ref_buf = reinterpret_cast<const unsigned char*>(buffer.buffer());
		// 		std::ostringstream ref_hex;
		// 		ref_hex << std::hex << std::setfill('0');
		// 		for (size_t i = 0; i < ref_dump; i++) {
		// 			if (i > 0 && i % 16 == 0) ref_hex << "\n";
		// 			if (i % 16 == 0) ref_hex << std::setw(4) << i << ": ";
		// 			ref_hex << std::setw(2) << (int)ref_buf[i] << " ";
		// 		}
		// 		LogInfo("[REFERENCE_ITEM_RAW] item_id={} ItemClass={} first {} bytes (hex):\n{}", item->ID, (unsigned)item->ItemClass, ref_dump, ref_hex.str());
		// 	}
		// }
	}

	static inline void ServerToLaurionConvertLinks(std::string& message_out, const std::string& message_in)
	{
		if (message_in.find('\x12') == std::string::npos) {
			message_out = message_in;
			return;
		}

	auto segments = Strings::Split(message_in, '\x12');
	for (size_t segment_iter = 0; segment_iter < segments.size(); ++segment_iter) {
		if (segment_iter & 1) {
		// Safety check: ensure segment is not empty and has valid content
		if (segments[segment_iter].empty()) {
			message_out += segments[segment_iter];
			continue;
		}
		
		// Check if first character is a digit (valid link tag)
		// If not, this is probably plain text that happened to be between \x12 markers
		if (!std::isdigit(static_cast<unsigned char>(segments[segment_iter][0]))) {
			// Not a link tag, just append as-is
			message_out += segments[segment_iter];
			continue;
		}
		
		int etag = 0;
		try {
			etag = std::stoi(segments[segment_iter].substr(0, 1));
		} catch (const std::exception& e) {
			// Failed to parse, append as-is
			message_out += segments[segment_iter];
			continue;
		}

				switch (etag) {
				case 0:
				{
					size_t index = 1;
					auto item_id = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug1 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug2 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug3 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug4 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug5 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto aug6 = segments[segment_iter].substr(index, 5);
					index += 5;

					auto is_evolving = segments[segment_iter].substr(index, 1);
					index += 1;

					auto evolutionGroup = segments[segment_iter].substr(index, 4);
					index += 4;

					auto evolutionLevel = segments[segment_iter].substr(index, 2);
					index += 2;

					auto ornamentationIconID = segments[segment_iter].substr(index, 5);
					index += 5;

					auto itemHash = segments[segment_iter].substr(index, 8);
					index += 8;

					auto text = segments[segment_iter].substr(index);

					message_out.push_back('\x12');
					message_out.push_back('0'); //etag item
					message_out.append(item_id);
					message_out.append(aug1);
					message_out.append("00000");
					message_out.append(aug2);
					message_out.append("00000");
					message_out.append(aug3);
					message_out.append("00000");
					message_out.append(aug4);
					message_out.append("00000");
					message_out.append(aug5);
					message_out.append("00000");
					message_out.append(aug6);
					message_out.append("00000");
					message_out.append(is_evolving);
					message_out.append(evolutionGroup);
					message_out.append(evolutionLevel);
					message_out.append(ornamentationIconID);
					message_out.append("00000");
					message_out.append(itemHash);
					message_out.append(text);
					message_out.push_back('\x12');

					break;
				}
				default:
					//unsupported etag right now; just pass it as is
					message_out.append(segments[segment_iter]);
					break;
				}
			}
			else {
				message_out.append(segments[segment_iter]);
			}
		}
	}

	static inline void LaurionToServerConvertLinks(std::string& message_out, const std::string& message_in) {
		message_out = message_in;
	}

	static inline uint32 ServerToLaurionSpawnAppearanceType(uint32 server_type) {
		switch (server_type)
		{
		case AppearanceType::WhoLevel:
			return structs::LaurionAppearance::WhoLevel;
		case AppearanceType::MaxHealth:
			return structs::LaurionAppearance::MaxHealth;
		case AppearanceType::Invisibility:
			return structs::LaurionAppearance::Invisibility;
		case AppearanceType::PVP:
			return structs::LaurionAppearance::PVP;
		case AppearanceType::Light:
			return structs::LaurionAppearance::Light;
		case AppearanceType::Animation:
			return structs::LaurionAppearance::Animation;
		case AppearanceType::Sneak:
			return structs::LaurionAppearance::Sneak;
		case AppearanceType::SpawnID:
			return structs::LaurionAppearance::SpawnID;
		case AppearanceType::Health:
			return structs::LaurionAppearance::Health;
		case AppearanceType::Linkdead:
			return structs::LaurionAppearance::Linkdead;
		case AppearanceType::FlyMode:
			return structs::LaurionAppearance::FlyMode;
		case AppearanceType::GM:
			return structs::LaurionAppearance::GM;
		case AppearanceType::Anonymous:
			return structs::LaurionAppearance::Anonymous;
		case AppearanceType::GuildID:
			return structs::LaurionAppearance::GuildID;
		case AppearanceType::AFK:
			return structs::LaurionAppearance::AFK;
		case AppearanceType::Pet:
			return structs::LaurionAppearance::Pet;
		case AppearanceType::Summoned:
			return structs::LaurionAppearance::Summoned;
		case AppearanceType::SetType:
			return structs::LaurionAppearance::NPCName;
		case AppearanceType::CancelSneakHide:
			return structs::LaurionAppearance::CancelSneakHide;
		case AppearanceType::AreaHealthRegen:
			return structs::LaurionAppearance::AreaHealthRegen;
		case AppearanceType::AreaManaRegen:
			return structs::LaurionAppearance::AreaManaRegen;
		case AppearanceType::AreaEnduranceRegen:
			return structs::LaurionAppearance::AreaEnduranceRegen;
		case AppearanceType::FreezeBeneficialBuffs:
			return structs::LaurionAppearance::FreezeBeneficialBuffs;
		case AppearanceType::NPCTintIndex:
			return structs::LaurionAppearance::NPCTintIndex;
		case AppearanceType::ShowHelm:
			return structs::LaurionAppearance::ShowHelm;
		case AppearanceType::DamageState:
			return structs::LaurionAppearance::DamageState;
		case AppearanceType::TextureType:
			return structs::LaurionAppearance::TextureType;
		case AppearanceType::GuildShow:
			return structs::LaurionAppearance::GuildShow;
		case AppearanceType::OfflineMode:
			return structs::LaurionAppearance::OfflineMode;
		default:
			return structs::LaurionAppearance::None;
		}
	}

	static inline uint32 LaurionToServerSpawnAppearanceType(uint32 laurion_type) {
		switch (laurion_type)
		{
		case structs::LaurionAppearance::WhoLevel:
			return AppearanceType::WhoLevel;
		case structs::LaurionAppearance::MaxHealth:
			return AppearanceType::MaxHealth;
		case structs::LaurionAppearance::Invisibility:
			return AppearanceType::Invisibility;
		case structs::LaurionAppearance::PVP:
			return AppearanceType::PVP;
		case structs::LaurionAppearance::Light:
			return AppearanceType::Light;
		case structs::LaurionAppearance::Animation:
			return AppearanceType::Animation;
		case structs::LaurionAppearance::Sneak:
			return AppearanceType::Sneak;
		case structs::LaurionAppearance::SpawnID:
			return AppearanceType::SpawnID;
		case structs::LaurionAppearance::Health:
			return AppearanceType::Health;
		case structs::LaurionAppearance::Linkdead:
			return AppearanceType::Linkdead;
		case structs::LaurionAppearance::FlyMode:
			return AppearanceType::FlyMode;
		case structs::LaurionAppearance::GM:
			return AppearanceType::GM;
		case structs::LaurionAppearance::Anonymous:
			return AppearanceType::Anonymous;
		case structs::LaurionAppearance::GuildID:
			return AppearanceType::GuildID;
		case structs::LaurionAppearance::AFK:
			return AppearanceType::AFK;
		case structs::LaurionAppearance::Pet:
			return AppearanceType::Pet;
		case structs::LaurionAppearance::Summoned:
			return AppearanceType::Summoned;
		case structs::LaurionAppearance::SetType:
			return AppearanceType::NPCName;
		case structs::LaurionAppearance::CancelSneakHide:
			return AppearanceType::CancelSneakHide;
		case structs::LaurionAppearance::AreaHealthRegen:
			return AppearanceType::AreaHealthRegen;
		case structs::LaurionAppearance::AreaManaRegen:
			return AppearanceType::AreaManaRegen;
		case structs::LaurionAppearance::AreaEnduranceRegen:
			return AppearanceType::AreaEnduranceRegen;
		case structs::LaurionAppearance::FreezeBeneficialBuffs:
			return AppearanceType::FreezeBeneficialBuffs;
		case structs::LaurionAppearance::NPCTintIndex:
			return AppearanceType::NPCTintIndex;
		case structs::LaurionAppearance::ShowHelm:
			return AppearanceType::ShowHelm;
		case structs::LaurionAppearance::DamageState:
			return AppearanceType::DamageState;
		case structs::LaurionAppearance::TextureType:
			return AppearanceType::TextureType;
		case structs::LaurionAppearance::GuildShow:
			return AppearanceType::GuildShow;
		case structs::LaurionAppearance::OfflineMode:
			return AppearanceType::OfflineMode;
		default:
			return AppearanceType::Die;
		}
	}

	static inline structs::InventorySlot_Struct ServerToLaurionSlot(uint32 server_slot)
	{
		structs::InventorySlot_Struct LaurionSlot;
		LaurionSlot.Type = invtype::TYPE_INVALID;
		LaurionSlot.Slot = invslot::SLOT_INVALID;
		LaurionSlot.SubIndex = invbag::SLOT_INVALID;
		LaurionSlot.AugIndex = invaug::SOCKET_INVALID;

		uint32 TempSlot = EQ::invslot::SLOT_INVALID;

		if (server_slot < EQ::invtype::POSSESSIONS_SIZE) {
			LaurionSlot.Type = invtype::typePossessions;

			if (server_slot == EQ::invslot::slotCursor) {
				LaurionSlot.Slot = invslot::slotCursor;
			}
			else
			{
				LaurionSlot.Slot = server_slot;
			}
		}

		else if (server_slot <= EQ::invbag::CURSOR_BAG_END && server_slot >= EQ::invbag::GENERAL_BAGS_BEGIN) {
			TempSlot = server_slot - EQ::invbag::GENERAL_BAGS_BEGIN;

			LaurionSlot.Type = invtype::typePossessions;
			LaurionSlot.Slot = invslot::GENERAL_BEGIN + (TempSlot / EQ::invbag::SLOT_COUNT);
			LaurionSlot.SubIndex = TempSlot - ((LaurionSlot.Slot - invslot::GENERAL_BEGIN) * EQ::invbag::SLOT_COUNT);
		}

		else if (server_slot <= EQ::invslot::TRIBUTE_END && server_slot >= EQ::invslot::TRIBUTE_BEGIN) {
			LaurionSlot.Type = invtype::typeTribute;
			LaurionSlot.Slot = server_slot - EQ::invslot::TRIBUTE_BEGIN;
		}

		else if (server_slot <= EQ::invslot::GUILD_TRIBUTE_END && server_slot >= EQ::invslot::GUILD_TRIBUTE_BEGIN) {
			LaurionSlot.Type = invtype::typeGuildTribute;
			LaurionSlot.Slot = server_slot - EQ::invslot::GUILD_TRIBUTE_BEGIN;
		}

		else if (server_slot == EQ::invslot::SLOT_TRADESKILL_EXPERIMENT_COMBINE) {
			LaurionSlot.Type = invtype::typeWorld;
		}

		else if (server_slot >= 5000 && server_slot <= 5199) {
			LaurionSlot.Type = invtype::typeDragonHoard;
			LaurionSlot.Slot = server_slot - 5000;
		}

		else if (server_slot <= EQ::invslot::BANK_END && server_slot >= EQ::invslot::BANK_BEGIN) {
			LaurionSlot.Type = invtype::typeBank;
			LaurionSlot.Slot = server_slot - EQ::invslot::BANK_BEGIN;
		}

		else if (server_slot <= EQ::invbag::BANK_BAGS_END && server_slot >= EQ::invbag::BANK_BAGS_BEGIN) {
			TempSlot = server_slot - EQ::invbag::BANK_BAGS_BEGIN;

			LaurionSlot.Type = invtype::typeBank;
			LaurionSlot.Slot = TempSlot / EQ::invbag::SLOT_COUNT;
			LaurionSlot.SubIndex = TempSlot - (LaurionSlot.Slot * EQ::invbag::SLOT_COUNT);
		}

		else if (server_slot <= EQ::invslot::SHARED_BANK_END && server_slot >= EQ::invslot::SHARED_BANK_BEGIN) {
			LaurionSlot.Type = invtype::typeSharedBank;
			LaurionSlot.Slot = server_slot - EQ::invslot::SHARED_BANK_BEGIN;
		}

		else if (server_slot <= EQ::invbag::SHARED_BANK_BAGS_END && server_slot >= EQ::invbag::SHARED_BANK_BAGS_BEGIN) {
			TempSlot = server_slot - EQ::invbag::SHARED_BANK_BAGS_BEGIN;

			LaurionSlot.Type = invtype::typeSharedBank;
			LaurionSlot.Slot = TempSlot / EQ::invbag::SLOT_COUNT;
			LaurionSlot.SubIndex = TempSlot - (LaurionSlot.Slot * EQ::invbag::SLOT_COUNT);
		}

		else if (server_slot <= EQ::invslot::TRADE_END && server_slot >= EQ::invslot::TRADE_BEGIN) {
			LaurionSlot.Type = invtype::typeTrade;
			LaurionSlot.Slot = server_slot - EQ::invslot::TRADE_BEGIN;
		}

		else if (server_slot <= EQ::invbag::TRADE_BAGS_END && server_slot >= EQ::invbag::TRADE_BAGS_BEGIN) {
			TempSlot = server_slot - EQ::invbag::TRADE_BAGS_BEGIN;

			LaurionSlot.Type = invtype::typeTrade;
			LaurionSlot.Slot = TempSlot / EQ::invbag::SLOT_COUNT;
			LaurionSlot.SubIndex = TempSlot - (LaurionSlot.Slot * EQ::invbag::SLOT_COUNT);
		}

		else if (server_slot <= EQ::invslot::WORLD_END && server_slot >= EQ::invslot::WORLD_BEGIN) {
			LaurionSlot.Type = invtype::typeWorld;
			LaurionSlot.Slot = server_slot - EQ::invslot::WORLD_BEGIN;
		}

		Log(Logs::Detail, Logs::Netcode, "Convert Server Slot %i to Laurion Slot [%i, %i, %i, %i]",
			server_slot, LaurionSlot.Type, LaurionSlot.Slot, LaurionSlot.SubIndex, LaurionSlot.AugIndex);

		return LaurionSlot;
	}

	static inline structs::InventorySlot_Struct ServerToLaurionCorpseSlot(uint32 server_corpse_slot)
	{
		structs::InventorySlot_Struct LaurionSlot;
		LaurionSlot.Type = invtype::TYPE_INVALID;
		LaurionSlot.Slot = ServerToLaurionCorpseMainSlot(server_corpse_slot);
		LaurionSlot.SubIndex = invbag::SLOT_INVALID;
		LaurionSlot.AugIndex = invaug::SOCKET_INVALID;

		if (LaurionSlot.Slot != invslot::SLOT_INVALID)
			LaurionSlot.Type = invtype::typeCorpse;

		Log(Logs::Detail, Logs::Netcode, "Convert Server Corpse Slot %i to Laurion Corpse Slot [%i, %i, %i, %i]",
			server_corpse_slot, LaurionSlot.Type, LaurionSlot.Slot, LaurionSlot.SubIndex, LaurionSlot.AugIndex);

		return LaurionSlot;
	}

	static inline uint32 ServerToLaurionCorpseMainSlot(uint32 server_corpse_slot)
	{
		uint32 LaurionSlot = invslot::SLOT_INVALID;

		if (server_corpse_slot <= EQ::invslot::CORPSE_END && server_corpse_slot >= EQ::invslot::CORPSE_BEGIN) {
			LaurionSlot = server_corpse_slot;
		}

		LogNetcode("Convert Server Corpse Slot [{}] to Laurion Corpse Main Slot [{}]", server_corpse_slot, LaurionSlot);

		return LaurionSlot;
	}

	static inline structs::TypelessInventorySlot_Struct ServerToLaurionTypelessSlot(uint32 server_slot, int16 server_type)
	{
		structs::TypelessInventorySlot_Struct LaurionSlot;
		LaurionSlot.Slot = invslot::SLOT_INVALID;
		LaurionSlot.SubIndex = invbag::SLOT_INVALID;
		LaurionSlot.AugIndex = invaug::SOCKET_INVALID;

		uint32 TempSlot = EQ::invslot::SLOT_INVALID;

		if (server_type == EQ::invtype::typePossessions) {
			if (server_slot < EQ::invtype::POSSESSIONS_SIZE) {
				LaurionSlot.Slot = server_slot;
			}

			else if (server_slot <= EQ::invbag::CURSOR_BAG_END && server_slot >= EQ::invbag::GENERAL_BAGS_BEGIN) {
				TempSlot = server_slot - EQ::invbag::GENERAL_BAGS_BEGIN;

				LaurionSlot.Slot = invslot::GENERAL_BEGIN + (TempSlot / EQ::invbag::SLOT_COUNT);
				LaurionSlot.SubIndex = TempSlot - ((LaurionSlot.Slot - invslot::GENERAL_BEGIN) * EQ::invbag::SLOT_COUNT);
			}
		}

		Log(Logs::Detail, Logs::Netcode, "Convert Server Slot %i to Laurion Typeless Slot [%i, %i, %i] (implied type: %i)",
			server_slot, LaurionSlot.Slot, LaurionSlot.SubIndex, LaurionSlot.AugIndex, server_type);

		return LaurionSlot;
	}

	static inline uint32 LaurionToServerSlot(structs::InventorySlot_Struct laurion_slot)
	{
		if (laurion_slot.AugIndex < invaug::SOCKET_INVALID || laurion_slot.AugIndex >= invaug::SOCKET_COUNT) {
			Log(Logs::Detail, Logs::Netcode, "Convert Laurion Slot [%i, %i, %i, %i] to Server Slot %i",
				laurion_slot.Type, laurion_slot.Slot, laurion_slot.SubIndex, laurion_slot.AugIndex, EQ::invslot::SLOT_INVALID);

			return EQ::invslot::SLOT_INVALID;
		}

		uint32 server_slot = EQ::invslot::SLOT_INVALID;
		uint32 temp_slot = invslot::SLOT_INVALID;

		switch (laurion_slot.Type) {
		case invtype::typePossessions: {
			// Client uses Slot=35 for cursor; server uses slotCursor. Map 35 -> server cursor.
			if (laurion_slot.SubIndex == invbag::SLOT_INVALID && laurion_slot.Slot == 35) {
				server_slot = EQ::invslot::slotCursor;
				break;
			}
			// No deposit button in DH UI — deposit is drag-only. Client uses Type=0 Slot 36..235 for DH list (200 slots after cursor).
			if (laurion_slot.SubIndex == invbag::SLOT_INVALID && laurion_slot.Slot >= 36 && laurion_slot.Slot < 236) {
				server_slot = invslot::DRAGON_HOARD_BEGIN + (laurion_slot.Slot - 36);
				break;
			}
			if (laurion_slot.Slot >= invslot::POSSESSIONS_BEGIN && laurion_slot.Slot <= invslot::POSSESSIONS_END) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					if (laurion_slot.Slot == invslot::slotCursor) {
						server_slot = EQ::invslot::slotCursor;
					}
					else if(laurion_slot.Slot == invslot::slotGeneral11 || laurion_slot.Slot == invslot::slotGeneral12)
					{
						return EQ::invslot::SLOT_INVALID;
					}
					else {
						server_slot = laurion_slot.Slot;
					}
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					if (laurion_slot.Slot < invslot::GENERAL_BEGIN)
						return EQ::invslot::SLOT_INVALID;

					temp_slot = (laurion_slot.Slot - invslot::GENERAL_BEGIN) * invbag::SLOT_COUNT;
					server_slot = EQ::invbag::GENERAL_BAGS_BEGIN + temp_slot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeBank: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::BANK_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					server_slot = EQ::invslot::BANK_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					temp_slot = laurion_slot.Slot * invbag::SLOT_COUNT;
					server_slot = EQ::invbag::BANK_BAGS_BEGIN + temp_slot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeSharedBank: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::SHARED_BANK_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					server_slot = EQ::invslot::SHARED_BANK_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					temp_slot = laurion_slot.Slot * invbag::SLOT_COUNT;
					server_slot = EQ::invbag::SHARED_BANK_BAGS_BEGIN + temp_slot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeTrade: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::TRADE_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					server_slot = EQ::invslot::TRADE_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					temp_slot = laurion_slot.Slot * invbag::SLOT_COUNT;
					server_slot = EQ::invbag::TRADE_BAGS_BEGIN + temp_slot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeWorld: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::WORLD_SIZE) {
				server_slot = EQ::invslot::WORLD_BEGIN + laurion_slot.Slot;
			}

			else if (laurion_slot.Slot == invslot::SLOT_INVALID) {
				server_slot = EQ::invslot::SLOT_TRADESKILL_EXPERIMENT_COMBINE;
			}

			break;
		}
		case invtype::typeLimbo: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::LIMBO_SIZE) {
				server_slot = EQ::invslot::slotCursor;
			}

			break;
		}
		case invtype::typeTribute: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::TRIBUTE_SIZE) {
				server_slot = EQ::invslot::TRIBUTE_BEGIN + laurion_slot.Slot;
			}

			break;
		}
		case invtype::typeGuildTribute: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::GUILD_TRIBUTE_SIZE) {
				server_slot = EQ::invslot::GUILD_TRIBUTE_BEGIN + laurion_slot.Slot;
			}

			break;
		}
		case invtype::typeCorpse: {
			if (laurion_slot.Slot >= invslot::CORPSE_BEGIN && laurion_slot.Slot <= invslot::CORPSE_END) {
				server_slot = laurion_slot.Slot;
			}

			break;
		}
		case invtype::typeDragonHoard: {
			// 200 slots: map client slot 0..199 to server 5000..5199
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < 200 && laurion_slot.SubIndex == invbag::SLOT_INVALID) {
				server_slot = invslot::DRAGON_HOARD_BEGIN + laurion_slot.Slot;
			}
			break;
		}
		default: {

			break;
		}
		}

		Log(Logs::Detail, Logs::Netcode, "Convert Laurion Slot [%i, %i, %i, %i] to Server Slot %i",
			laurion_slot.Type, laurion_slot.Slot, laurion_slot.SubIndex, laurion_slot.AugIndex, server_slot);

		return server_slot;
	}

	static inline uint32 LaurionToServerCorpseSlot(structs::InventorySlot_Struct laurion_corpse_slot)
	{
		uint32 ServerSlot = EQ::invslot::SLOT_INVALID;

		if (laurion_corpse_slot.Type != invtype::typeCorpse || laurion_corpse_slot.SubIndex != invbag::SLOT_INVALID || laurion_corpse_slot.AugIndex != invaug::SOCKET_INVALID) {
			ServerSlot = EQ::invslot::SLOT_INVALID;
		}

		else {
			ServerSlot = LaurionToServerCorpseMainSlot(laurion_corpse_slot.Slot);
		}

		Log(Logs::Detail, Logs::Netcode, "Convert Laurion Slot [%i, %i, %i, %i] to Server Slot %i",
			laurion_corpse_slot.Type, laurion_corpse_slot.Slot, laurion_corpse_slot.SubIndex, laurion_corpse_slot.AugIndex, ServerSlot);

		return ServerSlot;
	}

	static inline uint32 LaurionToServerCorpseMainSlot(uint32 laurion_corpse_slot)
	{
		uint32 ServerSlot = EQ::invslot::SLOT_INVALID;

		if (laurion_corpse_slot <= invslot::CORPSE_END && laurion_corpse_slot >= invslot::CORPSE_BEGIN) {
			ServerSlot = laurion_corpse_slot;
		}

		LogNetcode("Convert Laurion Corpse Main Slot [{}] to Server Corpse Slot [{}]", laurion_corpse_slot, ServerSlot);

		return ServerSlot;
	}

	static inline uint32 LaurionToServerTypelessSlot(structs::TypelessInventorySlot_Struct laurion_slot, int16 laurion_type)
	{
		if (laurion_slot.AugIndex < invaug::SOCKET_INVALID || laurion_slot.AugIndex >= invaug::SOCKET_COUNT) {
			Log(Logs::Detail, Logs::Netcode, "Convert Laurion Typeless Slot [%i, %i, %i] (implied type: %i) to Server Slot %i",
				laurion_slot.Slot, laurion_slot.SubIndex, laurion_slot.AugIndex, laurion_type, EQ::invslot::SLOT_INVALID);

			return EQ::invslot::SLOT_INVALID;
		}

		uint32 ServerSlot = EQ::invslot::SLOT_INVALID;
		uint32 TempSlot = invslot::SLOT_INVALID;

		switch (laurion_type) {
		case invtype::typePossessions: {
			if (laurion_slot.Slot >= invslot::POSSESSIONS_BEGIN && laurion_slot.Slot <= invslot::POSSESSIONS_END) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					ServerSlot = laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					if (laurion_slot.Slot < invslot::GENERAL_BEGIN)
						return EQ::invslot::SLOT_INVALID;

					TempSlot = (laurion_slot.Slot - invslot::GENERAL_BEGIN) * invbag::SLOT_COUNT;
					ServerSlot = EQ::invbag::GENERAL_BAGS_BEGIN + TempSlot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeBank: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::BANK_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					ServerSlot = EQ::invslot::BANK_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					TempSlot = laurion_slot.Slot * invbag::SLOT_COUNT;
					ServerSlot = EQ::invbag::BANK_BAGS_BEGIN + TempSlot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeSharedBank: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::SHARED_BANK_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					ServerSlot = EQ::invslot::SHARED_BANK_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					TempSlot = laurion_slot.Slot * invbag::SLOT_COUNT;
					ServerSlot = EQ::invbag::SHARED_BANK_BAGS_BEGIN + TempSlot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeTrade: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::TRADE_SIZE) {
				if (laurion_slot.SubIndex == invbag::SLOT_INVALID) {
					ServerSlot = EQ::invslot::TRADE_BEGIN + laurion_slot.Slot;
				}

				else if (laurion_slot.SubIndex >= invbag::SLOT_BEGIN && laurion_slot.SubIndex <= invbag::SLOT_END) {
					TempSlot = laurion_slot.Slot * invbag::SLOT_COUNT;
					ServerSlot = EQ::invbag::TRADE_BAGS_BEGIN + TempSlot + laurion_slot.SubIndex;
				}
			}

			break;
		}
		case invtype::typeWorld: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::WORLD_SIZE) {
				ServerSlot = EQ::invslot::WORLD_BEGIN + laurion_slot.Slot;
			}

			else if (laurion_slot.Slot == invslot::SLOT_INVALID) {
				ServerSlot = EQ::invslot::SLOT_TRADESKILL_EXPERIMENT_COMBINE;
			}

			break;
		}
		case invtype::typeLimbo: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::LIMBO_SIZE) {
				ServerSlot = EQ::invslot::slotCursor;
			}

			break;
		}
		case invtype::typeTribute: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::TRIBUTE_SIZE) {
				ServerSlot = EQ::invslot::TRIBUTE_BEGIN + laurion_slot.Slot;
			}

			break;
		}
		case invtype::typeGuildTribute: {
			if (laurion_slot.Slot >= invslot::SLOT_BEGIN && laurion_slot.Slot < invtype::GUILD_TRIBUTE_SIZE) {
				ServerSlot = EQ::invslot::GUILD_TRIBUTE_BEGIN + laurion_slot.Slot;
			}

			break;
		}
		case invtype::typeCorpse: {
			if (laurion_slot.Slot >= invslot::CORPSE_BEGIN && laurion_slot.Slot <= invslot::CORPSE_END) {
				ServerSlot = laurion_slot.Slot;
			}

			break;
		}
		default: {

			break;
		}
		}

		Log(Logs::Detail, Logs::Netcode, "Convert Laurion Typeless Slot [%i, %i, %i] (implied type: %i) to Server Slot %i",
			laurion_slot.Slot, laurion_slot.SubIndex, laurion_slot.AugIndex, laurion_type, ServerSlot);

		return ServerSlot;
	}

	static inline structs::InventorySlot_Struct LaurionCastingInventorySlotToInventorySlot(structs::CastSpellInventorySlot_Struct laurion_slot) {
		structs::InventorySlot_Struct ret;
		ret.Type = laurion_slot.type;
		ret.Slot = laurion_slot.slot;
		ret.SubIndex = laurion_slot.sub_index;
		ret.AugIndex = laurion_slot.aug_index;
		return ret;
	}

	static inline structs::CastSpellInventorySlot_Struct LaurionInventorySlotToCastingInventorySlot(structs::InventorySlot_Struct laurion_slot) {
		structs::CastSpellInventorySlot_Struct ret;
		ret.type = laurion_slot.Type;
		ret.slot = laurion_slot.Slot;
		ret.sub_index = laurion_slot.SubIndex;
		ret.aug_index = laurion_slot.AugIndex;
		return ret;
	}

	static item::ItemPacketType ServerToLaurionItemPacketType(ItemPacketType server_type) {
		switch (server_type) {
		case ItemPacketType::ItemPacketMerchant:
			return item::ItemPacketType::ItemPacketMerchant;
		case ItemPacketType::ItemPacketTradeView:
			return item::ItemPacketType::ItemPacketTradeView;
		case ItemPacketType::ItemPacketLoot:
			return item::ItemPacketType::ItemPacketLoot;
		case ItemPacketType::ItemPacketTrade:
			return item::ItemPacketType::ItemPacketTrade;
		case ItemPacketType::ItemPacketCharInventory:
			return item::ItemPacketType::ItemPacketCharInventory;
		case ItemPacketType::ItemPacketLimbo:
			return item::ItemPacketType::ItemPacketLimbo;
		case ItemPacketType::ItemPacketWorldContainer:
			return item::ItemPacketType::ItemPacketWorldContainer;
		case ItemPacketType::ItemPacketTributeItem:
			return item::ItemPacketType::ItemPacketTributeItem;
		case ItemPacketType::ItemPacketGuildTribute:
			return item::ItemPacketType::ItemPacketGuildTribute;
		case ItemPacketType::ItemPacketCharmUpdate:
			return item::ItemPacketType::ItemPacketCharmUpdate;
		case ItemPacketType::ItemPacketDragonHoard:
			return item::ItemPacketType::ItemPacketDragonHoard;
		case ItemPacketType::ItemPacketDragonHoardBatch:
			return item::ItemPacketType::ItemPacketDragonHoard; // same client type; payload is count(4)+N×258
		default:
			return item::ItemPacketType::ItemPacketInvalid;
		}
	}

	//This stuff isn't right because they for one removed potion belt
	//This will probably be enough to get casting working for now though
	static inline spells::CastingSlot ServerToLaurionCastingSlot(EQ::spells::CastingSlot slot) {
		switch (slot) {
		case EQ::spells::CastingSlot::Gem1:
			return spells::CastingSlot::Gem1;
		case EQ::spells::CastingSlot::Gem2:
			return spells::CastingSlot::Gem2;
		case EQ::spells::CastingSlot::Gem3:
			return spells::CastingSlot::Gem3;
		case EQ::spells::CastingSlot::Gem4:
			return spells::CastingSlot::Gem4;
		case EQ::spells::CastingSlot::Gem5:
			return spells::CastingSlot::Gem5;
		case EQ::spells::CastingSlot::Gem6:
			return spells::CastingSlot::Gem6;
		case EQ::spells::CastingSlot::Gem7:
			return spells::CastingSlot::Gem7;
		case EQ::spells::CastingSlot::Gem8:
			return spells::CastingSlot::Gem8;
		case EQ::spells::CastingSlot::Gem9:
			return spells::CastingSlot::Gem9;
		case EQ::spells::CastingSlot::Gem10:
			return spells::CastingSlot::Gem10;
		case EQ::spells::CastingSlot::Gem11:
			return spells::CastingSlot::Gem11;
		case EQ::spells::CastingSlot::Gem12:
			return spells::CastingSlot::Gem12;
		case EQ::spells::CastingSlot::Item:
		case EQ::spells::CastingSlot::PotionBelt:
			return spells::CastingSlot::Item;
		case EQ::spells::CastingSlot::Discipline:
			return spells::CastingSlot::Discipline;
		case EQ::spells::CastingSlot::AltAbility:
			return spells::CastingSlot::AltAbility;
		default: // we shouldn't have any issues with other slots ... just return something
			return spells::CastingSlot::Discipline;
		}
	}

	static inline EQ::spells::CastingSlot LaurionToServerCastingSlot(spells::CastingSlot slot) {
		switch (slot) {
		case spells::CastingSlot::Gem1:
			return EQ::spells::CastingSlot::Gem1;
		case spells::CastingSlot::Gem2:
			return EQ::spells::CastingSlot::Gem2;
		case spells::CastingSlot::Gem3:
			return EQ::spells::CastingSlot::Gem3;
		case spells::CastingSlot::Gem4:
			return EQ::spells::CastingSlot::Gem4;
		case spells::CastingSlot::Gem5:
			return EQ::spells::CastingSlot::Gem5;
		case spells::CastingSlot::Gem6:
			return EQ::spells::CastingSlot::Gem6;
		case spells::CastingSlot::Gem7:
			return EQ::spells::CastingSlot::Gem7;
		case spells::CastingSlot::Gem8:
			return EQ::spells::CastingSlot::Gem8;
		case spells::CastingSlot::Gem9:
			return EQ::spells::CastingSlot::Gem9;
		case spells::CastingSlot::Gem10:
			return EQ::spells::CastingSlot::Gem10;
		case spells::CastingSlot::Gem11:
			return EQ::spells::CastingSlot::Gem11;
		case spells::CastingSlot::Gem12:
			return EQ::spells::CastingSlot::Gem12;
		case spells::CastingSlot::Discipline:
			return EQ::spells::CastingSlot::Discipline;
		case spells::CastingSlot::Item:
			return EQ::spells::CastingSlot::Item;
		case spells::CastingSlot::AltAbility:
			return EQ::spells::CastingSlot::AltAbility;
		default: // we shouldn't have any issues with other slots ... just return something
			return EQ::spells::CastingSlot::Discipline;
		}
	}

	//Laurion has the same # of long buffs as rof2, but 10 more short buffs
	static inline int ServerToLaurionBuffSlot(int index)
	{
		// we're a disc
		if (index >= EQ::spells::LONG_BUFFS + EQ::spells::SHORT_BUFFS)
			return index - EQ::spells::LONG_BUFFS - EQ::spells::SHORT_BUFFS +
			spells::LONG_BUFFS + spells::SHORT_BUFFS;
		// we're a song
		if (index >= EQ::spells::LONG_BUFFS)
			return index - EQ::spells::LONG_BUFFS + spells::LONG_BUFFS;
		// we're a normal buff
		return index; // as long as we guard against bad slots server side, we should be fine
	}

	static inline int LaurionToServerBuffSlot(int index)
	{
		// we're a disc
		if (index >= spells::LONG_BUFFS + spells::SHORT_BUFFS)
			return index - spells::LONG_BUFFS - spells::SHORT_BUFFS + EQ::spells::LONG_BUFFS +
			EQ::spells::SHORT_BUFFS;
		// we're a song
		if (index >= spells::LONG_BUFFS)
			return index - spells::LONG_BUFFS + EQ::spells::LONG_BUFFS;
		// we're a normal buff
		return index; // as long as we guard against bad slots server side, we should be fine
	}
} /*Laurion*/
