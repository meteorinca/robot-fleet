package audio

import (
	"time"
)

// SoundMetadata holds descriptive information about a stored audio file.
type SoundMetadata struct {
	Name           string    `json:"name"`
	Filename       string    `json:"filename"`
	DurationSec    float64   `json:"duration_sec"`
	TotalSamples   int       `json:"total_samples"`
	SizeBytes      int       `json:"size_bytes"`
	SampleRate     int       `json:"sample_rate"`
	Channels       int       `json:"channels"`
	CreatedAt      time.Time `json:"created_at"`
	SourceFormat   string    `json:"source_format,omitempty"`
	IsPreset       bool      `json:"is_preset"`
}

// StreamStatus describes the real-time status of audio streaming to SpeakerBot.
type StreamStatus struct {
	Active         bool      `json:"active"`
	SoundName      string    `json:"sound_name,omitempty"`
	TargetBot      string    `json:"target_bot,omitempty"`
	TargetIP       string    `json:"target_ip,omitempty"`
	TotalChunks    int       `json:"total_chunks"`
	CurrentChunk   int       `json:"current_chunk"`
	ProgressPct    int       `json:"progress_pct"`
	BytesDelivered int       `json:"bytes_delivered"`
	TotalBytes     int       `json:"total_bytes"`
	StartTime      time.Time `json:"start_time,omitempty"`
	Error          string    `json:"error,omitempty"`
}

// PlayRequest specifies playback parameters for a sound.
type PlayRequest struct {
	Name      string  `json:"name"`
	Target    string  `json:"target,omitempty"`
	Volume    float64 `json:"volume,omitempty"` // 0.1 to 1.0 (default 0.8)
	Interrupt bool    `json:"interrupt"`        // Preempt active playback
	Repeat    int     `json:"repeat,omitempty"`
}
