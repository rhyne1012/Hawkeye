// Flight Replay Local serves bundled static assets on loopback only.
// ULogs are read by the browser; the server has no file-upload endpoint.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"time"
)

type instance struct {
	URL string `json:"url"`
	PID int    `json:"pid"`
}

func openBrowser(url string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("/usr/bin/open", url)
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	return cmd.Run()
}
func webDirectory() string {
	exe, err := os.Executable()
	if err != nil {
		return "web"
	}
	dir := filepath.Dir(exe)
	if runtime.GOOS == "darwin" && filepath.Base(dir) == "MacOS" {
		return filepath.Join(dir, "..", "Resources", "web")
	}
	return filepath.Join(dir, "web")
}
func existingURL(stateFile string) string {
	data, err := os.ReadFile(stateFile)
	if err != nil {
		return ""
	}
	var state instance
	if json.Unmarshal(data, &state) != nil || !strings.HasPrefix(state.URL, "http://127.0.0.1:") {
		return ""
	}
	client := &http.Client{Timeout: 500 * time.Millisecond, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	response, err := client.Get(state.URL + "api/health")
	if err != nil {
		return ""
	}
	defer response.Body.Close()
	var health map[string]string
	if response.StatusCode == 200 && json.NewDecoder(response.Body).Decode(&health) == nil && health["app"] == "flight-replay-local" {
		return state.URL
	}
	return ""
}

func handler(files fs.FS, host, token string, shutdown func(), lastSeen *atomic.Int64) http.Handler {
	prefix := "/" + token + "/"
	fileServer := http.FileServer(http.FS(files))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Exact Host validation also prevents DNS rebinding to the loopback server.
		if r.Host != host || !strings.HasPrefix(r.URL.Path, prefix) {
			http.NotFound(w, r)
			return
		}
		route := strings.TrimPrefix(r.URL.Path, prefix)
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; object-src 'none'; base-uri 'none'")
		if route == "api/health" && r.Method == http.MethodGet {
			lastSeen.Store(time.Now().Unix())
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(map[string]string{"app": "flight-replay-local"})
			return
		}
		if route == "api/shutdown" {
			if r.Method != http.MethodPost {
				w.WriteHeader(http.StatusMethodNotAllowed)
				return
			}
			if r.Header.Get("Origin") != "http://"+host {
				w.WriteHeader(http.StatusForbidden)
				return
			}
			w.WriteHeader(http.StatusNoContent)
			go func() { time.Sleep(100 * time.Millisecond); shutdown() }()
			return
		}
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		clean := path.Clean(route)
		if route == "" {
			clean = "index.html"
		}
		// Never list directories or serve files outside the packaged web root.
		if clean != route && route != "" || strings.HasPrefix(clean, ".") || !fs.ValidPath(clean) {
			http.NotFound(w, r)
			return
		}
		info, err := fs.Stat(files, clean)
		if err != nil || info.IsDir() {
			http.NotFound(w, r)
			return
		}
		lastSeen.Store(time.Now().Unix())
		// FileServer redirects index.html; map the root request to / instead.
		cloned := r.Clone(r.Context())
		u := *r.URL
		cloned.URL = &u
		cloned.URL.Path = "/" + route
		if strings.HasSuffix(clean, ".wasm") {
			w.Header().Set("Content-Type", "application/wasm")
		}
		fileServer.ServeHTTP(w, cloned)
	})
}

func run() error {
	noBrowser := flag.Bool("no-browser", false, "do not open a browser (validation)")
	webRoot := flag.String("web-dir", webDirectory(), "packaged web directory")
	cache, _ := os.UserCacheDir()
	stateDir := flag.String("state-dir", filepath.Join(cache, "FlightReplayLocal"), "local runtime state directory")
	flag.Parse()
	if _, err := os.Stat(filepath.Join(*webRoot, "engine", "replay-core.wasm")); err != nil {
		return fmt.Errorf("web assets missing: %w", err)
	}
	if err := os.MkdirAll(*stateDir, 0700); err != nil {
		return err
	}
	stateFile := filepath.Join(*stateDir, "instance.json")
	lock := filepath.Join(*stateDir, "instance.lock")
	locked := false
	for attempt := 0; attempt < 170; attempt++ {
		if err := os.Mkdir(lock, 0700); err == nil {
			locked = true
			break
		} else if !os.IsExist(err) {
			return err
		}
		if url := existingURL(stateFile); url != "" {
			fmt.Println(url)
			if !*noBrowser {
				return openBrowser(url)
			}
			return nil
		}
		if info, err := os.Stat(lock); err == nil && time.Since(info.ModTime()) > 15*time.Second {
			_ = os.Remove(lock)
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !locked {
		return errors.New("another launcher is still starting; please try again")
	}
	defer os.Remove(lock)
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return err
	}
	defer listener.Close()
	secret := make([]byte, 24)
	if _, err = rand.Read(secret); err != nil {
		return err
	}
	token := hex.EncodeToString(secret)
	url := "http://" + listener.Addr().String() + "/" + token + "/"
	state, _ := json.Marshal(instance{URL: url, PID: os.Getpid()})
	if err = os.WriteFile(stateFile+".tmp", state, 0600); err != nil {
		return err
	}
	if err = os.Rename(stateFile+".tmp", stateFile); err != nil {
		return err
	}
	defer os.Remove(stateFile)
	var lastSeen atomic.Int64
	lastSeen.Store(time.Now().Unix())
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()
	server := &http.Server{ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 << 10}
	server.Handler = handler(os.DirFS(*webRoot), listener.Addr().String(), token, stop, &lastSeen)
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if time.Now().Unix()-lastSeen.Load() > 600 {
					stop()
					return
				}
			}
		}
	}()
	go func() {
		<-ctx.Done()
		timeout, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(timeout)
	}()
	fmt.Println(url)
	if !*noBrowser {
		go func() {
			if err := openBrowser(url); err != nil {
				log.Printf("Open this address in a browser: %s (%v)", url, err)
			}
		}()
	}
	err = server.Serve(listener)
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}
func main() {
	if err := run(); err != nil {
		log.Print(err)
		os.Exit(1)
	}
}
