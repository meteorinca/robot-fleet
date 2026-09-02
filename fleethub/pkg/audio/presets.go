package audio

import (
	"encoding/hex"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// SeedPresets extracts and pre-converts real audio files from the repository into the sound library.
func (l *Library) SeedPresets() {
	// 1. Check for notebooks/yes.mp3
	repoRoot := findRepoRoot(l.storageDir)
	if repoRoot != "" {
		yesMp3Path := filepath.Join(repoRoot, "firmware", "platforms", "speakerbot", "notebooks", "yes.mp3")
		if _, err := os.Stat(yesMp3Path); err == nil && !l.HasSound("yes_mp3") {
			pcm, err := l.converter.ConvertFile(yesMp3Path, 0.8)
			if err == nil && len(pcm) > 0 {
				_, _ = l.SaveSound("yes_mp3", pcm, "mp3", true)
				log.Printf("[FleetHub Audio] Pre-converted and registered preset: yes_mp3 (%d bytes)", len(pcm))
			}
		}

		// 2. Parse and convert the 8-bit firmware sound files into 16-bit PCM if not present
		fwMainDir := filepath.Join(repoRoot, "firmware", "platforms", "speakerbot", "main")
		seedFirmware8BitSound(l, fwMainDir, "choola_audio_8bit.h", "sound_choola", "choola")
		seedFirmware8BitSound(l, fwMainDir, "dogbark_audio_8bit.h", "dogbark_audio_8bit", "bark")
		seedFirmware8BitSound(l, fwMainDir, "paulbot_audio_8bit.h", "paulbot_audio_8bit", "paulbot")
		seedFirmwareExtraSounds(l, fwMainDir)
	}
}

func findRepoRoot(startDir string) string {
	dir, err := filepath.Abs(startDir)
	if err != nil {
		return ""
	}
	for i := 0; i < 5; i++ {
		if _, err := os.Stat(filepath.Join(dir, "firmware")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return ""
}

func seedFirmware8BitSound(l *Library, dir, filename, varName, soundName string) {
	if l.HasSound(soundName) {
		return
	}
	filePath := filepath.Join(dir, filename)
	content, err := os.ReadFile(filePath)
	if err != nil {
		return
	}

	pcm := extractCArrayToPCM16(string(content), varName)
	if len(pcm) > 0 {
		_, _ = l.SaveSound(soundName, pcm, "raw_8bit", true)
		log.Printf("[FleetHub Audio] Pre-converted hardware preset: %s (%d bytes)", soundName, len(pcm))
	}
}

func seedFirmwareExtraSounds(l *Library, dir string) {
	filePath := filepath.Join(dir, "extra_sounds.h")
	contentBytes, err := os.ReadFile(filePath)
	if err != nil {
		return
	}
	content := string(contentBytes)

	//huh
	if !l.HasSound("huh") {
		pcm := extractCArrayToPCM16(content, "sound_freesound_community_huh_102688")
		if len(pcm) > 0 {
			_, _ = l.SaveSound("huh", pcm, "raw_8bit", true)
			log.Printf("[FleetHub Audio] Pre-converted hardware preset: huh (%d bytes)", len(pcm))
		}
	}
	//yes
	if !l.HasSound("yes") {
		pcm := extractCArrayToPCM16(content, "sound_sergequadrado_child_says_yes_113117")
		if len(pcm) > 0 {
			_, _ = l.SaveSound("yes", pcm, "raw_8bit", true)
			log.Printf("[FleetHub Audio] Pre-converted hardware preset: yes (%d bytes)", len(pcm))
		}
	}
	//jump
	if !l.HasSound("jump") {
		pcm := extractCArrayToPCM16(content, "sound_freesound_community_cartoon_jump_6462")
		if len(pcm) > 0 {
			_, _ = l.SaveSound("jump", pcm, "raw_8bit", true)
			log.Printf("[FleetHub Audio] Pre-converted hardware preset: jump (%d bytes)", len(pcm))
		}
	}
	//ding
	if !l.HasSound("ding") {
		pcm := extractCArrayToPCM16(content, "sound_freesound_community_ding_107786")
		if len(pcm) > 0 {
			_, _ = l.SaveSound("ding", pcm, "raw_8bit", true)
			log.Printf("[FleetHub Audio] Pre-converted hardware preset: ding (%d bytes)", len(pcm))
		}
	}
}

// extractCArrayToPCM16 locates a C uint8_t array by name, extracts the bytes,
// and converts each signed 8-bit sample to a 16-bit signed PCM sample (s8 << 8).
func extractCArrayToPCM16(source string, varName string) []byte {
	idx := strings.Index(source, varName)
	if idx == -1 {
		return nil
	}
	openBrace := strings.Index(source[idx:], "{")
	if openBrace == -1 {
		return nil
	}
	start := idx + openBrace + 1
	closeBrace := strings.Index(source[start:], "}")
	if closeBrace == -1 {
		return nil
	}
	arrayBody := source[start : start+closeBrace]

	// Extract hex numbers (0xNN)
	reHex := regexp.MustCompile(`0x[0-9a-fA-F]{2}`)
	matches := reHex.FindAllString(arrayBody, -1)
	if len(matches) == 0 {
		return nil
	}

	pcm := make([]byte, len(matches)*2)
	for i, m := range matches {
		b, err := hex.DecodeString(m[2:])
		if err != nil || len(b) == 0 {
			continue
		}
		val8 := int8(b[0])
		// Upsample 8-bit signed to 16-bit signed with -2dB volume scaling (0.8)
		val16 := int16(float64(int16(val8)<<8) * 0.8)
		pcm[i*2] = byte(val16 & 0xFF)
		pcm[i*2+1] = byte((val16 >> 8) & 0xFF)
	}

	return pcm
}
