#define NO_MALLOC_OVERRIDE

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iostream>
#include <iclient.h>
#include <unordered_map>
#include "ivoicecodec.h"
#include "audio_effects.h"
#include "net.h"
#include "thirdparty.h"
#include "steam_voice.h"
#include "eightbit_state.h"
#include <GarrysMod/Symbol.hpp>
#include <cstdint>
#include "opus_framedecoder.h"

#define STEAM_PCKT_SZ sizeof(uint64_t) + sizeof(CRC32_t)
#ifdef SYSTEM_WINDOWS
	#include <windows.h>

	const std::vector<Symbol> BroadcastVoiceSyms = {
#if defined ARCHITECTURE_X86
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
		Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50"),
#elif defined ARCHITECTURE_X86_64
		Symbol::FromSignature("\x48\x89\x5C\x24*\x56\x57\x41\x56\x48\x81\xEC****\x8B\xF2\x4C\x8B\xF1"),
#endif
	};
#endif

#ifdef SYSTEM_LINUX
	#include <dlfcn.h>
	const std::vector<Symbol> BroadcastVoiceSyms = {
		Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx"),
		Symbol::FromSignature("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25"),
	};
#endif

static char decompressedBuffer[20 * 1024];
static char recompressBuffer[20 * 1024];

Net* net_handl = nullptr;
EightbitState* g_eightbit = nullptr;

typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
Detouring::Hook detour_BroadcastVoiceData;

