#pragma once
// Chaine d'effets vocaux temps reel (24 kHz mono), un etat par joueur.
// Ordre : pitch/formants (+ octave dessous) -> passe-haut -> saturation
//         -> EQ -> resonance metallique -> chorus -> reverb -> gain -> limiteur
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <algorithm>

namespace VoiceFX {

	constexpr int SR = 24000;
	constexpr float PI_F = 3.14159265358979f;

	struct Params {
		float pitch = 0.0f;       // demi-tons
		float formant = 0.0f;     // demi-tons (0 = timbre naturel conserve)
		float sub = 0.0f;         // 0..1, voix une octave dessous
		float drive = 0.0f;       // 0..1, saturation
		float highpass = 0.0f;    // Hz (0 = off)
		float bass = 0.0f;        // dB, plateau 250 Hz
		float treble = 0.0f;      // dB, plateau 3 kHz
		float lowpass = 12000.0f; // Hz (>= 11000 = off)
		float metal = 0.0f;       // 0..1, resonance metallique
		float metalFreq = 220.0f; // Hz
		float chorus = 0.0f;      // 0..1
		float reverb = 0.0f;      // 0..1
		float room = 0.5f;        // 0..1, taille de la piece
		float gain = 1.0f;        // multiplicateur
	};

	inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// Renvoie false si la cle est inconnue.
	inline bool SetParam(Params& p, const std::string& key, float v) {
		if (key == "pitch") p.pitch = Clamp(v, -24.0f, 24.0f);
		else if (key == "formant") p.formant = Clamp(v, -24.0f, 24.0f);
		else if (key == "sub") p.sub = Clamp(v, 0.0f, 1.0f);
		else if (key == "drive") p.drive = Clamp(v, 0.0f, 1.0f);
		else if (key == "highpass") p.highpass = Clamp(v, 0.0f, 2000.0f);
		else if (key == "bass") p.bass = Clamp(v, -24.0f, 24.0f);
		else if (key == "treble") p.treble = Clamp(v, -24.0f, 24.0f);
		else if (key == "lowpass") p.lowpass = Clamp(v, 500.0f, 12000.0f);
		else if (key == "metal") p.metal = Clamp(v, 0.0f, 1.0f);
		else if (key == "metalfreq") p.metalFreq = Clamp(v, 50.0f, 2000.0f);
		else if (key == "chorus") p.chorus = Clamp(v, 0.0f, 1.0f);
		else if (key == "reverb") p.reverb = Clamp(v, 0.0f, 1.0f);
		else if (key == "room") p.room = Clamp(v, 0.0f, 1.0f);
		else if (key == "gain") p.gain = Clamp(v, 0.0f, 4.0f);
		else return false;
		return true;
	}

	// FFT complexe radix-2 en place.
	inline void FFT(float* re, float* im, int n, bool inverse) {
		for (int i = 1, j = 0; i < n; i++) {
			int bit = n >> 1;
			for (; j & bit; bit >>= 1) j ^= bit;
			j ^= bit;
			if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
		}
		for (int len = 2; len <= n; len <<= 1) {
			double ang = 2.0 * 3.14159265358979323846 / len * (inverse ? 1.0 : -1.0);
			double wr = std::cos(ang), wi = std::sin(ang);
			int half = len >> 1;
			for (int i = 0; i < n; i += len) {
				double cr = 1.0, ci = 0.0;
				for (int j = 0; j < half; j++) {
					int a = i + j, b = a + half;
					float xr = (float)(re[b] * cr - im[b] * ci);
					float xi = (float)(re[b] * ci + im[b] * cr);
					re[b] = re[a] - xr; im[b] = im[a] - xi;
					re[a] += xr; im[a] += xi;
					double t = cr * wr - ci * wi;
					ci = cr * wi + ci * wr;
					cr = t;
				}
			}
		}
	}

	// Pitch shift par vocodeur de phase, avec enveloppe spectrale (cepstre)
	// pour deplacer les formants independamment du pitch.
	class PitchShifter {
	public:
		static constexpr int N = 1024;
		static constexpr int OSAMP = 4;
		static constexpr int HOP = N / OSAMP;
		static constexpr int HALF = N / 2;
		static constexpr int LATENCY = N - HOP;
		static constexpr int LIFTER = 40; // coupe le pitch (> 600 Hz) de l'enveloppe

		PitchShifter() {
			for (int k = 0; k < N; k++)
				win[k] = 0.5f - 0.5f * std::cos(2.0f * PI_F * k / N);
			Reset();
		}

