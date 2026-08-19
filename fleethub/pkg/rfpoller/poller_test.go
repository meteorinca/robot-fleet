package rfpoller

import (
	"testing"
)

func TestParseCode(t *testing.T) {
	tests := []struct {
		input    interface{}
		expected uint32
	}{
		{"1E240", 123456},
		{"0x1E240", 123456},
		{"1DD6E", 122222},
		{"0x1DD6E", 122222},
		{float64(123456), 123456},
		{int(123456), 123456},
		{nil, 0},
		{"", 0},
	}

	for _, tt := range tests {
		got := parseCode(tt.input)
		if got != tt.expected {
			t.Errorf("parseCode(%v) = %d; want %d", tt.input, got, tt.expected)
		}
	}
}
