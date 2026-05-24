package main

import (
	"bytes"
	"net/http"
	"net/http/httptest"
)

// recorder — обёртка над httptest.ResponseRecorder с публичными полями
// `code`/`body` (не дублирующая internal fields).
type recorder struct {
	rec  *httptest.ResponseRecorder
	code int
	body bytes.Buffer
}

func (r *recorder) Header() http.Header { return r.rec.Header() }
func (r *recorder) Write(p []byte) (int, error) {
	r.body.Write(p)
	return r.rec.Write(p)
}
func (r *recorder) WriteHeader(code int) {
	r.code = code
	r.rec.WriteHeader(code)
}

func httpTestRec() *recorder {
	r := &recorder{rec: httptest.NewRecorder()}
	r.code = 200
	return r
}

func httpTestReq(method, path, body, adminToken string) *http.Request {
	var rd *bytes.Buffer
	if body != "" {
		rd = bytes.NewBufferString(body)
	} else {
		rd = bytes.NewBuffer(nil)
	}
	req := httptest.NewRequest(method, path, rd)
	if adminToken != "" {
		req.Header.Set("X-Admin-Token", adminToken)
	}
	return req
}
