package main

import (
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"testing/fstest"
)

func TestLoopbackHandler(t *testing.T) {
	files := fstest.MapFS{"index.html": {Data: []byte("replay")}, "engine/core.wasm": {Data: []byte("wasm")}}
	var seen atomic.Int64
	h := handler(files, "127.0.0.1:1234", "secret", func() {}, &seen)
	cases := []struct {
		method, route, host, origin string
		status                      int
	}{
		{"GET", "/secret/", "127.0.0.1:1234", "", 200},
		{"GET", "/secret/engine/core.wasm", "127.0.0.1:1234", "", 200},
		{"GET", "/secret/api/health", "127.0.0.1:1234", "", 200},
		{"GET", "/secret/", "attacker.example", "", 404},
		{"GET", "/wrong/", "127.0.0.1:1234", "", 404},
		{"GET", "/secret/../private.ulg", "127.0.0.1:1234", "", 404},
		{"GET", "/secret/engine/", "127.0.0.1:1234", "", 404},
		{"POST", "/secret/upload", "127.0.0.1:1234", "", 405},
		{"GET", "/secret/api/shutdown", "127.0.0.1:1234", "", 405},
		{"POST", "/secret/api/shutdown", "127.0.0.1:1234", "http://attacker.example", 403},
		{"POST", "/secret/api/shutdown", "127.0.0.1:1234", "http://127.0.0.1:1234", 204},
	}
	for _, c := range cases {
		t.Run(c.method+c.route+c.origin, func(t *testing.T) {
			r := httptest.NewRequest(c.method, "http://"+c.host+c.route, nil)
			r.Header.Set("Origin", c.origin)
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if w.Code != c.status {
				t.Fatalf("got %d, want %d", w.Code, c.status)
			}
			if w.Code == 200 && !strings.Contains(w.Header().Get("Content-Security-Policy"), "connect-src 'self'") {
				t.Fatal("missing local-only connection policy")
			}
		})
	}
	if seen.Load() == 0 {
		t.Fatal("successful requests did not keep service alive")
	}
}
