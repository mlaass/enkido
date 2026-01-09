/**
 * Cedar AudioWorklet Processor
 *
 * This runs in the AudioWorklet thread and processes audio
 * using the Cedar VM.
 *
 * For now, this is a stub that outputs silence until the WASM
 * module is properly integrated.
 */

class CedarProcessor extends AudioWorkletProcessor {
	constructor() {
		super();

		this.isInitialized = false;
		this.bpm = 120;
		this.phase = 0;

		// Test oscillator frequency
		this.testFreq = 440;
		this.testPhaseL = 0;
		this.testPhaseR = 0;

		// Handle messages from main thread
		this.port.onmessage = (event) => {
			this.handleMessage(event.data);
		};

		// Mark as initialized
		this.isInitialized = true;
		this.port.postMessage({ type: 'initialized' });
	}

	handleMessage(msg) {
		switch (msg.type) {
			case 'loadProgram':
				// TODO: Load bytecode into Cedar VM
				console.log('[CedarProcessor] loadProgram - bytecode size:', msg.bytecode?.byteLength);
				this.port.postMessage({ type: 'programLoaded' });
				break;

			case 'setBpm':
				this.bpm = msg.bpm;
				break;

			case 'setParam':
				// TODO: Forward to Cedar VM
				console.log('[CedarProcessor] setParam:', msg.name, '=', msg.value);
				break;

			case 'reset':
				this.testPhaseL = 0;
				this.testPhaseR = 0;
				this.phase = 0;
				break;
		}
	}

	process(inputs, outputs, parameters) {
		const output = outputs[0];
		if (!output || output.length < 2) return true;

		const outLeft = output[0];
		const outRight = output[1];

		if (!this.isInitialized) {
			// Output silence
			outLeft.fill(0);
			outRight.fill(0);
			return true;
		}

		// Generate test tone (simple sine wave)
		// This will be replaced by Cedar VM output
		const phaseIncrement = (this.testFreq * 2 * Math.PI) / sampleRate;

		for (let i = 0; i < outLeft.length; i++) {
			// Simple stereo sine wave
			outLeft[i] = Math.sin(this.testPhaseL) * 0.3;
			outRight[i] = Math.sin(this.testPhaseR) * 0.3;

			this.testPhaseL += phaseIncrement;
			this.testPhaseR += phaseIncrement * 1.001; // Slight detune for stereo width

			// Wrap phase
			if (this.testPhaseL > 2 * Math.PI) this.testPhaseL -= 2 * Math.PI;
			if (this.testPhaseR > 2 * Math.PI) this.testPhaseR -= 2 * Math.PI;
		}

		return true;
	}
}

registerProcessor('cedar-processor', CedarProcessor);
