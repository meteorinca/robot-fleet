package audio

import (
	"bytes"
	"encoding/binary"
	"os"
	"testing"

	"github.com/meteorinca/robot-fleet/fleethub/pkg/config"
)

func TestPCMToWAV(t *testing.T) {
	// Create 16000 samples (1 second) of 16-bit mono PCM (silence)
	pcm := make([]byte, 32000)
	wav := PCMToWAV(pcm)

	if len(wav) != len(pcm)+44 {
		t.Fatalf("expected WAV length %d, got %d", len(pcm)+44, len(wav))
	}

	if string(wav[0:4]) != "RIFF" {
		t.Fatalf("expected RIFF header, got %s", string(wav[0:4]))
	}

	if string(wav[8:12]) != "WAVE" {
		t.Fatalf("expected WAVE format, got %s", string(wav[8:12]))
	}

	// Verify sample rate is 16000
	rate := binary.LittleEndian.Uint32(wav[24:28])
	if rate != 16000 {
		t.Fatalf("expected sample rate 16000, got %d", rate)
	}
}

func TestConvertWAVBytes(t *testing.T) {
	// Generate a 1-second 8000Hz 16-bit mono WAV and convert to 16000Hz
	conv := NewConverter()

	// 8000 samples of PCM
	sourceSamples := 8000
	pcmSource := make([]byte, sourceSamples*2)
	for i := 0; i < sourceSamples; i++ {
		val := int16(1000)
		binary.LittleEndian.PutUint16(pcmSource[i*2:i*2+2], uint16(val))
	}

	// Build 8kHz WAV
	var buf bytes.Buffer
	buf.WriteString("RIFF")
	_ = binary.Write(&buf, binary.LittleEndian, uint32(36+len(pcmSource)))
	buf.WriteString("WAVE")
	buf.WriteString("fmt ")
	_ = binary.Write(&buf, binary.LittleEndian, uint32(16))
	_ = binary.Write(&buf, binary.LittleEndian, uint16(1))     // PCM
	_ = binary.Write(&buf, binary.LittleEndian, uint16(1))     // Mono
	_ = binary.Write(&buf, binary.LittleEndian, uint32(8000))  // SampleRate
	_ = binary.Write(&buf, binary.LittleEndian, uint32(16000)) // ByteRate
	_ = binary.Write(&buf, binary.LittleEndian, uint16(2))     // BlockAlign
	_ = binary.Write(&buf, binary.LittleEndian, uint16(16))    // BitsPerSample
	buf.WriteString("data")
	_ = binary.Write(&buf, binary.LittleEndian, uint32(len(pcmSource)))
	buf.Write(pcmSource)

	converted, err := conv.ConvertWAVBytes(buf.Bytes(), 1.0)
	if err != nil {
		t.Fatalf("conversion failed: %v", err)
	}

	expectedBytes := 16000 * 2 // 32000 bytes at 16kHz
	if len(converted) != expectedBytes {
		t.Fatalf("expected %d converted bytes, got %d", expectedBytes, len(converted))
	}
}

func TestLibraryManagement(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "fleethub_audio_test_*")
	if err != nil {
		t.Fatalf("failed to create temp dir: %v", err)
	}
	defer func() {
		_ = os.RemoveAll(tmpDir)
	}()

	conv := NewConverter()
	lib, err := NewLibrary(tmpDir, conv)
	if err != nil {
		t.Fatalf("failed to create library: %v", err)
	}

	testPCM := make([]byte, 64000) // 2 seconds
	meta, err := lib.SaveSound("test_alert", testPCM, "raw", false)
	if err != nil {
		t.Fatalf("save sound failed: %v", err)
	}

	if meta.Name != "test_alert" {
		t.Fatalf("expected name test_alert, got %s", meta.Name)
	}

	if meta.DurationSec != 2.0 {
		t.Fatalf("expected duration 2.0s, got %.2fs", meta.DurationSec)
	}

	if !lib.HasSound("test_alert") {
		t.Fatalf("expected HasSound to return true")
	}

	gotPCM, gotMeta, err := lib.GetSound("test_alert")
	if err != nil {
		t.Fatalf("GetSound failed: %v", err)
	}

	if len(gotPCM) != len(testPCM) {
		t.Fatalf("expected PCM length %d, got %d", len(testPCM), len(gotPCM))
	}

	if gotMeta.TotalSamples != 32000 {
		t.Fatalf("expected 32000 samples, got %d", gotMeta.TotalSamples)
	}

	previewWAV, err := lib.GetPreviewWAV("test_alert")
	if err != nil {
		t.Fatalf("GetPreviewWAV failed: %v", err)
	}
	if len(previewWAV) != len(testPCM)+44 {
		t.Fatalf("expected WAV length %d, got %d", len(testPCM)+44, len(previewWAV))
	}

	err = lib.DeleteSound("test_alert")
	if err != nil {
		t.Fatalf("DeleteSound failed: %v", err)
	}

	if lib.HasSound("test_alert") {
		t.Fatalf("expected HasSound to return false after delete")
	}
}

func TestStreamerStop(t *testing.T) {
	streamer := NewStreamer(nil)
	bot := config.RobotNode{
		ID:       "speakerbot1",
		Hostname: "speakerbot1.local",
		IP:       "127.0.0.1",
		Port:     9999, // Unreachable port for test
	}

	err := streamer.Stop(bot)
	// Even if network fails, Stop should clear internal state cleanly
	status := streamer.GetStatus()
	if status.Active {
		t.Fatalf("expected streamer status.Active to be false after Stop()")
	}
	_ = err
}
