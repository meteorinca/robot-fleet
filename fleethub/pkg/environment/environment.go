package environment

import (
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"sync"
	"time"
)

// Service tracks environmental state including weather and solar position.
type Service struct {
	latitude      float64
	longitude     float64
	weatherState  string
	isRaining     bool
	temperature   float64
	lastWeatherAt time.Time
	httpClient    *http.Client
	mu            sync.RWMutex
}

// OpenMeteoResponse models the weather response from Open-Meteo.
type OpenMeteoResponse struct {
	Current struct {
		WeatherCode   int     `json:"weather_code"`
		Precipitation float64 `json:"precipitation"`
		Temperature2M float64 `json:"temperature_2m"`
		IsDay         int     `json:"is_day"`
	} `json:"current"`
}

// NewService creates an environment tracker for the given coordinates.
// Defaults to lat/lon if coordinates are 0.
func NewService(lat, lon float64) *Service {
	if lat == 0 && lon == 0 {
		lat = 40.7128 // Default reference latitude
		lon = -74.0060 // Default reference longitude
	}
	s := &Service{
		latitude:     lat,
		longitude:    lon,
		weatherState: "clear",
		httpClient: &http.Client{
			Timeout: 6 * time.Second,
		},
	}
	// Initial async fetch
	go s.RefreshWeather()
	return s
}

// SetCoordinates updates geographic position for solar and weather tracking.
func (s *Service) SetCoordinates(lat, lon float64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.latitude = lat
	s.longitude = lon
}

// IsSunDown returns true if the sun is below the horizon at current time.
func (s *Service) IsSunDown() bool {
	s.mu.RLock()
	lat := s.latitude
	lon := s.longitude
	s.mu.RUnlock()

	altitude := calculateSolarAltitude(time.Now().UTC(), lat, lon)
	return altitude < 0.0 // Below horizon
}

// SunPosition returns "down" or "up" and altitude degrees.
func (s *Service) SunPosition() (string, float64) {
	s.mu.RLock()
	lat := s.latitude
	lon := s.longitude
	s.mu.RUnlock()

	altitude := calculateSolarAltitude(time.Now().UTC(), lat, lon)
	if altitude < 0.0 {
		return "down", altitude
	}
	return "up", altitude
}

// IsRaining returns whether precipitation/rain is actively detected.
func (s *Service) IsRaining() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.isRaining
}

// GetWeatherState returns current human-readable weather description.
func (s *Service) GetWeatherState() (string, float64, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.weatherState, s.temperature, s.isRaining
}

// RefreshWeather fetches latest data from keyless Open-Meteo endpoint.
func (s *Service) RefreshWeather() {
	s.mu.RLock()
	lat := s.latitude
	lon := s.longitude
	s.mu.RUnlock()

	url := fmt.Sprintf("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=weather_code,precipitation,temperature_2m,is_day", lat, lon)
	resp, err := s.httpClient.Get(url)
	if err != nil {
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return
	}

	var data OpenMeteoResponse
	if err := json.NewDecoder(resp.Body).Decode(&data); err != nil {
		return
	}

	code := data.Current.WeatherCode
	raining := (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || (code >= 95 && code <= 99) || data.Current.Precipitation > 0.0

	desc := "clear"
	switch {
	case code >= 95:
		desc = "thunderstorm"
	case code >= 71 && code <= 77:
		desc = "snow"
	case raining:
		desc = "rain"
	case code >= 45 && code <= 48:
		desc = "fog"
	case code >= 1 && code <= 3:
		desc = "cloudy"
	default:
		desc = "clear"
	}

	s.mu.Lock()
	s.isRaining = raining
	s.weatherState = desc
	s.temperature = data.Current.Temperature2M
	s.lastWeatherAt = time.Now()
	s.mu.Unlock()
}

// calculateSolarAltitude computes solar elevation angle in degrees.
func calculateSolarAltitude(t time.Time, lat, lon float64) float64 {
	yearDay := float64(t.YearDay())
	hour := float64(t.Hour()) + float64(t.Minute())/60.0 + float64(t.Second())/3600.0

	// Fractional year in radians
	gamma := (2.0 * math.Pi / 365.0) * (yearDay - 1.0 + (hour-12.0)/24.0)

	// Equation of time in minutes
	eqtime := 229.18 * (0.000075 + 0.001868*math.Cos(gamma) - 0.032077*math.Sin(gamma) -
		0.014615*math.Cos(2.0*gamma) - 0.040849*math.Sin(2.0*gamma))

	// Solar declination angle in radians
	decl := 0.006918 - 0.399912*math.Cos(gamma) + 0.070257*math.Sin(gamma) -
		0.006758*math.Cos(2.0*gamma) + 0.000907*math.Sin(2.0*gamma) -
		0.002697*math.Cos(3.0*gamma) + 0.00148*math.Sin(3.0*gamma)

	// Time offset in minutes
	timeOffset := eqtime + 4.0*lon
	tst := hour*60.0 + timeOffset

	// Solar hour angle in degrees and radians
	haDeg := (tst / 4.0) - 180.0
	haRad := haDeg * math.Pi / 180.0

	latRad := lat * math.Pi / 180.0

	// Solar zenith angle cosine
	cosZenith := math.Sin(latRad)*math.Sin(decl) + math.Cos(latRad)*math.Cos(decl)*math.Cos(haRad)
	if cosZenith > 1.0 {
		cosZenith = 1.0
	} else if cosZenith < -1.0 {
		cosZenith = -1.0
	}

	zenithRad := math.Acos(cosZenith)
	altitudeDeg := 90.0 - (zenithRad * 180.0 / math.Pi)
	return altitudeDeg
}
