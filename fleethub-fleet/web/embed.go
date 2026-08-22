package web

import (
	"embed"
	"io/fs"
)

//go:embed public/*
var embeddedFiles embed.FS

// Assets returns the embedded public filesystem.
func Assets() (fs.FS, error) {
	return fs.Sub(embeddedFiles, "public")
}
