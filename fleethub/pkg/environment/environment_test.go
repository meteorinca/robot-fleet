package environment

import (
	"testing"
	"time"
)

func TestSolarAltitudeCalculation(t *testing.T) {
	// Summer noon in New York: sun should be high up (> 60 degrees)
	summerNoon := time.Date(2026, 6, 21, 16, 0, 0, 0, time.UTC) // ~12pm EDT
	altNoon := calculateSolarAltitude(summerNoon, 40.7128, -74.0060)
	if altNoon < 50.0 {
		t.Errorf("Expected high solar elevation at summer noon, got %.2f", altNoon)
	}

	// Midnight in New York: sun should be below horizon (< 0 degrees)
	midnight := time.Date(2026, 6, 21, 4, 0, 0, 0, time.UTC) // ~12am EDT
	altMidnight := calculateSolarAltitude(midnight, 40.7128, -74.0060)
	if altMidnight >= 0.0 {
		t.Errorf("Expected negative solar elevation at midnight, got %.2f", altMidnight)
	}
}

func TestEnvironmentServiceDefaults(t *testing.T) {
	svc := NewService(40.7128, -74.0060)
	state, _, _ := svc.GetWeatherState()
	if state == "" {
		t.Errorf("Expected non-empty default weather state")
	}

	pos, _ := svc.SunPosition()
	if pos != "up" && pos != "down" {
		t.Errorf("Expected sun position to be 'up' or 'down', got %s", pos)
	}
}
