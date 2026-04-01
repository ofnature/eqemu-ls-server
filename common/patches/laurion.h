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

#ifndef COMMON_LAURION_H
#define COMMON_LAURION_H

#include "../struct_strategy.h"
#include "../serialize_buffer.h"
#include "../item_instance.h"

class EQStreamIdentifier;

namespace Laurion
{

	// Per-item size for Dragon's Hoard batch packet (59 header + 9 trail + 4 blob L + 186 blob = 258). Count + N×this = total payload.
	static const size_t kDragonHoardItemDataSize = 258;

	// Serialize one Dragon's Hoard item into buffer (no leading count). Used by SendDragonHoardItemList to build batch packet. Pad to kDragonHoardItemDataSize if needed.
	void SerializeOneDragonHoardItem(SerializeBuffer& buffer, const EQ::ItemInstance* inst, int16 slot_id);

	//these are the only public member of this namespace.
	extern void Register(EQStreamIdentifier& into);
	extern void Reload();



	//you should not directly access anything below..
	//I just dont feel like making a seperate header for it.

	class Strategy : public StructStrategy {
	public:
		Strategy();

	protected:

		virtual std::string Describe() const;
		virtual const EQ::versions::ClientVersion ClientVersion() const;

		//magic macro to declare our opcode processors
#include "ss_declare.h"
#include "laurion_ops.h"
	};

}; /*Laurion*/

#endif /*COMMON_LAURION_H*/
