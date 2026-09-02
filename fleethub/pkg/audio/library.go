package audio

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

// Library manages stored audio files and metadata.
type Library struct {
	storageDir string
	converter  *Converter
	mu         sync.RWMutex
}

// NewLibrary initializes the audio storage directory and pre-seeds presets.
func NewLibrary(storageDir string, converter *Converter) (*Library, error) {
	if err := os.MkdirAll(storageDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create sound storage directory: %w", err)
	}

	lib := &Library{
		storageDir: storageDir,
		converter:  converter,
	}

	// Extract and seed real audio presets from repo
	lib.SeedPresets()

	return lib, nil
}

// ListSounds returns metadata for all sounds currently in the library.
func (l *Library) ListSounds() []SoundMetadata {
	l.mu.RLock()
	defer l.mu.RUnlock()

	var sounds []SoundMetadata
	entries, err := os.ReadDir(l.storageDir)
	if err != nil {
		return sounds
	}

	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".json") {
			continue
		}
		jsonPath := filepath.Join(l.storageDir, entry.Name())
		data, err := os.ReadFile(jsonPath)
		if err != nil {
			continue
		}
		var meta SoundMetadata
		if err := json.Unmarshal(data, &meta); err == nil {
			// Verify accompanying .pcm file exists
			pcmPath := filepath.Join(l.storageDir, meta.Filename)
			if fi, err := os.Stat(pcmPath); err == nil {
				meta.SizeBytes = int(fi.Size())
				meta.TotalSamples = meta.SizeBytes / 2
				meta.DurationSec = float64(meta.TotalSamples) / float64(TargetSampleRate)
				sounds = append(sounds, meta)
			}
		}
	}

	// Sort alphabetically by name
	sort.Slice(sounds, func(i, j int) bool {
		return sounds[i].Name < sounds[j].Name
	})

	return sounds
}

// HasSound checks if a sound exists in the library.
func (l *Library) HasSound(name string) bool {
	l.mu.RLock()
	defer l.mu.RUnlock()
	cleanName := sanitizeSoundName(name)
	pcmPath := filepath.Join(l.storageDir, cleanName+".pcm")
	_, err := os.Stat(pcmPath)
	return err == nil
}

// GetSound retrieves the raw PCM bytes and metadata for a given sound name.
func (l *Library) GetSound(name string) ([]byte, SoundMetadata, error) {
	l.mu.RLock()
	defer l.mu.RUnlock()

	cleanName := sanitizeSoundName(name)
	pcmPath := filepath.Join(l.storageDir, cleanName+".pcm")
	pcmData, err := os.ReadFile(pcmPath)
	if err != nil {
		return nil, SoundMetadata{}, fmt.Errorf("sound '%s' not found: %w", name, err)
	}

	metaPath := filepath.Join(l.storageDir, cleanName+".json")
	var meta SoundMetadata
	metaBytes, err := os.ReadFile(metaPath)
	if err == nil {
		_ = json.Unmarshal(metaBytes, &meta)
	}

	meta.SizeBytes = len(pcmData)
	meta.TotalSamples = len(pcmData) / 2
	meta.DurationSec = float64(meta.TotalSamples) / float64(TargetSampleRate)
	meta.SampleRate = TargetSampleRate
	meta.Channels = TargetChannels
	if meta.Name == "" {
		meta.Name = cleanName
	}
	if meta.Filename == "" {
		meta.Filename = cleanName + ".pcm"
	}

	return pcmData, meta, nil
}

// SaveSound writes PCM bytes and metadata to storage.
func (l *Library) SaveSound(name string, pcmData []byte, sourceFormat string, isPreset bool) (SoundMetadata, error) {
	if len(pcmData) == 0 {
		return SoundMetadata{}, errors.New("cannot save empty audio data")
	}

	cleanName := sanitizeSoundName(name)
	if cleanName == "" {
		cleanName = fmt.Sprintf("sound_%d", time.Now().Unix())
	}

	l.mu.Lock()
	defer l.mu.Unlock()

	pcmFilename := cleanName + ".pcm"
	pcmPath := filepath.Join(l.storageDir, pcmFilename)
	if err := os.WriteFile(pcmPath, pcmData, 0644); err != nil {
		return SoundMetadata{}, fmt.Errorf("failed to write PCM file: %w", err)
	}

	totalSamples := len(pcmData) / 2
	durationSec := float64(totalSamples) / float64(TargetSampleRate)

	meta := SoundMetadata{
		Name:         cleanName,
		Filename:     pcmFilename,
		DurationSec:  durationSec,
		TotalSamples: totalSamples,
		SizeBytes:    len(pcmData),
		SampleRate:   TargetSampleRate,
		Channels:     TargetChannels,
		CreatedAt:    time.Now(),
		SourceFormat: sourceFormat,
		IsPreset:     isPreset,
	}

	metaData, err := json.MarshalIndent(meta, "", "  ")
	if err == nil {
		metaPath := filepath.Join(l.storageDir, cleanName+".json")
		_ = os.WriteFile(metaPath, metaData, 0644)
	}

	return meta, nil
}

// DeleteSound removes a sound and its metadata from the library.
func (l *Library) DeleteSound(name string) error {
	cleanName := sanitizeSoundName(name)
	l.mu.Lock()
	defer l.mu.Unlock()

	pcmPath := filepath.Join(l.storageDir, cleanName+".pcm")
	metaPath := filepath.Join(l.storageDir, cleanName+".json")

	_ = os.Remove(pcmPath)
	_ = os.Remove(metaPath)
	return nil
}

// GetPreviewWAV returns a standard WAV file wrapping the sound's PCM bytes for in-browser playback.
func (l *Library) GetPreviewWAV(name string) ([]byte, error) {
	pcm, _, err := l.GetSound(name)
	if err != nil {
		return nil, err
	}
	return PCMToWAV(pcm), nil
}

func sanitizeSoundName(name string) string {
	clean := strings.ToLower(strings.TrimSpace(name))
	clean = strings.ReplaceAll(clean, " ", "_")
	clean = strings.ReplaceAll(clean, "-", "_")
	clean = strings.ReplaceAll(clean, ".pcm", "")
	clean = strings.ReplaceAll(clean, ".mp3", "")
	clean = strings.ReplaceAll(clean, ".wav", "")
	var sb strings.Builder
	for _, r := range clean {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '_' {
			sb.WriteRune(r)
		}
	}
	return sb.String()
}
