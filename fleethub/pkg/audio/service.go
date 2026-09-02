package audio

import (
	"context"
	"fmt"
	"log"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

// Service provides a unified audio management and chunk streaming interface for FleetHub.
type Service struct {
	converter *Converter
	library   *Library
	streamer  *Streamer
}

// NewService initializes Converter, Library, and Streamer.
func NewService(storageDir string, onProgress func(status StreamStatus)) (*Service, error) {
	conv := NewConverter()
	lib, err := NewLibrary(storageDir, conv)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize audio library: %w", err)
	}

	streamer := NewStreamer(onProgress)

	svc := &Service{
		converter: conv,
		library:   lib,
		streamer:  streamer,
	}

	log.Printf("[FleetHub Audio] Audio subsystem active (FFmpeg available: %v, Library sounds: %d)",
		conv.HasFFmpeg(), len(lib.ListSounds()))

	return svc, nil
}

// Converter returns the audio converter instance.
func (s *Service) Converter() *Converter {
	return s.converter
}

// Library returns the sound library instance.
func (s *Service) Library() *Library {
	return s.library
}

// Streamer returns the audio chunk streamer instance.
func (s *Service) Streamer() *Streamer {
	return s.streamer
}

// PlaySound retrieves a named sound from the library and streams it to SpeakerBot in paced chunks.
func (s *Service) PlaySound(ctx context.Context, name string, targetBot config.RobotNode, volume float64, interrupt bool) error {
	pcmData, meta, err := s.library.GetSound(name)
	if err != nil {
		return err
	}

	// Apply volume scaling if requested and different from default
	if volume > 0.0 && volume < 1.0 && volume != 0.8 {
		pcmData = scalePCMVolume(pcmData, volume)
	}

	return s.streamer.StreamPCM(ctx, meta.Name, targetBot, pcmData, interrupt)
}

// PlayRawPCM streams arbitrary 16kHz 16-bit mono PCM bytes to SpeakerBot.
func (s *Service) PlayRawPCM(ctx context.Context, soundName string, targetBot config.RobotNode, pcmData []byte, interrupt bool) error {
	return s.streamer.StreamPCM(ctx, soundName, targetBot, pcmData, interrupt)
}

// ConvertAndSave converts an uploaded audio file into 16kHz PCM and stores it in the library.
func (s *Service) ConvertAndSave(name string, fileBytes []byte, fileExt string, volume float64) (SoundMetadata, error) {
	pcm, err := s.converter.ConvertBytes(fileBytes, fileExt, volume)
	if err != nil {
		return SoundMetadata{}, fmt.Errorf("conversion failed: %w", err)
	}
	return s.library.SaveSound(name, pcm, fileExt, false)
}

// ConvertAndStream converts an audio file, streams it immediately, and optionally saves it.
func (s *Service) ConvertAndStream(ctx context.Context, name string, fileBytes []byte, fileExt string, targetBot config.RobotNode, volume float64, interrupt bool, saveToLibrary bool) error {
	pcm, err := s.converter.ConvertBytes(fileBytes, fileExt, volume)
	if err != nil {
		return fmt.Errorf("conversion failed: %w", err)
	}

	if saveToLibrary {
		_, _ = s.library.SaveSound(name, pcm, fileExt, false)
	}

	return s.streamer.StreamPCM(ctx, name, targetBot, pcm, interrupt)
}

// Stop halts ongoing streaming on FleetHub and dispatches /stop to SpeakerBot.
func (s *Service) Stop(targetBot config.RobotNode) error {
	return s.streamer.Stop(targetBot)
}

// GetStatus returns the current streaming status.
func (s *Service) GetStatus() StreamStatus {
	return s.streamer.GetStatus()
}

func scalePCMVolume(pcmData []byte, volume float64) []byte {
	if len(pcmData) < 2 {
		return pcmData
	}
	out := make([]byte, len(pcmData))
	copy(out, pcmData)

	for i := 0; i < len(out)-1; i += 2 {
		val := int16(out[i]) | (int16(out[i+1]) << 8)
		scaled := float64(val) * volume
		if scaled > 32767 {
			scaled = 32767
		}
		if scaled < -32768 {
			scaled = -32768
		}
		val16 := int16(scaled)
		out[i] = byte(val16 & 0xFF)
		out[i+1] = byte((val16 >> 8) & 0xFF)
	}
	return out
}