		void Reset() {
			std::memset(inFIFO, 0, sizeof(inFIFO));
			std::memset(outFIFO, 0, sizeof(outFIFO));
			std::memset(outAccum, 0, sizeof(outAccum));
			std::memset(lastPhase, 0, sizeof(lastPhase));
			std::memset(sumPhase, 0, sizeof(sumPhase));
			std::memset(sumPhaseSub, 0, sizeof(sumPhaseSub));
			rover = LATENCY;
		}

		void Process(float* buf, int n, const Params& p) {
			for (int i = 0; i < n; i++) {
				inFIFO[rover] = buf[i];
				buf[i] = outFIFO[rover - LATENCY];
				rover++;
				if (rover >= N) {
					rover = LATENCY;
					ProcessFrame(p);
				}
			}
		}

	private:
		float win[N];
		float inFIFO[N], outFIFO[N], outAccum[2 * N];
		float lastPhase[HALF + 1], sumPhase[HALF + 1], sumPhaseSub[HALF + 1];
		float re[N], im[N], cre[N], cim[N];
		float anaMagn[HALF + 1], anaFreq[HALF + 1], env[HALF + 1], fine[HALF + 1];
		float synMagn[HALF + 1], synFreq[HALF + 1];
		int rover = LATENCY;

		// Gain de reconstruction : somme des fenetres de Hann au carre a 4x = 1.5
		static constexpr float OLA_NORM = 1.0f / 1.5f;

		float EnvAt(float bin) const {
			if (bin <= 0.0f) return env[0];
			if (bin >= (float)HALF) return env[HALF] * 0.1f;
			int i = (int)bin;
			float f = bin - i;
			return env[i] * (1.0f - f) + env[i + 1] * f;
		}

		void Synthesize(float ratio, float formRatio, float* phaseAcc, float mix, float expct) {
			std::memset(synMagn, 0, sizeof(synMagn));
			std::memset(synFreq, 0, sizeof(synFreq));
			for (int k = 0; k <= HALF; k++) {
				int idx = (int)(k * ratio + 0.5f);
				if (idx > HALF) break;
				synMagn[idx] += fine[k];
				synFreq[idx] = anaFreq[k] * ratio;
			}
			for (int k = 0; k <= HALF; k++) {
				float m = synMagn[k] * EnvAt(k / formRatio) * mix;
				float tmp = synFreq[k] - (float)k;
				tmp = 2.0f * PI_F * tmp / OSAMP + k * expct;
				phaseAcc[k] = std::fmod(phaseAcc[k] + tmp, 2.0f * PI_F);
				re[k] += m * std::cos(phaseAcc[k]);
				im[k] += m * std::sin(phaseAcc[k]);
			}
		}

		void ProcessFrame(const Params& p) {
			const float expct = 2.0f * PI_F * HOP / N;
			const bool bypass = std::fabs(p.pitch) < 0.01f && std::fabs(p.formant) < 0.01f && p.sub < 0.001f;

			for (int k = 0; k < N; k++) { re[k] = inFIFO[k] * win[k]; im[k] = 0.0f; }

			if (bypass) {
				// Passage direct, mais on garde les phases a jour pour une reprise propre.
				for (int k = 0; k < N; k++) outAccum[k] += re[k] * win[k] * OLA_NORM;
				FFT(re, im, N, false);
				for (int k = 0; k <= HALF; k++) {
					float ph = std::atan2(im[k], re[k]);
					lastPhase[k] = ph;
					sumPhase[k] = ph;
					sumPhaseSub[k] = ph;
				}
			}
			else {
				FFT(re, im, N, false);

				// Analyse : amplitude + frequence reelle de chaque bin
				for (int k = 0; k <= HALF; k++) {
					float magn = std::sqrt(re[k] * re[k] + im[k] * im[k]);
					float ph = std::atan2(im[k], re[k]);
					float tmp = ph - lastPhase[k];
					lastPhase[k] = ph;
					tmp -= k * expct;
					tmp = tmp - 2.0f * PI_F * std::floor(tmp / (2.0f * PI_F) + 0.5f);
					anaFreq[k] = k + OSAMP * tmp / (2.0f * PI_F);
					anaMagn[k] = magn;
				}

				// Enveloppe spectrale par cepstre lisse
				for (int k = 0; k < N; k++) {
					int b = k <= HALF ? k : N - k;
					cre[k] = std::log(anaMagn[b] + 1e-6f);
					cim[k] = 0.0f;
				}
				FFT(cre, cim, N, true);
				for (int k = 0; k < N; k++) {
					bool keep = (k <= LIFTER) || (k >= N - LIFTER);
					cre[k] = keep ? cre[k] / N : 0.0f;
					cim[k] = keep ? cim[k] / N : 0.0f;
				}
				FFT(cre, cim, N, false);
				for (int k = 0; k <= HALF; k++) {
					env[k] = std::exp(cre[k]);
					fine[k] = anaMagn[k] / (env[k] + 1e-9f);
				}

				const float ratio = std::pow(2.0f, p.pitch / 12.0f);
				const float formRatio = std::pow(2.0f, p.formant / 12.0f);

				std::memset(re, 0, sizeof(re));
				std::memset(im, 0, sizeof(im));
				Synthesize(ratio, formRatio, sumPhase, 1.0f, expct);
				if (p.sub > 0.001f)
					Synthesize(ratio * 0.5f, formRatio, sumPhaseSub, p.sub, expct);

				// Spectre symetrique pour une sortie reelle
				im[0] = 0.0f; im[HALF] = 0.0f;
				for (int k = 1; k < HALF; k++) { re[N - k] = re[k]; im[N - k] = -im[k]; }
				FFT(re, im, N, true);

				for (int k = 0; k < N; k++)
					outAccum[k] += re[k] * win[k] * (OLA_NORM / N);
			}

			std::memcpy(outFIFO, outAccum, HOP * sizeof(float));
			std::memmove(outAccum, outAccum + HOP, (2 * N - HOP) * sizeof(float));
			std::memset(outAccum + 2 * N - HOP, 0, HOP * sizeof(float));
			std::memmove(inFIFO, inFIFO + HOP, LATENCY * sizeof(float));
		}
	};

