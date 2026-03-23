#include "plugin.hpp"
#include <cmath>
#include <algorithm>

static const int NUM_STEPS = 16;

// ─────────────────────────────────────────────────────────────────────────────
// Module
// ─────────────────────────────────────────────────────────────────────────────

struct Barrage : Module {
	enum ParamId {
		ENUMS(COUNT_PARAM,  NUM_STEPS),   // Gates per step (1–16)
		ENUMS(LENGTH_PARAM, NUM_STEPS),   // Gate length (fraction of step)
		ENUMS(PROB_PARAM,   NUM_STEPS),   // Probability (0–100%)
		ENUMS(SPEED_PARAM,  NUM_STEPS),   // Burst speed multiplier (0.25–4×)
		SEQ_LENGTH_PARAM,                 // Active step count (1–16)
		DIRECTION_PARAM,                  // Fwd / Rev toggle button
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(GATE_OUTPUT,      NUM_STEPS), // Per-step burst gate outputs
		ENUMS(BURST_EOC_OUTPUT, NUM_STEPS), // Per-step burst end-of-cycle pulses
		EOC_OUTPUT,                         // End-of-sequence pulse
		STEP_OUTPUT,                        // Current step as 0–10 V
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(STEP_LIGHT, NUM_STEPS),
		LIGHTS_LEN
	};

	// DSP helpers
	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::PulseGenerator eocPulse;
	dsp::PulseGenerator burstEocPulses[NUM_STEPS];

	// Sequencer state
	int   currentStep        = -1;  // -1 = before first clock
	float clockPeriod        = 0.f; // set by onSampleRateChange before first process()
	float samplesSinceClock  = 0.f;

	// Burst state for the current step
	bool  stepActive      = false;
	int   burstTotal      = 1;
	float burstLength     = 0.5f;
	bool  pingPongForward = true;

	Barrage() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		for (int i = 0; i < NUM_STEPS; i++) {
			configParam(COUNT_PARAM  + i, 1.f,  16.f, 1.f,  string::f("Step %d: Gate Count",  i + 1), "")->snapEnabled = true;
			configParam(LENGTH_PARAM + i, 0.f,   1.f, 0.5f, string::f("Step %d: Gate Length",  i + 1), "%", 0.f, 100.f);
			configParam(PROB_PARAM   + i, 0.f,   1.f, 1.f,  string::f("Step %d: Probability",  i + 1), "%", 0.f, 100.f);
			configParam(SPEED_PARAM  + i, 0.25f, 4.f, 1.f,  string::f("Step %d: Speed",        i + 1), "x");
		}

		configParam(SEQ_LENGTH_PARAM, 1.f, 16.f, 16.f, "Sequence Length", " steps")->snapEnabled = true;
		configSwitch(DIRECTION_PARAM, 0.f, 3.f, 0.f, "Direction", {"Forward", "Reverse", "Ping-pong", "Random"});

		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");

