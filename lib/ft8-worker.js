/**
 * FT8/FT2 decode/encode worker thread.
 * Runs ft8js and ft2 (ESM/WASM) in an isolated thread to avoid blocking the main process.
 *
 * Messages IN:
 *   { type: 'decode', id, samples: Float32Array }
 *   { type: 'encode', id, text: string, frequency: number }
 *   { type: 'ft2-decode', id, samples: Float32Array }
 *   { type: 'ft2-encode', id, text: string, frequency: number }
 *
 * Messages OUT:
 *   { type: 'decode-result', id, results: [{db, dt, df, text}] }
 *   { type: 'encode-result', id, samples: Float32Array | null }
 *   { type: 'error', id, message: string }
 *   { type: 'ready' }
 */

const { parentPort } = require('worker_threads');
const path = require('path');

let decode, encode;
let ft2Decode, ft2Encode;

async function init() {
  try {
    const ft8 = await import('ft8js');
    decode = ft8.decode;
    encode = ft8.encode;
  } catch (err) {
    console.error('[FT8 Worker] Failed to load ft8js:', err.message);
  }

  try {
    const ft2 = require(path.join(__dirname, 'ft2', 'ft2.js'));
    await ft2.init();
    ft2Decode = ft2.decode;
    ft2Encode = ft2.encode;
  } catch (err) {
    console.error('[FT8 Worker] Failed to load ft2:', err.message);
  }

  parentPort.postMessage({ type: 'ready' });
}

parentPort.on('message', async (msg) => {
  try {
    if (msg.type === 'decode') {
      if (!decode) {
        parentPort.postMessage({ type: 'error', id: msg.id, message: 'ft8js not loaded yet' });
        return;
      }
      const samples = new Float32Array(msg.samples);
      const results = await decode(samples);
      parentPort.postMessage({ type: 'decode-result', id: msg.id, results });

    } else if (msg.type === 'encode') {
      if (!encode) {
        parentPort.postMessage({ type: 'error', id: msg.id, message: 'ft8js not loaded yet' });
        return;
      }
      const samples = await encode(msg.text, msg.frequency);
      if (samples) {
        const buf = samples.buffer;
        parentPort.postMessage(
          { type: 'encode-result', id: msg.id, samples },
          [buf]
        );
      } else {
        parentPort.postMessage({ type: 'encode-result', id: msg.id, samples: null });
      }

    } else if (msg.type === 'ft2-decode') {
      if (!ft2Decode) {
        parentPort.postMessage({ type: 'error', id: msg.id, message: 'ft2 not loaded yet' });
        return;
      }
      const samples = new Float32Array(msg.samples);
      const results = await ft2Decode(samples);
      parentPort.postMessage({ type: 'decode-result', id: msg.id, results });

    } else if (msg.type === 'ft2-encode') {
      if (!ft2Encode) {
        parentPort.postMessage({ type: 'error', id: msg.id, message: 'ft2 not loaded yet' });
        return;
      }
      const samples = await ft2Encode(msg.text, msg.frequency);
      if (samples) {
        const buf = samples.buffer;
        parentPort.postMessage(
          { type: 'encode-result', id: msg.id, samples },
          [buf]
        );
      } else {
        parentPort.postMessage({ type: 'encode-result', id: msg.id, samples: null });
      }
    }
  } catch (err) {
    parentPort.postMessage({ type: 'error', id: msg.id, message: err.message });
  }
});

init();