	// Filtre biquad (formules RBJ).
	struct Biquad {
		float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
		float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

		void Reset() { x1 = x2 = y1 = y2 = 0; }

		void Set(float nb0, float nb1, float nb2, float na0, float na1, float na2) {
			b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
		}

		void LowPass(float f) {
			float w = 2 * PI_F * f / SR, c = std::cos(w), a = std::sin(w) / (2 * 0.7071f);
			Set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
		}

		void HighPass(float f) {
			float w = 2 * PI_F * f / SR, c = std::cos(w), a = std::sin(w) / (2 * 0.7071f);
			Set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a);
		}

		void Shelf(float f, float db, bool high) {
			float A = std::pow(10.0f, db / 40.0f);
			float w = 2 * PI_F * f / SR, c = std::cos(w), s = std::sin(w);
			float a = s / 2 * std::sqrt(2.0f), sq = 2 * std::sqrt(A) * a;
			if (!high)
				Set(A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sq),
					(A + 1) + (A - 1) * c + sq, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
			else
				Set(A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq),
					(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
		}

		float Run(float x) {
			float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1; x1 = x; y2 = y1; y1 = y;
			return y;
		}
	};

	// Reverb type Freeverb, longueurs ramenees a 24 kHz.
	class Reverb {
	public:
		Reverb() { Reset(); }

		void Reset() {
			std::memset(combBuf, 0, sizeof(combBuf));
			std::memset(apBuf, 0, sizeof(apBuf));
			std::memset(combIdx, 0, sizeof(combIdx));
			std::memset(apIdx, 0, sizeof(apIdx));
			std::memset(combLp, 0, sizeof(combLp));
		}

		float Run(float x, float room) {
			const float fb = 0.70f + 0.28f * room;
			const float damp = 0.3f;
			float in = x * 0.015f, out = 0.0f;
			for (int c = 0; c < NCOMB; c++) {
				float& slot = combBuf[c][combIdx[c]];
				float y = slot;
				combLp[c] = y * (1 - damp) + combLp[c] * damp;
				slot = in + combLp[c] * fb;
				if (++combIdx[c] >= COMB_LEN[c]) combIdx[c] = 0;
				out += y;
			}
			for (int a = 0; a < NAP; a++) {
				float& slot = apBuf[a][apIdx[a]];
				float b = slot;
				slot = out + b * 0.5f;
				out = b - out;
				if (++apIdx[a] >= AP_LEN[a]) apIdx[a] = 0;
			}
			return out * 3.0f;
		}

	private:
		static constexpr int NCOMB = 8, NAP = 4, MAXLEN = 900;
		const int COMB_LEN[NCOMB] = { 607, 646, 695, 738, 774, 811, 847, 880 };
		const int AP_LEN[NAP] = { 302, 240, 186, 122 };
		float combBuf[NCOMB][MAXLEN];
		float apBuf[NAP][MAXLEN];
		float combLp[NCOMB];
		int combIdx[NCOMB], apIdx[NAP];
	};

	class Processor {
	public:
		Params params;

		Processor() { Reset(); }

		void Reset() {
			pitch.Reset();
			reverb.Reset();
			hp.Reset(); lowShelf.Reset(); highShelf.Reset(); lp.Reset();
			std::memset(metalBuf, 0, sizeof(metalBuf));
			std::memset(chorusBuf, 0, sizeof(chorusBuf));
			metalIdx = chorusIdx = 0;
			lfo = 0.0f;
			cacheValid = false;
		}