void hook_BroadcastVoiceData(IClient* cl, uint nBytes, char* data, int64 xuid) {
	//Check if the player is in the set of enabled players.
	//This is (and needs to be) and O(1) operation for how often this function is called.
	//If not in the set, just hit the trampoline to ensure default behavior.
	int uid = cl->GetUserID();

#ifdef THIRDPARTY_LINK
	if(checkIfMuted(cl->GetPlayerSlot()+1)) {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
#endif

	if (g_eightbit->broadcastPackets && nBytes > sizeof(uint64_t)) {
		//Get the user's steamid64, put it at the beginning of the buffer.
		//Notice that we don't use the conveniently provided one in the voice packet. The client can manipulate that one.

#if defined ARCHITECTURE_X86
		uint64_t id64 = *(uint64_t*)((char*)cl + 181);
#else
		uint64_t id64 = *(uint64_t*)((char*)cl + 189);
#endif

		*(uint64_t*)decompressedBuffer = id64;

		//Transfer the packet data to our scratch buffer
		//This looks jank, but it's to prevent a theoretically malformed packet triggering a massive memcpy
		size_t toCopy = nBytes - sizeof(uint64_t);
		std::memcpy(decompressedBuffer + sizeof(uint64_t), data + sizeof(uint64_t), toCopy);

		//Finally we'll broadcast our new packet
 		net_handl->SendPacket(g_eightbit->ip.c_str(), g_eightbit->port, decompressedBuffer, nBytes);
	}

	auto found = g_eightbit->players.find(uid);
	if (found != g_eightbit->players.end() && found->second->effect != AudioEffects::EFF_NONE) {
		PlayerVoice* pv = found->second;
		IVoiceCodec* codec = pv->codec;

		if(nBytes < STEAM_PCKT_SZ) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int bytesDecompressed = SteamVoice::DecompressIntoBuffer(codec, data, nBytes, decompressedBuffer, sizeof(decompressedBuffer));
		int samples = bytesDecompressed / 2;
		if (bytesDecompressed <= 0) {
			//Just hit the trampoline at this point.
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Decompressed samples " << samples << std::endl;
		#endif

		//Apply audio effect
		int eff = pv->effect;
		switch (eff) {
		case AudioEffects::EFF_VOICEFX: {
			// Apres un silence, on repart d'un etat propre (pas de reste de l'ancienne phrase).
			auto now = std::chrono::steady_clock::now();
			if (now - pv->lastPacket > std::chrono::milliseconds(400))
				pv->fx.Reset();
			pv->lastPacket = now;
			pv->fx.Process((int16_t*)decompressedBuffer, samples);
			break;
		}
		case AudioEffects::EFF_BITCRUSH:
			AudioEffects::BitCrush((uint16_t*)&decompressedBuffer, samples, g_eightbit->crushFactor, g_eightbit->gainFactor);
			break;
		case AudioEffects::EFF_DESAMPLE:
			AudioEffects::Desample((uint16_t*)&decompressedBuffer, samples, g_eightbit->desampleRate);
			break;
		default:
			break;
		}

		//Recompress the stream
		uint64_t steamid = *(uint64_t*)data;
		int bytesWritten = SteamVoice::CompressIntoBuffer(steamid, codec, decompressedBuffer, samples*2, recompressBuffer, sizeof(recompressBuffer), 24000);
		if (bytesWritten <= 0) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		#ifdef _DEBUG
			std::cout << "Retransmitted pckt size: " << bytesWritten << std::endl;
		#endif

		//Broadcast voice data with our updated compressed data.
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

LUA_FUNCTION_STATIC(eightbit_crush) {
	g_eightbit->crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_gain) {
	g_eightbit->gainFactor = (float)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastip) {
	g_eightbit->ip = std::string(LUA->GetString());
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastport) {
	g_eightbit->port = (uint16_t)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_broadcast) {
	g_eightbit->broadcastPackets = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_getcrush) {
	LUA->PushNumber(g_eightbit->crushFactor);
	return 1;
}

LUA_FUNCTION_STATIC(eightbit_setdesamplerate) {
	g_eightbit->desampleRate = (int)LUA->GetNumber(1);
	return 0;
}

static PlayerVoice* GetOrCreatePlayer(int id) {
	auto it = g_eightbit->players.find(id);
	if (it != g_eightbit->players.end())
		return it->second;

	PlayerVoice* pv = new PlayerVoice();
	pv->codec = new SteamOpus::Opus_FrameDecoder();
	pv->codec->Init(5, 24000);
	g_eightbit->players[id] = pv;
	return pv;
}

LUA_FUNCTION_STATIC(eightbit_enableEffect) {
	int id = (int)LUA->CheckNumber(1);
	int eff = (int)LUA->CheckNumber(2);

	auto it = g_eightbit->players.find(id);
	if (it == g_eightbit->players.end() && eff == AudioEffects::EFF_NONE)
		return 0;

	PlayerVoice* pv = GetOrCreatePlayer(id);
	if (pv->effect != eff) {
		// Changement d'effet : on vide les etats pour eviter les craquements.
		pv->codec->ResetState();
		pv->fx.Reset();
	}
	pv->effect = eff;
	return 0;
}

// eightbit.SetVoiceParam(userid, "pitch", -4) -> true si la cle existe
LUA_FUNCTION_STATIC(eightbit_setVoiceParam) {
	int id = (int)LUA->CheckNumber(1);
	std::string key = LUA->CheckString(2);
	float value = (float)LUA->CheckNumber(3);

	PlayerVoice* pv = GetOrCreatePlayer(id);
	LUA->PushBool(VoiceFX::SetParam(pv->fx.params, key, value));
	return 1;
}

// eightbit.SetVoiceParams(userid, { pitch = -4, formant = -3, ... })
LUA_FUNCTION_STATIC(eightbit_setVoiceParams) {
	int id = (int)LUA->CheckNumber(1);
	LUA->CheckType(2, GarrysMod::Lua::Type::Table);

	PlayerVoice* pv = GetOrCreatePlayer(id);
	LUA->PushNil();
	while (LUA->Next(2) != 0) {
		// Ne pas convertir une cle non-string : ca casserait l'iteration.
		if (LUA->IsType(-2, GarrysMod::Lua::Type::String) && LUA->IsType(-1, GarrysMod::Lua::Type::Number))
			VoiceFX::SetParam(pv->fx.params, LUA->GetString(-2), (float)LUA->GetNumber(-1));
		LUA->Pop();
	}
	return 0;
}

// eightbit.ClearPlayer(userid) : libere tout (a appeler a la deconnexion)
LUA_FUNCTION_STATIC(eightbit_clearPlayer) {
	int id = (int)LUA->CheckNumber(1);
	auto it = g_eightbit->players.find(id);
	if (it != g_eightbit->players.end()) {
		delete it->second;
		g_eightbit->players.erase(it);
	}
	return 0;
}


GMOD_MODULE_OPEN()
{
	g_eightbit = new EightbitState();

	SourceSDK::ModuleLoader engine_loader("engine");
	SymbolFinder symfinder;

	void* sv_bcast = nullptr;

	for (const auto& sym : BroadcastVoiceSyms) {
		sv_bcast = symfinder.Resolve(engine_loader.GetModule(), sym.name.c_str(), sym.length);

		if (sv_bcast)
			break;
	}

	if (sv_bcast == nullptr) {
		LUA->ThrowError("Could not locate SV_BroadcastVoice symbol!");
	}

	detour_BroadcastVoiceData.Create(Detouring::Hook::Target(sv_bcast), reinterpret_cast<void*>(&hook_BroadcastVoiceData));
	detour_BroadcastVoiceData.Enable();

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("eightbit");
	LUA->CreateTable();
		LUA->PushString("SetCrushFactor");
		LUA->PushCFunction(eightbit_crush);
		LUA->SetTable(-3);

		LUA->PushString("GetCrushFactor");
		LUA->PushCFunction(eightbit_getcrush);
		LUA->SetTable(-3);

		LUA->PushString("EnableEffect");
		LUA->PushCFunction(eightbit_enableEffect);
		LUA->SetTable(-3);

		LUA->PushString("EnableBroadcast");
		LUA->PushCFunction(eightbit_broadcast);
		LUA->SetTable(-3);

		LUA->PushString("SetGainFactor");
		LUA->PushCFunction(eightbit_gain);
		LUA->SetTable(-3);

		LUA->PushString("SetDesampleRate");
		LUA->PushCFunction(eightbit_setdesamplerate);
		LUA->SetTable(-3);

		LUA->PushString("SetBroadcastIP");
		LUA->PushCFunction(eightbit_setbroadcastip);
		LUA->SetTable(-3);

		LUA->PushString("SetBroadcastPort");
		LUA->PushCFunction(eightbit_setbroadcastport);
		LUA->SetTable(-3);

		LUA->PushString("EFF_NONE");
		LUA->PushNumber(AudioEffects::EFF_NONE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_DESAMPLE");
		LUA->PushNumber(AudioEffects::EFF_DESAMPLE);
		LUA->SetTable(-3);

		LUA->PushString("EFF_BITCRUSH");
		LUA->PushNumber(AudioEffects::EFF_BITCRUSH);
		LUA->SetTable(-3);

		LUA->PushString("EFF_VOICEFX");
		LUA->PushNumber(AudioEffects::EFF_VOICEFX);
		LUA->SetTable(-3);

		LUA->PushString("SetVoiceParam");
		LUA->PushCFunction(eightbit_setVoiceParam);
		LUA->SetTable(-3);

		LUA->PushString("SetVoiceParams");
		LUA->PushCFunction(eightbit_setVoiceParams);
		LUA->SetTable(-3);

		LUA->PushString("ClearPlayer");
		LUA->PushCFunction(eightbit_clearPlayer);
		LUA->SetTable(-3);

		LUA->PushString("VOICEFX_VERSION");
		LUA->PushNumber(1);
		LUA->SetTable(-3);
	LUA->SetTable(-3);
	LUA->Pop();

	net_handl = new Net();

#ifdef THIRDPARTY_LINK
	linkMutedFunc();
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
	detour_BroadcastVoiceData.Disable();
	detour_BroadcastVoiceData.Destroy();

	for (auto& p : g_eightbit->players)
		delete p.second;
	g_eightbit->players.clear();

	delete net_handl;
	delete g_eightbit;

	return 0;
}