		for (int i = 0; i < NUM_STEPS; i++)
			configOutput(GATE_OUTPUT + i,      string::f("Step %d Gate",      i + 1));
		for (int i = 0; i < NUM_STEPS; i++)
			configOutput(BURST_EOC_OUTPUT + i, string::f("Step %d Burst EOC", i + 1));
		configOutput(EOC_OUTPUT,  "End of Sequence");
		configOutput(STEP_OUTPUT, "Step CV");
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		clockPeriod = e.sampleRate; // default: 1 second at the actual sample rate
	}

	void process(const ProcessArgs& args) override {
		int seqLength = clamp((int)std::round(params[SEQ_LENGTH_PARAM].getValue()), 1, NUM_STEPS);
		int dirMode   = (int)std::round(params[DIRECTION_PARAM].getValue()); // 0=fwd 1=rev 2=pingpong 3=random

		// Keep currentStep in bounds if seqLength was reduced
		if (currentStep >= seqLength)
			currentStep = seqLength - 1;

		// ── Reset ────────────────────────────────────────────────────────────
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 2.f)) {
			currentStep     = -1;
			pingPongForward = true;
			stepActive      = false;
		}

		// ── Clock ────────────────────────────────────────────────────────────
		if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.f)) {
			// Update clock period estimate (ignore implausibly long gaps)
			if (samplesSinceClock > 0.f && samplesSinceClock < args.sampleRate * 10.f)
				clockPeriod = samplesSinceClock;
			samplesSinceClock = 0.f;

			int prevStep = currentStep;

			// Fire burst EOC for the outgoing step on the clock edge — clock-locked,
			// so downstream switches get a stable trigger with no drift.
			if (prevStep >= 0 && stepActive)
				burstEocPulses[prevStep].trigger(1e-3f);

			switch (dirMode) {
				case 0: // Forward
					currentStep++;
					if (currentStep >= seqLength) {
						currentStep = 0;
						if (prevStep >= 0) eocPulse.trigger(1e-3f);
					}
					break;

				case 1: // Reverse
					currentStep--;
					if (currentStep < 0) {
						currentStep = seqLength - 1;
						if (prevStep >= 0) eocPulse.trigger(1e-3f);
					}
					break;

				case 2: // Ping-pong
					if (currentStep < 0) {
						currentStep     = 0;
						pingPongForward = true;
					} else if (pingPongForward) {
						currentStep++;
						if (currentStep >= seqLength) {
							pingPongForward = false;
							currentStep     = std::max(seqLength - 2, 0);
						}
					} else {
						currentStep--;
						if (currentStep < 0) {
							pingPongForward = true;
							currentStep     = std::min(1, seqLength - 1);
							if (prevStep >= 0) eocPulse.trigger(1e-3f);
						}
					}
					break;

				case 3: // Random
				default:
					currentStep = (int)(random::uniform() * seqLength);
					break;
			}

			// Probability gate
			stepActive = (random::uniform() < params[PROB_PARAM + currentStep].getValue());

			if (stepActive) {
				burstTotal  = clamp((int)std::round(params[COUNT_PARAM + currentStep].getValue()), 1, NUM_STEPS);
				burstLength = std::min(params[LENGTH_PARAM + currentStep].getValue(), 0.9999f);
			}
		}

		samplesSinceClock += 1.f;

		// ── Burst gate ───────────────────────────────────────────────────────
		bool gateHigh = false;
		if (stepActive && clockPeriod > 0.f) {
			float speed      = params[SPEED_PARAM + currentStep].getValue();
			float phase      = samplesSinceClock * (float)burstTotal * speed / clockPeriod;
			float liveLength = std::min(params[LENGTH_PARAM + currentStep].getValue(), 0.9999f);
			float subPhase   = std::fmod(phase, 1.f);
			gateHigh = (phase < (float)burstTotal) && (subPhase < liveLength);

		}

		// ── Outputs ──────────────────────────────────────────────────────────
		for (int i = 0; i < NUM_STEPS; i++) {
			outputs[GATE_OUTPUT      + i].setVoltage((i == currentStep && gateHigh) ? 10.f : 0.f);
			outputs[BURST_EOC_OUTPUT + i].setVoltage(burstEocPulses[i].process(args.sampleTime) ? 10.f : 0.f);
		}
		outputs[EOC_OUTPUT ].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
		outputs[STEP_OUTPUT].setVoltage(currentStep >= 0 ? (currentStep / 15.f * 10.f) : 0.f);

		// ── Lights ───────────────────────────────────────────────────────────
		for (int i = 0; i < NUM_STEPS; i++)
			lights[STEP_LIGHT + i].setBrightness(i == currentStep ? 1.f : 0.f);
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Panel
// ─────────────────────────────────────────────────────────────────────────────

struct BarragePanel : Widget {
	BarragePanel(Vec size) {
		box.size = size;
	}

	// Horizontal separator helper — spans the main panel, clear of the left sidebar
	void drawSep(NVGcontext* vg, float y, float x1 = 78.f, float x2 = 654.f) {
		nvgBeginPath(vg);
		nvgMoveTo(vg, x1, y);
		nvgLineTo(vg, x2, y);
		nvgStrokeColor(vg, nvgRGB(0x44, 0x44, 0x44));
		nvgStrokeWidth(vg, 0.8f);
		nvgStroke(vg);
	}

