package audio

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
)

const (
	TargetSampleRate = 16000
	TargetChannels   = 1
	BytesPerSample   = 2 // 16-bit signed integer
	TargetBytesPerSec = TargetSampleRate * TargetChannels * BytesPerSample // 32000 bytes/sec
)

// Converter handles audio transcoding into 16kHz 16-bit signed mono PCM.
type Converter struct {
	ffmpegPath string
}

// NewConverter initializes a Converter, checking for ffmpeg in PATH.
func NewConverter() *Converter {
	path, err := exec.LookPath("ffmpeg")
	if err != nil {
		path = ""
	}
	return &Converter{ffmpegPath: path}
}

// HasFFmpeg returns whether ffmpeg is available on the system.
func (c *Converter) HasFFmpeg() bool {
	return c.ffmpegPath != ""
}

// ConvertFile converts any audio file (MP3, WAV, M4A, OGG, FLAC) to 16kHz 16-bit mono PCM.
// volume is scaled from 0.05 to 1.0 (default 0.8 to provide clean headroom and avoid amp clipping).
func (c *Converter) ConvertFile(filePath string, volume float64) ([]byte, error) {
	if _, err := os.Stat(filePath); err != nil {
		return nil, fmt.Errorf("file not found: %w", err)
	}

	if volume <= 0.0 {
		volume = 0.8
	}
	if volume > 1.0 {
		volume = 1.0
	}
	if volume < 0.05 {
		volume = 0.05
	}

	// Try FFmpeg first if available
	if c.HasFFmpeg() {
		pcm, err := c.convertWithFFmpeg(filePath, volume)
		if err == nil && len(pcm) > 0 {
			return pcm, nil
		}
	}

	// If the file is a WAV file, try native Go WAV decoder
	if strings.HasSuffix(strings.ToLower(filePath), ".wav") {
		data, err := os.ReadFile(filePath)
		if err != nil {
			return nil, err
		}
		return c.ConvertWAVBytes(data, volume)
	}

	return nil, fmt.Errorf("conversion failed: ffmpeg not found and file is not uncompressed WAV")
}

// ConvertBytes converts raw audio file bytes to 16kHz 16-bit mono PCM.
func (c *Converter) ConvertBytes(data []byte, fileExt string, volume float64) ([]byte, error) {
	if volume <= 0.0 {
		volume = 0.8
	}
	if volume > 1.0 {
		volume = 1.0
	}
	if volume < 0.05 {
		volume = 0.05
	}

	// If data is already WAV, check if native decoder works
	if strings.EqualFold(fileExt, ".wav") || (len(data) >= 12 && string(data[0:4]) == "RIFF" && string(data[8:12]) == "WAVE") {
		pcm, err := c.ConvertWAVBytes(data, volume)
		if err == nil && len(pcm) > 0 {
			return pcm, nil
		}
	}

	// If FFmpeg is available, write to a temporary file and convert
	if c.HasFFmpeg() {
		tmpFile, err := os.CreateTemp("", "fleethub_audio_*"+fileExt)
		if err != nil {
			return nil, err
		}
		defer func() {
			_ = os.Remove(tmpFile.Name())
		}()

		if _, err := tmpFile.Write(data); err != nil {
			_ = tmpFile.Close()
			return nil, err
		}
		_ = tmpFile.Close()

		return c.ConvertFile(tmpFile.Name(), volume)
	}

	return nil, fmt.Errorf("unable to decode audio format %s without ffmpeg", fileExt)
}

