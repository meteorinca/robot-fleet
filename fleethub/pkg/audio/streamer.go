package audio

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

const (
	// DefaultChunkSec is 4 seconds of 16kHz 16-bit mono audio (128,000 bytes).
	// Clips <= 4 seconds fit in a single chunk and never repeat.
	DefaultChunkSec = 4.0
	ChunkSizeBytes  = int(DefaultChunkSec * TargetBytesPerSec) // 128000 bytes
)

// Streamer handles chunking, pacing, and HTTP transmission of PCM audio to SpeakerBot.
type Streamer struct {
	client      *http.Client
	mu          sync.RWMutex
	activeCtx   context.Context
	cancelFunc  context.CancelFunc
	status      StreamStatus
	onProgress  func(status StreamStatus)
}

// NewStreamer creates a new Streamer instance.
func NewStreamer(progressCallback func(status StreamStatus)) *Streamer {
	return &Streamer{
		client: &http.Client{
			Timeout: 10 * time.Second,
		},
		onProgress: progressCallback,
	}
}

// GetStatus returns a snapshot of current streaming activity.
func (s *Streamer) GetStatus() StreamStatus {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.status
}

// Stop halts active streaming immediately and sends /stop to the target robot.
func (s *Streamer) Stop(targetBot config.RobotNode) error {
	s.mu.Lock()
	if s.cancelFunc != nil {
		s.cancelFunc()
		s.cancelFunc = nil
	}
	s.status.Active = false
	s.status.ProgressPct = 0
	s.mu.Unlock()

	if s.onProgress != nil {
		s.onProgress(s.GetStatus())
	}

	// Dispatch stop request to SpeakerBot
	return s.sendStopCommand(targetBot)
}

// StreamPCM streams raw 16kHz 16-bit mono PCM bytes to SpeakerBot in paced chunks.
func (s *Streamer) StreamPCM(ctx context.Context, soundName string, targetBot config.RobotNode, pcmData []byte, interrupt bool) error {
	if len(pcmData) == 0 {
		return errors.New("cannot stream empty audio data")
	}

	// Ensure any prior streaming task is terminated
	_ = s.Stop(targetBot)

	streamCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	s.mu.Lock()
	s.activeCtx = streamCtx
	s.cancelFunc = cancel

	totalBytes := len(pcmData)
	totalChunks := (totalBytes + ChunkSizeBytes - 1) / ChunkSizeBytes
	if totalChunks < 1 {
		totalChunks = 1
	}

	targetAddr := s.resolveTargetAddress(targetBot)
	s.status = StreamStatus{
		Active:         true,
		SoundName:      soundName,
		TargetBot:      targetBot.Hostname,
		TargetIP:       targetAddr,
		TotalChunks:    totalChunks,
		CurrentChunk:   0,
		ProgressPct:    0,
		BytesDelivered: 0,
		TotalBytes:     totalBytes,
		StartTime:      time.Now(),
	}
	s.mu.Unlock()

	if s.onProgress != nil {
		s.onProgress(s.GetStatus())
	}

	log.Printf("[FleetHub Audio] Starting stream '%s' to %s (%d bytes, %d chunks)",
		soundName, targetAddr, totalBytes, totalChunks)

	streamStart := time.Now()
	audioDeliveredSec := 0.0

	defer func() {
		s.mu.Lock()
		s.status.Active = false
		s.cancelFunc = nil
		s.mu.Unlock()

		if s.onProgress != nil {
			s.onProgress(s.GetStatus())
		}
	}()

	for i := 0; i < totalChunks; i++ {
		// Check for cancellation
		select {
		case <-streamCtx.Done():
			log.Printf("[FleetHub Audio] Stream '%s' cancelled by user stop", soundName)
			return nil
		default:
		}

		start := i * ChunkSizeBytes
		end := start + ChunkSizeBytes
		if end > totalBytes {
			end = totalBytes
		}

		chunk := pcmData[start:end]
		chunkDurationSec := float64(len(chunk)) / float64(TargetBytesPerSec)

		// First chunk carries ?interrupt=1 if interrupt is true
		useInterrupt := (i == 0 && interrupt)
		err := s.postChunk(streamCtx, targetBot, chunk, useInterrupt)
		if err != nil {
			if streamCtx.Err() != nil {
				return nil
			}
			s.mu.Lock()
			s.status.Error = err.Error()
			s.mu.Unlock()
			log.Printf("[FleetHub Audio] Chunk %d/%d failed: %v", i+1, totalChunks, err)
			return err
		}

		audioDeliveredSec += chunkDurationSec

		s.mu.Lock()
		s.status.CurrentChunk = i + 1
		s.status.BytesDelivered = end
		s.status.ProgressPct = int((float64(end) / float64(totalBytes)) * 100.0)
		s.mu.Unlock()

		if s.onProgress != nil {
			s.onProgress(s.GetStatus())
		}

		// Back-pressure pacing for multi-chunk streams
		if i > 0 && i < totalChunks-1 {
			elapsed := time.Since(streamStart).Seconds()
			lead := audioDeliveredSec - elapsed - 0.5 // Keep 0.5s lead in device buffer
			if lead > 0 {
				select {
				case <-streamCtx.Done():
					return nil
				case <-time.After(time.Duration(lead * float64(time.Second))):
				}
			}
		}
	}

	log.Printf("[FleetHub Audio] Finished stream '%s' (%d bytes delivered)", soundName, totalBytes)
	return nil
}