	// Left-aligned section label with a trailing rule
	void drawSectionLabel(NVGcontext* vg, std::shared_ptr<Font> font,
	                      float y, const char* label, float ruleX2 = 654.f) {
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, 7.5f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0x55, 0x55, 0x66));
		nvgText(vg, 82.f, y, label, NULL);

		// Trailing rule after label text
		float bounds[4];
		nvgTextBounds(vg, 82.f, y, label, NULL, bounds);
		nvgBeginPath(vg);
		nvgMoveTo(vg, bounds[2] + 5.f, y);
		nvgLineTo(vg, ruleX2, y);
		nvgStrokeColor(vg, nvgRGB(0x3a, 0x3a, 0x44));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float W = box.size.x;

		// ── Background ─────────────────────────────────────────────────────
		nvgBeginPath(vg);
		nvgRect(vg, 0, 0, W, box.size.y);
		nvgFillColor(vg, nvgRGB(0x2a, 0x2a, 0x2a));
		nvgFill(vg);

		// ── Inner panel face ───────────────────────────────────────────────
		float inset = 4.f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, inset, inset, W - 2.f * inset, box.size.y - 2.f * inset, 5.f);
		nvgFillColor(vg, nvgRGB(0x22, 0x22, 0x22));
		nvgFill(vg);
		nvgStrokeColor(vg, nvgRGB(0x33, 0x33, 0x33));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// ── Font ───────────────────────────────────────────────────────────
		std::shared_ptr<Font> font = APP->window->loadFont(
			asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		if (!font) return;
		nvgFontFaceId(vg, font->handle);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

		// ── Title ──────────────────────────────────────────────────────────
		nvgFontSize(vg, 11.f);
		nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
		nvgText(vg, 366.f, 13.f, "BARRAGE", NULL);

		drawSep(vg, 23.f);

		// ── Step indicator row ─────────────────────────────────────────────
		// LEDs are placed by the widget; draw step-number labels here
		nvgFontSize(vg, 6.5f);
		nvgFillColor(vg, nvgRGB(0x55, 0x55, 0x66));
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = 100.f + i * 36.25f;
			char buf[4];
			snprintf(buf, sizeof(buf), "%02d", i + 1);
			nvgText(vg, x, 50.f, buf, NULL);
		}

		drawSep(vg, 60.f);

		// ── Per-step section labels (30 px row spacing) ───────────────────
		drawSectionLabel(vg, font, 73.f,  "COUNT");
		drawSectionLabel(vg, font, 103.f, "LENGTH");
		drawSectionLabel(vg, font, 133.f, "PROB");
		drawSectionLabel(vg, font, 163.f, "SPEED");

		drawSep(vg, 193.f);

		// ── Global controls section ────────────────────────────────────────
		nvgFontSize(vg, 8.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0x88, 0x88, 0x88));
		nvgText(vg, 147.f, 208.f, "STEPS", NULL);
		nvgText(vg, 222.f, 208.f, "DIR",   NULL);

		drawSep(vg, 250.f);

		// ── Gate outputs section ───────────────────────────────────────────
		drawSectionLabel(vg, font, 258.f, "GATE OUT");
		drawSectionLabel(vg, font, 302.f, "BURST EOC");
		drawSep(vg, 340.f);

		// ── Footer branding ────────────────────────────────────────────────
		nvgFontSize(vg, 7.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

		// Measure both parts so "VONK" is centred at x=294
		float vonBounds[4], kBounds[4];
		float vonWidth = nvgTextBounds(vg, 0.f, 360.f, "VON", NULL, vonBounds);
		float kWidth   = nvgTextBounds(vg, 0.f, 360.f, "K",   NULL, kBounds);
		float startX   = 366.f - (vonWidth + kWidth) / 2.f;

		nvgFillColor(vg, nvgRGB(0xf4, 0xf5, 0xf7));
		nvgText(vg, startX, 360.f, "VON", NULL);

		nvgFillColor(vg, nvgRGB(0xc0, 0x84, 0xfc));
		nvgText(vg, startX + vonWidth, 360.f, "K", NULL);

		// ── Sidebar — vertical separator ───────────────────────────────────
		nvgBeginPath(vg);
		nvgMoveTo(vg, 72.f, 4.f);
		nvgLineTo(vg, 72.f, box.size.y - 4.f);
		nvgStrokeColor(vg, nvgRGB(0x33, 0x33, 0x33));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);

		// ── Sidebar — IN section ───────────────────────────────────────────
		nvgFontSize(vg, 7.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0x55, 0x55, 0x66));
		nvgText(vg, 36.f, 82.f, "IN", NULL);

		nvgBeginPath(vg);
		nvgMoveTo(vg, 6.f, 91.f);
		nvgLineTo(vg, 66.f, 91.f);
		nvgStrokeColor(vg, nvgRGB(0x3a, 0x3a, 0x44));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);

		nvgFontSize(vg, 8.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0xcc, 0xcc, 0xcc));
		nvgText(vg, 36.f, 103.f, "CLK", NULL);
		nvgText(vg, 36.f, 148.f, "RST", NULL);

		// ── Sidebar — separator between inputs and outputs ─────────────────
		nvgBeginPath(vg);
		nvgMoveTo(vg, 6.f, 185.f);
		nvgLineTo(vg, 66.f, 185.f);
		nvgStrokeColor(vg, nvgRGB(0x3a, 0x3a, 0x44));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);

		// ── Sidebar — OUT section ──────────────────────────────────────────
		nvgFontSize(vg, 7.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0x55, 0x55, 0x66));
		nvgText(vg, 36.f, 195.f, "OUT", NULL);

		nvgBeginPath(vg);
		nvgMoveTo(vg, 6.f, 204.f);
		nvgLineTo(vg, 66.f, 204.f);
		nvgStrokeColor(vg, nvgRGB(0x3a, 0x3a, 0x44));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);

		nvgFontSize(vg, 8.5f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0x88, 0xcc, 0x88));
		nvgText(vg, 36.f, 211.f, "EOC",  NULL);
		nvgText(vg, 36.f, 256.f, "STEP", NULL);

		// Horizontal rule below all sidebar ports
		nvgBeginPath(vg);
		nvgMoveTo(vg, 6.f, 288.f);
		nvgLineTo(vg, 66.f, 288.f);
		nvgStrokeColor(vg, nvgRGB(0x3a, 0x3a, 0x44));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Widget