func (c *Converter) convertWithFFmpeg(filePath string, volume float64) ([]byte, error) {
	volFilter := fmt.Sprintf("volume=%.2f,aresample=resampler=soxr", volume)
	cmd := exec.Command(c.ffmpegPath, "-y", "-i", filePath,
		"-af", volFilter,
		"-f", "s16le",
		"-acodec", "pcm_s16le",
		"-ar", "16000",
		"-ac", "1",
		"-",
	)

	var out bytes.Buffer
	var errOut bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &errOut

	err := cmd.Run()
	if err != nil {
		// Fallback without soxr resampler
		volFilter = fmt.Sprintf("volume=%.2f", volume)
		cmdFallback := exec.Command(c.ffmpegPath, "-y", "-i", filePath,
			"-af", volFilter,
			"-f", "s16le",
			"-acodec", "pcm_s16le",
			"-ar", "16000",
			"-ac", "1",
			"-",
		)
		out.Reset()
		errOut.Reset()
		cmdFallback.Stdout = &out
		cmdFallback.Stderr = &errOut
		err = cmdFallback.Run()
		if err != nil {
			return nil, fmt.Errorf("ffmpeg conversion failed: %w (stderr: %s)", err, errOut.String())
		}
	}

	return out.Bytes(), nil
}

// ConvertWAVBytes parses a standard PCM WAV buffer and resamples/mixes to 16kHz 16-bit mono PCM.
func (c *Converter) ConvertWAVBytes(data []byte, volume float64) ([]byte, error) {
	if len(data) < 44 {
		return nil, errors.New("invalid WAV file: header too short")
	}

	r := bytes.NewReader(data)
	var riffTag [4]byte
	if err := binary.Read(r, binary.LittleEndian, &riffTag); err != nil || string(riffTag[:]) != "RIFF" {
		return nil, errors.New("invalid WAV file: missing RIFF signature")
	}

	var fileSize uint32
	_ = binary.Read(r, binary.LittleEndian, &fileSize)

	var waveTag [4]byte
	if err := binary.Read(r, binary.LittleEndian, &waveTag); err != nil || string(waveTag[:]) != "WAVE" {
		return nil, errors.New("invalid WAV file: missing WAVE tag")
	}

	var audioFormat uint16
	var numChannels uint16
	var sampleRate uint32
	var byteRate uint32
	var blockAlign uint16
	var bitsPerSample uint16
	var rawData []byte

	// Traverse chunks
	for {
		var chunkID [4]byte
		var chunkSize uint32
		if err := binary.Read(r, binary.LittleEndian, &chunkID); err != nil {
			break
		}
		if err := binary.Read(r, binary.LittleEndian, &chunkSize); err != nil {
			break
		}

		id := string(chunkID[:])
		if id == "fmt " {
			_ = binary.Read(r, binary.LittleEndian, &audioFormat)
			_ = binary.Read(r, binary.LittleEndian, &numChannels)
			_ = binary.Read(r, binary.LittleEndian, &sampleRate)
			_ = binary.Read(r, binary.LittleEndian, &byteRate)
			_ = binary.Read(r, binary.LittleEndian, &blockAlign)
			_ = binary.Read(r, binary.LittleEndian, &bitsPerSample)
			if chunkSize > 16 {
				_, _ = r.Seek(int64(chunkSize-16), io.SeekCurrent)
			}
		} else if id == "data" {
			rawData = make([]byte, chunkSize)
			_, _ = io.ReadFull(r, rawData)
			break
		} else {
			_, _ = r.Seek(int64(chunkSize), io.SeekCurrent)
		}
	}

	if audioFormat != 1 { // 1 = uncompressed PCM
		return nil, fmt.Errorf("unsupported WAV format (%d): only uncompressed PCM is supported natively", audioFormat)
	}
	if len(rawData) == 0 {
		return nil, errors.New("empty WAV data chunk")
	}

	// Convert input samples to float64 normalized [-1.0, 1.0]
	var inSamples []float64
	bytesPerChannel := int(bitsPerSample / 8)
	if bytesPerChannel < 1 {
		bytesPerChannel = 1
	}
	frameSize := int(numChannels) * bytesPerChannel
	totalFrames := len(rawData) / frameSize

	for i := 0; i < totalFrames; i++ {
		frameOffset := i * frameSize
		var monoVal float64

		// Average channels
		for ch := 0; ch < int(numChannels); ch++ {
			sampleOffset := frameOffset + ch*bytesPerChannel
			if bitsPerSample == 8 {
				val8 := int(rawData[sampleOffset]) - 128
				monoVal += float64(val8) / 128.0
			} else if bitsPerSample == 16 {
				val16 := int16(binary.LittleEndian.Uint16(rawData[sampleOffset : sampleOffset+2]))
				monoVal += float64(val16) / 32768.0
			} else if bitsPerSample == 24 {
				val24 := int32(rawData[sampleOffset]) | (int32(rawData[sampleOffset+1]) << 8) | (int32(int8(rawData[sampleOffset+2])) << 16)
				monoVal += float64(val24) / 8388608.0
			} else if bitsPerSample == 32 {
				val32 := int32(binary.LittleEndian.Uint32(rawData[sampleOffset : sampleOffset+4]))
				monoVal += float64(val32) / 2147483648.0
			}
		}
		monoVal = monoVal / float64(numChannels)
		inSamples = append(inSamples, monoVal)
	}

	// Resample to 16,000 Hz using linear interpolation
	targetFrames := int(float64(totalFrames) * float64(TargetSampleRate) / float64(sampleRate))
	if targetFrames <= 0 {
		targetFrames = 1
	}

	ratio := float64(totalFrames) / float64(targetFrames)
	resampled := make([]int16, targetFrames)

	for i := 0; i < targetFrames; i++ {
		srcIdx := float64(i) * ratio
		idx0 := int(srcIdx)
		idx1 := idx0 + 1
		frac := srcIdx - float64(idx0)

		var s float64
		if idx1 < len(inSamples) {
			s = inSamples[idx0]*(1.0-frac) + inSamples[idx1]*frac
		} else if idx0 < len(inSamples) {
			s = inSamples[idx0]
		}

		// Apply volume scaling
		s *= volume

		// Soft clipping limiter
		if s > 0.98 {
			s = 0.98
		}
		if s < -0.98 {
			s = -0.98
		}

		resampled[i] = int16(s * 32767.0)
	}

	// Serialize to LittleEndian PCM16
	out := make([]byte, len(resampled)*2)
	for i, smp := range resampled {
		binary.LittleEndian.PutUint16(out[i*2:i*2+2], uint16(smp))
	}

	return out, nil
}