func (s *Streamer) postChunk(ctx context.Context, bot config.RobotNode, chunk []byte, interrupt bool) error {
	candidates := s.buildTargetCandidates(bot)
	var lastErr error

	endpoint := "/audio"
	if interrupt {
		endpoint = "/audio?interrupt=1"
	}

	for _, cand := range candidates {
		url := fmt.Sprintf("http://%s%s", strings.TrimRight(cand, "/"), endpoint)
		req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(chunk))
		if err != nil {
			lastErr = err
			continue
		}
		req.Header.Set("Content-Type", "application/octet-stream")

		resp, err := s.client.Do(req)
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode >= 200 && resp.StatusCode < 300 {
				return nil
			}
			lastErr = fmt.Errorf("HTTP error %d from %s", resp.StatusCode, url)
		} else {
			lastErr = err
		}
	}

	return fmt.Errorf("all connection candidates failed: %w", lastErr)
}

func (s *Streamer) sendStopCommand(bot config.RobotNode) error {
	candidates := s.buildTargetCandidates(bot)
	var lastErr error

	for _, cand := range candidates {
		url := fmt.Sprintf("http://%s/stop", strings.TrimRight(cand, "/"))
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		req, err := http.NewRequestWithContext(ctx, "POST", url, nil)
		if err != nil {
			cancel()
			lastErr = err
			continue
		}
		resp, err := s.client.Do(req)
		cancel()
		if err == nil {
			_ = resp.Body.Close()
			return nil
		}
		lastErr = err
	}
	return lastErr
}

func (s *Streamer) buildTargetCandidates(bot config.RobotNode) []string {
	port := bot.Port
	if port <= 0 || port == 4330 {
		port = 80
	}

	var candidates []string
	if isValidIP(bot.IP) {
		candidates = append(candidates, fmt.Sprintf("%s:%d", bot.IP, port))
	}
	if isValidIP(bot.FallbackIP) && bot.FallbackIP != bot.IP {
		candidates = append(candidates, fmt.Sprintf("%s:%d", bot.FallbackIP, port))
	}
	if bot.Hostname != "" {
		candidates = append(candidates, fmt.Sprintf("%s:%d", bot.Hostname, port))
	}
	if len(candidates) == 0 {
		candidates = append(candidates, fmt.Sprintf("speakerbot1.local:%d", port))
	}
	return candidates
}

func (s *Streamer) resolveTargetAddress(bot config.RobotNode) string {
	cands := s.buildTargetCandidates(bot)
	if len(cands) > 0 {
		return cands[0]
	}
	return "speakerbot1.local:80"
}

func isValidIP(ip string) bool {
	return net.ParseIP(strings.TrimSpace(ip)) != nil
}