// ─────────────────────────────────────────────────────────────────────────────

struct BarrageWidget : ModuleWidget {
	BarrageWidget(Barrage* module) {
		setModule(module);
		box.size = Vec(RACK_GRID_WIDTH * 44, RACK_GRID_HEIGHT);
		addChild(new BarragePanel(box.size));

		// ── Screws ──────────────────────────────────────────────────────────
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// ── Per-step knobs ──────────────────────────────────────────────────
		// 16 columns, each 36.25 px wide, first centre at x=28
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = 100.f + i * 36.25f;

			// Step active LED  (y=37)
			addChild(createLightCentered<SmallLight<GreenLight>>(Vec(x, 37.f), module, Barrage::STEP_LIGHT + i));

			// COUNT Trimpot   (y=87)
			addParam(createParamCentered<Trimpot>(Vec(x, 87.f), module, Barrage::COUNT_PARAM + i));

			// LENGTH Trimpot  (y=117)
			addParam(createParamCentered<Trimpot>(Vec(x, 117.f), module, Barrage::LENGTH_PARAM + i));

			// PROB Trimpot    (y=147)
			addParam(createParamCentered<Trimpot>(Vec(x, 147.f), module, Barrage::PROB_PARAM + i));

			// SPEED Trimpot   (y=177)
			addParam(createParamCentered<Trimpot>(Vec(x, 177.f), module, Barrage::SPEED_PARAM + i));
		}

		// ── Global controls ─────────────────────────────────────────────────
		// Sequence length knob  (x=75, y=230)
		addParam(createParamCentered<RoundBlackKnob>(Vec(147.f, 230.f), module, Barrage::SEQ_LENGTH_PARAM));

		// Direction knob  (x=222, y=230)
		addParam(createParamCentered<RoundBlackSnapKnob>(Vec(222.f, 230.f), module, Barrage::DIRECTION_PARAM));

		// ── Gate outputs — one per step, aligned to step columns ────────────
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = 100.f + i * 36.25f;
			addOutput(createOutputCentered<PJ301MPort>(Vec(x, 280.f), module, Barrage::GATE_OUTPUT + i));
		}

		// ── Burst EOC outputs — one per step ────────────────────────────────
		for (int i = 0; i < NUM_STEPS; i++) {
			float x = 100.f + i * 36.25f;
			addOutput(createOutputCentered<PJ301MPort>(Vec(x, 324.f), module, Barrage::BURST_EOC_OUTPUT + i));
		}

		// ── CLK / RST / EOC / STEP — sidebar ───────────────────────────────
		addInput(createInputCentered<PJ301MPort>(Vec(36.f, 118.f), module, Barrage::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(36.f, 163.f), module, Barrage::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(36.f, 226.f), module, Barrage::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(36.f, 271.f), module, Barrage::STEP_OUTPUT));
	}
};

Model* modelBarrage = createModel<Barrage, BarrageWidget>("Barrage");