// PCMToWAV wraps raw 16kHz 16-bit mono PCM bytes in a valid RIFF/WAV header
// so browsers and media players can directly stream or preview it.
func PCMToWAV(pcmData []byte) []byte {
	var buf bytes.Buffer
	numChannels := uint16(TargetChannels)
	sampleRate := uint32(TargetSampleRate)
	bitsPerSample := uint16(16)
	byteRate := uint32(TargetBytesPerSec)
	blockAlign := uint16(numChannels * (bitsPerSample / 8))
	dataSize := uint32(len(pcmData))
	fileSize := 36 + dataSize

	// RIFF header
	buf.WriteString("RIFF")
	_ = binary.Write(&buf, binary.LittleEndian, fileSize)
	buf.WriteString("WAVE")

	// fmt sub-chunk
	buf.WriteString("fmt ")
	_ = binary.Write(&buf, binary.LittleEndian, uint32(16)) // subchunk1 size
	_ = binary.Write(&buf, binary.LittleEndian, uint16(1))  // PCM format
	_ = binary.Write(&buf, binary.LittleEndian, numChannels)
	_ = binary.Write(&buf, binary.LittleEndian, sampleRate)
	_ = binary.Write(&buf, binary.LittleEndian, byteRate)
	_ = binary.Write(&buf, binary.LittleEndian, blockAlign)
	_ = binary.Write(&buf, binary.LittleEndian, bitsPerSample)

	// data sub-chunk
	buf.WriteString("data")
	_ = binary.Write(&buf, binary.LittleEndian, dataSize)
	buf.Write(pcmData)

	return buf.Bytes()
}
