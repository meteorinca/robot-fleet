const SamJs = require('sam-js');
const fs = require('fs');

let sam = new SamJs({pitch: 64, speed: 72, mouth: 128, throat: 128});
let wav = sam.wav('Hi, My name is Paulbot');

let samples = Array.from(wav).slice(44); // 8-bit unsigned
let pcm_16 = samples.map(v => (v - 128) * 256); // 16-bit signed

// Nearest neighbor resample 22050 -> 16000
let resampled = [];
let ratio = 22050 / 16000;
let out_length = Math.floor(pcm_16.length / ratio);
for (let i = 0; i < out_length; i++) {
    let in_idx = Math.floor(i * ratio);
    resampled.push(pcm_16[in_idx]);
}

// Format as C array
let c_array = `// Auto-generated SAM TTS audio\n`;
c_array += `#include <stdint.h>\n`;
c_array += `#include <stddef.h>\n\n`;
c_array += `const uint8_t paulbot_audio[] = {\n`;
for (let i = 0; i < resampled.length; i++) {
    let val = resampled[i];
    if (val < 0) val = 65536 + val; // two's complement
    let low = val & 0xFF;
    let high = (val >> 8) & 0xFF;
    c_array += `0x${low.toString(16).padStart(2, '0')}, 0x${high.toString(16).padStart(2, '0')}, `;
    if (i % 8 === 7) c_array += '\n';
}
c_array += `\n};\nconst size_t paulbot_audio_len = ${resampled.length * 2};\n`;

fs.writeFileSync('main/paulbot_audio.h', c_array);
console.log(`Generated main/paulbot_audio.h with size ${resampled.length * 2} bytes`);
