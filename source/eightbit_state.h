#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include "ivoicecodec.h"
#include "voice_fx.h"

struct PlayerVoice {
	IVoiceCodec* codec = nullptr;
	int effect = 0; // AudioEffects::EFF_NONE
	VoiceFX::Processor fx;
	std::chrono::steady_clock::time_point lastPacket;

	~PlayerVoice() { delete codec; }
};

struct EightbitState {
	int crushFactor = 350;
	float gainFactor = 1.2;
	bool broadcastPackets = false;
	int desampleRate = 2;
	uint16_t port = 4000;
	std::string ip = "127.0.0.1";
	std::unordered_map<int, PlayerVoice*> players;
};