		void Process(int16_t* samples, int n) {
			static float tmp[16 * 1024];
			if (n <= 0) return;
			if (n > (int)(sizeof(tmp) / sizeof(tmp[0]))) n = (int)(sizeof(tmp) / sizeof(tmp[0]));

			UpdateFilters();
			for (int i = 0; i < n; i++) tmp[i] = samples[i] / 32768.0f;

			pitch.Process(tmp, n, params);

			const Params& p = params;
			const float driveK = 1.0f + p.drive * 15.0f;
			const float driveNorm = 1.0f / std::tanh(driveK);
			const int metalDelay = std::max(1, std::min(METAL_LEN - 1, (int)(SR / p.metalFreq)));
			const float metalFb = 0.5f + 0.4f * p.metal;

			for (int i = 0; i < n; i++) {
				float x = tmp[i];

				if (p.highpass > 20.0f) x = hp.Run(x);

				if (p.drive > 0.001f)
					x = x * (1.0f - p.drive) + std::tanh(x * driveK) * driveNorm * p.drive * 0.7f;

				if (std::fabs(p.bass) > 0.05f) x = lowShelf.Run(x);
				if (std::fabs(p.treble) > 0.05f) x = highShelf.Run(x);
				if (p.lowpass < 11000.0f) x = lp.Run(x);

				if (p.metal > 0.001f) {
					int r = metalIdx - metalDelay; if (r < 0) r += METAL_LEN;
					float y = x + metalFb * metalBuf[r];
					metalBuf[metalIdx] = y;
					if (++metalIdx >= METAL_LEN) metalIdx = 0;
					x = x * (1.0f - p.metal) + y * (1.0f - metalFb) * p.metal * 1.5f;
				}

				chorusBuf[chorusIdx] = x;
				if (p.chorus > 0.001f) {
					lfo += 2.0f * PI_F * 0.7f / SR;
					if (lfo > 2.0f * PI_F) lfo -= 2.0f * PI_F;
					float v = ReadChorus(0.012f + 0.004f * std::sin(lfo)) + ReadChorus(0.018f + 0.005f * std::sin(lfo * 1.3f + 1.7f));
					x = x * (1.0f - 0.4f * p.chorus) + v * 0.5f * p.chorus;
				}
				if (++chorusIdx >= CHORUS_LEN) chorusIdx = 0;

				if (p.reverb > 0.001f) {
					float w = reverb.Run(x, p.room);
					x = x * (1.0f - 0.5f * p.reverb) + w * p.reverb;
				}

				x *= p.gain;

				// Limiteur doux au-dessus de 0.7
				float ax = std::fabs(x);
				if (ax > 0.7f) x = (x > 0 ? 1.0f : -1.0f) * (0.7f + 0.3f * std::tanh((ax - 0.7f) / 0.3f));

				int s = (int)(x * 32767.0f);
				samples[i] = (int16_t)std::max(-32768, std::min(32767, s));
			}
		}

	private:
		static constexpr int METAL_LEN = 512;
		static constexpr int CHORUS_LEN = 1024;

		PitchShifter pitch;
		Reverb reverb;
		Biquad hp, lowShelf, highShelf, lp;
		float metalBuf[METAL_LEN];
		float chorusBuf[CHORUS_LEN];
		int metalIdx = 0, chorusIdx = 0;
		float lfo = 0.0f;

		bool cacheValid = false;
		float cHp = -1, cBass = -1000, cTreble = -1000, cLp = -1;

		float ReadChorus(float delaySec) {
			float d = delaySec * SR;
			float pos = (float)chorusIdx - d;
			while (pos < 0) pos += CHORUS_LEN;
			int i0 = (int)pos;
			float f = pos - i0;
			int i1 = (i0 + 1) % CHORUS_LEN;
			return chorusBuf[i0] * (1 - f) + chorusBuf[i1] * f;
		}

		// Ne recalcule les coefficients que si les reglages ont change.
		void UpdateFilters() {
			const Params& p = params;
			if (!cacheValid || p.highpass != cHp) { hp.HighPass(std::max(20.0f, p.highpass)); cHp = p.highpass; }
			if (!cacheValid || p.bass != cBass) { lowShelf.Shelf(250.0f, p.bass, false); cBass = p.bass; }
			if (!cacheValid || p.treble != cTreble) { highShelf.Shelf(3000.0f, p.treble, true); cTreble = p.treble; }
			if (!cacheValid || p.lowpass != cLp) { lp.LowPass(std::min(11500.0f, p.lowpass)); cLp = p.lowpass; }
			cacheValid = true;
		}
	};
}
